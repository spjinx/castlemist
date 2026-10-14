/// @file
/// @brief Tests for the npc layer: packed clip-name tokens and clip grouping.

#include "test_framework.h"

#include "castlemist/npc/tokens.h"

using namespace castlemist::npc;

CM_TEST(npc, decodes_a_packed_clip_name) {
    CHECK_EQ(decode_token(0x80000029dd5604f7ULL), std::string("wgalunge"));
    CHECK_EQ(decode_token(0x2cdf07c8baULL), std::string("zeropose"));
}

CM_TEST(npc, rejects_groups_above_z) {
    CHECK_EQ(decode_token(0x1f), std::string());
}

CM_TEST(npc, groups_clips_by_action) {
    CHECK_EQ(clip_group("wgalunge08"), std::string("lunge"));
    CHECK_EQ(clip_prefix("wgalunge08"), std::string("wga"));
    CHECK_EQ(clip_group("uamland    l08"), std::string("land    l"));
    CHECK_EQ(clip_group("zeropose"), std::string("zeropose"));
    CHECK_EQ(clip_prefix("zeropose"), std::string());
}
