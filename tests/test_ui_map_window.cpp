/// @file
/// @brief Tests for the Map window's pure input maths (src/ui/detail/map_window.h).

#include "test_framework.h"

#include "detail/map_window.h"
#include "detail/map_window_loader.h"

#include <cmath>

using namespace castlemist::ui;

namespace {

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
float len3(const float a[3]) { return std::sqrt(dot3(a, a)); }

FlyState make_state() {
    FlyState s;
    s.pos[0] = 10; s.pos[1] = 20; s.pos[2] = -30;
    // Looking along a diagonal, level (up = -Z).
    const float r = 1.0f / std::sqrt(2.0f);
    s.fwd[0] = r; s.fwd[1] = r; s.fwd[2] = 0;
    s.up[0] = 0; s.up[1] = 0; s.up[2] = -1;
    s.speed = 100;
    return s;
}

} // namespace

CM_TEST(map_window, fly_forward_moves_along_fwd) {
    FlyState s = make_state();
    bool keys[8] = {};
    keys[MW_FWD] = true;
    // The brief's "dt 0.5, speed 100" would be clamped to dt 0.1 (see
    // fly_dt_clamped); the same 50-unit step at an unclamped dt:
    s.speed = 1000;
    CHECK(fly_step(s, keys, 0.05f));
    const float r = 1.0f / std::sqrt(2.0f);
    CHECK_NEAR(s.pos[0], 10 + r * 50, 1e-3);
    CHECK_NEAR(s.pos[1], 20 + r * 50, 1e-3);
    CHECK_NEAR(s.pos[2], -30, 1e-3);
}

CM_TEST(map_window, fly_up_is_world_up) {
    FlyState s = make_state();
    // Roll the camera 90 degrees: its own up now lies in the XY plane.
    s.fwd[0] = 1; s.fwd[1] = 0; s.fwd[2] = 0;
    s.up[0] = 0; s.up[1] = 1; s.up[2] = 0;
    bool keys[8] = {};
    keys[MW_UP] = true;
    CHECK(fly_step(s, keys, 0.05f));
    CHECK_NEAR(s.pos[0], 10, 1e-4);
    CHECK_NEAR(s.pos[1], 20, 1e-4);
    CHECK_NEAR(s.pos[2], -30 - 100 * 0.05f, 1e-4);   // world up is -Z

    FlyState d = make_state();
    bool down[8] = {};
    down[MW_DOWN] = true;
    fly_step(d, down, 0.05f);
    CHECK_NEAR(d.pos[2], -30 + 100 * 0.05f, 1e-4);
}

CM_TEST(map_window, fly_dt_clamped) {
    FlyState a = make_state(), b = make_state();
    bool keys[8] = {};
    keys[MW_RIGHT] = true;
    fly_step(a, keys, 5.0f);
    fly_step(b, keys, 0.1f);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(a.pos[i], b.pos[i], 1e-4);
    // and it did move
    CHECK(std::fabs(a.pos[0] - 10) + std::fabs(a.pos[1] - 20) > 1.0f);
}

CM_TEST(map_window, fly_speed_modifiers) {
    FlyState fast = make_state(), slow = make_state();
    bool kf[8] = {}, ks[8] = {};
    kf[MW_UP] = kf[MW_FAST] = true;
    ks[MW_UP] = ks[MW_SLOW] = true;
    fly_step(fast, kf, 0.1f);
    fly_step(slow, ks, 0.1f);
    CHECK_NEAR(fast.pos[2], -30 - 100 * 6 * 0.1f, 1e-3);
    CHECK_NEAR(slow.pos[2], -30 - 100 * 0.15f * 0.1f, 1e-3);
    bool none[8] = {};
    FlyState still = make_state();
    CHECK_FALSE(fly_step(still, none, 0.1f));
}

CM_TEST(map_window, fly_mouse_pitch_past_vertical_no_flip) {
    FlyState s = make_state();
    // +200 degrees of pitch in 10-pixel steps (0.0032 rad per pixel).
    const float total = 200.0f * 3.14159265f / 180.0f;
    const int steps = static_cast<int>(total / (10 * 0.0032f));
    float prevFwd[3] = {s.fwd[0], s.fwd[1], s.fwd[2]};
    for (int i = 0; i < steps; ++i) {
        fly_mouse(s, 0, 10);
        CHECK_NEAR(len3(s.fwd), 1.0f, 1e-4);
        CHECK_NEAR(len3(s.up), 1.0f, 1e-4);
        CHECK_NEAR(dot3(s.fwd, s.up), 0.0f, 1e-4);
        // Each step turns by 0.032 rad: no sudden snap over the pole.
        CHECK(dot3(prevFwd, s.fwd) > std::cos(0.05f));
        prevFwd[0] = s.fwd[0]; prevFwd[1] = s.fwd[1]; prevFwd[2] = s.fwd[2];
    }
    // Past vertical: the camera now looks back over its start, upside down.
    const float r = 1.0f / std::sqrt(2.0f);
    const float start[3] = {r, r, 0};
    CHECK(dot3(s.fwd, start) < 0);
    CHECK(s.up[2] > 0);   // camera up points down the world (+Z)
}

CM_TEST(map_window, fly_mouse_drag_right_turns_right) {
    FlyState s = make_state();
    s.fwd[0] = 1; s.fwd[1] = 0; s.fwd[2] = 0;
    float right[3];
    fly_right(s, right);
    fly_mouse(s, 50, 0);
    CHECK(dot3(s.fwd, right) > 0.1f);
    CHECK_NEAR(s.fwd[2], 0, 1e-4);   // a level yaw stays level
}

CM_TEST(map_window, adjust_speed_clamps) {
    FlyState s = make_state();
    fly_adjust_speed(s, 1);
    CHECK_NEAR(s.speed, 125, 1e-3);
    fly_adjust_speed(s, -1000);
    CHECK_NEAR(s.speed, 1, 1e-6);
    fly_adjust_speed(s, 1000);
    CHECK_NEAR(s.speed, 200000, 1e-2);
}

CM_TEST(map_window, walk_delta_is_horizontal) {
    FlyState s = make_state();
    // Looking 45 degrees down: the walk step still has full length on XY.
    const float r = 1.0f / std::sqrt(2.0f);
    s.fwd[0] = r; s.fwd[1] = 0; s.fwd[2] = r;
    s.up[0] = r; s.up[1] = 0; s.up[2] = -r;
    bool keys[8] = {};
    keys[MW_FWD] = true;
    keys[MW_UP] = true;   // ignored in walk
    float hd[2];
    walk_delta(s, keys, 300, 0.1f, hd);
    CHECK_NEAR(hd[0], 30, 1e-3);
    CHECK_NEAR(hd[1], 0, 1e-3);
    bool none[8] = {};
    walk_delta(s, none, 300, 0.1f, hd);
    CHECK_NEAR(hd[0], 0, 1e-6);
    CHECK_NEAR(hd[1], 0, 1e-6);
}

CM_TEST(map_window, input_maps_movement_keys) {
    CHECK_EQ(map_win_key('W'), static_cast<int>(MW_FWD));
    CHECK_EQ(map_win_key('A'), static_cast<int>(MW_LEFT));
    CHECK_EQ(map_win_key('S'), static_cast<int>(MW_BACK));
    CHECK_EQ(map_win_key('D'), static_cast<int>(MW_RIGHT));
    CHECK_EQ(map_win_key('Q'), static_cast<int>(MW_DOWN));
    CHECK_EQ(map_win_key('E'), static_cast<int>(MW_UP));
    CHECK_EQ(map_win_key(VK_SHIFT), static_cast<int>(MW_FAST));
    CHECK_EQ(map_win_key(VK_CONTROL), static_cast<int>(MW_SLOW));
    CHECK_EQ(map_win_key('X'), -1);
}

CM_TEST(map_window, pool_size_is_capped) {
    CHECK_EQ(mapwin_pool_size(0), 1);    // hardware_concurrency() may report 0
    CHECK_EQ(mapwin_pool_size(1), 1);
    CHECK_EQ(mapwin_pool_size(2), 1);
    CHECK_EQ(mapwin_pool_size(9), 8);
    CHECK_EQ(mapwin_pool_size(32), 8);
}

CM_TEST(map_window, wait_workers_with_none_running) {
    CHECK(mapwin_wait_workers(0));
}
