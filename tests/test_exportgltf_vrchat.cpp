/// @file
/// @brief Tests for the VRChat (Poiyomi) export helpers: blend decode.

#include "test_framework.h"

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/shader_profiles.h"
#include "castlemist/extract/model_types.h"

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

namespace {
ModelMaterialCPU mat_with_file(uint32_t file) {
    ModelMaterialCPU m;
    m.materialFile = file;
    return m;
}
}  // namespace

CM_TEST(vrchat, profile_by_amat) {
    const ShaderProfile& wg = profile_for(mat_with_file(561567), 0);
    CHECK(wg.name == "weapon-glow");
    CHECK(wg.clips);
    CHECK(wg.diffuseAlpha == AlphaUse::HolesAndShine);

    const ShaderProfile& ws = profile_for(mat_with_file(2348484), 0);
    CHECK(ws.name == "weapon-spec");
    CHECK(ws.specLayer == SpecLayer::GlossInAlpha);
    CHECK(ws.glowOnUv2MaskOnUv0);

    const ShaderProfile& ls = profile_for(mat_with_file(13822), 0);
    CHECK(ls.name == "legacy-spec");
    CHECK_FALSE(ls.clips);
    CHECK(ls.diffuseAlpha == AlphaUse::ReflectionOnly);
    CHECK(ls.specLayer == SpecLayer::ExponentInAlpha);

    const ShaderProfile& lc = profile_for(mat_with_file(14149), 0);
    CHECK(lc.name == "legacy-spec");
    CHECK(lc.clips);
    CHECK(lc.maskGlowGate == Channel::R);

    const ShaderProfile& am = profile_for(mat_with_file(2449347), 0);
    CHECK(am.name == "armor-mask");
    CHECK(am.maskMetal == Channel::R);
    CHECK(am.maskGloss == Channel::G);
    CHECK(am.maskSheen == Channel::B);
    CHECK(am.maskGlow == Channel::A);

    const ShaderProfile& an = profile_for(mat_with_file(1171332), 0);
    CHECK(an.name == "armor-mask-noglowA");
    CHECK(an.maskGlow == Channel::None);

    const ShaderProfile& pn = profile_for(mat_with_file(15999), 0);
    CHECK(pn.name == "prop-lit-noclip");
    CHECK_FALSE(pn.clips);

    const ShaderProfile& jd = profile_for(mat_with_file(2472137), 0);
    CHECK(jd.name == "jade-interior");
    CHECK_FALSE(jd.supported);
    CHECK(jd.diffuseAlpha == AlphaUse::InteriorWeight);

    const ShaderProfile& as = profile_for(mat_with_file(2507831), 0);
    CHECK(as.name == "armor-silk");
    CHECK_FALSE(as.supported);
}

CM_TEST(vrchat, profile_by_untagged_trait) {
    ModelMaterialCPU m = mat_with_file(123456789);
    m.materialId = 0;
    m.materialFlags = 0;
    m.extraTextures.resize(2);  // roles left empty
    const ShaderProfile& p = profile_for(m, 0x6565000);
    CHECK(p.name == "legacy-untagged");
    CHECK_EQ(p.opacityTexture, 2);
}

CM_TEST(vrchat, profile_default_for_unknown) {
    ModelMaterialCPU m = mat_with_file(999999999);
    m.extraTextures.resize(1);
    m.extraTextures[0].role = "mask";
    const ShaderProfile& p = profile_for(m, 0x6565000);
    CHECK(&p == &default_profile());
    CHECK(p.name == "default");
    CHECK_FALSE(p.clips);
    CHECK(p.diffuseAlpha == AlphaUse::Shine);
    CHECK(p.maskMetal == Channel::None);
    CHECK(p.maskGloss == Channel::None);
    CHECK(p.maskSheen == Channel::None);
    CHECK(p.maskGlow == Channel::None);
    CHECK(p.maskGlowGate == Channel::None);
}
