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

CM_TEST(atlas, wrap_uv_folds_mirrored_halves_back) {
    // GW2 mirrors a garment's halves by shifting one half's UVs by -1; the
    // sampler's repeat brings them back. -0.993 -> 0.007, and in-range values
    // (including the 1.0 edge of a rect that ends at 1024) stay put.
    CHECK_NEAR(wrap_uv(-0.993f), 0.007, 1e-5);
    CHECK_NEAR(wrap_uv(-0.496f), 0.504, 1e-5);
    CHECK_NEAR(wrap_uv(0.371f), 0.371, 1e-6);
    CHECK_NEAR(wrap_uv(1.0f), 1.0, 1e-6);
    CHECK_NEAR(wrap_uv(0.0f), 0.0, 1e-6);
    CHECK_NEAR(wrap_uv(1.25f), 0.25, 1e-6);
}

namespace {
BlitRectSet heavy_full() {
    BlitRectSet s = armor_heavy();
    s.rects.push_back({384, 768, 896, 1024});  // chest skin
    s.rects.push_back({768, 512, 1024, 768});  // legs (upper)
    s.rects.push_back({896, 768, 1024, 896});  // legs (lower)
    return s;
}
std::vector<std::pair<float, float>> pts(std::initializer_list<std::pair<int, int>> px) {
    std::vector<std::pair<float, float>> out;
    for (auto [x, y] : px) out.push_back({x / 1024.0f, y / 1024.0f});
    return out;
}
} // namespace

CM_TEST(atlas, region_spans_every_rect_its_points_hit) {
    // Legs body: points in (768,512)-(1024,768) and (896,768)-(1024,896).
    AtlasRegion r = region_for(heavy_full(), pts({{780, 520}, {1000, 700}, {900, 800}, {1010, 890}}));
    CHECK_EQ(r.rects.size(), size_t{2});
    CHECK_EQ(r.ax, 768u);
    CHECK_EQ(r.ay, 512u);
}

CM_TEST(atlas, region_ignores_stray_points) {
    std::vector<std::pair<float, float>> p;
    for (int i = 0; i < 200; ++i) p.push_back({(10 + i) / 1024.0f, 600 / 1024.0f});  // coat rect
    p.push_back({950 / 1024.0f, 300 / 1024.0f});                                     // one stray in the boots rect
    AtlasRegion r = region_for(heavy_full(), p);
    CHECK_EQ(r.rects.size(), size_t{1});
    CHECK_EQ(r.ax, 0u);
    CHECK_EQ(r.ay, 512u);
}

CM_TEST(atlas, blit_places_texture_at_2x_inside_its_rects_only) {
    ImageRgba atlas{1024, 1024, std::vector<uint8_t>(1024 * 1024 * 4, 0)};
    ImageRgba tex = solid(512, 256, 90);      // covers 1024x512 atlas px from the anchor
    tex.px[(10 * 512 + 20) * 4] = 222;        // texel (20,10)
    AtlasRegion r{{BlitRect{0, 512, 384, 1024}}, 0, 512};
    blit(atlas, tex, r);
    auto at = [&](int x, int y) { return int(atlas.px[(static_cast<size_t>(y) * 1024 + x) * 4]); };
    CHECK(at(40, 512 + 20) > 150);    // texel (20,10) -> atlas (40..41, 532..533), smoothly
    CHECK(at(41, 512 + 21) > 150);
    CHECK_EQ(at(100, 700), 90);
    CHECK_EQ(at(500, 700), 0);        // outside the rect: untouched, though the texture reaches it
}

CM_TEST(atlas, blit_full_resolution_texture_one_to_one) {
    // A full-resolution copy covers the same atlas region at one texel per pixel.
    ImageRgba atlas{1024, 1024, std::vector<uint8_t>(1024 * 1024 * 4, 0)};
    ImageRgba tex = solid(1024, 512, 90);
    tex.px[(10 * 1024 + 20) * 4] = 222;  // texel (20,10)
    AtlasRegion r{{BlitRect{0, 512, 384, 1024}}, 0, 512};
    blit(atlas, tex, r, 1.0f);
    auto at = [&](int x, int y) { return int(atlas.px[(static_cast<size_t>(y) * 1024 + x) * 4]); };
    CHECK_EQ(at(20, 522), 222);
    CHECK_EQ(at(21, 522), 90);
}

CM_TEST(atlas, blit_upscales_smoothly) {
    // Two texels 0 and 200 side by side, drawn at 2x: the pixel between them blends.
    ImageRgba atlas{1024, 1024, std::vector<uint8_t>(1024 * 1024 * 4, 0)};
    ImageRgba tex = solid(2, 1, 0);
    tex.px[4] = 200;  // texel (1,0)
    AtlasRegion r{{BlitRect{0, 0, 4, 2}}, 0, 0};
    blit(atlas, tex, r, 2.0f);
    const int mid = atlas.px[(0 * 1024 + 2) * 4];  // between the two texel centres
    CHECK(mid > 40 && mid < 160);
    CHECK_EQ(int(atlas.px[(0 * 1024 + 0) * 4]), 0);
    CHECK_EQ(int(atlas.px[(0 * 1024 + 3) * 4]), 200);
}

CM_TEST(atlas, crop_full_resolution_takes_the_whole_rect) {
    ImageRgba tex = solid(1024, 512, 10);
    ImageRgba out = crop_piece(tex, BlitRect{0, 512, 384, 1024}, 1.0f);
    CHECK_EQ(out.w, 384);
    CHECK_EQ(out.h, 512);
}

CM_TEST(atlas, resize_bilinear_blends_between_texels) {
    ImageRgba src = solid(2, 1, 0);
    src.px[4] = 200;  // texel (1,0)
    ImageRgba big = resize_bilinear(src, 4, 2);
    CHECK_EQ(big.w, 4);
    CHECK_EQ(big.h, 2);
    CHECK_EQ(int(big.px[0]), 0);              // left edge keeps the left texel
    CHECK_EQ(int(big.px[3 * 4]), 200);        // right edge keeps the right texel
    const int mid = big.px[1 * 4];            // between them: a blend
    CHECK(mid > 20 && mid < 120);
}

CM_TEST(atlas, blit_over_layers_by_alpha_and_adds_coverage) {
    // A face already in the atlas; a hair/scalp overlay drawn over it.
    ImageRgba atlas{1024, 1024, std::vector<uint8_t>(1024 * 1024 * 4, 0)};
    auto px = [&](int x, int y) { return atlas.px.data() + (static_cast<size_t>(y) * 1024 + x) * 4; };
    for (int x = 0; x < 4; ++x) {
        uint8_t* p = px(x, 0);
        p[0] = 100; p[1] = 50; p[2] = 40; p[3] = 103;  // face colour, alpha = its shading map
    }
    ImageRgba overlay{2, 1, {0, 0, 0, 0,  200, 200, 200, 255}};  // transparent, then opaque grey
    AtlasRegion r{{BlitRect{0, 0, 4, 1}}, 0, 0};
    blit(atlas, overlay, r, 2.0f, BlitMode::Over);
    CHECK_EQ(int(px(0, 0)[0]), 100);  // under the transparent texel: the face is untouched
    CHECK_EQ(int(px(0, 0)[3]), 103);  // and keeps its own alpha
    CHECK_EQ(int(px(3, 0)[0]), 200);  // under the opaque texel: the overlay
    CHECK_EQ(int(px(3, 0)[3]), 255);  // with its coverage (a hair strand stays visible)
}

namespace {
/// ArmorLight as the dat has it (2026-11-02): rectIndex holds each slot's first rect, then the count.
BlitRectSet armor_light() {
    BlitRectSet s;
    s.name = "ArmorLight";
    s.width = s.height = 1024;
    s.rects = {{0, 512, 384, 768},   {128, 768, 384, 1024}, {640, 0, 1024, 256},   {768, 256, 1024, 512},
               {512, 256, 768, 512}, {512, 0, 640, 256},    {384, 384, 512, 512},  {256, 256, 384, 512},
               {384, 256, 512, 384}, {0, 0, 384, 256},      {0, 768, 128, 1024},   {0, 256, 256, 512},
               {384, 512, 768, 768}, {0, 0, 384, 256},      {384, 0, 512, 256},    {384, 768, 896, 1024},
               {896, 896, 1024, 960}, {896, 960, 1024, 1024}, {768, 512, 1024, 768}, {896, 768, 1024, 896}};
    s.rect_index = {0, 2, 4, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20};
    return s;
}
}  // namespace

CM_TEST(atlas, slot_rects_follow_rect_index) {
    BlitRectSet s = armor_light();
    auto coat = slot_rects(s, 0);
    CHECK_EQ(coat.size(), size_t{2});
    CHECK_EQ(coat[1].x0, 128u);
    auto face = slot_rects(s, kFaceSlot);
    CHECK_EQ(face.size(), size_t{1});
    CHECK_EQ(face[0].x0, 384u);
    CHECK_EQ(face[0].y0, 512u);
    CHECK(slot_rects(s, 16).empty());       // past the last slot
    CHECK(slot_rects(BlitRectSet{}, 0).empty());
}

// Measured 2026-11-02 on the Mad Prince's pieces (HumanMale, ArmorLight): each full-size texture lands 100% inside
// the slots it names when anchored at their common top-left.
CM_TEST(atlas, piece_regions_anchor_at_every_named_slot) {
    BlitRectSet s = armor_light();
    // Helm madprinceih: type 13 (slot 7), skinFlags face; 512x256 at 2x.
    PieceRegions h = piece_regions(s, 13, 0, kSkinFlagFace, {}, 512, 256, 2.0f);
    CHECK_EQ(h.paint.rects.size(), size_t{1});
    CHECK_EQ(h.paint.ax, 0u);
    CHECK_EQ(h.paint.ay, 512u);
    CHECK_EQ(h.skin.rects.size(), size_t{1});
    CHECK_EQ(h.skin.rects[0].x0, 384u);
    // Gloves villacig: type 10 (slots 4, 5), hands skin; 1024x1024 at 1x.
    PieceRegions g = piece_regions(s, 10, 0x2000, 0x2000, {}, 1024, 1024, 1.0f);
    CHECK_EQ(g.paint.ax, 256u);
    CHECK_EQ(g.paint.ay, 256u);
    CHECK_EQ(g.paint.rects.size(), size_t{3});   // slots 4, 5
    CHECK_EQ(g.over.rects.size(), size_t{1});    // the hands skin it hides
    // Boots carnieacib: type 8 (slots 2, 3).
    PieceRegions b = piece_regions(s, 8, 0x1000, 0x1000, {}, 512, 1024, 1.0f);
    CHECK_EQ(b.paint.ax, 512u);
    CHECK_EQ(b.paint.ay, 0u);
}

// The Mad Prince's coat (hideFlags 31038: slots 1-5 and 8, the four body skins) paints the leggings, boots,
// gloves and shoulders slots; the gloves, losing theirs, paint nothing.
CM_TEST(atlas, hide_flags_hand_slots_to_the_coat) {
    BlitRectSet s = armor_light();
    PieceRegions c = piece_regions(s, 9, 31038, 0x800, {}, 1024, 1024, 1.0f);
    CHECK_EQ(c.paint.ax, 0u);
    CHECK_EQ(c.paint.ay, 0u);
    // slot 0 (2) + 1 (2) + 2 (1) + 3 (1) + 4 (1) + 5 (2) + 8 (1); the skin it hides goes over the skin by its
    // alpha -- chest, feet, hands, legs (2): clear over the chest, it keeps his neck.
    CHECK_EQ(c.paint.rects.size(), size_t{10});
    CHECK_EQ(c.over.rects.size(), size_t{5});
    CHECK(c.skin.rects.empty());   // skinFlags' chest is hidden too: over the skin, not through the cut mask
    PieceRegions g = piece_regions(s, 10, 0x2000, 0x2000, {4, 5, 14}, 1024, 1024, 1.0f);
    CHECK(g.paint.rects.empty());
    CHECK(g.skin.rects.empty());
    CHECK(g.over.rects.empty());
    CHECK_EQ(g.paint.ax, 256u);   // still laid out from its own corner
}

CM_TEST(atlas, piece_regions_are_empty_when_the_texture_does_not_reach) {
    BlitRectSet s = armor_light();
    PieceRegions h = piece_regions(s, 13, 0, kSkinFlagFace, {}, 64, 64, 2.0f);
    CHECK(h.paint.rects.empty());
    CHECK(h.skin.rects.empty());
    CHECK(piece_regions(BlitRectSet{}, 9, 0, 0, {}, 512, 512, 2.0f).paint.rects.empty());
    CHECK(piece_regions(s, 5, 0, 0, {}, 512, 512, 2.0f).paint.rects.empty());   // not armour
}

CM_TEST(atlas, flag_bits_name_slots) {
    CHECK_EQ(flag_slot(0), size_t{0});
    CHECK_EQ(flag_slot(9), kFaceSlot);
    CHECK_EQ(flag_slot(11), size_t{12});   // chest skin
    CHECK_EQ(flag_slot(14), size_t{15});   // legs skin
    CHECK_EQ(flag_slot(16), size_t{11});   // ears
    CHECK_EQ(flag_slot(15), kNoSlot);
}

CM_TEST(atlas, mask_alpha_takes_the_masks_red) {
    ImageRgba tex = solid(4, 2, 50);
    ImageRgba mask = solid(2, 1, 0);
    mask.px[0] = 255;   // left half white
    ImageRgba out = with_mask_alpha(tex, mask);
    CHECK_EQ(out.w, 4);
    CHECK_EQ(out.px[0], 50);
    CHECK_EQ(out.px[3], 255);                  // under the white texel
    CHECK_EQ(out.px[(0 * 4 + 3) * 4 + 3], 0);  // under the black one
}

CM_TEST(atlas, split_face_separates_the_face_slots_rects) {
    BlitRectSet s = armor_light();
    AtlasRegion hair{{{0, 0, 384, 256}, {384, 512, 768, 768}}, 0, 0};
    auto [face, rest] = split_face(hair, s);
    CHECK_EQ(face.rects.size(), size_t{1});
    CHECK_EQ(face.rects[0].x0, 384u);
    CHECK_EQ(rest.rects.size(), size_t{1});
    CHECK_EQ(rest.rects[0].y1, 256u);
    CHECK_EQ(face.ax, 0u);   // both keep the anchor
    CHECK_EQ(rest.ay, 0u);
}

// 2026-11-02: texels no mesh samples take the nearest sampled texel's colour and alpha, a few rings out, so
// filtering and mipmaps at a mesh's edge read its own colour: the Mad Prince's ears showed orange fringes from
// the coat's red beside them, and see-through gaps from cut-away texels.
CM_TEST(atlas, seal_seams_grows_used_texels_into_unused_ones) {
    ImageRgba im{8, 1, std::vector<uint8_t>(8 * 4, 0)};
    std::vector<uint8_t> cov(8, 0);
    cov[2] = cov[3] = 1;
    im.px[2 * 4] = 200; im.px[2 * 4 + 3] = 255;
    im.px[3 * 4] = 100; im.px[3 * 4 + 3] = 255;
    im.px[7 * 4] = 9;                                  // unused, beyond reach: left as it is
    seal_seams(im, cov, 2);
    CHECK_EQ(im.px[1 * 4], 200);                       // one ring out
    CHECK_EQ(im.px[0 * 4], 200);                       // two rings out
    CHECK_EQ(im.px[0 * 4 + 3], 255);
    CHECK_EQ(im.px[5 * 4], 100);
    CHECK_EQ(im.px[6 * 4], 0);                         // three rings: not reached
    CHECK_EQ(im.px[7 * 4], 9);
    CHECK_EQ(im.px[2 * 4], 200);                       // used texels unchanged
}
