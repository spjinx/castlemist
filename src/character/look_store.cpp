#include "castlemist/character/look_store.h"

#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "castlemist/character/key_store.h"

namespace castlemist::character {
namespace fs = std::filesystem;
using nlohmann::json;

bool LookStore::load(const fs::path& file, std::string* error) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        looks_.clear();
        return true;
    }
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot read " + file.string();
        return false;
    }
    std::string text(std::istreambuf_iterator<char>(f), {});
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("looks") || !j["looks"].is_object()) {
        if (error) *error = file.string() + " is not a valid look file (expected {\"looks\":{...}})";
        return false;
    }
    std::map<std::string, CharacterLook> parsed;
    for (const auto& [name, l] : j["looks"].items()) {
        if (!l.is_object()) {
            if (error) *error = file.string() + ": the look of \"" + name + "\" is not an object";
            return false;
        }
        CharacterLook look;
        look.face = l.value("face", 0);
        look.hair = l.value("hair", 0);
        look.skin_color = l.value("skin_color", 0u);
        look.hair_color = l.value("hair_color", 0u);
        look.hair_color2 = l.value("hair_color2", 0u);
        look.ears = l.value("ears", 0);
        look.eye_color = l.value("eye_color", 0u);
        look.pattern = l.value("pattern", -1);
        look.pattern_color = l.value("pattern_color", 0u);
        look.glow_color = l.value("glow_color", 0u);
        look.glow_intensity = l.value("glow_intensity", 1.0f);
        parsed[name] = look;
    }
    looks_ = std::move(parsed);
    return true;
}

bool LookStore::save(const fs::path& file, std::string* error) const {
    json looks = json::object();
    for (const auto& [name, l] : looks_)
        looks[name] = {{"face", l.face},
                       {"hair", l.hair},
                       {"skin_color", l.skin_color},
                       {"hair_color", l.hair_color},
                       {"hair_color2", l.hair_color2},
                       {"ears", l.ears},
                       {"eye_color", l.eye_color},
                       {"pattern", l.pattern},
                       {"pattern_color", l.pattern_color},
                       {"glow_color", l.glow_color},
                       {"glow_intensity", l.glow_intensity}};
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + file.string();
        return false;
    }
    f << json{{"looks", looks}}.dump(2) << "\n";
    return static_cast<bool>(f);
}

std::optional<CharacterLook> LookStore::get(const std::string& character) const {
    auto it = looks_.find(character);
    if (it == looks_.end()) return std::nullopt;
    return it->second;
}

void LookStore::set(const std::string& character, const CharacterLook& look) { looks_[character] = look; }

bool LookStore::remove(const std::string& character) { return looks_.erase(character) > 0; }

fs::path default_look_file() { return default_key_file().parent_path() / "character_looks.json"; }

} // namespace castlemist::character
