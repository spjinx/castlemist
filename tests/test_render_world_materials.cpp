/// @file
/// @brief Tests for WorldView's texture cache (src/render/world_materials.h)
///        and the texture list world_texture_ids builds from a scene.

#include "test_framework.h"

#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_prep.h"
#include "castlemist/render/world_view.h"
#include "castlemist/world/terrain_blend.h"

#include "world_materials.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace castlemist::render;
using Microsoft::WRL::ComPtr;

namespace {

CappedTexture solid_texture(uint32_t fileId, int w, int h, int mipCount) {
    CappedTexture t;
    t.fileId = fileId;
    t.width = w;
    t.height = h;
    for (int i = 0; i < mipCount; ++i) {
        const int mw = std::max(1, w >> i), mh = std::max(1, h >> i);
        t.mips.emplace_back(static_cast<size_t>(mw) * mh * 4, static_cast<uint8_t>(40 * (i + 1)));
    }
    return t;
}

ComPtr<ID3D11Device> warp_device() {
    ComPtr<ID3D11Device> dev;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 1, D3D11_SDK_VERSION, &dev,
                                 nullptr, nullptr)))
        dev.Reset();
    return dev;
}

HWND hidden_window(int w, int h) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cm_test_world_materials";
    RegisterClassA(&wc);   // fails harmlessly when already registered
    return CreateWindowExA(0, wc.lpszClassName, "world_materials test", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

/// One mip of w x h RGBA8 from @p px(x, y, rgba).
template <class F>
std::shared_ptr<const CappedTexture> image(uint32_t fileId, int w, int h, F px) {
    auto t = std::make_shared<CappedTexture>();
    t->fileId = fileId;
    t->width = w;
    t->height = h;
    t->mips.emplace_back(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) px(x, y, &t->mips[0][(static_cast<size_t>(y) * w + x) * 4]);
    return t;
}

/// The four 1x1 ground textures 101-104: distinct RGB, alpha 0, 64, 128, 255.
constexpr uint8_t kGround[4][4] = {{200, 60, 40, 0}, {40, 180, 60, 64}, {50, 70, 210, 128}, {230, 220, 90, 255}};

std::vector<std::shared_ptr<const CappedTexture>> ground_textures() {
    std::vector<std::shared_ptr<const CappedTexture>> v;
    for (int k = 0; k < 4; ++k)
        v.push_back(image(101u + k, 1, 1, [k](int, int, uint8_t* o) { std::copy(kGround[k], kGround[k] + 4, o); }));
    return v;
}

/// One flat 17x17 chunk over (0,0)-(1600,1600), material resolved, grounds 101-104.
castlemist::world::WorldScene material_scene() {
    castlemist::world::WorldScene s;
    s.terrain.present = true;
    s.terrain.chunksX = s.terrain.chunksY = 1;
    castlemist::world::TerrainChunk c;
    c.rect[0] = 0; c.rect[1] = 0; c.rect[2] = 1600; c.rect[3] = 1600;
    c.samples = 17;
    c.heights.assign(17 * 17, 0.0f);
    c.material.resolved = true;
    c.material.textureFileIds = {101, 102, 103, 104};
    c.material.normalFileIds = {0, 0, 0, 0};
    c.material.pickerFileId = 201;
    c.material.picker2FileId = 202;
    c.material.pickerOffset[0] = 0.25f;
    c.material.pickerOffset[1] = 0.75f;
    c.material.pickerScale = 0.25f;
    s.terrain.chunks.push_back(std::move(c));
    return s;
}

castlemist::world::BlendTexel texel(const uint8_t b[4], bool srgb) {
    castlemist::world::BlendTexel t{};
    for (int c = 0; c < 4; ++c) {
        const float v = b[c] / 255.0f;
        t.rgba[c] = (srgb && c < 3) ? srgb_to_linear(v) : v;
    }
    return t;
}

} // namespace

CM_TEST(world_materials, texture_cache_counts_bytes_and_failures) {
    ComPtr<ID3D11Device> dev = warp_device();
    if (!dev) SKIP("no WARP device");
    TextureCache c;
    CHECK(c.state(1) == TextureCache::State::Unknown);
    c.add(dev.Get(), solid_texture(1, 8, 8, 2));   // 8x8 + 4x4
    CHECK_EQ(c.ready(), 1);
    CHECK_EQ(c.failed(), 0);
    CHECK_EQ(c.bytes(), size_t(320));
    CHECK(c.state(1) == TextureCache::State::Ready);
    ID3D11ShaderResourceView* srgb = c.srv(1, true);
    ID3D11ShaderResourceView* lin = c.srv(1, false);
    CHECK(srgb != nullptr);
    CHECK(lin != nullptr);
    CHECK(srgb != lin);

    c.add(dev.Get(), solid_texture(1, 8, 8, 2));   // once per fileId
    CHECK_EQ(c.ready(), 1);
    CHECK_EQ(c.bytes(), size_t(320));

    CappedTexture bad;
    bad.fileId = 2;   // empty mips: the decode failed
    c.add(dev.Get(), bad);
    CHECK_EQ(c.failed(), 1);
    CHECK_EQ(c.ready(), 1);
    CHECK(c.state(2) == TextureCache::State::Failed);
    CHECK(c.srv(2, true) == nullptr);
    CHECK(c.srv(2, false) == nullptr);
    CHECK(c.srv(3, false) == nullptr);   // never added

    c.clear();
    CHECK_EQ(c.ready(), 0);
    CHECK_EQ(c.failed(), 0);
    CHECK_EQ(c.bytes(), size_t(0));
    CHECK(c.state(1) == TextureCache::State::Unknown);
    CHECK(c.srv(1, true) == nullptr);
}

CM_TEST(world_materials, texture_ids_cover_terrain_and_water) {
    castlemist::world::WorldScene s;
    s.terrain.present = true;
    castlemist::world::TerrainChunk c0;
    c0.material.textureFileIds = {11, 12, 0, 0};
    c0.material.normalFileIds = {21, 0, 0, 0};
    c0.material.pickerFileId = 31;
    c0.material.picker2FileId = 32;
    castlemist::world::TerrainChunk c1;
    c1.material.textureFileIds = {11, 13, 0, 0};
    c1.material.normalFileIds = {21, 0, 0, 0};
    c1.material.pickerFileId = 0;   // solid page
    c1.material.picker2FileId = 33;
    s.terrain.chunks.push_back(c0);
    s.terrain.chunks.push_back(c1);
    s.environment.water.resize(2);
    s.environment.water[1].textureFileIds = {0, 186359};
    s.environment.light.presetIndex = 1;
    const std::vector<uint32_t> ids = world_texture_ids(s, WorldMaterialSettings{});
    const std::vector<uint32_t> want = {11, 12, 21, 31, 32, 13, 33, 186359};
    CHECK(ids == want);

    // No water preset at presetIndex (or no bump texture there): terrain only.
    s.environment.light.presetIndex = 5;
    CHECK_EQ(world_texture_ids(s, WorldMaterialSettings{}).size(), size_t(7));
    s.environment.light.presetIndex = -1;
    CHECK_EQ(world_texture_ids(s, WorldMaterialSettings{}).size(), size_t(7));
    s.environment.light.presetIndex = 1;
    WorldMaterialSettings cfg;
    cfg.waterBumpTextureIndex = 2;   // past the preset's list
    CHECK_EQ(world_texture_ids(s, cfg).size(), size_t(7));
}

CM_TEST(world_materials, chunk_albedo_matches_cpu_bake) {
    HWND wnd = hidden_window(64, 64);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    CHECK(v.shading() == WorldShading::Materials);
    v.set_world(material_scene());
    std::vector<std::shared_ptr<const CappedTexture>> tex = ground_textures();
    tex.push_back(image(201, 64, 64, [](int x, int y, uint8_t* o) {
        o[0] = static_cast<uint8_t>(4 * x); o[1] = static_cast<uint8_t>(4 * y);
        o[2] = static_cast<uint8_t>(255 - 4 * x); o[3] = static_cast<uint8_t>(2 * (x + y));
    }));
    tex.push_back(image(202, 64, 64, [](int x, int y, uint8_t* o) {
        o[0] = static_cast<uint8_t>(128 + x); o[1] = static_cast<uint8_t>(128 - y); o[2] = 0; o[3] = 0;
    }));
    v.add_textures(tex);

    std::vector<uint8_t> out;
    CHECK(v.capture_chunk_albedo(0, 16, out));
    CHECK_EQ(out.size(), size_t(16 * 16 * 4));
    if (out.size() != 16 * 16 * 4) return;
    int maxErr = 0;
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            const int X = 16 + x, Y = 48 + y;
            const uint8_t pk[4] = {static_cast<uint8_t>(4 * X), static_cast<uint8_t>(4 * Y),
                                   static_cast<uint8_t>(255 - 4 * X), static_cast<uint8_t>(2 * (X + Y))};
            const uint8_t mx[4] = {static_cast<uint8_t>(128 + X), static_cast<uint8_t>(128 - Y), 0, 0};
            castlemist::world::TerrainBlendInput in;
            in.picker = texel(pk, false);
            in.modx = texel(mx, false);
            for (int k = 0; k < 4; ++k) in.ground[k] = texel(kGround[k], true);
            float alb[3], w[4];
            castlemist::world::terrain_blend(in, alb, w);
            for (int c = 0; c < 3; ++c) {
                const int want = static_cast<int>(std::lround(std::clamp(alb[c], 0.0f, 1.0f) * 255.0f));
                const int got = out[(static_cast<size_t>(y) * 16 + x) * 4 + c];
                maxErr = std::max(maxErr, std::abs(want - got));
            }
        }
    std::printf("    chunk_albedo_matches_cpu_bake: max per-channel error %d/255\n", maxErr);
    CHECK(maxErr <= 2);

    // Capturing leaves the swap chain's frame alone: a normal render still works.
    v.render(v.frame_all(nullptr));
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().chunksUntextured, 0);
    CHECK_EQ(v.stats().visible, 1);

    // Geometry shading: the AO look, no capture.
    v.set_shading(WorldShading::Geometry);
    CHECK(v.shading() == WorldShading::Geometry);
    CHECK_FALSE(v.capture_chunk_albedo(0, 16, out));
    v.render(v.frame_all(nullptr));
    CHECK_EQ(v.stats().chunksUntextured, 1);
    v.set_shading(WorldShading::Materials);
    CHECK_FALSE(v.capture_chunk_albedo(1, 16, out));   // no such chunk
    CHECK_FALSE(v.capture_chunk_albedo(0, 0, out));    // no size
}

CM_TEST(world_materials, unresolved_chunk_draws_ao_look) {
    HWND wnd = hidden_window(64, 64);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = material_scene();
    s.terrain.chunks[0].material.resolved = false;
    v.set_world(s);
    v.add_textures(ground_textures());
    v.render(v.frame_all(nullptr));
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().visible, 1);
    CHECK_EQ(v.stats().chunksUntextured, 1);
    std::vector<uint8_t> out;
    CHECK_FALSE(v.capture_chunk_albedo(0, 16, out));

    // Resolved, but the pages 201/202 never arrived: still the AO look.
    v.set_world(material_scene());
    v.add_textures(ground_textures());
    v.render(v.frame_all(nullptr));
    CHECK_EQ(v.stats().chunksUntextured, 1);
    CHECK_FALSE(v.capture_chunk_albedo(0, 16, out));
}

CM_TEST(world_materials, solid_page_is_one_texel) {
    HWND wnd = hidden_window(64, 64);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = material_scene();
    auto& m = s.terrain.chunks[0].material;
    m.pickerFileId = 0;
    m.picker2FileId = 0;
    const uint8_t solid2[4] = {128, 128, 0, 0};
    std::fill(m.pickerSolid, m.pickerSolid + 4, uint8_t(0));
    std::copy(solid2, solid2 + 4, m.picker2Solid);
    v.set_world(s);
    v.add_textures(ground_textures());
    std::vector<uint8_t> out;
    CHECK(v.capture_chunk_albedo(0, 16, out));
    CHECK_EQ(out.size(), size_t(16 * 16 * 4));
    if (out.size() != 16 * 16 * 4) return;
    int maxErr = 0;
    for (size_t p = 0; p < 16 * 16; ++p)
        for (int c = 0; c < 3; ++c) {
            const int want = static_cast<int>(std::lround(srgb_to_linear(kGround[0][c] / 255.0f) * 255.0f));
            maxErr = std::max(maxErr, std::abs(want - static_cast<int>(out[p * 4 + c])));
        }
    CHECK(maxErr <= 2);
}

CM_TEST(world_materials, setting_warnings_name_note_sections) {
    const std::vector<std::string> w = material_setting_warnings(WorldMaterialSettings{});
    auto has = [&](const char* a, const char* b) {
        for (const std::string& l : w)
            if (l.find(a) != std::string::npos && l.find(b) != std::string::npos) return true;
        return false;
    };
    CHECK(has("groundRepeatsPerChunk = 8", "\xc2\xa7" "1.6"));
    CHECK(has("uvScrollUnitSeconds", "\xc2\xa7" "4"));
    CHECK(has("terrainNormalMaps", "UNPROVEN"));
    CHECK(has("pickerSolid", "\xc2\xa7" "1.6"));
    CHECK(has("t = 0", "\xc2\xa7" "5"));
    CHECK(has("waterTileUnit", "\xc2\xa7" "6"));
    CHECK(has("water preset", "\xc2\xa7" "6"));

    HWND wnd = hidden_window(64, 64);
    if (!wnd) return;
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) return;
    WorldMaterialSettings cfg;
    cfg.groundRepeatsPerChunk = 4;
    v.set_material_settings(cfg);
    CHECK_EQ(v.material_settings().groundRepeatsPerChunk, 4.0f);
    bool found = false;
    for (const std::string& l : v.warnings()) found = found || l.find("groundRepeatsPerChunk = 4") != std::string::npos;
    CHECK(found);
}
