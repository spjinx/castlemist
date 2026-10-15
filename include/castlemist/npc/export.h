/// @file
/// @brief Pure parts of exporting one NPC: the export folder name and the manifest JSON.
/// @ingroup npc

#pragma once

#include "castlemist/npc/catalogue.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace castlemist::npc {

/// A clip written for an NPC: its name and duration in seconds.
struct ClipInfo {
    std::string name;
    double duration = 0;
};

/// A weapon and the file it was written to ("" when not written).
struct WrittenWeapon {
    Weapon weapon;
    std::string file;
};

/// `<id>_<codename>` with every character outside [A-Za-z0-9._-] replaced by '_'; `<id>` when the codename is empty.
std::string export_folder_name(const NpcRecord&);

/// The manifest ("version": 1). Clip group and prefix come from clip_group/clip_prefix; `notes` are used verbatim.
nlohmann::json export_manifest(const NpcRecord&, const std::vector<ClipInfo>& clips,
                               const std::vector<WrittenWeapon>& weapons,
                               const std::vector<std::string>& files, const std::vector<std::string>& notes);

/// Notes derivable from the record alone: a missing body model, then one per weapon without a skin model.
std::vector<std::string> base_notes(const NpcRecord&);

}  // namespace castlemist::npc
