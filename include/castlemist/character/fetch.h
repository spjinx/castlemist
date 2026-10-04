#ifndef CASTLEMIST_CHARACTER_FETCH_H
#define CASTLEMIST_CHARACTER_FETCH_H

// One call from "character name" to a resolved manifest: the sequence the UI
// dialog and the CLI both run (on a background thread in the UI).

#include <optional>
#include <string>
#include <vector>

#include "castlemist/character/gw2_api.h"
#include "castlemist/character/manifest.h"
#include "castlemist/character/resolver.h"

namespace castlemist::character {

struct TabSummary {
    int tab = 0;
    std::string name;
    bool is_active = false;
};

struct FetchResult {
    CharacterManifest manifest;
    std::vector<TabSummary> tabs;
};

/// core -> equipment tabs -> the requested tab (default: the active one) ->
/// items -> skins -> colors -> resolve_character(). Throws ApiError, including
/// ApiError(0, "Character has no equipment tab <n>") for a tab that doesn't exist.
FetchResult fetch_character(Gw2Api& api, const std::string& name, std::optional<int> tab,
                            const AssetLookup& assets);

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_FETCH_H
