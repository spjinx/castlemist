/// @file
/// @brief Tests for the fxsim layer: the reference particle sim over a ModelPreview.

#include "test_framework.h"

#include "castlemist/fxsim/sim.h"
#include "castlemist/fxsim/stats.h"

#include <nlohmann/json.hpp>

using namespace castlemist;

namespace {

ModelPreview two_joint_model() {
    ModelPreview m;
    ModelJoint root;
    root.name = "root";
    ModelJoint hand;
    hand.name = "hand";
    hand.parent = 0;
    hand.pos[0] = 1; hand.pos[1] = 2; hand.pos[2] = 3;
    ModelJoint cloudBone;
    cloudBone.name = "fx";
    cloudBone.pos[2] = 9;
    m.joints = {root, hand, cloudBone};
    return m;
}

}  // namespace

// An emitter sits on its own bone, else on its cloud's bone.
CM_TEST(fxsim, make_input_attaches_by_bone) {
    ModelPreview m = two_joint_model();
    ParticleEmitterCPU own, inherits;
    own.boneJoint = 1;
    m.emitters = {own, inherits};
    ParticleCloudCPU c;
    c.boneJoint = 2;
    c.emitterIndices = {0, 1};
    m.clouds = {c};
    fxsim::SimInput in = fxsim::make_input(m);
    CHECK_EQ(in.emitters.size(), size_t{2});
    CHECK_EQ(in.emitters[0].cloud, 0);
    CHECK_NEAR(in.emitters[0].origin.x, 1.0f, 1e-6f);
    CHECK_NEAR(in.emitters[0].origin.z, 3.0f, 1e-6f);
    CHECK_NEAR(in.emitters[1].origin.z, 9.0f, 1e-6f);
    CHECK(in.warnings.empty());
}

CM_TEST(fxsim, make_input_skips_bad_emitter_index) {
    ModelPreview m = two_joint_model();
    m.emitters = {ParticleEmitterCPU{}};
    ParticleCloudCPU c;
    c.emitterIndices = {0, 7};
    m.clouds = {c};
    fxsim::SimInput in = fxsim::make_input(m);
    CHECK_EQ(in.emitters.size(), size_t{1});
    CHECK_EQ(in.warnings.size(), size_t{1});
}

CM_TEST(fxsim, make_input_without_skeleton_uses_origin) {
    ModelPreview m;
    ParticleEmitterCPU e;
    e.boneJoint = 4;              // no joints at all
    m.emitters = {e};
    ParticleCloudCPU c;
    c.emitterIndices = {0};
    m.clouds = {c};
    fxsim::SimInput in = fxsim::make_input(m);
    CHECK_EQ(in.emitters.size(), size_t{1});
    CHECK_NEAR(in.emitters[0].origin.x, 0.0f, 1e-6f);
    CHECK_NEAR(in.emitters[0].origin.z, 0.0f, 1e-6f);
}

namespace {

// One emitter in one cloud: a group of 1 every `period` s, 100 % chance, living `life` s.
fxsim::SimInput steady(float period, float life, float probability = 100.0f) {
    fxsim::SimInput in;
    in.clouds.resize(1);
    fxsim::EmitterIn e;
    e.cloud = 0;
    e.e.spawnPeriod = period;
    e.e.spawnProbability = probability;
    e.e.spawnGroupSize[0] = 1; e.e.spawnGroupSize[1] = 1;
    e.e.lifetime[0] = life; e.e.lifetime[1] = life;
    in.emitters.push_back(e);
    return in;
}

void run(fxsim::Sim& s, float seconds, float dt) {
    for (float t = 0; t < seconds - 1e-6f; t += dt) s.step(dt);
}

}  // namespace

// spawnPeriod 0.1 s, life 1 s: about 10 alive once the first ones start dying.
CM_TEST(fxsim, constant_rate_reaches_steady_count) {
    fxsim::Sim s(steady(0.1f, 1.0f), 1);
    run(s, 2.0f, 1.0f / 60.0f);
    const size_t n = s.frame().particles.size();
    CHECK(n >= 9 && n <= 11);
    CHECK(s.frame().time > 1.99f);
}

CM_TEST(fxsim, zero_period_or_chance_spawns_nothing) {
    fxsim::Sim a(steady(0.0f, 1.0f), 1);
    run(a, 1.0f, 0.1f);
    CHECK(a.frame().particles.empty());
    fxsim::Sim b(steady(0.1f, 1.0f, 0.0f), 1);
    run(b, 1.0f, 0.1f);
    CHECK(b.frame().particles.empty());
}

CM_TEST(fxsim, same_seed_same_frames) {
    fxsim::SimInput in = steady(0.05f, 2.0f);
    in.emitters[0].e.spawnRadius[0] = 1; in.emitters[0].e.spawnRadius[1] = 5;   // random positions
    in.emitters[0].e.spawnShape = 4;
    fxsim::Sim a(in, 7), b(in, 7), c(in, 8);
    run(a, 1.0f, 1.0f / 30.0f); run(b, 1.0f, 1.0f / 30.0f); run(c, 1.0f, 1.0f / 30.0f);
    CHECK_EQ(a.frame().particles.size(), b.frame().particles.size());
    bool same = true, differs = false;
    for (size_t i = 0; i < a.frame().particles.size() && i < c.frame().particles.size(); ++i) {
        same = same && a.frame().particles[i].pos.x == b.frame().particles[i].pos.x;
        differs = differs || a.frame().particles[i].pos.x != c.frame().particles[i].pos.x;
    }
    CHECK(same);
    CHECK(differs);
}

CM_TEST(fxsim, dt_zero_changes_nothing) {
    fxsim::Sim s(steady(0.1f, 1.0f), 1);
    run(s, 0.5f, 0.1f);
    const size_t n = s.frame().particles.size();
    const float t = s.frame().time;
    s.step(0.0f);
    s.step(-1.0f);
    CHECK_EQ(s.frame().particles.size(), n);
    CHECK_NEAR(s.frame().time, t, 1e-9f);
}

CM_TEST(fxsim, live_cap_counts_dropped) {
    fxsim::SimInput in = steady(0.001f, 100.0f);
    in.emitters[0].e.spawnGroupSize[0] = 1000; in.emitters[0].e.spawnGroupSize[1] = 1000;
    fxsim::Sim s(in, 1);
    s.step(0.1f);   // 99 or 100 groups of 1000 (0.1 / 0.001 accumulates in float)
    CHECK_EQ(s.frame().particles.size(), fxsim::kMaxParticles);
    CHECK(s.dropped() == size_t{99000} - fxsim::kMaxParticles || s.dropped() == size_t{100000} - fxsim::kMaxParticles);
}

// Velocity from the emitter's direction and speed, plus the cloud's gravity, integrated.
CM_TEST(fxsim, particles_move_with_velocity_and_gravity) {
    fxsim::SimInput in = steady(10.0f, 5.0f);   // one particle at t = 0... after the first period
    in.emitters[0].e.spawnPeriod = 0.01f;
    in.emitters[0].e.velocity[2][0] = 1; in.emitters[0].e.velocity[2][1] = 1;   // +Z
    in.emitters[0].e.velocity[3][0] = 10; in.emitters[0].e.velocity[3][1] = 10; // 10 units/s
    in.clouds[0].acceleration[2] = -10.0f;
    fxsim::Sim s(in, 1);
    s.step(0.01f);                               // spawns one
    CHECK_EQ(s.frame().particles.size(), size_t{1});
    for (int i = 0; i < 100; ++i) s.step(0.01f);
    const fxsim::Particle& p = s.frame().particles.front();
    CHECK(p.vel.z < 1.0f && p.vel.z > -1.0f);   // 10 - 10 * ~1 s
    CHECK(p.pos.z > 4.0f && p.pos.z < 6.0f);    // 10t - 5t^2 at t ~ 1
}

// Colour runs begin -> end by fx::colour_weight over the life fraction (period 1, falloff 0..1).
CM_TEST(fxsim, colour_follows_the_life_sweep) {
    fxsim::SimInput in = steady(10.0f, 1.0f);
    in.emitters[0].e.spawnPeriod = 0.5f;
    for (int r = 0; r < 2; ++r) {
        in.emitters[0].e.colorBegin[r][0] = 1; in.emitters[0].e.colorBegin[r][3] = 1;   // red
        in.emitters[0].e.colorEnd[r][2] = 1;   in.emitters[0].e.colorEnd[r][3] = 1;     // blue
    }
    in.emitters[0].e.colorPeriod = 1.0f;
    in.emitters[0].e.colorFalloff[0] = 0; in.emitters[0].e.colorFalloff[1] = 1;
    fxsim::Sim s(in, 1);
    s.step(0.5f);                       // spawn at 0.5
    for (int i = 0; i < 25; ++i) s.step(0.01f);
    const fxsim::Particle& p = s.frame().particles.front();
    CHECK_NEAR(p.colour[0], 0.75f, 0.02f);   // a quarter of the way through its 1 s life
    CHECK_NEAR(p.colour[2], 0.25f, 0.02f);
    CHECK_NEAR(p.colour[3], 1.0f, 1e-5f);
}

// Size = (scaleInitial + scaleChange * age) * scale preset (9: t^2); Y follows X under file flag 0x10.
CM_TEST(fxsim, size_follows_scale_preset) {
    fxsim::SimInput in = steady(10.0f, 1.0f);
    in.emitters[0].e.spawnPeriod = 0.5f;
    in.emitters[0].e.scaleInitial[0][0] = 4; in.emitters[0].e.scaleInitial[0][1] = 4;
    in.emitters[0].e.scaleInitial[1][0] = 9; in.emitters[0].e.scaleInitial[1][1] = 9;
    in.emitters[0].e.scaleCurvePreset = 9;
    in.emitters[0].e.flags = 0x10;
    fxsim::Sim s(in, 1);
    s.step(0.5f);
    for (int i = 0; i < 50; ++i) s.step(0.01f);
    const fxsim::Particle& p = s.frame().particles.front();
    CHECK_NEAR(p.size[0], 4.0f * 0.25f, 0.05f);   // t = 0.5 -> 0.25
    CHECK_NEAR(p.size[1], p.size[0], 1e-6f);
}

// Flipbook at fps > 0: frame = start + floor(age * fps) mod count; uv inside texCoordRect.
CM_TEST(fxsim, flipbook_frame_at_fps) {
    fxsim::SimInput in = steady(10.0f, 5.0f);
    in.emitters[0].e.spawnPeriod = 0.5f;
    auto& fb = in.emitters[0].e.flipbook;
    fb.present = true; fb.columns = 2; fb.rows = 2; fb.count = 4; fb.start = 0; fb.fps = 10;
    in.emitters[0].e.texCoordRect[0] = 0.5f; in.emitters[0].e.texCoordRect[1] = 0;
    in.emitters[0].e.texCoordRect[2] = 1;    in.emitters[0].e.texCoordRect[3] = 0.5f;
    fxsim::Sim s(in, 1);
    s.step(0.5f);
    for (int i = 0; i < 25; ++i) s.step(0.01f);   // age 0.25 -> frame 2 (column 0, row 1)
    const fxsim::Particle& p = s.frame().particles.front();
    CHECK_EQ(p.frame, 2);
    CHECK_NEAR(p.uv[0], 0.5f, 1e-5f);
    CHECK_NEAR(p.uv[1], 0.25f, 1e-5f);
    CHECK_NEAR(p.uv[2], 0.75f, 1e-5f);
    CHECK_NEAR(p.uv[3], 0.5f, 1e-5f);
}

// Stats count spawns and expiries per emitter and sample the live count each step.
CM_TEST(fxsim, stats_count_spawn_and_expiry) {
    fxsim::Sim s(steady(0.1f, 1.0f), 1);
    run(s, 2.0f, 0.1f);
    const fxsim::Stats& st = s.stats();
    CHECK_EQ(st.emitters.size(), size_t{1});
    CHECK(st.emitters[0].spawned >= 19 && st.emitters[0].spawned <= 21);
    CHECK(st.emitters[0].expired >= 9 && st.emitters[0].expired <= 11);
    CHECK_EQ(st.live.size(), size_t{20});
    CHECK_EQ(st.live.back(), static_cast<uint32_t>(s.frame().particles.size()));
}

// Spread: distances from the emitter at the 5th / 50th / 95th percentile, over all steps.
CM_TEST(fxsim, stats_spread_on_a_disc_shell) {
    fxsim::SimInput in = steady(0.01f, 0.5f);
    in.emitters[0].e.spawnShape = 0;
    in.emitters[0].e.spawnRadius[0] = 3; in.emitters[0].e.spawnRadius[1] = 3;   // a ring of radius 3
    fxsim::Sim s(in, 1);
    run(s, 1.0f, 0.05f);
    const fxsim::EmitterStats& e = s.stats().emitters[0];
    CHECK_NEAR(e.spread[0], 3.0f, 1e-3f);
    CHECK_NEAR(e.spread[2], 3.0f, 1e-3f);
    const nlohmann::json j = fxsim::stats_json(s.stats());
    CHECK_NEAR(j["emitters"][0]["spread"][1].get<double>(), 3.0, 1e-3);
    CHECK(j.contains("live"));
}
