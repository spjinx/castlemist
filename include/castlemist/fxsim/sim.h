/// @file
/// @brief The reference particle sim over a model's baked effects
///        (docs/superpowers/specs/2026-10-28-reference-sim-design.md).
/// @ingroup fxsim

#pragma once

#include "castlemist/extract/model_types.h"
#include "castlemist/fx/fx_math.h"
#include "castlemist/fxsim/stats.h"

#include <string>
#include <vector>

namespace castlemist::fxsim {

/// @brief One emitter as the sim runs it: the stored emitter, its cloud, where it sits.
struct EmitterIn {
    ParticleEmitterCPU e;
    int cloud = -1;      ///< Index into SimInput::clouds; -1 = in no cloud.
    fx::Vec3 origin;     ///< Bind-pose position: the emitter's bone, else its cloud's, else 0.
};

/// @brief Everything the sim reads, taken from a ModelPreview once.
struct SimInput {
    std::vector<ParticleCloudCPU> clouds;
    std::vector<EmitterIn> emitters;     ///< In cloud order, each cloud's emitterIndices in turn.
    std::vector<std::string> warnings;   ///< Indices that pointed nowhere, and the like.
};

/// @brief The sim input of @p model. A cloud's emitter index past the emitter list is
///        skipped with a warning; a bone index past the joint list means the model origin.
SimInput make_input(const ModelPreview& model);

/// @brief Live particles per sim; spawns past it are counted in Sim::dropped().
constexpr size_t kMaxParticles = 65536;

/// @brief One live particle (GW2 model space: Z up, inches).
struct Particle {
    uint16_t emitter = 0;   ///< Index into SimInput::emitters.
    fx::Vec3 pos, vel;
    float age = 0, life = 1;
    // drawn at spawn
    float begin[4] = {1, 1, 1, 1}, end[4] = {1, 1, 1, 1};
    float size0[2] = {1, 1}, growth[2] = {0, 0};
    float rotation0 = 0, spin = 0;          ///< Radians, radians per second.
    // after each step
    float colour[4] = {1, 1, 1, 1};         ///< RGBA, alpha with the opacity curve applied.
    float size[2] = {1, 1};                 ///< Half-sizes (GW2 sizes are half-sizes).
    float rotation = 0;
    int frame = 0;                          ///< Flipbook frame (0 without a flipbook).
    float uv[4] = {0, 0, 1, 1};             ///< u0, v0, u1, v1 inside texCoordRect.
};

/// @brief The sim's state after a step.
struct Frame {
    float time = 0;
    std::vector<Particle> particles;   ///< Oldest first.
};

/// @brief The reference sim. Deterministic: the same input, seed and steps give the same frames.
class Sim {
public:
    Sim(SimInput in, uint32_t seed);
    /// @brief Advances @p dt seconds; dt <= 0 does nothing.
    void step(float dt);
    const Frame& frame() const { return frame_; }
    const SimInput& input() const { return in_; }
    size_t dropped() const { return dropped_; }
    /// @brief The run so far (percentiles and means computed on each call).
    const Stats& stats() const;
    /// @brief The pieces of client behaviour not yet read, run as approximations.
    std::vector<std::string> approximations() const;

private:
    void spawn(size_t ei);
    void appearance(Particle& p) const;   ///< colour, size, rotation, frame from age
    SimInput in_;
    std::vector<fx::Rng> rng_;
    std::vector<float> acc_;
    Frame frame_;
    size_t dropped_ = 0;
    // stats: counters kept per step, finished in stats()
    struct Acc {
        std::vector<float> dist;            ///< Capped sample of distances from the emitter.
        double speedSum = 0; size_t speedN = 0;
        double alpha[5] = {}, size[5] = {}; size_t n[5] = {};
    };
    std::vector<Acc> acc2_;
    mutable Stats stats_;
};

}  // namespace castlemist::fxsim
