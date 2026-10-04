/// @file
/// @brief The Character Ripper dialog's data state and its transitions, kept
///        free of Win32 so the rules about what survives a failed fetch are testable.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "castlemist/character/fetch.h"

namespace castlemist::ui {

struct RipperState {
    std::vector<std::string> names;                       // character names, combo order
    std::optional<castlemist::character::FetchResult> current;  // what the table shows
    std::string current_key;                              // key `names` came from
    /// (slot, race-correct mesh) from the last export of `current`; Open model
    /// prefers it over the manifest's default model. Cleared whenever `current` changes.
    std::vector<std::pair<std::string, uint32_t>> exported_mesh;

    /// A characters-list request for `key` finished. Success adopts the key and
    /// its names; failure forgets the previous account entirely, so no later
    /// fetch can pair one account's character names with another's key.
    void names_done(const std::string& key, std::vector<std::string> new_names, const std::string& error) {
        current.reset();
        exported_mesh.clear();
        if (!error.empty()) {
            names.clear();
            current_key.clear();
            return;
        }
        names = std::move(new_names);
        current_key = key;
    }

    /// A character fetch finished. A failure drops the previously shown
    /// character, so the table and Save never present a different character
    /// than the one selected.
    void fetch_done(std::optional<castlemist::character::FetchResult> result, const std::string& error) {
        exported_mesh.clear();
        if (!error.empty()) current.reset();
        else current = std::move(result);
    }
};

} // namespace castlemist::ui
