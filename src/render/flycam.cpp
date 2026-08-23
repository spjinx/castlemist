/// @file
/// @brief The fly view's free camera: WASD/QE movement and unconstrained mouse look.
///
/// Kept apart from the draw path because it is driven by a different clock: the
/// UI pumps `fly_tick()` off a 60 Hz timer while the surface is in fly mode,
/// whereas `render_fly()` only runs when there is a frame to paint.
///
/// The camera carries an explicit orthonormal basis instead of yaw/pitch angles.
/// See the note on `g_fly_fwd` in `detail/map_fly.h` for why: Euler angles need a
/// clamp just short of vertical, and this view is supposed to rotate freely.

#include "detail/map_fly.h"
#include "detail/state.h"

#include <algorithm>
#include <cmath>

namespace castlemist::render {

namespace {

Vec3 vadd(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 vscale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

/// @brief Rotates @p v about the unit axis @p axis by @p ang radians (Rodrigues).
Vec3 rotate_about(Vec3 v, Vec3 axis, float ang) {
    float c = std::cos(ang), s = std::sin(ang);
    Vec3 cr = cross(axis, v);
    float d = dot(axis, v);
    return {v.x * c + cr.x * s + axis.x * d * (1.0f - c),
            v.y * c + cr.y * s + axis.y * d * (1.0f - c),
            v.z * c + cr.z * s + axis.z * d * (1.0f - c)};
}

/// @brief Re-orthonormalizes the basis so 32-bit drift cannot skew it over time.
void renormalize() {
    g_fly_fwd = norm(g_fly_fwd);
    Vec3 right = cross(g_fly_up, g_fly_fwd);
    float rl = std::sqrt(dot(right, right));
    if (rl < 1e-5f) {
        // up and forward went parallel (only reachable through accumulated error);
        // rebuild up from whichever world axis forward is least aligned with.
        Vec3 a = (std::fabs(g_fly_fwd.z) < 0.9f) ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
        right = norm(cross(a, g_fly_fwd));
    } else {
        right = vscale(right, 1.0f / rl);
    }
    g_fly_up = norm(cross(g_fly_fwd, right));
}

} // namespace

/// @brief Camera right, derived from the basis (never stored, never stale).
Vec3 fly_right() { return norm(cross(g_fly_up, g_fly_fwd)); }

void fly_set_key(int key, bool down) {
    if (key < 0 || key >= 8) return;
    g_fly_keys[key] = down;
}

void fly_clear_keys() {
    for (bool& k : g_fly_keys) k = false;
}

void fly_look(float dxPixels, float dyPixels) {
    constexpr float kSens = 0.0032f; // radians per pixel

    // Yaw about the camera's OWN up, pitch about its OWN right. Rotating about
    // the camera basis rather than the world axes is what removes the pole: at
    // no orientation does either axis become parallel to what it rotates, so
    // there is nothing to clamp and the view never snaps over.
    Vec3 right = fly_right();
    if (dxPixels != 0.0f) {
        float a = -dxPixels * kSens;   // drag right -> turn right
        g_fly_fwd = rotate_about(g_fly_fwd, g_fly_up, a);
    }
    if (dyPixels != 0.0f) {
        float a = dyPixels * kSens;    // drag down -> look down
        g_fly_fwd = rotate_about(g_fly_fwd, right, a);
        g_fly_up = rotate_about(g_fly_up, right, a);
    }
    renormalize();
}

/// @brief Rolls about the view axis (bound to Z/C in the host).
void fly_roll(float radians) {
    g_fly_up = rotate_about(g_fly_up, g_fly_fwd, radians);
    renormalize();
}

/// @brief Re-levels the horizon without moving or re-aiming the camera.
///
/// Free rotation accumulates roll by design; this is the cheap way back without
/// losing the position you flew to.
void fly_level() {
    Vec3 right = cross(g_fly_fwd, Vec3{0, 0, 1});
    if (dot(right, right) < 1e-6f) return;  // looking straight up/down: nothing to level against
    right = norm(right);
    g_fly_up = norm(cross(right, g_fly_fwd));
    renormalize();
}

bool fly_tick(float dt) {
    if (dt <= 0.0f) return false;
    if (dt > 0.1f) dt = 0.1f; // a stall should not teleport the camera

    float ax = 0, ay = 0, az = 0;
    if (g_fly_keys[FLY_FWD]) ax += 1;
    if (g_fly_keys[FLY_BACK]) ax -= 1;
    if (g_fly_keys[FLY_RIGHT]) ay += 1;
    if (g_fly_keys[FLY_LEFT]) ay -= 1;
    if (g_fly_keys[FLY_UP]) az += 1;
    if (g_fly_keys[FLY_DOWN]) az -= 1;
    if (ax == 0 && ay == 0 && az == 0) return false;

    Vec3 right = fly_right();
    float speed = g_fly_speed;
    if (g_fly_keys[FLY_FAST]) speed *= 6.0f;
    if (g_fly_keys[FLY_SLOW]) speed *= 0.15f;
    float d = speed * dt;

    // Forward/strafe follow the camera basis. Q/E stay on WORLD up instead: on a
    // map you want "gain altitude" to mean altitude, not "slide along whatever
    // the camera currently calls up" -- which is useless once you have rolled.
    Vec3 move = vadd(vscale(g_fly_fwd, ax), vscale(right, ay));
    g_fly_pos = vadd(g_fly_pos, vscale(move, d));
    g_fly_pos.z += az * d;
    return true;
}

void fly_adjust_speed(float notches) {
    g_fly_speed = std::clamp(g_fly_speed * std::pow(1.25f, notches), 1.0f, 200000.0f);
}

float fly_speed() { return g_fly_speed; }
void fly_get_pos(float out[3]) { out[0] = g_fly_pos.x; out[1] = g_fly_pos.y; out[2] = g_fly_pos.z; }

void fly_stats(int& visible, int& draws, int& ktris, float& ms) {
    visible = g_fly_stat_visible;
    draws = g_fly_stat_draws;
    ktris = g_fly_stat_tris;
    ms = g_fly_stat_ms;
}

void set_fly_view_distance(float d) { g_fly_view_dist = std::clamp(d, 100.0f, 500000.0f); }
float fly_view_distance() { return g_fly_view_dist; }
void set_fly_ao_strength(float v) { g_fly_ao_strength = std::clamp(v, 0.0f, 1.0f); }
float fly_ao_strength() { return g_fly_ao_strength; }
void set_fly_fog(bool on) { g_fly_fog = on; }
bool fly_fog() { return g_fly_fog; }
void set_fly_wireframe(bool on) { g_fly_wire = on; }
bool fly_wireframe() { return g_fly_wire; }

void fly_reset_view() { fly_frame_scene(); }

} // namespace castlemist::render
