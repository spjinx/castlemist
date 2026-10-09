/// @file
/// @brief Tests for shader-side dyes (ripper/shader_dye.h): mounts and other
///        models whose material dyes in the pixel shader.

#include "test_framework.h"

#include "castlemist/ripper/shader_dye.h"

#include <algorithm>
#include <cmath>
#include <string>

using namespace castlemist::ripper;
using castlemist::character::DyeShift;

namespace {

// What the springer's colour PS does for one channel at full mask weight.
std::array<float, 3> shade(const DyeRows& r, std::array<uint8_t, 3> rgb) {
    std::array<float, 3> out{};
    for (int i = 0; i < 3; ++i)
        out[i] = (r[i][0] * rgb[0] / 255.0f + r[i][1] * rgb[1] / 255.0f + r[i][2] * rgb[2] / 255.0f + r[i][3]) * 255.0f;
    return out;
}

// A model with one material that dyes in the shader: a 2x1 diffuse, a 2x1
// dyemask whose first texel is all channel 1 and second all channel 3, and
// game-shader constants at the uniforms' byte offsets.
ModelPreview springer_like() {
    ModelPreview m;
    ModelTextureCPU diffuse;
    diffuse.width = 2;
    diffuse.height = 1;
    diffuse.rgba = {200, 100, 50, 255, 200, 100, 50, 255};
    ModelTextureCPU mask = diffuse;
    mask.rgba = {255, 0, 0, 0, 0, 0, 255, 0};
    m.textures = {diffuse, mask};
    ModelMaterialCPU mat;
    mat.diffuseTex = 0;
    mat.extraTextures.push_back({1, 0, 0, "dyemask"});
    for (int ch = 0; ch < 4; ++ch)
        for (int row = 0; row < 3; ++row) {
            std::array<float, 4> v{};
            v[static_cast<size_t>(row)] = 1;
            mat.namedConstantVectors.emplace_back(kDyeUniforms[ch][row], v);
        }
    m.materials = {mat};
    GameMaterial gm;
    gm.ok = true;
    for (int ch = 0; ch < 4; ++ch)
        for (int row = 0; row < 3; ++row)
            gm.psUniforms.push_back({kDyeUniforms[ch][row], 2, 1, 144 + 16 * (ch * 3 + row), 1});
    gm.psUniforms.push_back({"envcp", 2, 1, 336, 1});
    gm.psConsts.push_back({336, {1, 1, 1, 1}});
    m.gameMaterials = {gm};
    return m;
}

ColorMatrix red_dye() { return dye_matrix(DyeShift{10, 1.2f, 30, 0.8f, 1.1f}); }

} // namespace

CM_TEST(shader_dye, rows_reproduce_apply_dye) {
    const ColorMatrix m = red_dye();
    const DyeRows r = dye_shader_rows(m);
    for (std::array<uint8_t, 3> c : {std::array<uint8_t, 3>{200, 100, 50}, {10, 220, 90}, {128, 128, 128}}) {
        const auto want = apply_dye(m, c);
        const auto got = shade(r, c);
        for (int i = 0; i < 3; ++i)
            CHECK(std::abs(std::clamp(got[i], 0.0f, 255.0f) - want[i]) < 1.01f);
    }
}

CM_TEST(shader_dye, identity_dye_is_identity_rows) {
    ColorMatrix id{};
    for (int i = 0; i < 4; ++i) id[i][i] = 1;
    const DyeRows r = dye_shader_rows(id);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) CHECK_EQ(r[i][j], i == j ? 1.0f : 0.0f);
}

CM_TEST(shader_dye, detects_and_finds_channels) {
    ModelPreview m = springer_like();
    CHECK(has_shader_dyes(m));
    const auto ch = shader_dye_channels(m);
    CHECK(ch[0] && !ch[1] && ch[2] && !ch[3]);
    m.materials[0].namedConstantVectors.clear();
    CHECK(!has_shader_dyes(m));
}

CM_TEST(shader_dye, uniforms_leave_undyed_channels_identity) {
    const ShaderUniforms u = shader_dye_uniforms({red_dye(), std::nullopt, std::nullopt, std::nullopt});
    CHECK_EQ(u.size(), size_t{12});
    CHECK(u.at("hsmntd") == (std::array<float, 4>{1, 0, 0, 0}));
    CHECK(u.at("hsmnta") != (std::array<float, 4>{1, 0, 0, 0}));
}

CM_TEST(shader_dye, set_writes_game_constants_and_bakes_full_mode) {
    const ModelPreview pristine = springer_like();
    ModelPreview m = pristine;
    const ColorMatrix dye = red_dye();
    const ShaderUniforms u = shader_dye_uniforms({dye, std::nullopt, std::nullopt, std::nullopt});
    CHECK_EQ(set_shader_dyes(m, pristine, u), 1);

    // Shader mode: hsmnta at byte 144 now holds the dye's red row; envcp untouched.
    const auto& pc = m.gameMaterials[0].psConsts;
    bool sawA = false, sawEnv = false;
    for (const GameConstOverride& c : pc) {
        if (c.byteOff == 144) {
            sawA = true;
            for (int k = 0; k < 4; ++k) CHECK_EQ(c.value[k], u.at("hsmnta")[static_cast<size_t>(k)]);
        }
        if (c.byteOff == 336) sawEnv = c.value[0] == 1;
    }
    CHECK(sawA && sawEnv);

    // Full mode: a new diffuse, dyed where channel 1 covers it, as authored where
    // only the undyed channel 3 does. The pristine texture is untouched.
    const int t = m.materials[0].diffuseTex;
    CHECK(t != 0);
    const auto& px = m.textures[static_cast<size_t>(t)].rgba;
    const auto want = apply_dye(dye, {200, 100, 50});
    for (int i = 0; i < 3; ++i) CHECK(std::abs(int(px[static_cast<size_t>(i)]) - int(want[static_cast<size_t>(i)])) <= 1);
    CHECK(px[4] == 200 && px[5] == 100 && px[6] == 50);
    CHECK(m.textures[0].rgba == pristine.textures[0].rgba);

    // Again, with no dye: back to the pristine texture and identity constants.
    CHECK_EQ(set_shader_dyes(m, pristine, shader_dye_uniforms({})), 1);
    CHECK_EQ(m.textures.size(), pristine.textures.size());
    CHECK_EQ(m.materials[0].diffuseTex, 0);
}
