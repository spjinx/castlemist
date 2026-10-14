/// @file
/// @brief Tests for model_load_order and chunk_normals (world/stream.h, terrain_mesh.h).

#include "test_framework.h"

#include "castlemist/world/stream.h"
#include "castlemist/world/terrain_mesh.h"

#include <cmath>
#include <vector>

using namespace castlemist::world;

namespace {

void add_prop(WorldScene& s, uint32_t model, float x) {
    PropInstance p;
    p.model = model;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.world[12] = x;
    s.props.push_back(p);
}

template <class F>
TerrainChunk make_chunk(int cx, float x0, float x1, int n, F f) {
    TerrainChunk c;
    c.cx = cx; c.cy = 0; c.samples = n;
    c.rect[0] = x0; c.rect[1] = 0; c.rect[2] = x1; c.rect[3] = 30;
    c.heights.resize(size_t(n) * n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            c.heights[size_t(j) * n + i] = f(x0 + i * (x1 - x0) / float(n - 1));
    return c;
}

} // namespace

CM_TEST(stream, order_by_nearest_instance) {
    WorldScene s;
    s.models.resize(3);
    add_prop(s, 0, 1000);
    add_prop(s, 1, 10);
    add_prop(s, 1, 5000);
    const float cam[3] = {0, 0, 0};
    const auto o = model_load_order(s, cam);
    CHECK(o.size() == 3);
    CHECK(o[0] == 1 && o[1] == 0 && o[2] == 2);
}

CM_TEST(stream, ties_by_index) {
    WorldScene s;
    s.models.resize(2);
    add_prop(s, 1, 100);
    add_prop(s, 0, -100);
    const float cam[3] = {0, 0, 0};
    const auto o = model_load_order(s, cam);
    CHECK(o.size() == 2);
    CHECK(o[0] == 0 && o[1] == 1);
}

CM_TEST(stream, empty_scene) {
    WorldScene s;
    const float cam[3] = {0, 0, 0};
    CHECK(model_load_order(s, cam).empty());
}

CM_TEST(terrain, normals_flat_point_up) {
    Terrain t;
    t.chunks.push_back(make_chunk(0, 0, 30, 4, [](float) { return 0.0f; }));
    const auto n = chunk_normals(t, 0);
    CHECK(n.size() == 48);
    for (size_t k = 0; k < 16; ++k) {
        CHECK_NEAR(n[k * 3], 0.0, 1e-5);
        CHECK_NEAR(n[k * 3 + 1], 0.0, 1e-5);
        CHECK_NEAR(n[k * 3 + 2], -1.0, 1e-5);
    }
}

CM_TEST(terrain, normals_slope_tilts) {
    Terrain t;
    t.chunks.push_back(make_chunk(0, 0, 30, 4, [](float x) { return 0.5f * x; }));
    const auto n = chunk_normals(t, 0);
    CHECK(n.size() == 48);
    for (size_t k = 0; k < 16; ++k) {
        CHECK_NEAR(n[k * 3], 0.447, 1e-3);
        CHECK_NEAR(n[k * 3 + 1], 0.0, 1e-5);
        CHECK_NEAR(n[k * 3 + 2], -0.894, 1e-3);
    }
}

CM_TEST(terrain, normals_seam_continuous) {
    auto f = [](float x) { return 0.01f * x * x; };
    Terrain t;
    t.chunks.push_back(make_chunk(0, 0, 30, 4, f));
    t.chunks.push_back(make_chunk(1, 30, 60, 4, f));
    const auto a = chunk_normals(t, 0);
    const auto b = chunk_normals(t, 1);
    CHECK(a.size() == 48 && b.size() == 48);
    const int n = 4;
    for (int j = 0; j < n; ++j)
        for (int c = 0; c < 3; ++c)
            CHECK_NEAR(a[(size_t(j) * n + (n - 1)) * 3 + c], b[(size_t(j) * n) * 3 + c], 1e-4);
}
