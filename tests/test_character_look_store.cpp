/// @file
/// @brief Tests for the per-character look presets file (look_store.h).

#include "test_framework.h"

#include "castlemist/character/look_store.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace castlemist::character;
namespace fs = std::filesystem;

namespace {

fs::path scratch(const char* test) {
    fs::path d = fs::temp_directory_path() / (std::string("cm_test_looks_") + test);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

} // namespace

CM_TEST(look_store, missing_file_loads_empty) {
    LookStore s;
    std::string err;
    CHECK(s.load(scratch("missing") / "character_looks.json", &err));
    CHECK(s.all().empty());
    CHECK(!s.get("Musa Blossom").has_value());
}

CM_TEST(look_store, round_trips_per_character) {
    fs::path f = scratch("roundtrip") / "character_looks.json";
    LookStore s;
    CharacterLook musa{3, 12, 1048, 2001, 0};
    s.set("Musa Blossom", musa);
    s.set("Other", CharacterLook{1, 2, 3, 4, 5});
    std::string err;
    CHECK(s.save(f, &err));
    LookStore t;
    CHECK(t.load(f, &err));
    CHECK(t.get("Musa Blossom") == std::optional<CharacterLook>(musa));
    CHECK_EQ(t.get("Other")->hair_color2, 5u);
    CHECK(t.remove("Other"));
    CHECK(!t.get("Other").has_value());
}

CM_TEST(look_store, malformed_file_is_rejected_and_keeps_the_store) {
    fs::path f = scratch("bad") / "character_looks.json";
    { std::ofstream o(f); o << "{\"looks\": [1, 2]}"; }
    LookStore s;
    s.set("Keep", CharacterLook{});
    std::string err;
    CHECK_FALSE(s.load(f, &err));
    CHECK(!err.empty());
    CHECK(s.get("Keep").has_value());
}
