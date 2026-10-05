/// @file
/// @brief Tests for character looks (ripper/look.h): palette table and names.

#include "test_framework.h"

#include "castlemist/ripper/look.h"

#include <string>

using namespace castlemist::ripper;

CM_TEST(look, names_creator_and_dye_colours) {
    CHECK_EQ(color_name(1097), std::string("Banana"));          // sylvari skin
    CHECK_EQ(color_name(80), std::string("Midnight Green"));    // sylvari hair (a dye too)
    CHECK_EQ(color_name(1513), std::string("Aquamarine"));      // makeover-kit eye colour
    CHECK_EQ(color_name(975), std::string("Dark Pine"));        // sylvari pattern
    CHECK_EQ(color_name(1), std::string("Dye Remover"));        // API dye
    CHECK(color_name(999999).empty());
}

CM_TEST(look, race_palettes_cover_every_playable_race) {
    RacePalettes s = race_palettes("Sylvari", "Female");
    CHECK_EQ(s.skin, 70u);
    CHECK_EQ(s.hair, 50u);
    CHECK_EQ(s.eye, 25u);
    CHECK_EQ(s.pattern, 75u);
    CHECK_EQ(s.glow, 52u);
    CHECK_EQ(race_palettes("Norn", "Male").pattern, 66u);   // tattoos
    CHECK_EQ(race_palettes("Charr", "Female").hair, 57u);
    CHECK_EQ(race_palettes("Human", "Male").eye, 6u);
    CHECK_EQ(race_palettes("Asura", "Female").eye, 15u);
    CHECK_EQ(race_palettes("Kodan", "Male").skin, 0u);
}

CM_TEST(look, eye_swatches_use_the_iris_red_when_the_palette_has_no_base) {
    castlemist::cmap::Palette eyes;  // base 0,0,0
    castlemist::cmap::PaletteColor plain{1, {castlemist::cmap::ColorShift{}}};
    CHECK(swatch_rgb(eyes, plain) == (std::array<uint8_t, 3>{192, 0, 0}));
}
