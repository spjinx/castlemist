/// @file
/// @brief Tests for the fxsim layer: the reference particle sim over a ModelPreview.

#include "test_framework.h"

#include "castlemist/fxsim/sim.h"

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
