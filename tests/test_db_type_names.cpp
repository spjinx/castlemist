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
    // Every one of these was cross-checked against a real gw2index build by
    // joining entries.container to chunks.struct_variant (see type_names.cpp's
    // top-of-function comment) -- not exhaustive, just enough to catch a
    // typo'd key.
    CHECK_EQ(std::string(castlemist::db::container_type_name("MODL")), std::string("Model"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("anic")), std::string("Anim Blend Tree"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("emoc")), std::string("Emote Animations"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("cmpc")), std::string("Composite"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("bone")), std::string("Composite Bone Scale"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("hvkC")), std::string("Map Collision"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("CINP")), std::string("Cinematic Scene"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("cntc")), std::string("Content Database"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("ASND")), std::string("Sound"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("ABNK")), std::string("Audio Bank"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("ABIX")), std::string("Bank Index"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("AMAT")), std::string("Material"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("mapc")), std::string("Map"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("mpsd")), std::string("Map Shadow"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("PIMG")), std::string("Image Atlas"));
    // Circumstantial rather than struct-variant confirmed (see type_names.cpp) --
    // still worth a test so a future edit can't silently drop them.
    CHECK_EQ(std::string(castlemist::db::container_type_name("cmaC")), std::string("Collision Model Manifest"));
    CHECK_EQ(std::string(castlemist::db::container_type_name("mMet")), std::string("Map Metadata"));
}

CM_TEST(type_names, an_unconfirmed_container_returns_null_rather_than_a_guess) {
    CHECK(castlemist::db::container_type_name("0s1B") == nullptr);
    CHECK(castlemist::db::container_type_name("") == nullptr);
}

CM_TEST(type_names, a_chunk_fourcc_is_not_mistaken_for_its_container) {
    // Regression test for the actual bug: an earlier version of
    // container_type_name() named these as if they were containers, when a
    // real archive shows each is only ever a chunk nested *inside* the
    // container named in the comment -- so they must resolve to nullptr, not
    // to their old (wrong) label.
    CHECK(castlemist::db::container_type_name("mach") == nullptr);  // chunk inside "anic"
    CHECK(castlemist::db::container_type_name("anim") == nullptr);  // chunk inside "emoc"
    CHECK(castlemist::db::container_type_name("comp") == nullptr);  // chunk inside "cmpc"
    CHECK(castlemist::db::container_type_name("havk") == nullptr);  // chunk inside "hvkC"
    CHECK(castlemist::db::container_type_name("CSCN") == nullptr);  // chunk inside "CINP"
    CHECK(castlemist::db::container_type_name("GRMT") == nullptr);  // chunk inside "AMAT"
    CHECK(castlemist::db::container_type_name("BKCK") == nullptr);  // chunk inside "ABNK"
    CHECK(castlemist::db::container_type_name("PGTB") == nullptr);  // chunk inside "PIMG"
    CHECK(castlemist::db::container_type_name("BIDX") == nullptr);  // chunk inside "ABIX"
    CHECK(castlemist::db::container_type_name("ROOT") == nullptr);  // chunk inside "MODL"
}
