#ifndef CASTLEMIST_RIPPER_SKELETON_MERGE_H
#define CASTLEMIST_RIPPER_SKELETON_MERGE_H

// Building one rigged character out of separately extracted models: armor,
// body parts and back items share the race skeleton by joint NAME (their own
// joint numbering differs), and weapons are moved onto a holster/hand joint.
// Joint matrices follow ModelJoint: row vectors, invWorld = model -> bone.

#include <array>
#include <cstddef>
#include <string>

#include "castlemist/extract/model_types.h"

namespace castlemist::ripper {

/// Appends `src`'s meshes, materials and textures to `dst`, re-pointing every
/// vertex's bone indices at `dst`'s joints by name. Joints `dst` lacks (cape
/// bones) are appended, parented to the dst joint named like their src parent.
/// An empty `dst` simply takes `src`'s skeleton. Returns the index of the
/// first appended mesh.
size_t merge_into(ModelPreview& dst, const ModelPreview& src);

/// Places `weapon` so its `weapon_joint` sits on `dst`'s `body_joint` (bind
/// pose) and appends its geometry rigidly skinned to that body joint; the
/// weapon's own joints are not added. False (nothing appended) if either
/// joint is missing.
bool attach_rigid(ModelPreview& dst, const ModelPreview& weapon, const std::string& weapon_joint,
                  const std::string& body_joint);

/// Places a self-rigged attachment (a back item with its own bones, hung from
/// the shared root) so its `src_joint` sits on `dst`'s `body_joint`: geometry
/// and src's own joints move there, src's root-level joints are re-parented to
/// `body_joint`, and joints `dst` already has (the root) collapse onto it. Its
/// bones stay animatable. False (nothing appended) if either joint is missing.
bool attach_skinned(ModelPreview& dst, const ModelPreview& src, const std::string& src_joint,
                    const std::string& body_joint);

/// A joint's bind local transform (x y z, quat x y z w) recomputed from the
/// invWorld matrices -- what attach_skinned writes for re-parented joints.
std::array<float, 7> local_from_bind(const ModelPreview& m, size_t joint);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_SKELETON_MERGE_H
