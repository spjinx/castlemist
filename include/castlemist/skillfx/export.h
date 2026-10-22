/// @file
/// @brief Names a skill set's skills, merges in log pairings, and writes the export folder.
///
/// Layout (docs/superpowers/specs/2026-10-22-skill-effects-design.md):
///   skill_set.json                     namespaces, skills {id, label, folder}
///   skills/<id>-<slug>/skill.json      one skill: effects, related skills, applied boons
///   skills/<id>-<slug>/<model>.glb     models only this skill uses
///   shared/<model>.glb                 models several skills use
///   unlinked/unlinked.json, <model>.glb  effects no skill reached
/// Each .glb comes with the exporter's `<model>_particles.json` sidecar.

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

/// @brief Exports one model to `glb`: "" on success, else the error text (it lands in skill.json).
using ModelExporter = std::function<std::string(uint32_t model, const std::filesystem::path& glb)>;

struct ExportReport {
    size_t skills = 0, models = 0, failed = 0;
    std::vector<std::string> notes;
};

/// @brief Writes the layout above under `out`, exporting each model once.
ExportReport export_skill_set(const SkillSet& set, const std::filesystem::path& out, const ModelExporter& exporter);

/// @brief Lowercase; each run of characters outside [a-z0-9] becomes one '-'; "skill" if empty.
std::string slug(const std::string& label);

}  // namespace castlemist::skillfx
