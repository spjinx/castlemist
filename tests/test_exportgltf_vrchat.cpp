/// @file
/// @brief Tests for the VRChat (Poiyomi) export helpers: blend decode, shader
///        profiles, map building.

#include "test_framework.h"

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/shader_profiles.h"
#include "castlemist/exportgltf/vrchat_maps.h"
#include "castlemist/extract/model_types.h"

#include <array>
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
