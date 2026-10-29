/// @file
/// @brief Tests for the fx layer: the client's per-particle maths, case by case.
///
/// Expected values are worked by hand from the decompiled client functions each
/// fx function names (docs/research/gw2-particle-system.md).

#include "test_framework.h"

#include "castlemist/fx/fx_math.h"

#include <cmath>
#include <utility>
#include <vector>

using namespace castlemist::fx;

// The file stores {min, max}; the loader keeps base = min, span = max - min (FUN_140ac8c40).
CM_TEST(fx, range_from_min_max) {
    const Range r = Range::from_min_max(3, 1);
    CHECK_NEAR(r.base, 3.0f, 1e-6f);
    CHECK_NEAR(r.span, -2.0f, 1e-6f);
    CHECK_NEAR(r.at(0.5f), 2.0f, 1e-6f);
}

// FUN_140ad3a30: period 1 with falloff (0, 1) is one sweep over the life.
CM_TEST(fx, colour_weight_single_sweep) {
    const Range falloff = Range::from_min_max(0, 1);
    CHECK_NEAR(colour_weight(0.25f, 1.0f, falloff), 0.25f, 1e-6f);
    CHECK_NEAR(colour_weight(1.0f, 1.0f, falloff), 1.0f, 1e-6f);   // m 0, n 1 (odd): x = 1
}

CM_TEST(fx, colour_weight_ping_pong) {
    // period 0.5, life 0.75: m 0.25, n 1 (odd) -> x = 1 - 0.5 = 0.5
    CHECK_NEAR(colour_weight(0.75f, 0.5f, Range::from_min_max(0, 1)), 0.5f, 1e-6f);
    // life 0.6: m 0.1, n 1 -> x = 0.8; falloff (0.2, 0.6) clamps it to 1
    CHECK_NEAR(colour_weight(0.6f, 0.5f, Range::from_min_max(0.2f, 0.6f)), 1.0f, 1e-6f);
}

CM_TEST(fx, colour_weight_degenerate) {
    CHECK_NEAR(colour_weight(0.7f, 0.0f, Range::from_min_max(0, 1)), 0.0f, 1e-6f);  // period 0: x = 0
    const Range step = Range::from_min_max(0.5f, 0.5f);
    CHECK_NEAR(colour_weight(0.6f, 1.0f, step), 1.0f, 1e-6f);
    CHECK_NEAR(colour_weight(0.4f, 1.0f, step), 0.0f, 1e-6f);
    CHECK(!std::isnan(colour_weight(0.5f, 1.0f, step)));
}

CM_TEST(fx, spawn_velocity_normalises_direction) {
    const Range dir[3] = {Range::from_min_max(2, 2), Range{}, Range{}};
    const float u[4] = {0.3f, 0.3f, 0.3f, 0.3f};
    const Vec3 v = spawn_velocity(dir, Range::from_min_max(5, 5), u);
    CHECK_NEAR(v.x, 5.0f, 1e-5f);
    CHECK_NEAR(v.y, 0.0f, 1e-6f);
    CHECK_NEAR(v.z, 0.0f, 1e-6f);
    const Range none[3] = {};
    const Vec3 z = spawn_velocity(none, Range::from_min_max(5, 5), u);
    CHECK_NEAR(z.x, 0.0f, 1e-6f);
    CHECK_NEAR(z.y, 0.0f, 1e-6f);
    CHECK_NEAR(z.z, 0.0f, 1e-6f);
}

// Baked curves are clamped to 0..1 (FUN_140acbe30).
CM_TEST(fx, curve_eval_clamps_and_interpolates) {
    const std::vector<std::pair<float, float>> keys = {{0.0f, 0.0f}, {1.0f, 2.0f}};
    CHECK_NEAR(curve_eval(keys, 0.25f, 0.0f), 0.5f, 1e-6f);
    CHECK_NEAR(curve_eval(keys, 0.75f, 0.0f), 1.0f, 1e-6f);    // 1.5 clamped
    CHECK_NEAR(curve_eval({}, 0.5f, 0.7f), 0.7f, 1e-6f);
}

// The 18 curve presets (table 0x14257d870, functions 0x1409c4930..0x1409c4d50), by hand.
CM_TEST(fx, curve_preset_simple_shapes) {
    CHECK_NEAR(curve_preset(0, 0.3f), 1.0f, 1e-6f);            // constant
    CHECK_NEAR(curve_preset(1, 0.3f), 0.3f, 1e-6f);            // identity (a bare ret)
    CHECK_NEAR(curve_preset(9, 0.5f), 0.25f, 1e-6f);           // t^2
    CHECK_NEAR(curve_preset(10, 0.5f), 0.0625f, 1e-6f);        // t^4
    CHECK_NEAR(curve_preset(17, 0.25f), 0.5f, 1e-6f);          // 1 - |2t - 1|
    CHECK_NEAR(curve_preset(15, 0.25f), 0.25f, 1e-6f);         // (1 - |2t - 1|)^2
    CHECK_NEAR(curve_preset(16, 0.25f), 0.0625f, 1e-6f);       // (1 - |2t - 1|)^4
}

CM_TEST(fx, curve_preset_polynomials) {
    const float t = 0.5f;
    // 2: -1.0347t + 21.174t^2 - 40.278t^3 + 20.139t^4 (stored float constants)
    const float a = -1.0347f * t + 21.174f * t * t - 40.278f * t * t * t + 20.139f * t * t * t * t;
    CHECK_NEAR(curve_preset(2, t), a, 2e-3f);
    CHECK_NEAR(curve_preset(3, t), a * 0.75f + 0.25f, 2e-3f);
    CHECK_NEAR(curve_preset(4, t), a * 0.5f + 0.5f, 2e-3f);
    CHECK_NEAR(curve_preset(5, t), a * 0.25f + 0.75f, 2e-3f);
    CHECK_NEAR(curve_preset(6, t), 8.0606f * t - 20.059f * t * t + 16.354f * t * t * t - 4.3561f * t * t * t * t, 2e-3f);
    CHECK_NEAR(curve_preset(7, t), 9.3333f * t - 30.667f * t * t + 42.667f * t * t * t - 21.333f * t * t * t * t, 2e-3f);
    CHECK_NEAR(curve_preset(8, t), 6.3333f * t - 11.4f * t * t + 5.0667f * t * t * t, 2e-3f);
    const float e = -0.6f * t + 4.8f * t * t - 3.2f * t * t * t;
    CHECK_NEAR(curve_preset(11, t), e, 2e-3f);
    CHECK_NEAR(curve_preset(14, t), e * 0.25f + 0.75f, 2e-3f);
    // the bump curves start and end at 0, the ease curve 11 ends at 1
    CHECK_NEAR(curve_preset(2, 1.0f), 0.0f, 2e-3f);
    CHECK_NEAR(curve_preset(7, 1.0f), 0.0f, 2e-3f);
    CHECK_NEAR(curve_preset(11, 1.0f), 1.0f, 2e-3f);
    CHECK_NEAR(curve_preset(99, 0.5f), 1.0f, 1e-6f);           // outside the table: constant 1
}

// FUN_140d52160, bit by bit; the three bits the 2026-10-22 note proved, and the curve bits.
CM_TEST(fx, runtime_emitter_flags_permutation) {
    CHECK_EQ(runtime_emitter_flags(0x4), 0x10u);
    CHECK_EQ(runtime_emitter_flags(0x10), 0x800u);
    CHECK_EQ(runtime_emitter_flags(0x1000), 0x20u);
    CHECK_EQ(runtime_emitter_flags(0x80), 0x80u);
    CHECK_EQ(runtime_emitter_flags(0x400), 0x200u);
    CHECK_EQ(runtime_emitter_flags(0x200), 0x400u);
    CHECK_EQ(runtime_emitter_flags(0x80000), 0x1u);
    CHECK_EQ(runtime_emitter_flags(0x200000), 0x80000u);
}

// Scale preset: file flag 0x400 runs it backwards, 0x200 takes 1 - value; opacity: 0x80 takes 1 - value.
CM_TEST(fx, preset_flags) {
    CHECK_NEAR(scale_preset_value(9, 0.25f, 0), 0.0625f, 1e-6f);
    CHECK_NEAR(scale_preset_value(9, 0.25f, 0x400), 0.5625f, 1e-6f);
    CHECK_NEAR(scale_preset_value(9, 0.25f, 0x200), 0.9375f, 1e-6f);
    CHECK_NEAR(opacity_preset_value(9, 0.25f, 0x80), 0.9375f, 1e-6f);
    CHECK_NEAR(opacity_preset_value(9, 0.25f, 0x400), 0.0625f, 1e-6f);   // reverse is the scale's bit only
}

// FUN_140d52110's table 0x141ee8df0: file alignmentType -> the builder's case.
CM_TEST(fx, plane_alignment_remap) {
    const uint8_t expect[6] = {1, 5, 4, 2, 0, 3};
    for (uint8_t f = 0; f < 6; ++f) CHECK_EQ(plane_alignment_case(f), expect[f]);
    CHECK_EQ(plane_alignment_case(6), uint8_t(255));
}

// Case 1 (file 0): the quad's axes in the camera plane, rotated by r (cos 0x140e56c60, sin 0x140e57160).
CM_TEST(fx, camera_sprite_axes_rotate_in_the_view_plane) {
    const SpriteAxes a = camera_sprite_axes(0.0f, 2.0f, 3.0f);
    CHECK_NEAR(a.u.x, 2.0f, 1e-6f); CHECK_NEAR(a.u.y, 0.0f, 1e-6f);
    CHECK_NEAR(a.v.x, 0.0f, 1e-6f); CHECK_NEAR(a.v.y, 3.0f, 1e-6f);
    const SpriteAxes b = camera_sprite_axes(1.5707964f, 2.0f, 3.0f);
    CHECK_NEAR(b.u.x, 0.0f, 1e-5f); CHECK_NEAR(b.u.y, 2.0f, 1e-5f);
    CHECK_NEAR(b.v.x, -3.0f, 1e-5f); CHECK_NEAR(b.v.y, 0.0f, 1e-5f);
    const SpriteAxes neg = camera_sprite_axes(0.0f, -1.0f, -1.0f);         // sizes clamp at 0
    CHECK_NEAR(neg.u.x, 0.0f, 1e-6f); CHECK_NEAR(neg.v.y, 0.0f, 1e-6f);
}

// FUN_140e1e6a0: MINSTD (48271, Schrage q 44488) in 32-bit arithmetic; u = state * 2^-31.
CM_TEST(fx, rng_matches_the_client_generator) {
    Rng r{1};
    CHECK_NEAR(r.u01(), 48271.0f / 2147483648.0f, 1e-12f);
    CHECK_EQ(r.state, 48271u);
    r.u01();
    CHECK_EQ(r.state, 182605794u);                 // 48271^2 mod (2^31 - 1)
    Rng q{44488};                                  // a*(s mod q) - 3399*(s/q) = -3399 < 0
    q.u01();
    CHECK_EQ(q.state, 2147480249u);                // the client adds 2^31 (Schrage adds 2^31 - 1)
    Rng z{0};
    z.u01();
    CHECK_EQ(z.state, 0x75bd924u);                 // a zero state is replaced
}

// FUN_140e1f110: no draw at all when the span is 0.
CM_TEST(fx, sample_skips_the_draw_for_a_constant) {
    Rng r{7};
    CHECK_NEAR(sample(Range{2.5f, 0.0f}, r), 2.5f, 1e-6f);
    CHECK_EQ(r.state, 7u);
    const float v = sample(Range{1.0f, 2.0f}, r);
    CHECK(r.state != 7u);
    CHECK(v >= 1.0f && v < 3.0f);
}

// FUN_140c08e70: mode 0 linear, mode 1 Catmull-Rom with the end points repeated.
CM_TEST(fx, bolt_path_linear_and_catmull_rom) {
    const std::vector<Vec3> pts = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
    const PathPoint a = bolt_path_point(0, pts, 0.25f);   // half way along the first of two segments
    CHECK_NEAR(a.pos.x, 5.0f, 1e-5f); CHECK_NEAR(a.pos.y, 0.0f, 1e-5f);
    CHECK_NEAR(a.tangent.x, 1.0f, 1e-5f);
    const PathPoint b = bolt_path_point(1, pts, 0.5f);    // a node: the curve passes through it
    CHECK_NEAR(b.pos.x, 10.0f, 1e-4f); CHECK_NEAR(b.pos.y, 0.0f, 1e-4f);
    // segment 0 at f 0.5, P0 = P1: x = 0.5*(5 + 7.5 - 2.5), y = 0.5*(-2.5 + 1.25)
    const PathPoint c = bolt_path_point(1, pts, 0.25f);
    CHECK_NEAR(c.pos.x, 5.0f, 1e-4f);
    CHECK_NEAR(c.pos.y, -0.625f, 1e-4f);
    const PathPoint end = bolt_path_point(1, pts, 1.0f);
    CHECK_NEAR(end.pos.y, 10.0f, 1e-4f);
    CHECK_NEAR(end.tangent.y, 1.0f, 1e-4f);
}

// FUN_140b13cf0, node shape 0: a point on a disc, angle 2*pi*u first, then the radius sample.
CM_TEST(fx, lightning_disc_offset_draw_order) {
    Rng r{12345};
    Rng copy = r;
    const Vec3 p = lightning_disc_offset(Range{3.0f, 0.0f}, r);
    const float ang = 2.0f * 3.1415927f * copy.u01();
    CHECK_NEAR(p.x, std::cos(ang) * 3.0f, 1e-4f);
    CHECK_NEAR(p.y, std::sin(ang) * 3.0f, 1e-4f);
    CHECK_NEAR(p.z, 0.0f, 1e-6f);
    CHECK_EQ(r.state, copy.state);                 // a constant radius drew nothing more
}

// Review fix: on an interior node the path is exactly that node (float floor/fmod disagreed:
// n = 7, t = 0.5 gave node 4), and t outside [0, 1] clamps.
CM_TEST(fx, bolt_path_lands_on_interior_nodes_and_clamps) {
    std::vector<Vec3> pts;
    for (int i = 0; i < 7; ++i) pts.push_back({static_cast<float>(i) * 10.0f, 0, 0});
    for (uint32_t mode = 0; mode < 2; ++mode) {
        CHECK_NEAR(bolt_path_point(mode, pts, 0.5f).pos.x, 30.0f, 1e-3f);
        CHECK_NEAR(bolt_path_point(mode, pts, -0.5f).pos.x, 0.0f, 1e-3f);
        CHECK_NEAR(bolt_path_point(mode, pts, 1.5f).pos.x, 60.0f, 1e-3f);
    }
    std::vector<Vec3> eight;
    for (int i = 0; i < 8; ++i) eight.push_back({static_cast<float>(i), 0, 0});
    CHECK_NEAR(bolt_path_point(0, eight, 3.0f / 7.0f).pos.x, 3.0f, 1e-4f);
}

// Case 5 (file alignment 1), FUN_140ad11b0: u = dir * sx along the motion, v = normalize(dir x toEye) * sy.
CM_TEST(fx, axis_sprite_runs_along_the_motion) {
    const Axes3 a = axis_sprite_axes({2, 0, 0}, {0, 0, 5}, 3.0f, 4.0f);
    CHECK_NEAR(a.u.x, 3.0f, 1e-5f); CHECK_NEAR(a.u.y, 0.0f, 1e-6f); CHECK_NEAR(a.u.z, 0.0f, 1e-6f);
    CHECK_NEAR(a.v.x, 0.0f, 1e-6f); CHECK_NEAR(a.v.y, -4.0f, 1e-5f); CHECK_NEAR(a.v.z, 0.0f, 1e-6f);
    const Axes3 still = axis_sprite_axes({0, 0, 0}, {0, 0, 5}, 3.0f, 4.0f);   // no motion: degenerate
    CHECK_NEAR(still.u.x, 0.0f, 1e-6f); CHECK_NEAR(still.v.y, 0.0f, 1e-6f);
    const Axes3 neg = axis_sprite_axes({1, 0, 0}, {0, 0, 5}, -3.0f, 4.0f);    // sizes clamp at 0
    CHECK_NEAR(neg.u.x, 0.0f, 1e-6f);
}

// Case 4 (file alignment 2): the velocity's on-screen direction (x, y) -> u = (x, y) sx, v = (-y, x) sy;
// no on-screen motion gives (1, 0).
CM_TEST(fx, screen_velocity_sprite_axes) {
    const SpriteAxes a = screen_velocity_axes({0, 2}, 3.0f, 4.0f);
    CHECK_NEAR(a.u.x, 0.0f, 1e-6f); CHECK_NEAR(a.u.y, 3.0f, 1e-5f);
    CHECK_NEAR(a.v.x, -4.0f, 1e-5f); CHECK_NEAR(a.v.y, 0.0f, 1e-6f);
    const SpriteAxes z = screen_velocity_axes({0, 0}, 3.0f, 4.0f);
    CHECK_NEAR(z.u.x, 3.0f, 1e-5f); CHECK_NEAR(z.v.y, 4.0f, 1e-5f);
}

// FUN_140acb1d0: pos += v dt + a dt^2 / 2, v += a dt, then v *= 1 - drag dt with a speed floor.
CM_TEST(fx, integrate_is_the_exact_constant_acceleration_step) {
    Vec3 p{0, 0, 0}, v{0, 0, 10};
    integrate(p, v, {0, 0, -10}, 0.0f, 0.1f);
    CHECK_NEAR(p.z, 0.95f, 1e-5f);    // 10 * 0.1 - 0.5 * 10 * 0.01
    CHECK_NEAR(v.z, 9.0f, 1e-5f);
}

CM_TEST(fx, integrate_drag_and_its_floor) {
    Vec3 p{}, v{10, 0, 0};
    integrate(p, v, {}, 0.5f, 0.1f);
    CHECK_NEAR(v.x, 9.5f, 1e-5f);
    CHECK_NEAR(p.x, 1.0f, 1e-5f);     // position moves with the pre-drag velocity
    Vec3 q{}, w{1, 0, 0};
    integrate(q, w, {}, 10.0f, 0.1f); // 1 - 10 * 0.1 = 0: below the 1e-4 floor
    CHECK_NEAR(w.x, 1e-4f, 1e-9f);
    Vec3 r{}, z{};
    integrate(r, z, {}, 10.0f, 0.1f); // no direction to keep: stays zero
    CHECK_NEAR(z.x, 0.0f, 1e-9f);
}
