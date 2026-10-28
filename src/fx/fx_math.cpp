/// @file
/// @brief The GW2 client's per-particle maths (see fx_math.h).

#include "castlemist/fx/fx_math.h"

#include <algorithm>
#include <cmath>

namespace castlemist::fx {

Range Range::from_min_max(float lo, float hi) { return {lo, hi - lo}; }

float Range::at(float u) const { return base + u * span; }

// FUN_140ad3a30 (colour), its clamp FUN_140acf2e0.
float colour_weight(float life, float period, Range falloff) {
    float x = 0;
    if (period > 0) {
        const float n = std::floor(life / period);
        const float m = life - n * period;
        x = (static_cast<long long>(n) % 2 == 0) ? m / period : 1.0f - m / period;
    }
    // The client divides by (hi - lo); with hi == lo that is +-inf, clamped: a step at lo.
    if (falloff.span == 0) return x >= falloff.base ? 1.0f : 0.0f;
    return std::clamp((x - falloff.base) / falloff.span, 0.0f, 1.0f);
}

// FUN_140ace900: normalize(direction) * magnitude; no direction at all -> no velocity.
Vec3 spawn_velocity(const Range dir[3], Range magnitude, const float u[4]) {
    if (dir[0].base == 0 && dir[0].span == 0 && dir[1].base == 0 && dir[1].span == 0 &&
        dir[2].base == 0 && dir[2].span == 0)
        return {};
    const Vec3 d{dir[0].at(u[0]), dir[1].at(u[1]), dir[2].at(u[2])};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len <= 0) return {};
    const float s = magnitude.at(u[3]) / len;
    return {d.x * s, d.y * s, d.z * s};
}

// The baked 0..1 curve of FUN_140acbe30.
float curve_eval(const std::vector<std::pair<float, float>>& keys, float t, float dflt) {
    if (keys.empty()) return dflt;
    float v = keys.back().second;
    if (t <= keys.front().first) {
        v = keys.front().second;
    } else {
        for (size_t i = 1; i < keys.size(); ++i) {
            if (t > keys[i].first) continue;
            const auto& a = keys[i - 1];
            const auto& b = keys[i];
            const float f = b.first > a.first ? (t - a.first) / (b.first - a.first) : 1.0f;
            v = a.second + f * (b.second - a.second);
            break;
        }
    }
    return std::clamp(v, 0.0f, 1.0f);
}

}  // namespace castlemist::fx
