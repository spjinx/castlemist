#ifndef CASTLEMIST_RIPPER_CHARACTER_EXPORT_H
#define CASTLEMIST_RIPPER_CHARACTER_EXPORT_H

// A whole character manifest -> a folder of per-piece .glb files plus
// manifest.json and export_report.json.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "castlemist/character/manifest.h"
#include "castlemist/format/composite.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/ripper/piece_export.h"

namespace castlemist::ripper {

/// The Composite file: the index entry with container "cmpc" when an index DB
/// is open, else fileId 154681. nullopt if missing or unparsable.
std::optional<composite::Composite> load_composite(Gw2Dat& dat);

struct CharacterExportReport {
    std::vector<std::pair<std::string, PieceExportResult>> pieces;  // (slot, result), manifest order
    std::string error;  // set when nothing could be exported at all (no dat / no Composite / no folder)
    size_t exported() const;
};

/// Opens its own handle on the dat (safe on a worker thread), finds and parses
/// the Composite once, and writes `<out_dir>/<NN>_<Slot>_<skin name>.glb` for
/// every exportable piece.
CharacterExportReport export_character(const character::CharacterManifest& manifest, const std::string& dat_path,
                                       const std::string& out_dir);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_CHARACTER_EXPORT_H
