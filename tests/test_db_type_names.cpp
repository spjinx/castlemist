/// @file
/// @brief Tests for the coarse-type / container friendly-name lookups that back
/// the index-mode type/container filter combos (see index_ui.cpp's fill()).

#include "test_framework.h"

#include "castlemist/db/type_names.h"

#include <cstring>
#include <string>

CM_TEST(type_names, names_every_coarse_type_index_builder_can_produce) {
    // Mirrors index_builder.cpp's classify(): these are the only values
    // entries.type ever holds, so every one of them must resolve to a label.
    static const char* kCoarseTypes[] = {
        "packfile", "texture", "dds", "strs", "riff",
        "png", "jpeg", "exe", "asnd", "binary", "empty",
    };
    for (const char* t : kCoarseTypes) {
        const char* name = castlemist::db::coarse_type_name(t);
        CHECK(name != nullptr);
        CHECK(std::strlen(name) > 0);
    }
}

CM_TEST(type_names, an_unrecognized_coarse_type_returns_null) {
    CHECK(castlemist::db::coarse_type_name("not-a-real-type") == nullptr);
    CHECK(castlemist::db::coarse_type_name("") == nullptr);
}

CM_TEST(type_names, names_confirmed_containers) {
    // A representative sample of containers this codebase has already confirmed
    // elsewhere (docs/research/filetypes.md, dumps/packfile/gw2_packfile.json,
    // gw2-shaders-dxbc.md) -- not exhaustive, just enough to catch a typo'd key.
    CHECK_EQ(std::string(castlemist::db::container_type_name("MODL")), std::string("Model"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("mach")), std::string("Anim Blend Tree"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("cntc")), std::string("Content Database"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("ASND")), std::string("Sound"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("ABNK")), std::string("Audio Bank"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("AMAT")), std::string("Material"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("mapc")), std::string("Map"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("PIMG")), std::string("Image Atlas"));
}

CM_TEST(type_names, an_unconfirmed_container_returns_null_rather_than_a_guess) {
    // Obfuscated containers with no resolved chunks in the struct template
    // (dumps/packfile/gw2_packfile.json) -- deliberately left unnamed so the
    // caller falls back to the raw fourcc instead of showing a fabricated label.
    CHECK(castlemist::db::container_type_name("0s1B") == nullptr);
    CHECK(castlemist::db::container_type_name("") == nullptr);
}
