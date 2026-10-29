/// @file
/// @brief The baked particle/effect system: capture, simulation and billboard rendering.

#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace castlemist::render {

namespace {
uint32_t g_fxSeed = 1;
float g_fxSpeed = 1.0f;
bool g_fxPaused = false;
std::vector<bool> g_fxSolo;   // per SimInput emitter; empty or mismatched = all drawn
}  // namespace

void set_show_effects(bool on) {
    g_show_effects = on;
    if (!on && g_fxSim) g_fxSim = std::make_unique<fxsim::Sim>(g_fxSim->input(), g_fxSeed, false);   // start over when shown again
}
const fxsim::Sim* effect_sim() { return g_fxSim.get(); }
void restart_effects(uint32_t seed) {
    g_fxSeed = seed;
    if (g_fxSim) g_fxSim = std::make_unique<fxsim::Sim>(g_fxSim->input(), seed, false);
}
void set_effect_solo(const std::vector<bool>& mask) { g_fxSolo = mask; }
void set_effect_time(float seconds) {
    if (g_fxSim) { const fxsim::SimInput in = g_fxSim->input(); fxsim::seek(*g_fxSim, in, g_fxSeed, 1.0f / 60.0f, seconds, false); }
}
void set_effect_speed(float factor) { g_fxSpeed = std::clamp(factor, 0.05f, 8.0f); }
void set_effects_paused(bool paused) { g_fxPaused = paused; }
bool show_effects() { return g_show_effects; }
bool has_effects() { return effects_present(); }
void effect_stats(int& clouds, int& emitters, int& lights, int& live) {
    clouds = (int)g_fxClouds.size(); emitters = (int)g_fxEmitters.size();
    lights = (int)g_fxLights.size(); live = g_fxSim ? (int)g_fxSim->frame().particles.size() : 0;
}
void set_show_skeleton(bool show) { g_show_skeleton = show; }
bool has_skeleton() { return g_has_skeleton; }


// ---- particle system: capture, simulate, render ----------------------------
void clear_effects() {
    g_fxClouds.clear(); g_fxEmitters.clear(); g_fxLights.clear();
    g_fxSim.reset();
    g_fx_qpc_init = false;
}

void capture_effects(const ModelPreview& model) {
    clear_effects();
    if (!model.hasEffects()) return;
    fxsim::SimInput in = fxsim::make_input(model);
    g_fxClouds = in.clouds;
    for (const auto& e : in.emitters) g_fxEmitters.push_back(e.e);
    g_fxLights = model.effectLights;
    g_fxSim = std::make_unique<fxsim::Sim>(std::move(in), g_fxSeed, false);
    g_fxSolo.clear();
}

bool effects_present() { return !g_fxClouds.empty() || !g_fxLights.empty(); }

// GW2 refraction/heat-haze emitters store a flat surface normal (~0,0,1) as their
// "color" and perturb the framebuffer instead of adding light -- invisible in-game.
// Our additive pass would draw them as huge blue/purple streaks, so skip them.
bool fxIsDistortion(const ParticleEmitterCPU& e) {
    float r = e.colorBegin[0][0], g = e.colorBegin[0][1], b = e.colorBegin[0][2];
    float er = e.colorEnd[0][0], eg = e.colorEnd[0][1];
    bool beginNormal = (b > 0.8f && r < 0.2f && g < 0.2f);       // flat normal (0,0,1)
    bool endNormal   = (er > 0.8f && eg < 0.2f);                 // -> tilted normal (1,0,*)
    return beginNormal && endNormal;
}

void update_particles(float dt) {
    if (!g_show_effects || !g_fxSim || dt <= 0 || g_fxPaused) return;
    g_fxSim->step(std::min(dt, 0.05f) * g_fxSpeed); // clamp big frame gaps (paused/minimized)
}

void render_particles(const Mat4& mvp) {
    if (!g_show_effects || !g_partVs || !g_partIl) return;
    static const std::vector<fxsim::Particle> kNone;
    const std::vector<fxsim::Particle>& parts = g_fxSim ? g_fxSim->frame().particles : kNone;
    if (parts.empty() && g_fxLights.empty()) return;

    // Camera basis in model space (billboards face the camera): world right/up
    // are +X/+Y (fixed lookAt); the trackball model rotation g_rot maps model->
    // world for directions, so model-space basis = columns 0/1 of g_rot.
    Vec3 right{g_rot.m[0], g_rot.m[4], g_rot.m[8]};
    Vec3 up{g_rot.m[1], g_rot.m[5], g_rot.m[9]};

    std::vector<PartVtx> vtx;
    vtx.reserve((parts.size() + g_fxLights.size()) * 6);

    // A quad from its two half-axes (already scaled by the sizes).
    auto emitAxes = [&](Vec3 c, Vec3 ax, Vec3 ay, const float col[4], float u0, float v0, float u1, float v1) {
        auto corner = [&](float dx, float dy, float u, float v) -> PartVtx {
            return { c.x + ax.x*dx + ay.x*dy, c.y + ax.y*dx + ay.y*dy, c.z + ax.z*dx + ay.z*dy,
                     u, v, dx, dy, col[0], col[1], col[2], col[3] };
        };
        PartVtx a = corner(-1,-1,u0,v1), b = corner(1,-1,u1,v1), d = corner(1,1,u1,v0), e = corner(-1,1,u0,v0);
        vtx.push_back(a); vtx.push_back(b); vtx.push_back(d);
        vtx.push_back(a); vtx.push_back(d); vtx.push_back(e);
    };
    // Camera-facing, rotated in the view plane (sprite case 1; also the stand-in for 0, 2, 3).
    auto emitQuad = [&](Vec3 c, float sx, float sy, float rot, const float col[4], float u0, float v0, float u1, float v1) {
        const fx::SpriteAxes s = fx::camera_sprite_axes(rot, sx, sy);
        emitAxes(c, {right.x*s.u.x + up.x*s.u.y, right.y*s.u.x + up.y*s.u.y, right.z*s.u.x + up.z*s.u.y},
                    {right.x*s.v.x + up.x*s.v.y, right.y*s.v.x + up.y*s.v.y, right.z*s.v.x + up.z*s.v.y},
                 col, u0, v0, u1, v1);
    };
    // The view axis in model space, from the same basis as right / up (its sign only mirrors
    // an axis billboard across its own axis).
    const fx::Vec3 toEye{right.y*up.z - right.z*up.y, right.z*up.x - right.x*up.z, right.x*up.y - right.y*up.x};

    // Group particles by material so we can bind each cloud material's texture in
    // one draw. We build all verts, tracking per-material spans.
    struct Span { int material; UINT start, count; };
    std::vector<Span> spans;
    // sort particle indices by their cloud's material
    auto materialOf = [&](const fxsim::Particle& p) {
        const int cloud = g_fxSim->input().emitters[p.emitter].cloud;
        return cloud >= 0 && static_cast<size_t>(cloud) < g_fxClouds.size()
                   ? static_cast<int>(g_fxClouds[static_cast<size_t>(cloud)].materialIndex) : -1;
    };
    std::vector<int> order;
    order.reserve(parts.size());
    for (size_t i = 0; i < parts.size(); ++i)
        if (!fxIsDistortion(g_fxEmitters[parts[i].emitter]) &&
            (g_fxSolo.size() != g_fxEmitters.size() || g_fxSolo[parts[i].emitter]))
            order.push_back(static_cast<int>(i));
    std::vector<int> mats(parts.size());
    for (size_t i = 0; i < parts.size(); ++i) mats[i] = materialOf(parts[i]);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return mats[a] < mats[b]; });

    int curMat = -2; UINT spanStart = 0;
    for (int oi : order) {
        const fxsim::Particle& p = parts[static_cast<size_t>(oi)];
        if (mats[oi] != curMat) {
            if (curMat != -2) spans.push_back({curMat, spanStart, (UINT)vtx.size() - spanStart});
            curMat = mats[oi]; spanStart = (UINT)vtx.size();
        }
        // The sim's colour (begin -> end, opacity curve or preset), size, rotation and flipbook
        // cell; uv is (u0, v0, u1, v1) as the emitter's texCoordRect is stored.
        float col[4] = {p.colour[0], p.colour[1], p.colour[2], std::clamp(p.colour[3], 0.0f, 1.0f) * g_fxIntensity};
        // Sprite alignment by the client's case (fx::plane_alignment_case): 5 along the motion,
        // 4 along the on-screen motion, 0 / 2 / 3 planes facing a direction, 1 camera-facing.
        const Vec3 c{p.pos.x, p.pos.y, p.pos.z};
        switch (fx::plane_alignment_case(g_fxEmitters[p.emitter].alignmentType)) {
            case 5: {
                const fx::Axes3 a = fx::axis_sprite_axes(p.vel, toEye, p.size[0], p.size[1]);
                emitAxes(c, {a.u.x, a.u.y, a.u.z}, {a.v.x, a.v.y, a.v.z}, col, p.uv[0], p.uv[1], p.uv[2], p.uv[3]);
                break;
            }
            case 4: {
                const fx::Vec2 sv{p.vel.x*right.x + p.vel.y*right.y + p.vel.z*right.z, p.vel.x*up.x + p.vel.y*up.y + p.vel.z*up.z};
                const fx::SpriteAxes s = fx::screen_velocity_axes(sv, p.size[0], p.size[1]);
                emitAxes(c, {right.x*s.u.x + up.x*s.u.y, right.y*s.u.x + up.y*s.u.y, right.z*s.u.x + up.z*s.u.y},
                            {right.x*s.v.x + up.x*s.v.y, right.y*s.v.x + up.y*s.v.y, right.z*s.v.x + up.z*s.v.y},
                         col, p.uv[0], p.uv[1], p.uv[2], p.uv[3]);
                break;
            }
            case 0: case 2: case 3: {
                // a plane facing alignmentDir (0; model space here, the client uses the cloud's
                // matrix), the motion (2) or against it (3)
                const ParticleEmitterCPU& em = g_fxEmitters[p.emitter];
                const int k = fx::plane_alignment_case(em.alignmentType);
                const fx::Vec3 n = k == 0 ? fx::Vec3{em.alignmentDir[0], em.alignmentDir[1], em.alignmentDir[2]}
                                 : k == 2 ? p.vel : fx::Vec3{-p.vel.x, -p.vel.y, -p.vel.z};
                const fx::Axes3 a = fx::facing_axes(n, p.rotation, p.size[0], p.size[1]);
                emitAxes(c, {a.u.x, a.u.y, a.u.z}, {a.v.x, a.v.y, a.v.z}, col, p.uv[0], p.uv[1], p.uv[2], p.uv[3]);
                break;
            }
            default:
                emitQuad(c, p.size[0], p.size[1], p.rotation, col, p.uv[0], p.uv[1], p.uv[2], p.uv[3]);
        }
    }
    if (curMat != -2) spans.push_back({curMat, spanStart, (UINT)vtx.size() - spanStart});

    // Effect lights: a soft additive glow disc at the bone position.
    if (!g_fxLights.empty()) {
        UINT ls = (UINT)vtx.size();
        for (const auto& L : g_fxLights) {
            float rad = std::max(0.02f, L.farDistance * 0.5f);
            float inten = std::clamp(L.intensity, 0.05f, 4.0f);
            float col[4] = {L.color[0]*inten, L.color[1]*inten, L.color[2]*inten, 1.0f};
            emitQuad({L.pos[0], L.pos[1], L.pos[2]}, rad, rad, 0, col, 0, 0, 1, 1);
        }
        spans.push_back({-1, ls, (UINT)vtx.size() - ls});
    }
    if (vtx.empty()) return;

    // (re)alloc the dynamic VB
    if (g_partVbCap < vtx.size()) {
        g_partVb.Reset();
        D3D11_BUFFER_DESC vbd{};
        vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        vbd.ByteWidth = (UINT)(vtx.size() * sizeof(PartVtx) * 3 / 2 + 256);
        if (FAILED(g_dev->CreateBuffer(&vbd, nullptr, &g_partVb))) return;
        g_partVbCap = vbd.ByteWidth / sizeof(PartVtx);
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_partVb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    std::memcpy(ms.pData, vtx.data(), vtx.size() * sizeof(PartVtx));
    g_ctx->Unmap(g_partVb.Get(), 0);
    // upload mvp
    if (SUCCEEDED(g_ctx->Map(g_partCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, mvp.m, 64); g_ctx->Unmap(g_partCb.Get(), 0);
    }

    // pipeline state: additive, depth-test on / write off, particle shaders
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendAdd.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dssNoWrite.Get(), 0);
    g_ctx->RSSetState(g_rsSolid.Get());
    UINT stride = sizeof(PartVtx), off = 0;
    ID3D11Buffer* vbs[] = {g_partVb.Get()};
    g_ctx->IASetInputLayout(g_partIl.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_partVs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_partPs.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_partCb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samps[] = {g_samp.Get()};
    g_ctx->PSSetSamplers(0, 1, samps);

    for (const auto& s : spans) {
        if (s.count == 0) continue;
        ID3D11ShaderResourceView* srv = g_softDisc.Get();
        if (s.material >= 0 && s.material < (int)g_mats.size() && g_mats[s.material].srv)
            srv = g_mats[s.material].srv.Get();
        ID3D11ShaderResourceView* srvs[] = {srv};
        g_ctx->PSSetShaderResources(0, 1, srvs);
        // The cloud material's own blend (its AMAT renderState: additive One/InvSrcColor,
        // premultiplied One/InvSrcAlpha, ...); additive when the material has none.
        ID3D11BlendState* blend = g_blendAdd.Get();
        if (s.material >= 0 && s.material < (int)g_mats.size() && g_mats[s.material].blend)
            blend = g_mats[s.material].blend.Get();
        g_ctx->OMSetBlendState(blend, bf, 0xffffffff);
        g_ctx->Draw(s.count, s.start);
    }
    // restore sane defaults for any following passes
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
}

// ==========================================================================
// Blender-style interactive transform gizmo
// ==========================================================================

} // namespace castlemist::render
