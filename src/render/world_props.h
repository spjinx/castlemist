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

/// @brief One model flattened to MapVtx with its AO baked, ready to upload.
struct PreparedModel {
    std::vector<MapVtx> verts;
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;   ///< (first index, count) per LOD
    float center[3] = {0, 0, 0};   ///< model-space bounding-sphere centre
    float radius = 0;              ///< model-space bounding-sphere radius
    bool ok = false;
};

/// @brief Flattens every submesh (mesh_bake::flatten) and bakes AO.
///
/// `ok` is false -- and nothing is uploaded or drawn for the model -- when it
/// has no meshes, no triangles at LOD 0, or a non-finite centre or radius. A
/// radius <= 0 is replaced by the farthest vertex from the centre.
PreparedModel prepare_world_model(const ModelPreview& m);

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
