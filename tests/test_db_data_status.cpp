/// @file
/// @brief Tests for the staleness verdicts behind the Data status window: the
/// dat fingerprint, the stamp store, and the rule each derived file is judged by.

#include "test_framework.h"

#include "castlemist/db/data_status.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace castlemist::db;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

const DatFingerprint kOld{89104234416ull, 89000000000ull, 39283328u};
const DatFingerprint kNew{89204234416ull, 89100000000ull, 39283456u};

fs::path temp_file(const char* name) { return fs::temp_directory_path() / name; }

} // namespace

CM_TEST(data_status, fingerprint_round_trips_through_text) {
    auto back = DatFingerprint::parse(kOld.to_string());
    CHECK(back.has_value());
    CHECK(*back == kOld);
}

CM_TEST(data_status, fingerprint_rejects_garbage) {
    CHECK_FALSE(DatFingerprint::parse("").has_value());
    CHECK_FALSE(DatFingerprint::parse("12:34").has_value());
    CHECK_FALSE(DatFingerprint::parse("12:34:56:78").has_value());
    CHECK_FALSE(DatFingerprint::parse("0:0:0").has_value());
}

CM_TEST(data_status, stamped_file_is_ok_only_for_the_same_dat) {
    CHECK(judge_stamped(Stamp{kOld, 1}, kOld).state == Freshness::Ok);
    CHECK(judge_stamped(Stamp{kOld, 1}, kNew).state == Freshness::Stale);
    CHECK(judge_stamped(std::nullopt, kNew).state == Freshness::Unknown);
    CHECK(judge_stamped(Stamp{kOld, 1}, DatFingerprint{}).state == Freshness::Unknown);
}

CM_TEST(data_status, observe_dat_records_when_the_dat_changed) {
    StampStore s;
    CHECK_FALSE(s.observe_dat(kOld, 100));  // first sighting: no "before"
    CHECK_EQ(s.dat_first_seen(), int64_t{0});
    CHECK_FALSE(s.observe_dat(kOld, 200));  // unchanged
    CHECK(s.observe_dat(kNew, 300));        // patched
    CHECK_EQ(s.dat_first_seen(), int64_t{300});
    CHECK(*s.last_dat() == kNew);
}

CM_TEST(data_status, stamp_store_survives_a_save_and_load) {
    const fs::path f = temp_file("cm_test_data_stamps.json");
    StampStore a;
    a.observe_dat(kOld, 0);
    a.observe_dat(kNew, 4242);
    a.set("content_map", Stamp{kOld, 17});
    CHECK(a.save(f));

    StampStore b;
    b.load(f);
    CHECK(*b.last_dat() == kNew);
    CHECK_EQ(b.dat_first_seen(), int64_t{4242});
    auto st = b.get("content_map");
    CHECK(st.has_value());
    CHECK(st->dat == kOld);
    CHECK_EQ(st->at, int64_t{17});
    CHECK_FALSE(b.get("content_names").has_value());
    fs::remove(f);
}

CM_TEST(data_status, a_corrupt_stamp_file_loads_empty) {
    const fs::path f = temp_file("cm_test_data_stamps_bad.json");
    std::ofstream(f) << "{ not json";
    StampStore s;
    s.load(f);
    CHECK_FALSE(s.last_dat().has_value());
    fs::remove(f);
}

CM_TEST(data_status, index_with_a_fingerprint_is_judged_by_it) {
    std::map<std::string, std::string> meta{
        {"dat_path", "E:\\Games\\Gw2.dat"}, {"dat_size", "1"}, {"mft_entries", "1"},
        {"dat_fingerprint", kOld.to_string()}};
    CHECK(judge_index(meta, kOld, 818506, "E:/Games/Gw2.dat").state == Freshness::Ok);
    CHECK(judge_index(meta, kNew, 818506, "E:/Games/Gw2.dat").state == Freshness::Stale);
}

CM_TEST(data_status, an_older_index_falls_back_to_size_and_entry_count) {
    std::map<std::string, std::string> meta{
        {"dat_path", "E:\\Games\\Gw2.dat"}, {"dat_size", std::to_string(kOld.file_size)}, {"mft_entries", "818506"}};
    CHECK(judge_index(meta, kOld, 818506, "E:\\Games\\Gw2.dat").state == Freshness::Ok);
    CHECK(judge_index(meta, kOld, 818507, "E:\\Games\\Gw2.dat").state == Freshness::Stale);
    CHECK(judge_index(meta, kNew, 818506, "E:\\Games\\Gw2.dat").state == Freshness::Stale);
}

CM_TEST(data_status, an_index_of_another_archive_is_stale) {
    std::map<std::string, std::string> meta{
        {"dat_path", "C:\\Users\\me\\AppData\\Roaming\\Guild Wars 2\\Local.dat"},
        {"dat_fingerprint", kOld.to_string()}};
    CHECK(judge_index(meta, kOld, 1, "E:\\Games\\Gw2.dat").state == Freshness::Stale);
}

CM_TEST(data_status, an_unfinished_index_is_stale) {
    CHECK(judge_index({}, kOld, 1, "E:\\Games\\Gw2.dat").state == Freshness::Stale);
}

CM_TEST(data_status, mtime_rule_needs_a_seen_patch_to_call_anything_stale) {
    CHECK(judge_by_mtime(10, 0).state == Freshness::Ok);
    CHECK(judge_by_mtime(10, 20).state == Freshness::Stale);
    CHECK(judge_by_mtime(30, 20).state == Freshness::Ok);
}

CM_TEST(data_status, template_with_provenance_matches_the_exe_build) {
    json src = {{"exe", "Gw2-64.exe"}, {"peTimestamp", 1234}, {"size", 1}};
    CHECK(judge_template(&src, 1234u, 0, 0, -1).state == Freshness::Ok);
    CHECK(judge_template(&src, 9999u, 0, 0, -1).state == Freshness::Stale);
}

CM_TEST(data_status, unmapped_chunk_versions_make_a_template_stale_whatever_it_claims) {
    json src = {{"peTimestamp", 1234}};
    CHECK(judge_template(&src, 1234u, 0, 0, 3).state == Freshness::Stale);
}

CM_TEST(data_status, template_without_provenance_leans_on_the_index_check) {
    CHECK(judge_template(nullptr, std::nullopt, 100, 200, 0).state == Freshness::Ok);
    CHECK(judge_template(nullptr, std::nullopt, 100, 200, -1).state == Freshness::Unknown);
    CHECK(judge_template(nullptr, std::nullopt, 300, 200, -1).state == Freshness::Unknown);
}

CM_TEST(data_status, unmapped_versions_reports_known_fourccs_with_a_missing_version) {
    json tpl = {{"chunks", {{"MODL", {{"70", "ModelFileDataV70"}}}, {"GEOM", {{"1", "Geom"}}}}},
                {"fileTypes", {{"AMAT", {{"GRMT", {{"5", "MatV5"}}}}}}}};
    std::vector<ChunkVersion> seen{
        {"MODL", "MODL", 70},  // mapped
        {"MODL", "MODL", 71},  // fourcc known, version not: the patch signature
        {"ABCD", "MODL", 71},  // same fourcc+version in another container: reported once
        {"MODL", "XXXX", 1},   // fourcc the template never described: not counted
        {"AMAT", "GRMT", 5},   // container-specific mapping
    };
    auto out = unmapped_versions(tpl, seen);
    CHECK_EQ(out.size(), size_t{1});
    CHECK_EQ(out[0].fourcc, std::string("MODL"));
    CHECK_EQ(out[0].version, 71);
}

CM_TEST(data_status, pe_timestamp_reads_the_coff_header) {
    const fs::path f = temp_file("cm_test_pe.bin");
    {
        std::string img(0x100, '\0');
        img[0] = 'M';
        img[1] = 'Z';
        img[0x3C] = static_cast<char>(0x80);
        img.replace(0x80, 4, std::string("PE\0\0", 4));
        const uint32_t ts = 0x5F3A1B2Cu;
        for (int i = 0; i < 4; ++i) img[0x88 + i] = static_cast<char>((ts >> (8 * i)) & 0xFF);
        std::ofstream(f, std::ios::binary) << img;
    }
    auto ts = pe_timestamp(f);
    CHECK(ts.has_value());
    CHECK_EQ(*ts, 0x5F3A1B2Cu);
    fs::remove(f);
    CHECK_FALSE(pe_timestamp(f).has_value());
}
