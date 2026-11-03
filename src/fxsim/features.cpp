/// @file
/// @brief Model feature counts (see features.h).

#include "castlemist/fxsim/features.h"

#include "castlemist/fx/fx_math.h"
#include "castlemist/native/gw2model.hpp"

#include <algorithm>

namespace castlemist::fxsim {

ModelFeatures summarize(const castlemist::model::Model& m) {
    auto clamp16 = [](size_t n) { return static_cast<uint16_t>(std::min<size_t>(n, 0xffff)); };
    ModelFeatures f;
    const auto& fx = m.effects;
    f.clouds = clamp16(fx.clouds.size());
    f.emitters = clamp16(fx.emitters.size());
    f.streaks = clamp16(fx.streaks.size());
    f.lightning = clamp16(fx.lightningBolts.size());
    f.lights = clamp16(fx.lights.size());
    size_t mesh = 0, bursts = 0, tracks = 0;
    for (const auto& e : fx.emitters) {
        if (e.hasMesh) ++mesh;
        if (e.emitterFlags & 0x8) ++bursts;
        if (e.hasPlane) {
            const uint8_t c = fx::plane_alignment_case(e.alignmentType);
            if (c < 8) f.alignMask = static_cast<uint8_t>(f.alignMask | (1u << c));
        }
    }
    for (const auto& c : m.anim.clips) tracks += c.effectTracks.size();
    f.meshEmitters = clamp16(mesh);
    f.bursts = clamp16(bursts);
    f.clipTracks = clamp16(tracks);
    return f;
}

}  // namespace castlemist::fxsim
