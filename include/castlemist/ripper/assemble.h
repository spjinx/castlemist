#ifndef CASTLEMIST_RIPPER_ASSEMBLE_H
#define CASTLEMIST_RIPPER_ASSEMBLE_H

// A whole character as one rigged .glb: the race skeleton with its bare body,
// a head (face + hair + ears), every visible armor piece and the back item
// bound to that skeleton, weapons on their holster/hand joints, and one
// texture atlas rebuilt the way the game composites it (dyes baked in).
// docs/superpowers/specs/2026-10-04-character-assembly-design.md

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "castlemist/character/manifest.h"
#include "castlemist/ripper/atlas.h"

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
    int ears = 0;                                // index into the race's ears
    std::optional<character::DyeShift> eye_tint;  // the iris (sylvari: the face's cut mask)
    int pattern = -1;                            // index into the race's skin patterns; -1 = none
    std::optional<character::DyeShift> pattern_tint;
    /// Glow (sylvari): the pattern lit in this colour, as an emissive texture.
    std::optional<std::array<uint8_t, 3>> glow_rgb;
    float glow_intensity = 1;
    /// Face-detail blend shapes (ripper/face_morphs.h) on every mesh the face
    /// rig moves; `face_sliders` (name -> 0..1) sets their default weights.
    bool face_morphs = true;
    std::map<std::string, float> face_sliders;
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

enum class LookPart { Face, Hair, Ears };

/// Front-view thumbnails of every face / hair style / ear option of the
/// manifest's race: the head (face, ears, hair) in `options`' look with that
/// one choice swapped, `size` px square, in option order. Empty + `*error` on
/// failure. Slow-ish (one head build per option): run it off the UI thread.
std::vector<ImageRgba> look_thumbnails(const character::CharacterManifest& manifest, const std::string& dat_path,
                                       LookPart part, const AssemblyOptions& options, int size,
                                       std::string* error = nullptr);

/// One swatch per skin pattern of the race: its chest mask blended from
/// `skin_rgb` (no pattern) to `pattern_rgb`, `size` px wide (2:1).
std::vector<ImageRgba> pattern_thumbnails(const character::CharacterManifest& manifest, const std::string& dat_path,
                                          std::array<uint8_t, 3> skin_rgb, std::array<uint8_t, 3> pattern_rgb,
                                          int size, std::string* error = nullptr);

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
