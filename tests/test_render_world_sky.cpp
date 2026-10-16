/// @file
/// @brief The Map window's sky: the mode a time of day picks, the sky pass's
///        inverse view-projection, and (device test) the background it draws.

#include "test_framework.h"

#include "castlemist/render/world_sky.h"
#include "castlemist/render/world_view.h"

#include "world_camera.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace castlemist::render;

CM_TEST(world_sky, time_of_day_picks_the_mode) {
    bool fell = true;
    CHECK_EQ(world_sky_mode({0, 1}, WorldSkyTime::Day, &fell), 0);
    CHECK_FALSE(fell);
    CHECK_EQ(world_sky_mode({0, 1}, WorldSkyTime::Night, &fell), 1);
    CHECK_FALSE(fell);
    // Only one mode baked: the other time of day shows it, and says so.
    CHECK_EQ(world_sky_mode({0}, WorldSkyTime::Night, &fell), 0);
    CHECK(fell);
    CHECK_EQ(world_sky_mode({1}, WorldSkyTime::Day, &fell), 1);
    CHECK(fell);
    CHECK_EQ(world_sky_mode({0, 1}, WorldSkyTime::Off, &fell), -1);
    CHECK_FALSE(fell);
    CHECK_EQ(world_sky_mode({}, WorldSkyTime::Day), -1);
    CHECK_EQ(world_sky_mode({2, 3}, WorldSkyTime::Day), -1);   // modes 2/3 (UNPROVEN) never stand in
}

CM_TEST(world_sky, parse_time_of_day) {
    WorldSkyTime t = WorldSkyTime::Off;
    CHECK(parse_world_sky_time("Day", t));
    CHECK(t == WorldSkyTime::Day);
    CHECK(parse_world_sky_time("NIGHT", t));
    CHECK(t == WorldSkyTime::Night);
    CHECK(parse_world_sky_time("off", t));
    CHECK(t == WorldSkyTime::Off);
    t = WorldSkyTime::Night;
    CHECK_FALSE(parse_world_sky_time("dusk", t));
    CHECK_FALSE(parse_world_sky_time("", t));
    CHECK(t == WorldSkyTime::Night);   // untouched
    CHECK_EQ(std::string(world_sky_time_name(WorldSkyTime::Night)), std::string("night"));
}

CM_TEST(world_sky, no_sky_bakes_nothing) {
    castlemist::exportgltf::sky::SkyInputs in;   // sky.present = false
    std::vector<std::string> w;
    CHECK(bake_world_sky(in, 256, w).empty());
    CHECK_EQ(w.size(), size_t(1));
}

CM_TEST(world_sky, horizon_is_the_middle_band) {
    WorldSkyImage img;
    img.width = 4;
    img.height = 100;
    img.rgba.assign(4 * 100 * 4, 0);
    for (int y = 45; y < 55; ++y)
        for (int x = 0; x < 4; ++x) img.rgba[(static_cast<size_t>(y) * 4 + x) * 4 + 1] = 255;
    float c[3];
    world_sky_horizon(img, 0.1f, c);
    CHECK_NEAR(c[0], 0.0f, 1e-6f);
    CHECK_NEAR(c[1], 1.0f, 1e-6f);
}

CM_TEST(world_sky, inverse_viewproj_gives_view_rays) {
    WorldCamera c;   // fwd +X, up -Z
    c.pos[0] = 12345; c.pos[1] = -6789; c.pos[2] = 400;   // ignored: the sky has no position
    const Mat4 inv = world_camera_sky_inv_viewproj(c, 16.0f / 9.0f);
    auto dir = [&](float x, float y, float out[3]) {
        float p[4];
        for (int k = 0; k < 4; ++k) p[k] = x * inv.m[k] + y * inv.m[4 + k] + 1.0f * inv.m[8 + k] + inv.m[12 + k];
        for (int k = 0; k < 3; ++k) out[k] = p[k] / p[3];
        const float l = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        for (int k = 0; k < 3; ++k) out[k] /= l;
    };
    float d[3];
    dir(0, 0, d);
    CHECK_NEAR(d[0], 1.0f, 1e-3f);
    CHECK_NEAR(d[1], 0.0f, 1e-3f);
    CHECK_NEAR(d[2], 0.0f, 1e-3f);
    dir(0, 1, d);   // top of the screen looks up (-Z) by half the fov
    CHECK_NEAR(std::atan2(-d[2], d[0]), c.fovY * 0.5f, 1e-3f);
    dir(1, 0, d);   // right of the screen: right = cross(up, fwd) = -Y
    CHECK(d[1] < -0.5f);
}

namespace {

HWND sky_window(int w, int h) {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "cm_test_world_sky";
    RegisterClassA(&wc);
    return CreateWindowExA(0, wc.lpszClassName, "world_sky test", WS_POPUP, 0, 0, w, h, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::vector<uint8_t> b;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return b;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    return b;
}

/// RGB of pixel (x, y) (y = 0 top) of the view's last frame, via its BMP.
bool shot_pixel(WorldView& v, int w, int h, int x, int y, int rgb[3]) {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    const std::string path = std::string(tmp) + "cm_world_sky.bmp";
    if (!v.save_screenshot(path.c_str())) return false;
    const std::vector<uint8_t> b = read_bytes(path);
    const int stride = (w * 3 + 3) & ~3;
    const size_t at = 54 + static_cast<size_t>(h - 1 - y) * stride + static_cast<size_t>(x) * 3;
    if (b.size() < at + 3) return false;
    rgb[0] = b[at + 2];
    rgb[1] = b[at + 1];
    rgb[2] = b[at];
    return true;
}

/// An equirect whose upper half (looking up) is red, lower half blue, and
/// whose u > 0.5 half (map south, Unity -Z) has green 255.
WorldSkyImage quadrant_sky(int mode, uint8_t red) {
    WorldSkyImage s;
    s.mode = mode;
    s.width = 64;
    s.height = 32;
    s.rgba.resize(64 * 32 * 4);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            uint8_t* p = &s.rgba[(static_cast<size_t>(y) * 64 + x) * 4];
            p[0] = y < 16 ? red : 0;
            p[1] = x >= 32 ? 255 : 0;
            p[2] = y < 16 ? 0 : 200;
            p[3] = 255;
        }
    return s;
}

} // namespace

CM_TEST(world_sky, view_draws_the_sky_behind_an_empty_world) {
    const int W = 160, H = 120;
    HWND wnd = sky_window(W, H);
    if (!wnd) SKIP("no window");
    struct Closer { HWND h; ~Closer() { DestroyWindow(h); } } closer{wnd};
    WorldView v;
    if (!v.create(wnd)) SKIP("no D3D11 device (hardware or WARP)");
    v.set_world(castlemist::world::WorldScene{});
    v.set_sky({quadrant_sky(0, 200), quadrant_sky(1, 90)});
    WorldCamera cam;   // looking east, up -Z
    CHECK_EQ(v.sky_mode(), 0);
    v.render(cam);
    CHECK_EQ(v.stats().skyMode, 0);
    int px[3];
    CHECK(shot_pixel(v, W, H, W / 4, H / 4, px));   // upper left: up, north of east
    CHECK_EQ(px[0], 200);
    CHECK(px[1] < 10);
    CHECK(shot_pixel(v, W, H, 3 * W / 4, H / 4, px));   // upper right: south of east
    CHECK(px[1] > 245);
    CHECK(shot_pixel(v, W, H, W / 4, 3 * H / 4, px));   // well below the horizon: its mean colour
    WorldSkyImage day = quadrant_sky(0, 200);
    float hz[3];
    world_sky_horizon(day, 0.04f, hz);
    CHECK_NEAR(px[0], static_cast<int>(hz[0] * 255.0f + 0.5f), 2);
    CHECK_NEAR(px[2], static_cast<int>(hz[2] * 255.0f + 0.5f), 2);

    v.set_sky_time(WorldSkyTime::Night);
    CHECK_EQ(v.sky_mode(), 1);
    v.render(cam);
    CHECK(shot_pixel(v, W, H, W / 4, H / 4, px));
    CHECK_EQ(px[0], 90);

    // Geometry and Off keep the flat background.
    v.set_shading(WorldShading::Geometry);
    CHECK_EQ(v.sky_mode(), -1);
    v.render(cam);
    CHECK_EQ(v.stats().skyMode, -1);
    CHECK(shot_pixel(v, W, H, W / 4, H / 4, px));
    CHECK(px[0] < 40);
    v.set_shading(WorldShading::Shader);
    CHECK_EQ(v.sky_mode(), 1);
    v.set_sky_time(WorldSkyTime::Off);
    CHECK_EQ(v.sky_mode(), -1);
}
