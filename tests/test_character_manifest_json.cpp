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
    coat.skin_type = "Armor";
    coat.skin_token = 0x00000348C28A32A3ull;
    coat.dyes.push_back(ManifestDye{0, 6, "Abyss", "cloth", {255, 16, 0}, true, DyeShift{-8, 1.0f, 34, 0.3125f, 1.09375f}});
    coat.dyes.push_back(ManifestDye{3, 1682, "", "leather", {}, false});
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
        CHECK_EQ(y.skin_type, x.skin_type);
        CHECK_EQ(y.skin_token, x.skin_token);
        CHECK_EQ(y.dyes.size(), x.dyes.size());
        for (size_t d = 0; d < x.dyes.size(); ++d) {
            CHECK_EQ(y.dyes[d].slot, x.dyes[d].slot);
            CHECK_EQ(y.dyes[d].color_id, x.dyes[d].color_id);
            CHECK_EQ(y.dyes[d].color_name, x.dyes[d].color_name);
            CHECK_EQ(y.dyes[d].material, x.dyes[d].material);
            CHECK(y.dyes[d].rgb == x.dyes[d].rgb);
            CHECK_EQ(y.dyes[d].known, x.dyes[d].known);
            CHECK_EQ(y.dyes[d].shift.has_value(), x.dyes[d].shift.has_value());
            if (x.dyes[d].shift) CHECK_NEAR(y.dyes[d].shift->hue, x.dyes[d].shift->hue, 1e-6);
        }
    }
}

CM_TEST(manifest_json, rgb_is_hex) {
    nlohmann::json j = manifest_to_json(sample());
    CHECK_EQ(j["version"].get<int>(), 1);
    CHECK_EQ(j["pieces"][0]["dyes"][0]["rgb"].get<std::string>(), std::string("#FF1000"));
    CHECK_EQ(j["pieces"][0]["dyes"][1]["slot"].get<int>(), 3);
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

CM_TEST(manifest_json, old_manifest_without_new_fields_loads) {
    nlohmann::json j = manifest_to_json(sample());
    for (auto& p : j["pieces"]) {
        p.erase("skin_type");
        p.erase("skin_token");
        for (auto& d : p["dyes"]) d.erase("shift");
    }
    CharacterManifest m = manifest_from_json(j);
    CHECK(m.pieces[0].skin_type.empty());
    CHECK_EQ(m.pieces[0].skin_token, 0ull);
    CHECK(!m.pieces[0].dyes[0].shift.has_value());
}

CM_TEST(manifest_json, token_is_hex_string) {
    nlohmann::json j = manifest_to_json(sample());
    CHECK_EQ(j["pieces"][0]["skin_token"].get<std::string>(), std::string("0x00000348C28A32A3"));
}
