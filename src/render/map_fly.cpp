/// @file
/// @brief Mesh-only fly-through map view: compact upload, baked AO, instanced draw.
///
/// See `detail/map_fly.h` for why this exists as a second path rather than a
/// mode of `render_scene()`.

#include "detail/map_fly.h"
#include "detail/state.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace castlemist::render {
namespace {

/// @brief CPU-side result of preparing one model, before any D3D call.
struct Prepared {
    std::vector<MapVtx> verts;
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;
};

bool create_model_buffers(const Prepared& p, const ModelPreview& mp, FlyModelGPU& out) {
    if (p.verts.empty() || p.indices.empty()) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(p.verts.size() * sizeof(MapVtx));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{p.verts.data(), 0, 0};
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.vb))) return false;

    bd.ByteWidth = static_cast<UINT>(p.indices.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = p.indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.ib))) { out.vb.Reset(); return false; }

    out.lodRanges = p.lodRanges;
    out.vertCount = static_cast<uint32_t>(p.verts.size());
    for (int k = 0; k < 3; ++k) out.center[k] = mp.center[k];
    out.radius = (std::isfinite(mp.radius) && mp.radius > 0) ? mp.radius : 1.0f;
    out.ok = true;
    return true;
}

/// @brief Grows the shared per-instance vertex buffer to hold `need` entries.
bool ensure_inst_capacity(uint32_t need) {
    if (g_flyInstVB && g_flyInstCap >= need) return true;
    uint32_t cap = std::max<uint32_t>(need + need / 2, 4096);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = cap * sizeof(MapInstGPU);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> nb;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &nb))) return false;
    g_flyInstVB = nb;
    g_flyInstCap = cap;
    return true;
}

/// @brief Packs a row-vector world matrix into the three columns the VS wants.
void pack_inst(const Mat4& w, int layer, MapInstGPU& o) {
    o.r0[0] = w.m[0]; o.r0[1] = w.m[4]; o.r0[2] = w.m[8];  o.r0[3] = w.m[12];
    o.r1[0] = w.m[1]; o.r1[1] = w.m[5]; o.r1[2] = w.m[9];  o.r1[3] = w.m[13];
    o.r2[0] = w.m[2]; o.r2[1] = w.m[6]; o.r2[2] = w.m[10]; o.r2[3] = w.m[14];
    int l = (layer >= 0 && layer < LAYER_COUNT) ? layer : LAYER_PROP;
    for (int k = 0; k < 3; ++k) o.tint[k] = kFlyLayerTint[l][k];
    o.tint[3] = 1.0f;
}

} // namespace

// ---------------------------------------------------------------------------
// Build / teardown
// ---------------------------------------------------------------------------

void clear_fly_scene() {
    g_fly_models.clear();
    g_fly_insts.clear();
    g_fly_built = false;
}

/// @brief Prepares compact buffers + AO for `models`, appending at `base`.
///
/// The expensive half (flatten + voxelize + march) touches no D3D and is run
/// across every core; buffer creation is serial afterwards because the device
/// context is not free-threaded.
void fly_add_models(const std::vector<ModelPreview>& models, size_t base) {
    if (!g_dev || models.empty()) return;
    if (g_fly_models.size() < base + models.size()) g_fly_models.resize(base + models.size());

    std::vector<Prepared> prep(models.size());
    parallel_for(models.size(), [&](size_t i) {
        flatten(models[i], prep[i].verts, prep[i].indices, prep[i].lodRanges);
        bake_ao(prep[i].verts, prep[i].indices);
    });
    for (size_t i = 0; i < models.size(); ++i)
        create_model_buffers(prep[i], models[i], g_fly_models[base + i]);
}

void fly_add_instance(int model, const Mat4& world, float radius, int layer) {
    if (model < 0 || model >= static_cast<int>(g_fly_models.size())) return;
    const FlyModelGPU& fm = g_fly_models[model];
    if (!fm.ok) return;
    FlyInstCPU in;
    in.model = model;
    in.layer = layer;
    in.world = world;
    Vec3 c = transformPoint({fm.center[0], fm.center[1], fm.center[2]}, world);
    in.centerW[0] = c.x; in.centerW[1] = c.y; in.centerW[2] = c.z;
    in.radiusW = radius;
    g_fly_insts.push_back(in);
    g_fly_built = true;
}

void fly_frame_scene() {
    // Frame on the FULL scene box, terrain included. g_scene_center/g_scene_radius
    // track the prop-only box (so the orbit camera is not dragged out by a
    // map-sized ground plane), and using those here dropped the camera at the
    // props' own scale -- which on a map whose props cluster inside a 6k-unit
    // terrain plane starts you *underneath* the ground looking up.
    Vec3 lo = g_scene_lo, hi = g_scene_hi;
    Vec3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
    float R = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    if (!std::isfinite(R) || R < 1.0f) { R = std::max(g_scene_radius, 1.0f); c = g_scene_center; }

    // Stand back along -X at the scene's own scale, and clearly ABOVE everything
    // in it rather than level with its centre. R is the box half-DIAGONAL, which
    // overestimates what actually has to fit on screen, so the pull-back is well
    // under 1R -- at 1.15R the map sat in the middle of the view as a postage stamp.
    //
    // ABOVE means the SMALLEST z, not the largest: GW2's +Z points down (kWorldUp).
    // Starting at `hi.z + ...` put the camera under the map looking up through it.
    g_fly_pos = {c.x - R * 0.62f, c.y, lo.z - R * 0.28f};
    Vec3 aim{c.x - g_fly_pos.x, c.y - g_fly_pos.y, c.z - g_fly_pos.z};
    g_fly_fwd = norm(aim);
    // Level horizon: up is world up projected perpendicular to forward.
    Vec3 right = cross(kWorldUp, g_fly_fwd);   // basis convention: right = up x fwd
    if (dot(right, right) < 1e-6f) right = Vec3{0, 1, 0};
    g_fly_up = norm(cross(g_fly_fwd, norm(right)));

    // Cross the scene in a few seconds, and see across all of it.
    g_fly_speed = std::clamp(R * 0.5f, 20.0f, 20000.0f);
    g_fly_view_dist = std::max(R * 3.0f, 5000.0f);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void render_fly() {
    if (!g_fly_pipeline_ok || g_fly_insts.empty()) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    // --- camera --------------------------------------------------------------
    // Built from the camera's own basis, so there is no world up-vector to go
    // parallel with and no orientation the view cannot reach.
    Vec3 at{g_fly_pos.x + g_fly_fwd.x, g_fly_pos.y + g_fly_fwd.y, g_fly_pos.z + g_fly_fwd.z};
    Mat4 view = lookAt(g_fly_pos, at, g_fly_up);
    float aspect = static_cast<float>(g_w) / std::max(1, g_h);
    // Near/far scaled to the map: a fixed 0.1 near plane on a 30k-unit map is
    // all the depth precision gone.
    float far_ = std::max(g_fly_view_dist * 1.2f, 1000.0f);
    float near_ = std::clamp(far_ * 0.00005f, 0.05f, 20.0f);
    Mat4 proj = perspective(1.15f, aspect, near_, far_);
    Mat4 VP = mul(view, proj);

    float planes[6][4];
    frustum_planes(VP, planes);

    // --- cull + LOD select, bucketed by (model, lod) ------------------------
    struct Bucket { int model, lod; std::vector<const FlyInstCPU*> insts; };
    static std::vector<Bucket> buckets;      // reused across frames; no per-frame churn
    static std::vector<int> bucketOf;
    buckets.clear();
    bucketOf.assign(g_fly_models.size() * 4, -1);

    int visible = 0;
    float cullDistSq = g_fly_view_dist * g_fly_view_dist;
    for (const auto& in : g_fly_insts) {
        if (!g_layer_visible[in.layer]) continue;
        float dx = in.centerW[0] - g_fly_pos.x;
        float dy = in.centerW[1] - g_fly_pos.y;
        float dz = in.centerW[2] - g_fly_pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        // Distance cull first: it is one compare and rejects most of a big map.
        if (d2 > cullDistSq + in.radiusW * in.radiusW) continue;

        bool out = false;
        for (int p = 0; p < 6 && !out; ++p) {
            float dist = planes[p][0] * in.centerW[0] + planes[p][1] * in.centerW[1] +
                         planes[p][2] * in.centerW[2] + planes[p][3];
            if (dist < -in.radiusW) out = true;
        }
        if (out) continue;

        // Screen-relative size picks the LOD: a distant prop that covers a few
        // pixels has no business submitting its full-detail index set.
        const FlyModelGPU& fm = g_fly_models[in.model];
        int maxLod = static_cast<int>(fm.lodRanges.size()) - 1;
        int lod = 0;
        if (maxLod > 0) {
            float d = std::sqrt(std::max(d2, 1e-4f));
            float sr = in.radiusW / d;             // ~ tan(angular radius)
            lod = (sr > 0.20f) ? 0 : (sr > 0.06f ? 1 : (sr > 0.02f ? 2 : 3));
            lod = std::min(lod, maxLod);
        }
        size_t key = static_cast<size_t>(in.model) * 4 + lod;
        if (key >= bucketOf.size()) continue;
        if (bucketOf[key] < 0) {
            bucketOf[key] = static_cast<int>(buckets.size());
            buckets.push_back({in.model, lod, {}});
        }
        buckets[bucketOf[key]].insts.push_back(&in);
        ++visible;
    }

    g_fly_stat_visible = visible;
    if (visible == 0) {
        g_fly_stat_draws = 0; g_fly_stat_tris = 0;
        QueryPerformanceCounter(&t1);
        if (g_qpc_freq.QuadPart)
            g_fly_stat_ms = static_cast<float>(1000.0 * double(t1.QuadPart - t0.QuadPart) / g_qpc_freq.QuadPart);
        return;
    }
    if (!ensure_inst_capacity(static_cast<uint32_t>(visible))) return;

    // --- one upload for the whole visible set -------------------------------
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_flyInstVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    auto* dst = static_cast<MapInstGPU*>(ms.pData);
    uint32_t off = 0;
    std::vector<uint32_t> bucketOffset(buckets.size());
    for (size_t b = 0; b < buckets.size(); ++b) {
        bucketOffset[b] = off;
        for (const FlyInstCPU* in : buckets[b].insts) pack_inst(in->world, in->layer, dst[off++]);
    }
    g_ctx->Unmap(g_flyInstVB.Get(), 0);

    // --- constants ----------------------------------------------------------
    FlyCB cb{};
    cb.viewProj = VP;
    const MapEnvRig& env = g_env_rig;
    Vec3 sun = norm({env.sunDir[0], env.sunDir[1], env.sunDir[2]});
    cb.sunDir[0] = sun.x; cb.sunDir[1] = sun.y; cb.sunDir[2] = sun.z;
    // Budget: ambient peaks near 0.30 and the sun adds up to ~0.55, so a fully
    // lit unoccluded surface lands around 0.85 instead of clipping to white --
    // which is what the first build did, flattening the whole map into a sheet
    // of paper with black holes in it.
    for (int k = 0; k < 3; ++k) cb.sunCol[k] = env.sunColor[k] * 0.55f;
    // Sky/ground come from the map's own fill colour so different zones do not
    // all read as the same grey box.
    cb.skyCol[0] = env.fillColor[0] * 0.26f; cb.skyCol[1] = env.fillColor[1] * 0.28f;
    cb.skyCol[2] = env.fillColor[2] * 0.32f;
    cb.groundCol[0] = 0.07f; cb.groundCol[1] = 0.065f; cb.groundCol[2] = 0.06f;
    cb.fogCol[0] = 0.10f; cb.fogCol[1] = 0.11f; cb.fogCol[2] = 0.13f;  // == the clear colour
    cb.exposure = g_light_intensity;
    cb.aoStrength = g_fly_ao_strength;
    cb.fogStart = g_fly_fog ? g_fly_view_dist * 0.55f : 1e9f;
    cb.fogEnd = g_fly_fog ? g_fly_view_dist : 1e9f + 1.0f;
    cb.camPos[0] = g_fly_pos.x; cb.camPos[1] = g_fly_pos.y; cb.camPos[2] = g_fly_pos.z;
    if (SUCCEEDED(g_ctx->Map(g_flyCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_flyCB.Get(), 0);
    }

    // --- draw ---------------------------------------------------------------
    g_ctx->IASetInputLayout(g_flyIL.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_flyVS.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_flyPS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_flyCB.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->RSSetState(g_fly_wire ? g_rsWire.Get() : g_rsSolid.Get());

    int draws = 0;
    size_t tris = 0;
    for (size_t b = 0; b < buckets.size(); ++b) {
        const Bucket& bk = buckets[b];
        const FlyModelGPU& fm = g_fly_models[bk.model];
        if (!fm.ok || bk.lod >= static_cast<int>(fm.lodRanges.size())) continue;
        auto range = fm.lodRanges[bk.lod];
        if (range.second == 0) continue;

        ID3D11Buffer* vbs[] = {fm.vb.Get(), g_flyInstVB.Get()};
        UINT strides[] = {sizeof(MapVtx), sizeof(MapInstGPU)};
        UINT offsets[] = {0, bucketOffset[b] * sizeof(MapInstGPU)};
        g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        g_ctx->IASetIndexBuffer(fm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_ctx->DrawIndexedInstanced(range.second, static_cast<UINT>(bk.insts.size()), range.first, 0, 0);
        ++draws;
        tris += static_cast<size_t>(range.second / 3) * bk.insts.size();
    }

    g_fly_stat_draws = draws;
    g_fly_stat_tris = static_cast<int>(tris / 1000);
    QueryPerformanceCounter(&t1);
    if (g_qpc_freq.QuadPart)
        g_fly_stat_ms = static_cast<float>(1000.0 * double(t1.QuadPart - t0.QuadPart) / g_qpc_freq.QuadPart);
}

} // namespace castlemist::render
