/// @file
/// @brief What effect features a parsed model carries: the Particle Browser's Models tab and
///        `gw2dat_cli fx-census` count the same way.
/// @ingroup fxsim

#pragma once

#include <cstdint>

namespace castlemist::model { struct Model; }

namespace castlemist::fxsim {

/// @brief Per-model counts. alignMask bit k: some plane emitter takes sprite case k
///        (fx::plane_alignment_case of its file alignmentType).
struct ModelFeatures {
    uint16_t clouds = 0, emitters = 0, streaks = 0, lightning = 0, lights = 0, meshEmitters = 0, bursts = 0,
             clipTracks = 0;
    uint8_t alignMask = 0;
    bool any() const { return clouds || streaks || lightning || lights; }
};

/// @brief The counts of @p m (effects and clips; lightning counts bolts).
ModelFeatures summarize(const castlemist::model::Model& m);

}  // namespace castlemist::fxsim
