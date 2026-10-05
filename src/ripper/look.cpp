#include "castlemist/ripper/look.h"

#include <algorithm>
#include <iterator>

#include "castlemist/ripper/dye.h"

namespace castlemist::ripper {

RacePalettes race_palettes(const std::string& race, const std::string& gender) {
    (void)gender;  // both genders share their race's palettes
    // Palette uids, matched against the wiki's per-race swatch lists (regular +
    // makeover-kit exclusive counts):
    //   skin:    67 human/norn (36), 89 asura (34), 20 charr fur (35), 70 sylvari (96)
    //   hair:    72 human/norn/asura (46+72), 57 charr (35+72), 50 sylvari (76+72)
    //   eyes:    6 human/norn (31+50), 15 asura/charr (43+50), 25 sylvari (36+50)
    //   pattern: 66 norn tattoos (14), 75 sylvari (64); the others reuse skin
    //   glow:    52 sylvari (40);  accessory: 51 (21+30)
    if (race == "Human") return {67, 72, 6, 67, 0, 51};
    if (race == "Norn") return {67, 72, 6, 66, 0, 51};
    if (race == "Asura") return {89, 72, 15, 89, 0, 51};
    if (race == "Charr") return {20, 57, 15, 20, 0, 51};
    if (race == "Sylvari") return {70, 50, 25, 75, 52, 0};
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
    const std::array<uint8_t, 3> base =
        palette.base == std::array<uint8_t, 3>{} ? std::array<uint8_t, 3>{192, 0, 0} : palette.base;
    if (color.materials.empty()) return base;
    return apply_dye(dye_matrix(to_dye_shift(color.materials[0])), base);
}

namespace {
struct NamedColor {
    uint32_t id;
    const char8_t* name;
};
constexpr NamedColor kColorNames[] = {
#include "color_names.inc"
};
} // namespace

std::string color_name(uint32_t color_id) {
    auto it = std::lower_bound(std::begin(kColorNames), std::end(kColorNames), color_id,
                               [](const NamedColor& c, uint32_t id) { return c.id < id; });
    if (it == std::end(kColorNames) || it->id != color_id) return {};
    const char8_t* s = it->name;
    return std::string(reinterpret_cast<const char*>(s));
}

void apply_look(AssemblyOptions& options, const character::CharacterLook& look, const std::string& race,
                const std::string& gender) {
    const RacePalettes pal = race_palettes(race, gender);
    options.face = look.face;
    options.hair = look.hair;
    options.skin_tint = palette_shift(pal.skin, look.skin_color);
    options.hair_tint = palette_shift(pal.hair, look.hair_color);
    options.hair_tint2 = palette_shift(pal.hair, look.hair_color2 ? look.hair_color2 : look.hair_color);
    options.ears = look.ears;
    options.eye_tint = palette_shift(pal.eye, look.eye_color);
    options.pattern = look.pattern;
    options.pattern_tint = palette_shift(pal.pattern, look.pattern_color);
    options.glow_rgb.reset();
    options.glow_intensity = look.glow_intensity;
    options.face_sliders = look.sliders;
    if (look.glow_color && pal.glow)
        if (const cmap::Palette* p = cmap::palette(pal.glow))
            for (const cmap::PaletteColor& c : p->colors)
                if (c.id == look.glow_color) {
                    // Glow swatches are dim (Banana = 5A3D00); the game adds the glow
                    // boosted. Light it at full brightness, same hue.
                    std::array<uint8_t, 3> rgb = swatch_rgb(*p, c);
                    const int peak = std::max({rgb[0], rgb[1], rgb[2]});
                    if (peak > 0)
                        for (uint8_t& v : rgb) v = static_cast<uint8_t>(v * 255 / peak);
                    options.glow_rgb = rgb;
                }
}

} // namespace castlemist::ripper
