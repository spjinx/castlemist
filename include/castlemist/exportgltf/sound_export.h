/// @file
/// @brief Model sounds (2026-10-25): a model's AMSP sound script (ModelFileData.soundScript) as a
///        `<stem>_sounds.json` sidecar beside its glb, with the MP3 / Ogg files it plays in
///        `sounds/`. The script's trigger keys are named after the model's clips; each marker
///        plays a metaSound (by its token) at a time into the clip.
/// @ingroup exportgltf
#pragma once

#include "castlemist/extract/model_types.h"
#include "castlemist/native/gw2model.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace castlemist::exportgltf {

/// @brief The sidecar JSON of @p script (fileId @p scriptId) for @p model:
///        `sounds[]` (id = token64 as a decimal string, name = its decoded letters, files with
///        their written path from @p audioPaths or null, volume / pitch as {value, type,
///        random, envelope}, rolloff = attenuation volumeA's envelope or null, fades, ranges,
///        loop and playback modes as stored), `triggers[]` (id, name, clip = the model clip
///        whose token resolves to it (world::resolve_prop_clip) or null, markers {sound id,
///        time, type}) and `handlers[]` (names; their bytecode is not decoded).
nlohmann::json sound_sidecar_json(const ModelPreview& model, const castlemist::model::SoundScript& script,
                                  uint32_t scriptId, const std::map<uint32_t, std::string>& audioPaths);

/// @brief Reads @p model's sound script through @p readFile (decompressed dat bytes by fileId;
///        empty = missing), writes each sound file's MP3 / Ogg / WAV into `<folder>/sounds/
///        <fileId>.<ext>` and the sidecar `<folder>/<stem>_sounds.json`.
/// @return The sidecar's file name, or "" when the model has no script or it holds no sounds
///         (with a line in @p warnings for a script that did not read).
std::string write_model_sounds(const ModelPreview& model, const std::function<std::vector<uint8_t>(uint32_t)>& readFile,
                               const nlohmann::json& tpl, const std::string& folderUtf8, const std::string& stem,
                               std::vector<std::string>& warnings);

} // namespace castlemist::exportgltf
