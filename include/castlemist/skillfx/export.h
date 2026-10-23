/// @file
/// @brief Names a skill set's skills, merges in log pairings, and writes the export folder.
///
/// Layout (docs/superpowers/specs/2026-10-22-skill-effects-design.md):
///   skill_set.json                     namespaces, skills {id, label, folder}
///   skills/<id>-<slug>/skill.json      one skill: effects, related skills, applied boons
///   models/<model>/                    each effect model once, laid out as a world export's model:
///                                      <model>.glb, materials.json, <model>_particles.json (+ .fbx)
///   unlinked/unlinked.json             effects no skill reached
/// skill.json effects name their model folder ("folder", "" when it failed) and .glb.

#pragma once

#include "castlemist/skillfx/pairing.h"
#include "castlemist/skillfx/skill_set.h"

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace castlemist::skillfx {

/// @brief Adds each pairing's effect (source "arcdps") to the skill it names and drops it from
/// `unlinked`; then labels every skill: `api_names`, else the logs' skill name, else
/// "<bundle item> slot <k+1>" for bundle slots, else the codename. Effects get `ei` names.
void apply_pairings(SkillSet& set, const std::vector<Pairing>& pairs,
                    const std::map<Guid, std::pair<size_t, const cntc::Object*>>& index, const cntc::PackSet& s,
                    const ContainerOf& container, const std::map<uint32_t, std::string>& api_names,
                    const std::map<Guid, std::string>& ei);

/// @brief Writes one model's folder (`folder/<model>.glb`, materials.json, sidecar): "" on success,
/// else the error text (it lands in skill.json).
using ModelExporter = std::function<std::string(uint32_t model, const std::filesystem::path& folder)>;

struct ExportReport {
    size_t skills = 0, models = 0, failed = 0;
    std::vector<std::string> notes;
};

/// @brief Writes the layout above under `out`, exporting each model once.
ExportReport export_skill_set(const SkillSet& set, const std::filesystem::path& out, const ModelExporter& exporter);

/// @brief Lowercase; each run of characters outside [a-z0-9] becomes one '-'; "skill" if empty.
std::string slug(const std::string& label);

}  // namespace castlemist::skillfx
