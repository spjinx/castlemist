#include "castlemist/character/gw2_api.h"

#include <algorithm>
#include <functional>

#include <nlohmann/json.hpp>

namespace castlemist::character {
using nlohmann::json;

namespace {

constexpr size_t kBatch = 200;

json parse_or_throw(const std::string& body, int status, const std::string& path) {
    json j = json::parse(body, nullptr, false);
    if (j.is_discarded()) {
        std::string p = path.substr(0, path.find('?'));
        throw ApiError(status, "GW2 API returned malformed JSON for " + p);
    }
    return j;
}

std::string str(const json& o, const char* k) {
    return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
}

std::optional<uint32_t> opt_u32(const json& o, const char* k) {
    if (o.contains(k) && o[k].is_number_unsigned()) return o[k].get<uint32_t>();
    return std::nullopt;
}

std::vector<uint32_t> u32_list(const json& o, const char* k) {
    std::vector<uint32_t> v;
    if (o.contains(k) && o[k].is_array())
        for (const json& x : o[k]) if (x.is_number_unsigned()) v.push_back(x.get<uint32_t>());
    return v;
}

DyeSlots parse_dye_slots(const json& arr) {
    DyeSlots out;
    if (!arr.is_array()) return out;
    for (const json& s : arr) {
        if (s.is_object()) out.push_back(DyeSlot{s.value("color_id", 0u), str(s, "material")});
        else out.push_back(std::nullopt);
    }
    return out;
}

} // namespace

std::vector<std::string> missing_scopes(const TokenInfo& t) {
    std::vector<std::string> out;
    for (const char* s : {"account", "characters", "builds"}) {
        if (std::find(t.permissions.begin(), t.permissions.end(), s) == t.permissions.end()) out.push_back(s);
    }
    return out;
}

std::string Gw2Api::get_json(const std::string& path) {
    HttpResponse r = http_.get(std::string(kApiBase) + path,
                               {{"Authorization", "Bearer " + key_}, {"X-Schema-Version", "latest"}});
    if (r.status == 0) throw ApiError(0, "Could not reach api.guildwars2.com: " + r.error);
    if (r.status == 401 || r.status == 403)
        throw ApiError(r.status, "API key rejected (HTTP " + std::to_string(r.status) +
                                     ") - check the key on account.arena.net");
    if (r.status < 200 || r.status > 299) {
        json j = json::parse(r.body, nullptr, false);
        std::string text = !j.is_discarded() && j.is_object() ? str(j, "text") : std::string();
        throw ApiError(r.status, "GW2 API error (HTTP " + std::to_string(r.status) + ")" +
                                     (text.empty() ? "" : ": " + text));
    }
    return r.body;
}

TokenInfo Gw2Api::token_info() {
    const std::string path = "/v2/tokeninfo";
    json j = parse_or_throw(get_json(path), 200, path);
    TokenInfo t;
    t.name = str(j, "name");
    if (j.contains("permissions"))
        for (const json& p : j["permissions"]) if (p.is_string()) t.permissions.push_back(p.get<std::string>());
    return t;
}

std::vector<std::string> Gw2Api::character_names() {
    const std::string path = "/v2/characters";
    json j = parse_or_throw(get_json(path), 200, path);
    std::vector<std::string> out;
    if (j.is_array())
        for (const json& n : j) if (n.is_string()) out.push_back(n.get<std::string>());
    return out;
}

CharacterCore Gw2Api::character_core(const std::string& name) {
    const std::string path = "/v2/characters/" + url_encode(name) + "/core";
    json j = parse_or_throw(get_json(path), 200, path);
    CharacterCore c;
    c.name = str(j, "name");
    c.race = str(j, "race");
    c.gender = str(j, "gender");
    c.profession = str(j, "profession");
    c.level = j.value("level", 0);
    return c;
}

std::vector<EquipmentTab> Gw2Api::equipment_tabs(const std::string& name) {
    const std::string path = "/v2/characters/" + url_encode(name) + "/equipmenttabs?tabs=all";
    json j = parse_or_throw(get_json(path), 200, path);
    std::vector<EquipmentTab> out;
    if (!j.is_array()) return out;
    for (const json& t : j) {
        EquipmentTab tab;
        tab.tab = t.value("tab", 0);
        tab.name = str(t, "name");
        tab.is_active = t.value("is_active", false);
        if (t.contains("equipment") && t["equipment"].is_array()) {
            for (const json& e : t["equipment"]) {
                EquipmentEntry en;
                en.item_id = e.value("id", 0u);
                en.slot = str(e, "slot");
                en.skin = opt_u32(e, "skin");
                if (e.contains("dyes") && e["dyes"].is_array()) {
                    for (size_t d = 0; d < 4 && d < e["dyes"].size(); ++d)
                        if (e["dyes"][d].is_number_unsigned()) en.dyes[d] = e["dyes"][d].get<uint32_t>();
                }
                en.upgrades = u32_list(e, "upgrades");
                en.infusions = u32_list(e, "infusions");
                tab.equipment.push_back(std::move(en));
            }
        }
        out.push_back(std::move(tab));
    }
    return out;
}

namespace {

// Runs `endpoint?ids=` in 200-id chunks and hands each returned object to `add`.
void batched(const std::vector<uint32_t>& ids, const std::string& endpoint,
             const std::function<std::string(const std::string&)>& get,
             const std::function<void(const json&)>& add) {
    for (size_t at = 0; at < ids.size(); at += kBatch) {
        std::string path = "/v2/" + endpoint + "?ids=";
        for (size_t i = at; i < std::min(ids.size(), at + kBatch); ++i)
            path += (i == at ? "" : ",") + std::to_string(ids[i]);
        json j = parse_or_throw(get(path), 200, path);
        if (j.is_array())
            for (const json& o : j) if (o.is_object()) add(o);
    }
}

} // namespace

std::map<uint32_t, ApiItem> Gw2Api::items(const std::vector<uint32_t>& ids) {
    std::map<uint32_t, ApiItem> out;
    batched(ids, "items", [this](const std::string& p) { return get_json(p); }, [&](const json& o) {
        ApiItem it;
        it.id = o.value("id", 0u);
        it.name = str(o, "name");
        it.type = str(o, "type");
        it.default_skin = opt_u32(o, "default_skin");
        out[it.id] = std::move(it);
    });
    return out;
}

std::map<uint32_t, ApiSkin> Gw2Api::skins(const std::vector<uint32_t>& ids) {
    std::map<uint32_t, ApiSkin> out;
    batched(ids, "skins", [this](const std::string& p) { return get_json(p); }, [&](const json& o) {
        ApiSkin s;
        s.id = o.value("id", 0u);
        s.name = str(o, "name");
        s.type = str(o, "type");
        if (o.contains("details") && o["details"].is_object()) {
            const json& d = o["details"];
            s.weight_class = str(d, "weight_class");
            if (d.contains("dye_slots") && d["dye_slots"].is_object()) {
                const json& ds = d["dye_slots"];
                if (ds.contains("default")) s.dye_default = parse_dye_slots(ds["default"]);
                if (ds.contains("overrides") && ds["overrides"].is_object())
                    for (auto it = ds["overrides"].begin(); it != ds["overrides"].end(); ++it)
                        s.dye_overrides[it.key()] = parse_dye_slots(it.value());
            }
        }
        out[s.id] = std::move(s);
    });
    return out;
}

std::map<uint32_t, ApiColor> Gw2Api::colors(const std::vector<uint32_t>& ids) {
    std::map<uint32_t, ApiColor> out;
    batched(ids, "colors", [this](const std::string& p) { return get_json(p); }, [&](const json& o) {
        ApiColor c;
        c.id = o.value("id", 0u);
        c.name = str(o, "name");
        for (const char* m : {"cloth", "leather", "metal", "fur"}) {
            if (!o.contains(m) || !o[m].is_object() || !o[m].contains("rgb")) continue;
            const json& mo = o[m];
            c.shift[m] = DyeShift{mo.value("brightness", 0.0f), mo.value("contrast", 1.0f), mo.value("hue", 0.0f),
                                  mo.value("saturation", 1.0f), mo.value("lightness", 1.0f)};
            const json& rgb = o[m]["rgb"];
            if (rgb.is_array() && rgb.size() == 3)
                c.rgb[m] = {rgb[0].get<uint8_t>(), rgb[1].get<uint8_t>(), rgb[2].get<uint8_t>()};
        }
        out[c.id] = std::move(c);
    });
    return out;
}

} // namespace castlemist::character
