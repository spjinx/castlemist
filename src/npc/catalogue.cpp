#include "castlemist/npc/catalogue.h"

#include "character.h"
#include "pack.h"

#include "castlemist/native/cmp_decompress_method0.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <span>

namespace castlemist::npc {

namespace {

namespace cmap = castlemist::cmap;

/// Decompresses one MFT entry to raw file bytes, the way cmap::build does.
std::vector<uint8_t> decompress_entry(const std::string& dat_path, const MftData& e) {
    std::vector<uint8_t> raw = read_entry_bytes(dat_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | (static_cast<uint32_t>(s[7]) << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

constexpr uint32_t kCharacterType = 67;

uint32_t first_in_container(const std::vector<uint32_t>& ids, const ContainerOf& container_of, const char* want) {
    for (uint32_t fid : ids)
        if (container_of(fid) == want) return fid;
    return 0;
}

}  // namespace

Coverage Catalogue::coverage() const {
    Coverage c;
    c.npcs = npcs.size();
    for (const NpcRecord& n : npcs) {
        if (n.model != 0) ++c.with_model;
        if (!n.race.empty()) ++c.with_race;
        if (!n.weapons.empty()) ++c.with_weapons;
        if (!n.skills.empty()) ++c.with_skills;
    }
    return c;
}

const NpcRecord* Catalogue::find(uint32_t id) const {
    auto it = std::lower_bound(npcs.begin(), npcs.end(), id,
                               [](const NpcRecord& n, uint32_t v) { return n.id < v; });
    return it != npcs.end() && it->id == id ? &*it : nullptr;
}

std::vector<const NpcRecord*> Catalogue::by_model(uint32_t model) const {
    std::vector<const NpcRecord*> out;
    for (const NpcRecord& n : npcs)
        if (n.model == model) out.push_back(&n);
    return out;
}

Catalogue build_catalogue(const std::vector<cmap::PackBytes>& packs, const ContainerOf& container_of) {
    Catalogue cat;
    if (!cmap::built()) {
        cat.warnings.push_back("content map not loaded");
        return cat;
    }

    detail::PackSet set;
    for (const cmap::PackBytes& p : packs) {
        std::optional<detail::Pack> parsed;
        if (!p.bytes.empty()) parsed = detail::parse_pack(p.bytes, p.base_id, p.file_id);
        if (!parsed) {
            cat.warnings.push_back("pack " + std::to_string(p.base_id) + ": unreadable");
            continue;
        }
        set.packs.push_back(std::move(*parsed));
    }
    set.finish();

    for (size_t pi = 0; pi < set.packs.size(); ++pi) {
        for (const detail::Object& obj : set.packs[pi].objects) {
            if (obj.type != kCharacterType) continue;
            detail::CharacterFields f = detail::read_character(set, pi, obj);
            NpcRecord r;
            r.id = f.id;
            r.codename = std::move(f.codename);
            r.race = std::move(f.race);
            r.modifier = std::move(f.modifier);
            r.skills = std::move(f.skills);
            const std::vector<uint32_t>& files = cmap::resolve_all(kCharacterType, f.id);
            r.model = first_in_container(files, container_of, "MODL");
            r.sound_script = first_in_container(files, container_of, "AMSP");
            for (const auto& [slot, item] : f.weapons) {
                Weapon w;
                w.slot = slot;
                w.item = item;
                for (const cmap::ContentLink& l : cmap::item_links(item)) {
                    if (l.type != cmap::CONTENT_TYPE_SKIN) continue;
                    w.skin = l.id;
                    w.model = cmap::resolve(cmap::CONTENT_TYPE_SKIN, l.id);
                    break;
                }
                r.weapons.push_back(w);
            }
            cat.npcs.push_back(std::move(r));
        }
    }

    std::stable_sort(cat.npcs.begin(), cat.npcs.end(),
                     [](const NpcRecord& a, const NpcRecord& b) { return a.id < b.id; });
    std::vector<NpcRecord> unique;
    unique.reserve(cat.npcs.size());
    for (NpcRecord& n : cat.npcs) {
        if (!unique.empty() && unique.back().id == n.id) {
            cat.warnings.push_back("duplicate npc id " + std::to_string(n.id));
            continue;
        }
        unique.push_back(std::move(n));
    }
    cat.npcs = std::move(unique);
    return cat;
}

Catalogue build_catalogue(const std::string& dat_path, const std::vector<MftData>& entries,
                          const std::vector<uint32_t>& file_ids, const ContainerOf& container_of,
                          const std::function<void(size_t, size_t)>& progress) {
    std::vector<cmap::PackBytes> packs;
    packs.reserve(entries.size());
    const size_t total = entries.size();
    for (size_t i = 0; i < total; ++i) {
        cmap::PackBytes p;
        p.base_id = static_cast<uint32_t>(entries[i].original_index) + 1;
        p.file_id = i < file_ids.size() ? file_ids[i] : 0;
        p.bytes = decompress_entry(dat_path, entries[i]);
        packs.push_back(std::move(p));
        if (progress) progress(i + 1, total);
    }
    return build_catalogue(packs, container_of);
}

bool save(const Catalogue& c, const std::filesystem::path& path) {
    using nlohmann::json;
    json npcs = json::array();
    for (const NpcRecord& n : c.npcs) {
        json weapons = json::array();
        for (const Weapon& w : n.weapons)
            weapons.push_back({{"slot", w.slot}, {"item", w.item}, {"skin", w.skin}, {"model", w.model}});
        npcs.push_back({{"id", n.id},
                        {"codename", n.codename},
                        {"model", n.model},
                        {"sound_script", n.sound_script},
                        {"race", n.race},
                        {"modifier", n.modifier},
                        {"weapons", weapons},
                        {"skills", n.skills}});
    }
    json doc = {{"version", 1}, {"warnings", c.warnings}, {"npcs", npcs}};
    try {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << doc.dump();
        return static_cast<bool>(out);
    } catch (...) {
        return false;
    }
}

std::optional<Catalogue> load(const std::filesystem::path& path) {
    using nlohmann::json;
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        json doc = json::parse(in, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) return std::nullopt;
        if (!doc.contains("version") || doc["version"] != 1) return std::nullopt;
        Catalogue c;
        if (doc.contains("warnings")) c.warnings = doc["warnings"].get<std::vector<std::string>>();
        if (doc.contains("npcs")) {
            for (const json& j : doc["npcs"]) {
                NpcRecord n;
                n.id = j.at("id").get<uint32_t>();
                n.codename = j.at("codename").get<std::string>();
                n.model = j.at("model").get<uint32_t>();
                n.sound_script = j.at("sound_script").get<uint32_t>();
                n.race = j.at("race").get<std::string>();
                n.modifier = j.at("modifier").get<std::string>();
                for (const json& w : j.at("weapons"))
                    n.weapons.push_back({w.at("slot").get<uint32_t>(), w.at("item").get<uint32_t>(),
                                         w.at("skin").get<uint32_t>(), w.at("model").get<uint32_t>()});
                n.skills = j.at("skills").get<std::vector<uint32_t>>();
                c.npcs.push_back(std::move(n));
            }
        }
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace castlemist::npc
