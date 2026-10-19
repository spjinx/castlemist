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

CM_TEST(world_materials, chunk_without_colour_textures_draws_ao_look) {
    // Controller ruling P5: resolved, pages present, but all four colour
    // fileIds 0 -- drawing textured would sample null SRVs (black).
    HWND wnd = hidden_window(64, 64);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = material_scene();
    s.terrain.chunks[0].material.textureFileIds = {0, 0, 0, 0};
    v.set_world(s);
    std::vector<std::shared_ptr<const CappedTexture>> tex;
    tex.push_back(image(201, 4, 4, [](int, int, uint8_t* o) { o[0] = o[1] = o[2] = o[3] = 128; }));
    tex.push_back(image(202, 4, 4, [](int, int, uint8_t* o) { o[0] = o[1] = o[2] = o[3] = 128; }));
    v.add_textures(tex);
    v.render(v.frame_all(nullptr));
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().visible, 1);
    CHECK_EQ(v.stats().chunksUntextured, 1);
    std::vector<uint8_t> out;
    CHECK_FALSE(v.capture_chunk_albedo(0, 16, out));
}

// ---- Task 10: prop material pass ----

CM_TEST(world_materials, alpha_mode_rules) {
    ModelMaterialCPU plain;
    CHECK(world_alpha_mode(plain, false, false) == WorldAlpha::Opaque);
    CHECK(world_alpha_mode(plain, true, false) == WorldAlpha::Clip);          // cutout
    CHECK(world_alpha_mode(plain, false, true) == WorldAlpha::Additive);      // looksLikeEffect
    CHECK(world_alpha_mode(plain, true, true) == WorldAlpha::Clip);           // cutout first (export order)
    ModelMaterialCPU fx;
    fx.isEffect = true;
    CHECK(world_alpha_mode(fx, false, false) == WorldAlpha::Additive);

    ModelMaterialCPU blend;
    blend.hasRenderState = true;
    blend.renderState = (5ull << 12) | (6ull << 16);   // SrcAlpha / InvSrcAlpha
    CHECK(world_alpha_mode(blend, false, false) == WorldAlpha::Blend);
    // With a render state, looksLikeEffect does not make it an effect.
    CHECK(world_alpha_mode(blend, false, true) == WorldAlpha::Blend);

    ModelMaterialCPU clip;
    clip.hasRenderState = true;
    clip.renderState = 0;
    clip.materialFile = 512093;   // prop-projector, clips (shader_profiles.cpp)
    CHECK(world_alpha_mode(clip, false, false) == WorldAlpha::Clip);
    ModelMaterialCPU opaque = clip;
    opaque.materialFile = 0;
    CHECK(world_alpha_mode(opaque, false, false) == WorldAlpha::Opaque);
    ModelMaterialCPU add;
    add.hasRenderState = true;
    add.renderState = (5ull << 12) | (2ull << 16);   // SrcAlpha / One
    CHECK(world_alpha_mode(add, false, false) == WorldAlpha::Additive);

    // Ruling P7: each preset's exact factors (bgfx ids: 1 Zero, 2 One,
    // 3 SrcColor, 4 InvSrcColor, 5 SrcAlpha, 6 InvSrcAlpha, 9 DstColor).
    auto with_rs = [](uint64_t src, uint64_t dst) {
        ModelMaterialCPU m;
        m.hasRenderState = true;
        m.renderState = (src << 12) | (dst << 16);
        return m;
    };
    CHECK(world_alpha_mode(with_rs(2, 2), false, false) == WorldAlpha::AdditiveOne);     // One / One
    CHECK(world_alpha_mode(with_rs(2, 6), false, false) == WorldAlpha::Premultiplied);   // Transparent
    CHECK(world_alpha_mode(with_rs(9, 1), false, false) == WorldAlpha::Multiply);        // Multiplicative
    CHECK(world_alpha_mode(with_rs(9, 3), false, false) == WorldAlpha::Multiply2x);      // Multiplicative2x
    CHECK(world_alpha_mode(with_rs(2, 4), false, false) == WorldAlpha::Unmapped);        // SoftAdditive
    CHECK(world_alpha_mode(with_rs(5, 4), false, false) == WorldAlpha::Unmapped);        // Custom
    ModelMaterialCPU transClip = with_rs(5, 6);
    transClip.materialFile = 512093;   // clips: TransClipping
    CHECK(world_alpha_mode(transClip, false, false) == WorldAlpha::Blend);
    // With a render state the texture's flags do not change the blend word's mode.
    CHECK(world_alpha_mode(with_rs(9, 1), true, true, true) == WorldAlpha::Multiply);

    // No render state: a cutout whose surviving texels are all black blends.
    CHECK(world_alpha_mode(plain, true, true, true) == WorldAlpha::Blend);
    CHECK(world_alpha_mode(plain, true, false, true) == WorldAlpha::Blend);
    CHECK(world_alpha_mode(plain, false, false, true) == WorldAlpha::Opaque);   // no cutout: unchanged
    CHECK(world_alpha_mode(plain, false, true, true) == WorldAlpha::Additive);
}

CM_TEST(world_materials, dark_coverage_is_black_where_alpha_passes) {
    auto px = [](std::initializer_list<uint8_t> v) { return std::vector<uint8_t>(v); };
    // Black under alpha 200, anything under alpha < 64 (clipped away).
    CHECK(dark_coverage(px({0, 0, 0, 200, 250, 30, 30, 10, 12, 16, 3, 64})));
    CHECK_FALSE(dark_coverage(px({0, 0, 0, 200, 40, 0, 0, 64})));   // a coloured texel survives
    CHECK_FALSE(dark_coverage(px({0, 0, 0, 10, 0, 0, 0, 63})));      // nothing survives
    CHECK_FALSE(dark_coverage(px({})));
}

CM_TEST(world_materials, prop_materials_pick_uv_and_fileid) {
    ModelPreview m;
    m.textures.resize(1);
    m.textures[0].fileId = 77;
    m.materials.resize(4);
    m.materials[0].diffuseTex = 0;
    m.materials[0].diffuseUv = 0;
    m.materials[1].diffuseTex = 0;
    m.materials[1].diffuseUv = 1;
    m.materials[2].diffuseTex = 0;
    m.materials[2].diffuseUv = 2;
    m.materials[3].diffuseTex = -1;
    const std::vector<PropMaterial> p = prop_materials(m);
    CHECK_EQ(p.size(), size_t(4));
    if (p.size() != 4) return;
    CHECK(p[0].textured);
    CHECK_EQ(p[0].diffuseFileId, 77u);
    CHECK_EQ(int(p[0].uvSet), 0);
    CHECK(p[1].textured);
    CHECK_EQ(p[1].diffuseFileId, 77u);
    CHECK_EQ(int(p[1].uvSet), 1);
    CHECK_FALSE(p[2].textured);
    CHECK_FALSE(p[3].textured);
    CHECK_EQ(p[0].scroll, -1);
}

CM_TEST(world_materials, blend_back_to_front) {
    CHECK(blend_back_to_front({10, 30, 20}) == (std::vector<size_t>{1, 2, 0}));
    CHECK(blend_back_to_front({5, 5}) == (std::vector<size_t>{0, 1}));
    CHECK(blend_back_to_front({}).empty());
}

namespace {

/// A cube (half-size 50) split into two meshes of three faces each: mesh 0
/// uses material 0 (diffuse 501), mesh 1 material 1 (diffuse 502), both UV0.
std::shared_ptr<const ModelPreview> split_cube() {
    auto mp = std::make_shared<ModelPreview>();
    const float h = 50;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    mp->meshes.resize(2);
    for (int f = 0; f < 6; ++f) {
        ModelMeshCPU& m = mp->meshes[static_cast<size_t>(f % 2)];
        const int a = (f / 2 + 1) % 3, b = (f / 2 + 2) % 3;
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (int c = 0; c < 4; ++c) {
            GVertex v{};
            float p[3] = {n[f][0] * h, n[f][1] * h, n[f][2] * h};
            p[a] = (c & 1) ? h : -h;
            p[b] = (c & 2) ? h : -h;
            v.px = p[0]; v.py = p[1]; v.pz = p[2];
            v.nx = n[f][0]; v.ny = n[f][1]; v.nz = n[f][2];
            v.u = (c & 1) ? 1.0f : 0.0f;
            v.v = (c & 2) ? 1.0f : 0.0f;
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
    for (size_t k = 0; k < 2; ++k) {
        mp->meshes[k].materialIndex = static_cast<uint32_t>(k);
        mp->meshes[k].vertexCount = static_cast<uint32_t>(mp->meshes[k].vertices.size());
    }
    mp->textures.resize(2);
    mp->textures[0].fileId = 501;
    mp->textures[1].fileId = 502;
    mp->materials.resize(2);
    for (size_t k = 0; k < 2; ++k) {
        mp->materials[k].index = static_cast<uint32_t>(k);
        mp->materials[k].diffuseTex = static_cast<int>(k);
    }
    mp->radius = h * std::sqrt(3.0f);
    return mp;
}

/// One split_cube at (0, 0, -500) (up = -Z), no terrain.
castlemist::world::WorldScene cube_scene() {
    castlemist::world::WorldScene s;
    s.models.push_back({4321});
    castlemist::world::PropInstance p;
    p.model = 0;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.world[14] = -500;
    s.props.push_back(p);
    return s;
}

/// From the west, south and a little above, looking at the cube.
WorldCamera cube_camera() {
    WorldCamera c;
    c.pos[0] = -300; c.pos[1] = -120; c.pos[2] = -620;
    c.fwd[0] = 300; c.fwd[1] = 120; c.fwd[2] = 120;
    c.farDist = 5000;
    return c;
}

/// Opaque 4x4 diffuse textures 501 (orange) and 502 (blue).
std::vector<std::shared_ptr<const CappedTexture>> cube_textures() {
    std::vector<std::shared_ptr<const CappedTexture>> v;
    v.push_back(image(501, 4, 4, [](int, int, uint8_t* o) { o[0] = 230; o[1] = 120; o[2] = 20; o[3] = 255; }));
    v.push_back(image(502, 4, 4, [](int, int, uint8_t* o) { o[0] = 20; o[1] = 60; o[2] = 220; o[3] = 255; }));
    return v;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

std::string temp_path(const char* name) {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    return std::string(tmp) + name;
}

} // namespace

CM_TEST(world_materials, textured_props_draw_per_submesh) {
    HWND wnd = hidden_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_world(cube_scene());
    v.add_models({{0u, split_cube()}});
    CHECK_EQ(v.stats().modelsLoaded, 1);
    const WorldCamera cam = cube_camera();
    v.render(cam);
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().visible, 1);
    CHECK_EQ(v.stats().draws, 2);   // one bucket, two sub-ranges
    CHECK_EQ(v.stats().materialsUntextured, 2);

    v.add_textures(cube_textures());
    CHECK_EQ(v.stats().textures, 2);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 1);
    CHECK_EQ(v.stats().draws, 2);
    CHECK_EQ(v.stats().materialsUntextured, 0);

    // A failed texture draws its sub-range in the AO look.
    HWND wnd2 = hidden_window(160, 120);
    if (!wnd2) return;
    Closer closer2{wnd2};
    WorldView v2;
    if (!v2.create(wnd2)) return;
    v2.set_world(cube_scene());
    v2.add_models({{0u, split_cube()}});
    std::vector<std::shared_ptr<const CappedTexture>> tex = cube_textures();
    auto bad = std::make_shared<CappedTexture>();
    bad->fileId = 502;   // empty mips: the decode failed
    tex[1] = bad;
    v2.add_textures(tex);
    CHECK_EQ(v2.stats().texturesFailed, 1);
    v2.render(cam);
    CHECK_EQ(v2.stats().draws, 2);
    CHECK_EQ(v2.stats().materialsUntextured, 1);
}

CM_TEST(world_materials, dark_decal_blends_not_black) {
    // Ruling P7: Queensdale's AMAT 58246 decals (rgb 0 under a soft alpha,
    // hasCutout) drew as solid black splats when clipped.
    HWND wnd = hidden_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_world(cube_scene());
    v.add_models({{0u, split_cube()}});
    std::vector<std::shared_ptr<const CappedTexture>> tex = cube_textures();
    // 502 (the faces the camera sees): black, alpha 96 on the left half, 0 on the right.
    auto decal = std::make_shared<CappedTexture>(*image(502, 4, 4, [](int x, int, uint8_t* o) {
        o[0] = o[1] = o[2] = 0;
        o[3] = x < 2 ? 96 : 0;
    }));
    decal->hasCutout = true;
    tex[1] = decal;
    v.add_textures(tex);
    const WorldCamera cam = cube_camera();
    v.render(cam);
    CHECK_EQ(v.stats().draws, 2);
    CHECK_EQ(v.stats().materialsUntextured, 0);
    const std::string path = temp_path("cm_wm_dark_decal.bmp");
    CHECK(v.save_screenshot(path.c_str()));
    const std::vector<uint8_t> bmp = read_file(path);
    CHECK(bmp.size() > 54);
    // Clipped, the decal's texels are rgb 0 at full coverage: pure black.
    // Blended, it darkens the cube's far (orange) faces behind it.
    long black = 0, cube = 0;
    for (size_t i = 54; i + 2 < bmp.size(); i += 3) {
        const int b = bmp[i], g = bmp[i + 1], r = bmp[i + 2];
        if (r + g + b < 6) ++black;
        if (std::abs(r - 26) + std::abs(g - 28) + std::abs(b - 33) > 6) ++cube;   // not the clear colour
    }
    std::printf("    dark_decal_blends_not_black: %ld black px, %ld cube px\n", black, cube);
    CHECK_EQ(black, 0L);
    CHECK(cube > 100);
}

CM_TEST(world_materials, geometry_shading_is_subproject2_look) {
    HWND wa = hidden_window(160, 120);
    HWND wb = hidden_window(160, 120);
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } ca{wa}, cb{wb};
    if (!wa || !wb) SKIP("no window");
    WorldView a, b;
    if (!a.create(wa) || !b.create(wb)) SKIP("no D3D11 device (hardware or WARP)");
    const WorldCamera cam = cube_camera();

    a.set_world(cube_scene());
    a.add_models({{0u, split_cube()}});
    a.add_textures(cube_textures());
    a.set_shading(WorldShading::Geometry);
    a.render(cam);
    const std::string pa = temp_path("cm_wm_geometry_a.bmp");
    CHECK(a.save_screenshot(pa.c_str()));

    b.set_world(cube_scene());   // Materials, but no textures: every sub-range in the AO look
    b.add_models({{0u, split_cube()}});
    b.render(cam);
    CHECK_EQ(b.stats().materialsUntextured, 2);
    const std::string pb = temp_path("cm_wm_geometry_b.bmp");
    CHECK(b.save_screenshot(pb.c_str()));

    a.set_shading(WorldShading::Materials);
    a.render(cam);
    CHECK_EQ(a.stats().materialsUntextured, 0);
    const std::string pm = temp_path("cm_wm_materials_a.bmp");
    CHECK(a.save_screenshot(pm.c_str()));

    const std::vector<uint8_t> ia = read_file(pa), ib = read_file(pb), im = read_file(pm);
    CHECK(!ia.empty());
    CHECK(ia == ib);
    CHECK(ia.size() == im.size());
    CHECK(ia != im);
}

CM_TEST(world_materials, scroll_offset_is_frac) {
    ModelTexTransformCPU t;
    t.columns = t.rows = t.count = 1;
    float o[2];
    t.scroll[0] = 0; t.scroll[1] = 0.2f;
    uv_scroll_offset(t, 2.5, 1.0f, o);
    CHECK(std::fabs(o[0]) < 1e-5f && std::fabs(o[1] - 0.5f) < 1e-5f);
    uv_scroll_offset(t, 6.0, 1.0f, o);
    CHECK(std::fabs(o[1] - 0.2f) < 1e-5f);
    t.scroll[1] = -0.1f;
    uv_scroll_offset(t, 1.0, 1.0f, o);
    CHECK(std::fabs(o[1] - 0.9f) < 1e-5f);
    t.scroll[1] = 0.2f;
    uv_scroll_offset(t, 2.5, 2.0f, o);
    CHECK(std::fabs(o[1] - 0.25f) < 1e-5f);
}

CM_TEST(world_materials, only_scroll_only_entries_animate) {
    ModelTexTransformCPU t;
    t.uvIndex = 1; t.columns = t.rows = t.count = 1;
    t.scroll[1] = 0.2f;
    CHECK(tex_transform_animates(t));
    ModelTexTransformCPU a = t; a.columns = 2;
    CHECK(!tex_transform_animates(a));
    a = t; a.count = 0;
    CHECK(!tex_transform_animates(a));
    a = t; a.scale[0] = a.scale[1] = 1;
    CHECK(!tex_transform_animates(a));
    a = t; a.rotate = 0.1f;
    CHECK(!tex_transform_animates(a));
    a = t; a.scroll[0] = a.scroll[1] = 0;   // nothing scrolls: not an animating entry
    CHECK(!tex_transform_animates(a));
}

CM_TEST(world_materials, scroll_entry_matches_diffuse_uv) {
    ModelPreview m;
    m.textures.resize(1);
    m.textures[0].fileId = 7;
    m.materials.resize(1);
    ModelMaterialCPU& mat = m.materials[0];
    mat.diffuseTex = 0;
    mat.diffuseUv = 0;
    ModelTexTransformCPU e;
    e.columns = e.rows = e.count = 1;
    e.scroll[1] = 0.2f;
    e.uvIndex = 1; mat.texTransforms.push_back(e);
    e.uvIndex = 0; mat.texTransforms.push_back(e);
    int still = 0;
    const std::vector<PropMaterial> pm = prop_materials(m, &still);
    CHECK_EQ(pm[0].scroll, 1);
    CHECK_EQ(still, 0);
    CHECK_EQ(pm[0].scrollRate[1], 0.2f);
}

CM_TEST(world_materials, setting_warnings_name_dark_decal_rule) {
    bool found = false;
    for (const std::string& l : material_setting_warnings(WorldMaterialSettings{}))
        found = found || (l.find("dark decals") != std::string::npos && l.find("58246") != std::string::npos &&
                          l.find("castlemist's choice") != std::string::npos &&
                          l.find("research note") != std::string::npos && l.find("\xc2\xa7" "3") == std::string::npos);
    CHECK(found);
}

CM_TEST(world_materials, setting_warnings_name_water_alpha) {
    bool found = false;
    for (const std::string& l : material_setting_warnings(WorldMaterialSettings{}))
        found = found || (l.find("water alpha") != std::string::npos && l.find("0.45") != std::string::npos &&
                          l.find("castlemist's choice") != std::string::npos);
    CHECK(found);
}

CM_TEST(world_view, motion_off_freezes_time) {
    HWND wnd = hidden_window(64, 64);
    if (!wnd) return;
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) return;
    CHECK(v.motion());
    v.set_time(2.5);
    v.set_motion(false);
    CHECK_EQ(v.time(), 2.5);   // frozen where it was, not rewound to 0
    CHECK(!v.animating());
    v.set_motion(true);
    CHECK_EQ(v.time(), 2.5);   // resumes from there
    v.set_time(3.0);
    CHECK_EQ(v.time(), 3.0);
}

// ---------------------------------------------------------------------------
// Task 12: skinned props (spec SkinnedPropPass, note §5)
// ---------------------------------------------------------------------------

namespace {

/// Two joints: a root at the origin and a child at (0, 0, 10) whose
/// InverseWorld translates by -10 (row-vector, translation in the last row).
void two_joints(std::vector<ModelJoint>& j) {
    j.resize(2);
    j[1].parent = 0;
    j[1].pos[2] = 10;
    j[1].localPos[2] = 10;
    j[1].invWorld[14] = -10;
    j[0].name = "root";
    j[1].name = "child";
}

/// PoseRig over @p j with @p a, as add_models builds it.
PoseRig rig_of(const std::vector<ModelJoint>& j, const castlemist::granny::Anim& a) {
    PoseRig rig;
    for (const ModelJoint& b : j)
        rig.bones.push_back({b.name.c_str(), b.parent, b.localPos, b.localQuat, b.localScale, b.invWorld});
    rig.clip = &a;
    rig.trackByName = castlemist::granny::trackIndexByName(a);
    return rig;
}

/// The child's position: (0, 0, 10) before t = 0.5, (0, 3000, 10) after
/// (degree 0: held from each knot).
castlemist::granny::Track jump_track() {
    castlemist::granny::Track t;
    t.name = "child";
    t.pos.fmt = castlemist::granny::F_Constant32f;
    t.pos.degree = 0;
    t.pos.dim = 3;
    t.pos.knots = {0.0f, 0.5f};
    t.pos.controls = {0, 0, 10, 0, 3000, 10};
    return t;
}

/// split_cube rigid-bound to joint 1 (weight 1, model_preview.cpp:329-333),
/// @p joints joints (two_joints, then identity roots), clip "on" (463) of
/// duration 1 with the child's track @p track (none when null).
std::shared_ptr<const ModelPreview> rigged_cube(size_t joints, const castlemist::granny::Track* track = nullptr) {
    auto mp = std::make_shared<ModelPreview>(*split_cube());
    two_joints(mp->joints);
    for (size_t k = 2; k < joints; ++k) {
        ModelJoint e;
        e.name = "extra" + std::to_string(k);
        mp->joints.push_back(e);
    }
    for (ModelMeshCPU& m : mp->meshes) {
        m.hasSkin = true;
        for (GVertex& g : m.vertices) {
            g.bidx[0] = 1; g.bidx[1] = g.bidx[2] = g.bidx[3] = 0;
            g.bwt[0] = 1; g.bwt[1] = g.bwt[2] = g.bwt[3] = 0;
        }
    }
    castlemist::granny::Anim a;
    a.valid = true;
    a.name = "on";
    a.duration = 1;
    if (track) a.tracks.push_back(*track);
    mp->animClips = {a};
    mp->animClipTokens = {463};
    return mp;
}

WorldModelLoad skinned_load(std::shared_ptr<const ModelPreview> p) {
    auto prep = prepare_world_model_shared(*p, true);
    return WorldModelLoad(0u, std::move(p), std::move(prep));
}

/// Pixels of @p bmp (a save_screenshot BMP) that are not the clear colour.
long non_clear(const std::vector<uint8_t>& bmp) {
    long n = 0;
    for (size_t i = 54; i + 2 < bmp.size(); i += 3) {
        const int b = bmp[i], g = bmp[i + 1], r = bmp[i + 2];
        if (std::abs(r - 26) + std::abs(g - 28) + std::abs(b - 33) > 6) ++n;
    }
    return n;
}

} // namespace

CM_TEST(world_materials, pose_palette_bind_is_identity) {
    std::vector<ModelJoint> j;
    two_joints(j);
    castlemist::granny::Anim a;
    a.valid = true;
    a.duration = 1;
    const PoseRig rig = rig_of(j, a);
    std::vector<float> pal;
    pose_palette(rig, 0.3, pal);
    CHECK_EQ(pal.size(), size_t{32});
    for (size_t b = 0; b < 2 && pal.size() == 32; ++b)
        for (int k = 0; k < 16; ++k) CHECK_NEAR(pal[b * 16 + k], (k % 5 == 0) ? 1.0f : 0.0f, 1e-5f);
}

CM_TEST(world_materials, pose_palette_zero_duration_no_nan) {
    std::vector<ModelJoint> j;
    two_joints(j);
    castlemist::granny::Anim a;
    a.valid = true;
    a.duration = 0;
    a.tracks.push_back(jump_track());
    const PoseRig rig = rig_of(j, a);
    std::vector<float> pal;
    pose_palette(rig, 7.3, pal);
    CHECK_EQ(pal.size(), size_t{32});
    for (float f : pal) CHECK(std::isfinite(f));
}

CM_TEST(world_materials, clip_time_wraps) {
    CHECK_NEAR(clip_time(2.5, 1.0f), 0.5, 1e-9);
    CHECK_EQ(clip_time(3.0, 0.0f), 0.0);
    CHECK_EQ(clip_time(0.0, 2.0f), 0.0);
    CHECK_NEAR(clip_time(-0.25, 1.0f), 0.75, 1e-9);
    CHECK_EQ(clip_time(std::nan(""), 1.0f), 0.0);
}

CM_TEST(world_materials, decode_failed_clip_is_not_a_miss) {
    HWND wnd = hidden_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = cube_scene();
    s.props[0].animSequence = 463;   // a clip the model has but that failed to decode
    s.props.push_back(s.props[0]);
    s.props[1].animSequence = 6351;  // a clip the model does not have at all
    s.props[1].world[13] = 300;
    v.set_world(s);
    auto mp = std::const_pointer_cast<ModelPreview>(rigged_cube(2));
    mp->animationTokens = {463};
    mp->animClips.clear();
    mp->animClipTokens.clear();
    v.add_models({skinned_load(mp)});
    CHECK_EQ(v.stats().animated, 0);
    CHECK_EQ(v.stats().unresolvedClips, 2);
    CHECK_EQ(v.stats().clipsFailedDecode, 1);
    bool failed = false, miss = false;
    for (const std::string& l : v.warnings()) {
        failed = failed || (l.find("1 animated placements name a clip that failed to decode") != std::string::npos);
        miss = miss || (l.find("1 animated placements name no clip") != std::string::npos);
    }
    CHECK(failed);
    CHECK(miss);
}

CM_TEST(world_materials, animated_instances_counted) {
    for (const size_t joints : {size_t{2}, size_t{300}}) {
        HWND wnd = hidden_window(160, 120);
        if (!wnd) SKIP("no window");
        struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
        WorldView v;
        if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
        castlemist::world::WorldScene s = cube_scene();
        s.props[0].animSequence = 463;    // "on": the model's clip
        s.props.push_back(s.props[0]);
        s.props[1].animSequence = 6351;   // "off": not in the bank
        s.props[1].world[13] = 300;
        s.props.push_back(s.props[0]);
        s.props[2].animSequence = 0;      // propArray: no clip
        s.props[2].world[13] = -300;
        v.set_world(s);
        v.add_models({skinned_load(rigged_cube(joints))});
        CHECK_EQ(v.stats().modelsLoaded, 1);
        v.render(cube_camera());
        CHECK_FALSE(v.device_lost());
        bool warned = false;
        for (const std::string& l : v.warnings())
            warned = warned || (l.find("1 animated placement") != std::string::npos &&
                                l.find("note \xc2\xa7" "5") != std::string::npos);
        CHECK(warned);
        WorldCamera cam = cube_camera();
        int rect[4] = {0, 0, 0, 0};
        if (joints == 2) {
            CHECK_EQ(v.stats().animated, 1);
            CHECK_EQ(v.stats().unresolvedClips, 1);
            CHECK(v.animating());
            CHECK(v.focus_animated(cam));
            // 4x the padded radius (1.5 x 50 sqrt 3) back along the view direction from the cube's centre.
            const float dx = cam.pos[0], dy = cam.pos[1], dz = cam.pos[2] + 500;
            CHECK_NEAR(std::sqrt(dx * dx + dy * dy + dz * dz), 4.0f * 1.5f * 50.0f * std::sqrt(3.0f), 0.5f);
            CHECK(v.animated_screen_rect(cam, rect));
            CHECK(rect[2] > rect[0] && rect[3] > rect[1]);
            v.set_motion(false);
            CHECK_FALSE(v.animating());
        } else {
            CHECK_EQ(v.stats().animated, 0);
            CHECK_EQ(v.stats().unresolvedClips, 2);
            CHECK_FALSE(v.animating());
            CHECK_FALSE(v.focus_animated(cam));
            CHECK_FALSE(v.animated_screen_rect(cam, rect));
        }
    }
}

CM_TEST(world_materials, skinned_prop_follows_its_clip) {
    HWND wnd = hidden_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = cube_scene();
    s.props[0].animSequence = 463;
    v.set_world(s);
    const castlemist::granny::Track jump = jump_track();
    v.add_models({skinned_load(rigged_cube(2, &jump))});
    CHECK_EQ(v.stats().animated, 1);
    const WorldCamera cam = cube_camera();
    auto shot = [&](const char* name) {
        v.render(cam);
        const std::string p = temp_path(name);
        CHECK(v.save_screenshot(p.c_str()));
        return read_file(p);
    };
    v.set_time(0.1);   // bind pose: the cube where it stands
    const std::vector<uint8_t> bind = shot("cm_wm_skin_bind.bmp");
    v.set_time(1.7);   // wraps to 0.7: the child has jumped 3000 along +Y, out of view
    const std::vector<uint8_t> moved = shot("cm_wm_skin_moved.bmp");
    v.set_time(0.1);
    v.set_motion(false);   // frozen at the time it had: bind pose again
    const std::vector<uint8_t> frozen = shot("cm_wm_skin_frozen.bmp");
    v.set_motion(true);
    v.set_shading(WorldShading::Geometry);   // Geometry draws everything static
    const std::vector<uint8_t> geo = shot("cm_wm_skin_geometry.bmp");
    std::printf("    skinned_prop_follows_its_clip: bind %ld, moved %ld, frozen %ld, geometry %ld px\n",
                non_clear(bind), non_clear(moved), non_clear(frozen), non_clear(geo));
    CHECK(non_clear(bind) > 100);
    CHECK_EQ(non_clear(moved), 0L);
    CHECK(bind == frozen);
    CHECK(non_clear(geo) > 100);

    // Textured: kPropSkinned's PSMain, one draw per sub-range, same motion.
    v.set_shading(WorldShading::Materials);
    v.add_textures(cube_textures());
    v.set_time(0.1);
    const long texBind = non_clear(shot("cm_wm_skin_tex_bind.bmp"));
    CHECK_EQ(v.stats().draws, 2);
    CHECK_EQ(v.stats().materialsUntextured, 0);
    v.set_time(0.7);
    const long texMoved = non_clear(shot("cm_wm_skin_tex_moved.bmp"));
    CHECK(texBind > 100);
    CHECK_EQ(texMoved, 0L);

    // In bind pose the skinned AO look is the static one, byte for byte.
    HWND w2 = hidden_window(160, 120);
    if (!w2) return;
    Closer c2{w2};
    WorldView st;
    if (!st.create(w2)) return;
    st.set_world(cube_scene());
    st.add_models({{0u, split_cube()}});
    st.render(cam);
    const std::string ps = temp_path("cm_wm_skin_static.bmp");
    CHECK(st.save_screenshot(ps.c_str()));
    CHECK(read_file(ps) == bind);
}

// ---------------------------------------------------------------------------
// Task 13: water from the env preset (spec WaterPass, note §6)
// ---------------------------------------------------------------------------

CM_TEST(world_materials, water_colour_stored_order) {
    const uint8_t stored[4] = {0xA5, 0xDF, 0x5C, 0};
    float rgb[3];
    water_colour(stored, rgb);
    CHECK(std::fabs(rgb[0] - 165.0f / 255.0f) < 1e-6f);
    CHECK(std::fabs(rgb[1] - 223.0f / 255.0f) < 1e-6f);
    CHECK(std::fabs(rgb[2] - 92.0f / 255.0f) < 1e-6f);
}

CM_TEST(world_materials, water_bump_uv_units) {
    float uv[2];
    water_bump_uv(1000, 0, 1, 0, 0, 0, 1000, uv);
    CHECK(std::fabs(uv[0] - 1) < 1e-5f && std::fabs(uv[1]) < 1e-5f);
    water_bump_uv(1000, 0, 1, 0, 3.14159265f, 0, 1000, uv);
    CHECK(std::fabs(uv[0] + 1) < 1e-5f && std::fabs(uv[1]) < 1e-5f);
    water_bump_uv(1000, 0, 1, 0, 3.14159265f / 2, 0, 1000, uv);
    CHECK(std::fabs(uv[0]) < 1e-5f && std::fabs(uv[1] - 1) < 1e-5f);
    water_bump_uv(1000, 0, 1, 0.5f, 0, 2, 1000, uv);
    CHECK(std::fabs(uv[0] - 2) < 1e-5f && std::fabs(uv[1]) < 1e-5f);
}

CM_TEST(world_materials, water_bump_angle_and_strength_settings) {
    WorldMaterialSettings cfg;
    CHECK(cfg.waterBumpAngleDegrees);
    CHECK_EQ(cfg.waterBumpStrengthScale, 0.02f);
    CHECK(std::fabs(water_bump_angle(180.0f, true) - 3.14159265f) < 1e-5f);
    CHECK(std::fabs(water_bump_angle(143.324f, true) - 2.50148f) < 1e-4f);
    CHECK_EQ(water_bump_angle(1.5f, false), 1.5f);
    CHECK(std::fabs(water_bump_strength(23.7268f, 0.02f) - 0.474536f) < 1e-5f);
    CHECK_EQ(water_bump_strength(23.7268f, 0.0f), 0.0f);
    CHECK_EQ(water_bump_strength(-1.0f, 0.02f), 0.0f);   // a negative amount flips no normals
    bool angle = false, scale = false, sign = false;
    for (const std::string& l : material_setting_warnings(cfg)) {
        angle = angle || (l.find("waterBumpAngleDegrees = on") != std::string::npos &&
                          l.find("UNPROVEN") != std::string::npos && l.find("\xc2\xa7" "6") != std::string::npos);
        sign = sign || (l.find("water bump: castlemist's choice") != std::string::npos &&
                        l.find("sign UNPROVEN") != std::string::npos && l.find("§" "6") != std::string::npos);
        scale = scale || (l.find("waterBumpStrengthScale = 0.02") != std::string::npos &&
                          l.find("castlemist's choice") != std::string::npos &&
                          l.find("\xc2\xa7" "6") != std::string::npos);
    }
    CHECK(angle);
    CHECK(scale);
    CHECK(sign);
}

CM_TEST(world_materials, water_depth_mix) {
    CHECK(std::fabs(water_depth_mix(15.66f, 0, 15.66f) - 1) < 1e-6f);
    CHECK_EQ(water_depth_mix(-5, 0, 15), 0.0f);   // terrain above the plane (up is -Z)
    CHECK(std::fabs(water_depth_mix(7.5f, 0, 15) - 0.5f) < 1e-6f);
    CHECK_EQ(water_depth_mix(30, 0, 15), 1.0f);
    CHECK_EQ(water_depth_mix(10, 0, 0), 0.0f);
    CHECK_EQ(water_depth_mix(10, 0, -1), 0.0f);
}

CM_TEST(world_view, water_preset_used_when_in_range) {
    HWND wnd = hidden_window(96, 72);
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    if (!wnd) SKIP("no window");
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    castlemist::world::WorldScene s = material_scene();
    for (float& z : s.terrain.chunks[0].heights) z = 10;   // 10 below the plane at 0
    s.water.hasPlane = true;
    s.water.planeZ = 0;
    s.environment.water.resize(2);
    for (auto& w : s.environment.water) {
        const uint8_t sh[4] = {0xA5, 0xDF, 0x5C, 0}, dp[4] = {0x36, 0x4F, 0x21, 0};
        std::copy(sh, sh + 4, w.surfaceShallowColor);
        std::copy(dp, dp + 4, w.surfaceDeepColor);
        w.depthAttenuation = 15.66f;
        w.surfaceFresnel = 3.89f;
        w.bumpTile[0] = 5.01f; w.bumpTile[1] = 13.66f;
        w.bumpSpeed[0] = 1.31f; w.bumpSpeed[1] = 0.64f;
        w.textureFileIds = {0, 186359};
    }
    WorldCamera cam;
    cam.pos[0] = 800; cam.pos[1] = 800; cam.pos[2] = -900;
    cam.fwd[0] = 0; cam.fwd[1] = 0.2f; cam.fwd[2] = 1;
    cam.up[0] = 0; cam.up[1] = -1; cam.up[2] = 0;
    cam.farDist = 5000;
    auto no_preset_warning = [&] {
        for (const std::string& l : v.warnings())
            if (l.find("water: no preset for lighting preset") != std::string::npos &&
                l.find("note \xc2\xa7" "6") != std::string::npos)
                return true;
        return false;
    };

    s.environment.light.presetIndex = 1;
    v.set_world(s);
    v.set_time(1.5);
    v.render(cam);
    CHECK(v.stats().waterPreset);
    CHECK(!no_preset_warning());
    CHECK(v.animating());
    const std::string pp = temp_path("cm_wm_water_preset.bmp");
    CHECK(v.save_screenshot(pp.c_str()));

    s.environment.light.presetIndex = 5;
    v.set_world(s);
    v.render(cam);
    CHECK(!v.stats().waterPreset);
    CHECK(no_preset_warning());
    const std::string pf = temp_path("cm_wm_water_flat.bmp");
    CHECK(v.save_screenshot(pf.c_str()));
    const std::vector<uint8_t> ip = read_file(pp), ifl = read_file(pf);
    CHECK(!ip.empty() && ip.size() == ifl.size());
    CHECK(ip != ifl);

    // Geometry keeps the flat sub-project 2 water even with a preset.
    s.environment.light.presetIndex = 1;
    v.set_world(s);
    v.set_shading(WorldShading::Geometry);
    v.render(cam);
    const std::string pg = temp_path("cm_wm_water_geometry.bmp");
    CHECK(v.save_screenshot(pg.c_str()));
    WorldView f;
    HWND wf = hidden_window(96, 72);
    struct Closer2 { HWND h; ~Closer2() { if (h) DestroyWindow(h); } } cf{wf};
    if (!wf || !f.create(wf)) return;
    castlemist::world::WorldScene flat = s;
    flat.environment.water.clear();
    f.set_world(flat);
    f.set_shading(WorldShading::Geometry);
    f.render(cam);
    const std::string pg2 = temp_path("cm_wm_water_geometry_ref.bmp");
    CHECK(f.save_screenshot(pg2.c_str()));
    CHECK(read_file(pg) == read_file(pg2));
}

namespace {

/// cube_scene's split_cube as a zone Collider instance instead of a prop: one
/// type-3 layer with fadeRange [n, f].
castlemist::world::WorldScene zone_cube_scene(float n, float f) {
    castlemist::world::WorldScene s;
    s.models.push_back({4321});
    castlemist::world::ZoneLayer L;
    L.type = 3;
    L.fadeRange[0] = n; L.fadeRange[1] = f;
    s.zones.present = true;
    s.zones.layers.push_back(L);
    castlemist::world::ZoneInstance z;
    z.model = 0;
    z.layer = 0;
    z.world[0] = z.world[5] = z.world[10] = z.world[15] = 1;
    z.world[14] = -500;
    s.zones.instances.push_back(z);
    return s;
}

} // namespace

// Spec D6 "Draw": textured foliage takes the material pass per sub-range,
// and the screen door drops about half its pixels at fade 0.5.
CM_TEST(world_materials, textured_foliage_draws_and_dithers) {
    HWND wnd = hidden_window(160, 120);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { if (h) DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    const WorldCamera cam = cube_camera();
    const float d = std::sqrt(300.0f * 300.0f + 120.0f * 120.0f + 120.0f * 120.0f);   // camera to the cube centre
    v.set_world(zone_cube_scene(d + 1000, d + 2000));
    v.add_models({{0u, split_cube()}});
    v.add_textures(cube_textures());
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 1);
    CHECK_EQ(v.stats().foliageDraws, 2);   // one bucket, two textured sub-ranges
    CHECK_EQ(v.stats().materialsUntextured, 0);
    const std::string full = temp_path("cm_wm_foliage_full.bmp");
    CHECK(v.save_screenshot(full.c_str()));

    // fade 0.5: the range [d - 500, d + 500], with the quality factor undone.
    v.set_foliage_distance_scale(1.0f / v.foliage_fade_quality_scale());
    v.set_world(zone_cube_scene(d - 500, d + 500));
    v.add_models({{0u, split_cube()}});
    v.add_textures(cube_textures());
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 1);
    const std::string half = temp_path("cm_wm_foliage_half.bmp");
    CHECK(v.save_screenshot(half.c_str()));

    // The same cube as a prop draws byte-identical pixels (the foliage PS is kPropMat's).
    v.set_world(cube_scene());
    v.add_models({{0u, split_cube()}});
    v.add_textures(cube_textures());
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 0);
    const std::string prop = temp_path("cm_wm_foliage_prop.bmp");
    CHECK(v.save_screenshot(prop.c_str()));

    const std::vector<uint8_t> bf = read_file(full), bh = read_file(half);
    const long nf = non_clear(bf), nh = non_clear(bh);
    std::printf("      textured foliage: %ld px, half faded %ld px\n", nf, nh);
    CHECK(nf > 500);
    CHECK(bf == read_file(prop));
    CHECK(nh > nf * 3 / 10);
    CHECK(nh < nf * 7 / 10);
}
