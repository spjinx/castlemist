/// @file
/// @brief The VRChat (Poiyomi) export folder for one model: the .glb, the
///        Poiyomi-named texture maps and materials.json.
///
/// Layout and schema: docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md,
/// sections "Output folder" and "4. materials.json". Blender (the .fbx) is the
/// ripper's job; this writes everything that needs no external tool.
/// @ingroup exportgltf

#pragma once

#include "castlemist/extract/model_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::exportgltf {

/// Result of write_vrchat_folder. Paths are UTF-8.
struct VrchatFolderResult {
    bool ok = false;
    std::string error;
    std::string folder, glb, materialsJson, particlesJson;  ///< particlesJson empty when none
    size_t materials = 0, clips = 0;
    std::vector<std::string> warnings;  ///< every material's warnings, prefixed "<Mat>: "
};

/// Write `<folder>/<name>.glb`, `<folder>/materials.json` and
/// `<folder>/Textures/<Mat> - <Map>.png` for `model`. `modelFileId` is
/// recorded in materials.json. A model with no meshes is refused before
/// anything is written.
VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                       const std::string& name, uint32_t modelFileId);

/// A legal Windows file name from `s`: control characters and
/// `/ \ : * ? " < > |` become '_', leading/trailing spaces and dots are
/// trimmed, reserved device names (CON, NUL, COM1, ...) get a '_' appended.
/// Never empty ("_"). UTF-8 is kept.
std::string safe_file_name(const std::string& s);

}  // namespace castlemist::exportgltf
