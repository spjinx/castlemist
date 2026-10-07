#ifndef CASTLEMIST_CHARACTER_MANIFEST_JSON_H
#define CASTLEMIST_CHARACTER_MANIFEST_JSON_H

// CharacterManifest <-> JSON (format version 1). This is the file the
// Character Ripper dialog saves and `gw2dat_cli character` emits, and what the
// later export/assembly steps read back.
//
// {"version":1,"name","race","gender","profession","level","tab":{"id","name"},
//  "pieces":[{"slot","item_id","item_name","skin_id","skin_name","weight_class",
//             "file_ids":[...],"status":"ok",
//             "dyes":[{"slot","color_id","color_name","material","rgb":"#RRGGBB","known"}]}]}

#include <nlohmann/json.hpp>

#include "castlemist/character/manifest.h"

namespace castlemist::character {

nlohmann::json manifest_to_json(const CharacterManifest& m);
/// Throws std::runtime_error for anything but version 1, or a malformed document.
CharacterManifest manifest_from_json(const nlohmann::json& j);

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_MANIFEST_JSON_H
