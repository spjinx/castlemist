/// @file
/// @brief The GW2 client's per-particle maths, as pure functions.
/// @ingroup fx

#pragma once

#include <utility>
#include <vector>

/// @defgroup fx fx -- the client's effect maths
/// @brief Transcriptions of Gw2-64.exe's particle, streak and lightning routines.
///
/// Every function names the client function it transcribes (2026-10 build;
/// docs/research/gw2-particle-system.md). No globals, no renderer: B's sim, the
/// browser and the Unity translation all run the same code.
/// @{

namespace castlemist::fx {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

/// @brief A runtime range: `base + u * span`, u in [0, 1).
///
/// The file stores `{min, max}`; the loader keeps `base = min, span = max - min`
/// (GrCloud.cpp, FUN_140ac8c40), so a reversed pair is a negative span.
struct Range {
    float base = 0, span = 0;
    static Range from_min_max(float lo, float hi);
    float at(float u) const;
};

/// @brief The begin -> end colour weight at life fraction @p life (FUN_140ad3a30).
///
/// `m = life mod period`, `n = floor(life / period)`; `x = m / period` on even n,
/// else `1 - m / period` (a ping-pong); `x = 0` when the period is 0. Then
/// `w = clamp((x - lo) / (hi - lo))` with @p falloff as `{lo, hi}` (base = lo,
/// base + span = hi); `hi == lo` is a step at lo.
float colour_weight(float life, float period, Range falloff);

/// @brief Spawn velocity: normalize(direction sample) * magnitude sample.
///
/// @p u holds the four uniforms for dirX, dirY, dirZ and the magnitude. Every
/// direction range zero means no velocity at all (FUN_140ace900).
Vec3 spawn_velocity(const Range dir[3], Range magnitude, const float u[4]);

/// @brief A stored lifetime curve at @p t: linear between keys, clamped 0..1
///        (the baked curve, FUN_140acbe30). No keys -> @p dflt.
float curve_eval(const std::vector<std::pair<float, float>>& keys, float t, float dflt);

}  // namespace castlemist::fx

/// @}
