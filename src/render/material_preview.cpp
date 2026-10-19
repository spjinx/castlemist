/// @file
/// @brief Material thumbnails: a sphere drawn offscreen with a material's own
///        game shaders (material_preview.h).

#include "detail/state.h"

#include "detail/game_uniforms.h"

#include "castlemist/render/material_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace castlemist::render {
namespace {

// Offscreen targets for one thumbnail size.
struct PreviewTargets {
    ComPtr<ID3D11Texture2D> hdr, ldr, depth, staging;
    ComPtr<ID3D11RenderTargetView> hdrRTV, ldrRTV;
    ComPtr<ID3D11ShaderResourceView> hdrSRV;
    ComPtr<ID3D11DepthStencilView> dsv;
};
std::map<int, PreviewTargets> g_pvTargets;
ID3D11Device* g_pvDevice = nullptr;  // the device the cached objects belong to
ComPtr<ID3D11Buffer> g_sphereVB, g_sphereIB;
UINT g_sphereIndexCount = 0;

constexpr float kPi = 3.14159265358979f;
constexpr float kFov = 0.6f;    // vertical field of view
constexpr float kDist = 3.4f;   // camera distance from the unit sphere's centre

// A UV sphere, poles on Z (GW2 is Z-up), with the tangent frame the game VS read.
bool build_sphere() {
    constexpr int kLon = 64, kLat = 40;
    std::vector<GVertex> v;
    std::vector<uint32_t> idx;
    for (int j = 0; j <= kLat; ++j) {
        const float lat = -kPi / 2 + kPi * j / kLat;
        for (int i = 0; i <= kLon; ++i) {
            const float lon = 2 * kPi * i / kLon;
            const float cx = std::cos(lat) * std::cos(lon), cy = std::cos(lat) * std::sin(lon), cz = std::sin(lat);
            const float tx = -std::sin(lon), ty = std::cos(lon);
            GVertex g{cx, cy, cz, cx, cy, cz, tx, ty, 0,
                      // bitangent = n x t: points toward +lat
                      cy * 0 - cz * ty, cz * tx - cx * 0, cx * ty - cy * tx,
                      static_cast<float>(i) / kLon, 1.0f - static_cast<float>(j) / kLat};
            for (auto& uv : g.uv1) { uv[0] = g.u; uv[1] = g.v; }
            v.push_back(g);
        }
    }
    for (int j = 0; j < kLat; ++j)
        for (int i = 0; i < kLon; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * (kLon + 1) + i), b = a + kLon + 1;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(GVertex));
    D3D11_SUBRESOURCE_DATA sd{v.data(), 0, 0};
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_sphereVB))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint32_t));
    sd.pSysMem = idx.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &g_sphereIB))) return false;
    g_sphereIndexCount = static_cast<UINT>(idx.size());
    return true;
}

PreviewTargets* targets(int size) {
    auto it = g_pvTargets.find(size);
    if (it != g_pvTargets.end()) return &it->second;
    PreviewTargets t;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = static_cast<UINT>(size);
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;  // as the Shader-mode scene target
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.hdr))) return nullptr;
    g_dev->CreateRenderTargetView(t.hdr.Get(), nullptr, &t.hdrRTV);
    g_dev->CreateShaderResourceView(t.hdr.Get(), nullptr, &t.hdrSRV);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.ldr))) return nullptr;
    g_dev->CreateRenderTargetView(t.ldr.Get(), nullptr, &t.ldrRTV);
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.depth))) return nullptr;
    g_dev->CreateDepthStencilView(t.depth.Get(), nullptr, &t.dsv);
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.BindFlags = 0;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &t.staging))) return nullptr;
    return &g_pvTargets.emplace(size, std::move(t)).first->second;
}

std::array<float, 4> val4(const GameVals& vals, const char* name) {
    std::array<float, 4> out{};
    auto it = vals.find(name);
    if (it != vals.end())
        for (size_t k = 0; k < 4 && k < it->second.size(); ++k) out[k] = it->second[k];
    return out;
}

// The light buffer a deferred material would read for this sphere: the same SH
// ambient + sun the forward materials compute (see apply_env_uniforms), per pixel.
ComPtr<ID3D11ShaderResourceView> sphere_light_buffer(int size, const GameVals& vals, Vec3 eye, Vec3 right, Vec3 up,
                                                     Vec3 fwd) {
    const auto shR = val4(vals, "shRed"), shG = val4(vals, "shGreen"), shB = val4(vals, "shBlue");
    const auto sun = val4(vals, "shSun"), sunC = val4(vals, "shSunColor");
    const float th = std::tan(kFov * 0.5f);
    std::vector<float> px(static_cast<size_t>(size) * size * 4, 1.0f);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const float nx = (2.0f * (x + 0.5f) / size - 1.0f) * th, ny = (1.0f - 2.0f * (y + 0.5f) / size) * th;
            const Vec3 d = norm({right.x * nx + up.x * ny + fwd.x, right.y * nx + up.y * ny + fwd.y,
                                 right.z * nx + up.z * ny + fwd.z});
            // |eye + t d| = 1, nearest root.
            const float b = dot(eye, d), c = dot(eye, eye) - 1.0f, disc = b * b - c;
            if (disc < 0) continue;
            const float t = -b - std::sqrt(disc);
            const Vec3 n = norm({eye.x + t * d.x, eye.y + t * d.y, eye.z + t * d.z});
            const float ndl = std::clamp(n.x * sun[0] + n.y * sun[1] + n.z * sun[2], 0.0f, 1.0f);
            auto sh = [&](const std::array<float, 4>& s) { return n.x * s[0] + n.y * s[1] + n.z * s[2] + s[3]; };
            float* o = &px[(static_cast<size_t>(y) * size + x) * 4];
            o[0] = std::max(0.0f, sh(shR) + ndl * sunC[0]);
            o[1] = std::max(0.0f, sh(shG) + ndl * sunC[1]);
            o[2] = std::max(0.0f, sh(shB) + ndl * sunC[2]);
        }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = static_cast<UINT>(size);
    td.MipLevels = td.ArraySize = 1;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px.data(), static_cast<UINT>(size) * 16, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) g_dev->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}

} // namespace

bool render_material_preview(const GameMaterial& mat, const ModelPreview& textures, int size, std::vector<uint8_t>& rgba) {
    if (!g_dev || !g_ctx || size <= 0 || size > 1024 || !mat.ok) return false;
    if (g_pvDevice != g_dev.Get()) {  // a new device (reinitialised): drop what the old one owned
        g_pvTargets.clear();
        g_sphereVB.Reset();
        g_sphereIB.Reset();
        g_pvDevice = g_dev.Get();
    }
    if (!g_sphereVB && !build_sphere()) return false;
    PreviewTargets* t = targets(size);
    if (!t) return false;
    setup_game_uniforms();
    GameMatGPU g;
    if (!build_one_game_mat(mat, textures, g, nullptr, nullptr)) return false;

    // Camera: the sun's side of the sphere, turned a little so the terminator shows.
    GameVals vals = g_game_vals;
    const Vec3 worldUp{0, 0, 1};
    const auto sun = val4(vals, "shSun");
    Vec3 flat{sun[0], sun[1], 0};
    flat = dot(flat, flat) > 1e-4f ? norm(flat) : Vec3{0, -1, 0};
    const Vec3 side = norm(cross(worldUp, flat));
    const Vec3 dir = norm({flat.x * 0.8f + side.x * 0.55f, flat.y * 0.8f + side.y * 0.55f, 0.35f});
    const Vec3 eyeLocal{dir.x * kDist, dir.y * kDist, dir.z * kDist};
    const Vec3 park = kPreviewPark;  // where the model view parks its models (render_game)
    const Vec3 eye{park.x + eyeLocal.x, park.y + eyeLocal.y, park.z + eyeLocal.z};
    const Mat4 world = translate(park);
    const Mat4 view = lookAt(eye, park, worldUp);
    const Mat4 proj = perspective(kFov, 1.0f, 0.5f, 10.0f);
    const Mat4 vp = mul(view, proj);
    auto pM = [&](const char* n, const Mat4& m) {
        Mat4 tr{};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) tr.m[i * 4 + j] = m.m[j * 4 + i];
        vals[n] = std::vector<float>(tr.m, tr.m + 16);
    };
    pM("World", world);
    pM("ViewProjection", vp);
    pM("WorldViewProjection", mul(world, vp));
    pM("WorldView", mul(world, view));
    pM("View", view);
    vals["CameraPosition"] = {eye.x, eye.y, eye.z, 1};
    const float fs = static_cast<float>(size);
    vals["ScreenDims"] = {1.0f / fs, 1.0f / fs, fs, fs};
    game_update_cb(g.vcb.Get(), g.vsU, g.vsCB, vals, g.vsConsts);
    game_update_cb(g.pcb.Get(), g.psU, g.psCB, vals, g.psConsts);

    const Vec3 fwd = norm(sub(Vec3{0, 0, 0}, eyeLocal));
    const Vec3 right = norm(cross(worldUp, fwd));
    const Vec3 up = cross(fwd, right);
    ComPtr<ID3D11ShaderResourceView> light;
    if (g.lightSlots) light = sphere_light_buffer(size, vals, eyeLocal, right, up, fwd);

    const bool post = g_post_ready && g_postVS && g_postPS && g_postCB;
    ID3D11RenderTargetView* rtv = post ? t->hdrRTV.Get() : t->ldrRTV.Get();
    const float clear[4] = {0.17f, 0.18f, 0.20f, 1.0f};
    g_ctx->ClearRenderTargetView(rtv, clear);
    g_ctx->ClearDepthStencilView(t->dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_ctx->OMSetRenderTargets(1, &rtv, t->dsv.Get());
    D3D11_VIEWPORT viewport{0, 0, fs, fs, 0, 1};
    g_ctx->RSSetViewports(1, &viewport);
    g_ctx->RSSetState(g_rsSolid.Get());

    UINT stride = sizeof(GVertex), off = 0;
    ID3D11Buffer* vbs[] = {g_sphereVB.Get()};
    g_ctx->IASetInputLayout(g.layout.Get());
    g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
    g_ctx->IASetIndexBuffer(g_sphereIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g.vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g.ps.Get(), nullptr, 0);
    ID3D11Buffer* vcb[] = {g.vcb.Get()};
    ID3D11Buffer* pcb[] = {g.pcb.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, vcb);
    g_ctx->PSSetConstantBuffers(0, 1, pcb);
    ID3D11SamplerState* samps[16];
    for (auto& s : samps) s = g_samp.Get();
    g_ctx->PSSetSamplers(0, 16, samps);
    ID3D11ShaderResourceView* srvs[16];
    for (int i = 0; i < 16; i++) srvs[i] = (light && (g.lightSlots & (1u << i))) ? light.Get() : g.srv[i].Get();
    g_ctx->PSSetShaderResources(0, 16, srvs);
    const float bf[4] = {0, 0, 0, 0};
    if (g.blend) {
        g_ctx->OMSetDepthStencilState(g.depthWrite ? g_dss.Get() : g_dssNoWrite.Get(), 0);
        g_ctx->OMSetBlendState(g.blend.Get(), bf, 0xffffffff);
    } else {
        g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
        g_ctx->OMSetBlendState(g.opaqueBlend ? g.opaqueBlend.Get() : g_blendOpaque.Get(), bf, 0xffffffff);
    }
    g_ctx->DrawIndexed(g_sphereIndexCount, 0, 0);
    ID3D11ShaderResourceView* none[16] = {};
    g_ctx->PSSetShaderResources(0, 16, none);
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);

    if (post) {  // the Shader-mode post pass (exposure, gamma), onto the RGBA8 target
        ID3D11RenderTargetView* ldr = t->ldrRTV.Get();
        g_ctx->OMSetRenderTargets(1, &ldr, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_postCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const float p[4] = {g_post_exp, g_post_amb, 1.0f / g_post_gamma, 0.0f};
            std::memcpy(m.pData, p, sizeof p);
            g_ctx->Unmap(g_postCB.Get(), 0);
        }
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_postVS.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_postPS.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {g_postCB.Get()};
        g_ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* scene[] = {t->hdrSRV.Get()};
        g_ctx->PSSetShaderResources(0, 1, scene);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, none);
    }
    ID3D11RenderTargetView* nort[] = {nullptr};
    g_ctx->OMSetRenderTargets(1, nort, nullptr);

    g_ctx->CopyResource(t->staging.Get(), t->ldr.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(t->staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    rgba.resize(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
        std::memcpy(&rgba[static_cast<size_t>(y) * size * 4], static_cast<const uint8_t*>(m.pData) + y * m.RowPitch,
                    static_cast<size_t>(size) * 4);
    g_ctx->Unmap(t->staging.Get(), 0);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;  // some effects write no alpha
    return true;
}

} // namespace castlemist::render
