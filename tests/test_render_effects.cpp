/// @file
/// @brief Tests for the preview's particle hooks: a new capture is a new generation, a restart is not.

#include "test_framework.h"

#include "castlemist/render/model_renderer.h"
#include "detail/state.h"

using namespace castlemist::render;

namespace {

ModelPreview one_emitter() {
    ModelPreview m;
    ParticleEmitterCPU e;
    e.spawnPeriod = 0.1f;
    e.spawnProbability = 100;
    m.emitters = {e};
    ParticleCloudCPU c;
    c.emitterIndices = {0};
    m.clouds = {c};
    return m;
}

}  // namespace

// Review fix: the browser keyed its inspector on the sim's address, which the heap can hand to the
// next model's sim; it keys on this counter instead.
CM_TEST(render, effect_generation_counts_captures_not_restarts) {
    const ModelPreview m = one_emitter();
    capture_effects(m);
    const uint64_t g1 = effect_generation();
    capture_effects(m);   // the same-shaped model again: still a new capture
    const uint64_t g2 = effect_generation();
    CHECK(g2 > g1);
    restart_effects(7);
    set_effect_time(1.0f);
    CHECK_EQ(effect_generation(), g2);
    clear_effects();
}
