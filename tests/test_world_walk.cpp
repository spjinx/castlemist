/// @file
/// @brief Tests for CollisionIndex and raycast (world/walk.h).

#include "test_framework.h"

#include "castlemist/world/walk.h"

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
