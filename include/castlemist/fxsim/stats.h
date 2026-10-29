/// @file
/// @brief What a sim run did, in numbers: the oracle the Unity translation is measured against.
/// @ingroup fxsim

#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace castlemist::fxsim {

/// @brief One emitter over a run.
struct EmitterStats {
    size_t spawned = 0, expired = 0;
    std::vector<uint32_t> live;        ///< Live particles after each step.
    float spread[3] = {0, 0, 0};       ///< Distance from the emitter: 5th, 50th, 95th percentile.
    float meanSpeed = 0;               ///< Over every live particle of every step.
    float alphaAt[5] = {0, 0, 0, 0, 0};   ///< Mean alpha at life fractions 0, .25, .5, .75, 1 (+-0.125).
    float sizeAt[5] = {0, 0, 0, 0, 0};    ///< Mean half-size X at the same fractions.
};

/// @brief A whole run.
struct Stats {
    std::vector<EmitterStats> emitters;
    std::vector<uint32_t> live;        ///< Total live particles after each step.
    size_t dropped = 0;                ///< Spawns past kMaxParticles.
};

/// @brief {"live": [...], "dropped": n, "emitters": [{spawned, expired, live, spread, meanSpeed,
///        alphaAt, sizeAt}]}.
nlohmann::json stats_json(const Stats& s);

}  // namespace castlemist::fxsim
