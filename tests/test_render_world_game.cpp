/// @file
/// @brief WorldView's Shader shading: the device-free game-uniform helpers
///        it shares with the main renderer, and (device tests) the game-shader
///        prop pass and its fallbacks.

#include "test_framework.h"

#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_view.h"

#include "detail/game_uniforms.h"
#include "detail/light_calib.h"
#include "world_game.h"

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
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

CM_TEST(world_game, dynamic_uniform_names) {
    CHECK(game_dyn_of("World") == GameDyn::World);
    CHECK(game_dyn_of("WorldViewProjection") == GameDyn::WorldViewProjection);
    CHECK(game_dyn_of("ScreenDims") == GameDyn::ScreenDims);
    CHECK(game_dyn_of("shSun") == GameDyn::Count);   // static: the env rig
}

// ---------------------------------------------------------------------------
// Device tests: a synthetic "game" material (DXBC compiled here, with a bgfx-
// style uniform table) drawn by WorldView's Shader shading.
// ---------------------------------------------------------------------------

namespace {

std::vector<uint8_t> compile(const char* src, const char* entry, const char* target) {
    Microsoft::WRL::ComPtr<ID3DBlob> b, err;
    if (FAILED(D3DCompile(src, std::strlen(src), "test", nullptr, nullptr, entry, target, 0, 0, &b, &err))) return {};
    const uint8_t* p = static_cast<const uint8_t*>(b->GetBufferPointer());
    return std::vector<uint8_t>(p, p + b->GetBufferSize());
}

// The bgfx matrix layout (the transposed row-vector matrix) read with HLSL's
// default column_major packing is the row-vector matrix again.
const char* const kGameVS =
    "cbuffer U : register(b0) { float4x4 WorldViewProjection; };\n"
    "float4 VSMain(float3 p : POSITION) : SV_Position { return mul(float4(p, 1.0), WorldViewProjection); }\n";
const char* const kGamePS =
    "cbuffer U : register(b0) { float4 tintc; };\n"
    "Texture2D t0 : register(t0);\n"
    "SamplerState s0 : register(s0);\n"
    "float4 PSMain(float4 pos : SV_Position) : SV_Target { return t0.Sample(s0, float2(0.5, 0.5)) * tintc; }\n";

/// One material: kGameVS / kGamePS, tintc = (0.5, 1, 0.25, 1) as a material
/// constant, texture 0 = fileId 701.
std::shared_ptr<const ModelPreview> game_cube(bool ok = true) {
    auto g = std::make_shared<ModelPreview>();
    g->textures.resize(1);
    g->textures[0].fileId = 701;
    GameMaterial m;
    m.index = 0;
    m.ok = ok;
    m.vsDXBC = compile(kGameVS, "VSMain", "vs_5_0");
    m.psDXBC = compile(kGamePS, "PSMain", "ps_5_0");
    m.vsUniforms = {{"WorldViewProjection", 4, 0, 0, 4}};
    m.psUniforms = {{"tintc", 2, 1, 0, 1}};
    GameConstOverride c;
    c.byteOff = 0;
    c.value[0] = 0.5f; c.value[1] = 1.0f; c.value[2] = 0.25f; c.value[3] = 1.0f;
    m.psConsts = {c};
    GameSamplerCPU s;
    s.slot = 0;
    s.gameTex = 0;
    m.samplers = {s};
    g->gameMaterials.push_back(std::move(m));
    return g;
}

/// A 100-unit cube, one mesh, material 0 (diffuse fileId 701).
std::shared_ptr<const ModelPreview> plain_cube() {
    auto mp = std::make_shared<ModelPreview>();
    ModelMeshCPU m;
    const float h = 50;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        const int a = (f / 2 + 1) % 3, b = (f / 2 + 2) % 3;
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (int c = 0; c < 4; ++c) {
            GVertex v{};
            float p[3] = {n[f][0] * h, n[f][1] * h, n[f][2] * h};
            p[a] = (c & 1) ? h : -h;
            p[b] = (c & 2) ? h : -h;
            v.px = p[0]; v.py = p[1]; v.pz = p[2];
            v.nx = n[f][0]; v.ny = n[f][1]; v.nz = n[f][2];
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
    m.vertexCount = static_cast<uint32_t>(m.vertices.size());
    mp->meshes.push_back(std::move(m));
    mp->textures.resize(1);
    mp->textures[0].fileId = 701;
    mp->materials.resize(1);
    mp->materials[0].diffuseTex = 0;
    mp->radius = h * std::sqrt(3.0f);
    return mp;
}

castlemist::world::WorldScene one_prop(float x) {
    castlemist::world::WorldScene s;
    s.models.push_back({9001});
    castlemist::world::PropInstance p;
    p.model = 0;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.world[12] = x;
    s.props.push_back(p);
    return s;
}

std::shared_ptr<const CappedTexture> white_701() {
    auto t = std::make_shared<CappedTexture>();
    t->fileId = 701;
    t->width = t->height = 1;
    t->mips.emplace_back(4, uint8_t(255));
    return t;
}

HWND game_window(int w, int h) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cm_test_world_game";
    RegisterClassA(&wc);
    return CreateWindowExA(0, wc.lpszClassName, "world_game test", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

bool centre_pixel(WorldView& v, int w, int h, int rgb[3]) {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "cm_world_game.bmp";
    if (!v.save_screenshot(path.c_str())) return false;
    std::vector<uint8_t> b;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
        std::fclose(f);
    }
    const int stride = (w * 3 + 3) & ~3;
    const size_t at = 54 + static_cast<size_t>(h / 2) * stride + static_cast<size_t>(w / 2) * 3;
    if (b.size() < at + 3) return false;
    rgb[0] = b[at + 2];
    rgb[1] = b[at + 1];
    rgb[2] = b[at];
    return true;
}

} // namespace

CM_TEST(world_game, near_prop_draws_with_its_game_shader) {
    const int W = 160, H = 120;
    HWND wnd = game_window(W, H);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_world(one_prop(400));   // 400 units east of the camera, in range
    v.add_models({{0u, plain_cube()}});
    v.set_shading(WorldShading::Shader);
    WorldCamera cam;   // at the origin, looking east
    cam.farDist = 5000;

    v.render(cam);   // no game data yet: the Materials look, counted pending
    CHECK_EQ(v.stats().shaderInstances, 0);
    CHECK_EQ(v.stats().shaderPending, 1);
    CHECK_EQ(v.stats().visible, 1);

    v.add_game_materials({{0u, game_cube()}});
    v.render(cam);   // built, but texture 701 not in the cache yet: still pending
    CHECK_EQ(v.stats().shaderModels, 1);
    CHECK_EQ(v.stats().shaderPending, 1);
    CHECK_EQ(v.prepare_shader(cam, 0), 1);

    v.add_textures({white_701()});
    CHECK_EQ(v.prepare_shader(cam, 0), 0);
    v.render(cam);
    CHECK_EQ(v.stats().shaderInstances, 1);
    CHECK_EQ(v.stats().shaderDraws, 1);
    CHECK_EQ(v.stats().shaderPending, 0);
    CHECK_EQ(v.stats().visible, 1);
    CHECK_EQ(v.stats().shaderPrograms, 2);
    int px[3];
    CHECK(centre_pixel(v, W, H, px));
    CHECK_NEAR(px[0], 128, 2);   // white * tintc, written as it is (no gamma)
    CHECK_EQ(px[1], 255);
    CHECK_NEAR(px[2], 64, 2);

    // Past the cap: the Materials look again, counted.
    WorldMaterialSettings ms = v.material_settings();
    ms.shaderMaxInstances = 0;
    v.set_material_settings(ms);
    v.render(cam);
    CHECK_EQ(v.stats().shaderInstances, 0);
    CHECK_EQ(v.stats().shaderCapped, 1);
    // Out of range: neither drawn nor counted.
    ms.shaderMaxInstances = 10;
    ms.shaderRange = 100;
    v.set_material_settings(ms);
    v.render(cam);
    CHECK_EQ(v.stats().shaderInstances, 0);
    CHECK_EQ(v.stats().shaderCapped + v.stats().shaderPending + v.stats().shaderNoShader, 0);
    CHECK_EQ(v.stats().visible, 1);
    // Materials shading never uses the game pass.
    ms.shaderRange = 6000;
    v.set_material_settings(ms);
    v.set_shading(WorldShading::Materials);
    v.render(cam);
    CHECK_EQ(v.stats().shaderInstances, 0);
    CHECK_EQ(v.stats().shaderDraws, 0);
}

CM_TEST(world_game, unusable_game_material_falls_back) {
    HWND wnd = game_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_world(one_prop(400));
    v.add_models({{0u, plain_cube()}});
    v.add_textures({white_701()});
    v.set_shading(WorldShading::Shader);
    v.add_game_materials({{0u, game_cube(/*ok=*/false)}});
    WorldCamera cam;
    cam.farDist = 5000;
    CHECK_EQ(v.prepare_shader(cam, 0), 0);
    v.render(cam);
    CHECK_EQ(v.stats().shaderInstances, 0);
    CHECK_EQ(v.stats().shaderNoShader, 1);
    CHECK_EQ(v.stats().shaderModelsFailed, 1);
    CHECK_EQ(v.stats().visible, 1);   // drawn in the Materials look
    // A failed trace (null) counts the same.
    WorldView v2;
    if (!v2.create(wnd)) return;
    v2.set_world(one_prop(400));
    v2.add_models({{0u, plain_cube()}});
    v2.set_shading(WorldShading::Shader);
    v2.add_game_materials({{0u, nullptr}});
    v2.render(cam);
    CHECK_EQ(v2.stats().shaderNoShader, 1);
}
