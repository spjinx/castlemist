/// @file
/// @brief WorldView: own D3D11 device, map camera, terrain chunks, streamed
///        instanced props, collision and water layers.
///
/// Shares nothing with the main renderer's globals (detail/state.h): every
/// device object lives in WorldView::Impl. The instanced draw (props,
/// collision, water) lives in world_draw.cpp.

#include "castlemist/render/world_view.h"

#include "castlemist/extract/map_types.h"
#include "castlemist/world/terrain_mesh.h"

#include "detail/mesh_bake.h"
#include "world_camera.h"
#include "world_draw.h"
#include "world_materials.h"
#include "world_props.h"
#include "world_view_shaders.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

namespace {

/// Terrain albedo (the fly view's LAYER_TERRAIN tint).
constexpr float kTerrainTint[3] = {0.52f, 0.56f, 0.46f};
/// Prop albedo (the fly view's LAYER_PROP tint).
constexpr float kPropTint[3] = {0.78f, 0.76f, 0.72f};
constexpr float kCollisionTint[3] = {0.90f, 0.42f, 0.36f};
constexpr float kWaterTint[3] = {0.30f, 0.55f, 0.85f};
constexpr float kWaterAlpha = 0.45f;
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
    // Instanced pipeline (props, collision, water).
    ComPtr<ID3D11VertexShader> vsInst;
    ComPtr<ID3D11PixelShader> psInst;
    ComPtr<ID3D11InputLayout> ilInst;
    ComPtr<ID3D11BlendState> bsAlpha;
    ComPtr<ID3D11DepthStencilState> dssNoWrite;   ///< water: depth-tested, not written
    int w = 1, h = 1;

    std::vector<ChunkGPU> chunks;
    std::map<int, GridIB> ibs;   ///< by sample count

    // Props: one MeshGPU per WorldScene::models entry, filled by add_models.
    std::vector<MeshGPU> models;
    std::vector<uint8_t> modelTried;                             ///< add_models already took it
    std::vector<std::shared_ptr<const ModelPreview>> previews;   ///< kept for picking
    std::vector<InstCPU> props;
    std::vector<std::vector<uint32_t>> instOf;                   ///< model -> indices into props
    // Collision: set_world keeps the CPU copy; ensure_collision() uploads
    // every mesh once, on first show (ruling R8), and releases the copy.
    castlemist::world::Collision colSrc;
    bool colPending = false;
    std::vector<MeshGPU> colMeshes;
    std::vector<InstCPU> colInsts;
    // Water: zero or one quad, one identity instance.
    std::vector<MeshGPU> waterMesh;
    std::vector<InstCPU> waterInst;

    InstancedBatch batch;   ///< this frame's instanced draw

    TextureCache textures;  ///< fileId -> texture, filled by add_textures

    // Lighting (MapEnvRig layout; defaults are the fly view's).
    float sunDir[3], sunColor[3], fillColor[3];

    // Map extent for frame_all (map space; lo.z is the highest point, up = -Z).
    bool hasExtent = false;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};

    bool layers[static_cast<int>(WorldLayer::Count)];
    WorldViewStats st;
    WorldCamera lastCam;
    bool hasFrame = false;
    bool lost = false;   ///< Device removed/reset or targets unrecoverable; owner must create() again.
    LARGE_INTEGER freq{};

    Impl() {
        for (bool& l : layers) l = true;
        layers[static_cast<int>(WorldLayer::Collision)] = false;
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

    /// Compiles @p src's VSMain/PSMain and creates both shaders and the input layout.
    bool compile(const char* src, const char* name, const D3D11_INPUT_ELEMENT_DESC* el, UINT nel,
                 ComPtr<ID3D11VertexShader>& v, ComPtr<ID3D11PixelShader>& p, ComPtr<ID3D11InputLayout>& layout) {
        ComPtr<ID3DBlob> vsb, psb, err;
        if (FAILED(D3DCompile(src, std::strlen(src), name, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vsb, &err)) ||
            FAILED(D3DCompile(src, std::strlen(src), name, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &psb, &err))) {
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &v)) &&
               SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p)) &&
               SUCCEEDED(dev->CreateInputLayout(el, nel, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout));
    }

    bool create_pipeline() {
        const D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (!compile(world_shaders::kTerrain, "world_terrain", el, 2, vs, ps, il)) return false;
        // Slot 1 steps once per instance (the fly view's layout, device.cpp).
        const D3D11_INPUT_ELEMENT_DESC eli[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        if (!compile(world_shaders::kInstanced, "world_instanced", eli, 6, vsInst, psInst, ilInst)) return false;

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
        if (FAILED(dev->CreateDepthStencilState(&dd, &dss))) return false;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(dev->CreateDepthStencilState(&dd, &dssNoWrite))) return false;

        D3D11_BLEND_DESC bl{};
        D3D11_RENDER_TARGET_BLEND_DESC& t = bl.RenderTarget[0];
        t.BlendEnable = TRUE;
        t.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        t.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOp = D3D11_BLEND_OP_ADD;
        t.SrcBlendAlpha = D3D11_BLEND_ONE;
        t.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        t.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        t.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        return SUCCEEDED(dev->CreateBlendState(&bl, &bsAlpha));
    }

    /// While lost, nothing touches the device (device_lost() semantics).
    bool gpu_ok() const { return dev && !lost; }

    /// Uploads the pending collision (once per set_world). While lost it
    /// stays pending; the owner recreates the view anyway.
    void ensure_collision() {
        if (!colPending || !gpu_ok()) return;
        upload_collision(dev.Get(), colSrc, colMeshes, colInsts);
        colPending = false;
        colSrc = castlemist::world::Collision{};   // release the CPU copy
        st.collisionMeshes = static_cast<int>(
            std::count_if(colMeshes.begin(), colMeshes.end(), [](const MeshGPU& m) { return m.ok; }));
    }

    /// One quad over [x0,x1] x [y0,y1] at height z, facing up (-Z), with one
    /// identity instance.
    void build_water(float x0, float y0, float x1, float y1, float z) {
        PreparedModel q;
        const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        for (int k = 0; k < 4; ++k) {
            MapVtx v{};
            v.px = xs[k]; v.py = ys[k]; v.pz = z;
            v.nx = enc_n(0); v.ny = enc_n(0); v.nz = enc_n(-1);
            v.ao = 255;
            q.verts.push_back(v);
        }
        q.indices = {0, 1, 2, 1, 3, 2};
        q.lodRanges.emplace_back(0u, 6u);
        q.center[0] = (x0 + x1) * 0.5f; q.center[1] = (y0 + y1) * 0.5f; q.center[2] = z;
        q.radius = std::max(0.5f * std::hypot(x1 - x0, y1 - y0), 1.0f);
        q.ok = true;
        MeshGPU m;
        if (!upload_mesh(dev.Get(), q, m)) return;
        waterMesh.push_back(std::move(m));
        InstCPU in;
        in.world = identity();
        place(in, waterMesh[0]);
        waterInst.push_back(in);
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

        // Props, then collision, then water: the translucent plane goes last
        // so everything under it is already in the depth buffer.
        batch.clear();
        if (layers[static_cast<int>(WorldLayer::Props)])
            visible += batch.cull_into(props, models, kPropTint, 1.0f, cam, farD, planes);
        if (layers[static_cast<int>(WorldLayer::Collision)])
            visible += batch.cull_into(colInsts, colMeshes, kCollisionTint, 1.0f, cam, farD, planes);
        if (layers[static_cast<int>(WorldLayer::Water)])
            visible += batch.cull_into(waterInst, waterMesh, kWaterTint, kWaterAlpha, cam, farD, planes);
        const InstPipeline pl{ilInst.Get(), vsInst.Get(), psInst.Get(), bsAlpha.Get(), dssNoWrite.Get(), dss.Get()};
        draws += batch.draw(dev.Get(), ctx.Get(), pl, tris);

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
    if (d->lost || !d->swap || w <= 0 || h <= 0) return;
    d->ctx->OMSetRenderTargets(0, nullptr, nullptr);
    d->rtv.Reset();
    d->dsv.Reset();
    d->ctx->Flush();
    d->w = w;
    d->h = h;
    // Either failure leaves the view without targets; report it as a lost
    // device so the owner recreates it rather than drawing nothing forever.
    if (FAILED(d->swap->ResizeBuffers(0, static_cast<UINT>(w), static_cast<UINT>(h), DXGI_FORMAT_UNKNOWN, 0)) ||
        !d->create_targets())
        d->lost = true;
}

void WorldView::set_world(const castlemist::world::WorldScene& s) {
    d->chunks.clear();
    d->models.assign(s.models.size(), MeshGPU{});
    d->modelTried.assign(s.models.size(), 0);
    d->previews.assign(s.models.size(), nullptr);
    d->instOf.assign(s.models.size(), {});
    d->props.clear();
    d->colSrc = castlemist::world::Collision{};
    d->colPending = false;
    d->colMeshes.clear();
    d->colInsts.clear();
    d->waterMesh.clear();
    d->waterInst.clear();
    d->hasExtent = false;
    d->hasFrame = false;
    d->st = WorldViewStats{};
    d->textures.clear();
    const auto& L = s.environment.light;
    if (L.present) {
        std::memcpy(d->sunDir, L.sunDir, sizeof d->sunDir);
        std::memcpy(d->sunColor, L.sunColor, sizeof d->sunColor);
        std::memcpy(d->fillColor, L.fillColor, sizeof d->fillColor);
    } else {
        d->reset_light();
    }
    if (d->gpu_ok() && s.terrain.present)
        for (size_t i = 0; i < s.terrain.chunks.size(); ++i) d->upload_chunk(s.terrain, i);
    d->st.chunks = static_cast<int>(d->chunks.size());
    if (!d->hasExtent && s.hasBounds) {
        d->lo[0] = std::min(s.bounds[0], s.bounds[2]); d->hi[0] = std::max(s.bounds[0], s.bounds[2]);
        d->lo[1] = std::min(s.bounds[1], s.bounds[3]); d->hi[1] = std::max(s.bounds[1], s.bounds[3]);
        d->lo[2] = d->hi[2] = 0;
        d->hasExtent = true;
    }

    // Props: recorded now, drawn once add_models brings their model. The
    // extent grows by each translation (ruling R7), so a props-only map
    // frames its props rather than z = 0.
    for (size_t pi = 0; pi < s.props.size(); ++pi) {
        const auto& p = s.props[pi];
        if (p.model >= s.models.size() || !finite16(p.world)) continue;
        InstCPU in;
        in.mesh = p.model;
        in.source = static_cast<uint32_t>(pi);
        in.world = colmajor_to_rowvec(p.world);
        in.scale = max_column_length(p.world);
        d->instOf[p.model].push_back(static_cast<uint32_t>(d->props.size()));
        d->props.push_back(in);
        const float* t = p.world + 12;   // column 3 = translation
        for (int k = 0; k < 3; ++k) {
            d->lo[k] = d->hasExtent ? std::min(d->lo[k], t[k]) : t[k];
            d->hi[k] = d->hasExtent ? std::max(d->hi[k], t[k]) : t[k];
        }
        d->hasExtent = true;
    }

    if (!d->gpu_ok()) return;

    // Collision: ~1M triangles on a big map, off by default -- kept on the
    // CPU here and uploaded when the layer is first shown (ruling R8).
    d->colSrc = s.collision;
    d->colPending = true;

    // Water: one plane at the stored height over the map bounds (or, with no
    // bounds, the extent so far). Where the game draws water is UNPROVEN (§6.2).
    if (s.water.hasPlane && std::isfinite(s.water.planeZ)) {
        if (s.hasBounds)
            d->build_water(std::min(s.bounds[0], s.bounds[2]), std::min(s.bounds[1], s.bounds[3]),
                           std::max(s.bounds[0], s.bounds[2]), std::max(s.bounds[1], s.bounds[3]), s.water.planeZ);
        else if (d->hasExtent && d->hi[0] > d->lo[0] && d->hi[1] > d->lo[1])
            d->build_water(d->lo[0], d->lo[1], d->hi[0], d->hi[1], s.water.planeZ);
    }
}

void WorldView::add_models(const std::vector<WorldModelLoad>& batch) {
    if (!d->gpu_ok()) return;
    for (const WorldModelLoad& l : batch) {
        const uint32_t m = l.model;
        if (!l.preview || m >= d->models.size()) { ++d->st.modelsFailed; continue; }
        if (d->modelTried[m]) continue;   // loaded, failed, or earlier in this batch
        d->modelTried[m] = 1;
        if (!l.prep || !upload_mesh(d->dev.Get(), *l.prep, d->models[m])) { ++d->st.modelsFailed; continue; }
        ++d->st.modelsLoaded;
        d->previews[m] = l.preview;
        for (uint32_t pi : d->instOf[m]) place(d->props[pi], d->models[m]);
    }
}

void WorldView::add_textures(const std::vector<std::shared_ptr<const CappedTexture>>& batch) {
    if (!d->gpu_ok()) return;
    for (const auto& t : batch)
        if (t) d->textures.add(d->dev.Get(), *t);
}

void WorldView::add_models(const std::vector<std::pair<uint32_t, std::shared_ptr<const ModelPreview>>>& batch) {
    if (!d->gpu_ok()) return;
    // The CPU half (flatten + AO) touches no D3D and runs across cores; the
    // prepared overload then creates the buffers serially. Models the view
    // will ignore (out of range, already tried, null) are not prepared.
    std::vector<WorldModelLoad> loads(batch.size());
    std::vector<uint8_t> seen(d->models.size(), 0);
    std::vector<size_t> todo;
    for (size_t i = 0; i < batch.size(); ++i) {
        loads[i].model = batch[i].first;
        loads[i].preview = batch[i].second;
        const uint32_t m = batch[i].first;
        if (!batch[i].second || m >= d->models.size() || d->modelTried[m] || seen[m]) continue;
        seen[m] = 1;
        todo.push_back(i);
    }
    parallel_for(todo.size(), [&](size_t k) {
        loads[todo[k]].prep = prepare_world_model_shared(*batch[todo[k]].second, false);
    });
    add_models(loads);
}

void WorldView::render(const WorldCamera& cam) {
    if (d->lost || !d->ctx || !d->rtv || !d->dsv) return;
    if (d->layers[static_cast<int>(WorldLayer::Collision)]) d->ensure_collision();
    d->draw(cam);
    d->lastCam = cam;
    d->hasFrame = true;
    const HRESULT hr = d->swap->Present(1, 0);   // DXGI_STATUS_OCCLUDED is not an error
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) d->lost = true;
}

void WorldView::set_layer(WorldLayer l, bool on) {
    const int i = static_cast<int>(l);
    if (i < 0 || i >= static_cast<int>(WorldLayer::Count)) return;
    d->layers[i] = on;
    if (on && l == WorldLayer::Collision) d->ensure_collision();
}

int WorldView::pick(int sx, int sy, const WorldCamera& cam) const {
    if (!d->layers[static_cast<int>(WorldLayer::Props)]) return -1;
    float o[3], dir[3];
    world_camera_ray(cam, d->w, d->h, sx, sy, o, dir);
    // Sphere test first; candidates nearest-entry first, so the exact test
    // stops once no remaining sphere can beat the best hit.
    std::vector<std::pair<float, uint32_t>> cand;   // (sphere entry t, index into props)
    for (uint32_t i = 0; i < d->props.size(); ++i) {
        const InstCPU& in = d->props[i];
        if (!in.ready || in.mesh >= d->previews.size() || !d->previews[in.mesh]) continue;
        const float oc[3] = {in.centerW[0] - o[0], in.centerW[1] - o[1], in.centerW[2] - o[2]};
        const float tca = oc[0] * dir[0] + oc[1] * dir[1] + oc[2] * dir[2];
        const float d2 = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - tca * tca;
        const float r2 = in.radiusW * in.radiusW;
        if (!(d2 <= r2)) continue;   // also rejects NaN
        const float thc = std::sqrt(r2 - d2);
        if (tca + thc < 0) continue;   // behind the camera
        cand.emplace_back(std::max(tca - thc, 0.0f), i);
    }
    std::sort(cand.begin(), cand.end());
    int best = -1;
    float bestT = 0;
    for (const auto& [enter, i] : cand) {
        if (best >= 0 && enter > bestT) break;
        const InstCPU& in = d->props[i];
        // in.world holds the scene's column-major bytes as-is (colmajor_to_rowvec).
        const float t = ray_model_hit(*d->previews[in.mesh], in.world.m, o, dir);
        if (t >= 0 && (best < 0 || t < bestT)) {
            best = static_cast<int>(in.source);
            bestT = t;
        }
    }
    return best;
}

bool WorldView::layer(WorldLayer l) const {
    const int i = static_cast<int>(l);
    return i >= 0 && i < static_cast<int>(WorldLayer::Count) && d->layers[i];
}

WorldViewStats WorldView::stats() const {
    WorldViewStats s = d->st;
    s.textures = d->textures.ready();
    s.texturesFailed = d->textures.failed();
    s.textureMB = static_cast<float>(static_cast<double>(d->textures.bytes()) / (1024.0 * 1024.0));
    return s;
}

bool WorldView::device_lost() const { return d->lost; }

bool WorldView::save_screenshot(const char* bmpPath) {
    if (d->lost || !bmpPath || !d->ctx || !d->rtv || !d->dsv || !d->hasFrame) return false;
    // A DISCARD swap chain's back buffer is undefined after Present, so the
    // last frame is drawn again (same camera) and read before presenting.
    // The redraw must not change what stats() reports about the presented frame.
    if (d->layers[static_cast<int>(WorldLayer::Collision)]) d->ensure_collision();
    const WorldViewStats keep = d->st;
    d->draw(d->lastCam);
    d->st = keep;
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
