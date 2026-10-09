/// @file
/// @brief Map space and its conversions to Unity and Blender.
///
/// Map space is GW2 map space as stored, WorldScene's only frame:
/// left-handed, +X east, +Y north, +Z down (up = -Z). Proof in
/// docs/research/gw2-world-frame.md §1.
///
/// Units per metre are UNPROVEN (§1.3), so there is no kMapUnitsPerMetre and
/// these conversions change axes only: the output stays in map units.
/// @ingroup world
#pragma once

namespace castlemist::world {

/// @brief Map space -> Unity (left-handed, +Y up, +Z forward = north):
///        (x, y, z) -> (x, -z, y). A proper rotation (det +1).
///        Works for positions and directions; @p in and @p out may alias.
void map_to_unity(const float in[3], float out[3]);

/// @brief Map space -> Blender (right-handed, +Z up, +Y north):
///        (x, y, z) -> (x, y, -z). A reflection (det -1): the handedness change.
///        Works for positions and directions; @p in and @p out may alias.
void map_to_blender(const float in[3], float out[3]);

/// @brief A column-major map-space matrix M (p' = M p) as the matching Unity
///        matrix C M C^-1, so that
///        map_matrix_to_unity(M) * map_to_unity(p) == map_to_unity(M * p).
///        @p in and @p out may alias.
void map_matrix_to_unity(const float in[16], float out[16]);

/// @brief A column-major map-space matrix M as the matching Blender matrix
///        C M C^-1 (C = diag(1, 1, -1)). The determinant is kept: a map-space
///        rotation stays a proper rotation. @p in and @p out may alias.
void map_matrix_to_blender(const float in[16], float out[16]);

} // namespace castlemist::world
