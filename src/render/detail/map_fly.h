/// @file
/// @brief The mesh-only fly-through map view: compact geometry, baked AO, instanced draw.
///
/// A second, independent draw path for map scenes. Where `render_scene()` walks
/// every instance and every submesh and issues one constant-buffer update plus
/// one `DrawIndexedInstanced` each -- tens of thousands of draws on a real map --
/// this path collapses a whole map to roughly **one draw call per visible model
/// per LOD**, which is what makes a full map navigable in real time.
///
/// It deliberately drops everything the game's look depends on: no textures, no
/// materials, no game DXBC shaders, no transparency sorting. Shading is a
/// hemisphere ambient plus one directional light modulated by ambient occlusion
/// that is **baked into the vertex at upload time**, so it costs nothing at
/// runtime and survives instancing (AO lives in model space, which every
/// instance of that model shares).
///
/// @warning Not a public header -- internal to `src/render/`. The outside world
///          goes through `castlemist/render/model_renderer.h`.

#pragma once

#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl/client.h>

#include "castlemist/render/model_renderer.h"

#include "detail/math.h"

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

/// @brief One static map vertex: 16 bytes, versus GVertex's 144.
///
/// Nine times smaller than the renderer's general vertex, which matters here
/// because a map is bandwidth-bound long before it is ALU-bound. Everything the
/// shading needs is position, normal and occlusion; UVs, tangents, bitangents
/// and skinning weights are all dead weight for a mesh-only view.
struct MapVtx {
    float px, py, pz;         ///< Model-space position (offset 0).
    /// @name Normal + AO, packed as R8G8B8A8_UNORM at offset 12
    /// @brief `nx/ny/nz` are `normal * 0.5 + 0.5`; `ao` is the baked occlusion.
    /// @{
    uint8_t nx, ny, nz, ao;
    /// @}
};
static_assert(sizeof(MapVtx) == 16, "MapVtx must stay 16 bytes -- the input layout hardcodes the stride");

/// @brief Per-instance stream entry: the world transform, as three float4s.
///
/// These are the **columns** of the row-vector world matrix, so the vertex
/// shader reconstructs a world position with three dot products
/// (`dot(float4(pos,1), r0/r1/r2)`) instead of a full matrix multiply. Uniform
/// scale means the same three rows also rotate the normal, after a normalize.
struct MapInstGPU {
    float r0[4]; ///< (W00, W10, W20, W30) -- x column.
    float r1[4]; ///< (W01, W11, W21, W31) -- y column.
    float r2[4]; ///< (W02, W12, W22, W32) -- z column.
    /// @brief Per-layer albedo tint (rgb; w unused).
    ///
    /// With no textures every surface would otherwise be the same white, and a
    /// collision hull sitting inside terrain would be impossible to pick out.
    /// Carrying it per instance rather than per draw keeps a model that appears
    /// in two layers on one buffer.
    float tint[4];
};
static_assert(sizeof(MapInstGPU) == 64, "MapInstGPU must stay 64 bytes");

/// @brief Albedo per SceneLayer, indexed by LAYER_*.
inline const float kFlyLayerTint[LAYER_COUNT][3] = {
    {0.78f, 0.76f, 0.72f}, // LAYER_PROP      -- warm stone
    {0.52f, 0.56f, 0.46f}, // LAYER_TERRAIN   -- muted green-grey ground
    {0.90f, 0.42f, 0.36f}, // LAYER_COLLISION -- red, deliberately loud
    {0.42f, 0.62f, 0.40f}, // LAYER_ZONE      -- foliage green
};

/// @brief One unique model, flattened for the fly view.
///
/// Every submesh of the model is concatenated into a single vertex and index
/// buffer. That is only possible because this view has no materials: with
/// nothing to rebind between submeshes there is no reason to keep them apart,
/// and the whole model becomes one draw.
struct FlyModelGPU {
    ComPtr<ID3D11Buffer> vb;  ///< All submeshes' MapVtx, concatenated.
    ComPtr<ID3D11Buffer> ib;  ///< All submeshes' indices, concatenated, per LOD.

    /// @brief `[lod]` -> (first index, index count) into @ref ib.
    ///
    /// LOD k merges every submesh's LOD-k index set (falling back to that
    /// submesh's coarsest available LOD when it has fewer levels than the model
    /// as a whole), so picking a LOD stays a single range.
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;

    float center[3] = {0, 0, 0}; ///< Model-space bounding-sphere centre.
    float radius = 1.0f;         ///< Model-space bounding-sphere radius.
    uint32_t vertCount = 0;
    bool ok = false;
};

/// @brief A placed instance, kept CPU-side for culling.
struct FlyInstCPU {
    Mat4 world;                    ///< Row-vector model->world.
    float centerW[3] = {0, 0, 0};  ///< World-space bounding-sphere centre.
    float radiusW = 1.0f;          ///< World-space bounding-sphere radius.
    int model = 0;
    int layer = LAYER_PROP;
};

/// @brief Constant buffer for the fly shader (register b0).
struct FlyCB {
    Mat4 viewProj;
    float sunDir[3];    float exposure;
    float sunCol[3];    float aoStrength;
    float skyCol[3];    float fogStart;
    float groundCol[3]; float fogEnd;
    float fogCol[3];    float _pad0;
    float camPos[3];    float _pad1;
};

// ---------------------------------------------------------------------------
// Fly-view state (C++17 inline variables, same convention as detail/state.h).
// ---------------------------------------------------------------------------

inline std::vector<FlyModelGPU> g_fly_models;
inline std::vector<FlyInstCPU> g_fly_insts;
inline bool g_fly_built = false;      ///< Compact buffers exist for the current scene.

/// @name Pipeline objects, created once in device.cpp
/// @{
inline ComPtr<ID3D11VertexShader> g_flyVS;
inline ComPtr<ID3D11PixelShader> g_flyPS;
inline ComPtr<ID3D11InputLayout> g_flyIL;
inline ComPtr<ID3D11Buffer> g_flyCB;
inline ComPtr<ID3D11Buffer> g_flyInstVB;   ///< Dynamic; regrown as the visible set grows.
inline uint32_t g_flyInstCap = 0;          ///< Instances the buffer can currently hold.
inline bool g_fly_pipeline_ok = false;
/// @}

/// @name Free camera (Z-up world, but the camera itself is unconstrained)
///
/// Stored as an explicit orthonormal basis rather than yaw/pitch Euler angles.
/// Euler angles force a clamp just short of vertical -- look straight up with a
/// fixed world up-vector and the look-at basis degenerates and the view snaps
/// over. Carrying the basis and rotating it about its OWN axes removes the pole
/// entirely: you can pitch past vertical, roll, and fly inverted, and nothing
/// flips. The cost is that roll accumulates, which is what "free" means; the
/// Reset button re-levels it.
/// @{
inline Vec3 g_fly_pos{0, 0, 0};
inline Vec3 g_fly_fwd{1, 0, 0};   ///< Unit forward.
inline Vec3 g_fly_up{0, 0, 1};    ///< Unit up, kept perpendicular to forward.
inline float g_fly_speed = 1000.0f;   ///< World units/second, auto-scaled on load.
inline bool g_fly_keys[8] = {};       ///< W A S D Q E Shift Ctrl -- see FlyKey.
/// @}

/// @name Tunables surfaced in the UI
/// @{
inline float g_fly_view_dist = 40000.0f; ///< Hard distance cull, world units.
inline float g_fly_ao_strength = 1.0f;   ///< 0 = flat, 1 = full baked AO.
inline bool g_fly_fog = true;
inline bool g_fly_wire = false;
/// @}

/// @name Per-frame stats, drawn in the HUD
/// @{
inline int g_fly_stat_visible = 0;   ///< Instances that survived culling.
inline int g_fly_stat_draws = 0;     ///< DrawIndexedInstanced calls issued.
inline int g_fly_stat_tris = 0;      ///< Triangles submitted (thousands).
inline float g_fly_stat_ms = 0.0f;   ///< Wall time of the last frame's cull+draw.
/// @}

/// @brief Which slot of @ref g_fly_keys a key maps to.
enum FlyKey { FLY_FWD = 0, FLY_LEFT, FLY_BACK, FLY_RIGHT, FLY_DOWN, FLY_UP, FLY_FAST, FLY_SLOW };

/// @brief Flattens + AO-bakes `models` into @ref g_fly_models starting at `base`.
///
/// The heavy half (flatten, voxelize, ray-march) runs across every core and
/// touches no D3D; only buffer creation is serial. Called from set_scene /
/// add_scene_models alongside the existing upload.
void fly_add_models(const std::vector<ModelPreview>& models, size_t base);
/// @brief Records one placement. `radius` is the instance's world-space radius.
void fly_add_instance(int model, const Mat4& world, float radius, int layer);
/// @brief Releases the compact buffers (called from clear_scene).
void clear_fly_scene();
/// @brief Culls, uploads the instance stream and draws. Assumes RT/depth bound.
void render_fly();
/// @brief Places the camera to view the whole scene, and picks a sane speed.
void fly_frame_scene();

} // namespace castlemist::render
