/// @file
/// @brief ModelMaterialCPU -> glTF PBR metallic-roughness material.
///
/// glTF's material fields (baseColorTexture, normalTexture, emissiveTexture,
/// alphaMode, alphaCutoff) are native, well-specified concepts every tool in
/// the glTF -> Blender -> FBX -> Unity chain already carries through via
/// shared property names (`_MainTex`, `_BumpMap`, `_EmissionMap`) -- so a
/// material dropped into Unity and switched to Poiyomi picks these up with no
/// companion script and no manual texture-dragging.

#include "internal.h"

namespace castlemist::exportgltf {

using nlohmann::json;

std::vector<int> write_materials(GltfWriter& w, const ModelPreview& model, const std::vector<int>& texIndices) {
    std::vector<int> out;
    out.reserve(model.materials.size());

    for (const ModelMaterialCPU& mat : model.materials) {
        json pbr{
            {"baseColorFactor", {mat.tint[0], mat.tint[1], mat.tint[2], mat.tint[3]}},
            {"metallicFactor", 0.0},
            // kind 2 == water in castlemist's ModelMaterialCPU: nudge toward a
            // glossier look rather than leaving it fully matte like painted stone.
            {"roughnessFactor", mat.kind == 2 ? 0.1 : 1.0},
        };
        if (mat.diffuseTex >= 0 && mat.diffuseTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.diffuseTex)] >= 0) {
            pbr["baseColorTexture"] = {{"index", texIndices[static_cast<size_t>(mat.diffuseTex)]}};
        }

        json material{{"name", "Mat_" + std::to_string(mat.index)}, {"pbrMetallicRoughness", pbr}};

        if (mat.normalTex >= 0 && mat.normalTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.normalTex)] >= 0) {
            material["normalTexture"] = {{"index", texIndices[static_cast<size_t>(mat.normalTex)]}};
        }

        bool hasCutout = mat.diffuseTex >= 0 && mat.diffuseTex < static_cast<int>(model.textures.size()) &&
                        model.textures[static_cast<size_t>(mat.diffuseTex)].hasCutout;
        if (hasCutout) {
            material["alphaMode"] = "MASK";
            material["alphaCutoff"] = 0.25; // matches castlemist's own cutout threshold
        } else if (mat.isEffect) {
            material["alphaMode"] = "BLEND";
        }
        material["doubleSided"] = mat.isEffect; // effect quads (foliage/particle-like) are commonly single-sided planes

        // Additive/glow effect materials get a real emissive texture baked
        // from the diffuse texture's own bright regions -- see
        // bake_effect_emissive_texture()'s comment for why this (not a
        // Blender node script) is what actually survives into Poiyomi.
        if (mat.isEffect && mat.diffuseTex >= 0 && mat.diffuseTex < static_cast<int>(model.textures.size())) {
            int emissiveTexIdx = bake_effect_emissive_texture(w, model.textures[static_cast<size_t>(mat.diffuseTex)]);
            if (emissiveTexIdx >= 0) {
                material["emissiveTexture"] = {{"index", emissiveTexIdx}};
                material["emissiveFactor"] = {1.0, 1.0, 1.0};
            }
        }

        out.push_back(w.add_material(std::move(material)));
    }
    return out;
}

} // namespace castlemist::exportgltf
