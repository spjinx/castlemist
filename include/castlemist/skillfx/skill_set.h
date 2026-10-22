/// @file
/// @brief Every skill of one codename namespace (e.g. Firebrand's "Bp5rI."), with its effects.
///
/// The public API misses skills (all tome chapters), so the set comes from the dat: every
/// type-64 object whose codename shares the seed skills' prefix. Tomes are bundle items
/// (type 35) whose bar (type 65, ref at +264) lists the chapter skills at +192 + 112*k.

#pragma once

#include "castlemist/skillfx/effects.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace castlemist::skillfx {

/// @brief One skill of the set.
struct SkillEntry {
    uint32_t id = 0;            ///< dataId (the API id).
    std::string codename;
    std::string label;          ///< Display name; filled by apply_pairings (export.h).
    uint32_t bundle_item = 0;   ///< Bundle slot skills (tome pages): the item's dataId.
    int bundle_slot = -1;       ///< 0..5, or -1.
    SkillLinks links;
};

/// @brief A namespace's skills and the effects none of them link to.
struct SkillSet {
    std::string skill_namespace, effect_namespace;  ///< Prefixes up to and including the '.', e.g. "Bp5rI.".
    std::vector<SkillEntry> skills;                  ///< Ordered by id.
    std::vector<Effect> unlinked;                    ///< Effects in effect_namespace that no skill links to.
};

/// @brief Builds the set for the namespace most of `seed_skill_ids` share.
SkillSet build_skill_set(const cntc::PackSet& s, const std::vector<uint32_t>& seed_skill_ids,
                         const ContainerOf& container);

/// @brief Every type-183 object, by GUID.
std::map<Guid, std::pair<size_t, const cntc::Object*>> effect_index(const cntc::PackSet& s);

}  // namespace castlemist::skillfx
