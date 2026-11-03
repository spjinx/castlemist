/// @file
/// @brief Tests for resolving a GW2 animation machine to clips (ripper/animator.h),
///        on hand-built machines and clip lists.

#include "test_framework.h"

#include "castlemist/ripper/animator.h"

#include <cmath>
#include <string>
#include <vector>

using namespace castlemist::ripper;
using castlemist::model::AnimMachine;
using castlemist::model::AnimMachineAction;
using castlemist::model::AnimMachineState;
using castlemist::model::AnimMachineTransition;
using castlemist::model::tokenizeBoneName;

namespace {

// Templates and clip names pack the same way: letters 1..26, anything else 0
// (a space in a clip name, '_' here for an empty template slot).
uint64_t tok(const std::string& s) { return tokenizeBoneName(s); }

AnimMachineAction play(const std::string& tmpl, float in, float out = 0, uint32_t flags = 0) {
    AnimMachineAction a;
    a.kind = AnimMachineAction::PlayAnimation;
    a.sequence = tok(tmpl);
    a.blendInTime = in;
    a.blendOutTime = out;
    a.flags = flags;
    return a;
}

AnimMachineAction rotate_z(float degrees) {
    AnimMachineAction a;
    a.kind = AnimMachineAction::SetRotation;
    const float h = degrees * 3.14159265f / 360.0f;
    a.rotation[2] = std::sin(h);
    a.rotation[3] = std::cos(h);
    return a;
}

}  // namespace

CM_TEST(animator, fills_the_stance_slot_only_when_empty) {
    CHECK_EQ(fill_stance(tok("_gmrun____f"), 'u'), tok("ugmrun____f"));
    CHECK_EQ(fill_stance(tok("wgmrun____f"), 'u'), tok("wgmrun____f"));
}

CM_TEST(animator, empty_template_slots_match_any_letter) {
    CHECK(sequence_matches(tok("ugmrun____f"), tok("ugmrun    f ")));
    CHECK(!sequence_matches(tok("ugmrun____f"), tok("ugmrun    b ")));
    CHECK(sequence_matches(tok("_gmrun____f"), tok("wgmrun    f ")));
    // the variant nibble (a two-digit suffix) does not take part
    CHECK(sequence_matches(tok("ugmidle"), tok("ugmidle") | (4ull << 60)));
}

CM_TEST(animator, find_clip_prefers_the_stance_then_unarmed_then_variant_zero) {
    const std::vector<uint64_t> clips = {tok("wgmrun    f "), tok("ugmrun    f ") | (4ull << 60), tok("ugmrun    f "),
                                         tok("hgmidle")};
    CHECK_EQ(find_clip(tok("_gmrun____f"), clips, 'w'), 0);
    CHECK_EQ(find_clip(tok("_gmrun____f"), clips, 'g'), 2);  // no 'g' clip: unarmed, variant 0
    CHECK_EQ(find_clip(tok("_gmidle"), clips, 'g'), 3);      // no g, no u: anything
    CHECK_EQ(find_clip(tok("_gmwalk___f"), clips, 'u'), -1);
    CHECK_EQ(find_clip(0, clips, 'u'), -1);
}

CM_TEST(animator, plans_states_rotations_and_transition_clips) {
    AnimMachine m;
    AnimMachineState idle;
    idle.name = "Ground Idle";
    idle.actions = {play("_gmidle", 0.25f)};
    AnimMachineTransition start;
    start.targetStateName = "Ground Run Forward";
    start.actions = {play("_gtidlerunf", 0.0f, 0.1f, 16)};
    AnimMachineTransition mount;  // to a state the plan drops
    mount.targetStateName = "Mount Ground Idle";
    idle.transitions = {start, mount};
    AnimMachineState run;
    run.name = "Ground Run Forward";
    run.actions = {play("_gmrun____f", 0.05f)};
    AnimMachineState diag;
    diag.name = "Ground Run Forward Left";
    diag.actions = {rotate_z(30.0f), play("_gmrun____f", 0.25f)};
    AnimMachineState mounted;
    mounted.name = "Mount Ground Idle";
    mounted.actions = {play("_wmidle", 0.1f)};
    m.states = {idle, run, diag, mounted};

    const std::vector<uint64_t> clips = {tok("ugmidle"), tok("ugmrun    f "), tok("ugtidlerunf")};
    const AnimatorPlan p = plan_animator(m, clips, 'u', is_locomotion_state);

    CHECK_EQ(p.states.size(), 3u);
    CHECK_EQ(p.states[0].clip, 0);
    CHECK_NEAR(p.states[0].blendIn, 0.25f, 1e-6f);
    CHECK_EQ(p.states[2].clip, 1);  // the diagonal plays the straight clip...
    CHECK_NEAR(p.states[2].yawDegrees, 30.0f, 1e-3f);  // ...turned
    CHECK_EQ(p.transitions.size(), 1u);  // the one into Mount Ground Idle is dropped
    CHECK_EQ(p.transitions[0].clip, 2);
    CHECK_NEAR(p.transitions[0].blendOut, 0.1f, 1e-6f);
    CHECK(p.clips == (std::vector<int>{0, 1, 2}));
    CHECK(p.missing.empty());
}

CM_TEST(animator, a_looping_state_takes_the_loop_part_a_one_shot_the_intro) {
    const std::vector<uint64_t> clips = {tok("uamfall") | (4ull << 60), tok("uamfall") | (8ull << 60), tok("uamfall") | (12ull << 60)};
    CHECK_EQ(find_clip(tok("_amfall"), clips, 'u', true), 1);
    CHECK_EQ(find_clip(tok("_amfall"), clips, 'u', false), 0);
}

CM_TEST(animator, reports_templates_no_clip_matches) {
    AnimMachine m;
    AnimMachineState s;
    s.name = "Ground Walk Forward";
    s.actions = {play("_gmwalk___f", 0.1f)};
    m.states = {s};
    const AnimatorPlan p = plan_animator(m, {tok("ugmidle")}, 'u');
    CHECK_EQ(p.states[0].clip, -1);
    CHECK(p.missing == (std::vector<std::string>{"_gmwalk___f"}));
}

CM_TEST(animator, locomotion_states_leave_out_mounts_and_combat) {
    CHECK(is_locomotion_state("Ground Idle"));
    CHECK(is_locomotion_state("Ground Walk Backward Left"));
    CHECK(is_locomotion_state("Ground Sprint Forward Right"));
    CHECK(is_locomotion_state("Air Jump Prepare Forward"));
    CHECK(is_locomotion_state("Air Fall"));
    CHECK(is_locomotion_state("Ground Alert"));
    CHECK(is_locomotion_state("Water Swim Up Forward Left"));
    CHECK(is_locomotion_state("Water Surface Swim Backward"));
    CHECK(!is_locomotion_state("Water Limp Forward"));
    CHECK(!is_locomotion_state("Water Death"));
    CHECK(!is_locomotion_state("Mount Ground Idle"));
    CHECK(!is_locomotion_state("Ground Death Fire"));
    CHECK(!is_locomotion_state("Air Fall Tumble"));
}

CM_TEST(animator, character_states_are_everything_but_mounts) {
    CHECK(is_character_state("Ground Evade Backward"));
    CHECK(is_character_state("Air Glide Boost Left"));
    CHECK(is_character_state("Ground Death Fire"));
    CHECK(!is_character_state("Mount Air Jump"));
}

CM_TEST(animator, emote_labels_name_player_emotes_only) {
    CHECK_EQ(emote_label("ugesit04"), std::string("Sit"));
    CHECK_EQ(emote_label("ugedance    "), std::string("Dance"));
    CHECK_EQ(emote_label("thumbsup04"), std::string("Thumbs Up"));
    CHECK_EQ(emote_label("Emote_Stretch"), std::string("Stretch"));
    CHECK(emote_label("udtalive").empty());   // the revive, not an emote
    CHECK(emote_label("umewave").empty());    // a mounted variant
}

CM_TEST(animator, clean_clip_names_have_no_spaces) {
    CHECK_EQ(clean_clip_name("ugmrun    f "), std::string("ugmrun_f"));
    CHECK_EQ(clean_clip_name("wgrthrow   l08"), std::string("wgrthrow_l08"));
    CHECK_EQ(clean_clip_name("zeropose"), std::string("zeropose"));
    CHECK_EQ(clean_clip_name("   "), std::string("clip"));
}

CM_TEST(animator, select_clips_keeps_and_renames_with_parallel_data) {
    ModelPreview m;
    for (const char* n : {"a", "b", "c"}) {
        castlemist::granny::Anim a;
        a.name = n;
        m.animClips.push_back(a);
    }
    m.animClipTokens = {10, 20, 30};
    m.animClipBank = {1, 2, 3};
    m.animClipMoveSpeed = {0.0f, 150.0f, 294.0f};
    select_clips(m, {{30, "Run"}, {10, "Idle"}});
    CHECK_EQ(m.animClips.size(), 2u);
    CHECK_EQ(m.animClips[0].name, std::string("Idle"));  // model order is kept
    CHECK_EQ(m.animClips[1].name, std::string("Run"));
    CHECK(m.animClipBank == (std::vector<uint32_t>{1, 3}));
    CHECK_EQ(m.animClipMoveSpeed.size(), 2u);
    CHECK_NEAR(m.animClipMoveSpeed[1], 294.0f, 1e-6f);
}

CM_TEST(animator, json_scales_ground_speed_to_metres) {
    AnimatorPlan p;
    AnimatorState s;
    s.name = "Ground Run Forward";
    s.clip = 0;
    p.states = {s};
    p.clips = {0};
    const nlohmann::json j = animator_json(p, {"ugmrun_f"}, {1.2f}, {100.0f}, true);
    CHECK_EQ(j["clips"][0]["name"].get<std::string>(), std::string("ugmrun_f"));
    CHECK_NEAR(j["clips"][0]["moveSpeed"].get<double>(), 2.54, 1e-6);
    CHECK_EQ(j["states"][0]["clip"].get<std::string>(), std::string("ugmrun_f"));
    CHECK_EQ(j["units"].get<std::string>(), std::string("metres"));
}
