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

#include <chrono>
#include <cstdio>
#include <random>

#include "castlemist/native/cmp_compress_method0.hpp"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"

using castlemist::extract::AtexHeader;
using castlemist::extract::estimate_mip_count;
using castlemist::extract::parse_atex_header;

CM_TEST(model_load, prefix_decode_matches_full) {
    std::mt19937 rng(7);
    std::vector<uint8_t> in(300 * 1024);
    for (size_t i = 0; i < in.size(); ++i) in[i] = (i % 97 < 60) ? uint8_t(i * 3) : uint8_t(rng() % 16);
    const std::vector<uint8_t> bits = castlemist::cmp::compress_method0(in);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> head = castlemist::cmp::decompress_method0(bits, 512);
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> full = castlemist::cmp::decompress_method0(bits, in.size());
    const auto t2 = std::chrono::steady_clock::now();
    CHECK_EQ(head.size(), size_t{512});
    CHECK(std::equal(head.begin(), head.end(), in.begin()));
    CHECK(full == in);
    std::printf("    prefix %lld us, full %lld us\n",
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count(),
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count());
}

CM_TEST(model_load, header_from_prefix_matches_full) {
    // Hand-built DXT1 ATEX: 8x8, one raw mip (dataSize = 8 + 4 blocks * 8 bytes).
    std::vector<uint8_t> a = {'A', 'T', 'E', 'X', 'D', 'X', 'T', '1', 8, 0, 8, 0};
    const uint32_t dataSize = 8 + 32, flags = 0;
    for (int i = 0; i < 4; ++i) a.push_back(uint8_t(dataSize >> (8 * i)));
    for (int i = 0; i < 4; ++i) a.push_back(uint8_t(flags >> (8 * i)));
    a.resize(a.size() + 32, 0x55);
    const castlemist::atex::Texture full = castlemist::atex::parse(a.data(), a.size());
    AtexHeader h;
    CHECK(parse_atex_header(a.data(), 16, h));   // prefix only
    CHECK_EQ(h.width, full.width);
    CHECK_EQ(h.height, full.height);
    CHECK(h.fmt == full.fmt_name);
    CHECK_EQ(estimate_mip_count(h, a.size()), (int)full.mips.size());
    CHECK_EQ(estimate_mip_count(h, a.size() + 100), 4);   // more data than mip 0: full chain 8,4,2,1
    std::vector<uint8_t> bad = a;
    bad[0] = 'Z';
    CHECK_FALSE(parse_atex_header(bad.data(), bad.size(), h));
}

// ---- decode_texture_capped helpers ----

CM_TEST(capped, halve_box_filters) {
    std::vector<uint8_t> in = {0,0,0,255, 255,0,0,255, 255,0,0,255, 0,0,0,255};
    std::vector<uint8_t> out;
    int ow = 0, oh = 0;
    castlemist::extract::halve_rgba(in, 2, 2, out, ow, oh);
    CHECK_EQ(ow, 1);
    CHECK_EQ(oh, 1);
    CHECK_EQ(out.size(), (size_t)4);
    CHECK_EQ((int)out[0], 128);
    CHECK_EQ((int)out[3], 255);
}

CM_TEST(capped, halve_odd_clamps) {
    std::vector<uint8_t> out;
    int ow = 0, oh = 0;
    castlemist::extract::halve_rgba(std::vector<uint8_t>(3 * 1 * 4, 7), 3, 1, out, ow, oh);
    CHECK_EQ(ow, 1);
    CHECK_EQ(oh, 1);
    CHECK_EQ(out.size(), (size_t)4);
    castlemist::extract::halve_rgba(std::vector<uint8_t>(1 * 5 * 4, 7), 1, 5, out, ow, oh);
    CHECK_EQ(ow, 1);
    CHECK_EQ(oh, 2);
    CHECK_EQ(out.size(), (size_t)8);
}

CM_TEST(capped, mips_stop_at_4x4) {
    using castlemist::extract::build_mips;
    auto m = build_mips(std::vector<uint8_t>(32 * 8 * 4, 1), 32, 8);
    CHECK_EQ(m.size(), (size_t)4);
    CHECK_EQ(m[1].size(), (size_t)(16 * 4 * 4));
    CHECK_EQ(m[2].size(), (size_t)(8 * 2 * 4));
    CHECK_EQ(m[3].size(), (size_t)(4 * 1 * 4));
    CHECK_EQ(build_mips(std::vector<uint8_t>(4 * 4 * 4, 1), 4, 4).size(), (size_t)1);
    auto s = build_mips(std::vector<uint8_t>(6 * 3 * 4, 1), 6, 3);
    CHECK_EQ(s.size(), (size_t)2);
    CHECK_EQ(s[1].size(), (size_t)(3 * 1 * 4));
    CHECK_EQ(build_mips(std::vector<uint8_t>(1 * 64 * 4, 1), 1, 64).size(), (size_t)5);
}

CM_TEST(capped, bytes_sum) {
    CappedTexture t;
    t.mips.push_back(std::vector<uint8_t>(8 * 8 * 4));
    t.mips.push_back(std::vector<uint8_t>(4 * 4 * 4));
    CHECK_EQ(t.bytes(), (size_t)320);
}

CM_TEST(capped, rejects_bad_max_size) {
    Gw2Dat dat;
    CappedTexture t;
    CHECK(!decode_texture_capped(dat, 123, 0, t));
    CHECK(t.mips.empty());
    CHECK_EQ(t.fileId, (uint32_t)123);
}

CM_TEST(capped, halve_short_input_is_safe) {
    std::vector<uint8_t> out;
    int ow = 0, oh = 0;
    castlemist::extract::halve_rgba(std::vector<uint8_t>(4, 9), 8, 8, out, ow, oh);
    CHECK_EQ(ow, 1);
    CHECK_EQ(oh, 1);
    CHECK(out == std::vector<uint8_t>(4, 0));
}
