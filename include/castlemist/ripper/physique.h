#ifndef CASTLEMIST_RIPPER_PHYSIQUE_H
#define CASTLEMIST_RIPPER_PHYSIQUE_H

// Physiques (the character creator's body types) on a rigged character.
//
// A race's Composite lists them as bone-scale presets (bodyBoneScales): groups
// (chest, hips, shoulders, legs, ...) with a slider weight, each moving named
// bones -- per bone 9 deltas in its own frame at full weight: rotation in
// degrees (x y z), scale (x y z, added to 1), offset (x y z). A flag bit (2)
// applies a left bone's deltas to its right twin too.

#include <cstddef>

#include "castlemist/extract/model_types.h"
#include "castlemist/format/composite.h"

namespace castlemist::ripper {

/// Bakes `preset` into `model`: each bone's scale, rotation and offset are
/// applied to the vertices it skins, rotation and offset carrying down to its
/// child bones (scale does not), and the joints' bind poses follow, so the rig
/// stays consistent. Returns how many joints it moved.
size_t apply_physique(ModelPreview& model, const composite::BoneScalePreset& preset);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_PHYSIQUE_H
