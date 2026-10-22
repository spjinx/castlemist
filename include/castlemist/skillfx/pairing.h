/// @file
/// @brief Pairs effects with the skills that played them, from arcdps logs.
///
/// Most Firebrand effects are referenced by nothing in client data: the game plays them by
/// an id it receives at runtime (docs/research/gw2-skill-effects.md). An arcdps log records
/// each effect the client plays with its GUID, beside the player's casts, so an effect that
/// follows a skill's cast every time belongs to that skill.

#pragma once

#include "castlemist/skillfx/evtc.h"

#include <istream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace castlemist::skillfx {

/// @brief One skill -> effect pairing seen in the logs.
struct Pairing {
    uint32_t skill_id = 0;   ///< Cast skill, or the buff an instant skill applies.
    std::string skill_name;  ///< From the log's skill table ("" if absent).
    Guid effect{};
    uint32_t count = 0;      ///< Markers (casts) it followed.
};

/// @brief Markers are the player's ANIMATIONSTART (its skill) and its BUFFAPPLY whose skill is
/// in `marker_buffs` (instant skills show no cast). Each effect the player creates (statechange
/// 60/62, or 45/51 in older logs) goes to the latest marker at or before it within `window_ms`.
/// A marker counts once per effect GUID. Pairings seen fewer than `min_count` times are
/// dropped. Sorted by skill id, then count (most first).
/// @param elite_spec The recording player's specialization id (62 = Firebrand).
std::vector<Pairing> pair_effects(const std::vector<EvtcLog>& logs, uint32_t elite_spec,
                                  const std::set<uint32_t>& marker_buffs, uint32_t window_ms = 3000,
                                  uint32_t min_count = 2);

/// @brief Elite Insights' EffectGUIDs.cs: lines `Guid Name = new("HEX32")` -> GUID bytes in order.
std::map<Guid, std::string> read_ei_guids(std::istream& cs);

}  // namespace castlemist::skillfx
