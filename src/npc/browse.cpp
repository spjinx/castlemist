#include "castlemist/npc/browse.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace castlemist::npc {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains(const std::string& haystack, const std::string& lowered_needle) {
    return lower(haystack).find(lowered_needle) != std::string::npos;
}

}  // namespace

bool matches(const NpcRecord& npc, const std::string& filter, const SkillName& skill_name) {
    if (filter.empty()) return true;
    const std::string needle = lower(filter);
    if (contains(std::to_string(npc.id), needle) || contains(npc.codename, needle) || contains(npc.race, needle) || contains(npc.name, needle) ||
        contains(std::to_string(npc.model), needle))
        return true;
    if (skill_name) {
        for (uint32_t skill : npc.skills)
            if (contains(skill_name(skill), needle)) return true;
    }
    return false;
}

std::vector<Group> group_creatures(const Catalogue& catalogue, const std::string& filter, const SkillName& skill_name) {
    std::map<std::pair<uint32_t, std::string>, Group> by_key;
    for (const NpcRecord& npc : catalogue.npcs) {
        if (!matches(npc, filter, skill_name)) continue;
        auto key = std::make_pair(npc.model, npc.model != 0 ? std::string() : npc.race);
        Group& g = by_key[key];
        g.model = key.first;
        g.race = key.second;
        g.npcs.push_back(&npc);
    }
    std::vector<Group> groups;
    groups.reserve(by_key.size());
    for (auto& entry : by_key) groups.push_back(std::move(entry.second));
    for (Group& g : groups)
        std::sort(g.npcs.begin(), g.npcs.end(), [](const NpcRecord* a, const NpcRecord* b) { return a->id < b->id; });
    std::sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) {
        if (a.npcs.size() != b.npcs.size()) return a.npcs.size() > b.npcs.size();
        if (a.model != b.model) return a.model < b.model;
        return a.race < b.race;
    });
    return groups;
}

std::string group_label(const Group& group) {
    if (group.model != 0) return "model " + std::to_string(group.model);
    if (!group.race.empty()) return group.race;
    return "(no body)";
}

}  // namespace castlemist::npc
