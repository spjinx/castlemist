/// @file
/// @brief Tests for WorldView's camera maths (pure) and a device smoke test.

#include "test_framework.h"

#include "world_camera.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
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
