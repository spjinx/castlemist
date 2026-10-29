/// @file
/// @brief The reference particle sim over a model's baked effects
///        (docs/superpowers/specs/2026-10-28-reference-sim-design.md).
/// @ingroup fxsim

#pragma once

#include "castlemist/extract/model_types.h"
#include "castlemist/fx/fx_math.h"

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

}  // namespace castlemist::fxsim
