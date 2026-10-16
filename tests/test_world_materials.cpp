/// @file
/// @brief Tests for terrain_blend, page_uv, ground_uv (world/terrain_blend.h) and resolve_prop_clip (world/anim.h).

#include "test_framework.h"

#include "castlemist/world/anim.h"
#include "castlemist/world/collision.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain.h"
#include "castlemist/world/walk.h"
#include "castlemist/world/terrain_blend.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace castlemist::world;

namespace {

bool near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }

TerrainBlendInput base() {
    TerrainBlendInput in;
    in.picker = {{0, 0, 0, 0}};
    in.modx = {{0.5f, 0.5f, 0, 0}};
    const float g[4][4] = {{0.2f, 0.4f, 0.6f, 0.5f}, {0.9f, 0.1f, 0.3f, 0.5f},
                           {0.1f, 0.8f, 0.2f, 0.5f}, {0.7f, 0.6f, 0.05f, 0.5f}};
    for (int i = 0; i < 4; ++i)
        for (int c = 0; c < 4; ++c) in.ground[i].rgba[c] = g[i][c];
    return in;
}

bool rgb_is(const float a[3], const BlendTexel& t) {
    return near(a[0], t.rgba[0]) && near(a[1], t.rgba[1]) && near(a[2], t.rgba[2]);
}

} // namespace

CM_TEST(terrain_blend, one_hot_pickers_select_each_ground) {
    const float pick[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    const int slot[4] = {3, 2, 1, 0};
    for (int i = 0; i < 4; ++i) {
        auto in = base();
        for (int c = 0; c < 4; ++c) in.picker.rgba[c] = pick[i][c];
        float alb[3], w[4];
        terrain_blend(in, alb, w);
        CHECK(rgb_is(alb, in.ground[slot[i]]));
        for (int s = 0; s < 4; ++s) CHECK(near(w[s], s == slot[i] ? 1.0f : 0.0f));
    }
}

CM_TEST(terrain_blend, all_zero_picker_is_color) {
    auto in = base();
    float alb[3], w[4];
    terrain_blend(in, alb, w);
    CHECK(rgb_is(alb, in.ground[0]));
    CHECK(near(w[0], 1) && near(w[1], 0) && near(w[2], 0) && near(w[3], 0));
}

CM_TEST(terrain_blend, alpha_biases_weight) {
    auto in = base();
    in.picker = {{0, 0, 0.5f, 0.5f}};
    in.ground[0].rgba[3] = 0;
    in.ground[1].rgba[3] = 1;
    float alb[3], w[4];
    terrain_blend(in, alb, w);
    CHECK(near(w[0] / w[1], 0.01f / 1.01f));
    CHECK(w[2] == 0 && w[3] == 0);
    CHECK(near(w[0] + w[1] + w[2] + w[3], 1));
}

CM_TEST(terrain_blend, modx_half_is_neutral) {
    auto in = base();
    in.ground[0].rgba[0] = 0.2f; in.ground[0].rgba[1] = 0.4f; in.ground[0].rgba[2] = 0.6f;
    float alb[3], w[4];
    terrain_blend(in, alb, w);
    CHECK(near(alb[0], 0.2f) && near(alb[1], 0.4f) && near(alb[2], 0.6f));
}

CM_TEST(terrain_blend, modx_g_zero_is_grey_at_luminance) {
    auto in = base();
    in.ground[0].rgba[0] = 0.2f; in.ground[0].rgba[1] = 0.4f; in.ground[0].rgba[2] = 0.6f;
    in.modx = {{0.5f, 0, 0, 0}};
    float alb[3], w[4];
    terrain_blend(in, alb, w);
    for (int c = 0; c < 3; ++c) CHECK(near(alb[c], 0.362f));
}

CM_TEST(terrain_blend, absent_slot_gets_no_weight) {
    auto in = base();
    in.picker = {{1, 0, 0, 0}};
    in.present[3] = false;
    in.ground[3] = {{0, 0, 0, 0}};
    float alb[3], w[4];
    terrain_blend(in, alb, w);
    CHECK(rgb_is(alb, in.ground[0]));
    CHECK(near(w[0], 1) && w[3] == 0);
    for (float v : alb) CHECK(std::isfinite(v));
    for (float v : w) CHECK(std::isfinite(v));
}

CM_TEST(terrain_blend, page_uv_covers_sub_rect) {
    TerrainMaterial m;
    m.pickerOffset[0] = 0.25f; m.pickerOffset[1] = 0.75f; m.pickerScale = 0.25f;
    float uv[2];
    page_uv(m, 0, 0, uv);       CHECK(near(uv[0], 0.25f) && near(uv[1], 0.75f));
    page_uv(m, 1, 1, uv);       CHECK(near(uv[0], 0.5f) && near(uv[1], 1.0f));
    page_uv(m, 0.5f, 0.5f, uv); CHECK(near(uv[0], 0.375f) && near(uv[1], 0.875f));
}

CM_TEST(terrain_blend, ground_uv_repeats) {
    float uv[2];
    ground_uv(0.5f, 0.25f, 8, uv);
    CHECK(near(uv[0], 4) && near(uv[1], 2));
    CHECK(kGroundRepeatsPerChunk == 8.0f);
}

CM_TEST(anim, resolve_exact_hit) {
    const std::vector<uint64_t> t = {192720390330ull, 463, 6351};
    CHECK(resolve_prop_clip(t, 463) == 1);
}

CM_TEST(anim, resolve_miss) {
    const std::vector<uint64_t> t = {192720390330ull, 463, 6351};
    CHECK(resolve_prop_clip(t, 464399) == -1);
}

CM_TEST(anim, resolve_zero_token) {
    const std::vector<uint64_t> t = {0, 463};
    CHECK(resolve_prop_clip(t, 0) == -1);
}

// ---- shared pure rules (surface_rules.h, collision.h, terrain.h) ----

namespace {
WorldScene tri_scene() {
    WorldScene s;
    CollisionMesh m;
    m.verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    m.indices = {0, 1, 2};
    s.collision.meshes.push_back(m);
    for (int i = 0; i < 2; ++i) {
        CollisionInstance inst;
        inst.mesh = 0;
        inst.world[0] = inst.world[5] = inst.world[10] = inst.world[15] = 1;
        inst.world[12] = i == 0 ? 0.0f : 5.0f;
        s.collision.instances.push_back(inst);
    }
    return s;
}
} // namespace

CM_TEST(surface, collision_triangles_match_index) {
    const WorldScene s = tri_scene();
    std::vector<float> tris;
    for (size_t i = 0; i < s.collision.instances.size(); ++i) collision_instance_triangles(s, i, tris);
    CHECK_EQ(tris.size(), size_t(18));
    const CollisionIndex a = CollisionIndex::from_triangles(tris, {});
    const CollisionIndex b = CollisionIndex::build(s, false);
    CHECK_EQ(a.triangle_count(), b.triangle_count());
    CHECK_EQ(b.triangle_count(), size_t(2));
    const float d[3] = {0, 0, 1};
    for (float x : {0.2f, 5.2f}) {
        const float o[3] = {x, 0.2f, -10};
        const auto ha = a.raycast(o, d, 100);
        const auto hb = b.raycast(o, d, 100);
        CHECK(ha.has_value());
        CHECK(hb.has_value());
        CHECK_EQ(ha->t, hb->t);
    }
}

CM_TEST(surface, bad_index_skipped) {
    WorldScene s = tri_scene();
    s.collision.meshes[0].indices = {0, 1, 9};
    std::vector<float> tris;
    collision_instance_triangles(s, 0, tris);
    CHECK(tris.empty());
}

CM_TEST(surface, terrain_texture_ids_order) {
    Terrain t;
    t.chunks.resize(2);
    t.chunks[0].material.textureFileIds = {11, 12, 0, 0};
    t.chunks[0].material.normalFileIds = {21, 0, 0, 0};
    t.chunks[0].material.pickerFileId = 31;
    t.chunks[0].material.picker2FileId = 32;
    t.chunks[1].material.textureFileIds = {11, 13, 0, 0};
    const std::vector<uint32_t> all = terrain_texture_ids(t);
    CHECK((all == std::vector<uint32_t>{11, 12, 21, 31, 32, 13}));
    const std::vector<size_t> only{1};
    CHECK((terrain_texture_ids(t, &only) == std::vector<uint32_t>{11, 13}));
}
