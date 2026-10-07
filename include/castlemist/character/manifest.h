#ifndef CASTLEMIST_CHARACTER_MANIFEST_H
#define CASTLEMIST_CHARACTER_MANIFEST_H

// What one character wears on one equipment tab, resolved from GW2 API ids
// down to dat fileIds: the input to every later step of the character ripper
// (per-piece export, assembly, head/hair presets).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace castlemist::character {

enum class PieceStatus {
    Ok,            // skin found in the content map; file_ids non-empty
    NoSkin,        // nothing to show: empty/hidden slot, or a skin-less item (trinkets)
    Unresolved,    // skin id known, but not in this dat's content map (patched since?)
    NoContentMap,  // the content map isn't built/loaded, so no fileIds were looked up
};

/// "ok", "no_skin", "unresolved", "no_content_map".
const char* to_string(PieceStatus s);
std::optional<PieceStatus> piece_status_from_string(std::string_view s);

/// A dye's color shift for one material, straight from /v2/colors: the inputs
/// of the dye color matrix (docs/research/gw2-armor-skins-and-dyes.md section 4).
struct DyeShift {
    float brightness = 0, contrast = 1, hue = 0, saturation = 1, lightness = 1;
};

struct ManifestDye {
    int slot = 0;          // the skin's dye channel (0-3); empty channels have no entry
    uint32_t color_id = 0;
    std::string color_name;
    std::string material;  // the dye slot's material: "cloth", "leather", "metal", "fur"
    std::array<uint8_t, 3> rgb{};
    bool known = false;    // false when the color, or its rgb for `material`, is unknown
    std::optional<DyeShift> shift;  // the color's shift for `material`, when known
};

struct ManifestPiece {
    std::string slot;
    uint32_t item_id = 0;
    std::string item_name;
    uint32_t skin_id = 0;
    std::string skin_name, weight_class;
    std::string skin_type;      // /v2/skins type: "Armor", "Weapon", "Back", ...
    uint64_t skin_token = 0;    // composite appearance token (armor), 0 = none/unknown
    std::vector<uint32_t> file_ids;  // content-map assets of the skin, model first
    std::vector<ManifestDye> dyes;   // one per non-empty dye slot of the skin, in slot order
    PieceStatus status = PieceStatus::NoSkin;
};

struct CharacterManifest {
    std::string name, race, gender, profession;
    int level = 0;
    int tab_id = 0;
    std::string tab_name;
    std::vector<ManifestPiece> pieces;
};

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_MANIFEST_H
