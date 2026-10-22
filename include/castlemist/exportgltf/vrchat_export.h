/// @file
/// @brief The VRChat (Poiyomi) export folder for one model: the .glb, the
///        Poiyomi-named texture maps and materials.json.
///
/// Layout and schema: docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md,
/// sections "Output folder" and "4. materials.json". Blender (the .fbx) is the
/// ripper's job; this writes everything that needs no external tool.
/// @ingroup exportgltf

#pragma once

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/extract/model_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

/// Result of write_vrchat_folder. Paths are UTF-8.
struct VrchatFolderResult {
    bool ok = false;
    std::string error;
    std::string folder, glb, materialsJson, particlesJson;  ///< particlesJson empty when none
    size_t materials = 0, clips = 0;
    std::vector<std::string> warnings;  ///< every material's warnings, prefixed "<Mat>: "
    /// One per model material when VrchatFolderOptions::materialKeys is set (Task 15d):
    /// its key (as in materials.json), display name and usedByMeshes.
    struct MaterialKey { std::string key, name; bool usedByMeshes = false; };
    std::vector<MaterialKey> materialKeys;
};

/// Options for write_vrchat_folder. Default-constructed, the folder is exactly
/// the VRChat model export's.
struct VrchatFolderOptions {
    /// Passed to export_model_gltf for the .glb (the world export sets
    /// rootMatrix and the image folder; see world-export spec D2).
    GltfExportOptions gltf;

    /// World export only (Task 15d). Set: every Poiyomi map is written once into
    /// this content pool as "<sha1 of the PNG>_<Role>.png" (SharedImagePool::
    /// ensure_content; Role is the map's suffix, e.g. BaseColor, Packed, mod) and
    /// materials.json names it as @ref generatedMapsPrefix + that name; nothing
    /// is written to Textures/. Null (the default): `Textures/<Mat> - <Role>.png`.
    std::shared_ptr<SharedImagePool> generatedMaps;
    std::string generatedMapsPrefix;   ///< the pool relative to the folder, e.g. "../../textures/gen/"
    /// World export only (Task 15d): each materials.json entry gains "key", the
    /// material's content key (export_model_gltf's GltfExportOptions::
    /// materialKeyInputs, with the entry less name, fileName, usedByMeshes and
    /// warnings, plus @ref materialKeyExtras as its input), and its glTF material
    /// extras.castlemistMaterialKey the same key. Off (the default) writes neither.
    bool materialKeys = false;
    /// Per model material, more key input (the world export's dark-decal flag and
    /// uvScroll entry); missing entries count as null.
    std::vector<nlohmann::json> materialKeyExtras;
};

/// Write `<folder>/<name>.glb`, `<folder>/materials.json` and
/// `<folder>/Textures/<Mat> - <Map>.png` for `model`. `modelFileId` is
/// recorded in materials.json. A model with no meshes is refused before
/// anything is written.
VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                       const std::string& name, uint32_t modelFileId);

/// As above, with options; the 4-argument form passes `VrchatFolderOptions{}`.
VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                       const std::string& name, uint32_t modelFileId,
                                       const VrchatFolderOptions& opts);

/// A legal Windows file name from `s`: control characters and
/// `/ \ : * ? " < > |` become '_', leading/trailing spaces and dots are
/// trimmed, reserved device names (CON, NUL, COM1, ...) get a '_' appended.
/// Never empty ("_"). UTF-8 is kept.
std::string safe_file_name(const std::string& s);

/// @brief A DDS cubemap with its mip chain completed down to 1 x 1 (the missing
///        levels repeat the smallest block), as Unity's DDS importer requires;
///        anything else (not a block-compressed cube, already complete) as it is.
std::vector<uint8_t> complete_dds_cube_mips(const std::vector<uint8_t>& dds);

}  // namespace castlemist::exportgltf
