/// @file
/// @brief Tests for the VRChat (Poiyomi) export helpers: blend decode.

#include "test_framework.h"

#include "castlemist/exportgltf/blend_mode.h"

#include <cstdint>
#include <cstring>

using namespace castlemist::exportgltf;

CM_TEST(vrchat, decode_opaque) {
    BlendInfo a = decode_blend(0x0, true, false, false);
    CHECK(a.preset == BlendPreset::Opaque);
    CHECK(a.exact);
    BlendInfo b = decode_blend(0x0, true, false, true);
    CHECK(b.preset == BlendPreset::Cutout);
    CHECK(b.exact);
    CHECK(b.alphaTest);
}

CM_TEST(vrchat, decode_fade) {
    BlendInfo a = decode_blend(0x6565000, true, false, false);
    CHECK(a.preset == BlendPreset::Fade);
    CHECK(a.exact);
    CHECK_EQ(a.srcRgb, 5);
    CHECK_EQ(a.dstRgb, 6);
    BlendInfo b = decode_blend(0x6565000, true, false, true);
    CHECK(b.preset == BlendPreset::TransClipping);
}

CM_TEST(vrchat, decode_one_invsrccolor) {
    BlendInfo a = decode_blend(0x4242000, true, false, false);
    CHECK(a.preset == BlendPreset::SoftAdditive);
    CHECK_FALSE(a.exact);
    CHECK_EQ(a.srcRgb, 2);
    CHECK_EQ(a.dstRgb, 4);
}

CM_TEST(vrchat, decode_premultiplied) {
    BlendInfo a = decode_blend(0x6262000, true, false, false);
    CHECK(a.preset == BlendPreset::Transparent);
    CHECK(a.exact);
}

CM_TEST(vrchat, decode_additive) {
    BlendInfo a = decode_blend(0x2525000, true, false, false);
    CHECK(a.preset == BlendPreset::Additive);
    CHECK(a.exact);
    BlendInfo b = decode_blend(0x2222000, true, false, false);
    CHECK(b.preset == BlendPreset::Additive);
    CHECK(b.exact);
}

CM_TEST(vrchat, decode_multiply) {
    BlendInfo a = decode_blend(0x1919000, true, false, false);
    CHECK(a.preset == BlendPreset::Multiplicative);
    CHECK(a.exact);
    BlendInfo b = decode_blend(0x3939000, true, false, false);
    CHECK(b.preset == BlendPreset::Multiplicative2x);
    CHECK(b.exact);
}

CM_TEST(vrchat, decode_soft_additive_exact) {
    uint64_t s = (uint64_t(10) << 12) | (uint64_t(2) << 16);
    BlendInfo a = decode_blend(s, true, false, false);
    CHECK(a.preset == BlendPreset::SoftAdditive);
    CHECK(a.exact);
}

CM_TEST(vrchat, decode_revsub) {
    BlendInfo a = decode_blend(0x2222000 | (uint64_t(0x12) << 28), true, false, false);
    CHECK(a.preset == BlendPreset::Custom);
    CHECK(a.nearest == BlendPreset::Multiplicative);
    CHECK_EQ(a.eqRgb, 2);
    CHECK_FALSE(a.exact);
}

CM_TEST(vrchat, decode_unknown_pair) {
    BlendInfo a = decode_blend((uint64_t(7) << 12) | (uint64_t(2) << 16), true, false, false);
    CHECK(a.preset == BlendPreset::Custom);
    CHECK(a.nearest == BlendPreset::Additive);
    CHECK_FALSE(a.exact);
    BlendInfo b = decode_blend((uint64_t(7) << 12) | (uint64_t(6) << 16), true, false, false);
    CHECK(b.preset == BlendPreset::Custom);
    CHECK(b.nearest == BlendPreset::Fade);
}

CM_TEST(vrchat, decode_blend_without_render_state) {
    BlendInfo a = decode_blend(0x6565000, false, true, false);
    CHECK(a.preset == BlendPreset::Additive);
    CHECK(a.exact);
    CHECK_EQ(a.srcRgb, 0);
    CHECK_EQ(a.dstRgb, 0);
    BlendInfo b = decode_blend(0x6565000, false, false, false);
    CHECK(b.preset == BlendPreset::Opaque);
    BlendInfo c = decode_blend(0x6565000, false, false, true);
    CHECK(c.preset == BlendPreset::Cutout);
}

CM_TEST(vrchat, poiyomi_names_and_modes) {
    struct Row { BlendPreset p; const char* name; int mode; };
    const Row rows[] = {
        {BlendPreset::Opaque, "Opaque", 0},
        {BlendPreset::Cutout, "Cutout", 1},
        {BlendPreset::Fade, "Fade", 2},
        {BlendPreset::TransClipping, "TransClipping", 9},
        {BlendPreset::Transparent, "Transparent", 3},
        {BlendPreset::Additive, "Additive", 4},
        {BlendPreset::SoftAdditive, "Soft Additive", 5},
        {BlendPreset::Multiplicative, "Multiplicative", 6},
        {BlendPreset::Multiplicative2x, "2x Multiplicative", 7},
    };
    for (const Row& r : rows) {
        CHECK(std::strcmp(poiyomi_preset_name(r.p), r.name) == 0);
        CHECK_EQ(poiyomi_mode_value(r.p), r.mode);
    }
    CHECK(std::strcmp(poiyomi_preset_name(BlendPreset::Custom), "Custom") == 0);
}

CM_TEST(vrchat, gltf_alpha_modes) {
    auto mode = [](BlendPreset p, BlendPreset nearest) {
        BlendInfo b;
        b.preset = p;
        b.nearest = nearest;
        return gltf_alpha_mode(b);
    };
    CHECK(mode(BlendPreset::Opaque, BlendPreset::Opaque) == GltfAlphaMode::Opaque);
    CHECK(mode(BlendPreset::Cutout, BlendPreset::Cutout) == GltfAlphaMode::Mask);
    CHECK(mode(BlendPreset::TransClipping, BlendPreset::TransClipping) == GltfAlphaMode::Mask);
    CHECK(mode(BlendPreset::Fade, BlendPreset::Fade) == GltfAlphaMode::Blend);
    CHECK(mode(BlendPreset::Additive, BlendPreset::Additive) == GltfAlphaMode::Blend);
    CHECK(mode(BlendPreset::Custom, BlendPreset::Additive) == GltfAlphaMode::Blend);
    CHECK(mode(BlendPreset::Custom, BlendPreset::Opaque) == GltfAlphaMode::Opaque);
}
