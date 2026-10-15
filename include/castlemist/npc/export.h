/// @file
/// @brief Pure parts of exporting one NPC: the export folder name and the manifest JSON.
/// @ingroup npc

#pragma once

#include "castlemist/npc/catalogue.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

struct Gw2Dat;

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

/// What export_npc wrote.
struct ExportResult {
    bool ok = false;                   ///< True when manifest.json was written.
    std::filesystem::path folder;      ///< out_dir / export_folder_name(record).
    std::vector<std::string> files;    ///< File names written, manifest.json last.
    std::vector<std::string> notes;    ///< base_notes followed by failures in the order they happened.
};

/// Writes body.glb, weapon_<slot>.glb and manifest.json for one NPC into out_dir/export_folder_name(r). Never throws.
ExportResult export_npc(Gw2Dat& dat, const NpcRecord&, const std::filesystem::path& out_dir);

}  // namespace castlemist::npc
