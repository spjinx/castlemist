#ifndef CASTLEMIST_RIPPER_PIECE_EXPORT_H
#define CASTLEMIST_RIPPER_PIECE_EXPORT_H

// One equipped piece -> one .glb. Armor is rebuilt the way the game dresses a
// character: the race/gender model and textures from the Composite file, the
// character's dyes baked into the diffuse, and its atlas UVs remapped onto the
// piece's own cropped textures. Weapons, back items and anything else with a
// self-contained model are exported as-is.

#include <cstdint>
#include <string>

#include "castlemist/character/manifest.h"
#include "castlemist/format/composite.h"
#include "castlemist/native/gw2dat.h"

namespace castlemist::ripper {

struct PieceContext {
    Gw2Dat* dat = nullptr;                          // open archive; used from one thread only
    const composite::Composite* comp = nullptr;     // parsed Composite file
    std::string race_key;                           // API race + gender, e.g. "SylvariFemale"
};

struct PieceExportResult {
    bool ok = false;
    std::string glb_path;
    std::string status;  // "armor", "model" or "skipped"
    std::string reason;  // why it was skipped / failed, empty on success
    uint32_t mesh = 0, texture_base = 0;
    int dyed_channels = 0;   // dyes that reached a mask and were baked
    int undyed_channels = 0; // dyes the piece has but whose mask was missing/undecodable
};

/// Skip reasons: "no skin", "no appearance token (rebuild the content map)",
/// "no appearance for <race_key>", "model failed to load",
/// "texture failed to decode", "no mesh inside an atlas rect", "glTF export failed: ...".
/// One of the race's undergarments (composite::kUndergarmentTopToken /
/// kUndergarmentBottomToken) as its own .glb, in Dye Remover. "skipped" when the race
/// has none (male races have no top).
PieceExportResult export_undergarment(const PieceContext& ctx, uint64_t token, const std::string& glb_path);

PieceExportResult export_piece(const PieceContext& ctx, const character::ManifestPiece& piece,
                               const std::string& glb_path);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_PIECE_EXPORT_H
