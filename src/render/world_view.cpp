/// @file
/// @brief WorldView: own D3D11 device, map camera, terrain chunks, streamed
///        instanced props, collision and water layers.
///
/// Shares nothing with the main renderer's globals (detail/state.h): every
/// device object lives in WorldView::Impl. The instanced draw (props,
/// collision, water) lives in world_draw.cpp.

#include "castlemist/render/world_view.h"

#include "castlemist/extract/map_types.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/world/anim.h"
#include "castlemist/world/terrain_mesh.h"

#include "detail/light_calib.h"
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

/// Constant buffer for world_shaders::kTerrain (register b0). The AO-look
/// shaders (kTerrain, kInstanced) declare it up to `albedo`; the textured ones
/// (kTerrainMat, kPropMat) also read the calibrated light (ruling P6).
struct WorldCB {
    Mat4 viewProj;
    float sunDir[3];    float exposure;
    float sunCol[3];    float aoStrength;
    float skyCol[3];    float fogStart;
    float groundCol[3]; float fogEnd;
    float fogCol[3];    float _pad0;
    float camPos[3];    float _pad1;
    float albedo[3];    float _pad2;
    /// Textured shaders only: detail/light_calib.h's sun and ambient DC (the
    /// game_material.cpp calibration). The AO look keeps sunCol / skyCol.
    float matSunCol[3]; float _pad3;
    float matSkyCol[3]; float _pad4;
};
static_assert(sizeof(WorldCB) % 16 == 0, "constant buffers are 16-byte multiples");

/// One terrain vertex: MapVtx plus the sample's chunk coordinate (s, t) as
/// UNORM16, s = i/(n-1) from the west, t = j/(n-1) from the north. Both
/// terrain pipelines read it; kTerrain ignores (s, t).
struct TerrainVtx {
    float px, py, pz;
    uint8_t nx, ny, nz, ao;
    uint16_t s, t;
};
static_assert(sizeof(TerrainVtx) == 20, "TerrainVtx must stay 20 bytes -- the input layouts hardcode the stride");

/// Constant buffer for world_shaders::kTerrainMat's ChunkCB (register b1).
struct ChunkCB {
    float pickerOffset[2]; float pickerScale; float groundRepeats;
    float present[4];
    float normalMaps; float specPower; float albedoOnly; float _pad;
};
static_assert(sizeof(ChunkCB) % 16 == 0, "constant buffers are 16-byte multiples");

/// Constant buffer for world_shaders::kWater's WaterCB (register b1).
struct WaterCB {
    float heightOrigin[2]; float heightStep[2];
    float heightSize[2];   float planeZ; float depthAtt;
    float shallow[4];      ///< rgb, w = srgb decode
    float deep[4];         ///< rgb, w = surfaceFresnel
    float bumpM0[4], bumpM1[4];
    float bumpOff[4];
    float bumpAmount; float specPower; float hasBump; float hasHeight;
};
static_assert(sizeof(WaterCB) % 16 == 0, "constant buffers are 16-byte multiples");

/// The map-wide terrain height grid kWater samples (spec WaterPass): R32F
/// over the terrain sample grid, (chunksX (S-1) + 1) x (chunksY (S-1) + 1)
/// with S the first chunk's samples, every k-th sample so neither side passes
/// 4096. Texel (0, 0) is the grid's west/north corner; a texel no chunk
/// writes (a missing chunk, another sample count, a non-finite height) is
/// @p planeZ. False when the terrain has no usable chunk.
struct HeightGrid {
    std::vector<float> z;
    int w = 0, h = 0;
    float origin[2] = {0, 0};   ///< map x of column 0, map y of row 0 (the north edge)
    float step[2] = {0, 0};     ///< map units per texel: x east, y south
};
bool build_height_grid(const castlemist::world::Terrain& t, float planeZ, HeightGrid& g) {
    constexpr int kMaxSide = 4096;
    if (!t.present || t.chunksX <= 0 || t.chunksY <= 0) return false;
    const castlemist::world::TerrainChunk* ref = nullptr;
    for (const auto& c : t.chunks)
        if (c.samples >= 2 && c.heights.size() == static_cast<size_t>(c.samples) * c.samples &&
            c.rect[2] > c.rect[0] && c.rect[3] > c.rect[1]) { ref = &c; break; }
    if (!ref) return false;
    const int S = ref->samples;
    const long long W = static_cast<long long>(t.chunksX) * (S - 1) + 1;
    const long long H = static_cast<long long>(t.chunksY) * (S - 1) + 1;
    // k = ceil((side - 1) / (kMaxSide - 1)), so (side - 1) / k + 1 <= kMaxSide.
    const long long k = std::max<long long>(
        {1, (W - 1 + kMaxSide - 2) / (kMaxSide - 1), (H - 1 + kMaxSide - 2) / (kMaxSide - 1)});
    g.w = static_cast<int>((W - 1) / k + 1);
    g.h = static_cast<int>((H - 1) / k + 1);
    const float cw = ref->rect[2] - ref->rect[0], ch = ref->rect[3] - ref->rect[1];
    g.origin[0] = ref->rect[0] - static_cast<float>(ref->cx) * cw;
    g.origin[1] = ref->rect[3] + static_cast<float>(ref->cy) * ch;
    g.step[0] = cw / static_cast<float>(S - 1) * static_cast<float>(k);
    g.step[1] = ch / static_cast<float>(S - 1) * static_cast<float>(k);
    g.z.assign(static_cast<size_t>(g.w) * g.h, planeZ);
    for (const auto& c : t.chunks) {
        if (c.samples != S || c.heights.size() != static_cast<size_t>(S) * S) continue;
        if (c.cx < 0 || c.cy < 0 || c.cx >= t.chunksX || c.cy >= t.chunksY) continue;
        for (int j = 0; j < S; ++j) {
            const long long gy = static_cast<long long>(c.cy) * (S - 1) + j;
            if (gy % k) continue;
            for (int i = 0; i < S; ++i) {
                const long long gx = static_cast<long long>(c.cx) * (S - 1) + i;
                if (gx % k) continue;
                const float z = c.heights[static_cast<size_t>(j) * S + i];
                if (std::isfinite(z)) g.z[static_cast<size_t>(gy / k) * g.w + static_cast<size_t>(gx / k)] = z;
            }
        }
    }
    return true;
}

/// One uploaded terrain chunk.
struct ChunkGPU {
    ComPtr<ID3D11Buffer> vb;
    int samples = 0;
    float center[3] = {0, 0, 0};
    float radius = 0;
    size_t src = 0;                    ///< index into the scene's terrain.chunks
    float rect[4] = {0, 0, 0, 0};      ///< x0, y0, x1, y1 (capture_chunk_albedo)
    // Material (note §1.2); see Impl::chunk_textured.
    bool resolved = false;
    float pickerOffset[2] = {0, 0};
    float pickerScale = 0;
    uint32_t color[4] = {0, 0, 0, 0}, normal[4] = {0, 0, 0, 0};
    uint32_t picker = 0, picker2 = 0;
    /// 1x1 UNORM pages from pickerSolid / picker2Solid (stored byte order,
    /// UNPROVEN, note §1.6), made when that page has no fileId.
    ComPtr<ID3D11ShaderResourceView> solid, solid2;
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
    // Textured terrain (kTerrainMat).
    ComPtr<ID3D11VertexShader> vsMat;
    ComPtr<ID3D11PixelShader> psMat;
    ComPtr<ID3D11InputLayout> ilMat;
    ComPtr<ID3D11Buffer> cbChunk;
    ComPtr<ID3D11SamplerState> sWrap, sClamp;
    ComPtr<ID3D11ShaderResourceView> flatNormal;   ///< 1x1 (128,128,255,0): a missing normal slot
    // Instanced pipeline (props, collision, water).
    ComPtr<ID3D11VertexShader> vsInst;
    ComPtr<ID3D11PixelShader> psInst;
    ComPtr<ID3D11InputLayout> ilInst;
    ComPtr<ID3D11BlendState> bsAlpha;
    ComPtr<ID3D11DepthStencilState> dssNoWrite;   ///< water: depth-tested, not written
    // Water from its env preset (kWater).
    ComPtr<ID3D11VertexShader> vsWater;
    ComPtr<ID3D11PixelShader> psWater;
    ComPtr<ID3D11InputLayout> ilWater;
    ComPtr<ID3D11Buffer> cbWater;
    // Prop material pass (kPropMat).
    ComPtr<ID3D11VertexShader> vsProp;
    ComPtr<ID3D11PixelShader> psProp;
    ComPtr<ID3D11InputLayout> ilProp;
    ComPtr<ID3D11Buffer> cbMat;                   ///< MatCB (world_draw.cpp)
    /// Pass 2's blend state per WorldAlpha (null for Opaque, Clip, Unmapped);
    /// [Blend] is bsAlpha.
    ComPtr<ID3D11BlendState> bsProp[static_cast<int>(WorldAlpha::Count)];
    // Skinned props (kPropSkinned): PSMain textured, PSAo the AO look.
    ComPtr<ID3D11VertexShader> vsSkin;
    ComPtr<ID3D11PixelShader> psSkin, psSkinAo;
    ComPtr<ID3D11InputLayout> ilSkin;
    ComPtr<ID3D11Buffer> cbSkin;                  ///< SkinCB (world_draw.cpp), per skinned bucket
    ComPtr<ID3D11Buffer> palette;                 ///< StructuredBuffer<float4x4>, every pose of the frame
    ComPtr<ID3D11ShaderResourceView> paletteSrv;
    UINT paletteCap = 0;                          ///< bones
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
    /// The plane's env preset (spec WaterPass): set_world copies
    /// environment.water[light.presetIndex] when the plane exists and the
    /// index is in range. waterNoPreset: a plane without one (warnings()).
    bool waterHasPreset = false;
    castlemist::model::Extractor::MapEnvWater waterPreset;
    float waterPlaneZ = 0;
    bool waterNoPreset = false;
    int waterPresetIndex = -1;
    ComPtr<ID3D11ShaderResourceView> heightSrv;   ///< build_height_grid's R32F; null without terrain
    HeightGrid heightGrid;                          ///< its placement (z released after upload)

    InstancedBatch batch;       ///< this frame's instanced draw (props too under Geometry)
    InstancedBatch propBatch;   ///< props under Materials: the material pass (static ones)
    InstancedBatch skinBatch;   ///< props under Materials that play a clip: (model, clip, LOD) buckets
    /// Per model, per clip index: the pose source of each clip an instance
    /// plays (add_models); bones borrow `previews[model]`.
    std::vector<std::vector<PoseRig>> rigs;
    int clipMisses = 0, skinMisses = 0;   ///< unresolvedClips split: no such clip / no skin stream

    TextureCache textures;  ///< fileId -> texture, filled by add_textures
    WorldShading shading = WorldShading::Materials;
    bool motion = true;
    bool hasWater = false;      ///< a water preset is loaded (set_world)
    double clock = 0;           ///< set_time
    int scrollingMaterials = 0, stillTransforms = 0;   ///< loaded props' scrolling materials; texTransforms drawn still
    WorldMaterialSettings mat;

    // Lighting (MapEnvRig layout; defaults are the fly view's).
    float sunDir[3], sunColor[3], fillColor[3];
    float sunIntensity = 1, fillIntensity = 0.3f;   ///< the env rig's; the textured shaders' calibration reads them

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
        sunIntensity = def.sunIntensity;
        fillIntensity = def.fillIntensity;
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

    /// Compiles @p src's pixel shader @p entry.
    bool compile_ps(const char* src, const char* name, const char* entry, ComPtr<ID3D11PixelShader>& p) {
        ComPtr<ID3DBlob> psb, err;
        if (FAILED(D3DCompile(src, std::strlen(src), name, nullptr, nullptr, entry, "ps_5_0", 0, 0, &psb, &err))) {
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return false;
        }
        return SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &p));
    }

    /// Writes @p pal (16 floats per bone) into the palette buffer, growing it
    /// when needed. False with nothing to write or on failure.
    bool upload_palette(const std::vector<float>& pal) {
        const UINT bones = static_cast<UINT>(pal.size() / 16);
        if (bones == 0) return false;
        if (!palette || paletteCap < bones) {
            const UINT cap = std::max<UINT>(bones + bones / 2, 1024);
            D3D11_BUFFER_DESC bd{};
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.ByteWidth = cap * 64;
            bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 64;
            ComPtr<ID3D11Buffer> nb;
            ComPtr<ID3D11ShaderResourceView> nv;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format = DXGI_FORMAT_UNKNOWN;
            vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
            vd.Buffer.FirstElement = 0;
            vd.Buffer.NumElements = cap;
            if (FAILED(dev->CreateBuffer(&bd, nullptr, &nb)) ||
                FAILED(dev->CreateShaderResourceView(nb.Get(), &vd, &nv)))
                return false;
            palette = nb;
            paletteSrv = nv;
            paletteCap = cap;
        }
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(palette.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
        std::memcpy(ms.pData, pal.data(), pal.size() * sizeof(float));
        ctx->Unmap(palette.Get(), 0);
        return true;
    }

    /// One pose per (model, clip) with a visible skinned bucket, at @p t
    /// (time()), packed into the palette buffer (spec SkinnedPropPass). Every
    /// instance starts at t = 0 and loops, so instances of a (model, clip)
    /// share it.
    bool pose_skinned(double t) {
        const std::vector<std::pair<uint32_t, int>> keys = skinBatch.palette_keys();
        if (keys.empty()) return false;
        std::vector<float> all, one;
        std::vector<uint32_t> base(keys.size(), 0), count(keys.size(), 0);
        for (size_t k = 0; k < keys.size(); ++k) {
            const auto [m, c] = keys[k];
            if (m >= rigs.size() || c < 0 || static_cast<size_t>(c) >= rigs[m].size()) continue;
            pose_palette(rigs[m][static_cast<size_t>(c)], t, one);
            base[k] = static_cast<uint32_t>(all.size() / 16);
            count[k] = static_cast<uint32_t>(one.size() / 16);
            all.insert(all.end(), one.begin(), one.end());
        }
        skinBatch.set_palettes(base, count);
        return upload_palette(all);
    }

    /// The ready playing instance (clip >= 0) whose world centre is nearest
    /// @p pos, or null.
    const InstCPU* nearest_animated(const float pos[3]) const {
        const InstCPU* best = nullptr;
        float bestD2 = 0;
        for (const InstCPU& in : props) {
            if (!in.ready || in.clip < 0) continue;
            const float dx = in.centerW[0] - pos[0], dy = in.centerW[1] - pos[1], dz = in.centerW[2] - pos[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (!best || d2 < bestD2) { best = &in; bestD2 = d2; }
        }
        return best;
    }

    bool create_pipeline() {
        const D3D11_INPUT_ELEMENT_DESC el[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (!compile(world_shaders::kTerrain, "world_terrain", el, 2, vs, ps, il)) return false;
        const D3D11_INPUT_ELEMENT_DESC elm[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R16G16_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (!compile(world_shaders::kTerrainMat, "world_terrain_mat", elm, 3, vsMat, psMat, ilMat)) return false;
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
        // kWater: the water quad's MapVtx (stride 16), positions only.
        if (!compile(world_shaders::kWater, "world_water", el, 1, vsWater, psWater, ilWater)) return false;
        // kPropMat: MatVtx (stride 32: kInstanced's 16 bytes, then uv0, uv1) + the instance stream.
        const D3D11_INPUT_ELEMENT_DESC elp[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 5, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        };
        if (!compile(world_shaders::kPropMat, "world_prop_mat", elp, 8, vsProp, psProp, ilProp)) return false;
        // kPropSkinned: kPropMat's layout plus SkinVtx in slot 2.
        const D3D11_INPUT_ELEMENT_DESC els[] = {
            elp[0], elp[1], elp[2], elp[3], elp[4], elp[5], elp[6], elp[7],
            {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 2, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 4, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        const std::string skinSrc = world_shaders::prop_skinned_source();
        if (!compile(skinSrc.c_str(), "world_prop_skinned", els, 10, vsSkin, psSkin, ilSkin) ||
            !compile_ps(skinSrc.c_str(), "world_prop_skinned", "PSAo", psSkinAo))
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = sizeof(WorldCB);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb))) return false;
        bd.ByteWidth = sizeof(ChunkCB);
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cbChunk))) return false;
        bd.ByteWidth = kMatCBBytes;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cbMat))) return false;
        bd.ByteWidth = kSkinCBBytes;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cbSkin))) return false;
        bd.ByteWidth = sizeof(WaterCB);
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &cbWater))) return false;

        // Trilinear; pages clamp, ground wraps.
        D3D11_SAMPLER_DESC sm{};
        sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sm.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sm, &sWrap))) return false;
        sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(dev->CreateSamplerState(&sm, &sClamp))) return false;
        const uint8_t flat[4] = {128, 128, 255, 0};
        if (!(flatNormal = texel_srv(flat))) return false;

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
        if (FAILED(dev->CreateBlendState(&bl, &bsAlpha))) return false;
        bsProp[static_cast<int>(WorldAlpha::Blend)] = bsAlpha;
        // The prop pass's other modes: each preset's colour factors exactly
        // (decode_blend's table); the target's alpha is kept.
        t.SrcBlendAlpha = D3D11_BLEND_ZERO;
        t.DestBlendAlpha = D3D11_BLEND_ONE;
        const struct { WorldAlpha a; D3D11_BLEND src, dst; } modes[] = {
            {WorldAlpha::Additive, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ONE},
            {WorldAlpha::AdditiveOne, D3D11_BLEND_ONE, D3D11_BLEND_ONE},
            {WorldAlpha::Premultiplied, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
            {WorldAlpha::Multiply, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO},
            {WorldAlpha::Multiply2x, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_SRC_COLOR},
        };
        for (const auto& m : modes) {
            t.SrcBlend = m.src;
            t.DestBlend = m.dst;
            if (FAILED(dev->CreateBlendState(&bl, &bsProp[static_cast<int>(m.a)]))) return false;
        }
        return true;
    }

    /// A 1x1 R8G8B8A8_UNORM texture holding @p rgba; null on failure.
    ComPtr<ID3D11ShaderResourceView> texel_srv(const uint8_t rgba[4]) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 1;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{rgba, 4, 0};
        ComPtr<ID3D11Texture2D> t;
        ComPtr<ID3D11ShaderResourceView> v;
        if (FAILED(dev->CreateTexture2D(&td, &sd, &t)) || FAILED(dev->CreateShaderResourceView(t.Get(), nullptr, &v)))
            return nullptr;
        return v;
    }

    /// The UNORM page view of fileId @p id, or @p solid when the page has no
    /// file; null unless available.
    ID3D11ShaderResourceView* page_srv(uint32_t id, const ComPtr<ID3D11ShaderResourceView>& solid) const {
        return id ? textures.srv(id, false) : solid.Get();
    }

    /// Whether chunk @p g draws textured: Materials, resolved, a page rect,
    /// both pages available (Ready or solid), at least one colour slot, and
    /// every non-zero colour slot Ready. With no colour slot at all every
    /// ground SRV would be null (black), so it takes the AO look (ruling P5).
    bool chunk_textured(const ChunkGPU& g) const {
        if (shading != WorldShading::Materials || !g.resolved || !(g.pickerScale > 0)) return false;
        if (!page_srv(g.picker, g.solid) || !page_srv(g.picker2, g.solid2)) return false;
        bool any = false;
        for (uint32_t id : g.color) {
            if (id && textures.state(id) != TextureCache::State::Ready) return false;
            any = any || id != 0;
        }
        return any;
    }

    /// Binds chunk @p g's textures and ChunkCB for kTerrainMat (call only
    /// when chunk_textured(g)).
    void bind_chunk_material(const ChunkGPU& g, bool albedoOnly) {
        ChunkCB c{};
        c.pickerOffset[0] = g.pickerOffset[0];
        c.pickerOffset[1] = g.pickerOffset[1];
        c.pickerScale = g.pickerScale;
        c.groundRepeats = mat.groundRepeatsPerChunk;
        for (int k = 0; k < 4; ++k) c.present[k] = g.color[k] ? 1.0f : 0.0f;
        c.normalMaps = mat.terrainNormalMaps ? 1.0f : 0.0f;
        c.specPower = mat.terrainSpecPower;
        c.albedoOnly = albedoOnly ? 1.0f : 0.0f;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(cbChunk.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &c, sizeof c);
            ctx->Unmap(cbChunk.Get(), 0);
        }
        ID3D11ShaderResourceView* srv[10] = {};
        for (int k = 0; k < 4; ++k) {
            // An absent colour slot stays null: its weight is 0 (terrain_blend).
            srv[k] = g.color[k] ? textures.srv(g.color[k], mat.srgbColourTextures) : nullptr;
            ID3D11ShaderResourceView* n = g.normal[k] ? textures.srv(g.normal[k], false) : nullptr;
            srv[6 + k] = n ? n : flatNormal.Get();
        }
        srv[4] = page_srv(g.picker, g.solid);
        srv[5] = page_srv(g.picker2, g.solid2);
        ctx->PSSetShaderResources(0, 10, srv);
    }

    /// Selects the terrain pipeline: kTerrainMat when @p textured, else kTerrain.
    void use_terrain_pipeline(bool textured) {
        ctx->IASetInputLayout(textured ? ilMat.Get() : il.Get());
        ctx->VSSetShader(textured ? vsMat.Get() : vs.Get(), nullptr, 0);
        ctx->PSSetShader(textured ? psMat.Get() : ps.Get(), nullptr, 0);
    }

    void unbind_terrain_textures() {
        ID3D11ShaderResourceView* none[10] = {};
        ctx->PSSetShaderResources(0, 10, none);
    }

    void upload_world_cb(const WorldCB& c) {
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &c, sizeof c);
            ctx->Unmap(cb.Get(), 0);
        }
    }

    /// The state both terrain pipelines share: cbuffers, samplers, topology,
    /// raster, depth, blend.
    void bind_terrain_state() {
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* cbs[2] = {cb.Get(), cbChunk.Get()};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ID3D11SamplerState* ss[2] = {sWrap.Get(), sClamp.Get()};
        ctx->PSSetSamplers(0, 2, ss);
        ctx->OMSetDepthStencilState(dss.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->RSSetState(rs.Get());
    }

    void draw_chunk(const ChunkGPU& g) {
        const GridIB& ib = ibs[g.samples];
        const UINT stride = sizeof(TerrainVtx), offset = 0;
        ctx->IASetVertexBuffers(0, 1, g.vb.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(ib.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(ib.count, 0, 0);
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

    /// Uploads @p g as heightSrv (R32_FLOAT, one mip) and keeps its placement.
    void upload_height_grid(HeightGrid& g) {
        heightSrv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(g.w);
        td.Height = static_cast<UINT>(g.h);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd{g.z.data(), static_cast<UINT>(g.w) * 4, 0};
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(dev->CreateTexture2D(&td, &sd, &t))) dev->CreateShaderResourceView(t.Get(), nullptr, &heightSrv);
        g.z.clear();
        g.z.shrink_to_fit();
        heightGrid = std::move(g);
    }

    /// Whether the water plane draws from its preset this frame (Materials
    /// shading, a preset, the quad uploaded); otherwise the flat sub-project 2 water.
    bool water_from_preset() const {
        return shading == WorldShading::Materials && waterHasPreset && !waterMesh.empty() && !waterInst.empty() &&
               waterInst[0].ready;
    }

    /// Draws the water quad with kWater (call only when water_from_preset()):
    /// alpha-blended, depth-tested, no depth writes. Returns 1 when drawn
    /// (inside the far distance and the frustum), else 0.
    int draw_water(const float planes[6][4], const WorldCamera& cam, float farD, size_t& tris) {
        const InstCPU& in = waterInst[0];
        const MeshGPU& m = waterMesh[0];
        const float dx = in.centerW[0] - cam.pos[0], dy = in.centerW[1] - cam.pos[1], dz = in.centerW[2] - cam.pos[2];
        const float reach = farD + in.radiusW;
        if (dx * dx + dy * dy + dz * dz > reach * reach) return 0;
        for (int p = 0; p < 6; ++p)
            if (planes[p][0] * in.centerW[0] + planes[p][1] * in.centerW[1] + planes[p][2] * in.centerW[2] +
                    planes[p][3] < -in.radiusW)
                return 0;

        const castlemist::model::Extractor::MapEnvWater& w = waterPreset;
        WaterCB c{};
        const bool hasHeight = heightSrv && heightGrid.w > 0 && heightGrid.h > 0;
        c.heightOrigin[0] = heightGrid.origin[0];
        c.heightOrigin[1] = heightGrid.origin[1];
        c.heightStep[0] = hasHeight ? heightGrid.step[0] : 1.0f;
        c.heightStep[1] = hasHeight ? heightGrid.step[1] : 1.0f;
        c.heightSize[0] = static_cast<float>(std::max(1, heightGrid.w));
        c.heightSize[1] = static_cast<float>(std::max(1, heightGrid.h));
        c.planeZ = waterPlaneZ;
        c.depthAtt = w.depthAttenuation;
        water_colour(w.surfaceShallowColor, c.shallow);
        water_colour(w.surfaceDeepColor, c.deep);
        c.shallow[3] = mat.srgbColourTextures ? 1.0f : 0.0f;
        c.deep[3] = w.surfaceFresnel;
        // Each bump's affine map from water_bump_uv (the one place its
        // UNPROVEN units live, note §6): offset at (0, 0), then the x and y
        // axes. The offset is frac'd here (the bump wraps), so float UVs stay
        // precise however long the clock runs.
        const double t = motion ? clock : 0.0;
        const float unit = mat.waterTileUnit;
        float* M[2] = {c.bumpM0, c.bumpM1};
        for (int k = 0; k < 2; ++k) {
            float o[2], ex[2], ey[2];
            const float ang = water_bump_angle(w.bumpAngle[k], mat.waterBumpAngleDegrees);   // ruling P10
            water_bump_uv(0, 0, w.bumpTile[k], w.bumpSpeed[k], ang, t, unit, o);
            water_bump_uv(1, 0, w.bumpTile[k], 0, ang, 0, unit, ex);
            water_bump_uv(0, 1, w.bumpTile[k], 0, ang, 0, unit, ey);
            M[k][0] = ex[0]; M[k][1] = ey[0];   // u = dot(M.xy, xy)
            M[k][2] = ex[1]; M[k][3] = ey[1];   // v = dot(M.zw, xy)
            c.bumpOff[2 * k + 0] = o[0] - std::floor(o[0]);
            c.bumpOff[2 * k + 1] = o[1] - std::floor(o[1]);
        }
        const int bi = mat.waterBumpTextureIndex;
        const uint32_t bumpId = (bi >= 0 && static_cast<size_t>(bi) < w.textureFileIds.size())
                                    ? w.textureFileIds[static_cast<size_t>(bi)] : 0;
        ID3D11ShaderResourceView* bump = bumpId ? textures.srv(bumpId, false) : nullptr;
        c.bumpAmount = water_bump_strength(w.bumpAmount, mat.waterBumpStrengthScale);   // ruling P10
        c.specPower = mat.waterSpecPower;
        c.hasBump = bump ? 1.0f : 0.0f;   // missing texture: flat N
        c.hasHeight = hasHeight ? 1.0f : 0.0f;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(cbWater.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return 0;
        std::memcpy(ms.pData, &c, sizeof c);
        ctx->Unmap(cbWater.Get(), 0);

        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(ilWater.Get());
        ctx->VSSetShader(vsWater.Get(), nullptr, 0);
        ctx->PSSetShader(psWater.Get(), nullptr, 0);
        ID3D11Buffer* cbs[2] = {cb.Get(), cbWater.Get()};
        ctx->VSSetConstantBuffers(0, 1, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ID3D11SamplerState* ss[2] = {sWrap.Get(), sClamp.Get()};
        ctx->PSSetSamplers(0, 2, ss);
        ID3D11ShaderResourceView* srv[2] = {bump, hasHeight ? heightSrv.Get() : nullptr};
        ctx->PSSetShaderResources(0, 2, srv);
        ctx->OMSetBlendState(bsAlpha.Get(), nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(dssNoWrite.Get(), 0);
        ctx->RSSetState(rs.Get());
        const UINT stride = m.stride, offset = 0;
        ctx->IASetVertexBuffers(0, 1, m.vb.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(m.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        const auto [first, count] = m.lodRanges[0];
        ctx->DrawIndexed(count, first, 0);
        tris += count / 3;
        ID3D11ShaderResourceView* none[2] = {};
        ctx->PSSetShaderResources(0, 2, none);
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(dss.Get(), 0);
        return 1;
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
        std::vector<TerrainVtx> v(c.heights.size());
        float zlo = 1e30f, zhi = -1e30f;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const size_t k = static_cast<size_t>(j) * n + i;
                TerrainVtx& o = v[k];
                o.px = x0 + static_cast<float>(i) * sx;
                o.py = y1 - static_cast<float>(j) * sy;
                o.pz = c.heights[k];
                o.nx = enc_n(nrm[k * 3 + 0]);
                o.ny = enc_n(nrm[k * 3 + 1]);
                o.nz = enc_n(nrm[k * 3 + 2]);
                o.ao = 255;
                o.s = static_cast<uint16_t>(std::lround(65535.0 * i / (n - 1)));
                o.t = static_cast<uint16_t>(std::lround(65535.0 * j / (n - 1)));
                if (std::isfinite(o.pz)) { zlo = std::min(zlo, o.pz); zhi = std::max(zhi, o.pz); }
            }
        if (zlo > zhi) zlo = zhi = 0;
        if (!grid_ib(n)) return;

        ChunkGPU g;
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.ByteWidth = static_cast<UINT>(v.size() * sizeof(TerrainVtx));
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{v.data(), 0, 0};
        if (FAILED(dev->CreateBuffer(&bd, &sd, &g.vb))) return;
        g.samples = n;
        g.center[0] = (x0 + x1) * 0.5f;
        g.center[1] = (y0 + y1) * 0.5f;
        g.center[2] = (zlo + zhi) * 0.5f;
        const float ex = (x1 - x0) * 0.5f, ey = (y1 - y0) * 0.5f, ez = (zhi - zlo) * 0.5f;
        g.radius = std::sqrt(ex * ex + ey * ey + ez * ez);
        g.src = ci;
        std::memcpy(g.rect, c.rect, sizeof g.rect);
        const castlemist::world::TerrainMaterial& m = c.material;
        g.resolved = m.resolved;
        g.pickerOffset[0] = m.pickerOffset[0];
        g.pickerOffset[1] = m.pickerOffset[1];
        g.pickerScale = m.pickerScale;
        for (size_t k = 0; k < 4; ++k) {
            g.color[k] = k < m.textureFileIds.size() ? m.textureFileIds[k] : 0;
            g.normal[k] = k < m.normalFileIds.size() ? m.normalFileIds[k] : 0;
        }
        g.picker = m.pickerFileId;
        g.picker2 = m.picker2FileId;
        if (m.resolved && !g.picker) g.solid = texel_srv(m.pickerSolid);
        if (m.resolved && !g.picker2) g.solid2 = texel_srv(m.picker2Solid);
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
        // The textured shaders' light: the measured calibration for textured
        // game materials (game_material.cpp's rig, via detail/light_calib.h).
        // The AO look's budget above is tuned for a constant 0.78 albedo and
        // left real textures about 2x too dark (ruling P6).
        calibrated_light(sunColor, sunIntensity, fillColor, fillIntensity, 1.0f, c.matSunCol, c.matSkyCol);
        upload_world_cb(c);
        bind_terrain_state();

        int visible = 0, draws = 0, untextured = 0;
        size_t tris = 0;
        int cur = -1;   // terrain pipeline bound: -1 none yet, 0 kTerrain, 1 kTerrainMat
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
                const bool tex = chunk_textured(g);
                if (cur != static_cast<int>(tex)) {
                    use_terrain_pipeline(tex);
                    cur = static_cast<int>(tex);
                }
                if (tex)
                    bind_chunk_material(g, false);
                else
                    ++untextured;
                draw_chunk(g);
                ++visible;
                ++draws;
                tris += ibs[g.samples].count / 3;
            }
        }
        if (cur == 1) unbind_terrain_textures();

        // Props, then collision, then water: the translucent plane goes last
        // so everything under it is already in the depth buffer. Under
        // Materials the props take the material pass instead: pass 1
        // (opaque, clip, the AO look) before collision and water, pass 2
        // (blend, additive; no depth writes) after them.
        // Under Materials the props that play a clip take the skinned pass
        // (the same two passes, with kPropSkinned); Geometry draws them all static.
        batch.clear();
        propBatch.clear();
        skinBatch.clear();
        const bool matProps = shading == WorldShading::Materials;
        if (layers[static_cast<int>(WorldLayer::Props)]) {
            if (matProps) {
                visible += propBatch.cull_into(props, models, kPropTint, 1.0f, cam, farD, planes, CullSkin::Static);
                visible += skinBatch.cull_into(props, models, kPropTint, 1.0f, cam, farD, planes, CullSkin::Skinned);
            } else {
                visible += batch.cull_into(props, models, kPropTint, 1.0f, cam, farD, planes);
            }
        }
        if (layers[static_cast<int>(WorldLayer::Collision)])
            visible += batch.cull_into(colInsts, colMeshes, kCollisionTint, 1.0f, cam, farD, planes);
        // Water: the flat sub-project 2 plane in the instanced batch, unless
        // it draws from its env preset (kWater, right after the batch: still
        // after pass 1 and collision, before pass 2).
        const bool presetWater = layers[static_cast<int>(WorldLayer::Water)] && water_from_preset();
        if (layers[static_cast<int>(WorldLayer::Water)] && !presetWater)
            visible += batch.cull_into(waterInst, waterMesh, kWaterTint, kWaterAlpha, cam, farD, planes);
        const InstPipeline pl{ilInst.Get(), vsInst.Get(), psInst.Get(), bsAlpha.Get(), dssNoWrite.Get(), dss.Get()};
        ID3D11BlendState* blendOf[static_cast<int>(WorldAlpha::Count)];
        for (int k = 0; k < static_cast<int>(WorldAlpha::Count); ++k) blendOf[k] = bsProp[k].Get();
        const PropMatPipeline pm{pl,      ilProp.Get(), vsProp.Get(), psProp.Get(), cbMat.Get(), sWrap.Get(),
                                 blendOf, &textures,    mat.srgbColourTextures,
                                 motion ? clock : 0.0, mat.uvScrollUnitSeconds};
        // The skinned pass: kPropSkinned's PSAo for the AO look, PSMain textured.
        PropMatPipeline sk = pm;
        sk.ao = InstPipeline{ilSkin.Get(), vsSkin.Get(), psSkinAo.Get(), bsAlpha.Get(), dssNoWrite.Get(), dss.Get()};
        sk.il = ilSkin.Get();
        sk.vs = vsSkin.Get();
        sk.ps = psSkin.Get();
        sk.cbSkin = cbSkin.Get();
        int matUntextured = 0;
        const bool propsUp = matProps && propBatch.upload(dev.Get(), ctx.Get());
        const bool skinUp = matProps && pose_skinned(pm.time) && skinBatch.upload(dev.Get(), ctx.Get());
        if (skinUp) ctx->VSSetShaderResources(1, 1, paletteSrv.GetAddressOf());
        if (propsUp) draws += propBatch.draw_materials(ctx.Get(), pm, 1, tris, matUntextured);
        if (skinUp) draws += skinBatch.draw_materials(ctx.Get(), sk, 1, tris, matUntextured);
        draws += batch.draw(dev.Get(), ctx.Get(), pl, tris);
        if (presetWater) {
            const int n = draw_water(planes, cam, farD, tris);
            visible += n;
            draws += n;
        }
        // Pass 2 per batch: the blended static buckets, then the blended
        // skinned ones (each sorted back to front within its own batch only).
        if (propsUp) draws += propBatch.draw_materials(ctx.Get(), pm, 2, tris, matUntextured);
        if (skinUp) {
            draws += skinBatch.draw_materials(ctx.Get(), sk, 2, tris, matUntextured);
            ID3D11ShaderResourceView* none = nullptr;
            ctx->VSSetShaderResources(1, 1, &none);
            ID3D11Buffer* noVb = nullptr;
            const UINT zero = 0;
            ctx->IASetVertexBuffers(2, 1, &noVb, &zero, &zero);
        }

        st.visible = visible;
        st.draws = draws;
        st.chunksUntextured = untextured;
        st.materialsUntextured = matUntextured;
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
    d->rigs.assign(s.models.size(), {});
    d->clipMisses = d->skinMisses = 0;
    d->instOf.assign(s.models.size(), {});
    d->props.clear();
    d->colSrc = castlemist::world::Collision{};
    d->colPending = false;
    d->colMeshes.clear();
    d->colInsts.clear();
    d->waterMesh.clear();
    d->waterInst.clear();
    d->scrollingMaterials = d->stillTransforms = 0;
    d->hasWater = false;
    d->waterHasPreset = d->waterNoPreset = false;
    d->waterPreset = castlemist::model::Extractor::MapEnvWater{};
    d->waterPresetIndex = s.environment.light.presetIndex;
    d->heightSrv.Reset();
    d->heightGrid = HeightGrid{};
    d->hasExtent = false;
    d->hasFrame = false;
    d->st = WorldViewStats{};
    d->textures.clear();
    const auto& L = s.environment.light;
    if (L.present) {
        std::memcpy(d->sunDir, L.sunDir, sizeof d->sunDir);
        std::memcpy(d->sunColor, L.sunColor, sizeof d->sunColor);
        std::memcpy(d->fillColor, L.fillColor, sizeof d->fillColor);
        d->sunIntensity = L.sunIntensity;
        d->fillIntensity = L.fillIntensity;
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
        in.animSequence = p.animSequence;
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
    // The plane's env preset (spec WaterPass): water preset k with lighting
    // preset k (pairing UNPROVEN, note §6). No preset: the flat water, warned.
    if (!d->waterMesh.empty()) {
        const int pi = s.environment.light.presetIndex;
        if (pi >= 0 && static_cast<size_t>(pi) < s.environment.water.size()) {
            d->waterHasPreset = true;
            d->waterPreset = s.environment.water[static_cast<size_t>(pi)];
            d->waterPlaneZ = s.water.planeZ;
            d->hasWater = true;
            d->st.waterPreset = true;
            HeightGrid g;
            if (build_height_grid(s.terrain, s.water.planeZ, g)) d->upload_height_grid(g);
        } else {
            d->waterNoPreset = true;
        }
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
        // The material pass's state: each material's diffuse, and its alpha
        // mode for every combination of the texture's flags (read when the
        // texture is Ready, at draw time; prop_alpha_index).
        const std::vector<PropMaterial> pms = prop_materials(*l.preview, &d->stillTransforms);
        std::vector<PropMatGPU>& mats = d->models[m].materials;
        mats.resize(pms.size());
        for (size_t k = 0; k < pms.size(); ++k) {
            mats[k].m = pms[k];
            if (pms[k].scroll >= 0) ++d->scrollingMaterials;
            for (int f = 0; f < 8; ++f)
                mats[k].alphaBy[f] =
                    world_alpha_mode(l.preview->materials[k], (f & 1) != 0, (f & 2) != 0, (f & 4) != 0);
        }
        ++d->st.modelsLoaded;
        d->previews[m] = l.preview;
        // Clips (spec SkinnedPropPass, note §5): an instance plays the clip
        // its animSequence names when the model has a skin stream; otherwise
        // it stays in bind pose and is counted. No fallback rule (UNPROVEN).
        const ModelPreview& pv = *l.preview;
        const bool skinnable = d->models[m].vbSkin && !pv.joints.empty();
        for (uint32_t pi : d->instOf[m]) {
            InstCPU& in = d->props[pi];
            place(in, d->models[m]);
            in.clip = -1;
            if (!in.ready || in.animSequence == 0) continue;
            const int c = castlemist::world::resolve_prop_clip(pv.animClipTokens, in.animSequence);
            if (c < 0 || static_cast<size_t>(c) >= pv.animClips.size()) {
                ++d->st.unresolvedClips;
                ++d->clipMisses;
                continue;
            }
            if (!skinnable) {
                ++d->st.unresolvedClips;
                ++d->skinMisses;
                continue;
            }
            in.clip = c;
            ++d->st.animated;
            std::vector<PoseRig>& rs = d->rigs[m];
            if (rs.size() < pv.animClips.size()) rs.resize(pv.animClips.size());
            PoseRig& rig = rs[static_cast<size_t>(c)];
            if (!rig.clip) {   // as skeleton.cpp's pose_bones
                rig.bones.resize(pv.joints.size());
                for (size_t j = 0; j < pv.joints.size(); ++j) {
                    const ModelJoint& b = pv.joints[j];
                    rig.bones[j] = {b.name.c_str(), b.parent, b.localPos, b.localQuat, b.localScale, b.invWorld};
                }
                rig.clip = &pv.animClips[static_cast<size_t>(c)];
                rig.trackByName = castlemist::granny::trackIndexByName(*rig.clip);
            }
        }
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
    s.scrollingMaterials = d->scrollingMaterials;
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

void WorldView::set_shading(WorldShading s) { d->shading = s; }
WorldShading WorldView::shading() const { return d->shading; }
void WorldView::set_material_settings(const WorldMaterialSettings& s) { d->mat = s; }
const WorldMaterialSettings& WorldView::material_settings() const { return d->mat; }

void WorldView::set_motion(bool on) { d->motion = on; }
bool WorldView::motion() const { return d->motion; }
void WorldView::set_time(double seconds) { d->clock = seconds; }
double WorldView::time() const { return d->motion ? d->clock : 0.0; }
bool WorldView::animating() const {
    return d->shading == WorldShading::Materials && d->motion &&
           (d->scrollingMaterials > 0 || d->hasWater || d->st.animated > 0);
}

bool WorldView::focus_animated(WorldCamera& cam) const {
    const InstCPU* in = d->nearest_animated(cam.pos);
    if (!in) return false;
    Vec3 f{cam.fwd[0], cam.fwd[1], cam.fwd[2]};
    if (!(dot(f, f) > 1e-12f)) f = Vec3{1, 0, 0};
    f = norm(f);
    const float back = 4.0f * in->radiusW;
    cam.pos[0] = in->centerW[0] - f.x * back;
    cam.pos[1] = in->centerW[1] - f.y * back;
    cam.pos[2] = in->centerW[2] - f.z * back;
    return true;
}

bool WorldView::animated_screen_rect(const WorldCamera& cam, int rect[4]) const {
    const InstCPU* in = d->nearest_animated(cam.pos);
    if (!in || d->w <= 0 || d->h <= 0) return false;
    const Mat4 VP = world_camera_viewproj(cam, static_cast<float>(d->w) / static_cast<float>(std::max(1, d->h)));
    // The 8 corners of the sphere's box (row vectors: clip = p VP). A corner
    // behind the camera makes the rect the whole surface (conservative); all
    // eight behind: none.
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    int behind = 0;
    const float r = in->radiusW;
    for (int k = 0; k < 8; ++k) {
        const float p[3] = {in->centerW[0] + ((k & 1) ? r : -r), in->centerW[1] + ((k & 2) ? r : -r),
                            in->centerW[2] + ((k & 4) ? r : -r)};
        float c[4];
        for (int j = 0; j < 4; ++j) c[j] = p[0] * VP.m[j] + p[1] * VP.m[4 + j] + p[2] * VP.m[8 + j] + VP.m[12 + j];
        if (!(c[3] > 1e-4f)) { ++behind; continue; }
        const float sx = (c[0] / c[3] * 0.5f + 0.5f) * static_cast<float>(d->w);
        const float sy = (0.5f - c[1] / c[3] * 0.5f) * static_cast<float>(d->h);
        x0 = std::min(x0, sx); x1 = std::max(x1, sx);
        y0 = std::min(y0, sy); y1 = std::max(y1, sy);
    }
    if (behind == 8) return false;
    if (behind > 0) { x0 = 0; y0 = 0; x1 = static_cast<float>(d->w); y1 = static_cast<float>(d->h); }
    const int L = std::clamp(static_cast<int>(std::floor(x0)), 0, d->w);
    const int T = std::clamp(static_cast<int>(std::floor(y0)), 0, d->h);
    const int R = std::clamp(static_cast<int>(std::ceil(x1)), 0, d->w);
    const int B = std::clamp(static_cast<int>(std::ceil(y1)), 0, d->h);
    if (R <= L || B <= T) return false;
    rect[0] = L; rect[1] = T; rect[2] = R; rect[3] = B;
    return true;
}

std::vector<std::string> WorldView::warnings() const {
    std::vector<std::string> w = material_setting_warnings(d->mat);
    if (d->waterNoPreset)
        w.push_back("water: no preset for lighting preset " + std::to_string(d->waterPresetIndex) +
                    "; flat water (note \xc2\xa7" "6)");
    if (d->clipMisses > 0)
        w.push_back(std::to_string(d->clipMisses) +
                    " animated placements name no clip of their model; drawn in bind pose (fallback rule UNPROVEN, "
                    "note \xc2\xa7" "5)");
    if (d->skinMisses > 0)
        w.push_back(std::to_string(d->skinMisses) +
                    " animated placements name a clip of a model without a skin stream (more than 256 joints, or "
                    "prepared without skin); drawn in bind pose");
    if (d->stillTransforms > 0)
        w.push_back(std::to_string(d->stillTransforms) +
                    " texTransforms entries are not scroll-only (flipbook, scale or rotate); drawn still (note \xc2\xa7" "4)");
    return w;
}

bool WorldView::capture_chunk_albedo(size_t chunk, int size, std::vector<uint8_t>& rgba) {
    rgba.clear();
    if (!d->gpu_ok() || size <= 0 || size > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
    const ChunkGPU* found = nullptr;
    for (const ChunkGPU& c : d->chunks)
        if (c.src == chunk) { found = &c; break; }
    if (!found || !d->chunk_textured(*found)) return false;
    const ChunkGPU& g = *found;

    // Its own target and staging copy: the swap chain's targets are not touched.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = static_cast<UINT>(size);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> rt, stg;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &rt)) ||
        FAILED(d->dev->CreateRenderTargetView(rt.Get(), nullptr, &rtv)))
        return false;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d->dev->CreateTexture2D(&td, nullptr, &stg))) return false;

    // Orthographic, top down over the chunk's rect (row vectors, translation
    // in the last row): x0 -> NDC -1 (column 0, west), y1 -> NDC +1 (row 0,
    // north). No depth buffer is bound, so nothing is depth tested.
    const float x0 = g.rect[0], y0 = g.rect[1], x1 = g.rect[2], y1 = g.rect[3];
    const float hx = 0.5f * (x1 - x0), hy = 0.5f * (y1 - y0);
    if (!(std::fabs(hx) > 0) || !(std::fabs(hy) > 0)) return false;
    WorldCB c{};
    c.viewProj.m[0] = 1.0f / hx;
    c.viewProj.m[5] = 1.0f / hy;
    c.viewProj.m[12] = -(x0 + hx) / hx;
    c.viewProj.m[13] = -(y0 + hy) / hy;
    c.viewProj.m[14] = 0.5f;
    c.viewProj.m[15] = 1.0f;
    c.exposure = 1.0f;
    d->upload_world_cb(c);   // draw() writes the whole buffer again each frame

    d->ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1};
    d->ctx->RSSetViewports(1, &vp);
    const float clear[4] = {0, 0, 0, 0};
    d->ctx->ClearRenderTargetView(rtv.Get(), clear);
    d->bind_terrain_state();
    d->use_terrain_pipeline(true);
    d->bind_chunk_material(g, true);
    d->draw_chunk(g);
    d->unbind_terrain_textures();
    d->ctx->OMSetRenderTargets(0, nullptr, nullptr);   // draw() binds the swap chain's targets again
    d->ctx->CopyResource(stg.Get(), rt.Get());

    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(d->ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    rgba.resize(static_cast<size_t>(size) * size * 4);
    for (int y = 0; y < size; ++y)
        std::memcpy(rgba.data() + static_cast<size_t>(y) * size * 4,
                    static_cast<const uint8_t*>(ms.pData) + static_cast<size_t>(y) * ms.RowPitch,
                    static_cast<size_t>(size) * 4);
    d->ctx->Unmap(stg.Get(), 0);
    return true;
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
