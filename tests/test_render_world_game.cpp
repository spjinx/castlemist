/// @file
/// @brief WorldView's Shader shading: the device-free game-uniform helpers
///        it shares with the main renderer, and (device tests) the game-shader
///        prop pass and its fallbacks.

#include "test_framework.h"

#include "detail/game_uniforms.h"
#include "detail/light_calib.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace castlemist::render;

CM_TEST(world_game, map_env_uniforms_follow_the_rig) {
    MapEnvRig r;
    r.present = true;
    const float toward[3] = {0, 0, -2};   // straight up (map up is -Z), not unit
    GameVals v;
    game_map_env_uniforms(v, r, toward, 1.0f);
    // shSun is the unit toward-the-sun vector, SunDirection its travel direction.
    CHECK_NEAR(v["shSun"][2], -1.0f, 1e-6f);
    CHECK_NEAR(v["shSunData"][2], -1.0f, 1e-6f);
    CHECK_NEAR(v["SunDirection"][2], 1.0f, 1e-6f);
    float sun[3], amb[3];
    calibrated_light(r.sunColor, r.sunIntensity, r.fillColor, r.fillIntensity, 1.0f, sun, amb);
    for (int k = 0; k < 3; ++k) CHECK_NEAR(v["shSunColor"][k], sun[k], 1e-6f);
    CHECK_NEAR(v["shRed"][3], amb[0], 1e-6f);
    CHECK_NEAR(v["shBlue"][3], amb[2], 1e-6f);
    CHECK_NEAR(v["BacklightColor"][0], 0.0f, 0.0f);
}

CM_TEST(world_game, base_uniforms_hold_the_measured_constants) {
    GameVals v;
    game_base_uniforms(v, 1.0f);
    CHECK_NEAR(v["LightBuffer"][3], 128.0f, 0.0f);   // specular power scale, not the decode
    CHECK_NEAR(v["AlphaRef"][0], 0.25f, 0.0f);
    CHECK_NEAR(v["fxclr"][0], 1.0f, 0.0f);
    CHECK_NEAR(v["LightCount"][0], 0.0f, 0.0f);
}

CM_TEST(world_game, cbuffer_fill_writes_uniforms_then_constants) {
    std::vector<GameShaderUniform> u = {{"World", 4, 0, 0, 4}, {"glofade", 2, 0, 64, 1}, {"tex", 0, 1, 80, 1}};
    CHECK_EQ(game_cb_size(u, 0), 80u);
    GameVals v;
    v["World"] = std::vector<float>(16, 2.0f);
    v["glofade"] = {9, 9, 9, 9};
    std::vector<GameConstOverride> c(1);
    c[0].byteOff = 64;
    c[0].value[0] = 0.5f;
    std::vector<uint8_t> buf(80, 0xff);
    game_fill_cb(buf, u, v, c);
    float f[20];
    std::memcpy(f, buf.data(), sizeof f);
    CHECK_NEAR(f[0], 2.0f, 0.0f);
    CHECK_NEAR(f[15], 2.0f, 0.0f);
    CHECK_NEAR(f[16], 0.5f, 0.0f);   // the material constant wins
    CHECK_NEAR(f[17], 0.0f, 0.0f);
}

CM_TEST(world_game, blend_desc_reads_the_bgfx_word) {
    D3D11_BLEND_DESC d{};
    CHECK_FALSE(game_blend_desc(0, 0, d));
    // SRC_ALPHA (5) / INV_SRC_ALPHA (6) on RGB, at BGFX_STATE_BLEND_SHIFT 12.
    const uint64_t st = (uint64_t(5) | (uint64_t(6) << 4) | (uint64_t(5) << 8) | (uint64_t(6) << 12)) << 12;
    CHECK(game_blend_desc(st, 0x4000u, d));
    CHECK(d.RenderTarget[0].BlendEnable == TRUE);
    CHECK(d.RenderTarget[0].SrcBlend == D3D11_BLEND_SRC_ALPHA);
    CHECK(d.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA);
    CHECK_EQ(int(d.RenderTarget[0].RenderTargetWriteMask), 7);   // 0x4000: no alpha write
    const D3D11_BLEND_DESC o = game_opaque_blend_desc(0x0004u);
    CHECK(o.RenderTarget[0].BlendEnable == FALSE);
    CHECK_EQ(int(o.RenderTarget[0].RenderTargetWriteMask), 0);   // depth-only pass
}
