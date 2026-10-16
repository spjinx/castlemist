/// @file
/// @brief Tests for WorldView's camera maths (pure) and a device smoke test.

#include "test_framework.h"

#include "castlemist/render/world_foliage_grid.h"
#include "castlemist/render/world_prep.h"

#include "world_camera.h"
#include "world_props.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace castlemist::render;

namespace {

/// Row-vector transform to NDC (x/w, y/w, z/w).
void project(const Mat4& vp, float x, float y, float z, float ndc[3]) {
    float c[4];
    for (int k = 0; k < 4; ++k) c[k] = x * vp.m[k] + y * vp.m[4 + k] + z * vp.m[8 + k] + vp.m[12 + k];
    for (int k = 0; k < 3; ++k) ndc[k] = c[k] / c[3];
}

WorldCamera looking_east() {
    WorldCamera c;   // pos 0, fwd +X, up -Z
    return c;
}

} // namespace

CM_TEST(world_view, viewproj_centre_projects_to_middle) {
    const Mat4 vp = world_camera_viewproj(looking_east(), 16.0f / 9.0f);
    float ndc[3];
    project(vp, 100, 0, 0, ndc);
    CHECK_NEAR(ndc[0], 0.0f, 1e-5f);
    CHECK_NEAR(ndc[1], 0.0f, 1e-5f);
    CHECK(ndc[2] > 0.0f);
    CHECK(ndc[2] < 1.0f);
}

CM_TEST(world_view, up_is_screen_up) {
    const Mat4 vp = world_camera_viewproj(looking_east(), 16.0f / 9.0f);
    float ndc[3];
    project(vp, 100, 0, -10, ndc);   // -Z is up: this point is above the camera
    CHECK(ndc[1] > 0.0f);
}

CM_TEST(world_view, pixel_ray_round_trip) {
    WorldCamera c = looking_east();
    c.pos[0] = 5; c.pos[1] = -3; c.pos[2] = 7;
    const int w = 800, h = 600;
    float o[3], d[3];
    world_camera_ray(c, w, h, w / 2, h / 2, o, d);
    CHECK_NEAR(o[0], 5.0f, 1e-6f);
    CHECK_NEAR(o[1], -3.0f, 1e-6f);
    CHECK_NEAR(o[2], 7.0f, 1e-6f);
    CHECK_NEAR(d[0], 1.0f, 1e-4f);
    CHECK_NEAR(d[1], 0.0f, 5e-3f);
    CHECK_NEAR(d[2], 0.0f, 5e-3f);

    // right = cross(up, fwd) = cross((0,0,-1), (1,0,0)) = (0,-1,0)
    world_camera_ray(c, w, h, w - 1, h / 2, o, d);
    const float right[3] = {0, -1, 0};
    CHECK(d[0] * right[0] + d[1] * right[1] + d[2] * right[2] > 0.0f);
    CHECK_NEAR(d[0] * d[0] + d[1] * d[1] + d[2] * d[2], 1.0f, 1e-5f);
}

namespace {

/// A unit-ish cube (half-size @p h, centred on the origin): 24 vertices with
/// face normals, 12 triangles, and one coarser LOD (two faces dropped).
ModelPreview cube_model(float h = 50.0f) {
    ModelMeshCPU m;
    const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; ++f) {
        // Two tangent axes perpendicular to the face normal.
        const int a = (f / 2 + 1) % 3, b = (f / 2 + 2) % 3;
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (int c = 0; c < 4; ++c) {
            GVertex v{};
            float p[3] = {n[f][0] * h, n[f][1] * h, n[f][2] * h};
            p[a] = (c & 1) ? h : -h;
            p[b] = (c & 2) ? h : -h;
            v.px = p[0]; v.py = p[1]; v.pz = p[2];
            v.nx = n[f][0]; v.ny = n[f][1]; v.nz = n[f][2];
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    }
    m.lodIndices.emplace_back(m.indices.begin(), m.indices.end() - 12);
    m.vertexCount = static_cast<uint32_t>(m.vertices.size());
    ModelPreview mp;
    mp.meshes.push_back(std::move(m));
    mp.radius = h * std::sqrt(3.0f);
    return mp;
}

} // namespace

CM_TEST(world_view, prepare_cube_ok) {
    const PreparedModel p = prepare_world_model(cube_model());
    CHECK(p.ok);
    CHECK_EQ(p.matVerts.size(), size_t(24));   // verts is cleared once matVerts holds them
    CHECK(p.lodRanges.size() >= 1);
    CHECK_EQ(p.lodRanges.size(), size_t(2));
    if (p.lodRanges.size() < 2) return;
    CHECK_EQ(p.lodRanges[0].first, 0u);
    CHECK_EQ(p.lodRanges[0].second, 36u);
    CHECK_EQ(p.lodRanges[1].second, 24u);
    CHECK_EQ(p.indices.size(), size_t(60));
    CHECK_NEAR(p.radius, 50.0f * std::sqrt(3.0f), 1e-3f);
    CHECK_NEAR(p.center[0], 0.0f, 1e-6f);
    // AO is baked, with the 0.30 floor (0.30 * 255 = 76.5 -> 76 or 77).
    for (const MatVtx& v : p.matVerts) {
        CHECK(v.ao >= 76);
        CHECK(v.ao <= 255);
    }
}

CM_TEST(world_view, skips_empty_and_nan_models) {
    ModelPreview empty;
    CHECK_FALSE(prepare_world_model(empty).ok);

    ModelPreview nan = cube_model();
    nan.radius = std::nanf("");
    CHECK_FALSE(prepare_world_model(nan).ok);

    ModelPreview inf = cube_model();
    inf.center[1] = INFINITY;
    CHECK_FALSE(prepare_world_model(inf).ok);

    // A mesh with vertices but no triangles draws nothing either.
    ModelPreview noTris = cube_model();
    noTris.meshes[0].indices.clear();
    noTris.meshes[0].lodIndices.clear();
    CHECK_FALSE(prepare_world_model(noTris).ok);
}

CM_TEST(world_view, column_major_world_to_row_vector) {
    // M = T(10,20,30) * S(2,3,1), column-major, p' = M p.
    float w[16] = {2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 1, 0,  10, 20, 30, 1};
    const Mat4 r = colmajor_to_rowvec(w);
    const Vec3 a = transformPoint({1, 0, 0}, r);
    CHECK_NEAR(a.x, 12.0f, 1e-6f);
    CHECK_NEAR(a.y, 20.0f, 1e-6f);
    CHECK_NEAR(a.z, 30.0f, 1e-6f);
    const Vec3 b = transformPoint({0, 1, 2}, r);
    CHECK_NEAR(b.x, 10.0f, 1e-6f);
    CHECK_NEAR(b.y, 23.0f, 1e-6f);
    CHECK_NEAR(b.z, 32.0f, 1e-6f);
    CHECK_NEAR(max_column_length(w), 3.0f, 1e-6f);

    // A 90-degree turn about Z (x -> y) with scale 2: p' = (-2y, 2x, 2z).
    float q[16] = {0, 2, 0, 0,  -2, 0, 0, 0,  0, 0, 2, 0,  0, 0, 0, 1};
    const Vec3 c = transformPoint({1, 0, 0}, colmajor_to_rowvec(q));
    CHECK_NEAR(c.x, 0.0f, 1e-6f);
    CHECK_NEAR(c.y, 2.0f, 1e-6f);
    CHECK_NEAR(max_column_length(q), 2.0f, 1e-6f);
}

CM_TEST(world_view, prepare_collision_mesh_normals_and_bounds) {
    castlemist::world::CollisionMesh m;
    // A flat square at z = 5 in the XY plane, plus one out-of-range triangle.
    m.verts = {0, 0, 5,  10, 0, 5,  0, 10, 5,  10, 10, 5};
    m.indices = {0, 1, 2,  1, 3, 2,  0, 1, 9};
    const PreparedModel p = prepare_collision_mesh(m);
    CHECK(p.ok);
    CHECK_EQ(p.verts.size(), size_t(4));
    CHECK_EQ(p.indices.size(), size_t(6));
    CHECK_EQ(p.lodRanges.size(), size_t(1));
    CHECK_NEAR(p.center[0], 5.0f, 1e-5f);
    CHECK_NEAR(p.center[2], 5.0f, 1e-5f);
    CHECK_NEAR(p.radius, std::sqrt(50.0f), 1e-4f);
    for (const MapVtx& v : p.verts) {
        CHECK_EQ(int(v.ao), 255);
        CHECK(v.nz == 0 || v.nz == 255);   // +-Z, consistent winding
        CHECK_EQ(int(v.nz), int(p.verts[0].nz));
    }
    castlemist::world::CollisionMesh none;
    CHECK_FALSE(prepare_collision_mesh(none).ok);
}

CM_TEST(world_view, ray_model_hit_translated_cube) {
    const ModelPreview cube = cube_model(0.5f);   // unit cube
    const float w[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  100, 0, 0, 1};
    const float o[3] = {0, 0, 0}, d[3] = {1, 0, 0};
    CHECK_NEAR(ray_model_hit(cube, w, o, d), 99.5f, 1e-3f);
    // From the far side the nearer face is x = 100.5.
    const float o2[3] = {200, 0, 0}, d2[3] = {-1, 0, 0};
    CHECK_NEAR(ray_model_hit(cube, w, o2, d2), 99.5f, 1e-3f);
}

CM_TEST(world_view, ray_model_miss) {
    const ModelPreview cube = cube_model(0.5f);
    const float w[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  100, 0, 0, 1};
    const float o[3] = {0, 0, 0};
    const float up[3] = {0, 0, -1}, back[3] = {-1, 0, 0};
    CHECK_EQ(ray_model_hit(cube, w, o, up), -1.0f);
    CHECK_EQ(ray_model_hit(cube, w, o, back), -1.0f);   // behind the origin
    const float o2[3] = {0, 2, 0}, d[3] = {1, 0, 0};    // passes beside it
    CHECK_EQ(ray_model_hit(cube, w, o2, d), -1.0f);
    // A singular matrix, an empty model: no hit, no NaN.
    const float zero[16] = {};
    CHECK_EQ(ray_model_hit(cube, zero, o, d), -1.0f);
    CHECK_EQ(ray_model_hit(ModelPreview{}, w, o, d), -1.0f);
}

CM_TEST(world_view, ray_model_hit_rotated_scaled) {
    // A 90-degree turn about Z (x -> y) with scale 3, at (0, 50, 0): the unit
    // cube spans y = 50 +- 1.5, so a +Y ray from the origin hits at 48.5 (t
    // stays in world units, not the model's).
    const ModelPreview cube = cube_model(0.5f);
    const float w[16] = {0, 3, 0, 0,  -3, 0, 0, 0,  0, 0, 3, 0,  0, 50, 0, 1};
    const float o[3] = {0, 0, 0}, d[3] = {0, 1, 0};
    CHECK_NEAR(ray_model_hit(cube, w, o, d), 48.5f, 1e-3f);
    // Non-uniform scale: x stretched 10x reaches x = 105 from a cube at 100.
    const float s[16] = {10, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  100, 0, 0, 1};
    const float o2[3] = {200, 0, 0}, d2[3] = {-1, 0, 0};
    CHECK_NEAR(ray_model_hit(cube, s, o2, d2), 95.0f, 1e-3f);
    // Only LOD0 counts: the coarse LOD drops two faces, LOD0 still has all six.
    const float o3[3] = {0, 50, 100}, d3[3] = {0, 0, -1};
    CHECK_NEAR(ray_model_hit(cube, w, o3, d3), 98.5f, 1e-3f);
}

namespace {

/// A synthetic two-chunk map: rolling hills, chunks side by side east-west.
castlemist::world::WorldScene two_chunk_scene() {
    castlemist::world::WorldScene s;
    s.terrain.present = true;
    s.terrain.chunksX = 2;
    s.terrain.chunksY = 1;
    const int n = 17;
    for (int cx = 0; cx < 2; ++cx) {
        castlemist::world::TerrainChunk c;
        c.cx = cx;
        c.cy = 0;
        c.rect[0] = cx * 1000.0f; c.rect[1] = 0; c.rect[2] = cx * 1000.0f + 1000.0f; c.rect[3] = 1000;
        c.samples = n;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const float x = c.rect[0] + i * 1000.0f / (n - 1), y = c.rect[3] - j * 1000.0f / (n - 1);
                c.heights.push_back(100.0f + 120.0f * std::sin(x / 180.0f) * std::cos(y / 230.0f));
            }
        s.terrain.chunks.push_back(std::move(c));
    }
    return s;
}

HWND hidden_window(int w, int h) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cm_test_world_view";
    RegisterClassA(&wc);   // fails harmlessly when already registered
    return CreateWindowExA(0, wc.lpszClassName, "world_view test", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

} // namespace

CM_TEST(world_view, renders_two_terrain_chunks) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");

    v.set_world(two_chunk_scene());
    CHECK_EQ(v.stats().chunks, 2);
    float speed = 0;
    const WorldCamera cam = v.frame_all(&speed);
    CHECK(speed > 0);
    CHECK(cam.pos[2] < -20.0f);   // above the highest hill (up = -Z)
    v.render(cam);
    CHECK_EQ(v.stats().visible, 2);
    CHECK_EQ(v.stats().draws, 2);

    // Layer off -> nothing drawn.
    v.set_layer(WorldLayer::Terrain, false);
    CHECK_FALSE(v.layer(WorldLayer::Terrain));
    v.render(cam);
    CHECK_EQ(v.stats().visible, 0);
    v.set_layer(WorldLayer::Terrain, true);

    // Off the map's west edge, looking further west -> both chunks culled.
    WorldCamera away;
    away.pos[0] = -3000; away.pos[1] = 500; away.pos[2] = 0;
    away.fwd[0] = -1; away.fwd[1] = 0; away.fwd[2] = 0;
    v.render(away);
    CHECK_EQ(v.stats().visible, 0);

    v.resize(400, 300);
    v.render(cam);
    CHECK_FALSE(v.device_lost());
    const WorldViewStats before = v.stats();
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "cm_world_view_smoke.bmp";
    CHECK(v.save_screenshot(path.c_str()));
    // The screenshot's redraw leaves the presented frame's stats alone (ms
    // would differ between two draws).
    CHECK(v.stats().ms == before.ms);
    CHECK_EQ(v.stats().visible, before.visible);

    FILE* f = std::fopen(path.c_str(), "rb");
    CHECK(f != nullptr);
    if (!f) return;
    std::vector<uint8_t> bmp;
    uint8_t buf[4096];
    for (size_t r; (r = std::fread(buf, 1, sizeof buf, f)) > 0;) bmp.insert(bmp.end(), buf, buf + r);
    std::fclose(f);
    CHECK(bmp.size() > 54 && bmp[0] == 'B' && bmp[1] == 'M');
    if (bmp.size() <= 54) return;
    int32_t bw = 0, bh = 0;
    std::memcpy(&bw, &bmp[18], 4);
    std::memcpy(&bh, &bmp[22], 4);
    CHECK_EQ(bw, 400);
    CHECK_EQ(bh, 300);

    // Luma variance over the image: one uniform colour (a clear-only frame) is 0.
    double sum = 0, sum2 = 0;
    size_t count = 0, notClear = 0;
    const int stride = (bw * 3 + 3) & ~3;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = &bmp[54 + static_cast<size_t>(y) * stride + x * 3];
            const double l = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
            sum += l;
            sum2 += l * l;
            ++count;
            // clear colour {0.10, 0.11, 0.13} = BGR ~(33, 28, 25)
            if (std::abs(p[0] - 33) + std::abs(p[1] - 28) + std::abs(p[2] - 25) > 6) ++notClear;
        }
    const double mean = sum / count, var = sum2 / count - mean * mean;
    std::printf("      smoke: %s mean luma %.1f variance %.1f, %zu/%zu px are not the clear colour\n", path.c_str(),
                mean, var, notClear, count);
    CHECK(var > 10.0);
    CHECK(notClear > count / 10);
    CHECK(notClear < count);   // some sky is left: the whole map is in frame
}

namespace {

/// Pixels in a 24-bit BMP that are not the clear colour; -1 when unreadable.
long count_not_clear(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<uint8_t> bmp;
    uint8_t buf[4096];
    for (size_t r; (r = std::fread(buf, 1, sizeof buf, f)) > 0;) bmp.insert(bmp.end(), buf, buf + r);
    std::fclose(f);
    if (bmp.size() <= 54 || bmp[0] != 'B' || bmp[1] != 'M') return -1;
    int32_t bw = 0, bh = 0;
    std::memcpy(&bw, &bmp[18], 4);
    std::memcpy(&bh, &bmp[22], 4);
    const int stride = (bw * 3 + 3) & ~3;
    if (bmp.size() < 54 + static_cast<size_t>(stride) * bh) return -1;
    long n = 0;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = &bmp[54 + static_cast<size_t>(y) * stride + x * 3];
            if (std::abs(p[0] - 33) + std::abs(p[1] - 28) + std::abs(p[2] - 25) > 6) ++n;
        }
    return n;
}

/// Five cubes in a row 500 units up (z = -500, up = -Z), one collision square
/// under the first, and a water plane at z = -400 over the bounds. No terrain.
castlemist::world::WorldScene props_scene() {
    castlemist::world::WorldScene s;
    s.hasBounds = true;
    s.bounds[0] = -1500; s.bounds[1] = -300; s.bounds[2] = 2700; s.bounds[3] = 300;
    s.models.push_back({1234});
    for (int i = 0; i < 5; ++i) {
        castlemist::world::PropInstance p;
        p.model = 0;
        const float k = (i == 2) ? 2.0f : 1.0f;   // the middle one is twice the size
        p.world[0] = k; p.world[5] = k; p.world[10] = k; p.world[15] = 1;
        p.world[12] = i * 300.0f; p.world[13] = 0; p.world[14] = -500;
        s.props.push_back(p);
    }
    castlemist::world::CollisionMesh cm;
    cm.verts = {-100, -100, 0,  100, -100, 0,  -100, 100, 0,  100, 100, 0};
    cm.indices = {0, 1, 2,  1, 3, 2};
    s.collision.meshes.push_back(cm);
    castlemist::world::CollisionInstance ci;
    ci.world[0] = ci.world[5] = ci.world[10] = ci.world[15] = 1;
    ci.world[14] = -440;
    s.collision.instances.push_back(ci);
    s.water.hasPlane = true;
    s.water.planeZ = -400;
    return s;
}

} // namespace

CM_TEST(world_view, renders_streamed_props_collision_and_water) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    CHECK_FALSE(v.layer(WorldLayer::Collision));   // off by default
    CHECK(v.layer(WorldLayer::Props));
    CHECK(v.layer(WorldLayer::Water));

    v.set_world(props_scene());
    CHECK_EQ(v.stats().chunks, 0);
    CHECK_EQ(v.stats().modelsLoaded, 0);
    // R8: collision stays on the CPU until the layer is first shown.
    CHECK_EQ(v.stats().collisionMeshes, 0);
    // R7: the props widen the framing extent, so the camera starts above them.
    const WorldCamera cam = v.frame_all(nullptr);
    CHECK(cam.pos[2] < -500.0f);

    // Before any model arrives the props are skipped.
    v.set_layer(WorldLayer::Water, false);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 0);
    CHECK_EQ(v.stats().draws, 0);

    auto cube = std::make_shared<const ModelPreview>(cube_model());
    v.add_models({{0u, cube}});
    CHECK_EQ(v.stats().modelsLoaded, 1);
    CHECK_EQ(v.stats().modelsFailed, 0);
    // Out of range, null, and an already loaded model: two failures, no reload.
    v.add_models({{9u, cube}, {0u, nullptr}, {0u, cube}});
    CHECK_EQ(v.stats().modelsLoaded, 1);
    CHECK_EQ(v.stats().modelsFailed, 2);

    v.render(cam);
    CHECK_EQ(v.stats().visible, 5);
    CHECK(v.stats().draws > 0);
    CHECK(v.stats().draws <= 2);   // one per (model, LOD) bucket, not per instance
    CHECK_EQ(v.stats().modelsLoaded, 1);

    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string onPath = std::string(tmp) + "cm_world_view_props_on.bmp";
    const std::string offPath = std::string(tmp) + "cm_world_view_props_off.bmp";
    CHECK(v.save_screenshot(onPath.c_str()));

    v.set_layer(WorldLayer::Props, false);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 0);
    CHECK_EQ(v.stats().draws, 0);
    CHECK(v.save_screenshot(offPath.c_str()));
    v.set_layer(WorldLayer::Props, true);

    const long on = count_not_clear(onPath), off = count_not_clear(offPath);
    std::printf("      smoke: props on %ld px, props off %ld px not the clear colour (%s)\n", on, off, onPath.c_str());
    CHECK(on > 500);
    CHECK_EQ(off, 0L);

    // Picking: straight down onto the middle (doubled) cube from 1500 above.
    WorldCamera down;
    down.pos[0] = 600; down.pos[1] = 0; down.pos[2] = -2000;
    down.fwd[0] = 0; down.fwd[1] = 0; down.fwd[2] = 1;   // +Z is down
    down.up[0] = 1; down.up[1] = 0; down.up[2] = 0;
    CHECK_EQ(v.pick(160, 120, down), 2);
    // Looking along the row from the west: the nearest cube (index 0) wins.
    WorldCamera east;
    east.pos[0] = -1000; east.pos[1] = 0; east.pos[2] = -500;
    CHECK_EQ(v.pick(160, 120, east), 0);
    // Empty sky (straight up) and a pixel beside the row miss.
    WorldCamera sky = down;
    sky.fwd[2] = -1;
    CHECK_EQ(v.pick(160, 120, sky), -1);
    CHECK_EQ(v.pick(0, 0, down), -1);
    // Props layer off: nothing to pick.
    v.set_layer(WorldLayer::Props, false);
    CHECK_EQ(v.pick(160, 120, down), -1);
    v.set_layer(WorldLayer::Props, true);

    // Collision layer: uploaded on first show (R8); one more instance and one
    // more draw.
    CHECK_EQ(v.stats().collisionMeshes, 0);
    v.set_layer(WorldLayer::Collision, true);
    CHECK_EQ(v.stats().collisionMeshes, 1);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 6);
    v.set_layer(WorldLayer::Collision, false);

    // Water: one quad, drawn last.
    v.render(cam);
    const int drawsNoWater = v.stats().draws;
    v.set_layer(WorldLayer::Water, true);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 6);
    CHECK_EQ(v.stats().draws, drawsNoWater + 1);
    const std::string waterPath = std::string(tmp) + "cm_world_view_water.bmp";
    CHECK(v.save_screenshot(waterPath.c_str()));
    const long water = count_not_clear(waterPath);
    std::printf("      smoke: with water %ld px not the clear colour (%s)\n", water, waterPath.c_str());
    CHECK(water > on);
    CHECK_FALSE(v.device_lost());

    // A new world drops the loaded models (and so the picks).
    v.set_world(props_scene());
    CHECK_EQ(v.stats().modelsLoaded, 0);
    CHECK_EQ(v.pick(160, 120, down), -1);
    v.set_layer(WorldLayer::Water, false);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 0);

    // With the collision layer already on, set_world still defers the upload
    // to the first render.
    v.set_layer(WorldLayer::Collision, true);
    v.set_world(props_scene());
    CHECK_EQ(v.stats().collisionMeshes, 0);
    v.render(cam);
    CHECK_EQ(v.stats().collisionMeshes, 1);
    CHECK_EQ(v.stats().visible, 1);   // the collision square; no models yet
}

// The Map window's device-recovery sequence: create() again on the same view,
// then set_world and add_models again, then the owner re-applies its layers.
CM_TEST(world_view, recreate_then_reload_restores_the_view) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    auto cube = std::make_shared<const ModelPreview>(cube_model());
    v.set_world(props_scene());
    v.add_models({{0u, cube}});
    const WorldCamera cam = v.frame_all(nullptr);
    v.set_layer(WorldLayer::Water, false);
    v.render(cam);
    CHECK_EQ(v.stats().modelsLoaded, 1);
    CHECK_EQ(v.stats().visible, 5);

    // The owner's layer choice before the "loss": props off, collision on.
    const bool props = false, coll = true, water = false;

    // Recovery: a second create() resets everything...
    if (!v.create(wnd)) SKIP("second D3D11 device could not be created");
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().modelsLoaded, 0);
    CHECK_EQ(v.stats().modelsFailed, 0);
    CHECK_EQ(v.stats().visible, 0);
    CHECK_EQ(v.stats().collisionMeshes, 0);
    CHECK(v.layer(WorldLayer::Props));            // back to the defaults
    CHECK_FALSE(v.layer(WorldLayer::Collision));

    // ...then the world, the kept models and the owner's layers come back.
    v.resize(320, 240);
    v.set_world(props_scene());
    v.add_models({{0u, cube}});
    CHECK_EQ(v.stats().modelsLoaded, 1);
    v.set_layer(WorldLayer::Props, props);
    v.set_layer(WorldLayer::Collision, coll);
    v.set_layer(WorldLayer::Water, water);
    CHECK_EQ(v.stats().collisionMeshes, 1);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 1);   // only the collision square: props are off
    v.set_layer(WorldLayer::Props, true);
    v.render(cam);
    CHECK_EQ(v.stats().visible, 6);   // the five props are back too
}

// ---- Task 7: CPU prep on the workers (MatVtx, SkinVtx, per-submesh ranges) ----

namespace {

/// cube_model's mesh (material 0, two LODs) plus a second mesh (material 1)
/// with one triangle and no lodIndices, so it has fewer LODs than the model.
ModelPreview two_mesh_model() {
    ModelPreview mp = cube_model();
    ModelMeshCPU t;
    for (int k = 0; k < 3; ++k) {
        GVertex v{};
        v.px = 60.0f + k; v.py = (k == 1) ? 10.0f : 0.0f; v.pz = (k == 2) ? 10.0f : 0.0f;
        v.nx = 1;
        t.vertices.push_back(v);
    }
    t.indices = {0, 1, 2};
    t.materialIndex = 1;
    t.vertexCount = 3;
    mp.meshes.push_back(std::move(t));
    return mp;
}

} // namespace

CM_TEST(world_view, flatten_subranges_cover_each_lod_once) {
    const ModelPreview mp = two_mesh_model();
    std::vector<MapVtx> verts;
    std::vector<uint32_t> idx;
    std::vector<std::pair<uint32_t, uint32_t>> lods;
    std::vector<std::vector<SubRange>> subs;
    flatten(mp, verts, idx, lods, &subs);
    CHECK_EQ(lods.size(), size_t(2));
    CHECK_EQ(subs.size(), lods.size());
    if (subs.size() != lods.size()) return;
    for (size_t lod = 0; lod < lods.size(); ++lod) {
        const auto& s = subs[lod];
        CHECK_EQ(s.size(), size_t(2));
        if (s.size() != 2) continue;
        CHECK_EQ(s[0].first, lods[lod].first);
        uint32_t sum = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            if (i + 1 < s.size()) CHECK_EQ(s[i + 1].first, s[i].first + s[i].count);
            sum += s[i].count;
        }
        CHECK_EQ(sum, lods[lod].second);
        CHECK_EQ(s[0].material, 0u);
        CHECK_EQ(s[1].material, 1u);
    }
    // The 4-argument form gives the same buffers.
    std::vector<MapVtx> v4;
    std::vector<uint32_t> i4;
    std::vector<std::pair<uint32_t, uint32_t>> l4;
    flatten(mp, v4, i4, l4);
    CHECK(i4 == idx);
    CHECK(l4 == lods);
}

CM_TEST(world_view, prepare_matvtx_carries_uvs) {
    CHECK_EQ(sizeof(MatVtx), size_t(32));
    CHECK_EQ(sizeof(SkinVtx), size_t(8));
    ModelPreview mp = cube_model();
    GVertex& g = mp.meshes[0].vertices[5];
    g.u = 0.25f; g.v = 0.5f;
    g.uv1[0][0] = 2.0f; g.uv1[0][1] = 3.0f;
    const PreparedModel p = prepare_world_model(mp);
    CHECK(p.ok);
    CHECK(p.verts.empty());
    CHECK_EQ(p.matVerts.size(), size_t(24));
    if (p.matVerts.size() != 24) return;
    const MatVtx& o = p.matVerts[5];
    CHECK_NEAR(o.u0, 0.25f, 1e-7f);
    CHECK_NEAR(o.v0, 0.5f, 1e-7f);
    CHECK_NEAR(o.u1, 2.0f, 1e-7f);
    CHECK_NEAR(o.v1, 3.0f, 1e-7f);
    CHECK_NEAR(o.px, g.px, 1e-7f);
    CHECK(o.ao >= 76);   // AO baked before the copy
    CHECK_EQ(p.subRanges.size(), p.lodRanges.size());
}

CM_TEST(world_view, prepare_skin_only_when_asked) {
    ModelPreview mp = two_mesh_model();
    mp.meshes[0].hasSkin = true;
    GVertex& g = mp.meshes[0].vertices[3];
    g.bidx[0] = 7; g.bidx[1] = 2; g.bidx[2] = 1; g.bidx[3] = 0;
    g.bwt[0] = 0.5f; g.bwt[1] = 0.25f; g.bwt[2] = 0.25f; g.bwt[3] = 0.0f;
    mp.meshes[1].hasSkin = false;

    CHECK(prepare_world_model(mp, false).skinVerts.empty());
    const PreparedModel p = prepare_world_model(mp, true);
    CHECK(p.ok);
    CHECK_EQ(p.skinVerts.size(), p.matVerts.size());
    if (p.skinVerts.size() != 27) return;
    const SkinVtx& s = p.skinVerts[3];
    CHECK_EQ(int(s.bone[0]), 7);
    CHECK_EQ(int(s.bone[1]), 2);
    CHECK_EQ(int(s.weight[0]), 128);
    CHECK_EQ(int(s.weight[1]), 64);
    CHECK_EQ(int(s.weight[2]), 64);
    CHECK_EQ(int(s.weight[3]), 0);
    for (size_t i = 24; i < 27; ++i)   // the unskinned second mesh
        for (int k = 0; k < 4; ++k) CHECK_EQ(int(p.skinVerts[i].weight[k]), 0);

    // More than 256 joints: no skinVerts (the byte indices cannot reach them).
    mp.joints.resize(257);
    CHECK(prepare_world_model(mp, true).skinVerts.empty());
}

CM_TEST(world_view, prepared_add_matches_preview_add) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_layer(WorldLayer::Water, false);
    auto cube = std::make_shared<const ModelPreview>(cube_model());

    v.set_world(props_scene());
    const WorldCamera cam = v.frame_all(nullptr);
    v.add_models({{0u, cube}});
    v.render(cam);
    const WorldViewStats a = v.stats();

    v.set_world(props_scene());
    CHECK_EQ(v.stats().modelsLoaded, 0);
    v.add_models(std::vector<WorldModelLoad>{WorldModelLoad{0, cube, prepare_world_model_shared(*cube, false)}});
    CHECK_EQ(v.stats().modelsLoaded, 1);
    v.render(cam);
    const WorldViewStats b = v.stats();
    CHECK_EQ(a.visible, 5);
    CHECK_EQ(b.visible, a.visible);
    CHECK_EQ(b.draws, a.draws);
    CHECK_EQ(b.ktris, a.ktris);

    // A null or not-ok prep counts as failed.
    v.set_world(props_scene());
    v.add_models(std::vector<WorldModelLoad>{WorldModelLoad{0, cube, nullptr}});
    CHECK_EQ(v.stats().modelsLoaded, 0);
    CHECK_EQ(v.stats().modelsFailed, 1);
}

// ---- Task 8: the texture cache ----

// Device recovery re-adds the models but not the textures: create() drops the
// cache with the old device, and nothing brings it back.
CM_TEST(world_view, recreate_drops_textures) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    auto cube = std::make_shared<const ModelPreview>(cube_model());
    v.set_world(props_scene());
    auto tex = std::make_shared<CappedTexture>();
    tex->fileId = 77;
    tex->width = tex->height = 4;
    tex->mips.emplace_back(4 * 4 * 4, uint8_t(200));
    v.add_textures({tex, nullptr});   // a null entry is skipped
    v.add_models({{0u, cube}});
    CHECK_EQ(v.stats().textures, 1);
    CHECK_EQ(v.stats().texturesFailed, 0);
    CHECK(v.stats().textureMB > 0.0f);

    if (!v.create(wnd)) SKIP("second D3D11 device could not be created");
    v.resize(320, 240);
    v.set_world(props_scene());
    v.add_models({{0u, cube}});
    const WorldCamera cam = v.frame_all(nullptr);
    v.render(cam);
    CHECK_FALSE(v.device_lost());
    CHECK_EQ(v.stats().modelsLoaded, 1);
    CHECK_EQ(v.stats().textures, 0);
    CHECK_EQ(v.stats().textureMB, 0.0f);
}

namespace {

/// Mean luma of the pixels that are not the clear colour; -1 when unreadable or none.
double lit_mean_luma(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<uint8_t> bmp;
    uint8_t buf[4096];
    for (size_t r; (r = std::fread(buf, 1, sizeof buf, f)) > 0;) bmp.insert(bmp.end(), buf, buf + r);
    std::fclose(f);
    if (bmp.size() <= 54 || bmp[0] != 'B' || bmp[1] != 'M') return -1;
    int32_t bw = 0, bh = 0;
    std::memcpy(&bw, &bmp[18], 4);
    std::memcpy(&bh, &bmp[22], 4);
    const int stride = (bw * 3 + 3) & ~3;
    if (bmp.size() < 54 + static_cast<size_t>(stride) * bh) return -1;
    double sum = 0;
    long n = 0;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x) {
            const uint8_t* p = &bmp[54 + static_cast<size_t>(y) * stride + x * 3];
            if (std::abs(p[0] - 33) + std::abs(p[1] - 28) + std::abs(p[2] - 25) <= 6) continue;
            sum += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
            ++n;
        }
    return n ? sum / n : -1;
}

} // namespace

// Review I7: the env's sunDir is the direction the light travels (z > 0 = down,
// up = -Z). Flat ground under a sun travelling straight down must be lit by it,
// and brighter than under one travelling straight up (which lights it from below).
CM_TEST(world_view, env_sun_travel_direction_lights_the_ground) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    double luma[2] = {-1, -1};
    for (int k = 0; k < 2; ++k) {
        castlemist::world::WorldScene s = two_chunk_scene();
        for (auto& c : s.terrain.chunks)
            for (float& h : c.heights) h = 100.0f;   // flat: every normal is up (-Z)
        auto& L = s.environment.light;
        L.present = true;
        L.sunDir[0] = 0; L.sunDir[1] = 0; L.sunDir[2] = k == 0 ? 1.0f : -1.0f;   // down, then up
        L.sunColor[0] = L.sunColor[1] = L.sunColor[2] = 1.0f;
        L.sunIntensity = 1.0f;
        L.fillIntensity = 0.1f;
        v.set_world(s);
        const WorldCamera cam = v.frame_all(nullptr);
        v.render(cam);
        const std::string path = std::string(tmp) + "cm_world_view_sun" + std::to_string(k) + ".bmp";
        CHECK(v.save_screenshot(path.c_str()));
        luma[k] = lit_mean_luma(path);
    }
    std::printf("      sun travelling down: luma %.1f; travelling up: %.1f\n", luma[0], luma[1]);
    CHECK(luma[0] > 0 && luma[1] > 0);
    CHECK(luma[0] > luma[1] + 10.0);
}

// ---- Foliage grid (spec D6), pure ----------------------------------------

namespace {

/// Bounds (0, 0, 6144, 6144), one type-3 layer with fadeRange [n, f], one
/// model; add instances with add_zone.
castlemist::world::WorldScene foliage_scene(float n = 100, float f = 200) {
    castlemist::world::WorldScene s;
    s.hasBounds = true;
    s.bounds[0] = 0; s.bounds[1] = 0; s.bounds[2] = 6144; s.bounds[3] = 6144;
    s.models.push_back({4321});
    castlemist::world::ZoneLayer L;
    L.type = 3;
    L.fadeRange[0] = n; L.fadeRange[1] = f;
    s.zones.present = true;
    s.zones.layers.push_back(L);
    return s;
}

/// A zone instance at (x, y, z) on layer 0, model 0, uniform scale.
void add_zone(castlemist::world::WorldScene& s, float x, float y, float z,
              castlemist::world::ZoneSource src = castlemist::world::ZoneSource::Collider, float scale = 1) {
    castlemist::world::ZoneInstance zi;
    zi.model = 0;
    zi.layer = 0;
    zi.source = src;
    zi.world[0] = zi.world[5] = zi.world[10] = scale;
    zi.world[15] = 1;
    zi.world[12] = x; zi.world[13] = y; zi.world[14] = z;
    s.zones.instances.push_back(zi);
}

/// Planes that keep everything (n = 0, d = 1).
void all_planes(float p[6][4]) {
    for (int k = 0; k < 6; ++k) { p[k][0] = p[k][1] = p[k][2] = 0; p[k][3] = 1; }
}

/// Sets every instance's sphere at its translation with radius r.
void spheres_at_translation(FoliageGrid& g, const castlemist::world::WorldScene& s, float r) {
    for (uint32_t i = 0; i < s.zones.instances.size(); ++i)
        foliage_set_sphere(g, i, s.zones.instances[i].world + 12, r);
}

} // namespace

CM_TEST(foliage_grid, fade_ends) {
    CHECK_EQ(foliage_fade(50, 100, 200), 1.0f);
    CHECK_NEAR(foliage_fade(150, 100, 200), 0.5f, 1e-6f);
    CHECK_EQ(foliage_fade(200, 100, 200), 0.0f);
    CHECK_EQ(foliage_fade(250, 100, 200), 0.0f);
    // n >= f: a step at f.
    CHECK_EQ(foliage_fade(199, 300, 200), 1.0f);
    CHECK_EQ(foliage_fade(200, 300, 200), 0.0f);
}

CM_TEST(foliage_grid, bayer_matrix) {
    CHECK_EQ(bayer4x4(0, 0), 0.5f / 16);
    CHECK_EQ(bayer4x4(1, 0), 8.5f / 16);
    std::vector<float> seen;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const float v = bayer4x4(x, y);
            CHECK(v > 0.0f);
            CHECK(v < 1.0f);
            for (float o : seen) CHECK(o != v);
            seen.push_back(v);
        }
    CHECK_EQ(seen.size(), size_t(16));
    CHECK_EQ(bayer4x4(4, 4), bayer4x4(0, 0));
}

CM_TEST(foliage_grid, instances_in_tiles) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, 0);
    add_zone(s, 1600, 10, 0);
    const FoliageGrid g = build_foliage_grid(s);
    CHECK_EQ(g.tilesX, 4);
    CHECK_EQ(g.tilesY, 4);
    CHECK_EQ(g.tiles.size(), size_t(16));
    CHECK_EQ(g.tileOf.size(), size_t(2));
    CHECK_EQ(g.tileOf[0], 0u);
    CHECK_EQ(g.tileOf[1], 1u);
    CHECK_EQ(g.tiles[0].instances.size(), size_t(1));
    CHECK_EQ(g.tiles[1].instances.size(), size_t(1));
    const float distanceScale = 1;
    CHECK_EQ(g.tiles[0].farMax * distanceScale, 200.0f);
    CHECK_EQ(g.tiles[1].farMax * distanceScale, 200.0f);
    CHECK_EQ(g.fadeNear[0], 100.0f);
    CHECK_EQ(g.fadeFar[1], 200.0f);
    CHECK_EQ(g.sphere.size(), size_t(8));
    CHECK(g.sphere[3] < 0.0f);
    CHECK_FALSE(g.tiles[0].hasAabb);
    CHECK_EQ(g.collider[0], 1);
}

CM_TEST(foliage_grid, unresolved_layer_takes_largest_type3_fade) {
    auto s = foliage_scene();
    castlemist::world::ZoneLayer big;
    big.type = 3;
    big.fadeRange[0] = 900; big.fadeRange[1] = 4000;
    castlemist::world::ZoneLayer grass;   // farther, but not type 3
    grass.type = 1;
    grass.fadeRange[0] = 9000; grass.fadeRange[1] = 9500;
    s.zones.layers.push_back(big);
    s.zones.layers.push_back(grass);
    add_zone(s, 10, 10, 0);
    s.zones.instances[0].layer = UINT32_MAX;   // a collider linked by geometry only
    const FoliageGrid g = build_foliage_grid(s);
    CHECK_EQ(g.fadeNear[0], 900.0f);
    CHECK_EQ(g.fadeFar[0], 4000.0f);
    CHECK_EQ(g.tiles[0].farMax, 4000.0f);
}

CM_TEST(foliage_grid, aabb_from_spheres) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, -5);
    FoliageGrid g = build_foliage_grid(s);
    const float c[3] = {10, 10, -5};
    foliage_set_sphere(g, 0, c, 3);
    const FoliageTile& t = g.tiles[0];
    CHECK(t.hasAabb);
    CHECK_EQ(t.aabbMin[0], 7.0f); CHECK_EQ(t.aabbMin[1], 7.0f); CHECK_EQ(t.aabbMin[2], -8.0f);
    CHECK_EQ(t.aabbMax[0], 13.0f); CHECK_EQ(t.aabbMax[1], 13.0f); CHECK_EQ(t.aabbMax[2], -2.0f);
    CHECK_EQ(g.sphere[3], 3.0f);
}

CM_TEST(foliage_grid, far_tile_culled) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, 0);
    FoliageGrid g = build_foliage_grid(s);
    spheres_at_translation(g, s, 3);
    float planes[6][4];
    all_planes(planes);
    std::vector<FoliageVisible> out;
    FoliageCullStats st;
    const float away[3] = {10, 5010, 0};
    foliage_cull(g, away, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK(out.empty());
    CHECK_EQ(st.tiles, 0);
    const float inside[3] = {10, 50, 0};   // in tile 0, 40 units from the instance
    foliage_cull(g, inside, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) {
        CHECK_EQ(out[0].instance, 0u);
        CHECK_EQ(out[0].fade, 1.0f);
    }
    CHECK_EQ(st.tiles, 1);
    // A frustum plane with the tile behind it culls the tile too.
    planes[0][0] = 1; planes[0][3] = -1000;   // keeps x >= 1000 only
    foliage_cull(g, inside, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK(out.empty());
    CHECK_EQ(st.tiles, 0);
    // The distance scale stretches the range: 150 units away, fade 0.5 at scale 1, 1 at scale 2.
    all_planes(planes);
    const float mid[3] = {10, 160, 0};
    foliage_cull(g, mid, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) CHECK_NEAR(out[0].fade, 0.5f, 1e-5f);
    foliage_cull(g, mid, planes, 2, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) CHECK_EQ(out[0].fade, 1.0f);
}

CM_TEST(foliage_grid, cap_keeps_nearest) {
    auto s = foliage_scene(100000, 200000);
    for (int t = 2; t >= 0; --t)   // farthest tile first, so index order is not distance order
        for (int j = 0; j < 10; ++j) add_zone(s, 100.0f + 1536.0f * t + j, 100, 0);
    FoliageGrid g = build_foliage_grid(s);
    spheres_at_translation(g, s, 1);
    float planes[6][4];
    all_planes(planes);
    std::vector<FoliageVisible> out;
    FoliageCullStats st;
    const float cam[3] = {0, 100, 0};
    foliage_cull(g, cam, planes, 1, false, 15, out, st);
    CHECK_EQ(out.size(), size_t(15));
    CHECK_EQ(st.capped, 15);
    CHECK_EQ(st.tiles, 3);
    int nearest = 0, next = 0;
    for (const FoliageVisible& v : out) {
        if (v.instance >= 20) ++nearest;     // tile 0: instances 20..29
        else if (v.instance >= 10) ++next;   // tile 1: instances 10..19
    }
    CHECK_EQ(nearest, 10);
    CHECK_EQ(next, 5);
}

CM_TEST(foliage_grid, colliders_only) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, 0, castlemist::world::ZoneSource::Collider);
    add_zone(s, 20, 10, 0, castlemist::world::ZoneSource::Density);
    FoliageGrid g = build_foliage_grid(s);
    CHECK_EQ(g.collider[0], 1);
    CHECK_EQ(g.collider[1], 0);
    spheres_at_translation(g, s, 3);
    float planes[6][4];
    all_planes(planes);
    std::vector<FoliageVisible> out;
    FoliageCullStats st;
    const float cam[3] = {15, 30, 0};
    foliage_cull(g, cam, planes, 1, true, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) CHECK_EQ(out[0].instance, 0u);
    foliage_cull(g, cam, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(2));
}

CM_TEST(foliage_grid, tile_without_spheres_skipped) {
    auto s = foliage_scene(100000, 200000);
    add_zone(s, 1530, 10, 0);   // tile 0: never gets a sphere
    add_zone(s, 1540, 10, 0);   // tile 1
    FoliageGrid g = build_foliage_grid(s);
    const float c[3] = {1540, 10, 0};
    foliage_set_sphere(g, 1, c, 3);
    float planes[6][4];
    all_planes(planes);
    std::vector<FoliageVisible> out;
    FoliageCullStats st;
    const float cam[3] = {1536, 10, 0};
    foliage_cull(g, cam, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(st.tiles, 1);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) CHECK_EQ(out[0].instance, 1u);
}

CM_TEST(foliage_grid, unloaded_instance_not_visible) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, 0);
    add_zone(s, 20, 10, 0);
    FoliageGrid g = build_foliage_grid(s);
    const float c[3] = {20, 10, 0};
    foliage_set_sphere(g, 1, c, 3);
    CHECK(g.sphere[3] < 0.0f);
    float planes[6][4];
    all_planes(planes);
    std::vector<FoliageVisible> out;
    FoliageCullStats st;
    const float cam[3] = {15, 30, 0};
    foliage_cull(g, cam, planes, 1, false, kFoliageMaxVisible, out, st);
    CHECK_EQ(out.size(), size_t(1));
    if (!out.empty()) CHECK_EQ(out[0].instance, 1u);
}

CM_TEST(foliage_grid, set_sphere_out_of_range_ignored) {
    auto s = foliage_scene();
    add_zone(s, 10, 10, 0);
    FoliageGrid g = build_foliage_grid(s);
    const std::vector<float> before = g.sphere;
    const float c[3] = {10, 10, 0};
    foliage_set_sphere(g, 999, c, 3);
    CHECK(g.sphere == before);
    for (const FoliageTile& t : g.tiles) CHECK_FALSE(t.hasAabb);
}

// ---- Foliage in WorldView (spec D6) ---------------------------------------

namespace {

/// foliage_scene with a wide fade, model 0 shared by one prop (west) and
/// two zone instances 300 and 600 units east of it: a Collider, then a Density.
castlemist::world::WorldScene foliage_view_scene() {
    auto s = foliage_scene(50000, 60000);
    castlemist::world::PropInstance p;
    p.model = 0;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.world[12] = 3000; p.world[13] = 3000; p.world[14] = -500;
    s.props.push_back(p);
    add_zone(s, 3300, 3000, -500, castlemist::world::ZoneSource::Collider);
    add_zone(s, 3600, 3000, -500, castlemist::world::ZoneSource::Density);
    s.zones.collidersUnlinked = 2;
    return s;
}

/// Looking north at the row from 1500 units south of it, level.
WorldCamera foliage_camera() {
    WorldCamera c;
    c.pos[0] = 3300; c.pos[1] = 1500; c.pos[2] = -500;
    c.fwd[0] = 0; c.fwd[1] = 1; c.fwd[2] = 0;
    return c;
}

} // namespace

CM_TEST(world_view, foliage_draws_zone_instances) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    CHECK(v.layer(WorldLayer::Foliage));   // on after create()

    const auto s = foliage_view_scene();
    v.set_world(s, std::make_unique<FoliageGrid>(build_foliage_grid(s)));
    CHECK_EQ(v.stats().foliageInstances, 2);
    CHECK_EQ(v.stats().foliagePlaced, 0);
    CHECK_EQ(v.stats().zoneTrees, 1);
    CHECK_EQ(v.stats().zoneUnlinked, 2);
    v.set_layer(WorldLayer::Props, false);   // the prop shares the model; keep it out of the pixels
    v.set_layer(WorldLayer::Water, false);
    const WorldCamera cam = foliage_camera();
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 0);   // no model yet
    CHECK_EQ(v.stats().foliageTiles, 0);

    auto cube = std::make_shared<const ModelPreview>(cube_model());
    v.add_models({{0u, cube}});
    CHECK_EQ(v.stats().modelsLoaded, 1);     // one MeshGPU for the prop and the zone instances
    CHECK_EQ(v.stats().foliagePlaced, 2);

    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 1);   // Colliders after create()
    CHECK(v.stats().foliageDraws > 0);
    CHECK(v.stats().foliageTiles >= 1);
    CHECK_EQ(v.stats().foliageCapped, 0);
    CHECK_EQ(v.stats().shaderFoliage, 0);
    CHECK(v.stats().foliageCullMs >= 0.0f);
    CHECK(v.stats().visible >= 1);

    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string onPath = std::string(tmp) + "cm_world_view_foliage_on.bmp";
    const std::string offPath = std::string(tmp) + "cm_world_view_foliage_off.bmp";
    CHECK(v.save_screenshot(onPath.c_str()));

    v.set_foliage_show(FoliageShow::All);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 2);

    v.set_shading(WorldShading::Geometry);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 2);
    CHECK(v.stats().foliageDraws > 0);
    v.set_shading(WorldShading::Shader);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 2);
    CHECK_EQ(v.stats().shaderFoliage, 2);    // drawn in the Materials look
    v.set_shading(WorldShading::Materials);

    // The distance scale shrinks the fade range: 0.01 x [50000, 60000] ends at 600 units.
    v.set_foliage_distance_scale(0.01f);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 0);
    // Half faded: the collider 1500 units away with [50000, 60000] x 1500/55000
    // = [1363.6, 1636.4] has fade 0.5, so the screen door drops about half its pixels.
    v.set_foliage_show(FoliageShow::Colliders);
    v.set_foliage_distance_scale(1500.0f / 55000.0f);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 1);
    const std::string halfPath = std::string(tmp) + "cm_world_view_foliage_half.bmp";
    CHECK(v.save_screenshot(halfPath.c_str()));
    v.set_foliage_distance_scale(1);

    v.set_layer(WorldLayer::Foliage, false);
    v.render(cam);
    CHECK_EQ(v.stats().foliageVisible, 0);
    CHECK_EQ(v.stats().foliageDraws, 0);
    CHECK_EQ(v.stats().draws, 0);
    CHECK(v.save_screenshot(offPath.c_str()));
    v.set_layer(WorldLayer::Foliage, true);

    const long on = count_not_clear(onPath), half = count_not_clear(halfPath), off = count_not_clear(offPath);
    std::printf("      foliage on %ld px, half faded %ld px, off %ld px not the clear colour\n", on, half, off);
    CHECK(on > 80);
    CHECK(half > on * 3 / 10);
    CHECK(half < on * 7 / 10);
    CHECK_EQ(off, 0L);

    // pick() still returns props only.
    v.set_layer(WorldLayer::Props, true);
    WorldCamera down;
    down.pos[0] = 3300; down.pos[1] = 3000; down.pos[2] = -2000;
    down.fwd[0] = 0; down.fwd[1] = 0; down.fwd[2] = 1;
    down.up[0] = 1; down.up[1] = 0; down.up[2] = 0;
    CHECK_EQ(v.pick(160, 120, down), -1);

    // focus_zone_tree: the Collider instance nearest the camera, from 4x its
    // radius, with the Collision layer on and its screen rect.
    WorldCamera f = cam;
    int rect[4] = {0, 0, 0, 0};
    CHECK(v.focus_zone_tree(f, rect));
    CHECK(v.layer(WorldLayer::Collision));
    const float r = std::sqrt(3.0f) * 50.0f;   // cube_model's sphere
    CHECK_NEAR(f.pos[0], 3300.0f, 1.0f);
    CHECK_NEAR(f.pos[1], 3000.0f - 4.0f * r, 2.0f);
    CHECK_NEAR(f.pos[2], -500.0f, 1.0f);
    CHECK(rect[2] > rect[0]);
    CHECK(rect[3] > rect[1]);
    CHECK(rect[0] <= 160 && rect[2] >= 160);
    CHECK(rect[1] <= 120 && rect[3] >= 120);
}

CM_TEST(world_view, foliage_grid_built_when_not_given) {
    HWND wnd = hidden_window(320, 240);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    WorldCamera f = foliage_camera();
    int rect[4] = {0, 0, 0, 0};
    CHECK_FALSE(v.focus_zone_tree(f, rect));   // no world
    const auto s = foliage_view_scene();
    v.set_world(s);
    CHECK_EQ(v.stats().foliageInstances, 2);
    CHECK_FALSE(v.focus_zone_tree(f, rect));   // no model yet
    v.add_models({{0u, std::make_shared<const ModelPreview>(cube_model())}});
    v.set_layer(WorldLayer::Props, false);
    v.render(foliage_camera());
    CHECK_EQ(v.stats().foliageVisible, 1);
    // A grid for another scene is replaced by one for this scene.
    auto other = foliage_scene();
    add_zone(other, 10, 10, 0);
    v.set_world(s, std::make_unique<FoliageGrid>(build_foliage_grid(other)));
    CHECK_EQ(v.stats().foliageInstances, 2);
}
