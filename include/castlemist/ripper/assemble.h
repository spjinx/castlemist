#ifndef CASTLEMIST_RIPPER_ASSEMBLE_H
#define CASTLEMIST_RIPPER_ASSEMBLE_H

// A whole character as one rigged .glb: the race skeleton with its bare body,
// a head (face + hair + ears), every visible armor piece and the back item
// bound to that skeleton, weapons on their holster/hand joints, and one
// texture atlas rebuilt the way the game composites it (dyes baked in).
// docs/superpowers/specs/2026-10-04-character-assembly-design.md

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "castlemist/character/manifest.h"

namespace castlemist::ripper {

enum class WeaponPlacement { Stowed, Hands, None };

struct AssemblyOptions {
    WeaponPlacement weapons = WeaponPlacement::Stowed;
    int face = 0;        // index into the race's faces
    int hair = 0;        // index into the race's hair styles
    int skin_style = 0;  // index into the race's bare-body styles
    /// Real-world metres (GW2 inches x 0.0254) for going straight to
    /// Unity/VRChat. Off by default: the same GW2 units as castlemist's other
    /// exports, so pieces and characters line up in one Blender scene.
    bool metres = false;
    /// Character-creator colours, as dye shifts (ripper/look.h resolves a saved
    /// look to these). skin_tint colours the bare body, face, ears and the
    /// scalp around the hair; hair_tint the hair's first dye channel,
    /// hair_tint2 its second. None = as the texture is authored.
    std::optional<character::DyeShift> skin_tint, hair_tint, hair_tint2;
};

struct AssemblyPart {
    std::string name;    // "body chest", "face", "hair", "Coat", "WeaponA1", ...
    std::string status;  // "used", "hidden" (texture only), "dropped"
    std::string reason;  // why it was hidden/dropped
    uint32_t mesh = 0;
    std::string file;    // separate mode: the .glb it went into (file name only)
};

struct AssemblyReport {
    bool ok = false;
    std::string error;
    std::vector<AssemblyPart> parts;
    size_t joints = 0;
};

/// Combined: the whole character as ONE rigged .glb. Bare-body parts under
/// armor and hair strands under a helm are left out; every weapon of the kit
/// sits on its own holster (one with no free holster is left out). Opens its own handle on
/// the dat (safe on a worker thread). `glb_path` is UTF-8.
AssemblyReport assemble_character(const character::CharacterManifest& manifest, const std::string& dat_path,
                                  const std::string& glb_path, const AssemblyOptions& options = {});

/// Separate: a folder holding `body.glb` (the full bare body + head, nothing
/// hidden) and one .glb per armor piece, back item and weapon, every file on
/// the same full race skeleton with the same root, so each piece fits onto the
/// body. Every weapon of the kit gets a file, on its holster. `out_dir` is UTF-8; files are named NN_<Slot>_<skin name>.glb.
AssemblyReport assemble_character_separate(const character::CharacterManifest& manifest,
                                           const std::string& dat_path, const std::string& out_dir,
                                           const AssemblyOptions& options = {});

const char* to_string(WeaponPlacement w);

/// Every weapon slot of the kit, in placement priority: the active set, the
/// second set, then the underwater weapons.
extern const char* const kWeaponSlots[6];

/// Picks a body holster for each stowed weapon. `stow_joints[i]` are weapon
/// i's own stow points ("actionpoint:RStowBack", ...), weapons in priority
/// order (`slots[i]` names them, e.g. "WeaponB2"). Maximises the weapons on a
/// holster of their own; ties favour earlier slots, then main hands (x1,
/// AquaticA) on the right and off-hands (x2, AquaticB) on the left. Returns the
/// chosen weapon stow joint per weapon ("" = it has none); a weapon that could
/// only share gets the same holster as an earlier one.
std::vector<std::string> choose_holsters(const std::vector<std::string>& slots,
                                         const std::vector<std::vector<std::string>>& stow_joints);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ASSEMBLE_H
