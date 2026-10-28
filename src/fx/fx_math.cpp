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

// The preset table 0x14257d870 (2026-09 build): the functions' float constants as stored.
float curve_preset(uint32_t preset, float t) {
    const float t2 = t * t, t3 = t2 * t, t4 = t3 * t;
    const float bumpA = -1.0347f * t + 21.174f * t2 - 40.278f * t3 + 20.139f * t4;   // 0x1409c4930
    const float ease = -0.6f * t + 4.8f * t2 - 3.2f * t3;                            // 0x1409c4bf0
    const float tent = 1.0f - std::fabs((t - 0.5f) + (t - 0.5f));                     // 0x1409c4d20
    switch (preset) {
        case 0: return 1.0f;
        case 1: return t;
        case 2: return bumpA;
        case 3: return bumpA * 0.75f + 0.25f;
        case 4: return bumpA * 0.5f + 0.5f;
        case 5: return bumpA * 0.25f + 0.75f;
        case 6: return 8.0606f * t - 20.059f * t2 + 16.354f * t3 - 4.3561f * t4;     // 0x1409c4aa0
        case 7: return 9.3333f * t - 30.667f * t2 + 42.667f * t3 - 21.333f * t4;     // 0x1409c4b30
        case 8: return 6.3333f * t - 11.4f * t2 + 5.0667f * t3;                      // 0x1409c4af0
        case 9: return t2;
        case 10: return t4;
        case 11: return ease;
        case 12: return ease * 0.75f + 0.25f;
        case 13: return ease * 0.5f + 0.5f;
        case 14: return ease * 0.25f + 0.75f;
        case 15: return tent * tent;
        case 16: return tent * tent * tent * tent;
        case 17: return tent;
        default: return 1.0f;
    }
}

float scale_preset_value(uint32_t preset, float t, uint32_t fileFlags) {
    const uint32_t rt = runtime_emitter_flags(fileFlags);
    float v = curve_preset(preset, (rt & 0x200) ? 1.0f - t : t);
    if (rt & 0x400) v = 1.0f - v;
    return v;
}

float opacity_preset_value(uint32_t preset, float t, uint32_t fileFlags) {
    const float v = curve_preset(preset, t);
    return (runtime_emitter_flags(fileFlags) & 0x80) ? 1.0f - v : v;
}

// FUN_140d52160: runtime bit k <- file bit kFrom[k]; bits 20 and up are dropped.
uint32_t runtime_emitter_flags(uint32_t fileFlags) {
    static const uint8_t kFrom[20] = {19, 0, 1, 6, 2, 12, 8, 7, 11, 10, 9, 4, 18, 16, 17, 13, 5, 3, 20, 21};
    uint32_t r = 0;
    for (int k = 0; k < 20; ++k)
        if (fileFlags & (1u << kFrom[k])) r |= 1u << k;
    return r;
}

uint8_t plane_alignment_case(uint8_t fileAlignmentType) {
    static const uint8_t kCase[6] = {1, 5, 4, 2, 0, 3};
    return fileAlignmentType < 6 ? kCase[fileAlignmentType] : uint8_t(255);
}

SpriteAxes camera_sprite_axes(float rotation, float sizeX, float sizeY) {
    const float sx = std::max(sizeX, 0.0f), sy = std::max(sizeY, 0.0f);
    const float c = std::cos(rotation), s = std::sin(rotation);
    return {{c * sx, s * sx}, {-s * sy, c * sy}};
}

}  // namespace castlemist::fx
