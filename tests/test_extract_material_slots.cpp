/// @file
/// @brief Tests for how a MODL material's texture slots become the diffuse,
///        the normal and the extra layers (model_preview.cpp).

#include "test_framework.h"

#include "internal.h"

#include <map>
#include <string>
#include <vector>

using castlemist::extract::assign_texture_slots;
using castlemist::extract::layer_only_files;
using castlemist::extract::MaterialTextureSlot;

namespace {

/// fileId -> texture index, as model_preview's texture cache answers it.
std::function<int(uint32_t)> indices(std::map<uint32_t, int> m) {
    return [m](uint32_t fid) {
        auto it = m.find(fid);
        return it == m.end() ? -1 : it->second;
    };
}

}  // namespace

// Character armor: the diffuse slot is empty (fileId 0) and the textures are
// named only as layers; none of them may become the base colour.
CM_TEST(material_slots, files_named_only_as_layers_are_layer_only) {
    const std::vector<MaterialTextureSlot> slots = {
        {0, "diffuse", 0}, {500, "mask", 0}, {501, "decal", 1}, {502, "glowmask", 0}};
    const std::set<uint32_t> only = layer_only_files(slots);
    CHECK_EQ(only.size(), size_t{3});
    CHECK(only.count(500) && only.count(501) && only.count(502));

    ModelMaterialCPU mat;
    mat.diffuseTex = 0;  // the biggest texture, picked by area: really the mask
    assign_texture_slots(mat, slots, only, indices({{500, 0}, {501, 1}, {502, 2}}));
    CHECK_EQ(mat.diffuseTex, -1);
    CHECK_EQ(mat.extraTextures.size(), size_t{3});
}

// AMAT 511663 (Skyforged Hammer 3123167, material 1): one file is both the
// diffuse (UV0) and the glow (UV1); the normal file is also the perturb (UV2).
CM_TEST(material_slots, a_file_with_two_roles_keeps_both) {
    const std::vector<MaterialTextureSlot> slots = {
        {100, "cutout", 0}, {200, "diffuse", 0}, {200, "glow", 1},
        {300, "normal", 0}, {300, "perturb", 2}};
    const std::set<uint32_t> only = layer_only_files(slots);
    CHECK_FALSE(only.count(200));  // also a diffuse: not layer-only

    ModelMaterialCPU mat;
    mat.diffuseTex = 1;
    mat.normalTex = 2;
    assign_texture_slots(mat, slots, only, indices({{100, 0}, {200, 1}, {300, 2}}));
    CHECK_EQ(mat.diffuseTex, 1);
    CHECK_EQ(int{mat.diffuseUv}, 0);
    CHECK_EQ(mat.normalTex, 2);
    CHECK_EQ(int{mat.normalUv}, 0);
    CHECK_EQ(mat.extraTextures.size(), size_t{3});
    if (mat.extraTextures.size() == 3) {
        CHECK(mat.extraTextures[0].role == "cutout");
        CHECK(mat.extraTextures[1].role == "glow");
        CHECK_EQ(int{mat.extraTextures[1].uvIndex}, 1);
        CHECK_EQ(mat.extraTextures[1].texIndex, 1);
        CHECK(mat.extraTextures[2].role == "perturb");
        CHECK_EQ(int{mat.extraTextures[2].uvIndex}, 2);
        CHECK_EQ(mat.extraTextures[2].texIndex, 2);
    }
}

// A layer slot listed before the diffuse slot of the same file does not claim it,
// and a repeat of the claiming role merges instead of becoming an extra.
CM_TEST(material_slots, first_non_layer_slot_claims) {
    const std::vector<MaterialTextureSlot> slots = {
        {200, "glow", 3}, {200, "diffuse", 1}, {200, "diffuse", 1}};
    ModelMaterialCPU mat;
    mat.diffuseTex = 0;
    assign_texture_slots(mat, slots, layer_only_files(slots), indices({{200, 0}}));
    CHECK_EQ(mat.diffuseTex, 0);
    CHECK_EQ(int{mat.diffuseUv}, 1);
    CHECK_EQ(mat.extraTextures.size(), size_t{1});
    if (!mat.extraTextures.empty()) CHECK(mat.extraTextures[0].role == "glow");
}
