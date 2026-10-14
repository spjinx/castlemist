/// @file
/// @brief WorldView: own D3D11 device, map camera, terrain chunks.
///
/// Shares nothing with the main renderer's globals (detail/state.h): every
/// device object lives in WorldView::Impl.

#include "castlemist/render/world_view.h"

#include "castlemist/extract/map_types.h"
#include "castlemist/world/terrain_mesh.h"

#include "detail/mesh_bake.h"
#include "world_camera.h"
#include "world_view_shaders.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

namespace {

/// Terrain albedo (the fly view's LAYER_TERRAIN tint).
constexpr float kTerrainTint[3] = {0.52f, 0.56f, 0.46f};
/// Clear colour == fog colour (the fly view's).
constexpr float kClear[4] = {0.10f, 0.11f, 0.13f, 1.0f};

/// Constant buffer for world_shaders::kTerrain (register b0).
struct WorldCB {
    Mat4 viewProj;
    float sunDir[3];    float exposure;
    float sunCol[3];    float aoStrength;
    float skyCol[3];    float fogStart;
    float groundCol[3]; float fogEnd;
    float fogCol[3];    float _pad0;
    float camPos[3];    float _pad1;
    float albedo[3];    float _pad2;
};
static_assert(sizeof(WorldCB) % 16 == 0, "constant buffers are 16-byte multiples");

/// One uploaded terrain chunk.
struct ChunkGPU {
    ComPtr<ID3D11Buffer> vb;
    int samples = 0;
    float center[3] = {0, 0, 0};
    float radius = 0;
};

/// The shared index buffer for one sample count.
struct GridIB {
    ComPtr<ID3D11Buffer> ib;
    UINT count = 0;
};

/// Two triangles per sample quad, row-major sample order (index j*n + i).
std::vector<uint32_t> grid_indices(int n) {
    std::vector<uint32_t> idx;
    idx.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * n + i), b = a + 1;
            const uint32_t c = a + static_cast<uint32_t>(n), e = c + 1;
            idx.insert(idx.end(), {a, c, b, b, c, e});
        }
    return idx;
}

} // namespace

struct WorldView::Impl {
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> il;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11RasterizerState> rs;
    ComPtr<ID3D11DepthStencilState> dss;
    int w = 1, h = 1;

    std::vector<ChunkGPU> chunks;
    std::map<int, GridIB> ibs;   ///< by sample count

    // Lighting (MapEnvRig layout; defaults are the fly view's).
    float sunDir[3], sunColor[3], fillColor[3];

    // Map extent for frame_all (map space; lo.z is the highest point, up = -Z).
    bool hasExtent = false;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};

    bool layers[static_cast<int>(WorldLayer::Count)];
    WorldViewStats st;
    WorldCamera lastCam;
    bool hasFrame = false;
    LARGE_INTEGER freq{};

    Impl() {
        for (bool& l : layers) l = true;
        QueryPerformanceFrequency(&freq);
        reset_light();
    }

    void reset_light() {
        const MapEnvRig def;
        std::memcpy(sunDir, def.sunDir, sizeof sunDir);
        std::memcpy(sunColor, def.sunColor, sizeof sunColor);
        std::memcpy(fillColor, def.fillColor, sizeof fillColor);
    }

    bool create_targets() {
        rtv.Reset();
        dsv.Reset();
        ComPtr<ID3D11Texture2D> back;
        if (FAILED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        if (FAILED(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv))) return false;
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = static_cast<UINT>(w);
        dd.Height = static_cast<UINT>(h);
        dd.MipLevels = 1;
        dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> ds;
        if (FAILED(dev->CreateTexture2D(&dd, nullptr, &ds))) return false;
        return SUCCEEDED(dev->CreateDepthStencilView(ds.Get(), nullptr, &dsv));
    }

    bool create_pipeline() {
        ComPtr<ID3DBlob> vsb, psb, err;
        const char* src = world_shaders::kTerrain;
        if (FAILED(D3DCompile(src, std::strlen(src), "world_terrain", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb,
                              &err)) ||
            FAILED(D3DCompile(src, std::strlen(src), "world_terrain", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb,
                              &err))) {
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        if (FAILED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs)) ||
            FAILED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps)))
            return false;
        const D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(dev->CreateInputLayout(el, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il))) return false;

        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(WorldCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return false;

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;   // as the fly view: no winding assumption
        rd.DepthClipEnable = TRUE;
        if (FAILED(dev->CreateRasterizerState(&rd, &rs))) return false;

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        return SUCCEEDED(dev->CreateDepthStencilState(&dd, &dss));
    }

    const GridIB* grid_ib(int n) {
        auto it = ibs.find(n);
        if (it != ibs.end()) return it->second.ib ? &it->second : nullptr;
        GridIB& g = ibs[n];
        const std::vector<uint32_t> idx = grid_indices(n);
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint32_t));
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{idx.data(), 0, 0};
        if (FAILED(dev->CreateBuffer(&bd, &sd, &g.ib))) return nullptr;
        g.count = static_cast<UINT>(idx.size());
        return &g;
    }

    void upload_chunk(const castlemist::world::Terrain& t, size_t ci) {
        const castlemist::world::TerrainChunk& c = t.chunks[ci];
        const int n = c.samples;
        if (n < 2 || c.heights.size() != static_cast<size_t>(n) * n) return;
        const std::vector<float> nrm = castlemist::world::chunk_normals(t, ci);
        if (nrm.size() != c.heights.size() * 3) return;

        const float x0 = c.rect[0], y0 = c.rect[1], x1 = c.rect[2], y1 = c.rect[3];
        const float sx = (x1 - x0) / static_cast<float>(n - 1), sy = (y1 - y0) / static_cast<float>(n - 1);
        std::vector<MapVtx> v(c.heights.size());
        float zlo = 1e30f, zhi = -1e30f;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const size_t k = static_cast<size_t>(j) * n + i;
                MapVtx& o = v[k];
                o.px = x0 + static_cast<float>(i) * sx;
                o.py = y1 - static_cast<float>(j) * sy;
                o.pz = c.heights[k];
                o.nx = enc_n(nrm[k * 3 + 0]);
                o.ny = enc_n(nrm[k * 3 + 1]);
                o.nz = enc_n(nrm[k * 3 + 2]);
                o.ao = 255;
                if (std::isfinite(o.pz)) { zlo = std::min(zlo, o.pz); zhi = std::max(zhi, o.pz); }
            }
        if (zlo > zhi) zlo = zhi = 0;
        if (!grid_ib(n)) return;

        ChunkGPU g;
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(MapVtx));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{v.data(), 0, 0};
        if (FAILED(dev->CreateBuffer(&bd, &sd, &g.vb))) return;
        g.samples = n;
        g.center[0] = (x0 + x1) * 0.5f;
        g.center[1] = (y0 + y1) * 0.5f;
        g.center[2] = (zlo + zhi) * 0.5f;
        const float ex = (x1 - x0) * 0.5f, ey = (y1 - y0) * 0.5f, ez = (zhi - zlo) * 0.5f;
        g.radius = std::sqrt(ex * ex + ey * ey + ez * ez);
        chunks.push_back(std::move(g));

        if (!hasExtent) {
            lo[0] = std::min(x0, x1); lo[1] = std::min(y0, y1); lo[2] = zlo;
            hi[0] = std::max(x0, x1); hi[1] = std::max(y0, y1); hi[2] = zhi;
            hasExtent = true;
        } else {
            lo[0] = std::min({lo[0], x0, x1}); lo[1] = std::min({lo[1], y0, y1}); lo[2] = std::min(lo[2], zlo);
            hi[0] = std::max({hi[0], x0, x1}); hi[1] = std::max({hi[1], y0, y1}); hi[2] = std::max(hi[2], zhi);
        }
    }

    /// Clears, culls and draws into the back buffer (no Present).
    void draw(const WorldCamera& cam) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);

        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), dsv.Get());
        D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
        ctx->RSSetViewports(1, &vp);
        ctx->ClearRenderTargetView(rtv.Get(), kClear);
        ctx->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

        const float farD = std::max(cam.farDist, 1.0f);
        const Mat4 VP = world_camera_viewproj(cam, static_cast<float>(w) / static_cast<float>(std::max(1, h)));
        float planes[6][4];
        frustum_planes(VP, planes);

        WorldCB c{};
        c.viewProj = VP;
        const Vec3 sun = norm({sunDir[0], sunDir[1], sunDir[2]});
        c.sunDir[0] = sun.x; c.sunDir[1] = sun.y; c.sunDir[2] = sun.z;
        // The fly view's budget (render_fly): sun up to ~0.55, ambient ~0.30.
        for (int k = 0; k < 3; ++k) c.sunCol[k] = sunColor[k] * 0.55f;
        c.skyCol[0] = fillColor[0] * 0.26f; c.skyCol[1] = fillColor[1] * 0.28f; c.skyCol[2] = fillColor[2] * 0.32f;
        c.groundCol[0] = 0.07f; c.groundCol[1] = 0.065f; c.groundCol[2] = 0.06f;
        for (int k = 0; k < 3; ++k) c.fogCol[k] = kClear[k];
        c.exposure = 1.0f;
        c.aoStrength = 1.0f;
        // farDist is both the cull distance and the far plane here; the fog
        // ramp keeps the fly view's ratio to the cull distance.
        c.fogStart = farD * 0.55f;
        c.fogEnd = farD;
        for (int k = 0; k < 3; ++k) c.camPos[k] = cam.pos[k];
        for (int k = 0; k < 3; ++k) c.albedo[k] = kTerrainTint[k];
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &c, sizeof c);
            ctx->Unmap(cb.Get(), 0);
        }

        ctx->IASetInputLayout(il.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, cb.GetAddressOf());
        ctx->OMSetDepthStencilState(dss.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->RSSetState(rs.Get());

        int visible = 0, draws = 0;
        size_t tris = 0;
        if (layers[static_cast<int>(WorldLayer::Terrain)]) {
            for (const ChunkGPU& g : chunks) {
                const float dx = g.center[0] - cam.pos[0], dy = g.center[1] - cam.pos[1],
                            dz = g.center[2] - cam.pos[2];
                const float reach = farD + g.radius;
                if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
                bool out = false;
                for (int p = 0; p < 6 && !out; ++p)
                    out = planes[p][0] * g.center[0] + planes[p][1] * g.center[1] + planes[p][2] * g.center[2] +
                              planes[p][3] < -g.radius;
                if (out) continue;
                const GridIB& ib = ibs[g.samples];
                const UINT stride = sizeof(MapVtx), offset = 0;
                ctx->IASetVertexBuffers(0, 1, g.vb.GetAddressOf(), &stride, &offset);
                ctx->IASetIndexBuffer(ib.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
                ctx->DrawIndexed(ib.count, 0, 0);
                ++visible;
                ++draws;
                tris += ib.count / 3;
            }
        }

        st.visible = visible;
        st.draws = draws;
        st.ktris = static_cast<int>(tris / 1000);
        QueryPerformanceCounter(&t1);
        if (freq.QuadPart)
            st.ms = static_cast<float>(1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
                                       static_cast<double>(freq.QuadPart));
    }
};

WorldView::WorldView() : d(std::make_unique<Impl>()) {}
WorldView::~WorldView() = default;

bool WorldView::create(HWND surface) {
    d = std::make_unique<Impl>();
    if (!surface) return false;
    RECT rc{};
    GetClientRect(surface, &rc);
    d->w = std::max<int>(1, rc.right - rc.left);
    d->h = std::max<int>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = static_cast<UINT>(d->w);
    sc.BufferDesc.Height = static_cast<UINT>(d->h);
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = surface;
    sc.SampleDesc.Count = 1;
    sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
                                               D3D11_SDK_VERSION, &sc, &d->swap, &d->dev, &got, &d->ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, ARRAYSIZE(levels),
                                           D3D11_SDK_VERSION, &sc, &d->swap, &d->dev, &got, &d->ctx);
    if (FAILED(hr) || !d->create_targets() || !d->create_pipeline()) {
        d = std::make_unique<Impl>();
        return false;
    }
    return true;
}

void WorldView::resize(int w, int h) {
    if (!d->swap || w <= 0 || h <= 0) return;
    d->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    d->rtv.Reset();
    d->dsv.Reset();
    d->ctx->Flush();
    d->w = w;
    d->h = h;
    if (FAILED(d->swap->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0))) return;
    d->create_targets();
}

void WorldView::set_world(const castlemist::world::WorldScene& s) {
    d->chunks.clear();
    d->hasExtent = false;
    d->hasFrame = false;
    d->st = WorldViewStats{};
    const auto& L = s.environment.light;
    if (L.present) {
        std::memcpy(d->sunDir, L.sunDir, sizeof d->sunDir);
        std::memcpy(d->sunColor, L.sunColor, sizeof d->sunColor);
        std::memcpy(d->fillColor, L.fillColor, sizeof d->fillColor);
    } else {
        d->reset_light();
    }
    if (d->dev && s.terrain.present)
        for (size_t i = 0; i < s.terrain.chunks.size(); ++i) d->upload_chunk(s.terrain, i);
    d->st.chunks = static_cast<int>(d->chunks.size());
    if (!d->hasExtent && s.hasBounds) {
        d->lo[0] = std::min(s.bounds[0], s.bounds[2]); d->hi[0] = std::max(s.bounds[0], s.bounds[2]);
        d->lo[1] = std::min(s.bounds[1], s.bounds[3]); d->hi[1] = std::max(s.bounds[1], s.bounds[3]);
        d->lo[2] = d->hi[2] = 0;
        d->hasExtent = true;
    }
}

void WorldView::render(const WorldCamera& cam) {
    if (!d->ctx || !d->rtv || !d->dsv) return;
    d->draw(cam);
    d->lastCam = cam;
    d->hasFrame = true;
    d->swap->Present(1, 0);
}

void WorldView::set_layer(WorldLayer l, bool on) {
    const int i = static_cast<int>(l);
    if (i >= 0 && i < static_cast<int>(WorldLayer::Count)) d->layers[i] = on;
}

bool WorldView::layer(WorldLayer l) const {
    const int i = static_cast<int>(l);
    return i >= 0 && i < static_cast<int>(WorldLayer::Count) && d->layers[i];
}

WorldViewStats WorldView::stats() const { return d->st; }

bool WorldView::save_screenshot(const char* bmpPath) {
    if (!bmpPath || !d->ctx || !d->rtv || !d->dsv || !d->hasFrame) return false;
    // A DISCARD swap chain's back buffer is undefined after Present, so the
    // last frame is drawn again (same camera) and read before presenting.
    d->draw(d->lastCam);
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(d->swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(d->dev->CreateTexture2D(&sd, nullptr, &stg))) return false;
    d->ctx->CopyResource(stg.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(d->ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;

    const int w = static_cast<int>(td.Width), h = static_cast<int>(td.Height);
    const int stride = (w * 3 + 3) & ~3;
    const uint32_t dataSize = static_cast<uint32_t>(stride) * static_cast<uint32_t>(h), fileSize = 54 + dataSize;
    std::vector<uint8_t> bmp(fileSize, 0);
    const uint32_t off = 54, ih = 40;
    const uint16_t planes = 1, bpp = 24;
    bmp[0] = 'B'; bmp[1] = 'M';
    std::memcpy(&bmp[2], &fileSize, 4);
    std::memcpy(&bmp[10], &off, 4);
    std::memcpy(&bmp[14], &ih, 4);
    std::memcpy(&bmp[18], &w, 4);
    std::memcpy(&bmp[22], &h, 4);
    std::memcpy(&bmp[26], &planes, 2);
    std::memcpy(&bmp[28], &bpp, 2);
    std::memcpy(&bmp[34], &dataSize, 4);
    const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = src + static_cast<size_t>(h - 1 - y) * ms.RowPitch;   // BMP is bottom-up
        uint8_t* dst = bmp.data() + 54 + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x) {   // RGBA -> BGR
            dst[x * 3 + 0] = row[x * 4 + 2];
            dst[x * 3 + 1] = row[x * 4 + 1];
            dst[x * 3 + 2] = row[x * 4 + 0];
        }
    }
    d->ctx->Unmap(stg.Get(), 0);
    FILE* f = std::fopen(bmpPath, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bmp.data(), 1, bmp.size(), f) == bmp.size();
    return std::fclose(f) == 0 && ok;
}

WorldCamera WorldView::frame_all(float* speedHint) const {
    WorldCamera cam;
    float lo[3] = {-1000, -1000, 0}, hi[3] = {1000, 1000, 0};
    if (d->hasExtent) {
        std::memcpy(lo, d->lo, sizeof lo);
        std::memcpy(hi, d->hi, sizeof hi);
    }
    const Vec3 c{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    const float ex = hi[0] - lo[0], ey = hi[1] - lo[1], ez = hi[2] - lo[2];
    float R = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    if (!std::isfinite(R) || R < 1.0f) R = 1.0f;

    // The fly view's framing (fly_frame_scene): back along -X and above the
    // highest point -- "above" is the smallest z, since up is -Z.
    const Vec3 pos{c.x - R * 0.62f, c.y, lo[2] - R * 0.28f};
    const Vec3 fwd = norm(sub(c, pos));
    Vec3 right = cross(kWorldUp, fwd);
    if (dot(right, right) < 1e-6f) right = Vec3{0, 1, 0};
    const Vec3 up = norm(cross(fwd, norm(right)));
    cam.pos[0] = pos.x; cam.pos[1] = pos.y; cam.pos[2] = pos.z;
    cam.fwd[0] = fwd.x; cam.fwd[1] = fwd.y; cam.fwd[2] = fwd.z;
    cam.up[0] = up.x; cam.up[1] = up.y; cam.up[2] = up.z;
    cam.farDist = std::max(R * 3.0f, 5000.0f);
    if (speedHint) *speedHint = std::clamp(R * 0.5f, 20.0f, 20000.0f);
    return cam;
}

} // namespace castlemist::render
