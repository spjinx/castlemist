#include "castlemist/ripper/look.h"

#include "castlemist/ripper/dye.h"

namespace castlemist::ripper {

RacePalettes race_palettes(const std::string& race, const std::string& gender) {
    (void)gender;  // both genders share their race's palettes
    // Palette uids, matched against the wiki's per-race swatch lists:
    //   67 human/norn skin (36)   7 human/norn/asura hair (46)
    //   89 asura skin (34)        20 charr fur and hair (35)
    //   70 sylvari skin (96)      50 sylvari hair (148; the wiki lists 76 of them)
    if (race == "Human" || race == "Norn") return {67, 7};
    if (race == "Asura") return {89, 7};
    if (race == "Charr") return {20, 20};
    if (race == "Sylvari") return {70, 50};
    return {};
}

character::DyeShift to_dye_shift(const cmap::ColorShift& s) {
    return {s.brightness, s.contrast, s.hue, s.saturation, s.lightness};
}

std::optional<character::DyeShift> palette_shift(uint32_t palette_uid, uint32_t color_id) {
    if (!palette_uid || !color_id) return std::nullopt;
    const cmap::Palette* p = cmap::palette(palette_uid);
    if (!p) return std::nullopt;
    for (const cmap::PaletteColor& c : p->colors)
        if (c.id == color_id && !c.materials.empty()) return to_dye_shift(c.materials[0]);
    return std::nullopt;
}

std::array<uint8_t, 3> swatch_rgb(const cmap::Palette& palette, const cmap::PaletteColor& color) {
    if (color.materials.empty()) return palette.base;
    return apply_dye(dye_matrix(to_dye_shift(color.materials[0])), palette.base);
}

void apply_look(AssemblyOptions& options, const character::CharacterLook& look, const std::string& race,
                const std::string& gender) {
    const RacePalettes pal = race_palettes(race, gender);
    options.face = look.face;
    options.hair = look.hair;
    options.skin_tint = palette_shift(pal.skin, look.skin_color);
    options.hair_tint = palette_shift(pal.hair, look.hair_color);
    options.hair_tint2 = palette_shift(pal.hair, look.hair_color2 ? look.hair_color2 : look.hair_color);
}

} // namespace castlemist::ripper
