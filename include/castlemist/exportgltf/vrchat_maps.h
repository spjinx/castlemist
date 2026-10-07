/// @file
/// @brief Poiyomi-ready texture maps for one model material, built in memory
///        from its decoded layers, blend and shader profile.
///
/// The rules are the "3. Map building" table of
/// docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md; the
/// per-profile channel meanings come from docs/research/gw2-material-channels.md
/// section 4. Nothing here touches the disk: the VRChat export writes the maps.
/// @ingroup exportgltf

#pragma once

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/shader_profiles.h"
#include "castlemist/extract/model_types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace castlemist::exportgltf {

/// One output map. `present == false` means it was not built (no source, a
/// layer that failed to decode, or a 4x4 placeholder that stands for a constant).
struct MapSlot {
    ModelTextureCPU tex;     ///< RGBA8 pixels (`tex.fileId` is the source's fileId).
    uint8_t uv = 0;          ///< UV set the map samples.
    uint32_t fileId = 0;     ///< Source dat fileId (0 when built from several sources).
    std::string source;      ///< What it was built from: "diffuse", "glow", "mask.A", ...
    bool present = false;
};

/// Every map and recorded value of one material.
struct MaterialMaps {
    MapSlot baseColor, normal, packed, emissionMap, emissionMask, emissionBaked, distortion;
    /// A layer kept raw, by role, with a hint of how the game uses it ("detail-multiply2x", ...).
    struct Extra { std::string role, use; MapSlot slot; };
    std::vector<Extra> extras;
    /// Where each packed channel came from, e.g. "mask.R", "diffuseAlpha",
    /// "specular.A", "conduct", "mtlness", "specstr", "envcr", "none".
    std::string metalSource, smoothSource, reflectionSource, specularSource;
    std::array<float, 3> emissionColor = {1, 1, 1};  ///< EmissionMap's average colour, peak 1.
    std::optional<std::array<float, 3>> specularTint, reflectionTint;  ///< speccp / envcr|envcp RGB
    std::vector<std::string> warnings;
};

/// Build the maps of `mat` (one of `model.materials`) for Poiyomi.
MaterialMaps build_material_maps(const ModelPreview& model, const ModelMaterialCPU& mat,
                                 const BlendInfo& blend, const ShaderProfile& profile);

/// The average colour of a glow texture, scaled so its brightest channel is 1:
/// the hue the glow paints its mask with, at full strength. White when the
/// texture is missing or black. Shared with the glTF material export.
std::array<double, 3> glow_colour(const ModelPreview& model, int texIndex);

}  // namespace castlemist::exportgltf
