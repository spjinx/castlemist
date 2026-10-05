/// @file
/// @brief Tests for the dye color matrix and texture bake (ripper/dye.h).

#include "test_framework.h"

#include "castlemist/ripper/dye.h"

#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace castlemist::ripper;
using castlemist::character::DyeShift;

namespace {

nlohmann::json colors_fixture() {
    std::ifstream f(CM_TEST_DATA_DIR "/character/colors.json", std::ios::binary);
    return nlohmann::json::parse(std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
}

DyeShift shift_of(const nlohmann::json& m) {
    return DyeShift{m["brightness"].get<float>(), m["contrast"].get<float>(), m["hue"].get<float>(),
                    m["saturation"].get<float>(), m["lightness"].get<float>()};
}

std::string show_rgb(std::array<uint8_t, 3> c) {
    return std::to_string(c[0]) + "," + std::to_string(c[1]) + "," + std::to_string(c[2]);
}

} // namespace

CM_TEST(dye, matches_api_for_every_fixture_color) {
    size_t checked = 0;
    for (const auto& c : colors_fixture()) {
        std::array<uint8_t, 3> base = {c["base_rgb"][0].get<uint8_t>(), c["base_rgb"][1].get<uint8_t>(),
                                       c["base_rgb"][2].get<uint8_t>()};
        for (const char* mat : {"cloth", "leather", "metal", "fur"}) {
            if (!c.contains(mat)) continue;
            const auto& m = c[mat];
            std::array<uint8_t, 3> want = {m["rgb"][0].get<uint8_t>(), m["rgb"][1].get<uint8_t>(),
                                           m["rgb"][2].get<uint8_t>()};
            CHECK_EQ(show_rgb(apply_dye(dye_matrix(shift_of(m)), base)), show_rgb(want));
            ++checked;
        }
    }
    CHECK(checked >= 50);
}

CM_TEST(dye, identity_shift_is_identity) {
    ColorMatrix m = dye_matrix(DyeShift{});
    CHECK_EQ(show_rgb(apply_dye(m, {12, 200, 77})), std::string("12,200,77"));
}

CM_TEST(dye, bake_full_mask_applies_matrix) {
    std::vector<uint8_t> rgba = {128, 26, 26, 255, 10, 20, 30, 128};
    std::vector<uint8_t> mask = {255, 255, 255, 255, 255, 255, 255, 255};
    ColorMatrix m = dye_matrix(DyeShift{15, 1.25f, 38, 0.28125f, 1.44531f});
    bake_dyes(rgba, 2, 1, {&mask, nullptr, nullptr, nullptr}, {m, std::nullopt, std::nullopt, std::nullopt});
    auto px0 = apply_dye(m, {128, 26, 26});
    auto px1 = apply_dye(m, {10, 20, 30});
    CHECK_EQ(show_rgb({rgba[0], rgba[1], rgba[2]}), show_rgb(px0));
    CHECK_EQ(show_rgb({rgba[4], rgba[5], rgba[6]}), show_rgb(px1));
}

CM_TEST(dye, bake_half_mask_lerps) {
    std::vector<uint8_t> rgba = {100, 100, 100, 255};
    std::vector<uint8_t> mask = {128, 128, 128, 255};
    ColorMatrix m = dye_matrix(DyeShift{64, 1, 0, 1, 1});  // pure brightness shift
    auto full = apply_dye(m, {100, 100, 100});
    bake_dyes(rgba, 1, 1, {&mask, nullptr, nullptr, nullptr}, {m, std::nullopt, std::nullopt, std::nullopt});
    int mid = (100 + full[0]) / 2;
    CHECK(std::abs(int(rgba[0]) - mid) <= 1);
}

CM_TEST(dye, bake_skips_missing_mask_and_dye) {
    std::vector<uint8_t> rgba = {50, 60, 70, 255};
    std::vector<uint8_t> mask = {255, 255, 255, 255};
    ColorMatrix m = dye_matrix(DyeShift{64, 1, 0, 1, 1});
    bake_dyes(rgba, 1, 1, {nullptr, &mask, nullptr, nullptr}, {m, std::nullopt, std::nullopt, std::nullopt});
    CHECK_EQ(show_rgb({rgba[0], rgba[1], rgba[2]}), std::string("50,60,70"));
}

CM_TEST(dye, bake_keeps_alpha) {
    std::vector<uint8_t> rgba = {128, 26, 26, 77};
    std::vector<uint8_t> mask = {255, 255, 255, 255};
    bake_dyes(rgba, 1, 1, {&mask, nullptr, nullptr, nullptr},
              {dye_matrix(DyeShift{15, 1.25f, 38, 0.28125f, 1.44531f}), std::nullopt, std::nullopt, std::nullopt});
    CHECK_EQ(int(rgba[3]), 77);
}

CM_TEST(dye, rest_tints_what_no_mask_covers) {
    // Texel 0 fully masked (hair), texel 1 unmasked (scalp): the dye wins on the
    // first, the rest (skin) colour on the second -- both from the authored base.
    std::vector<uint8_t> rgba = {128, 26, 26, 255, 128, 26, 26, 255};
    std::vector<uint8_t> mask = {255, 255, 255, 255, 0, 0, 0, 255};
    ColorMatrix hair = dye_matrix(DyeShift{15, 1.25f, 38, 0.28125f, 1.44531f});
    ColorMatrix skin = dye_matrix(DyeShift{17, 1.2109375f, 85, 0.328125f, 1.25f});
    bake_dyes(rgba, 2, 1, {&mask, nullptr, nullptr, nullptr}, {hair, std::nullopt, std::nullopt, std::nullopt}, skin);
    CHECK_EQ(show_rgb({rgba[0], rgba[1], rgba[2]}), show_rgb(apply_dye(hair, {128, 26, 26})));
    CHECK_EQ(show_rgb({rgba[4], rgba[5], rgba[6]}), show_rgb(apply_dye(skin, {128, 26, 26})));
}
