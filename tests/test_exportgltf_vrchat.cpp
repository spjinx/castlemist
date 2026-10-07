/// @file
/// @brief Tests for the VRChat (Poiyomi) export helpers: blend decode, shader
///        profiles, map building, the export folder.

#include "test_framework.h"

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/shader_profiles.h"
#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/vrchat_maps.h"
#include "castlemist/native/granny_anim.hpp"
#include "castlemist/extract/model_types.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

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

CM_TEST(vrchat, profile_by_amat_second_survey) {
    // New profiles (docs/research/gw2-material-channels.md section 8.4).
    const ShaderProfile& ps = profile_for(mat_with_file(1729747), 0);
    CHECK(ps.name == "prop-spec");
    CHECK(ps.supported);
    CHECK(ps.clips);
    CHECK(ps.diffuseAlpha == AlphaUse::HolesAndShine);
    CHECK(ps.specLayer == SpecLayer::GlossInAlpha);
    CHECK_FALSE(ps.glowOnUv2MaskOnUv0);

    const ShaderProfile& uh = profile_for(mat_with_file(77876), 0);
    CHECK(uh.name == "prop-unlit-holes");
    CHECK(uh.clips);
    CHECK(uh.diffuseAlpha == AlphaUse::Unused);

    for (uint32_t id : {77598u, 189570u}) {
        const ShaderProfile& dd = profile_for(mat_with_file(id), 0);
        CHECK(dd.name == "prop-diffuse-only");
        CHECK_FALSE(dd.clips);
        CHECK(dd.diffuseAlpha == AlphaUse::Unused);
    }

    for (uint32_t id : {23507u, 19255u}) {
        const ShaderProfile& fa = profile_for(mat_with_file(id), 0);
        CHECK(fa.name == "fx-alpha");
        CHECK(fa.supported);
        CHECK(fa.diffuseAlpha == AlphaUse::Opacity);
        CHECK_FALSE(fa.clips);
        CHECK_FALSE(fa.premultiplyRgbByAlpha);
    }

    const ShaderProfile& ap = profile_for(mat_with_file(2329259), 0);
    CHECK(ap.name == "armor-prism");
    CHECK(ap.supported);
    CHECK_FALSE(ap.clips);
    CHECK(ap.diffuseAlpha == AlphaUse::Shine);
    CHECK(ap.animatedGlowLayers);
    CHECK(ap.maskMetal == Channel::None);

    const ShaderProfile& cg = profile_for(mat_with_file(511663), 0);
    CHECK(cg.name == "weapon-cutout-glow");
    CHECK(cg.supported);
    CHECK_FALSE(cg.clips);
    CHECK(cg.diffuseAlpha == AlphaUse::Shine);

    for (uint32_t id : {157432u, 3718974u, 15206u}) {
        const ShaderProfile& fm = profile_for(mat_with_file(id), 0);
        CHECK(fm.name == "fx-multiply");
        CHECK_FALSE(fm.supported);
    }

    const ShaderProfile& fc = profile_for(mat_with_file(221571), 0);
    CHECK(fc.name == "fx-cubemap");
    CHECK_FALSE(fc.supported);

    for (uint32_t id : {49659u, 63923u, 57026u}) {
        const ShaderProfile& gr = profile_for(mat_with_file(id), 0);
        CHECK(gr.name == "glass-refract");
        CHECK_FALSE(gr.supported);
    }

    // More ids on existing profiles.
    for (uint32_t id : {20041u, 62080u, 57752u, 16104u, 58654u}) {
        const ShaderProfile& p = profile_for(mat_with_file(id), 0);
        CHECK(p.name == "prop-lit");
        CHECK(p.clips);
        CHECK(p.diffuseAlpha == AlphaUse::HolesAndShine);
    }
    for (uint32_t id : {14084u, 27305u, 231183u, 75778u}) {
        const ShaderProfile& p = profile_for(mat_with_file(id), 0);
        CHECK(p.name == "prop-lit-noclip");
        CHECK_FALSE(p.clips);
    }
    const ShaderProfile& lc = profile_for(mat_with_file(13361), 0);
    CHECK(lc.name == "legacy-spec");
    CHECK(lc.clips);
    CHECK(lc.specLayer == SpecLayer::ExponentInAlpha);
    CHECK(profile_for(mat_with_file(3423592), 0).name == "weapon-spec");
    CHECK(profile_for(mat_with_file(1203843), 0).name == "weapon-glow");
    CHECK(profile_for(mat_with_file(2212806), 0).name == "weapon-glow");
    CHECK(profile_for(mat_with_file(87345), 0).name == "fx-soft-additive");
    CHECK(profile_for(mat_with_file(1053007), 0).premultiplyRgbByAlpha);
    CHECK(profile_for(mat_with_file(48767), 0).name == "fx-premultiplied");
    CHECK(profile_for(mat_with_file(1171330), 0).name == "fx-premultiplied");
    CHECK(profile_for(mat_with_file(977200), 0).name == "fx-fire");
    CHECK_FALSE(profile_for(mat_with_file(1171331), 0).supported);
}

CM_TEST(vrchat, profile_table_has_no_duplicate_amat) {
    std::vector<uint32_t> ids = all_profile_amats();
    CHECK(ids.size() > 100);
    std::sort(ids.begin(), ids.end());
    CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
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

// ---------------------------------------------------------------- map building --

namespace {

ModelTextureCPU tex_from(int w, int h, const std::vector<std::array<uint8_t, 4>>& texels,
                         uint32_t fileId) {
    ModelTextureCPU t;
    t.fileId = fileId;
    t.width = w;
    t.height = h;
    for (const auto& p : texels) t.rgba.insert(t.rgba.end(), p.begin(), p.end());
    return t;
}

ModelTextureCPU solid(int w, int h, std::array<uint8_t, 4> p, uint32_t fileId) {
    return tex_from(w, h, std::vector<std::array<uint8_t, 4>>(static_cast<size_t>(w * h), p),
                    fileId);
}

/// A 2x2 diffuse with the four alpha bands {30, 90, 128, 255}.
ModelTextureCPU banded_diffuse() {
    return tex_from(2, 2,
                    {{100, 100, 100, 30}, {100, 100, 100, 90}, {100, 100, 100, 128},
                     {100, 100, 100, 255}},
                    1000);
}

int add_tex(ModelPreview& m, ModelTextureCPU t) {
    m.textures.push_back(std::move(t));
    return static_cast<int>(m.textures.size()) - 1;
}

void add_layer(ModelPreview& m, ModelMaterialCPU& mat, const char* role, int texIndex,
               uint8_t uv) {
    ModelMaterialCPU::ExtraTexture x;
    x.texIndex = texIndex;
    x.uvIndex = uv;
    x.fileId = texIndex >= 0 ? m.textures[static_cast<size_t>(texIndex)].fileId : 4242;
    x.role = role;
    mat.extraTextures.push_back(x);
}

int px(const ModelTextureCPU& t, int i, int c) {
    return t.rgba[static_cast<size_t>(i * 4 + c)];
}

bool any_warning(const MaterialMaps& m, const char* needle) {
    for (const std::string& w : m.warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

BlendInfo blend_of(BlendPreset p) {
    BlendInfo b;
    b.preset = b.nearest = p;
    return b;
}

MaterialMaps build(const ModelPreview& model, const ModelMaterialCPU& mat, BlendPreset p) {
    return build_material_maps(model, mat, blend_of(p), profile_for(mat, 0));
}

}  // namespace

CM_TEST(vrchat, alpha_bands_with_clipping_profile) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);  // weapon-glow
    mat.diffuseTex = add_tex(model, banded_diffuse());
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.baseColor.present);
    CHECK(m.packed.present);
    const int wantA[4] = {0, 255, 255, 255};
    const int wantG[4] = {0, 0, 0, 255};
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(px(m.baseColor.tex, i, 3), wantA[i]);
        CHECK_NEAR(px(m.packed.tex, i, 1), wantG[i], 1);
        CHECK_EQ(px(m.packed.tex, i, 3), px(m.packed.tex, i, 1));
    }
    CHECK(m.smoothSource == "diffuseAlpha");
    CHECK(m.specularSource == "diffuseAlpha");
}

CM_TEST(vrchat, alpha_bands_without_clipping) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(13822);  // legacy-spec, ReflectionOnly
    mat.diffuseTex = add_tex(model, banded_diffuse());
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    const int wantB[4] = {0, 0, 0, 255};
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(px(m.baseColor.tex, i, 3), 255);
        CHECK_NEAR(px(m.packed.tex, i, 2), wantB[i], 1);
    }
    CHECK(m.reflectionSource == "diffuseAlpha");
}

CM_TEST(vrchat, armor_mask_channels) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2449347);  // armor-mask
    mat.diffuseTex = add_tex(model, solid(8, 8, {120, 60, 30, 255}, 1000));
    add_layer(model, mat, "mask", add_tex(model, solid(2, 2, {200, 100, 50, 30}, 1001)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.packed.present);
    CHECK_EQ(m.packed.tex.width, 8);  // diffuse size, mask nearest-sampled
    CHECK_EQ(px(m.packed.tex, 9, 0), 200);
    CHECK_EQ(px(m.packed.tex, 9, 1), 100);
    CHECK(m.metalSource == "mask.R");
    CHECK(m.smoothSource == "mask.G");
    CHECK(m.emissionMask.present);
    CHECK(m.emissionMask.source == "mask.A");
    CHECK_EQ(px(m.emissionMask.tex, 0, 0), 30);
    CHECK(m.emissionMap.present);  // the base colour
}

CM_TEST(vrchat, weapon_glow_emission) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {128, 128, 128, 255}, 2002)), 2);
    add_layer(model, mat, "glowperturb",
              add_tex(model, solid(2, 2, {128, 128, 255, 255}, 2003)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.emissionMap.present);
    CHECK_EQ(m.emissionMap.uv, 0);
    CHECK_EQ(m.emissionMap.fileId, 2001u);
    CHECK(m.emissionMask.present);
    CHECK_EQ(m.emissionMask.uv, 2);
    CHECK(m.emissionBaked.present);
    CHECK_NEAR(m.emissionColor[0], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[1], 64.0f / 255.0f, 0.001f);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 0), 128, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 1), 32, 1);
    CHECK(m.distortion.present);
    CHECK_EQ(m.distortion.uv, 1);
    CHECK(m.distortion.source == "glowperturb");
    CHECK(m.extras.empty());
}

CM_TEST(vrchat, weapon_spec_swapped_uvs) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2348484);  // weapon-spec
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {0, 200, 255, 255}, 2001)), 2);
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {255, 255, 255, 255}, 2002)), 0);
    add_layer(model, mat, "specular", add_tex(model, solid(2, 2, {90, 90, 90, 77}, 2004)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_EQ(m.emissionMap.uv, 2);
    CHECK_EQ(m.emissionMask.uv, 0);
    CHECK(m.smoothSource == "specular.A");
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.packed.tex, i, 1), 77);
    bool specExtra = false;
    for (const auto& x : m.extras)
        if (x.role == "specular" && x.use == "specular-color" && x.slot.present) specExtra = true;
    CHECK(specExtra);
}

CM_TEST(vrchat, conduct_as_metal) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    mat.namedConstants = {{"conduct", 0.4f}};
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.metalSource == "conduct");
    for (int i = 0; i < 4; ++i) CHECK_NEAR(px(m.packed.tex, i, 0), 102, 1);

    mat.namedConstants = {{"conduct", 1.5f}};
    MaterialMaps hi = build(model, mat, BlendPreset::Cutout);
    CHECK_EQ(px(hi.packed.tex, 0, 0), 255);
}

CM_TEST(vrchat, normal_rebuilt_from_rg) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    mat.normalTex = add_tex(model, solid(2, 2, {255, 128, 0, 0}, 3000));
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.normal.present);
    CHECK_EQ(px(m.normal.tex, 0, 0), 255);
    CHECK_EQ(px(m.normal.tex, 0, 1), 127);  // 255 - 128: green flipped
    CHECK_NEAR(px(m.normal.tex, 0, 2), 128, 1);
    CHECK_EQ(px(m.normal.tex, 0, 3), 255);
}

CM_TEST(vrchat, premultiplied_effect_rgb) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(19092);  // fx-soft-additive
    mat.diffuseTex = add_tex(model, solid(2, 2, {200, 100, 50, 128}, 1000));
    MaterialMaps m = build(model, mat, BlendPreset::SoftAdditive);
    CHECK_NEAR(px(m.baseColor.tex, 0, 0), 100, 1);
    CHECK_NEAR(px(m.baseColor.tex, 0, 1), 50, 1);
    CHECK_NEAR(px(m.baseColor.tex, 0, 2), 25, 1);
    CHECK_EQ(px(m.baseColor.tex, 0, 3), 128);
    CHECK(m.emissionMap.present);  // additive-style with no glow layer: base colour
}

CM_TEST(vrchat, missing_layer_is_skipped_with_warning) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    mat.normalTex = add_tex(model, solid(2, 2, {128, 128, 255, 255}, 3000));
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    add_layer(model, mat, "glowmask", -1, 2);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_FALSE(m.emissionMask.present);
    CHECK(any_warning(m, "glowmask"));
    CHECK(m.baseColor.present);
    CHECK(m.normal.present);
    CHECK(m.packed.present);
    CHECK(m.emissionMap.present);
}

CM_TEST(vrchat, placeholder_texture_becomes_constant) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2449347);  // armor-mask
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "mask", add_tex(model, solid(4, 4, {255, 255, 255, 255}, 13368)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(any_warning(m, "placeholder"));
    CHECK_FALSE(m.emissionMask.present);
    for (const auto& x : m.extras) CHECK(x.slot.fileId != 13368u);
    // The constant still reaches the packed map.
    CHECK_EQ(px(m.packed.tex, 0, 0), 255);
    CHECK(m.metalSource == "mask.R");
}

CM_TEST(vrchat, uv_above_three_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {9, 9, 9, 255}, 2002)), 4);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(any_warning(m, "UV4"));
}

CM_TEST(vrchat, unsupported_profile_exports_layers_raw) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2507831);  // armor-silk
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "mask", add_tex(model, solid(2, 2, {200, 100, 50, 30}, 1001)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(any_warning(m, "armor-silk"));
    CHECK(m.baseColor.present);
    CHECK_EQ(px(m.baseColor.tex, 0, 3), 30);  // raw, not banded
    CHECK_FALSE(m.packed.present);
    CHECK_FALSE(m.emissionMask.present);
    CHECK_EQ(m.extras.size(), size_t{1});
    CHECK(m.extras[0].role == "mask");
}

CM_TEST(vrchat, legacy_untagged_opacity_from_texture) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(185120);  // legacy-untagged
    mat.diffuseTex = add_tex(model, solid(2, 2, {10, 20, 30, 0}, 1000));
    mat.normalTex = add_tex(model, solid(2, 2, {128, 128, 255, 255}, 1001));
    add_tex(model,
            tex_from(2, 2, {{11, 0, 0, 255}, {22, 0, 0, 255}, {33, 0, 0, 255}, {44, 0, 0, 255}},
                     1002));
    mat.textureFileIds = {1000, 1001, 1002};
    MaterialMaps m = build(model, mat, BlendPreset::TransClipping);
    const int want[4] = {11, 22, 33, 44};
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.baseColor.tex, i, 3), want[i]);
}

CM_TEST(vrchat, tints_from_constant_vectors) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    mat.namedConstantVectors = {{"speccp", {0.5f, 0.25f, 0.125f, 32.0f}},
                                {"envcr", {0.1f, 0.2f, 0.3f, 0.9f}}};
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.specularTint.has_value());
    CHECK_NEAR((*m.specularTint)[1], 0.25f, 1e-6f);
    CHECK(m.reflectionTint.has_value());
    CHECK_NEAR((*m.reflectionTint)[2], 0.3f, 1e-6f);
    CHECK(m.reflectionSource == "envcr");
    CHECK_EQ(px(m.packed.tex, 0, 2), 255);
}

CM_TEST(vrchat, placeholder_glow_with_real_glowmask) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(4, 4, {255, 128, 0, 255}, 958179)), 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {200, 200, 200, 255}, 2002)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_FALSE(m.emissionMap.present);  // no base-colour substitute
    CHECK_NEAR(m.emissionColor[0], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[1], 128.0f / 255.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[2], 0.0f, 0.001f);
    CHECK(m.emissionMask.present);
    CHECK(m.emissionBaked.present);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 0), 200, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 1), 100, 1);
    CHECK(any_warning(m, "placeholder"));
}

CM_TEST(vrchat, failed_glow_does_not_fall_back_to_base_color) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", -1, 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {200, 200, 200, 255}, 2002)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Additive);
    CHECK_FALSE(m.emissionMap.present);
    CHECK(any_warning(m, "glow layer"));
    CHECK_NEAR(m.emissionColor[0], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[1], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[2], 1.0f, 0.001f);
    CHECK(m.emissionMask.present);
}

CM_TEST(vrchat, black_placeholder_glowmask_means_no_emission) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(4, 4, {0, 0, 0, 255}, 27338)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_FALSE(m.emissionMap.present);
    CHECK_FALSE(m.emissionMask.present);
    CHECK_FALSE(m.emissionBaked.present);
    CHECK(any_warning(m, "black placeholder"));
}

CM_TEST(vrchat, uniform_placeholder_glowmask_scales_emission) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    add_layer(model, mat, "glowmask", add_tex(model, solid(4, 4, {128, 128, 128, 255}, 4444)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.emissionMap.present);
    CHECK_FALSE(m.emissionMask.present);
    CHECK_NEAR(m.emissionColor[0], 128.0f / 255.0f, 0.001f);
    CHECK(m.emissionBaked.present);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 0), 128, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 1), 32, 1);
}

CM_TEST(vrchat, legendary_animated_glow_layers_are_raw_extras) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2083141);  // weapon-glow-legendary
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glowmask", add_tex(model, solid(2, 2, {200, 200, 200, 255}, 2002)), 2);
    add_layer(model, mat, "glowfringe", add_tex(model, solid(2, 2, {1, 2, 3, 255}, 2005)), 0);
    add_layer(model, mat, "ramp", add_tex(model, solid(2, 2, {4, 5, 6, 255}, 2006)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(profile_for(mat, 0).animatedGlowLayers);
    bool fringe = false, ramp = false;
    for (const auto& x : m.extras) {
        if (x.role == "glowfringe" && x.slot.present) fringe = true;
        if (x.role == "ramp" && x.slot.present) ramp = true;
    }
    CHECK(fringe);
    CHECK(ramp);
    CHECK(any_warning(m, "animated legendary glow"));
}

CM_TEST(vrchat, default_profile_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(999999999);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(any_warning(m, "default profile"));
    MaterialMaps w = build(model, mat_with_file(561567), BlendPreset::Opaque);
    CHECK_FALSE(any_warning(w, "default profile"));
}

// M5: one default-profile warning that also says the clip is unknown.
CM_TEST(vrchat, default_profile_warning_names_clip_unknown) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(999999999);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    size_t n = 0;
    for (const std::string& w : m.warnings)
        if (w.find("default profile used") != std::string::npos) {
            ++n;
            CHECK(w.find("clip unknown") != std::string::npos);
        }
    CHECK_EQ(n, size_t{1});
}

// I1 / R8: on the default profile a uniform diffuse alpha carries no shine.
CM_TEST(vrchat, default_profile_uniform_alpha_is_unused) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(999999999);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 255}, 1000));
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.packed.present);
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(px(m.packed.tex, i, 1), 128);
        CHECK_EQ(px(m.packed.tex, i, 3), 255);
    }
    CHECK(m.smoothSource == "default");
    CHECK(m.specularSource == "default");
    CHECK(any_warning(m, "diffuse alpha is uniform (255): read as unused, not shine (default profile)"));

    mat.namedConstants = {{"specstr", 0.3f}};
    MaterialMaps s = build(model, mat, BlendPreset::Opaque);
    for (int i = 0; i < 4; ++i) CHECK_NEAR(px(s.packed.tex, i, 1), 77, 1);
    CHECK(s.smoothSource == "specstr");

    // A profiled shader keeps alpha 255 as full shine.
    ModelMaterialCPU wg = mat_with_file(561567);
    wg.diffuseTex = mat.diffuseTex;
    MaterialMaps p = build(model, wg, BlendPreset::Cutout);
    CHECK_EQ(px(p.packed.tex, 0, 1), 255);
    CHECK(p.smoothSource == "diffuseAlpha");
    CHECK_FALSE(any_warning(p, "uniform"));
}

// I4: the default profile on a blended preset keeps the alpha as opacity.
CM_TEST(vrchat, default_profile_blended_keeps_alpha_as_opacity) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(999999999);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    const int wantA[4] = {30, 90, 128, 255};
    for (int i = 0; i < 4; ++i) {
        CHECK_EQ(px(m.baseColor.tex, i, 0), 100);  // no premultiply
        CHECK_EQ(px(m.baseColor.tex, i, 3), wantA[i]);
        CHECK_EQ(px(m.packed.tex, i, 1), 128);
        CHECK_EQ(px(m.packed.tex, i, 3), 255);
    }
    CHECK(m.smoothSource == "default");
    CHECK(any_warning(m, "default profile on a blended preset: diffuse alpha kept as opacity, not shine"));
}

// I3: legacy-spec reads smoothness from the specular layer's alpha (exponent/128).
CM_TEST(vrchat, legacy_spec_specular_alpha_feeds_smoothness) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(13822);  // legacy-spec
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "specular",
              add_tex(model, tex_from(2, 2, {{90, 90, 90, 0}, {90, 90, 90, 85}, {90, 90, 90, 170},
                                             {90, 90, 90, 255}},
                                      2004)),
              0);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    const int wantG[4] = {0, 85, 170, 255};
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.packed.tex, i, 1), wantG[i]);
    CHECK(m.smoothSource == "specular.A (exponent/128)");
    bool specExtra = false;
    for (const auto& x : m.extras)
        if (x.role == "specular" && x.use == "specular-color" && x.slot.present) specExtra = true;
    CHECK(specExtra);
}

// M1: a mask / specular layer on another UV than the diffuse is named.
CM_TEST(vrchat, packed_layer_uv_differs_from_diffuse_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(2449347);  // armor-mask
    mat.diffuseTex = add_tex(model, solid(2, 2, {120, 60, 30, 255}, 1000));
    add_layer(model, mat, "mask", add_tex(model, solid(2, 2, {200, 100, 50, 30}, 1001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(any_warning(m, "mask layer uses UV1, the diffuse UV0"));

    ModelMaterialCPU ws = mat_with_file(2348484);  // weapon-spec
    ws.diffuseTex = mat.diffuseTex;
    add_layer(model, ws, "specular", add_tex(model, solid(2, 2, {90, 90, 90, 77}, 2004)), 2);
    MaterialMaps s = build(model, ws, BlendPreset::Cutout);
    CHECK(any_warning(s, "specular layer uses UV2, the diffuse UV0"));

    ModelMaterialCPU same = mat_with_file(2449347);
    same.diffuseTex = mat.diffuseTex;
    add_layer(model, same, "mask", 1, 0);
    CHECK_FALSE(any_warning(build(model, same, BlendPreset::Cutout), "the diffuse UV"));
}

// I2: a supported material with no diffuse says so.
CM_TEST(vrchat, missing_diffuse_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(561567);
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_FALSE(m.baseColor.present);
    CHECK(any_warning(m, "no diffuse texture: BaseColor left out"));

    ModelMaterialCPU silk = mat_with_file(2507831);  // unsupported: raw, no such warning
    CHECK_FALSE(any_warning(build(model, silk, BlendPreset::Cutout), "no diffuse texture"));
}

// ------------------------------------------------------------- export folder --

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path fresh_dir(const char* label) {
    fs::path dir = fs::temp_directory_path() / (std::string("cm_vrchat_") + label);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return json::parse(in);
}

/// The JSON chunk of a .glb.
json glb_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t i) {
        return static_cast<uint32_t>(b[i]) | (static_cast<uint32_t>(b[i + 1]) << 8) |
               (static_cast<uint32_t>(b[i + 2]) << 16) | (static_cast<uint32_t>(b[i + 3]) << 24);
    };
    if (b.size() < 20) return json();
    const uint32_t len = u32(12);
    return json::parse(std::string(reinterpret_cast<const char*>(&b[20]), len));
}

GVertex vtx(float x, float y, float u, float v) {
    GVertex g{};
    g.px = x; g.py = y; g.pz = 0;
    g.nz = 1; g.tx = 1; g.by = 1;
    g.u = u; g.v = v;
    return g;
}

ModelMeshCPU quad_mesh(uint32_t materialIndex) {
    ModelMeshCPU m;
    m.vertices = {vtx(0, 0, 0, 0), vtx(1, 0, 1, 0), vtx(1, 1, 1, 1), vtx(0, 1, 0, 1)};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.vertexCount = 4;
    m.hasTangents = true;
    m.materialIndex = materialIndex;
    return m;
}

/// A quad drawn with one weapon-glow material: banded diffuse, normal, glow.
ModelPreview glow_quad() {
    ModelPreview model;
    model.meshes.push_back(quad_mesh(0));
    ModelMaterialCPU mat = mat_with_file(561567);
    mat.materialName = "Blade";
    mat.hasRenderState = true;
    mat.renderState = 0;  // no blend: Opaque, Cutout with the clipping profile
    mat.diffuseTex = add_tex(model, banded_diffuse());
    mat.normalTex = add_tex(model, solid(2, 2, {128, 128, 255, 255}, 3000));
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    mat.namedConstants = {{"specstr", 0.5f}, {"glofade", 0.7f}, {"gloptrb", 0.4f}};
    model.materials.push_back(mat);
    castlemist::granny::Anim clip;
    clip.name = "StowedA";
    model.animClips.push_back(clip);
    return model;
}

const json* material_named(const json& doc, const std::string& name) {
    for (const json& m : doc["materials"])
        if (m["name"] == name) return &m;
    return nullptr;
}

bool any_of_warnings(const std::vector<std::string>& ws, const char* needle) {
    for (const std::string& w : ws)
        if (w.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

CM_TEST(vrchat, folder_layout) {
    ModelPreview model = glow_quad();
    fs::path dir = fresh_dir("layout");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1766522);
    CHECK(r.ok);
    CHECK(fs::exists(dir / "Dagger.glb"));
    CHECK(fs::exists(dir / "materials.json"));
    CHECK(fs::exists(dir / "Textures" / "Blade - BaseColor.png"));
    CHECK(fs::exists(dir / "Textures" / "Blade - Normal.png"));
    CHECK(fs::exists(dir / "Textures" / "Blade - Packed.png"));
    CHECK(fs::exists(dir / "Textures" / "Blade - EmissionMap.png"));
    CHECK_EQ(r.materials, size_t{1});
    CHECK_EQ(r.clips, size_t{1});
    CHECK(fs::path(r.glb) == dir / "Dagger.glb");
    json doc = read_json(dir / "materials.json");
    const json& maps = doc["materials"][0]["maps"];
    CHECK(maps["baseColor"]["file"] == "Textures/Blade - BaseColor.png");
    CHECK(maps["normal"]["file"] == "Textures/Blade - Normal.png");
    CHECK(maps["packed"]["file"] == "Textures/Blade - Packed.png");
    CHECK(maps["emissionMap"]["file"] == "Textures/Blade - EmissionMap.png");
    for (const char* key : {"baseColor", "normal", "packed", "emissionMap"})
        CHECK(fs::exists(dir / fs::path(maps[key]["file"].get<std::string>())));
}

CM_TEST(vrchat, materials_json_fields) {
    ModelPreview model = glow_quad();
    // A second material on the default profile (not clipping) with a scroll.
    ModelMaterialCPU plain = mat_with_file(999999999);
    plain.index = 1;
    plain.materialName = "Plain";
    plain.hasRenderState = true;
    plain.renderState = 0x6565000;  // SrcAlpha / InvSrcAlpha
    plain.diffuseTex = add_tex(model, solid(2, 2, {9, 9, 9, 255}, 5000));
    plain.namedConstants = {{"intscru", 0.25f}, {"intscrv", -1.5f}};
    add_layer(model, plain, "glow", add_tex(model, solid(2, 2, {0, 64, 255, 255}, 5001)), 0);
    model.materials.push_back(plain);
    model.meshes.push_back(quad_mesh(1));

    model.materials[0].sortLayer = 3;
    model.materials[0].sortOrder = 7;

    fs::path dir = fresh_dir("fields");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1766522);
    CHECK(r.ok);
    json doc = read_json(dir / "materials.json");
    CHECK(doc["model"] == 1766522);
    CHECK(doc["poiyomi"] == "10");
    CHECK(doc["castlemist"].is_string());
    CHECK(doc["animations"] == json::array({"StowedA"}));
    CHECK(doc["particles"].is_null());
    CHECK(r.particlesJson.empty());

    const json& blade = *material_named(doc, "Blade");
    CHECK(blade["amat"] == 561567);
    CHECK(blade["profile"] == "weapon-glow");
    CHECK(blade["preset"] == "Cutout");
    CHECK(blade["renderPreset"] == "Cutout");
    CHECK(blade["mode"] == 1);
    CHECK(blade["exact"] == true);
    CHECK(blade["usedByMeshes"] == true);
    CHECK(blade["cull"] == "Back");
    CHECK_NEAR(blade["alphaCutoff"].get<double>(), 0.25, 1e-9);
    CHECK(blade["alphaCutoffIsDefault"] == false);  // the clipping shader's own threshold
    CHECK(blade["renderQueueOffset"] == 3);  // the fixture's sortLayer
    CHECK(blade["sortOrder"] == 7);
    CHECK(blade["gw2"].size() == 3);
    CHECK_NEAR(blade["gw2"]["glofade"].get<double>(), 0.7, 1e-6);
    CHECK_NEAR(blade["gw2"]["gloptrb"].get<double>(), 0.4, 1e-6);
    CHECK_NEAR(blade["gw2"]["specstr"].get<double>(), 0.5, 1e-6);
    CHECK(blade["maps"]["packed"]["sources"]["smooth"] == "diffuseAlpha");
    CHECK(blade["maps"]["baseColor"]["source"] == "diffuse");
    CHECK(blade["maps"]["baseColor"]["fileId"] == 1000);
    CHECK(blade["maps"]["emissionMap"]["panning"].is_null());
    CHECK(blade["maps"]["distortion"].is_null());
    CHECK(blade["emission"]["color"].size() == 3);

    const json& p = *material_named(doc, "Plain");
    CHECK(p["profile"] == "default");
    CHECK(p["preset"] == "Fade");
    CHECK(p["mode"] == 2);
    CHECK(p["exact"] == true);
    CHECK(p["blend"]["srcRgb"] == 5);
    CHECK(p["blend"]["dstRgb"] == 6);
    CHECK(p["blend"]["eqRgb"] == 0);
    CHECK(p["blend"]["eqA"] == 0);
    CHECK_NEAR(p["alphaCutoff"].get<double>(), 0.25, 1e-9);
    CHECK(p["alphaCutoffIsDefault"] == true);
    CHECK(p["gw2"].size() == 2);
    CHECK_NEAR(p["gw2"]["intscru"].get<double>(), 0.25, 1e-6);
    const json& pan = p["maps"]["emissionMap"]["panning"];
    CHECK(pan["unit"] == "gw2-raw");
    CHECK_NEAR(pan["u"].get<double>(), 0.25, 1e-6);
    CHECK_NEAR(pan["v"].get<double>(), -1.5, 1e-6);
    CHECK(any_of_warnings(r.warnings, "Plain: default profile"));
    // M5: one warning for the default profile, naming the unknown clip too.
    size_t plainDefault = 0;
    for (const std::string& w : r.warnings)
        if (w.rfind("Plain: ", 0) == 0 &&
            (w.find("default profile used") != std::string::npos ||
             w.find("clip unknown") != std::string::npos)) {
            ++plainDefault;
            CHECK(w.find("clip unknown") != std::string::npos);
        }
    CHECK_EQ(plainDefault, size_t{1});
}

CM_TEST(vrchat, distortion_strength_and_missing_packed) {
    ModelPreview model = glow_quad();
    add_layer(model, model.materials[0], "glowperturb",
              add_tex(model, solid(2, 2, {128, 128, 255, 255}, 2003)), 1);
    // An effect with no diffuse: nothing to pack.
    ModelMaterialCPU fx = mat_with_file(999999999);
    fx.index = 1;
    fx.materialName = "Fx";
    fx.isEffect = true;
    model.materials.push_back(fx);
    fs::path dir = fresh_dir("distort");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1);
    CHECK(r.ok);
    json doc = read_json(dir / "materials.json");
    const json& blade = *material_named(doc, "Blade");
    CHECK(blade["maps"]["distortion"]["file"] == "Textures/Blade - Distortion.png");
    CHECK_NEAR(blade["maps"]["distortion"]["strength"].get<double>(), 0.4, 1e-6);
    CHECK(fs::exists(dir / "Textures" / "Blade - Distortion.png"));
    const json& f = *material_named(doc, "Fx");
    CHECK(f["maps"]["packed"].is_null());
    CHECK(f["cull"] == "Off");
    CHECK_FALSE(fs::exists(dir / "Textures" / "Fx - Packed.png"));
}

CM_TEST(vrchat, zero_triangle_material) {
    ModelPreview model = glow_quad();
    ModelMaterialCPU spare = model.materials[0];
    spare.index = 1;
    spare.materialName = "Spare";
    model.materials.push_back(spare);  // no mesh draws with it
    fs::path dir = fresh_dir("zero_tri");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1);
    CHECK(r.ok);
    CHECK_EQ(r.materials, size_t{2});
    json doc = read_json(dir / "materials.json");
    CHECK(material_named(doc, "Blade")->at("usedByMeshes") == true);
    const json& s = *material_named(doc, "Spare");
    CHECK(s["usedByMeshes"] == false);
    CHECK(s["maps"]["baseColor"]["file"] == "Textures/Spare - BaseColor.png");
    CHECK(fs::exists(dir / "Textures" / "Spare - BaseColor.png"));
}

CM_TEST(vrchat, material_file_names_are_unique_and_legal) {
    ModelPreview model = glow_quad();
    model.materials[0].materialName = "A:B";
    ModelMaterialCPU second = model.materials[0];
    second.index = 1;
    second.materialName = "A?B";
    model.materials.push_back(second);
    ModelMaterialCPU third = model.materials[0];
    third.index = 2;
    third.materialName = "\xC3\xA9p\xC3\xA9" "e";  // "epee" with accents: non-ASCII stays
    model.materials.push_back(third);
    ModelMaterialCPU fourth = model.materials[0];
    fourth.index = 3;
    fourth.materialName = "a_b";  // collides with "A_B" on a case-insensitive disk
    model.materials.push_back(fourth);

    fs::path dir = fresh_dir("names");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dag/ger", 1);
    CHECK(r.ok);
    CHECK(fs::exists(dir / "Dag_ger.glb"));
    json doc = read_json(dir / "materials.json");
    CHECK_EQ(doc["materials"].size(), size_t{4});
    std::vector<std::string> files;
    for (const json& m : doc["materials"]) {
        const std::string f = m["maps"]["baseColor"]["file"].get<std::string>();
        files.push_back(f);
        const std::string leaf = f.substr(std::string("Textures/").size());
        for (char c : std::string("/\\:*?\"<>|")) CHECK(leaf.find(c) == std::string::npos);
        CHECK(fs::exists(dir / fs::path(std::u8string(f.begin(), f.end()))));
    }
    for (size_t i = 0; i < files.size(); ++i)
        for (size_t j = i + 1; j < files.size(); ++j) {
            std::string a = files[i], b = files[j];
            for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            CHECK(a != b);
        }
    CHECK(doc["materials"][0]["name"] == "A:B");  // the real name is kept in the JSON

    CHECK(safe_file_name("") == "_");
    CHECK(safe_file_name("  . ") == "_");
    CHECK(safe_file_name(" a|b ") == "a_b");
    CHECK(safe_file_name("CON") != "CON");
}

CM_TEST(vrchat, folder_for_material_without_game_shader) {
    ModelPreview model = glow_quad();
    ModelMaterialCPU& m = model.materials[0];
    m.materialFile = 0;
    m.materialName.clear();
    m.hasRenderState = false;
    m.isEffect = true;
    fs::path dir = fresh_dir("no_shader");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Prop", 0);
    CHECK(r.ok);
    json doc = read_json(dir / "materials.json");
    const json& j = doc["materials"][0];
    CHECK(j["name"] == "Mat_0");
    CHECK(j["amat"].is_null());
    CHECK(j["preset"] == "Additive");
    CHECK(j["mode"] == 4);
    CHECK(j["profile"] == "default");
    CHECK(j["blend"].is_null());  // no game blend word: no table of zeros
    CHECK(any_of_warnings(r.warnings, "Mat_0: no game shader"));
    CHECK(any_of_warnings(r.warnings, "Mat_0: default profile"));
    CHECK(fs::exists(dir / "Prop.glb"));
    CHECK(fs::exists(dir / "materials.json"));
    CHECK(fs::exists(dir / "Textures" / "Mat_0 - BaseColor.png"));
}

CM_TEST(vrchat, empty_model_is_refused_before_writing) {
    ModelPreview model;
    fs::path dir = fresh_dir("empty");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Empty", 0);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.error.empty());
    CHECK_FALSE(fs::exists(dir));
}

CM_TEST(vrchat, gltf_alpha_mode_from_profile) {
    ModelPreview model = glow_quad();
    model.textures[static_cast<size_t>(model.materials[0].diffuseTex)].hasCutout = true;
    fs::path dir = fresh_dir("alpha");
    fs::create_directories(dir);
    CHECK(export_model_gltf(model, (dir / "a.glb").string()).ok);
    json g = glb_json(dir / "a.glb");
    const json& wm = g["materials"][0];
    CHECK(wm["alphaMode"] == "MASK");
    CHECK_NEAR(wm["alphaCutoff"].get<double>(), 0.25, 1e-9);
    // The packed map rides as metallicRoughness (G = 1 - smooth, B = metal).
    CHECK(wm["pbrMetallicRoughness"].contains("metallicRoughnessTexture"));

    model.materials[0].materialFile = 999999999;  // default profile: never clips
    CHECK(export_model_gltf(model, (dir / "b.glb").string()).ok);
    json h = glb_json(dir / "b.glb");
    CHECK(h["materials"][0].value("alphaMode", std::string("OPAQUE")) == "OPAQUE");
    CHECK_FALSE(h["materials"][0].contains("alphaCutoff"));
}

CM_TEST(vrchat, gltf_alpha_mask_kept_for_baked_textures_without_game_shader) {
    // Character pieces, map props and baked atlases carry no game shader: their
    // diffuse alpha is real coverage (hair cards, foliage), so the texture's own
    // hasCutout still makes the .glb material MASK, as before the profiles.
    ModelPreview model = glow_quad();
    model.materials[0].hasRenderState = false;
    model.materials[0].materialFile = 0;
    model.textures[static_cast<size_t>(model.materials[0].diffuseTex)].hasCutout = true;
    fs::path dir = fresh_dir("alpha_legacy");
    fs::create_directories(dir);
    CHECK(export_model_gltf(model, (dir / "a.glb").string()).ok);
    json g = glb_json(dir / "a.glb");
    CHECK(g["materials"][0]["alphaMode"] == "MASK");
    CHECK_NEAR(g["materials"][0]["alphaCutoff"].get<double>(), 0.25, 1e-9);
    // Their alpha is not shine either: no packed map, the glb stays matte.
    CHECK_FALSE(g["materials"][0]["pbrMetallicRoughness"].contains("metallicRoughnessTexture"));

    model.textures[static_cast<size_t>(model.materials[0].diffuseTex)].hasCutout = false;
    CHECK(export_model_gltf(model, (dir / "b.glb").string()).ok);
    CHECK(glb_json(dir / "b.glb")["materials"][0]["alphaMode"] == "OPAQUE");

    // An effect with a cutout (e.g. a map-path DXT1a flagged as effect): MASK
    // still wins over BLEND, as before the profiles.
    model.materials[0].isEffect = true;
    model.textures[static_cast<size_t>(model.materials[0].diffuseTex)].hasCutout = true;
    CHECK(export_model_gltf(model, (dir / "c.glb").string()).ok);
    json c = glb_json(dir / "c.glb");
    CHECK(c["materials"][0]["alphaMode"] == "MASK");
    CHECK_NEAR(c["materials"][0]["alphaCutoff"].get<double>(), 0.25, 1e-9);
    model.textures[static_cast<size_t>(model.materials[0].diffuseTex)].hasCutout = false;
    CHECK(export_model_gltf(model, (dir / "d.glb").string()).ok);
    CHECK(glb_json(dir / "d.glb")["materials"][0]["alphaMode"] == "BLEND");
}
