#include "castlemist/character/fetch.h"

namespace castlemist::character {

FetchResult fetch_character(Gw2Api& api, const std::string& name, std::optional<int> tab,
                            const AssetLookup& assets) {
    CharacterCore core = api.character_core(name);
    std::vector<EquipmentTab> tabs = api.equipment_tabs(name);

    FetchResult r;
    const EquipmentTab* chosen = nullptr;
    for (const EquipmentTab& t : tabs) {
        r.tabs.push_back({t.tab, t.name, t.is_active});
        if (tab ? t.tab == *tab : t.is_active) chosen = &t;
    }
    if (!chosen && !tab && !tabs.empty()) chosen = &tabs.front();
    if (!chosen) {
        throw ApiError(0, tab ? "Character has no equipment tab " + std::to_string(*tab)
                              : std::string("Character has no equipment tabs"));
    }

    auto items = api.items(collect_item_ids(*chosen));
    auto skins = api.skins(collect_skin_ids(*chosen, items, assets));
    auto colors = api.colors(collect_color_ids(*chosen, skins));
    r.manifest = resolve_character(core, *chosen, items, skins, colors, assets);
    return r;
}

} // namespace castlemist::character
