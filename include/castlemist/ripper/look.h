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
struct RacePalettes {
    uint32_t skin = 0;
    uint32_t hair = 0;
};

/// By manifest race ("Sylvari") and gender ("Female"). Unknown race -> all 0.
RacePalettes race_palettes(const std::string& race, const std::string& gender);

/// A palette colour's shift (its first material), in dye units.
character::DyeShift to_dye_shift(const cmap::ColorShift& s);

/// The shift of colour `color_id` in palette `palette_uid`; nullopt if either is
/// unknown (or the content map isn't loaded) or `color_id` is 0.
std::optional<character::DyeShift> palette_shift(uint32_t palette_uid, uint32_t color_id);

/// What a swatch of the colour looks like: its shift applied to the palette's
/// base colour (exactly the wiki's swatch colours).
std::array<uint8_t, 3> swatch_rgb(const cmap::Palette& palette, const cmap::PaletteColor& color);

/// Puts `look` into assembly options: face and hair style indices, and the skin
/// and hair colours resolved through the race's palettes (a colour the content
/// map doesn't know is left untinted).
void apply_look(AssemblyOptions& options, const character::CharacterLook& look, const std::string& race,
                const std::string& gender);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_LOOK_H
