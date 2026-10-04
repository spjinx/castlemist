#include "castlemist/character/manifest_json.h"

#include <cstdio>
#include <stdexcept>

namespace castlemist::character {
using nlohmann::json;

namespace {

std::string to_hex(const std::array<uint8_t, 3>& rgb) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", rgb[0], rgb[1], rgb[2]);
    return buf;
}

std::array<uint8_t, 3> from_hex(const std::string& s) {
    std::array<uint8_t, 3> rgb{};
    unsigned r = 0, g = 0, b = 0;
    if (s.size() == 7 && s[0] == '#' && std::sscanf(s.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3)
        rgb = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
    return rgb;
}

} // namespace

json manifest_to_json(const CharacterManifest& m) {
    json pieces = json::array();
    for (const ManifestPiece& p : m.pieces) {
        json dyes = json::array();
        for (const ManifestDye& d : p.dyes) {
            dyes.push_back({{"color_id", d.color_id},
                            {"color_name", d.color_name},
                            {"material", d.material},
                            {"rgb", to_hex(d.rgb)},
                            {"known", d.known}});
        }
        pieces.push_back({{"slot", p.slot},
                          {"item_id", p.item_id},
                          {"item_name", p.item_name},
                          {"skin_id", p.skin_id},
                          {"skin_name", p.skin_name},
                          {"weight_class", p.weight_class},
                          {"file_ids", p.file_ids},
                          {"status", to_string(p.status)},
                          {"dyes", dyes}});
    }
    return {{"version", 1},
            {"name", m.name},
            {"race", m.race},
            {"gender", m.gender},
            {"profession", m.profession},
            {"level", m.level},
            {"tab", {{"id", m.tab_id}, {"name", m.tab_name}}},
            {"pieces", pieces}};
}

CharacterManifest manifest_from_json(const json& j) {
    if (!j.is_object() || j.value("version", 0) != 1) throw std::runtime_error("unsupported manifest version");
    try {
        CharacterManifest m;
        m.name = j.at("name").get<std::string>();
        m.race = j.at("race").get<std::string>();
        m.gender = j.at("gender").get<std::string>();
        m.profession = j.at("profession").get<std::string>();
        m.level = j.at("level").get<int>();
        m.tab_id = j.at("tab").at("id").get<int>();
        m.tab_name = j.at("tab").at("name").get<std::string>();
        for (const json& pj : j.at("pieces")) {
            ManifestPiece p;
            p.slot = pj.at("slot").get<std::string>();
            p.item_id = pj.at("item_id").get<uint32_t>();
            p.item_name = pj.at("item_name").get<std::string>();
            p.skin_id = pj.at("skin_id").get<uint32_t>();
            p.skin_name = pj.at("skin_name").get<std::string>();
            p.weight_class = pj.at("weight_class").get<std::string>();
            p.file_ids = pj.at("file_ids").get<std::vector<uint32_t>>();
            auto st = piece_status_from_string(pj.at("status").get<std::string>());
            if (!st) throw std::runtime_error("unknown piece status");
            p.status = *st;
            for (const json& dj : pj.at("dyes")) {
                ManifestDye d;
                d.color_id = dj.at("color_id").get<uint32_t>();
                d.color_name = dj.at("color_name").get<std::string>();
                d.material = dj.at("material").get<std::string>();
                d.rgb = from_hex(dj.at("rgb").get<std::string>());
                d.known = dj.at("known").get<bool>();
                p.dyes.push_back(std::move(d));
            }
            m.pieces.push_back(std::move(p));
        }
        return m;
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("malformed manifest: ") + e.what());
    }
}

} // namespace castlemist::character
