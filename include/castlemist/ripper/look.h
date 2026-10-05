#ifndef CASTLEMIST_RIPPER_LOOK_H
#define CASTLEMIST_RIPPER_LOOK_H

// A character's look -- what the GW2 API does not expose -- from the game's own
// character-creator options: the race's faces and hair styles (Composite) and
// its skin and hair colour palettes (content map, cntc type 147). The Composite
// names a race's palettes ("Gw2.Common.Color.Female Skin Sylvari"), but the
// shipped content store only carries obfuscated names, so the race -> palette
// table below was matched by colour against the wiki's swatch lists (every
// listed swatch reproduces exactly). See docs/research/gw2-armor-skins-and-dyes.md
// section 6.

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "castlemist/character/look_store.h"
#include "castlemist/character/manifest.h"
#include "castlemist/format/content_map.h"
#include "castlemist/ripper/assemble.h"

namespace castlemist::ripper {

/// Palette uids (cmap::palette()) of a race's character-creator colours; 0 = none.
/// Each holds the regular colours first, then the Total Makeover Kit exclusives.
struct RacePalettes {
    uint32_t skin = 0;
    uint32_t hair = 0;
    uint32_t eye = 0;
    uint32_t pattern = 0;    // sylvari skin pattern, norn tattoos, charr fur pattern, asura markings
    uint32_t glow = 0;       // sylvari only
    uint32_t accessory = 0;  // horns, tusks, ... (not sylvari)
};

/// By manifest race ("Sylvari") and gender ("Female"). Unknown race -> all 0.
RacePalettes race_palettes(const std::string& race, const std::string& gender);

/// A palette colour's shift (its first material), in dye units.
character::DyeShift to_dye_shift(const cmap::ColorShift& s);

/// The shift of colour `color_id` in palette `palette_uid`; nullopt if either is
/// unknown (or the content map isn't loaded) or `color_id` is 0.
std::optional<character::DyeShift> palette_shift(uint32_t palette_uid, uint32_t color_id);

/// What a swatch of the colour looks like: its shift applied to the palette's
/// base colour (exactly the wiki's swatch colours). Eye palettes store no base;
/// their swatches use the iris red (192,0,0), which reproduces the wiki's eye
/// swatches to within a few levels.
std::array<uint8_t, 3> swatch_rgb(const cmap::Palette& palette, const cmap::PaletteColor& color);

/// The colour's name ("Banana", "Midnight Green", "Aquamarine", ...), or empty
/// when unknown. Dye names are the API's; character-creator names come from the
/// GW2 wiki's lists, matched to the palettes (see color_names.inc).
std::string color_name(uint32_t color_id);

/// Puts `look` into assembly options: face and hair style indices, and the skin
/// and hair colours resolved through the race's palettes (a colour the content
/// map doesn't know is left untinted).
void apply_look(AssemblyOptions& options, const character::CharacterLook& look, const std::string& race,
                const std::string& gender);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_LOOK_H
