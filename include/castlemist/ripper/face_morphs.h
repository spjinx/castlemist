#ifndef CASTLEMIST_RIPPER_FACE_MORPHS_H
#define CASTLEMIST_RIPPER_FACE_MORPHS_H

// Face-detail blend shapes, modelled on the Total Makeover Kit's sliders (head
// shape, mouth, nose, eyes). GW2 drives these sliders through its face rig
// (bone:Jaw, bone:Chin, bone:Nose, cheek / lip / brow / eyelid bones), but the
// per-slider bone data lives in the client, not the dat -- so each slider here
// is an authored bone deformation (scale / rotate / move about the bone's own
// pivot, carried down the hierarchy), baked through the skin weights into
// per-vertex offsets. Every slider becomes two shape keys, "<name>+" and
// "<name>-" (Unity / VRChat blendshapes only run 0..100), and a slider value
// (0..1, 0.5 = the middle tick) sets their default weights.

#include <map>
#include <string>
#include <vector>

#include "castlemist/extract/model_types.h"

namespace castlemist::ripper {

/// The slider names, in makeover-kit order (head shape, mouth, nose, eyes).
const std::vector<std::string>& face_slider_names();

/// Adds "<slider>+" / "<slider>-" morph targets to every mesh of `model` that
/// any slider moves (vertices skinned to the face rig: face, ears, hair,
/// scalp, a helm...). `values`: slider -> 0..1 (0.5 neutral; missing = 0.5)
/// sets the default weights. Needs the race skeleton's bind pose (joints by
/// name); returns the number of meshes that received targets.
size_t add_face_morphs(ModelPreview& model, const std::map<std::string, float>& values = {});

/// VRChat's face keys -- "Blink", "Blink_L", "Blink_R" and the visemes
/// "vrc.v_aa" .. "vrc.v_th" -- from the same face rig (jaw opening, mouth
/// corners, lips, eyelids), appended to the meshes they move. Approximate:
/// the game has no visemes. Returns the number of meshes that received keys.
size_t add_vrchat_face_keys(ModelPreview& model);
const std::vector<std::string>& vrchat_face_key_names();

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_FACE_MORPHS_H
