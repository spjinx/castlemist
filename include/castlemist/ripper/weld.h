/// @file
/// @brief Sealing a character's mesh seams: one normal where separate meshes meet (ripper output).

#ifndef CASTLEMIST_RIPPER_WELD_H
#define CASTLEMIST_RIPPER_WELD_H

#include "castlemist/extract/model_types.h"

#include <cstddef>
#include <vector>

namespace castlemist::ripper {

/// Every vertex of `meshes` (indices into m.meshes) takes the average of the normals at its position (within
/// `eps` per axis) that lie within `max_angle_deg` of its own -- across meshes too. GW2 builds a head from
/// separate meshes (face, ears, scalp, the coat's neck), each with its own normals along the shared edge, so
/// the light broke there (the Mad Prince's jaw and ears, 2026-11-02). Creases sharper than the angle stay.
/// Returns how many vertices changed.
size_t weld_normals(ModelPreview& m, const std::vector<size_t>& meshes, float eps = 0.01f, float max_angle_deg = 60.0f);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_WELD_H
