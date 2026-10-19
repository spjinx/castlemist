/// @file
/// @brief CPU-side mesh preparation shared by the fly view and WorldView:
///        the compact 16-byte vertex, the baked AO, the submesh flattener,
///        a parallel-for and the frustum-plane extraction.
///
/// Nothing here touches Direct3D, so both draw paths (and tests) use one copy.
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "castlemist/extract/model_types.h"

#include "detail/math.h"

namespace castlemist::render {

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

/// @brief One submesh's indices inside one LOD range of a flattened model.
struct SubRange {
    uint32_t first = 0, count = 0;   ///< into the flattened index buffer
    uint32_t material = 0;           ///< ModelMeshCPU::materialIndex
};

/// @brief One material-pass vertex: MapVtx's 16 bytes (same offsets, so the
///        same input layout reads it) plus UV0 and UV1. 32 bytes.
struct MatVtx {
    float px, py, pz;         ///< offset 0
    uint8_t nx, ny, nz, ao;   ///< offset 12, as MapVtx
    float u0, v0;             ///< offset 16: GVertex u, v
    float u1, v1;             ///< offset 24: GVertex uv1[0]
};
static_assert(sizeof(MatVtx) == 32, "MatVtx is 32 bytes -- the material input layout hardcodes the stride");

/// @brief Skinning for one vertex (animated props only), a second stream
///        parallel to MatVtx. `weight` is round(bwt * 255); all zero = identity.
struct SkinVtx {
    uint8_t bone[4];
    uint8_t weight[4];
};
static_assert(sizeof(SkinVtx) == 8, "SkinVtx is 8 bytes");

/// @brief Encodes one normal component (-1..1) as `c * 0.5 + 0.5` in a byte.
uint8_t enc_n(float c);

/// @brief Marches a hemisphere per vertex through a voxelization of the mesh
///        and writes the occlusion byte into each vertex's `ao`.
void bake_ao(std::vector<MapVtx>& v, const std::vector<uint32_t>& idx);

/// @brief Flattens every submesh of a ModelPreview into one vertex/index pair.
///
/// LOD k merges each submesh's LOD k, falling back to that submesh's coarsest
/// level when it has fewer than the model as a whole. `lodRanges[k]` is
/// (first index, index count) into @p indices.
void flatten(const ModelPreview& mp, std::vector<MapVtx>& verts, std::vector<uint32_t>& indices,
             std::vector<std::pair<uint32_t, uint32_t>>& lodRanges);

/// @brief As above, and when @p subRanges is non-null also appends, per LOD,
///        each mesh's appended indices in mesh order (empty ones skipped):
///        consecutive, starting at that LOD's first index, summing to its count.
void flatten(const ModelPreview& mp, std::vector<MapVtx>& verts, std::vector<uint32_t>& indices,
             std::vector<std::pair<uint32_t, uint32_t>>& lodRanges, std::vector<std::vector<SubRange>>* subRanges);

/// @brief Six frustum planes (xyz = normal, w = d; normalized, inside >= 0)
///        from a row-vector view-proj with D3D's 0..1 depth range.
void frustum_planes(const Mat4& vp, float pl[6][4]);

/// @brief Runs `fn(i)` for i in [0,count) across the machine's cores.
template <typename F>
void parallel_for(size_t count, F fn) {
    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = std::min<size_t>(count, hw ? hw : 4);
    if (workers <= 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (size_t i = next++; i < count; i = next++) fn(i);
        });
    }
    for (auto& t : pool) t.join();
}

} // namespace castlemist::render
