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
            // Real per-material values when the MODL declared them (see
            // model_preview.cpp's `mtlness`/`specstr` decoding) -- 0.0
            // metallic is still the fallback (right for the large majority of
            // GW2 materials with no metalness constant at all).
            {"metallicFactor", mat.metallic},
            // kind 2 == water in castlemist's ModelMaterialCPU: nudge toward a
            // glossier look rather than leaving it fully matte like painted
            // stone. Only applied when the material didn't already give us a
            // real (if approximate) roughness of its own.
            {"roughnessFactor", mat.roughness >= 0.0f ? mat.roughness : (mat.kind == 2 ? 0.1 : 1.0)},
        };
        if (mat.diffuseTex >= 0 && mat.diffuseTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.diffuseTex)] >= 0) {
            json baseColorTex{{"index", texIndices[static_cast<size_t>(mat.diffuseTex)]}};
            // texCoord defaults to 0 in the glTF spec, so it's only worth
            // writing when this texture actually samples a different UV set
            // (see ModelMaterialCPU::diffuseUv) -- an accessor for that set
            // must exist too; write_meshes() emits it for exactly this case.
            if (mat.diffuseUv != 0) baseColorTex["texCoord"] = mat.diffuseUv;
            pbr["baseColorTexture"] = std::move(baseColorTex);
        }

        // Same priority spjinx/t3d's own glTF exporter uses (RenderUtils.ts:
        // `finalMaterial.name = rawMesh.materialName || (mat ? String(mat.filename)
        // : finalMaterial.name)`): the artist-authored materialName first
        // (real GW2 material names, e.g. "MetalBladeMat" -- verified against
        // 1768614), falling back to materialFile (the .amat/GRMT fileId, a
        // real stable unique-per-material identifier) when a mesh didn't name
        // it, and only then the local, model-specific `index` (meaningless
        // outside the file it came from; two different models' "material 0"
        // are unrelated) when even materialFile is 0.
        std::string name = !mat.materialName.empty() ? mat.materialName
                          : mat.materialFile != 0     ? "Mat_" + std::to_string(mat.materialFile)
                                                       : "Mat_" + std::to_string(mat.index);
        json material{{"name", name}, {"pbrMetallicRoughness", pbr}};

        if (mat.normalTex >= 0 && mat.normalTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.normalTex)] >= 0) {
            json normalTex{{"index", texIndices[static_cast<size_t>(mat.normalTex)]}};
            if (mat.normalUv != 0) normalTex["texCoord"] = mat.normalUv;
            material["normalTexture"] = std::move(normalTex);
        }

        // Decal/detail/mask textures the material references beyond
        // diffuse/normal (see ModelMaterialCPU::extraTextures's own doc
        // comment) -- castlemist can't reconstruct whatever blend the real
        // shader applies, so the first one is wired into occlusionTexture. A
        // repurposed slot, not real ambient occlusion, but a REAL, UV-correct
        // glTF texture reference: Blender's importer creates an actual
        // texture node for it, wired to the right UV map, instead of leaving
        // the image an unreachable orphan datablock. Anything beyond the
        // first is still embedded in the .glb regardless (write_model_textures()
        // doesn't care whether any material references a texture), just not
        // pre-wired to anything -- listed in extras below so its real
        // fileId/UV set are at least visible for manual wiring.
        if (!mat.extraTextures.empty()) {
            const auto& ex0 = mat.extraTextures[0];
            if (ex0.texIndex >= 0 && ex0.texIndex < static_cast<int>(texIndices.size()) &&
                texIndices[static_cast<size_t>(ex0.texIndex)] >= 0) {
                json occTex{{"index", texIndices[static_cast<size_t>(ex0.texIndex)]}};
                if (ex0.uvIndex != 0) occTex["texCoord"] = ex0.uvIndex;
                material["occlusionTexture"] = std::move(occTex);
            }
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

        // Every named MODL material constant this codebase doesn't already
        // map to a real glTF property (glow, sss, scroll speed, the raw
        // specpwr/specstr behind the roughness approximation above, ...),
        // preserved as custom properties rather than silently dropped. Shows
        // up in Blender's Object/Material "Custom Properties" panel.
        json extras = json::object();
        for (const auto& [name, value] : mat.namedConstants) extras["gw2_" + name] = value;
        for (size_t i = 0; i < mat.extraTextures.size(); ++i) {
            const auto& ex = mat.extraTextures[i];
            extras["gw2_extraTex" + std::to_string(i) + "_fileId"] = ex.fileId;
            extras["gw2_extraTex" + std::to_string(i) + "_uvIndex"] = ex.uvIndex;
        }
        if (!extras.empty()) material["extras"] = std::move(extras);

        out.push_back(w.add_material(std::move(material)));
    }
    return out;
}

} // namespace castlemist::exportgltf
