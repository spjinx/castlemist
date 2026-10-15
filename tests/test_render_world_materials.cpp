/// @file
/// @brief Tests for WorldView's texture cache (src/render/world_materials.h)
///        and the texture list world_texture_ids builds from a scene.

#include "test_framework.h"

#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_prep.h"

#include "world_materials.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
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
