#ifndef CASTLEMIST_CHARACTER_RESOLVER_H
#define CASTLEMIST_CHARACTER_RESOLVER_H

// Pure resolution of a character's equipment tab (API data) into a
// CharacterManifest, with the dat side reached through AssetLookup so tests
// can supply an in-memory content map.

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "castlemist/character/gw2_api.h"
#include "castlemist/character/manifest.h"

namespace castlemist::character {

class AssetLookup {
public:
    virtual ~AssetLookup() = default;
    virtual bool built() const = 0;
    /// The skin's content-map asset fileIds, model first. Empty = not found.
    virtual std::vector<uint32_t> skin_assets(uint32_t skin_id) const = 0;
    /// The item's own skin according to the dat (for items the API didn't return).
    virtual std::optional<uint32_t> item_skin(uint32_t item_id) const = 0;
    /// The skin's composite appearance token (armor), 0 when unknown.
    virtual uint64_t skin_token(uint32_t skin_id) const = 0;
};

/// AssetLookup over the process-wide castlemist::cmap content map.
class CmapAssetLookup final : public AssetLookup {
public:
    bool built() const override;
    std::vector<uint32_t> skin_assets(uint32_t skin_id) const override;
    std::optional<uint32_t> item_skin(uint32_t item_id) const override;
    uint64_t skin_token(uint32_t skin_id) const override;
};

/// Ids to batch-fetch for a tab; each sorted and de-duplicated.
std::vector<uint32_t> collect_item_ids(const EquipmentTab& tab);
/// Transmute skins, else the items' default skins, else the dat's item skin.
std::vector<uint32_t> collect_skin_ids(const EquipmentTab& tab, const std::map<uint32_t, ApiItem>& items,
                                       const AssetLookup& assets);
/// Equipment dyes plus every skin's default (and race/gender override) dye colors.
std::vector<uint32_t> collect_color_ids(const EquipmentTab& tab, const std::map<uint32_t, ApiSkin>& skins);

/// Per entry, in tab order:
///  1. skin = transmute skin, else the item's default_skin, else assets.item_skin(), else none;
///  2. no skin -> NoSkin; content map not built -> NoContentMap; no assets -> Unresolved; else Ok;
///  3. dye slots = the skin's override for race+gender (e.g. "CharrFemale") if present,
///     else its defaults; each non-null slot gets the entry's dye or the slot default,
///     with rgb picked by the slot's material.
CharacterManifest resolve_character(const CharacterCore& core, const EquipmentTab& tab,
                                    const std::map<uint32_t, ApiItem>& items,
                                    const std::map<uint32_t, ApiSkin>& skins,
                                    const std::map<uint32_t, ApiColor>& colors, const AssetLookup& assets);

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_RESOLVER_H
