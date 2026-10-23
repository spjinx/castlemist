#include "castlemist/skillfx/export.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace castlemist::skillfx {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string hex(const Guid& g) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : g) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

void write_json(const fs::path& p, const json& j) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << j.dump(2);
}

}  // namespace

std::string slug(const std::string& label) {
    std::string out;
    bool dash = false;
    for (char c : label) {
        const char l = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) {
            if (dash && !out.empty()) out += '-';
            out += l;
            dash = false;
        } else {
            dash = true;
        }
    }
    return out.empty() ? "skill" : out;
}

void apply_pairings(SkillSet& set, const std::vector<Pairing>& pairs,
                    const std::map<Guid, std::pair<size_t, const cntc::Object*>>& index, const cntc::PackSet& s,
                    const ContainerOf& container, const std::map<uint32_t, std::string>& api_names,
                    const std::map<Guid, std::string>& ei) {
    std::map<uint32_t, std::string> log_names;
    std::set<Guid> paired;
    for (const Pairing& p : pairs) {
        if (!p.skill_name.empty()) log_names.emplace(p.skill_id, p.skill_name);
        auto entry = std::find_if(set.skills.begin(), set.skills.end(), [&](const SkillEntry& e) { return e.id == p.skill_id; });
        if (entry == set.skills.end()) continue;
        auto at = index.find(p.effect);
        if (at == index.end()) continue;
        paired.insert(p.effect);
        auto& fx = entry->links.effects;
        auto have = std::find_if(fx.begin(), fx.end(), [&](const Effect& f) { return f.guid == p.effect; });
        if (have != fx.end()) {  // linked in content and seen in the logs
            have->count = std::max(have->count, p.count);
            continue;
        }
        Effect f = read_effect(s, at->second.first, *at->second.second, container);
        f.source = "arcdps";
        f.count = p.count;
        fx.push_back(std::move(f));
    }
    std::erase_if(set.unlinked, [&](const Effect& f) { return paired.count(f.guid) > 0; });

    auto name_effects = [&](std::vector<Effect>& v) {
        for (Effect& f : v)
            if (auto n = ei.find(f.guid); n != ei.end()) f.ei_name = n->second;
    };
    for (SkillEntry& e : set.skills) {
        name_effects(e.links.effects);
        if (auto n = api_names.find(e.id); n != api_names.end() && !n->second.empty()) e.label = n->second;
        else if (auto m = log_names.find(e.id); m != log_names.end()) e.label = m->second;
        else if (e.bundle_slot >= 0) e.label = std::to_string(e.bundle_item) + " slot " + std::to_string(e.bundle_slot + 1);
        else e.label = e.codename;
    }
    name_effects(set.unlinked);
}

ExportReport export_skill_set(const SkillSet& set, const fs::path& out, const ModelExporter& exporter) {
    ExportReport rep;
    // Each model once, in a world-model folder models/<id>/ ({folder, error}).
    std::map<uint32_t, std::pair<std::string, std::string>> done;
    auto export_model = [&](uint32_t model) {
        auto it = done.find(model);
        if (it != done.end()) return it->second;
        const std::string rel = "models/" + std::to_string(model);
        std::string err = exporter(model, out / fs::path(rel));
        if (err.empty()) ++rep.models;
        else {
            ++rep.failed;
            rep.notes.push_back("model " + std::to_string(model) + ": " + err);
        }
        return done[model] = {err.empty() ? rel : std::string(), err};
    };
    auto effect_json = [&](const Effect& f) {
        json j{{"field", f.field}, {"kind", f.kind},   {"codename", f.codename}, {"model", f.model},
               {"sound", f.sound}, {"guid", hex(f.guid)}, {"source", f.source},     {"ei_name", f.ei_name},
               {"count", f.count}, {"folder", ""},     {"glb", ""}};
        if (f.model) {
            auto [folder, err] = export_model(f.model);
            j["folder"] = folder;
            if (!folder.empty()) j["glb"] = folder + "/" + std::to_string(f.model) + ".glb";
            if (!err.empty()) j["error"] = err;
        }
        return j;
    };

    json index{{"skill_namespace", set.skill_namespace}, {"effect_namespace", set.effect_namespace},
               {"skills", json::array()}};
    for (const SkillEntry& e : set.skills) {
        const std::string folder = "skills/" + std::to_string(e.id) + "-" + slug(e.label.empty() ? e.codename : e.label);
        json fx = json::array();
        for (const Effect& f : e.links.effects) fx.push_back(effect_json(f));
        json sj{{"id", e.id},           {"label", e.label},         {"codename", e.codename},
                {"bundle_item", e.bundle_item}, {"bundle_slot", e.bundle_slot}, {"effects", fx},
                {"related_skills", e.links.related_skills}, {"applies", e.links.applies}};
        write_json(out / folder / "skill.json", sj);
        index["skills"].push_back({{"id", e.id}, {"label", e.label}, {"folder", folder}});
        ++rep.skills;
    }
    json ul = json::array();
    for (const Effect& f : set.unlinked) ul.push_back(effect_json(f));
    write_json(out / "unlinked" / "unlinked.json", json{{"effects", ul}});
    write_json(out / "skill_set.json", index);
    return rep;
}

}  // namespace castlemist::skillfx
