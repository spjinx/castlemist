/// @file
/// @brief Tests for the typed GW2 API client (gw2_api.h), run against real API
///        responses captured by tools/character/capture_fixtures.py (scrubbed of
///        the account's key, key name, character names and guild).

#include "test_framework.h"

#include "fake_http.h"

#include "castlemist/character/gw2_api.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace castlemist::character;
using nlohmann::json;

namespace {

const std::string kBase = kApiBase;

json fixture(const char* name) { return json::parse(read_fixture(name)); }

std::vector<uint32_t> ids_of(const json& arr) {
    std::vector<uint32_t> ids;
    for (const json& o : arr) ids.push_back(o["id"].get<uint32_t>());
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string join(const std::vector<uint32_t>& ids) {
    std::string s;
    for (uint32_t id : ids) s += (s.empty() ? "" : ",") + std::to_string(id);
    return s;
}

// Routes every fixture under the URL Gw2Api requests it by.
FakeHttpClient fixture_client() {
    FakeHttpClient f;
    f.routes[kBase + "/v2/tokeninfo"] = {200, read_fixture("tokeninfo.json"), ""};
    f.routes[kBase + "/v2/characters"] = {200, read_fixture("characters.json"), ""};
    f.routes[kBase + "/v2/characters/Test%20Character/core"] = {200, read_fixture("core.json"), ""};
    f.routes[kBase + "/v2/characters/Test%20Character/equipmenttabs?tabs=all"] = {
        200, read_fixture("equipmenttabs.json"), ""};
    for (const char* ep : {"items", "skins", "colors"}) {
        std::string file = std::string(ep) + ".json";
        f.routes[kBase + "/v2/" + ep + "?ids=" + join(ids_of(fixture(file.c_str())))] = {
            200, read_fixture(file.c_str()), ""};
    }
    return f;
}

bool has_header(const Headers& h, const std::string& k, const std::string& v) {
    return std::find(h.begin(), h.end(), std::make_pair(k, v)) != h.end();
}

} // namespace

CM_TEST(api, sends_bearer_and_schema_headers) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    api.token_info();
    CHECK_EQ(f.calls.size(), size_t{1});
    CHECK(has_header(f.calls[0].second, "Authorization", "Bearer KEY"));
    CHECK(has_header(f.calls[0].second, "X-Schema-Version", "latest"));
    CHECK(f.calls[0].first.find("KEY") == std::string::npos);
}

CM_TEST(api, token_info_parses) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    TokenInfo t = api.token_info();
    CHECK_EQ(t.name, std::string("Test Key"));
    CHECK(t.permissions == fixture("tokeninfo.json")["permissions"].get<std::vector<std::string>>());
}

CM_TEST(api, missing_scopes_lists_builds) {
    CHECK(missing_scopes(TokenInfo{"", {"account", "characters"}}) == std::vector<std::string>{"builds"});
    CHECK(missing_scopes(TokenInfo{"", {}}) == (std::vector<std::string>{"account", "characters", "builds"}));
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    CHECK(missing_scopes(api.token_info()).empty());
}

CM_TEST(api, character_names_and_core) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    CHECK(api.character_names() == fixture("characters.json").get<std::vector<std::string>>());
    CharacterCore c = api.character_core("Test Character");
    CHECK_EQ(f.calls.back().first, kBase + "/v2/characters/Test%20Character/core");
    json want = fixture("core.json");
    CHECK_EQ(c.name, std::string("Test Character"));
    CHECK_EQ(c.race, want["race"].get<std::string>());
    CHECK_EQ(c.gender, want["gender"].get<std::string>());
    CHECK_EQ(c.profession, want["profession"].get<std::string>());
    CHECK_EQ(c.level, want["level"].get<int>());
}

CM_TEST(api, equipment_tabs_parse) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    std::vector<EquipmentTab> tabs = api.equipment_tabs("Test Character");
    json want = fixture("equipmenttabs.json");
    CHECK_EQ(tabs.size(), want.size());
    for (size_t t = 0; t < tabs.size(); ++t) {
        CHECK_EQ(tabs[t].tab, want[t]["tab"].get<int>());
        CHECK_EQ(tabs[t].is_active, want[t]["is_active"].get<bool>());
        const json& weq = want[t]["equipment"];
        CHECK_EQ(tabs[t].equipment.size(), weq.size());
        for (size_t i = 0; i < weq.size(); ++i) {
            const EquipmentEntry& e = tabs[t].equipment[i];
            CHECK_EQ(e.item_id, weq[i]["id"].get<uint32_t>());
            CHECK_EQ(e.slot, weq[i]["slot"].get<std::string>());
            CHECK_EQ(e.skin.has_value(), weq[i].contains("skin"));
            if (e.skin) CHECK_EQ(*e.skin, weq[i]["skin"].get<uint32_t>());
            for (size_t d = 0; d < 4; ++d) {
                bool set = weq[i].contains("dyes") && d < weq[i]["dyes"].size() && !weq[i]["dyes"][d].is_null();
                CHECK_EQ(e.dyes[d].has_value(), set);
                if (set) CHECK_EQ(*e.dyes[d], weq[i]["dyes"][d].get<uint32_t>());
            }
        }
    }
}

CM_TEST(api, items_skins_colors_parse) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    json wi = fixture("items.json"), ws = fixture("skins.json"), wc = fixture("colors.json");
    auto items = api.items(ids_of(wi));
    auto skins = api.skins(ids_of(ws));
    auto colors = api.colors(ids_of(wc));
    for (uint32_t id : ids_of(wi)) CHECK(items.count(id) == 1);
    for (uint32_t id : ids_of(ws)) CHECK(skins.count(id) == 1);
    for (uint32_t id : ids_of(wc)) CHECK(colors.count(id) == 1);

    // Skin 517 (a coat) has four default dye slots and Sylvari overrides.
    json s517;
    for (const json& s : ws) if (s["id"] == 517) s517 = s;
    const ApiSkin& sk = skins.at(517);
    const json& def = s517["details"]["dye_slots"]["default"];
    CHECK_EQ(sk.dye_default.size(), def.size());
    for (size_t i = 0; i < def.size(); ++i) {
        CHECK_EQ(sk.dye_default[i].has_value(), !def[i].is_null());
        if (sk.dye_default[i]) CHECK_EQ(sk.dye_default[i]->material, def[i]["material"].get<std::string>());
    }
    CHECK(sk.dye_overrides.count("SylvariFemale") == 1);
    CHECK_EQ(sk.type, std::string("Armor"));

    // Color 1 (Dye Remover) cloth rgb.
    json c1;
    for (const json& c : wc) if (c["id"] == 1) c1 = c;
    auto rgb = colors.at(1).rgb.at("cloth");
    CHECK_EQ(int(rgb[0]), c1["cloth"]["rgb"][0].get<int>());
    CHECK_EQ(int(rgb[1]), c1["cloth"]["rgb"][1].get<int>());
    CHECK_EQ(int(rgb[2]), c1["cloth"]["rgb"][2].get<int>());

    // Items carry their default skin when they have one.
    for (const json& i : wi) {
        CHECK_EQ(items.at(i["id"].get<uint32_t>()).default_skin.has_value(), i.contains("default_skin"));
    }
}

CM_TEST(api, batch_chunks_at_200) {
    FakeHttpClient f;
    f.routes.clear();
    std::vector<uint32_t> ids;
    for (uint32_t i = 1; i <= 450; ++i) ids.push_back(i);
    Gw2Api api(f, "KEY");
    for (size_t at = 0; at < ids.size(); at += 200) {
        std::vector<uint32_t> chunk(ids.begin() + at, ids.begin() + std::min(ids.size(), at + 200));
        f.routes[kBase + "/v2/items?ids=" + join(chunk)] = {200, "[]", ""};
    }
    api.items(ids);
    CHECK_EQ(f.calls.size(), size_t{3});
    CHECK(f.calls[2].first.find("ids=401,") != std::string::npos);
}

CM_TEST(api, batch_206_is_success) {
    FakeHttpClient f;
    f.routes[kBase + "/v2/items?ids=1,2"] = {206, R"([{"id":1,"name":"One","type":"Trophy"}])", ""};
    Gw2Api api(f, "KEY");
    auto items = api.items({1, 2});
    CHECK_EQ(items.size(), size_t{1});
    CHECK_EQ(items.at(1).name, std::string("One"));
}

CM_TEST(api, empty_batch_makes_no_request) {
    FakeHttpClient f;
    Gw2Api api(f, "KEY");
    CHECK(api.colors({}).empty());
    CHECK(f.calls.empty());
}

CM_TEST(api, http_401_message_has_no_key) {
    FakeHttpClient f;
    f.routes[kBase + "/v2/tokeninfo"] = {401, R"({"text":"Invalid access token"})", ""};
    Gw2Api api(f, "KEY");
    bool threw = false;
    try {
        api.token_info();
    } catch (const ApiError& e) {
        threw = true;
        CHECK_EQ(e.status(), 401);
        std::string m = e.what();
        CHECK(m.find("API key rejected") != std::string::npos);
        CHECK(m.find("KEY") == std::string::npos);
    }
    CHECK(threw);
}

CM_TEST(api, transport_failure_message) {
    struct Down : HttpClient {
        HttpResponse get(const std::string&, const Headers&) override { return {0, "", "timeout"}; }
    } down;
    Gw2Api api(down, "KEY");
    bool threw = false;
    try {
        api.character_names();
    } catch (const ApiError& e) {
        threw = true;
        CHECK_EQ(e.status(), 0);
        CHECK_EQ(std::string(e.what()).rfind("Could not reach api.guildwars2.com", 0), size_t{0});
    }
    CHECK(threw);
}

CM_TEST(api, malformed_json_is_api_error) {
    FakeHttpClient f;
    f.routes[kBase + "/v2/characters"] = {200, "<html>", ""};
    Gw2Api api(f, "KEY");
    bool threw = false;
    try {
        api.character_names();
    } catch (const ApiError& e) {
        threw = true;
        CHECK(std::string(e.what()).find("malformed JSON for /v2/characters") != std::string::npos);
    }
    CHECK(threw);
}

CM_TEST(api, colors_keep_shift) {
    FakeHttpClient f = fixture_client();
    Gw2Api api(f, "KEY");
    auto colors = api.colors(ids_of(fixture("colors.json")));
    const DyeShift& cloth = colors.at(1).shift.at("cloth");  // Dye Remover
    CHECK_NEAR(cloth.brightness, 15.0, 1e-6);
    CHECK_NEAR(cloth.contrast, 1.25, 1e-6);
    CHECK_NEAR(cloth.hue, 38.0, 1e-6);
    CHECK_NEAR(cloth.saturation, 0.28125, 1e-6);
    CHECK_NEAR(cloth.lightness, 1.44531, 1e-6);
}

CM_TEST(gw2api, names_come_from_a_public_endpoint_without_a_key) {
    FakeHttpClient f;
    f.routes[kBase + "/v2/mounts/skins?ids=292,9999"] = {
        206, R"([{"id":292,"name":"Dark Monarch Skyscale","icon":"x"}])", ""};
    Gw2Api api(f, "");
    std::map<uint32_t, std::string> n = api.names("mounts/skins", {292, 9999});
    CHECK_EQ(n.size(), size_t{1});
    CHECK_EQ(n[292], std::string("Dark Monarch Skyscale"));
    CHECK_EQ(f.calls.size(), size_t{1});
    if (!f.calls.empty())
        for (const auto& [k, v] : f.calls[0].second) CHECK(k != "Authorization");
}

CM_TEST(gw2api, names_batch_two_hundred_ids_per_request) {
    FakeHttpClient f;
    f.prefix_routes[kBase + "/v2/items?ids="] = {200, "[]", ""};
    Gw2Api api(f, "");
    std::vector<uint32_t> ids;
    for (uint32_t i = 1; i <= 201; ++i) ids.push_back(i);
    api.names("items", ids);
    CHECK_EQ(f.calls.size(), size_t{2});
}

CM_TEST(gw2api, names_of_only_unknown_ids_are_empty_not_an_error) {
    FakeHttpClient f;  // every route 404s
    Gw2Api api(f, "");
    CHECK(api.names("skins", {1, 2}).empty());
}
