/// @file
/// @brief Tests for CollisionIndex and raycast (world/walk.h).

#include "test_framework.h"

#include "castlemist/world/walk.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace castlemist::world;

namespace {

void quad(std::vector<float>& t, float x0, float y0, float x1, float y1, float z) {
    const float v[] = {x0, y0, z, x1, y0, z, x1, y1, z,
                       x0, y0, z, x1, y1, z, x0, y1, z};
    t.insert(t.end(), v, v + 18);
}

const float kDown[3] = {0, 0, 1};

} // namespace

CM_TEST(walk, raycast_hits_floor) {
    std::vector<float> tris;
    quad(tris, -100, -100, 100, 100, 0);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {0, 0, -50};
    const auto hit = idx.raycast(o, kDown, 1000);
    CHECK(hit.has_value());
    if (!hit) return;
    CHECK_NEAR(hit->t, 50.0f, 1e-3f);
    CHECK_NEAR(hit->point[2], 0.0f, 1e-3f);
    CHECK(hit->normal[2] < 0);
}

CM_TEST(walk, raycast_misses_outside) {
    std::vector<float> tris;
    quad(tris, -100, -100, 100, 100, 0);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {500, 0, -50};
    CHECK_FALSE(idx.raycast(o, kDown, 1000).has_value());
}

CM_TEST(walk, raycast_nearest_of_two) {
    std::vector<float> tris;
    quad(tris, -100, -100, 100, 100, 0);
    quad(tris, -100, -100, 100, 100, -20);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {0, 0, -50};
    const auto hit = idx.raycast(o, kDown, 1000);
    CHECK(hit.has_value());
    if (hit) CHECK_NEAR(hit->t, 30.0f, 1e-3f);
}

CM_TEST(walk, raycast_crosses_cells) {
    std::vector<float> tris;
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j)
            quad(tris, -1000.0f + i * 100, -1000.0f + j * 100, -900.0f + i * 100, -900.0f + j * 100, 0);
    // vertical wall at x = 900
    const float w[] = {900, -1000, -200, 900, 1000, -200, 900, 1000, 0,
                       900, -1000, -200, 900, 1000, 0, 900, -1000, 0};
    tris.insert(tris.end(), w, w + 18);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {-900, 0, -100}, d[3] = {1, 0, 0};
    const auto hit = idx.raycast(o, d, 5000);
    CHECK(hit.has_value());
    if (hit) CHECK_NEAR(hit->t, 1800.0f, 1e-2f);
}

CM_TEST(walk, raycast_mask) {
    std::vector<float> coll, terr;
    quad(coll, -100, -100, 100, 100, 0);
    quad(terr, -100, -100, 100, 100, -10);
    const auto idx = CollisionIndex::from_triangles(coll, terr);
    const float o[3] = {0, 0, -50};
    const auto c = idx.raycast(o, kDown, 1000, kHitCollision);
    const auto t = idx.raycast(o, kDown, 1000, kHitTerrain);
    CHECK(c.has_value());
    CHECK(t.has_value());
    if (c) { CHECK_NEAR(c->t, 50.0f, 1e-3f); CHECK(c->source == HitSource::Collision); }
    if (t) { CHECK_NEAR(t->t, 40.0f, 1e-3f); CHECK(t->source == HitSource::Terrain); }
}

CM_TEST(walk, build_from_scene_transforms_instances) {
    WorldScene scene;
    CollisionMesh mesh;
    mesh.verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    mesh.indices = {0, 1, 2};
    scene.collision.meshes.push_back(mesh);
    CollisionInstance inst;
    inst.mesh = 0;
    inst.world[0] = inst.world[5] = inst.world[10] = inst.world[15] = 1;
    inst.world[12] = 1000;
    scene.collision.instances.push_back(inst);
    const auto idx = CollisionIndex::build(scene, false);
    CHECK_EQ(idx.triangle_count(kHitCollision), (size_t)1);
    const float o[3] = {1000.2f, 0.2f, -10};
    CHECK(idx.raycast(o, kDown, 100).has_value());
}

CM_TEST(walk, build_includes_terrain) {
    WorldScene scene;
    TerrainChunk c;
    c.samples = 3;
    c.rect[0] = 0; c.rect[1] = 0; c.rect[2] = 200; c.rect[3] = 200;
    c.heights.assign(9, 5.0f);
    scene.terrain.present = true;
    scene.terrain.chunks.push_back(c);
    const auto idx = CollisionIndex::build(scene, true);
    CHECK_EQ(idx.triangle_count(kHitTerrain), (size_t)8);
    const float o[3] = {50, 50, -100};
    const auto hit = idx.raycast(o, kDown, 1000);
    CHECK(hit.has_value());
    if (hit) CHECK_NEAR(hit->point[2], 5.0f, 1e-3f);
    CHECK_EQ(CollisionIndex::build(scene, false).triangle_count(kHitTerrain), (size_t)0);
}

namespace {

/// Vertical wall in the plane x = x, spanning y0..y1 and z0..z1.
void wall_x(std::vector<float>& t, float x, float y0, float y1, float z0, float z1) {
    const float v[] = {x, y0, z0, x, y1, z0, x, y1, z1,
                       x, y0, z0, x, y1, z1, x, y0, z1};
    t.insert(t.end(), v, v + 18);
}

/// Vertical wall in the plane y = y, spanning x0..x1 and z0..z1.
void wall_y(std::vector<float>& t, float y, float x0, float x1, float z0, float z1) {
    const float v[] = {x0, y, z0, x1, y, z0, x1, y, z1,
                       x0, y, z0, x1, y, z1, x0, y, z1};
    t.insert(t.end(), v, v + 18);
}

/// 20x20 floor of 100-unit quads over [-1000,1000]^2 at z = 0.
void floor_grid(std::vector<float>& t) {
    for (int i = 0; i < 20; ++i)
        for (int j = 0; j < 20; ++j)
            quad(t, -1000.0f + i * 100, -1000.0f + j * 100, -900.0f + i * 100, -900.0f + j * 100, 0);
}

} // namespace

CM_TEST(walk, raycast_negative_x_and_y) {
    std::vector<float> tris;
    floor_grid(tris);
    wall_x(tris, -900, -1000, 1000, -200, 0);
    wall_x(tris, 900, -1000, 1000, -200, 0);
    wall_y(tris, -900, -1000, 1000, -200, 0);
    wall_y(tris, 900, -1000, 1000, -200, 0);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {0, 0, -100};
    const float mx[3] = {-1, 0, 0}, py[3] = {0, 1, 0}, my[3] = {0, -1, 0}, px[3] = {1, 0, 0};
    auto a = idx.raycast(o, mx, 5000);
    auto b = idx.raycast(o, py, 5000);
    auto c = idx.raycast(o, my, 5000);
    auto d = idx.raycast(o, px, 5000);
    CHECK(a.has_value()); CHECK(b.has_value()); CHECK(c.has_value()); CHECK(d.has_value());
    if (a) CHECK_NEAR(a->t, 900.0f, 1e-2f);
    if (b) CHECK_NEAR(b->t, 900.0f, 1e-2f);
    if (c) CHECK_NEAR(c->t, 900.0f, 1e-2f);
    if (d) CHECK_NEAR(d->t, 900.0f, 1e-2f);
    const float o2[3] = {500, 300, -100};
    auto e = idx.raycast(o2, my, 5000);
    auto f = idx.raycast(o2, mx, 5000);
    CHECK(e.has_value()); CHECK(f.has_value());
    if (e) CHECK_NEAR(e->t, 1200.0f, 1e-2f);
    if (f) CHECK_NEAR(f->t, 1400.0f, 1e-2f);
}

CM_TEST(walk, raycast_diagonal) {
    std::vector<float> tris;
    floor_grid(tris);
    wall_x(tris, 900, -1000, 1000, -200, 0);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {-800, -800, -100}, d[3] = {1, 1, 0};
    const auto hit = idx.raycast(o, d, 10000);
    CHECK(hit.has_value());
    if (hit) {
        CHECK_NEAR(hit->t, 1700.0f * std::sqrt(2.0f), 0.05f);
        CHECK_NEAR(hit->point[0], 900.0f, 0.05f);
        CHECK_NEAR(hit->point[1], 900.0f, 0.05f);
    }
}

CM_TEST(walk, raycast_from_outside_grid) {
    std::vector<float> tris;
    floor_grid(tris);
    wall_x(tris, 900, -1000, 1000, -200, 0);
    const auto idx = CollisionIndex::from_triangles(tris, {});
    const float o[3] = {-3000, 0, -100}, d[3] = {1, 0, 0};
    const auto hit = idx.raycast(o, d, 10000);
    CHECK(hit.has_value());
    if (hit) CHECK_NEAR(hit->t, 3900.0f, 1e-2f);
    const float o2[3] = {-3000, -3500, -100}, d2[3] = {1, 1, 0};
    const auto hit2 = idx.raycast(o2, d2, 10000);
    CHECK(hit2.has_value());
    if (hit2) {
        CHECK_NEAR(hit2->t, 3900.0f * std::sqrt(2.0f), 0.05f);
        CHECK_NEAR(hit2->point[1], 400.0f, 0.05f);
    }
    // Same diagonal shifted so it passes beyond the wall's y span.
    const float o3[3] = {-3000, -2500, -100};
    CHECK_FALSE(idx.raycast(o3, d2, 10000).has_value());
}

CM_TEST(walk, build_terrain_sloped_asymmetric) {
    WorldScene scene;
    TerrainChunk c;
    c.samples = 3;
    c.rect[0] = 0; c.rect[1] = 0; c.rect[2] = 200; c.rect[3] = 400;
    c.heights = {0, 10, 20,
                 30, 5, -15,
                 40, 50, 60};
    scene.terrain.present = true;
    scene.terrain.chunks.push_back(c);
    const auto idx = CollisionIndex::build(scene, true);
    const float dx = 100, dy = 200;
    auto h = [&](int i, int j) { return c.heights[(size_t)j * 3 + i]; };
    // Sample rule: column i at x0 + i*dx, row j at y1 - j*dy. In a quad, u runs
    // east and v runs south from its (i, j) corner; triangles are
    // (p00,p10,p11) when u >= v and (p00,p11,p01) otherwise.
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const float us[] = {0.7f, 0.2f, 0.35f, 0.9f}, vs[] = {0.2f, 0.7f, 0.3f, 0.6f};
            for (int k = 0; k < 4; ++k) {
                const float u = us[k], v = vs[k];
                const float h00 = h(i, j), h10 = h(i + 1, j), h01 = h(i, j + 1), h11 = h(i + 1, j + 1);
                const float expect = u >= v ? h00 + u * (h10 - h00) + v * (h11 - h10)
                                            : h00 + u * (h11 - h01) + v * (h01 - h00);
                const float px = i * dx + u * dx;
                const float py = 400.0f - (j * dy + v * dy);
                const float o[3] = {px, py, -1000};
                const auto hit = idx.raycast(o, kDown, 5000);
                CHECK(hit.has_value());
                if (hit) CHECK_NEAR(hit->point[2], expect, 1e-2f);
            }
        }
    }
    // North-west corner is row 0 / column 0 (height 0), south-west is row 2 (height 40).
    const float nw[3] = {1, 399, -1000}, sw[3] = {1, 1, -1000}, ne[3] = {199, 399, -1000};
    auto a = idx.raycast(nw, kDown, 5000), b = idx.raycast(sw, kDown, 5000), e = idx.raycast(ne, kDown, 5000);
    CHECK(a.has_value()); CHECK(b.has_value()); CHECK(e.has_value());
    if (a) CHECK_NEAR(a->point[2], 0.0f, 0.5f);
    if (b) CHECK_NEAR(b->point[2], 40.0f, 0.5f);
    if (e) CHECK_NEAR(e->point[2], 20.0f, 0.5f);
}

namespace {

constexpr float kDt = 1.0f / 60.0f;
const float kNoMove[2] = {0, 0};

/// Big flat floor at z = 0.
CollisionIndex flat_floor() {
    std::vector<float> t;
    quad(t, -1000, -1000, 1000, 1000, 0);
    return CollisionIndex::from_triangles(t, {});
}

} // namespace

CM_TEST(walk, walker_settles_on_flat_ground) {
    const auto idx = flat_floor();
    Walker w(&idx);
    float pos[3] = {0, 0, -300};
    w.reset(pos);
    StepResult r;
    for (int i = 0; i < 120; ++i) r = w.step(pos, kNoMove, kDt);
    CHECK(r.grounded);
    CHECK(w.grounded());
    CHECK_NEAR(pos[2], -120.0f, 0.5f);
}

CM_TEST(walk, walker_blocked_by_wall_and_slides) {
    std::vector<float> t;
    quad(t, -1000, -1000, 1000, 1000, 0);
    wall_x(t, 100, -1000, 1000, -400, 0);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {0, 0, -120};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    const float d[2] = {10, 10};
    for (int i = 0; i < 60; ++i) w.step(pos, d, kDt);
    CHECK(pos[0] < 100 - 18 + 0.5f);
    CHECK(pos[1] > 200);
}

CM_TEST(walk, walker_climbs_low_step) {
    std::vector<float> t;
    quad(t, -1000, -1000, 1000, 1000, 0);
    quad(t, 50, -1000, 1000, 1000, -10);
    wall_x(t, 50, -1000, 1000, -10, 0);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {0, 0, -120};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    const float d[2] = {5, 0};
    for (int i = 0; i < 60; ++i) w.step(pos, d, kDt);
    CHECK(pos[0] > 100);
    CHECK_NEAR(pos[2], -130.0f, 0.5f);
}

CM_TEST(walk, walker_blocked_by_high_step) {
    std::vector<float> t;
    quad(t, -1000, -1000, 1000, 1000, 0);
    quad(t, 50, -1000, 1000, 1000, -40);
    wall_x(t, 50, -1000, 1000, -40, 0);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {0, 0, -120};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    const float d[2] = {5, 0};
    for (int i = 0; i < 60; ++i) w.step(pos, d, kDt);
    CHECK(pos[0] < 50);
}

CM_TEST(walk, walker_falls_off_ledge_and_lands) {
    std::vector<float> t;
    quad(t, -1000, -1000, 0, 1000, -500);
    quad(t, 0, -1000, 1000, 1000, 0);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {-10, 0, -620};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    CHECK_NEAR(pos[2], -620.0f, 0.5f);
    const float d[2] = {3, 0};
    bool airborne = false;
    for (int i = 0; i < 300; ++i) {
        const StepResult r = w.step(pos, d, kDt);
        if (!r.grounded) airborne = true;
    }
    CHECK(airborne);
    CHECK(w.grounded());
    CHECK_NEAR(pos[2], -120.0f, 0.5f);
}

CM_TEST(walk, jump_rises_then_lands_no_double_jump) {
    const auto idx = flat_floor();
    Walker w(&idx);
    float pos[3] = {0, 0, -120};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    w.jump();
    w.step(pos, kNoMove, kDt);
    CHECK(w.vertical_velocity() > 0);
    CHECK(pos[2] < -120.0f);
    float peak = 0;
    for (int i = 0; i < 240; ++i) {
        if (!w.grounded()) w.jump();   // mid-air requests must be ignored
        w.step(pos, kNoMove, kDt);
        peak = std::max(peak, -120.0f - pos[2]);
    }
    CHECK(peak > 100.0f);
    CHECK(peak <= 900.0f * 900.0f / (2 * 2800.0f) + 1.0f);
    CHECK(w.grounded());
    CHECK_NEAR(pos[2], -120.0f, 0.5f);
}

CM_TEST(walk, fall_safety_returns_to_safe_ground) {
    std::vector<float> t;
    quad(t, -100, -100, 100, 100, 0);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {0, 0, -120};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    for (int i = 0; i < 10; ++i) w.step(pos, kNoMove, kDt);
    pos[0] = 500;
    StepResult r;
    bool recovered = false;
    for (int i = 0; i < 1000 && !recovered; ++i) {
        r = w.step(pos, kNoMove, kDt);
        recovered = r.recovered;
    }
    CHECK(recovered);
    CHECK_FALSE(r.gaveUp);
    CHECK(r.grounded);
    CHECK_NEAR(pos[0], 0.0f, 1e-3f);
    CHECK_NEAR(pos[2], -120.0f, 0.5f);
}

CM_TEST(walk, walker_no_geometry_never_grounded_and_reports) {
    const auto idx = CollisionIndex::from_triangles({}, {});
    Walker w(&idx);
    float pos[3] = {0, 0, 0};
    w.reset(pos);
    CHECK_FALSE(w.snap_to_ground(pos));
    bool gaveUp = false;
    for (int i = 0; i < 300; ++i) {
        const StepResult r = w.step(pos, kNoMove, kDt);
        CHECK_FALSE(r.grounded);
        if (r.gaveUp) gaveUp = true;
    }
    CHECK(gaveUp);
}

CM_TEST(walk, snap_from_below_finds_top_surface) {
    std::vector<float> t;
    quad(t, -100, -100, 100, 100, 0);
    quad(t, -100, -100, 100, 100, -300);
    const auto idx = CollisionIndex::from_triangles(t, {});
    Walker w(&idx);
    float pos[3] = {0, 0, 500};
    w.reset(pos);
    CHECK(w.snap_to_ground(pos));
    CHECK_NEAR(pos[2], -420.0f, 0.5f);
}

CM_TEST(walk, large_dt_is_clamped_no_tunnel) {
    const auto idx = flat_floor();
    float a[3] = {0, 0, -2000}, b[3] = {0, 0, -2000};
    Walker wa(&idx), wb(&idx);
    wa.reset(a);
    wb.reset(b);
    wa.step(a, kNoMove, 5.0f);
    wb.step(b, kNoMove, 0.1f);
    CHECK(a[2] < 0);
    for (int k = 0; k < 3; ++k) CHECK_NEAR(a[k], b[k], 1e-3f);
    bool tooLow = false;
    StepResult r;
    for (int i = 0; i < 300; ++i) {
        r = wa.step(a, kNoMove, kDt);
        if (a[2] > -120.0f + 0.5f) tooLow = true;
    }
    CHECK_FALSE(tooLow);
    CHECK(r.grounded);
    CHECK_NEAR(a[2], -120.0f, 0.5f);
}
