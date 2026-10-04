/// @file
/// @brief Tests for the plain-JSON GW2 API key store (key_store.h).

#include "test_framework.h"

#include "castlemist/character/key_store.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace castlemist::character;
namespace fs = std::filesystem;

namespace {

// A fresh, empty scratch directory per test.
fs::path scratch(const char* test) {
    fs::path d = fs::temp_directory_path() / (std::string("cm_test_keys_") + test);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

void write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

} // namespace

CM_TEST(key_store, missing_file_loads_empty) {
    fs::path d = scratch("missing");
    KeyStore ks;
    std::string err;
    CHECK(ks.load(d / "api_keys.json", &err));
    CHECK(ks.list().empty());
}

CM_TEST(key_store, round_trip) {
    fs::path f = scratch("round_trip") / "api_keys.json";
    KeyStore ks;
    ks.add("Main", "AAAA");
    ks.add("Alt", "BBBB");
    std::string err;
    CHECK(ks.save(f, &err));

    KeyStore back;
    CHECK(back.load(f, &err));
    CHECK_EQ(back.list().size(), size_t{2});
    CHECK_EQ(back.list()[0].name, std::string("Main"));
    CHECK_EQ(back.list()[0].key, std::string("AAAA"));
    CHECK_EQ(back.list()[1].name, std::string("Alt"));
    CHECK_EQ(back.list()[1].key, std::string("BBBB"));
}

CM_TEST(key_store, add_same_name_replaces) {
    KeyStore ks;
    ks.add("Main", "A");
    ks.add("Main", "B");
    CHECK_EQ(ks.list().size(), size_t{1});
    CHECK(ks.get("Main") != nullptr);
    CHECK_EQ(ks.get("Main")->key, std::string("B"));
}

CM_TEST(key_store, rename_and_remove) {
    KeyStore ks;
    ks.add("Main", "A");
    ks.add("Alt", "B");
    CHECK_FALSE(ks.rename("Main", "Alt"));  // target exists
    CHECK_FALSE(ks.rename("Nope", "X"));    // source missing
    CHECK(ks.remove("Alt"));
    CHECK(ks.rename("Main", "Alt"));
    CHECK(ks.get("Main") == nullptr);
    CHECK_EQ(ks.get("Alt")->key, std::string("A"));
    CHECK_FALSE(ks.remove("nope"));
}

CM_TEST(key_store, malformed_file_is_error_and_untouched) {
    fs::path f = scratch("malformed") / "api_keys.json";
    write_text(f, "{\"keys\": [");
    KeyStore ks;
    ks.add("Keep", "K");
    std::string err;
    CHECK_FALSE(ks.load(f, &err));
    CHECK(!err.empty());
    CHECK_EQ(read_text(f), std::string("{\"keys\": ["));
    CHECK(ks.get("Keep") != nullptr);  // store unchanged by a failed load
}

CM_TEST(key_store, root_found_from_nested_dir) {
    fs::path root = scratch("root_nested");
    write_text(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.25)\nproject(castlemist\n  VERSION 1.0)\n");
    fs::path bin = root / "build" / "debug" / "bin";
    fs::create_directories(bin);
    CHECK_EQ(find_castlemist_root(bin).string(), root.string());
}

CM_TEST(key_store, root_falls_back_to_start_dir) {
    fs::path d = scratch("root_fallback") / "a" / "b";
    fs::create_directories(d);
    write_text(d.parent_path() / "CMakeLists.txt", "project(something_else)\n");
    CHECK_EQ(find_castlemist_root(d).string(), d.string());
}
