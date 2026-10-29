/// @file
/// @brief Tests for the Particle Browser's model: rows, the fx index file, filters, the describer.

#include "test_framework.h"

#include "detail/particle_browser.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace castlemist::ui::particles;
namespace fs = std::filesystem;

namespace {

std::vector<ModelRow> three_rows() {
    ModelRow a; a.fileId = 100; a.clouds = 2; a.emitters = 5; a.streaks = 1; a.alignMask = 0x2;
    ModelRow b; b.fileId = 200; b.clouds = 1; b.emitters = 1; b.bursts = 1; b.alignMask = 0x20;
    ModelRow c; c.fileId = 300; c.lightning = 3; c.meshEmitters = 2; c.clipTracks = 4; c.alignMask = 0x2;
    return {a, b, c};
}

fs::path temp_file(const char* name) { return fs::temp_directory_path() / name; }

bool has_meaning(const std::vector<FieldLine>& lines, const std::string& part) {
    for (const auto& l : lines)
        if (l.meaning.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

CM_TEST(ui, fx_index_round_trip) {
    const fs::path p = temp_file("cm_test_fx_index.bin");
    CHECK(save_index(p, "dat-A", 1234, three_rows()));
    uint32_t upTo = 0;
    std::vector<ModelRow> back;
    CHECK(load_index(p, "dat-A", upTo, back));
    CHECK_EQ(upTo, 1234u);
    CHECK_EQ(back.size(), size_t{3});
    CHECK_EQ(back[2].fileId, 300u);
    CHECK_EQ(back[2].clipTracks, uint16_t{4});
    CHECK_EQ(back[1].alignMask, uint8_t{0x20});
    fs::remove(p);
}

CM_TEST(ui, fx_index_rejects_other_dat_and_truncation) {
    const fs::path p = temp_file("cm_test_fx_index2.bin");
    CHECK(save_index(p, "dat-A", 10, three_rows()));
    uint32_t upTo = 0;
    std::vector<ModelRow> back;
    CHECK(!load_index(p, "dat-B", upTo, back));       // another dat
    const auto size = fs::file_size(p);
    fs::resize_file(p, size - 5);
    CHECK(!load_index(p, "dat-A", upTo, back));       // truncated
    CHECK(back.empty());
    fs::remove(p);
}

CM_TEST(ui, fx_filter_features_and_search) {
    const std::vector<ModelRow> rows = three_rows();
    ModelFilter f;
    CHECK_EQ(filter_rows(rows, f).size(), size_t{3});
    f.streak = true;
    CHECK(filter_rows(rows, f) == std::vector<size_t>{0});
    f = {}; f.burst = true;
    CHECK(filter_rows(rows, f) == std::vector<size_t>{1});
    f = {}; f.mesh = true; f.tracks = true; f.lightning = true;
    CHECK(filter_rows(rows, f) == std::vector<size_t>{2});
    f = {}; f.alignCase = 1;
    CHECK((filter_rows(rows, f) == std::vector<size_t>{0, 2}));
    f = {}; f.search = "150-300";
    CHECK((filter_rows(rows, f) == std::vector<size_t>{1, 2}));
    f = {}; f.search = "100, 300";
    CHECK((filter_rows(rows, f) == std::vector<size_t>{0, 2}));
    f = {}; f.search = "pillar";                       // not ids: no model rows
    CHECK(filter_rows(rows, f).empty());
}

CM_TEST(ui, describe_emitter_alignment_burst_flags) {
    ParticleEmitterCPU e;
    e.hasPlane = true;
    e.alignmentType = 1;
    e.emitterFlags = 0x8;
    e.flags = 0x10 | 0x400;
    e.scaleCurvePreset = 9;
    e.lifetime[0] = 1; e.lifetime[1] = 2;
    const std::vector<FieldLine> lines = describe_emitter(e);
    CHECK(has_meaning(lines, "axis billboard"));
    CHECK(has_meaning(lines, "burst"));
    CHECK(has_meaning(lines, "Y size = X"));
    CHECK(has_meaning(lines, "reversed"));
    CHECK(has_meaning(lines, "t^2"));
    bool life = false;
    for (const auto& l : lines) life = life || (l.label == "lifetime" && l.value == "1..2 s");
    CHECK(life);
}
