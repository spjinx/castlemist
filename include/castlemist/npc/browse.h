/// @file
/// @brief Pure grouping and filtering of the NPC catalogue for the browsing UI.
/// @ingroup npc

#pragma once

#include "castlemist/npc/catalogue.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace castlemist::npc {

/// NPCs sharing a body: the same model fileId, or (for model-less NPCs) the same race.
struct Group {
    uint32_t model = 0;
    std::string race;
    std::vector<const NpcRecord*> npcs;  ///< Sorted by id; point into the Catalogue passed to group_creatures.
};

/// Resolves a skill dataId to its display name; may be empty (skill names are then not searched).
using SkillName = std::function<std::string(uint32_t skill_id)>;

/// True when `filter` is empty or is a case-insensitive substring of the decimal id, codename, race,
/// decimal model or any skill name (the last only when `skill_name` is non-empty).
bool matches(const NpcRecord& npc, const std::string& filter, const SkillName& skill_name);

/// Groups the matching NPCs by model (race when the model is 0). Groups are sorted by NPC count descending,
/// then model, then race.
std::vector<Group> group_creatures(const Catalogue& catalogue, const std::string& filter, const SkillName& skill_name);

/// "model <fileId>" for a model group, the race for a race group, "(no body)" when both are absent.
std::string group_label(const Group& group);

}  // namespace castlemist::npc
