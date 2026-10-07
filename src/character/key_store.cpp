#include "castlemist/character/key_store.h"

#include <algorithm>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace castlemist::character {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

bool read_file(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

bool KeyStore::load(const fs::path& file, std::string* error) {
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        keys_.clear();
        return true;
    }
    std::string text;
    if (!read_file(file, text)) {
        if (error) *error = "cannot read " + file.string();
        return false;
    }
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("keys") || !j["keys"].is_array()) {
        if (error) *error = file.string() + " is not a valid key file (expected {\"keys\":[...]})";
        return false;
    }
    std::vector<ApiKey> parsed;
    for (const json& k : j["keys"]) {
        if (!k.is_object() || !k.contains("name") || !k["name"].is_string() || !k.contains("key") ||
            !k["key"].is_string()) {
            if (error) *error = file.string() + ": every key needs a string \"name\" and \"key\"";
            return false;
        }
        parsed.push_back({k["name"].get<std::string>(), k["key"].get<std::string>()});
    }
    keys_ = std::move(parsed);
    return true;
}

bool KeyStore::save(const fs::path& file, std::string* error) const {
    json arr = json::array();
    for (const ApiKey& k : keys_) arr.push_back({{"name", k.name}, {"key", k.key}});
    json j = {{"keys", arr}};
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + file.string();
        return false;
    }
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

void KeyStore::add(const std::string& name, const std::string& key) {
    for (ApiKey& k : keys_) {
        if (k.name == name) { k.key = key; return; }
    }
    keys_.push_back({name, key});
}

bool KeyStore::rename(const std::string& from, const std::string& to) {
    if (get(to)) return false;
    for (ApiKey& k : keys_) {
        if (k.name == from) { k.name = to; return true; }
    }
    return false;
}

bool KeyStore::remove(const std::string& name) {
    auto it = std::find_if(keys_.begin(), keys_.end(), [&](const ApiKey& k) { return k.name == name; });
    if (it == keys_.end()) return false;
    keys_.erase(it);
    return true;
}

const ApiKey* KeyStore::get(const std::string& name) const {
    for (const ApiKey& k : keys_) {
        if (k.name == name) return &k;
    }
    return nullptr;
}

fs::path find_castlemist_root(const fs::path& start_dir) {
    for (fs::path d = start_dir; !d.empty(); d = d.parent_path()) {
        std::string text;
        if (read_file(d / "CMakeLists.txt", text) && text.find("project(castlemist") != std::string::npos)
            return d;
        if (d == d.parent_path()) break;  // filesystem root
    }
    return start_dir;
}

fs::path default_key_file() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return find_castlemist_root(fs::path(exe).parent_path()) / "api_keys.json";
}

} // namespace castlemist::character
