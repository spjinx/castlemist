/// @file
/// @brief Tests for the CharacterManifest JSON form (manifest_json.h).

#include "test_framework.h"

#include "castlemist/character/manifest_json.h"

#include <stdexcept>
#include <string>

using namespace castlemist::character;

namespace {

CharacterManifest sample() {
    CharacterManifest m;
    m.name = "Test Character";
    m.race = "Sylvari";
    m.gender = "Female";
    m.profession = "Guardian";
    m.level = 80;
    m.tab_id = 2;
    m.tab_name = "WvW";
    ManifestPiece coat;
    coat.slot = "Coat";
    coat.item_id = 78611;
    coat.item_name = "Coat Item";
    coat.skin_id = 517;
    coat.skin_name = "Coat Skin";
    coat.weight_class = "Heavy";
    coat.file_ids = {1200313, 1200325};
    coat.status = PieceStatus::Ok;
    coat.dyes.push_back(ManifestDye{6, "Abyss", "cloth", {255, 16, 0}, true});
    coat.dyes.push_back(ManifestDye{1682, "", "leather", {}, false});
    ManifestPiece ring;
    ring.slot = "Ring1";
    ring.item_id = 37079;
    ring.status = PieceStatus::NoSkin;
    m.pieces = {coat, ring};
    return m;
}

} // namespace

CM_TEST(manifest_json, round_trip) {
    CharacterManifest a = sample();
    CharacterManifest b = manifest_from_json(manifest_to_json(a));
    CHECK_EQ(b.name, a.name);
    CHECK_EQ(b.race, a.race);
    CHECK_EQ(b.gender, a.gender);
    CHECK_EQ(b.profession, a.profession);
    CHECK_EQ(b.level, a.level);
    CHECK_EQ(b.tab_id, a.tab_id);
    CHECK_EQ(b.tab_name, a.tab_name);
    CHECK_EQ(b.pieces.size(), a.pieces.size());
    for (size_t i = 0; i < a.pieces.size(); ++i) {
        const ManifestPiece &x = a.pieces[i], &y = b.pieces[i];
        CHECK_EQ(y.slot, x.slot);
        CHECK_EQ(y.item_id, x.item_id);
        CHECK_EQ(y.item_name, x.item_name);
        CHECK_EQ(y.skin_id, x.skin_id);
        CHECK_EQ(y.skin_name, x.skin_name);
        CHECK_EQ(y.weight_class, x.weight_class);
        CHECK(y.file_ids == x.file_ids);
        CHECK(y.status == x.status);
        CHECK_EQ(y.dyes.size(), x.dyes.size());
        for (size_t d = 0; d < x.dyes.size(); ++d) {
            CHECK_EQ(y.dyes[d].color_id, x.dyes[d].color_id);
            CHECK_EQ(y.dyes[d].color_name, x.dyes[d].color_name);
            CHECK_EQ(y.dyes[d].material, x.dyes[d].material);
            CHECK(y.dyes[d].rgb == x.dyes[d].rgb);
            CHECK_EQ(y.dyes[d].known, x.dyes[d].known);
        }
    }
}

CM_TEST(manifest_json, rgb_is_hex) {
    nlohmann::json j = manifest_to_json(sample());
    CHECK_EQ(j["version"].get<int>(), 1);
    CHECK_EQ(j["pieces"][0]["dyes"][0]["rgb"].get<std::string>(), std::string("#FF1000"));
    CHECK_EQ(j["pieces"][0]["status"].get<std::string>(), std::string("ok"));
    CHECK_EQ(j["tab"]["id"].get<int>(), 2);
}

CM_TEST(manifest_json, rejects_other_versions) {
    nlohmann::json j = manifest_to_json(sample());
    j["version"] = 2;
    bool threw = false;
    try { manifest_from_json(j); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}
