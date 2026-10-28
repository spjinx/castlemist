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
