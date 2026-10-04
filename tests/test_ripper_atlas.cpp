/// @file
/// @brief Tests for the character-atlas math (ripper/atlas.h): which blit rect
///        a piece's UVs live in, cropping its texture, and remapping its UVs.

#include "test_framework.h"

#include "castlemist/ripper/atlas.h"

using namespace castlemist::ripper;
using castlemist::composite::BlitRect;
using castlemist::composite::BlitRectSet;

namespace {

BlitRectSet armor_heavy() {
    BlitRectSet s;
    s.name = "ArmorHeavy";
    s.width = s.height = 1024;
    s.rects = {{0, 512, 384, 1024}, {512, 0, 1024, 256}, {640, 256, 896, 512}, {896, 256, 1024, 512},
               {512, 384, 640, 512}, {0, 0, 384, 256},   {0, 256, 256, 512}};
    return s;
}

ImageRgba solid(int w, int h, uint8_t r) {
    ImageRgba im{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 0)};
    for (size_t i = 0; i < im.px.size(); i += 4) {
        im.px[i] = r;
        im.px[i + 3] = 255;
    }
    return im;
}

} // namespace

CM_TEST(atlas, rect_contains_uv_box) {
    auto coat = piece_rect(armor_heavy(), 0.0018f, 0.5018f, 0.374f, 0.9973f);
    CHECK(coat.has_value());
    CHECK_EQ(coat->x0, 0u);
    CHECK_EQ(coat->y0, 512u);
    CHECK_EQ(coat->x1, 384u);
    auto boots = piece_rect(armor_heavy(), 0.879f, 0.255f, 0.995f, 0.495f);
    CHECK(boots.has_value());
    CHECK_EQ(boots->x0, 896u);
    CHECK_EQ(boots->y0, 256u);
}

CM_TEST(atlas, no_rect_is_nullopt) {
    CHECK(!piece_rect(armor_heavy(), 0.1f, 0.1f, 0.9f, 0.9f).has_value());
}

CM_TEST(atlas, crop_takes_half_rect_block) {
    ImageRgba tex = solid(512, 256, 10);
    tex.px[(20 * 512 + 10) * 4] = 99;  // pixel (10,20)
    ImageRgba out = crop_piece(tex, BlitRect{0, 512, 384, 1024});
    CHECK_EQ(out.w, 192);
    CHECK_EQ(out.h, 256);
    CHECK_EQ(int(out.px[(20 * 192 + 10) * 4]), 99);
    CHECK_EQ(int(out.px[(255 * 192 + 191) * 4]), 10);
}

CM_TEST(atlas, crop_pads_small_texture) {
    ImageRgba tex = solid(64, 64, 200);
    ImageRgba out = crop_piece(tex, BlitRect{0, 256, 256, 512});
    CHECK_EQ(out.w, 128);
    CHECK_EQ(out.h, 128);
    CHECK_EQ(int(out.px[(10 * 128 + 10) * 4]), 200);
    CHECK_EQ(int(out.px[(100 * 128 + 100) * 4 + 3]), 0);  // beyond the 64x64 texture: transparent
}

CM_TEST(atlas, remap_uv_maps_rect_to_unit) {
    float u = 384.0f / 1024.0f, v = 512.0f / 1024.0f;
    remap_uv(u, v, BlitRect{0, 512, 384, 1024});
    CHECK_NEAR(u, 1.0, 1e-6);
    CHECK_NEAR(v, 0.0, 1e-6);
    float u2 = 192.0f / 1024.0f, v2 = 768.0f / 1024.0f;
    remap_uv(u2, v2, BlitRect{0, 512, 384, 1024});
    CHECK_NEAR(u2, 0.5, 1e-6);
    CHECK_NEAR(v2, 0.5, 1e-6);
}

CM_TEST(atlas, armor_rect_ignores_skin_mesh_listed_first) {
    BlitRectSet s = armor_heavy();
    s.rects.push_back({384, 512, 768, 768});  // where Skin meshes sample the body texture
    std::vector<MeshUvInfo> meshes = {
        {0.40f, 0.52f, 0.70f, 0.70f, 900, true},     // Skin, inside (384,512,768,768)
        {0.0018f, 0.5018f, 0.374f, 0.9973f, 763, false},  // armor, inside (0,512,384,1024)
        {0.0067f, 0.54f, 0.37f, 0.76f, 262, false},       // glow, same rect as the armor
    };
    auto r = choose_armor_rect(s, meshes);
    CHECK(r.has_value());
    CHECK_EQ(r->x0, 0u);
    CHECK_EQ(r->y0, 512u);
}

CM_TEST(atlas, armor_rect_prefers_most_vertices) {
    std::vector<MeshUvInfo> meshes = {
        {0.879f, 0.255f, 0.995f, 0.495f, 50, false},     // boots rect, few vertices
        {0.0018f, 0.5018f, 0.374f, 0.9973f, 763, false},  // coat rect, most vertices
    };
    auto r = choose_armor_rect(armor_heavy(), meshes);
    CHECK(r.has_value());
    CHECK_EQ(r->x0, 0u);
}

CM_TEST(atlas, armor_rect_none_when_only_skin) {
    std::vector<MeshUvInfo> meshes = {{0.0018f, 0.5018f, 0.374f, 0.9973f, 763, true}};
    CHECK(!choose_armor_rect(armor_heavy(), meshes).has_value());
}

CM_TEST(atlas, resize_nearest_matches_the_target_size) {
    ImageRgba src = solid(4, 2, 0);
    src.px[(0 * 4 + 3) * 4] = 200;  // pixel (3,0)
    ImageRgba big = resize_nearest(src, 8, 4);
    CHECK_EQ(big.w, 8);
    CHECK_EQ(big.h, 4);
    CHECK_EQ(int(big.px[(0 * 8 + 6) * 4]), 200);  // (6,0) samples (3,0)
    CHECK_EQ(int(big.px[(3 * 8 + 7) * 4]), 0);
    ImageRgba small = resize_nearest(big, 4, 2);
    CHECK(small.px == src.px);
}
