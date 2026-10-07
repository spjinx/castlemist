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

#include "castlemist/exportgltf/vrchat_maps.h"

#include <algorithm>

namespace castlemist::exportgltf {

using nlohmann::json;

namespace {

json glow_colour_json(const ModelPreview& model, int texIndex) {
    const std::array<double, 3> c = glow_colour(model, texIndex);
    return {c[0], c[1], c[2]};
}

} // namespace

// Same priority spjinx/t3d's own glTF exporter uses (RenderUtils.ts:
// `finalMaterial.name = rawMesh.materialName || (mat ? String(mat.filename)
// : finalMaterial.name)`): the artist-authored materialName first (real GW2
// material names, e.g. "MetalBladeMat" -- verified against 1768614), falling
// back to materialFile (the .amat/GRMT fileId, a stable unique-per-material
// identifier) when a mesh didn't name it, and only then the local,
// model-specific `index` (meaningless outside the file it came from) when
// even materialFile is 0.
std::string material_name(const ModelMaterialCPU& mat) {
    return !mat.materialName.empty() ? mat.materialName
           : mat.materialFile != 0   ? "Mat_" + std::to_string(mat.materialFile)
                                     : "Mat_" + std::to_string(mat.index);
}

MaterialShading material_shading(const ModelMaterialCPU& mat) {
    MaterialShading s;
    s.profile = &profile_for(mat, mat.renderState);
    s.blend = decode_blend(mat.renderState, mat.hasRenderState, mat.isEffect, s.profile->clips);
    return s;
}

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
        if (mat.metalRoughTex >= 0 && mat.metalRoughTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.metalRoughTex)] >= 0) {
            pbr["metallicRoughnessTexture"] = {{"index", texIndices[static_cast<size_t>(mat.metalRoughTex)]}};
            pbr["metallicFactor"] = 1.0;
            pbr["roughnessFactor"] = 1.0;
        }

        const std::string name = material_name(mat);
        const MaterialShading shading = material_shading(mat);
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
        auto texRef = [&](const ModelMaterialCPU::ExtraTexture& ex) -> json {
            if (ex.texIndex < 0 || ex.texIndex >= static_cast<int>(texIndices.size()) ||
                texIndices[static_cast<size_t>(ex.texIndex)] < 0)
                return json();
            json ref{{"index", texIndices[static_cast<size_t>(ex.texIndex)]}};
            if (ex.uvIndex != 0) ref["texCoord"] = ex.uvIndex;
            return ref;
        };
        // GW2 draws a material's glow as its "glow" texture (a tiling colour
        // pattern, e.g. the Forged Dagger's fire) times its "glowmask" (where
        // that shows). glTF has one emissive texture, so the mask is the map
        // and the glow's average colour its factor; a glow alone is the map.
        // The occlusion slot below takes the first other layer.
        const ModelMaterialCPU::ExtraTexture* glowLayer = nullptr;
        const ModelMaterialCPU::ExtraTexture* glowMask = nullptr;
        for (const auto& ex : mat.extraTextures) {
            if (ex.role == "glow" && !glowLayer) glowLayer = &ex;
            if (ex.role == "glowmask" && !glowMask) glowMask = &ex;
        }
        const ModelMaterialCPU::ExtraTexture* occLayer = nullptr;
        for (const auto& ex : mat.extraTextures)
            if (ex.role.rfind("glow", 0) != 0) { occLayer = &ex; break; }
        if (occLayer) {
            const auto& ex0 = *occLayer;
            if (ex0.texIndex >= 0 && ex0.texIndex < static_cast<int>(texIndices.size()) &&
                texIndices[static_cast<size_t>(ex0.texIndex)] >= 0) {
                json occTex{{"index", texIndices[static_cast<size_t>(ex0.texIndex)]}};
                if (ex0.uvIndex != 0) occTex["texCoord"] = ex0.uvIndex;
                material["occlusionTexture"] = std::move(occTex);
            }
        }

        // The decoded blend and the shader profile decide, not the texture: a
        // dark shine map reads as holes to ModelTextureCPU::hasCutout, and
        // only clipping shaders discard (spec section 1). Without a game
        // shader (character pieces, map props, baked atlases) the diffuse
        // alpha is real coverage -- hair cards, foliage -- so hasCutout still
        // clips there, as it did before the profiles.
        GltfAlphaMode alphaMode = gltf_alpha_mode(shading.blend);
        const bool hasCutout = mat.diffuseTex >= 0 &&
                               mat.diffuseTex < static_cast<int>(model.textures.size()) &&
                               model.textures[static_cast<size_t>(mat.diffuseTex)].hasCutout;
        if (!mat.hasRenderState)  // the previous order exactly: cutout first, then effect
            alphaMode = hasCutout     ? GltfAlphaMode::Mask
                        : mat.isEffect ? GltfAlphaMode::Blend
                                       : GltfAlphaMode::Opaque;
        switch (alphaMode) {
            case GltfAlphaMode::Opaque: material["alphaMode"] = "OPAQUE"; break;
            case GltfAlphaMode::Mask:
                material["alphaMode"] = "MASK";
                material["alphaCutoff"] = kAlphaCutoff;
                break;
            case GltfAlphaMode::Blend: material["alphaMode"] = "BLEND"; break;
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

        json maskRef = glowMask ? texRef(*glowMask) : json();
        if (!maskRef.is_null()) {
            material["emissiveTexture"] = std::move(maskRef);
            material["emissiveFactor"] = glowLayer ? glow_colour_json(model, glowLayer->texIndex) : json{1.0, 1.0, 1.0};
        } else if (glowLayer) {
            json ref = texRef(*glowLayer);
            if (!ref.is_null()) {
                material["emissiveTexture"] = std::move(ref);
                material["emissiveFactor"] = {1.0, 1.0, 1.0};
            }
        }

        // A baked glow (e.g. a sylvari's pattern glow) rides on its own texture.
        if (mat.emissiveTex >= 0 && mat.emissiveTex < static_cast<int>(texIndices.size()) &&
            texIndices[static_cast<size_t>(mat.emissiveTex)] >= 0) {
            material["emissiveTexture"] = {{"index", texIndices[static_cast<size_t>(mat.emissiveTex)]}};
            material["emissiveFactor"] = {1.0, 1.0, 1.0};
        }

        // The Poiyomi packed map (R metal, G smooth) as glTF's metallicRoughness
        // (B metal, G roughness = 1 - smooth). A ready-made metalRoughTex wins.
        // Only for game-shader materials: without one (character pieces, map
        // props, baked atlases) the diffuse alpha is coverage, not shine.
        // Added after the emissive bake so earlier image indices stay put.
        if (mat.hasRenderState && !pbr.contains("metallicRoughnessTexture")) {
            const MaterialMaps maps = build_material_maps(model, mat, shading.blend, *shading.profile);
            if (maps.packed.present) {
                ModelTextureCPU mr = maps.packed.tex;
                for (size_t i = 0; i + 3 < mr.rgba.size(); i += 4) {
                    const uint8_t metal = maps.packed.tex.rgba[i];
                    const uint8_t smooth = maps.packed.tex.rgba[i + 1];
                    mr.rgba[i] = 255;
                    mr.rgba[i + 1] = static_cast<uint8_t>(255 - smooth);
                    mr.rgba[i + 2] = metal;
                    mr.rgba[i + 3] = 255;
                }
                const std::vector<uint8_t> png = encode_png(mr);
                if (!png.empty()) {
                    // fileId 0: generated per material, never shared.
                    json ref{{"index", w.add_or_reuse_texture(0, png, name + "_metalRough")}};
                    if (maps.packed.uv != 0) ref["texCoord"] = maps.packed.uv;
                    material["pbrMetallicRoughness"]["metallicRoughnessTexture"] = std::move(ref);
                    material["pbrMetallicRoughness"]["metallicFactor"] = 1.0;
                    material["pbrMetallicRoughness"]["roughnessFactor"] = 1.0;
                }
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
            if (!ex.role.empty()) extras["gw2_extraTex" + std::to_string(i) + "_role"] = ex.role;
        }
        if (!extras.empty()) material["extras"] = std::move(extras);

        out.push_back(w.add_material(std::move(material)));
    }
    return out;
}

} // namespace castlemist::exportgltf
