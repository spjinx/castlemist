/// @file
/// @brief Tests for ModelLoadOptions and the clip-token bookkeeping (model_preview.cpp).

#include "test_framework.h"

#include "internal.h"

using castlemist::extract::append_decoded_clips;

CM_TEST(model_load, clip_tokens_stay_parallel_when_one_fails) {
    std::vector<castlemist::model::AnimClip> clips(3);
    clips[0].token = 463;    clips[0].bankFileId = 10;   // "on"
    clips[1].token = 6351;   clips[1].bankFileId = 11;   // "off"
    clips[2].token = 464399; clips[2].bankFileId = 12;   // "open"
    ModelPreview out;
    append_decoded_clips(clips, out, [](const castlemist::model::AnimClip& c) {
        castlemist::granny::Anim a;
        a.valid = c.token != 6351;
        return a;
    });
    const std::vector<uint64_t> allTok = {463, 6351, 464399}, okTok = {463, 464399};
    const std::vector<uint32_t> okBank = {10, 12};
    CHECK(out.animationTokens == allTok);
    CHECK_EQ(out.animClips.size(), (size_t)2);
    CHECK(out.animClipTokens == okTok);
    CHECK_EQ(out.animClipBank.size(), (size_t)2);
    CHECK(out.animClipBank == okBank);
}

CM_TEST(model_load, no_clips_leaves_all_empty) {
    ModelPreview out;
    append_decoded_clips({}, out, [](const castlemist::model::AnimClip&) { return castlemist::granny::Anim{}; });
    CHECK(out.animationTokens.empty());
    CHECK(out.animClips.empty());
    CHECK(out.animClipTokens.empty());
    CHECK(out.animClipBank.empty());
}

CM_TEST(model_load, options_default_decode_everything) {
    ModelLoadOptions o;
    CHECK(o.wantGame && o.decodeTextures);
}
