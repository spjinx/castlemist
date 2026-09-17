/// @file
/// @brief Public API: ModelPreview / MapScene -> binary glTF (.glb) on disk.
/// @ingroup exportgltf

#pragma once

#include <string>
#include <vector>

#include "castlemist/extract/map_types.h"
#include "castlemist/extract/model_types.h"

/// @defgroup exportgltf exportgltf -- binary glTF (.glb) writer
/// @brief Turns already-extracted CPU model/map data into a Blender/Unity-ready .glb.
///
/// glTF rather than FBX: FBX's *binary* container (the only form Blender's
/// importer reliably reads -- ASCII FBX is technically legal but not
/// supported in practice) has no official public specification, and hand
/// matching it byte-for-byte against an undocumented format proved fragile.
/// glTF 2.0 is a precisely specified, Khronos-ratified format with
/// unambiguous skinning (a skin is just a joint list + inverse bind
/// matrices) and animation (channels target a node's translation/rotation/
/// scale directly, sampled from an explicit keyframe accessor -- no
/// implicit connection graph to get subtly wrong), and Blender's importer is
/// first-party/core rather than a community addon.
///
/// A single .glb embeds geometry, materials, textures, the skeleton/skin and
/// animation clips in one file. For the final Unity/VRChat hop, import the
/// .glb into Blender and use Blender's own FBX exporter -- one extra manual
/// step that trades a hand-rolled binary writer for Blender's battle-tested
/// one on that leg.
///
/// Pure CPU-in, files-out -- no UI or Direct3D dependency, safe to call from
/// a background thread exactly like `extract_entry`.
/// @{

namespace castlemist::exportgltf {

/// @brief Tunables for one export call.
struct GltfExportOptions {
    /// @brief Fixed sample rate used to bake every animation clip's keyframes.
    ///
    /// GW2's Granny curves come in a dozen native formats (constant, spline,
    /// dense keyframe); rather than translate each one into a glTF
    /// interpolation mode, every clip is resampled at this rate via
    /// `castlemist::granny::sample()`, which is format-agnostic and is what
    /// most game-asset exporters do.
    int animFps = 30;
};

/// @brief Result of one export call.
struct GltfExportResult {
    bool ok = false;             ///< False on any hard failure; see @ref error.
    std::string error;           ///< Human-readable failure reason when !ok.
    std::string glbPath;         ///< The .glb actually written.
};

/// @brief Export one model (as shown in the Model preview tab) to a .glb.
///
/// @param model   A fully built model preview (meshes, materials, textures,
///                skeleton and animation clips already decoded).
/// @param glbPath Destination .glb path.
GltfExportResult export_model_gltf(const ModelPreview& model, const std::string& glbPath,
                                   const GltfExportOptions& opts = {});

/// @brief Export a whole map's placed props to one glTF scene.
///
/// Every `MapInstance` already loaded into `scene` is written world-
/// transformed, sharing one glTF mesh per unique model across all of its
/// (unskinned) instances via ordinary node instancing -- glTF nodes may
/// carry an arbitrary 4x4 matrix, so GW2's own
/// `Scale * RotZ * RotX * RotY * Translate` placement is written directly
/// with no decomposition needed. Textures are deduped by fileId across every
/// unique model in the scene. Does not include the raw heightmap terrain
/// surface -- only placed props (whatever layers the caller already loaded
/// into `scene.instances`).
///
/// @param scene   A built map scene (::build_map_scene / ::build_map_zone_layer).
/// @param glbPath Destination .glb path.
GltfExportResult export_map_gltf(const MapScene& scene, const std::string& glbPath,
                                 const GltfExportOptions& opts = {});

/// @brief Result of one standalone texture save.
struct TextureSaveResult {
    bool ok = false;    ///< False on any hard failure; see @ref error.
    std::string error;  ///< Human-readable failure reason when !ok.
};

/// @brief Encodes one already-decoded model texture as a standalone PNG file.
///
/// Reuses the same stb_image_write path the glTF exporter embeds textures
/// with (the sole stb_image_write "implementation" translation unit lives in
/// this layer -- see texture_export.cpp), so a texture saved this way is
/// pixel-identical to what a full model export would have embedded.
///
/// @param tex     An already-decoded texture (`ModelPreview::textures[i]`).
/// @param pngPath Destination .png path.
TextureSaveResult save_texture_png(const ModelTextureCPU& tex, const std::string& pngPath);

} // namespace castlemist::exportgltf

/// @}
