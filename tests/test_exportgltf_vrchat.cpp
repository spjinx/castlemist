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

CM_TEST(vrchat, profile_decal_modes) {
    // docs/research/gw2-material-channels.md section 8: prop-decal, 57806, 57131; section 4: 54632.
    for (uint32_t id : {56533u, 60027u, 62212u, 44479u, 60145u, 19910u, 525886u, 69887u, 69913u,
                        79884u, 60530u, 69713u, 2597095u}) {
        const ShaderProfile& p = profile_for(mat_with_file(id), 0);
        CHECK(p.name == "prop-decal");
        CHECK(p.supported);
        CHECK_FALSE(p.clips);
        CHECK(p.diffuseAlpha == AlphaUse::Shine);
        CHECK(p.decalMode == DecalMode::DecalOverDiffuse);
        CHECK(p.decalGlow == DecalGlow::None);
    }
    for (uint32_t id : {19910u, 525886u}) {
        const ShaderProfile& p = profile_for(mat_with_file(id), 0);
        CHECK(p.decalMaskRole == "decalmask");
        CHECK(p.decalMaskChannel == Channel::R);
    }
    CHECK(profile_for(mat_with_file(69887), 0).decalMaskRole == "mask");
    CHECK(profile_for(mat_with_file(69887), 0).decalMaskChannel == Channel::R);
    CHECK(profile_for(mat_with_file(60530), 0).decalMaskRole == "blend");
    CHECK(profile_for(mat_with_file(60530), 0).decalMaskChannel == Channel::G);
    CHECK(profile_for(mat_with_file(60027), 0).decalMaskRole.empty());
    CHECK(profile_for(mat_with_file(60027), 0).decalMaskChannel == Channel::None);

    const ShaderProfile& g = profile_for(mat_with_file(57806), 0);
    CHECK(g.name == "prop-decal");
    CHECK(g.decalMode == DecalMode::DecalOverDiffuse);
    CHECK(g.decalGlow == DecalGlow::AboveHalf);
    CHECK(g.diffuseAlpha == AlphaUse::Shine);

    const ShaderProfile& dg = profile_for(mat_with_file(57131), 0);
    CHECK(dg.name == "decal-glow");
    CHECK(dg.supported);
    CHECK_FALSE(dg.clips);
    CHECK(dg.diffuseAlpha == AlphaUse::Shine);
    CHECK(dg.decalMode == DecalMode::DiffuseOverDecal);
    CHECK(dg.decalGlow == DecalGlow::BelowHalf);

    const ShaderProfile& sd = profile_for(mat_with_file(54632), 0);
    CHECK(sd.name == "subsurface-decal");
    CHECK(sd.decalMode == DecalMode::DiffuseOverDecal);
    CHECK(sd.decalGlow == DecalGlow::None);

    // Signature-only ids stay out of the table.
    for (uint32_t id : {76643u, 81309u, 62170u, 69623u})
        CHECK(&profile_for(mat_with_file(id), 0) == &default_profile());
    CHECK(default_profile().decalMode == DecalMode::None);
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

CM_TEST(vrchat, mask_role_reads_named_layer) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(3121953);  // prop-metalmask
    mat.diffuseTex = add_tex(model, solid(8, 8, {120, 60, 30, 255}, 1000));
    add_layer(model, mat, "metalmask", add_tex(model, solid(2, 2, {10, 180, 50, 30}, 1001)), 0);
    add_layer(model, mat, "mask", add_tex(model, solid(2, 2, {1, 2, 3, 4}, 1002)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(profile_for(mat, 0).name == "prop-metalmask");
    CHECK(m.packed.present);
    CHECK_EQ(px(m.packed.tex, 9, 0), 180);
    CHECK(m.metalSource == "metalmask.G");

    // A hand-built profile reading metalmask.G: same result, metalmask is no extra.
    ShaderProfile p;
    p.name = "t";
    p.maskRole = "metalmask";
    p.maskMetal = Channel::G;
    ModelPreview model2;
    ModelMaterialCPU mat2 = mat_with_file(1);
    mat2.diffuseTex = add_tex(model2, solid(8, 8, {120, 60, 30, 255}, 1000));
    add_layer(model2, mat2, "metalmask", add_tex(model2, solid(2, 2, {10, 180, 50, 30}, 1001)), 0);
    MaterialMaps m2 = build_material_maps(model2, mat2, blend_of(BlendPreset::Opaque), p);
    CHECK_EQ(px(m2.packed.tex, 9, 0), 180);
    CHECK(m2.metalSource == "metalmask.G");
    for (const auto& e : m2.extras) CHECK(e.role != "metalmask");
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

// --------------------------------------------------------------------- decals --

namespace {

/// One texel per alpha in `alphas`, colour {200, 100, 50}.
ModelTextureCPU decal_strip(std::initializer_list<uint8_t> alphas, uint32_t fileId) {
    std::vector<std::array<uint8_t, 4>> t;
    for (uint8_t a : alphas) t.push_back({200, 100, 50, a});
    return tex_from(static_cast<int>(t.size()), 1, t, fileId);
}

bool has_extra(const MaterialMaps& m, const char* role) {
    for (const auto& x : m.extras)
        if (x.role == role) return true;
    return false;
}

size_t count_warnings(const MaterialMaps& m, const char* needle) {
    size_t n = 0;
    for (const std::string& w : m.warnings)
        if (w.find(needle) != std::string::npos) ++n;
    return n;
}

}  // namespace

CM_TEST(vrchat, decal_over_diffuse_coverage) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(60027);  // prop-decal
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 64, 128, 255}, 7001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.present);
    CHECK_EQ(m.decal.uv, 1);
    CHECK_EQ(m.decal.fileId, 7001u);
    CHECK(m.decalMode == "decal-over-diffuse");
    CHECK(m.decal.source == "decal saturate(2a)");
    const int wantA[4] = {0, 128, 255, 255};
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(px(m.decal.tex, i, 3), wantA[i], 1);
        CHECK_EQ(px(m.decal.tex, i, 0), 200);
        CHECK_EQ(px(m.decal.tex, i, 1), 100);
        CHECK_EQ(px(m.decal.tex, i, 2), 50);
    }
    CHECK_FALSE(has_extra(m, "decal"));
    CHECK_EQ(count_warnings(m, "decal shine not mapped (decal is on UV1)"), size_t{1});
    // The decal is not baked into BaseColor and Packed keeps the diffuse shine.
    CHECK_EQ(px(m.baseColor.tex, 0, 0), 100);
    CHECK(m.smoothSource == "diffuseAlpha");
    CHECK_FALSE(m.emissionMap.present);
}

CM_TEST(vrchat, decal_mask_on_same_uv_multiplies) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(19910);  // prop-decal, decalmask.R
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({255, 255, 255, 64}, 7001)), 1);
    add_layer(model, mat, "decalmask",
              add_tex(model, tex_from(4, 1, {{255, 0, 0, 255}, {128, 0, 0, 255}, {0, 0, 0, 255},
                                             {255, 0, 0, 255}}, 7002)),
              1);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.present);
    CHECK(m.decal.source == "decal saturate(2a) x decalmask.R");
    const int wantA[4] = {255, 128, 0, 128};
    for (int i = 0; i < 4; ++i) CHECK_NEAR(px(m.decal.tex, i, 3), wantA[i], 1);
    CHECK_FALSE(has_extra(m, "decal"));
    CHECK_FALSE(has_extra(m, "decalmask"));

    // 60530 reads blend.G.
    ModelMaterialCPU b = mat_with_file(60530);
    b.diffuseTex = mat.diffuseTex;
    add_layer(model, b, "decal", add_tex(model, decal_strip({255, 255}, 7003)), 1);
    add_layer(model, b, "blend",
              add_tex(model, tex_from(2, 1, {{0, 255, 0, 255}, {255, 0, 0, 255}}, 7004)), 1);
    MaterialMaps mb = build(model, b, BlendPreset::Opaque);
    CHECK(mb.decal.source == "decal saturate(2a) x blend.G");
    CHECK_NEAR(px(mb.decal.tex, 0, 3), 255, 1);
    CHECK_NEAR(px(mb.decal.tex, 1, 3), 0, 1);
}

CM_TEST(vrchat, decal_mask_on_other_uv_is_its_own_map) {
    // 19910's PS samples the decalmask at TEXCOORD2 and the decal at TEXCOORD1:
    // the mask cannot be folded into the decal's alpha, so it ships as its own map.
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(19910);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({255, 255}, 7001)), 1);
    add_layer(model, mat, "decalmask",
              add_tex(model, tex_from(2, 1, {{40, 90, 0, 255}, {200, 10, 0, 255}}, 7002)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.present);
    CHECK(m.decal.source == "decal saturate(2a) (x decalMask on UV2)");
    CHECK_NEAR(px(m.decal.tex, 0, 3), 255, 1);  // not multiplied in
    CHECK(m.decalMask.present);
    CHECK_EQ(m.decalMask.uv, 2);
    CHECK_EQ(m.decalMask.fileId, 7002u);
    CHECK(m.decalMask.source == "decalmask.R");
    CHECK(m.decalMaskChannel == "R");
    CHECK_EQ(px(m.decalMask.tex, 0, 0), 40);
    CHECK_EQ(px(m.decalMask.tex, 1, 0), 200);
    CHECK_EQ(px(m.decalMask.tex, 1, 1), 200);  // greyscale
    CHECK(any_warning(m, "decal coverage must be multiplied by maps.decalMask (UV2)"));
    CHECK_FALSE(has_extra(m, "decalmask"));
}

CM_TEST(vrchat, decal_placeholder_mask_multiplies_on_any_uv) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(19910);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({255, 64}, 7001)), 1);
    add_layer(model, mat, "decalmask", add_tex(model, solid(4, 4, {128, 0, 0, 255}, 7002)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.source == "decal saturate(2a) x decalmask.R");
    CHECK_NEAR(px(m.decal.tex, 0, 3), 128, 1);
    CHECK_NEAR(px(m.decal.tex, 1, 3), 64, 1);
    CHECK_FALSE(m.decalMask.present);
    CHECK_FALSE(any_warning(m, "maps.decalMask"));
}

CM_TEST(vrchat, decal_mask_specular_b_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(69887);  // prop-decal, mask.R; specular x mask.B
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({255, 255}, 7001)), 1);
    add_layer(model, mat, "mask", add_tex(model, decal_strip({255, 255}, 7002)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.source == "decal saturate(2a) x mask.R");
    CHECK_EQ(count_warnings(m, "specular x mask.B not mapped"), size_t{1});
    ModelMaterialCPU plain = mat_with_file(60027);
    plain.diffuseTex = mat.diffuseTex;
    add_layer(model, plain, "decal", add_tex(model, decal_strip({255, 255}, 7003)), 1);
    CHECK_FALSE(any_warning(build(model, plain, BlendPreset::Opaque), "mask.B"));
}

CM_TEST(vrchat, decal_parallax_warns) {
    for (uint32_t id : {54632u, 57131u}) {
        ModelPreview model;
        ModelMaterialCPU mat = mat_with_file(id);
        mat.diffuseTex = add_tex(model, banded_diffuse());
        add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 255}, 7001)), 1);
        CHECK_EQ(count_warnings(build(model, mat, BlendPreset::Opaque), "decal parallax not mapped"),
                 size_t{1});
    }
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(60027);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 255}, 7001)), 1);
    CHECK_FALSE(any_warning(build(model, mat, BlendPreset::Opaque), "parallax"));
}

CM_TEST(vrchat, diffuse_over_decal_inverts_alpha) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(54632);  // subsurface-decal
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 128, 255}, 7001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.present);
    CHECK_EQ(m.decal.uv, 1);
    CHECK(m.decalMode == "diffuse-over-decal");
    CHECK(m.decal.source == "decal 1-a");
    const int wantA[3] = {255, 127, 0};
    for (int i = 0; i < 3; ++i) CHECK_NEAR(px(m.decal.tex, i, 3), wantA[i], 1);
    CHECK_FALSE(has_extra(m, "decal"));
    CHECK_FALSE(any_warning(m, "decal shine not mapped"));
    CHECK_FALSE(m.emissionMap.present);
}

CM_TEST(vrchat, decal_missing_layer_warns_without_slot) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(60027);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", -1, 1);  // failed to decode
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK_FALSE(m.decal.present);
    CHECK(m.decalMode.empty());
    CHECK(any_warning(m, "decal layer (fileId 4242) failed to decode"));
}

CM_TEST(vrchat, decal_glow_below_half) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(57131);  // decal-glow
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 128, 255}, 7001)), 1);
    mat.namedConstantVectors = {{"glowcol", {1.0f, 0.5f, 0.0f, 1.0f}}};
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decal.present);
    CHECK(m.emissionMap.present);
    CHECK_EQ(m.emissionMap.uv, 1);
    CHECK(m.emissionMap.source == "decal");
    CHECK_EQ(px(m.emissionMap.tex, 0, 0), 200);
    CHECK(m.emissionMask.present);
    CHECK_EQ(m.emissionMask.uv, 1);
    CHECK(m.emissionMask.source == "decal 1-a");
    const int wantMask[3] = {255, 127, 0};
    for (int i = 0; i < 3; ++i) CHECK_NEAR(px(m.emissionMask.tex, i, 0), wantMask[i], 1);
    // glowcol x 2 = (2, 1, 0): colour at peak 1, the peak as strength.
    CHECK_NEAR(m.emissionColor[0], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionColor[1], 0.5f, 0.001f);
    CHECK_NEAR(m.emissionColor[2], 0.0f, 0.001f);
    CHECK_NEAR(m.emissionStrength, 2.0f, 0.001f);
    CHECK(m.emissionBaked.present);
    CHECK_EQ(m.emissionBaked.uv, 1);
    // Baked = decal.rgb x mask x colour: texel 0 has a = 0 (mask 255), rgb {200, 100, 50}.
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 0), 200, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 1), 50, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 2), 0, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 2, 0), 0, 1);  // a = 255: no glow

    // No glowcol: white, with a warning.
    mat.namedConstantVectors.clear();
    MaterialMaps w = build(model, mat, BlendPreset::Opaque);
    CHECK(w.emissionMask.present);
    CHECK_NEAR(w.emissionColor[1], 1.0f, 0.001f);
    CHECK(any_warning(w, "glowcol"));
}

CM_TEST(vrchat, decal_glow_above_half) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(57806);  // prop-decal + AboveHalf glow
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 128, 192, 255}, 7001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.decalMode == "decal-over-diffuse");
    CHECK(m.emissionMask.present);
    CHECK(m.emissionMask.source == "decal saturate(2a-1)");
    const int wantMask[4] = {0, 1, 129, 255};
    for (int i = 0; i < 4; ++i) CHECK_NEAR(px(m.emissionMask.tex, i, 0), wantMask[i], 1);
    CHECK_NEAR(m.emissionColor[0], 1.0f, 0.001f);
    CHECK_NEAR(m.emissionStrength, 1.0f, 0.001f);
}

CM_TEST(vrchat, decal_glow_slot_in_use_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(57131);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "decal", add_tex(model, decal_strip({0, 255}, 7001)), 1);
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Opaque);
    CHECK(m.emissionMap.present);
    CHECK_EQ(m.emissionMap.fileId, 2001u);
    CHECK(m.emissionMap.source == "glow");
    CHECK(any_warning(m, "decal glow not mapped: emission slot in use"));
    CHECK(m.decal.present);  // the decal map itself is still built
}

CM_TEST(vrchat, opacity_and_glow_splits_alpha) {
    const ShaderProfile& p = profile_for(mat_with_file(44709), 0);
    CHECK(p.name == "fx-alpha-glow");
    CHECK(p.diffuseAlpha == AlphaUse::OpacityAndGlow);
    CHECK_FALSE(p.clips);

    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(44709);
    mat.diffuseTex = add_tex(
        model, tex_from(5, 1,
                        {{10, 20, 30, 0}, {10, 20, 30, 64}, {10, 20, 30, 127}, {10, 20, 30, 128},
                         {10, 20, 30, 255}},
                        1000));
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    CHECK(m.baseColor.present);
    const int wantA[5] = {0, 128, 254, 255, 255};
    for (int i = 0; i < 5; ++i) CHECK_NEAR(px(m.baseColor.tex, i, 3), wantA[i], 1);
    CHECK(m.emissionMask.present);
    CHECK(m.emissionMap.present);
    CHECK(m.emissionMap.source == "baseColor");
    const int wantG[5] = {0, 0, 0, 0, 255};
    for (int i = 0; i < 5; ++i) CHECK_NEAR(px(m.emissionMask.tex, i, 0), wantG[i], 1);
    CHECK_EQ(px(m.emissionMap.tex, 4, 0), 10);
    CHECK_NEAR(m.emissionStrength, 2.0f, 0.001f);
    CHECK(m.emissionBaked.present);
    CHECK_NEAR(px(m.emissionBaked.tex, 4, 1), 20, 1);
    CHECK_NEAR(px(m.emissionBaked.tex, 0, 1), 0, 1);
    CHECK(any_warning(m, "ramp"));
    CHECK(any_warning(m, "diffade"));
}

CM_TEST(vrchat, opacity_and_glow_with_glow_layer_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(44709);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 0);
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    CHECK(m.emissionMap.present);
    CHECK_EQ(m.emissionMap.fileId, 2001u);
    CHECK(m.emissionMap.source == "glow");
    CHECK(any_warning(m, "self-illumination"));
    CHECK_NEAR(px(m.baseColor.tex, 3, 3), 255, 1);  // the opacity half is still built
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

CM_TEST(vrchat, materials_json_decal_entry) {
    ModelPreview model = glow_quad();
    ModelMaterialCPU prop = mat_with_file(60027);  // prop-decal
    prop.index = 1;
    prop.materialName = "Wall";
    prop.hasRenderState = true;
    prop.renderState = 0;
    prop.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, prop, "decal", add_tex(model, decal_strip({0, 64, 128, 255}, 7001)), 1);
    model.materials.push_back(prop);
    model.meshes.push_back(quad_mesh(1));
    ModelMaterialCPU metal = mat_with_file(19910);  // decalmask on another UV
    metal.index = 2;
    metal.materialName = "Metal";
    metal.hasRenderState = true;
    metal.diffuseTex = prop.diffuseTex;
    add_layer(model, metal, "decal", add_tex(model, decal_strip({255, 255}, 7005)), 1);
    add_layer(model, metal, "decalmask",
              add_tex(model, tex_from(2, 1, {{40, 0, 0, 255}, {200, 0, 0, 255}}, 7006)), 2);
    model.materials.push_back(metal);
    model.meshes.push_back(quad_mesh(2));

    fs::path dir = fresh_dir("decal");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Prop", 1);
    CHECK(r.ok);
    CHECK(fs::exists(dir / "Textures" / "Wall - Decal.png"));
    json doc = read_json(dir / "materials.json");
    const json& wall = *material_named(doc, "Wall");
    CHECK(wall["maps"].contains("decal"));
    json decal = wall["maps"].value("decal", json());
    if (!decal.is_object()) decal = json::object();
    CHECK_FALSE(decal.empty());
    CHECK(decal.value("file", json()) == "Textures/Wall - Decal.png");
    CHECK(decal.value("uv", json()) == 1);
    CHECK(decal.value("fileId", json()) == 7001);
    CHECK(decal.value("source", json()) == "decal saturate(2a)");
    CHECK(decal.value("mode", json()) == "decal-over-diffuse");
    for (const json& x : wall["maps"]["extras"]) CHECK(x["role"] != "decal");
    CHECK(wall["maps"].value("decalMask", json(1)).is_null());
    const json& blade = *material_named(doc, "Blade");
    CHECK(blade["maps"].contains("decal"));
    CHECK(blade["maps"].value("decal", json(1)).is_null());
    CHECK_NEAR(blade["emission"]["strength"].get<double>(), 1.0, 1e-9);

    CHECK(fs::exists(dir / "Textures" / "Metal - DecalMask.png"));
    const json& mt = *material_named(doc, "Metal");
    json dm = mt["maps"].value("decalMask", json());
    if (!dm.is_object()) dm = json::object();
    CHECK(dm.value("file", json()) == "Textures/Metal - DecalMask.png");
    CHECK(dm.value("uv", json()) == 2);
    CHECK(dm.value("fileId", json()) == 7006);
    CHECK(dm.value("channel", json()) == "R");
    CHECK(dm.value("source", json()) == "decalmask.R");
    for (const json& x : mt["maps"]["extras"]) CHECK(x["role"] != "decalmask");
}

// ------------------------------------------------------------- cutout layers --
// docs/research/gw2-material-channels.md 8.2 (511663) and 8.3 "cutout + mod" (53858):
// the clip is on a `cutout` layer on its own UV, so it ships as maps.alphaMask.

CM_TEST(vrchat, cutout_layer_becomes_alpha_mask) {
    const ShaderProfile& p = profile_for(mat_with_file(511663), 0);
    CHECK(p.name == "weapon-cutout-glow");
    CHECK(p.cutoutRole == "cutout");
    CHECK(p.cutoutChannels == CutoutChannels::RxA);
    CHECK(alpha_tested(p));
    CHECK(decode_blend(0, true, false, alpha_tested(p)).preset == BlendPreset::Cutout);

    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(511663);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 164}, 1000));
    add_layer(model, mat, "cutout",
              add_tex(model, tex_from(4, 1, {{255, 0, 0, 255}, {255, 0, 0, 128},
                                             {128, 0, 0, 128}, {0, 0, 0, 255}}, 8001)),
              1);
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK(m.alphaMask.present);
    CHECK_EQ(m.alphaMask.uv, 1);
    CHECK_EQ(m.alphaMask.fileId, 8001u);
    CHECK(m.alphaMask.source == "cutout.R x cutout.A");
    CHECK_NEAR(m.alphaMaskCutoff, 0.5f, 1e-6);
    const int want[4] = {255, 128, 64, 0};
    for (int i = 0; i < 4; ++i) {
        CHECK_NEAR(px(m.alphaMask.tex, i, 0), want[i], 1);
        CHECK_EQ(px(m.alphaMask.tex, i, 1), px(m.alphaMask.tex, i, 0));
        CHECK_EQ(px(m.alphaMask.tex, i, 2), px(m.alphaMask.tex, i, 0));
        CHECK_EQ(px(m.alphaMask.tex, i, 3), 255);
    }
    CHECK_FALSE(has_extra(m, "cutout"));
    // The diffuse alpha is shine only: no holes in BaseColor; the glow is still mapped.
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.baseColor.tex, i, 3), 255);
    CHECK(m.emissionMap.present);
    CHECK(m.smoothSource == "diffuseAlpha");
    CHECK_FALSE(any_warning(m, "never cuts at rest"));

    // Every texel >= 0.5 (3123167): a dissolve, said once, naming cutfade.
    ModelMaterialCPU rest = mat_with_file(511663);
    rest.diffuseTex = mat.diffuseTex;
    rest.namedConstants = {{"cutfade", 1.0f}};
    add_layer(model, rest, "cutout",
              add_tex(model, tex_from(2, 1, {{255, 0, 0, 255}, {200, 0, 0, 200}}, 8002)), 1);
    MaterialMaps r = build(model, rest, BlendPreset::Cutout);
    CHECK(r.alphaMask.present);
    CHECK_EQ(count_warnings(r, "never cuts at rest"), size_t{1});
    CHECK(any_warning(r, "cutfade"));
}

CM_TEST(vrchat, cutout_layer_with_diffuse_holes) {
    const ShaderProfile& p = profile_for(mat_with_file(53858), 0);
    CHECK(p.supported);
    CHECK(p.clips);
    CHECK(p.diffuseAlpha == AlphaUse::HolesAndShine);
    CHECK(p.cutoutRole == "cutout");
    CHECK(p.cutoutChannels == CutoutChannels::R);

    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(53858);
    mat.diffuseTex = add_tex(model, banded_diffuse());
    add_layer(model, mat, "cutout",
              add_tex(model, tex_from(2, 1, {{200, 10, 0, 40}, {50, 0, 0, 255}}, 8003)), 2);
    add_layer(model, mat, "mod", add_tex(model, solid(2, 2, {128, 128, 128, 255}, 8004)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    // BaseColor keeps the diffuse holes saturate(2a) < 0.5.
    const int wantA[4] = {0, 255, 255, 255};
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.baseColor.tex, i, 3), wantA[i]);
    CHECK(m.alphaMask.present);
    CHECK_EQ(m.alphaMask.uv, 2);
    CHECK(m.alphaMask.source == "cutout.R");
    CHECK_NEAR(m.alphaMaskCutoff, 0.5f, 1e-6);
    CHECK_EQ(px(m.alphaMask.tex, 0, 0), 200);  // R only, the alpha is ignored
    CHECK_EQ(px(m.alphaMask.tex, 1, 0), 50);
    CHECK_FALSE(has_extra(m, "cutout"));
    CHECK(has_extra(m, "mod"));
    CHECK(any_warning(m, "alphaMask (cutout.R) uses UV2"));
    // The game multiplies the two before one test: splitting them is an approximation.
    CHECK(any_warning(m, "cutout.R x saturate(2a)"));
}

CM_TEST(vrchat, cutout_layer_missing_warns) {
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(511663);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 164}, 1000));
    add_layer(model, mat, "glow", add_tex(model, solid(2, 2, {255, 64, 0, 255}, 2001)), 1);
    MaterialMaps m = build(model, mat, BlendPreset::Cutout);
    CHECK_FALSE(m.alphaMask.present);
    CHECK_EQ(count_warnings(m, "no cutout layer: alpha mask not built"), size_t{1});
    CHECK(m.baseColor.present);
    CHECK(m.emissionMap.present);

    ModelMaterialCPU bad = mat;
    add_layer(model, bad, "cutout", -1, 1);  // failed to decode
    MaterialMaps b = build(model, bad, BlendPreset::Cutout);
    CHECK_FALSE(b.alphaMask.present);
    CHECK(any_warning(b, "cutout layer (fileId 4242) failed to decode"));
    CHECK(any_warning(b, "alpha mask not built"));
    CHECK_FALSE(has_extra(b, "cutout"));
    CHECK(b.baseColor.present);
    CHECK(b.emissionMap.present);
}

CM_TEST(vrchat, cutout_placeholder_is_constant) {
    ModelPreview model;
    const int diffuse = add_tex(model, solid(2, 2, {100, 100, 100, 164}, 1000));

    ModelMaterialCPU keep = mat_with_file(511663);
    keep.diffuseTex = diffuse;
    add_layer(model, keep, "cutout", add_tex(model, solid(4, 4, {255, 0, 0, 200}, 8005)), 1);
    MaterialMaps k = build(model, keep, BlendPreset::Cutout);
    CHECK_FALSE(k.alphaMask.present);
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(k.baseColor.tex, i, 3), 255);
    CHECK_FALSE(any_warning(k, "whole material"));
    CHECK_FALSE(any_warning(k, "alpha mask not built"));

    ModelMaterialCPU cut = mat_with_file(511663);
    cut.diffuseTex = diffuse;
    add_layer(model, cut, "cutout", add_tex(model, solid(4, 4, {100, 0, 0, 255}, 8006)), 1);
    MaterialMaps c = build(model, cut, BlendPreset::Cutout);
    CHECK_FALSE(c.alphaMask.present);
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(c.baseColor.tex, i, 3), 0);
    CHECK_EQ(count_warnings(c, "cuts the whole material"), size_t{1});
    CHECK_FALSE(has_extra(c, "cutout"));
}

CM_TEST(vrchat, materials_json_alpha_mask_entry) {
    ModelPreview model = glow_quad();
    ModelMaterialCPU hammer = mat_with_file(511663);
    hammer.index = 1;
    hammer.materialName = "Hammer";
    hammer.hasRenderState = true;
    hammer.renderState = 0;
    hammer.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 164}, 1000));
    add_layer(model, hammer, "cutout",
              add_tex(model, tex_from(2, 1, {{255, 0, 0, 255}, {40, 0, 0, 255}}, 8007)), 1);
    model.materials.push_back(hammer);
    model.meshes.push_back(quad_mesh(1));

    fs::path dir = fresh_dir("alphamask");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Hammer", 1);
    CHECK(r.ok);
    CHECK(fs::exists(dir / "Textures" / "Hammer - AlphaMask.png"));
    json doc = read_json(dir / "materials.json");
    const json& h = *material_named(doc, "Hammer");
    CHECK(h["preset"] == "Cutout");
    CHECK(h["alphaCutoffIsDefault"] == false);
    json am = h["maps"].value("alphaMask", json());
    if (!am.is_object()) am = json::object();
    CHECK(am.value("file", json()) == "Textures/Hammer - AlphaMask.png");
    CHECK(am.value("uv", json()) == 1);
    CHECK(am.value("fileId", json()) == 8007);
    CHECK(am.value("source", json()) == "cutout.R x cutout.A");
    CHECK_NEAR(am.value("cutoff", json(-9.0)).get<double>(), 0.5, 1e-6);
    for (const json& x : h["maps"]["extras"]) CHECK(x["role"] != "cutout");
    const json& blade = *material_named(doc, "Blade");
    CHECK(blade["maps"].contains("alphaMask"));
    CHECK(blade["maps"].value("alphaMask", json(1)).is_null());
    // The .glb keeps the BaseColor alpha test: MASK.
    json g = glb_json(dir / "Hammer.glb");
    CHECK(g["materials"][1]["alphaMode"] == "MASK");
}

// ------------------------------------------------- base colour / opacity layers --
// docs/research/gw2-material-channels.md 8.2 (842652, Astralaria): the colour is the
// `parallax` layer (UV0); castlemist's "diffuse" (UV3) is a UV-offset map; opacity =
// parallax.A x mask.R (mask on UV1) x diffade; unlit.

namespace {

const MaterialMaps::Extra* extra_named(const MaterialMaps& m, const char* role) {
    for (const auto& x : m.extras)
        if (x.role == role) return &x;
    return nullptr;
}

/// An 842652 material: diffuse (UV-offset map) on UV3, parallax colour on UV0,
/// mask on UV1, mskptrb (the diffuse file again) on UV2.
ModelMaterialCPU parallax_mat(ModelPreview& model, bool withParallax = true,
                              bool withMask = true) {
    ModelMaterialCPU mat = mat_with_file(842652);
    mat.diffuseTex = add_tex(model, solid(2, 2, {128, 128, 0, 255}, 57890));
    mat.diffuseUv = 3;
    if (withParallax)
        add_layer(model, mat, "parallax",
                  add_tex(model, tex_from(2, 2, {{10, 20, 30, 255}, {40, 50, 60, 128},
                                                 {70, 80, 90, 0}, {200, 210, 220, 255}},
                                          842653)),
                  0);
    if (withMask)
        add_layer(model, mat, "mask",
                  add_tex(model, tex_from(2, 1, {{0, 9, 9, 9}, {200, 9, 9, 9}}, 9001)), 1);
    add_layer(model, mat, "mskptrb", mat.diffuseTex, 2);
    return mat;
}

}  // namespace

CM_TEST(vrchat, base_color_from_named_role) {
    const ShaderProfile& p = profile_for(mat_with_file(842652), 0);
    CHECK(p.name == "fx-parallax-layer");
    CHECK(p.supported);
    CHECK_FALSE(alpha_tested(p));
    CHECK(p.diffuseAlpha == AlphaUse::Opacity);
    CHECK(p.baseColorRole == "parallax");
    CHECK(p.diffuseUse == "uv-offset");
    CHECK(p.opacityRole == "mask");
    CHECK(p.opacityChannel == Channel::R);
    CHECK(p.unlit);
    // Every other profile keeps the diffuse as colour and has no opacity layer.
    CHECK(profile_for(mat_with_file(561567), 0).baseColorRole.empty());
    CHECK(profile_for(mat_with_file(561567), 0).opacityRole.empty());
    CHECK_FALSE(profile_for(mat_with_file(561567), 0).unlit);

    ModelPreview model;
    ModelMaterialCPU mat = parallax_mat(model);
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    CHECK(m.baseColor.present);
    CHECK_EQ(m.baseColor.uv, 0);
    CHECK_EQ(m.baseColor.fileId, 842653u);
    CHECK(m.baseColor.source == "parallax");
    CHECK_EQ(m.baseColor.tex.width, 2);
    CHECK_EQ(px(m.baseColor.tex, 1, 0), 40);
    CHECK_EQ(px(m.baseColor.tex, 3, 2), 220);
    CHECK_FALSE(has_extra(m, "parallax"));
    // The castlemist "diffuse" goes out raw as a UV-offset extra on its own UV.
    const MaterialMaps::Extra* d = extra_named(m, "diffuse");
    CHECK(d != nullptr);
    if (d) {
        CHECK(d->use == "uv-offset");
        CHECK_EQ(d->slot.uv, 3);
        CHECK_EQ(d->slot.fileId, 57890u);
    }
    CHECK_FALSE(m.packed.present);  // no shine: the diffuse alpha is not read
    CHECK(any_warning(m, "uv-offset"));
    CHECK(any_warning(m, "pardist"));
    CHECK(any_warning(m, "unlit"));
    CHECK_FALSE(any_warning(m, "no diffuse texture"));
}

CM_TEST(vrchat, opacity_layer_becomes_alpha_mask) {
    ModelPreview model;
    ModelMaterialCPU mat = parallax_mat(model);
    mat.namedConstants = {{"cutptrb", 0.05f}, {"diffade", 1.0f}};
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    CHECK(m.alphaMask.present);
    CHECK_EQ(m.alphaMask.uv, 1);
    CHECK_EQ(m.alphaMask.fileId, 9001u);
    CHECK(m.alphaMask.source == "mask.R");
    CHECK(m.alphaMaskCutoff < 0.0f);  // opacity, not a cutoff
    CHECK_EQ(px(m.alphaMask.tex, 0, 0), 0);
    CHECK_EQ(px(m.alphaMask.tex, 1, 0), 200);
    CHECK_EQ(px(m.alphaMask.tex, 1, 1), 200);
    CHECK_EQ(px(m.alphaMask.tex, 1, 3), 255);
    CHECK_FALSE(has_extra(m, "mask"));
    // BaseColor A = parallax.A (the mask multiplies it in the shader).
    const int wantA[4] = {255, 128, 0, 255};
    for (int i = 0; i < 4; ++i) CHECK_EQ(px(m.baseColor.tex, i, 3), wantA[i]);
    CHECK(any_warning(m, "cutptrb"));
    CHECK_FALSE(any_warning(m, "diffade"));  // 1.0: nothing lost

    ModelMaterialCPU fade = parallax_mat(model);
    fade.namedConstants = {{"diffade", 0.5f}};
    CHECK(any_warning(build(model, fade, BlendPreset::Fade), "diffade"));
}

CM_TEST(vrchat, cutout_and_opacity_both_warn) {
    ShaderProfile p;
    p.name = "test-both";
    p.diffuseAlpha = AlphaUse::Opacity;
    p.cutoutRole = "cutout";
    p.cutoutChannels = CutoutChannels::R;
    p.opacityRole = "mask";
    p.opacityChannel = Channel::R;

    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(1);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 255}, 1000));
    add_layer(model, mat, "cutout",
              add_tex(model, tex_from(2, 1, {{255, 0, 0, 255}, {0, 0, 0, 255}}, 8001)), 1);
    add_layer(model, mat, "mask",
              add_tex(model, tex_from(2, 1, {{50, 0, 0, 255}, {90, 0, 0, 255}}, 9001)), 2);
    MaterialMaps m = build_material_maps(model, mat, blend_of(BlendPreset::Cutout), p);
    CHECK(m.alphaMask.present);
    CHECK(m.alphaMask.source == "cutout.R");
    CHECK_NEAR(m.alphaMaskCutoff, 0.5f, 1e-6);
    CHECK_EQ(count_warnings(m, "opacity (mask.R) not mapped"), size_t{1});
    CHECK(any_warning(m, "taken by the cutout layer"));
    CHECK(has_extra(m, "mask"));  // the loser still ships raw
    CHECK_FALSE(has_extra(m, "cutout"));
}

CM_TEST(vrchat, parallax_and_opacity_layers_missing_or_placeholder) {
    ModelPreview model;
    // No parallax layer: no BaseColor (the diffuse is not colour), the rest exports.
    ModelMaterialCPU noColour = parallax_mat(model, false, true);
    MaterialMaps a = build(model, noColour, BlendPreset::Fade);
    CHECK_FALSE(a.baseColor.present);
    CHECK_EQ(count_warnings(a, "no parallax layer: BaseColor left out"), size_t{1});
    CHECK(a.alphaMask.present);
    CHECK(has_extra(a, "diffuse"));

    // Parallax failed to decode: said by resolve, plus what is lost.
    ModelMaterialCPU badColour = parallax_mat(model, false, true);
    add_layer(model, badColour, "parallax", -1, 0);
    MaterialMaps b = build(model, badColour, BlendPreset::Fade);
    CHECK_FALSE(b.baseColor.present);
    CHECK(any_warning(b, "parallax layer (fileId 4242) failed to decode"));
    CHECK(any_warning(b, "BaseColor left out"));
    CHECK(b.alphaMask.present);

    // No mask layer: no alphaMask, said once; BaseColor stays.
    ModelMaterialCPU noMask = parallax_mat(model, true, false);
    MaterialMaps c = build(model, noMask, BlendPreset::Fade);
    CHECK(c.baseColor.present);
    CHECK_FALSE(c.alphaMask.present);
    CHECK_EQ(count_warnings(c, "no mask layer: opacity (mask.R) not built"), size_t{1});

    // A placeholder mask is a constant: multiplied into BaseColor A, no map.
    ModelMaterialCPU flatMask = parallax_mat(model, true, false);
    add_layer(model, flatMask, "mask", add_tex(model, solid(4, 4, {128, 0, 0, 255}, 9002)), 1);
    MaterialMaps d = build(model, flatMask, BlendPreset::Fade);
    CHECK_FALSE(d.alphaMask.present);
    CHECK_FALSE(has_extra(d, "mask"));
    const int wantA[4] = {128, 64, 0, 128};
    for (int i = 0; i < 4; ++i) CHECK_NEAR(px(d.baseColor.tex, i, 3), wantA[i], 1);
    CHECK(any_warning(d, "multiplied into the BaseColor alpha"));

    // A placeholder parallax is a constant: no BaseColor map, the value is named.
    ModelMaterialCPU flatColour = parallax_mat(model, false, true);
    add_layer(model, flatColour, "parallax",
              add_tex(model, solid(4, 4, {10, 20, 30, 255}, 842654)), 0);
    MaterialMaps e = build(model, flatColour, BlendPreset::Fade);
    CHECK_FALSE(e.baseColor.present);
    CHECK_FALSE(has_extra(e, "parallax"));
    CHECK(any_warning(e, "BaseColor is the parallax constant (10, 20, 30, 255)"));
    CHECK(e.alphaMask.present);
}

CM_TEST(vrchat, materials_json_opacity_mask_and_glb_base_color) {
    ModelPreview model = glow_quad();
    ModelMaterialCPU axe = parallax_mat(model);
    axe.index = 1;
    axe.materialName = "Axe";
    axe.hasRenderState = true;
    axe.renderState = 0x6565000;  // SrcA/InvSrcA
    model.materials.push_back(axe);
    model.meshes.push_back(quad_mesh(1));

    fs::path dir = fresh_dir("opacitymask");
    VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Axe", 1);
    CHECK(r.ok);
    json doc = read_json(dir / "materials.json");
    const json& a = *material_named(doc, "Axe");
    CHECK(a["profile"] == "fx-parallax-layer");
    CHECK(a["preset"] == "Fade");
    json bc = a["maps"].value("baseColor", json());
    if (!bc.is_object()) bc = json::object();
    CHECK(bc.value("source", json()) == "parallax");
    CHECK(bc.value("uv", json()) == 0);
    json am = a["maps"].value("alphaMask", json());
    if (!am.is_object()) am = json::object();
    CHECK(am.value("source", json()) == "mask.R");
    CHECK(am.value("uv", json()) == 1);
    CHECK(am.contains("cutoff"));
    CHECK(am.value("cutoff", json(1)).is_null());  // opacity: no cutoff
    bool offset = false;
    for (const json& x : a["maps"]["extras"])
        if (x["role"] == "diffuse" && x["use"] == "uv-offset" && x["uv"] == 3) offset = true;
    CHECK(offset);

    // The .glb's baseColorTexture is the parallax layer on UV0, not the UV3 diffuse.
    json g = glb_json(dir / "Axe.glb");
    const json& pbr = g["materials"][1]["pbrMetallicRoughness"];
    CHECK(pbr.contains("baseColorTexture"));
    CHECK(pbr.value("baseColorTexture", json::object()).value("index", -1) >= 0);
    CHECK_FALSE(pbr.value("baseColorTexture", json::object()).contains("texCoord"));
}

CM_TEST(vrchat, mskptrb_hint_only_with_an_opacity_layer) {
    // A default-profile material (1195172 is not hand-read) keeps the raw use null.
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(1195172);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 255}, 1000));
    add_layer(model, mat, "mskptrb", add_tex(model, solid(2, 2, {128, 128, 0, 255}, 57890)), 2);
    MaterialMaps m = build(model, mat, BlendPreset::Fade);
    const MaterialMaps::Extra* x = extra_named(m, "mskptrb");
    CHECK(x != nullptr);
    if (x) CHECK(x->use.empty());

    ModelMaterialCPU axe = parallax_mat(model);
    const MaterialMaps::Extra* y = extra_named(build(model, axe, BlendPreset::Fade), "mskptrb");
    CHECK(y != nullptr);
    if (y) CHECK(y->use == "uv-offset");
}

CM_TEST(vrchat, opacity_takes_alpha_mask_when_cutout_missing) {
    ShaderProfile p;
    p.name = "test-both";
    p.diffuseAlpha = AlphaUse::Opacity;
    p.cutoutRole = "cutout";
    p.opacityRole = "mask";
    p.opacityChannel = Channel::R;
    ModelPreview model;
    ModelMaterialCPU mat = mat_with_file(1);
    mat.diffuseTex = add_tex(model, solid(2, 2, {100, 100, 100, 255}, 1000));
    add_layer(model, mat, "mask",
              add_tex(model, tex_from(2, 1, {{50, 0, 0, 255}, {90, 0, 0, 255}}, 9001)), 2);
    MaterialMaps m = build_material_maps(model, mat, blend_of(BlendPreset::Cutout), p);
    CHECK(m.alphaMask.present);
    CHECK(m.alphaMask.source == "mask.R");
    CHECK(m.alphaMaskCutoff < 0.0f);
    CHECK_FALSE(any_warning(m, "taken by the cutout layer"));
    CHECK(any_warning(m, "no cutout layer"));

    ModelMaterialCPU bad = mat;
    add_layer(model, bad, "cutout", -1, 1);  // failed to decode
    MaterialMaps b = build_material_maps(model, bad, blend_of(BlendPreset::Cutout), p);
    CHECK(b.alphaMask.present);
    CHECK(b.alphaMask.source == "mask.R");
}

CM_TEST(vrchat, parallax_placeholder_wording) {
    ModelPreview model;
    // A placeholder diffuse is not exported: the warning must not say it is.
    ModelMaterialCPU flatDiffuse = parallax_mat(model);
    flatDiffuse.diffuseTex = add_tex(model, solid(4, 4, {128, 128, 0, 255}, 57891));
    MaterialMaps a = build(model, flatDiffuse, BlendPreset::Fade);
    CHECK_FALSE(has_extra(a, "diffuse"));
    CHECK(any_warning(a, "a placeholder, not exported"));
    CHECK_FALSE(any_warning(a, "exported raw as an extra"));

    // A placeholder mask with no BaseColor: nothing to multiply into.
    ModelMaterialCPU none = parallax_mat(model, false, false);
    add_layer(model, none, "mask", add_tex(model, solid(4, 4, {128, 0, 0, 255}, 9003)), 1);
    MaterialMaps b = build(model, none, BlendPreset::Fade);
    CHECK_FALSE(b.baseColor.present);
    CHECK_FALSE(any_warning(b, "multiplied into the BaseColor alpha"));
    CHECK(any_warning(b, "no BaseColor to multiply it into"));
}
