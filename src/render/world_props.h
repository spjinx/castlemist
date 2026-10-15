/// @file
/// @brief CPU-side preparation of WorldView's prop models (no Direct3D).
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "castlemist/extract/model_types.h"
#include "castlemist/world/world_scene.h"

#include "detail/math.h"
#include "detail/mesh_bake.h"

namespace castlemist::render {

/// @brief One mesh ready to upload: a prop model (prepare_world_model:
///        `matVerts`, `subRanges`, optionally `skinVerts`; `verts` empty) or
///        a collision mesh / the water quad (`verts` only).
///
/// Built off the UI thread (prepare_world_model_shared, world_prep.h) and
/// read-only afterwards.
struct PreparedModel {
    std::vector<MapVtx> verts;       ///< collision and water; empty for a prop model
    std::vector<MatVtx> matVerts;    ///< prop models: the flattened vertices with UVs and baked AO
    std::vector<SkinVtx> skinVerts;  ///< parallel to matVerts; only when prepared withSkin
    std::vector<std::vector<SubRange>> subRanges;   ///< per LOD: one range per non-empty submesh
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;   ///< (first index, count) per LOD
    float center[3] = {0, 0, 0};   ///< model-space bounding-sphere centre
    float radius = 0;              ///< model-space bounding-sphere radius
    bool ok = false;
};

/// @brief Flattens every submesh (mesh_bake::flatten, with the per-LOD
///        submesh ranges), bakes AO, then copies the vertices to `matVerts`
///        in the same order with UV0 (GVertex u, v) and UV1 (uv1[0]) and
///        clears `verts`.
///
/// With @p withSkin, `skinVerts` gets each vertex's `bidx` and
/// round(`bwt` * 255); a mesh without `hasSkin` gets weights {0,0,0,0}
/// (identity), and a model with more than 256 joints gets no `skinVerts`.
///
/// `ok` is false -- and nothing is uploaded or drawn for the model -- when it
/// has no meshes, no triangles at LOD 0, or a non-finite centre or radius. A
/// radius <= 0 is replaced by the farthest vertex from the centre.
PreparedModel prepare_world_model(const ModelPreview& m, bool withSkin = false);

/// @brief One collision mesh as a single-LOD MapVtx mesh: per-vertex normals
///        averaged from the stored winding's face normals (area-weighted),
///        AO 255 (not baked), triangles with an out-of-range index dropped.
///        The sphere is the vertex AABB's centre and the farthest vertex.
///        `ok` is false with no valid triangle or a non-finite vertex.
PreparedModel prepare_collision_mesh(const castlemist::world::CollisionMesh& m);

/// @brief A world/PropInstance/CollisionInstance matrix (column-major,
///        p' = M p, translation in w[12..14]) as the renderer's row-vector
///        Mat4 (p' = p R, translation in m[12..14]).
///
/// R = M^T, and the transpose of a column-major matrix stored as rows is the
/// same 16 floats in the same order: R.m[i*4+j] = M(j,i) = w[i*4+j]. So this
/// is a copy, not a shuffle -- the test pins it with a translation and a turn.
Mat4 colmajor_to_rowvec(const float w[16]);

/// @brief Largest column length of the upper 3x3 of a column-major matrix
///        (w[0..2], w[4..6], w[8..10]): the factor a bounding sphere grows by.
float max_column_length(const float w[16]);

/// @brief Nearest hit of the ray @p o + t @p d with @p m's LOD0 triangles
///        (every mesh's `indices`) placed by @p worldColMajor (column-major,
///        p' = M p, as PropInstance::world).
///
/// The ray goes into model space through the inverse matrix, unnormalized,
/// so t is the same parameter on both sides: in world units when @p d is
/// unit. Triangles are two-sided. Returns t >= 0, or -1 for no hit, a
/// singular or non-finite matrix, or an empty model.
float ray_model_hit(const ModelPreview& m, const float worldColMajor[16], const float o[3], const float d[3]);

} // namespace castlemist::render
