#include "castlemist/npc/catalogue.h"

#include "character.h"
#include "decompress.h"
#include "pack.h"

#include "castlemist/native/cmp_decompress_method0.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <span>

namespace castlemist::npc {

namespace detail {

std::vector<uint8_t> decompress_cntc(std::span<const uint8_t> raw, bool compressed) {
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(raw);
    if (!compressed) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | (static_cast<uint32_t>(s[7]) << 24);
    try {
        return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
    } catch (const std::exception&) {
        return {};  // corrupt stream: the caller records "unreadable" and continues
    }
}

}  // namespace detail

namespace {

namespace cmap = castlemist::cmap;

constexpr uint32_t kCharacterType = 67;
constexpr uint32_t kMinipetType = 48;

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
            // Keep the slot: cross-pack refs count packs in file_id order, so dropping one would
            // shift every later target.
            if (p.file_id != 0) {
                detail::Pack placeholder;
                placeholder.base_id = p.base_id;
                placeholder.file_id = p.file_id;
                set.packs.push_back(std::move(placeholder));
            }
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
            for (auto& [race, token] : f.looks) r.looks.push_back({std::move(race), token});
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

    // Minipets: each type-48 object refs its character; keep the lowest minipet dataId per character.
    std::map<uint32_t, uint32_t> minipet_of;
    for (size_t pi = 0; pi < set.packs.size(); ++pi) {
        for (const detail::Object& obj : set.packs[pi].objects) {
            if (obj.type != kMinipetType || obj.data_id == 0) continue;
            for (const detail::Ref& r : obj.refs) {
                auto [tp, to] = set.resolve(pi, r);
                if (!tp || !to || to->type != kCharacterType) continue;
                auto [it, fresh] = minipet_of.emplace(to->data_id, obj.data_id);
                if (!fresh && obj.data_id < it->second) it->second = obj.data_id;
            }
        }
    }
    for (NpcRecord& n : cat.npcs) {
        auto it = minipet_of.find(n.id);
        if (it != minipet_of.end()) n.minipet = it->second;
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
        std::vector<uint8_t> raw = read_entry_bytes(dat_path, entries[i]);  // open failures propagate
        p.bytes = detail::decompress_cntc(raw, entries[i].compression_flag != 0);
        packs.push_back(std::move(p));
        if (progress) progress(i + 1, total);
    }
    return build_catalogue(packs, container_of);
}

std::vector<uint32_t> minipet_ids(const Catalogue& c) {
    std::set<uint32_t> ids;
    for (const NpcRecord& n : c.npcs)
        if (n.minipet != 0) ids.insert(n.minipet);
    return {ids.begin(), ids.end()};
}

size_t apply_names(Catalogue& c, const std::map<uint32_t, std::string>& minipet_names) {
    size_t set = 0;
    for (NpcRecord& n : c.npcs) {
        auto it = minipet_names.find(n.minipet);
        if (n.minipet == 0 || it == minipet_names.end()) continue;
        n.name = it->second;
        ++set;
    }
    return set;
}

bool save(const Catalogue& c, const std::filesystem::path& path) {
    using nlohmann::json;
    json npcs = json::array();
    for (const NpcRecord& n : c.npcs) {
        json weapons = json::array();
        for (const Weapon& w : n.weapons)
            weapons.push_back({{"slot", w.slot}, {"item", w.item}, {"skin", w.skin}, {"model", w.model}});
        json looks = json::array();
        for (const NpcLook& l : n.looks)
            looks.push_back({{"race", l.race}, {"look_token", std::to_string(l.look_token)}});
        npcs.push_back({{"id", n.id},
                        {"codename", n.codename},
                        {"model", n.model},
                        {"sound_script", n.sound_script},
                        {"race", n.race},
                        {"modifier", n.modifier},
                        {"weapons", weapons},
                        {"skills", n.skills},
                        {"looks", looks},
                        {"minipet", n.minipet},
                        {"name", n.name}});
    }
    json doc = {{"version", 3}, {"warnings", c.warnings}, {"npcs", npcs}};
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
        if (!doc.contains("version") || doc["version"] != 3) return std::nullopt;
        Catalogue c;
        if (doc.contains("warnings")) c.warnings = doc["warnings"].get<std::vector<std::string>>();
        if (!doc.contains("npcs") || !doc["npcs"].is_array()) return std::nullopt;
        {
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
                for (const json& l : j.at("looks"))
                    n.looks.push_back({l.at("race").get<std::string>(), std::stoull(l.at("look_token").get<std::string>())});
                n.minipet = j.at("minipet").get<uint32_t>();
                n.name = j.at("name").get<std::string>();
                c.npcs.push_back(std::move(n));
            }
        }
        return c;
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace castlemist::npc
