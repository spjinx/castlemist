#include "castlemist/npc/export.h"

#include "castlemist/npc/tokens.h"

namespace castlemist::npc {

std::string export_folder_name(const NpcRecord& r) {
    std::string name = std::to_string(r.id);
    if (r.codename.empty()) return name;
    name += '_';
    for (unsigned char c : r.codename) {
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '_' || c == '-';
        name += ok ? static_cast<char>(c) : '_';
    }
    return name;
}

nlohmann::json export_manifest(const NpcRecord& r, const std::vector<ClipInfo>& clips,
                               const std::vector<WrittenWeapon>& weapons,
                               const std::vector<std::string>& files, const std::vector<std::string>& notes) {
    using nlohmann::json;
    json j;
    j["version"] = 1;
    j["id"] = r.id;
    j["codename"] = r.codename;
    j["model"] = r.model;
    j["sound_script"] = r.sound_script;
    j["race"] = r.race;
    j["modifier"] = r.modifier;
    j["clips"] = json::array();
    for (const ClipInfo& c : clips)
        j["clips"].push_back({{"name", c.name},
                              {"group", clip_group(c.name)},
                              {"prefix", clip_prefix(c.name)},
                              {"duration", c.duration}});
    j["weapons"] = json::array();
    for (const WrittenWeapon& w : weapons)
        j["weapons"].push_back({{"slot", w.weapon.slot},
                                {"item", w.weapon.item},
                                {"skin", w.weapon.skin},
                                {"model", w.weapon.model},
                                {"file", w.file}});
    j["skills"] = r.skills;
    j["files"] = files;
    j["notes"] = notes;
    return j;
}

std::vector<std::string> base_notes(const NpcRecord& r) {
    std::vector<std::string> notes;
    if (r.model == 0)
        notes.push_back(r.race.empty() ? "no body model"
                                       : "no body model: race-body NPC (" + r.race + "), outfit data not decoded");
    for (const Weapon& w : r.weapons)
        if (w.model == 0) notes.push_back("weapon " + std::to_string(w.slot) + ": no skin model");
    return notes;
}

}  // namespace castlemist::npc
