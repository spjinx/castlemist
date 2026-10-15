/// @file
/// @brief Tests for the CPU terrain albedo and water bake (world/terrain_bake.h).

#include "test_framework.h"

#include "castlemist/world/terrain_bake.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

using namespace castlemist::world;

namespace {

// 8x8 RGBA texture with two mips (8 and 4), texel colours from fn(x, y, mip, out).
template <class F>
std::shared_ptr<CappedTexture> tex8(uint32_t id, F fn) {
    auto t = std::make_shared<CappedTexture>();
    t->fileId = id; t->baseId = id; t->width = 8; t->height = 8;
    for (int mip = 0; mip < 2; ++mip) {
        const int n = 8 >> mip;
        std::vector<uint8_t> px(static_cast<size_t>(n) * n * 4);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) fn(x, y, mip, &px[(static_cast<size_t>(y) * n + x) * 4]);
        t->mips.push_back(std::move(px));
    }
    return t;
}

std::shared_ptr<CappedTexture> flat8(uint32_t id, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return tex8(id, [=](int, int, int, uint8_t* o) { o[0] = r; o[1] = g; o[2] = b; o[3] = a; });
}

struct Store {
    std::map<uint32_t, std::shared_ptr<CappedTexture>> m;
    void add(std::shared_ptr<CappedTexture> t) { m[t->fileId] = std::move(t); }
    TextureLookup lookup() const {
        return [this](uint32_t id) -> const CappedTexture* {
            auto it = m.find(id);
            return it == m.end() ? nullptr : it->second.get();
        };
    }
};

TerrainChunk chunk() {
    TerrainChunk c;
    c.samples = 2;
    c.rect[0] = 0; c.rect[1] = 0; c.rect[2] = 100; c.rect[3] = 100;
    c.material.resolved = true;
    c.material.textureFileIds = {10, 11, 12, 13};
    c.material.pickerScale = 0.5f;
    c.material.pickerOffset[0] = 0.5f; c.material.pickerOffset[1] = 0.0f;
    return c;
}

Store ground_store() {
    Store s;
    for (uint32_t k = 0; k < 4; ++k)
        s.add(tex8(10 + k, [k](int x, int y, int mip, uint8_t* o) {
            o[0] = static_cast<uint8_t>(40 + 50 * k + 5 * x); o[1] = static_cast<uint8_t>(200 - 30 * k + 3 * y);
            o[2] = static_cast<uint8_t>(10 * k + 20 * mip + x * y); o[3] = static_cast<uint8_t>(100 + 20 * k + 2 * x);
        }));
    s.add(tex8(20, [](int x, int y, int, uint8_t* o) {
        o[0] = static_cast<uint8_t>(30 * x); o[1] = static_cast<uint8_t>(255 - 20 * y);
        o[2] = static_cast<uint8_t>(15 * (x + y)); o[3] = static_cast<uint8_t>(32 * y);
    }));
    s.add(tex8(21, [](int x, int y, int, uint8_t* o) {
        o[0] = static_cast<uint8_t>(110 + 4 * x); o[1] = static_cast<uint8_t>(120 + 3 * y); o[2] = 0; o[3] = 0;
    }));
    return s;
}

} // namespace

CM_TEST(bake, mip_level) {
    CHECK_EQ(ground_mip_level(512, 8, 256, 8), 4);
    CHECK_EQ(ground_mip_level(512, 8, 512, 8), 3);
    CHECK_EQ(ground_mip_level(4, 1, 256, 3), 0);
    CHECK_EQ(ground_mip_level(4096, 8, 64, 3), 2);
}

CM_TEST(bake, srgb_roundtrip) {
    CHECK_NEAR(linear_to_srgb(0.0f), 0.0f, 1e-6);
    CHECK_NEAR(linear_to_srgb(1.0f), 1.0f, 1e-5);
    CHECK_NEAR(linear_to_srgb(0.2140411f), 0.5f, 1e-4);
}

CM_TEST(bake, texel_equals_blend) {
    Store st = ground_store();
    TerrainChunk c = chunk();
    c.material.pickerFileId = 20; c.material.picker2FileId = 21;
    BakeSettings s; s.texelsPerChunk = 4; s.groundRepeats = 1.0f; s.srgbColour = true;
    std::vector<uint8_t> out;
    CHECK(bake_chunk_albedo(c, st.lookup(), s, out));
    CHECK_EQ(out.size(), size_t(4 * 4 * 3));
    if (out.size() != 4 * 4 * 3) return;
    const int mip = ground_mip_level(8, 1.0f, 4, 2);
    CHECK_EQ(mip, 1);
    int maxErr = 0;
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            const float sx = (i + 0.5f) / 4, ty = (j + 0.5f) / 4;
            float puv[2], guv[2];
            page_uv(c.material, sx, ty, puv);
            ground_uv(sx, ty, 1.0f, guv);
            TerrainBlendInput in;
            in.picker = sample_bilinear(*st.m[20], 0, puv[0], puv[1], false, false);
            in.modx = sample_bilinear(*st.m[21], 0, puv[0], puv[1], false, false);
            for (int k = 0; k < 4; ++k) in.ground[k] = sample_bilinear(*st.m[10 + k], mip, guv[0], guv[1], true, true);
            float alb[3], w[4];
            terrain_blend(in, alb, w);
            for (int ch = 0; ch < 3; ++ch) {
                const int want = static_cast<int>(std::lround(linear_to_srgb(std::clamp(alb[ch], 0.0f, 1.0f)) * 255.0f));
                maxErr = std::max(maxErr, std::abs(want - int(out[(size_t(j) * 4 + i) * 3 + ch])));
            }
        }
    CHECK(maxErr <= 1);
}

CM_TEST(bake, solid_page_constant) {
    Store st;
    st.add(flat8(10, 200, 100, 50, 255)); st.add(flat8(11, 0, 0, 0, 255));
    st.add(flat8(12, 0, 0, 0, 255));      st.add(flat8(13, 90, 160, 220, 255));
    TerrainChunk c = chunk();
    c.material.pickerFileId = 0; c.material.picker2FileId = 0;
    c.material.pickerSolid[0] = 255;                       // R: all weight on colord
    c.material.picker2Solid[0] = 128; c.material.picker2Solid[1] = 128;   // modx 0.5 : brightness/saturation near identity
    BakeSettings s; s.texelsPerChunk = 4; s.srgbColour = false;
    std::vector<uint8_t> out;
    CHECK(bake_chunk_albedo(c, st.lookup(), s, out));
    CHECK_EQ(out.size(), size_t(48));
    if (out.size() != 48) return;
    const float k = 2 * (128 / 255.0f);
    const float lum = 0.30f * 90 + 0.59f * 160 + 0.11f * 220;
    const float want[3] = {k * (lum + (90 - lum) * k), k * (lum + (160 - lum) * k), k * (lum + (220 - lum) * k)};
    for (size_t t = 0; t < 16; ++t)
        for (int ch = 0; ch < 3; ++ch) CHECK_NEAR(out[t * 3 + ch], want[ch], 1.5);
}

CM_TEST(bake, absent_slot_zero_weight) {
    Store st;
    st.add(flat8(10, 200, 100, 50, 255)); st.add(flat8(11, 0, 0, 0, 255));
    st.add(flat8(13, 0, 0, 255, 255));
    TerrainChunk c = chunk();
    c.material.textureFileIds = {10, 11, 0, 13};            // slot 2 (colorc) absent
    c.material.pickerSolid[1] = 255;                        // pure G: colorc, which is absent
    c.material.picker2Solid[0] = 128; c.material.picker2Solid[1] = 128;
    BakeSettings s; s.texelsPerChunk = 2; s.srgbColour = false;
    std::vector<uint8_t> out;
    CHECK(bake_chunk_albedo(c, st.lookup(), s, out));
    if (out.size() != 12) { CHECK_EQ(out.size(), size_t(12)); return; }
    const float k = 2 * (128 / 255.0f);
    const float lum = 0.30f * 200 + 0.59f * 100 + 0.11f * 50;
    // Falls back to color (slot 0), not the missing colorc.
    CHECK_NEAR(out[0], k * (lum + (200 - lum) * k), 1.5);
    CHECK_NEAR(out[1], k * (lum + (100 - lum) * k), 1.5);
    CHECK_NEAR(out[2], k * (lum + (50 - lum) * k), 1.5);
}

CM_TEST(bake, no_colour_bakes_grey) {
    Store st;                                               // nothing available
    BakeSettings s; s.texelsPerChunk = 4;
    std::vector<uint8_t> out;
    CHECK_FALSE(bake_chunk_albedo(chunk(), st.lookup(), s, out));
    CHECK_EQ(out.size(), size_t(48));
    CHECK(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 128; }));
    // Unresolved chunk: same, even with textures present.
    Store full = ground_store();
    TerrainChunk c = chunk(); c.material.resolved = false;
    CHECK_FALSE(bake_chunk_albedo(c, full.lookup(), s, out));
    CHECK(std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 128; }));
}

namespace {
WorldScene water_scene(float terrainZ) {
    WorldScene w;
    w.terrain.present = true; w.terrain.chunksX = 1; w.terrain.chunksY = 1;
    TerrainChunk c = chunk();
    c.heights.assign(4, terrainZ);
    w.terrain.chunks.push_back(c);
    w.water.hasPlane = true; w.water.planeZ = 0;
    castlemist::model::Extractor::MapEnvWater p;
    p.depthAttenuation = 10;
    p.surfaceShallowColor[2] = 255;
    p.surfaceDeepColor[0] = 255;
    w.environment.water.push_back(p);
    w.environment.light.presetIndex = 0;
    return w;
}
} // namespace

CM_TEST(bake, water_depth) {
    WorldScene w = water_scene(5.0f);
    const float rect[4] = {0, 0, 100, 100};
    std::vector<uint8_t> out;
    CHECK(bake_cell_water(w, rect, 4, 2, false, out));
    CHECK_EQ(out.size(), size_t(4 * 2 * 4));
    if (out.size() != 32) return;
    for (size_t t = 0; t < 8; ++t) {
        CHECK_NEAR(out[t * 4 + 0], 128, 1);
        CHECK_NEAR(out[t * 4 + 1], 0, 1);
        CHECK_NEAR(out[t * 4 + 2], 128, 1);
        CHECK_EQ(out[t * 4 + 3], 115);
    }
    // sRGB: the mid mix is taken in linear then encoded: 0.5 linear -> 188.
    CHECK(bake_cell_water(w, rect, 1, 1, true, out));
    CHECK_NEAR(out[0], 188, 1);
}

CM_TEST(bake, water_above_plane_none) {
    WorldScene w = water_scene(-5.0f);
    const float rect[4] = {0, 0, 100, 100};
    std::vector<uint8_t> out;
    CHECK_FALSE(bake_cell_water(w, rect, 4, 4, false, out));
    CHECK(out.empty());
}
