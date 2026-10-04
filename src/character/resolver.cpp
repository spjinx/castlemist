#include "castlemist/character/resolver.h"

#include <algorithm>

#include "castlemist/format/content_map.h"

namespace castlemist::character {

const char* to_string(PieceStatus s) {
    switch (s) {
    case PieceStatus::Ok: return "ok";
    case PieceStatus::NoSkin: return "no_skin";
    case PieceStatus::Unresolved: return "unresolved";
    case PieceStatus::NoContentMap: return "no_content_map";
    }
    return "no_skin";
}

std::optional<PieceStatus> piece_status_from_string(std::string_view s) {
    for (PieceStatus p : {PieceStatus::Ok, PieceStatus::NoSkin, PieceStatus::Unresolved, PieceStatus::NoContentMap})
        if (s == to_string(p)) return p;
    return std::nullopt;
}

bool CmapAssetLookup::built() const { return cmap::built(); }

std::vector<uint32_t> CmapAssetLookup::skin_assets(uint32_t skin_id) const {
    return cmap::resolve_all(cmap::CONTENT_TYPE_SKIN, skin_id);
}

std::optional<uint32_t> CmapAssetLookup::item_skin(uint32_t item_id) const {
    for (const cmap::ContentLink& l : cmap::item_links(item_id))
        if (l.type == cmap::CONTENT_TYPE_SKIN && l.via_item == 0) return l.id;
    return std::nullopt;
}

namespace {

std::vector<uint32_t> sorted_unique(std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

uint32_t skin_for(const EquipmentEntry& e, const std::map<uint32_t, ApiItem>& items, const AssetLookup& assets) {
    if (e.skin) return *e.skin;
    auto it = items.find(e.item_id);
    if (it != items.end() && it->second.default_skin) return *it->second.default_skin;
    return assets.item_skin(e.item_id).value_or(0);
}

const DyeSlots& dye_slots_for(const ApiSkin& s, const std::string& race_gender) {
    auto it = s.dye_overrides.find(race_gender);
    return it != s.dye_overrides.end() ? it->second : s.dye_default;
}

} // namespace

std::vector<uint32_t> collect_item_ids(const EquipmentTab& tab) {
    std::vector<uint32_t> ids;
    for (const EquipmentEntry& e : tab.equipment) ids.push_back(e.item_id);
    return sorted_unique(std::move(ids));
}

std::vector<uint32_t> collect_skin_ids(const EquipmentTab& tab, const std::map<uint32_t, ApiItem>& items,
                                       const AssetLookup& assets) {
    std::vector<uint32_t> ids;
    for (const EquipmentEntry& e : tab.equipment)
        if (uint32_t s = skin_for(e, items, assets)) ids.push_back(s);
    return sorted_unique(std::move(ids));
}

std::vector<uint32_t> collect_color_ids(const EquipmentTab& tab, const std::map<uint32_t, ApiSkin>& skins) {
    std::vector<uint32_t> ids;
    for (const EquipmentEntry& e : tab.equipment)
        for (const auto& d : e.dyes) if (d) ids.push_back(*d);
    auto add_slots = [&](const DyeSlots& slots) {
        for (const auto& s : slots) if (s) ids.push_back(s->color_id);
    };
    for (const auto& [id, s] : skins) {
        add_slots(s.dye_default);
        for (const auto& [k, slots] : s.dye_overrides) add_slots(slots);
    }
    return sorted_unique(std::move(ids));
}

CharacterManifest resolve_character(const CharacterCore& core, const EquipmentTab& tab,
                                    const std::map<uint32_t, ApiItem>& items,
                                    const std::map<uint32_t, ApiSkin>& skins,
                                    const std::map<uint32_t, ApiColor>& colors, const AssetLookup& assets) {
    CharacterManifest m;
    m.name = core.name;
    m.race = core.race;
    m.gender = core.gender;
    m.profession = core.profession;
    m.level = core.level;
    m.tab_id = tab.tab;
    m.tab_name = tab.name;
    const std::string race_gender = core.race + core.gender;

    for (const EquipmentEntry& e : tab.equipment) {
        ManifestPiece p;
        p.slot = e.slot;
        p.item_id = e.item_id;
        if (auto it = items.find(e.item_id); it != items.end()) p.item_name = it->second.name;
        p.skin_id = skin_for(e, items, assets);
        if (p.skin_id == 0) {
            p.status = PieceStatus::NoSkin;
            m.pieces.push_back(std::move(p));
            continue;
        }
        if (!assets.built()) {
            p.status = PieceStatus::NoContentMap;
        } else {
            p.file_ids = assets.skin_assets(p.skin_id);
            p.status = p.file_ids.empty() ? PieceStatus::Unresolved : PieceStatus::Ok;
        }
        if (auto sit = skins.find(p.skin_id); sit != skins.end()) {
            const ApiSkin& s = sit->second;
            p.skin_name = s.name;
            p.weight_class = s.weight_class;
            const DyeSlots& slots = dye_slots_for(s, race_gender);
            for (size_t i = 0; i < slots.size(); ++i) {
                if (!slots[i]) continue;
                ManifestDye d;
                d.color_id = i < e.dyes.size() && e.dyes[i] ? *e.dyes[i] : slots[i]->color_id;
                d.material = slots[i]->material;
                if (auto cit = colors.find(d.color_id); cit != colors.end()) {
                    d.color_name = cit->second.name;
                    if (auto rit = cit->second.rgb.find(d.material); rit != cit->second.rgb.end()) {
                        d.rgb = rit->second;
                        d.known = true;
                    }
                }
                p.dyes.push_back(std::move(d));
            }
        }
        m.pieces.push_back(std::move(p));
    }
    return m;
}

} // namespace castlemist::character
