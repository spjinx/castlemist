/// @file
/// @brief Tests for resolving a character's equipment into a manifest
///        (resolver.h) and the end-to-end fetch_character() over the fixtures.

#include "test_framework.h"

#include "fake_http.h"

#include "castlemist/character/fetch.h"
#include "castlemist/character/resolver.h"

#include <nlohmann/json.hpp>
#include <string>

using namespace castlemist::character;
using nlohmann::json;

namespace {

struct FakeAssets : AssetLookup {
    bool is_built = true;
    std::map<uint32_t, std::vector<uint32_t>> skins;
    std::map<uint32_t, uint32_t> item_skins;

    bool built() const override { return is_built; }
    std::vector<uint32_t> skin_assets(uint32_t skin_id) const override {
        auto it = skins.find(skin_id);
        return it == skins.end() ? std::vector<uint32_t>{} : it->second;
    }
    std::optional<uint32_t> item_skin(uint32_t item_id) const override {
        auto it = item_skins.find(item_id);
        if (it == item_skins.end()) return std::nullopt;
        return it->second;
    }
};

CharacterCore core(const char* race = "Human", const char* gender = "Male") {
    return CharacterCore{"Tester", race, gender, "Warrior", 80};
}

EquipmentTab one(EquipmentEntry e) {
    EquipmentTab t;
    t.tab = 1;
    t.name = "Build";
    t.is_active = true;
    t.equipment.push_back(std::move(e));
    return t;
}

EquipmentEntry entry(uint32_t item, std::optional<uint32_t> skin = std::nullopt) {
    EquipmentEntry e;
    e.item_id = item;
    e.slot = "Coat";
    e.skin = skin;
    return e;
}

ApiItem item(uint32_t id, std::optional<uint32_t> default_skin) {
    return ApiItem{id, "Item " + std::to_string(id), "Armor", default_skin};
}

ApiColor color(uint32_t id, std::array<uint8_t, 3> cloth, std::array<uint8_t, 3> leather) {
    ApiColor c;
    c.id = id;
    c.name = "Color " + std::to_string(id);
    c.rgb["cloth"] = cloth;
    c.rgb["leather"] = leather;
    return c;
}

const ManifestPiece& only_piece(const CharacterManifest& m) {
    if (m.pieces.size() != 1) ::castlemist::test::fail(__FILE__, __LINE__, "expected exactly one piece");
    return m.pieces[0];
}

} // namespace

CM_TEST(resolver, transmute_override_wins) {
    FakeAssets a;
    a.skins[10] = {111};
    auto m = resolve_character(core(), one(entry(7, 10)), {{7, item(7, 20)}}, {}, {}, a);
    CHECK_EQ(only_piece(m).skin_id, 10u);
    CHECK(only_piece(m).status == PieceStatus::Ok);
    CHECK(only_piece(m).file_ids == std::vector<uint32_t>{111});
}

CM_TEST(resolver, default_skin_used_without_override) {
    FakeAssets a;
    a.skins[20] = {222};
    auto m = resolve_character(core(), one(entry(7)), {{7, item(7, 20)}}, {}, {}, a);
    CHECK_EQ(only_piece(m).skin_id, 20u);
    CHECK_EQ(only_piece(m).item_name, std::string("Item 7"));
}

CM_TEST(resolver, item_missing_from_api_uses_cmap_skin) {
    FakeAssets a;
    a.item_skins[7] = 30;
    a.skins[30] = {333};
    auto m = resolve_character(core(), one(entry(7)), {}, {}, {}, a);
    CHECK_EQ(only_piece(m).skin_id, 30u);
    CHECK(only_piece(m).item_name.empty());
    CHECK(only_piece(m).status == PieceStatus::Ok);
}

CM_TEST(resolver, trinket_is_no_skin) {
    FakeAssets a;
    auto m = resolve_character(core(), one(entry(7)), {{7, item(7, std::nullopt)}}, {}, {}, a);
    CHECK(only_piece(m).status == PieceStatus::NoSkin);
    CHECK(only_piece(m).file_ids.empty());
    CHECK(only_piece(m).dyes.empty());
}

CM_TEST(resolver, unresolved_and_no_content_map) {
    FakeAssets a;
    auto m = resolve_character(core(), one(entry(7, 10)), {}, {}, {}, a);
    CHECK(only_piece(m).status == PieceStatus::Unresolved);
    a.is_built = false;
    a.skins[10] = {111};
    m = resolve_character(core(), one(entry(7, 10)), {}, {}, {}, a);
    CHECK(only_piece(m).status == PieceStatus::NoContentMap);
    CHECK(only_piece(m).file_ids.empty());
}

CM_TEST(resolver, null_dye_falls_back_to_skin_default) {
    FakeAssets a;
    a.skins[10] = {111};
    ApiSkin s;
    s.id = 10;
    s.dye_default = {DyeSlot{5, "cloth"}, DyeSlot{6, "leather"}};
    EquipmentEntry e = entry(7, 10);
    e.dyes = {100u, std::nullopt, std::nullopt, std::nullopt};
    auto m = resolve_character(core(), one(e), {}, {{10, s}},
                               {{100, color(100, {1, 2, 3}, {4, 5, 6})}, {6, color(6, {7, 8, 9}, {10, 11, 12})}}, a);
    const auto& d = only_piece(m).dyes;
    CHECK_EQ(d.size(), size_t{2});
    CHECK_EQ(d[0].color_id, 100u);
    CHECK_EQ(d[0].material, std::string("cloth"));
    CHECK(d[0].rgb == (std::array<uint8_t, 3>{1, 2, 3}));
    CHECK(d[0].known);
    CHECK_EQ(d[1].color_id, 6u);
    CHECK_EQ(d[1].material, std::string("leather"));
    CHECK(d[1].rgb == (std::array<uint8_t, 3>{10, 11, 12}));
    CHECK_EQ(d[1].color_name, std::string("Color 6"));
}

CM_TEST(resolver, race_gender_override_slots) {
    FakeAssets a;
    a.skins[10] = {111};
    ApiSkin s;
    s.id = 10;
    s.dye_default = {DyeSlot{5, "cloth"}};
    s.dye_overrides["CharrFemale"] = {DyeSlot{9, "metal"}, std::nullopt};
    auto m = resolve_character(core("Charr", "Female"), one(entry(7, 10)), {}, {{10, s}}, {}, a);
    const auto& d = only_piece(m).dyes;
    CHECK_EQ(d.size(), size_t{1});  // the null override slot is skipped
    CHECK_EQ(d[0].color_id, 9u);
    CHECK_EQ(d[0].material, std::string("metal"));
}

CM_TEST(resolver, weapon_has_no_dyes) {
    FakeAssets a;
    a.skins[10] = {111};
    ApiSkin s;
    s.id = 10;
    s.type = "Weapon";
    EquipmentEntry e = entry(7, 10);
    e.dyes = {1u, 2u, 3u, 4u};
    auto m = resolve_character(core(), one(e), {}, {{10, s}}, {}, a);
    CHECK(only_piece(m).dyes.empty());
    CHECK(only_piece(m).status == PieceStatus::Ok);
}

CM_TEST(resolver, unknown_color_is_marked) {
    FakeAssets a;
    a.skins[10] = {111};
    ApiSkin s;
    s.id = 10;
    s.dye_default = {DyeSlot{5, "cloth"}};
    auto m = resolve_character(core(), one(entry(7, 10)), {}, {{10, s}}, {}, a);
    CHECK_EQ(only_piece(m).dyes.size(), size_t{1});
    CHECK_FALSE(only_piece(m).dyes[0].known);
    CHECK(only_piece(m).status == PieceStatus::Ok);
}

CM_TEST(resolver, collect_ids_dedup_sorted) {
    EquipmentTab t;
    EquipmentEntry r1 = entry(50), r2 = entry(50), coat = entry(9, 300);
    coat.dyes = {7u, std::nullopt, 3u, std::nullopt};
    t.equipment = {r1, coat, r2};
    CHECK(collect_item_ids(t) == (std::vector<uint32_t>{9, 50}));

    FakeAssets a;
    a.item_skins[50] = 400;
    CHECK(collect_skin_ids(t, {{9, item(9, 200)}}, a) == (std::vector<uint32_t>{300, 400}));

    ApiSkin s;
    s.id = 300;
    s.dye_default = {DyeSlot{12, "cloth"}, DyeSlot{3, "metal"}};
    CHECK(collect_color_ids(t, {{300, s}}) == (std::vector<uint32_t>{3, 7, 12}));
}

CM_TEST(resolver, header_fields_copied) {
    FakeAssets a;
    auto m = resolve_character(core("Asura", "Female"), one(entry(7)), {}, {}, {}, a);
    CHECK_EQ(m.name, std::string("Tester"));
    CHECK_EQ(m.race, std::string("Asura"));
    CHECK_EQ(m.gender, std::string("Female"));
    CHECK_EQ(m.profession, std::string("Warrior"));
    CHECK_EQ(m.level, 80);
    CHECK_EQ(m.tab_id, 1);
    CHECK_EQ(m.tab_name, std::string("Build"));
    CHECK_EQ(only_piece(m).slot, std::string("Coat"));
}

// ---- fetch_character over the captured fixtures -------------------------------

namespace {

// Every route fetch_character requests, served from the fixtures; the batched
// ids= lookups answer with the whole fixture regardless of the id list.
FakeHttpClient fetch_client() {
    const std::string base = kApiBase;
    FakeHttpClient f;
    f.routes[base + "/v2/characters/Test%20Character/core"] = {200, read_fixture("core.json"), ""};
    f.routes[base + "/v2/characters/Test%20Character/equipmenttabs?tabs=all"] = {
        200, read_fixture("equipmenttabs.json"), ""};
    f.prefix_routes[base + "/v2/items?ids="] = {200, read_fixture("items.json"), ""};
    f.prefix_routes[base + "/v2/skins?ids="] = {200, read_fixture("skins.json"), ""};
    f.prefix_routes[base + "/v2/colors?ids="] = {200, read_fixture("colors.json"), ""};
    return f;
}

} // namespace

CM_TEST(fetch, fetch_character_end_to_end) {
    FakeAssets a;
    json skins = json::parse(read_fixture("skins.json"));
    for (const json& s : skins) a.skins[s["id"].get<uint32_t>()] = {1};
    FakeHttpClient f = fetch_client();
    Gw2Api api(f, "KEY");
    FetchResult r = fetch_character(api, "Test Character", std::nullopt, a);

    json tabs = json::parse(read_fixture("equipmenttabs.json"));
    json active;
    for (const json& t : tabs) if (t["is_active"].get<bool>()) active = t;
    CHECK_EQ(r.manifest.name, std::string("Test Character"));
    CHECK_EQ(r.manifest.tab_id, active["tab"].get<int>());
    CHECK_EQ(r.manifest.pieces.size(), active["equipment"].size());
    CHECK_EQ(r.tabs.size(), tabs.size());
    size_t ok = 0;
    for (const ManifestPiece& p : r.manifest.pieces) {
        CHECK(p.status != PieceStatus::NoContentMap);
        if (p.status == PieceStatus::Ok) ++ok;
    }
    CHECK(ok > 0);
}

CM_TEST(fetch, fetch_missing_tab_throws) {
    FakeAssets a;
    FakeHttpClient f = fetch_client();
    Gw2Api api(f, "KEY");
    bool threw = false;
    try {
        fetch_character(api, "Test Character", 99, a);
    } catch (const ApiError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("no equipment tab 99") != std::string::npos);
    }
    CHECK(threw);
}
