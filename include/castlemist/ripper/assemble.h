#ifndef CASTLEMIST_RIPPER_ASSEMBLE_H
#define CASTLEMIST_RIPPER_ASSEMBLE_H

// A whole character as one rigged .glb: the race skeleton with its bare body,
// a head (face + hair + ears), every visible armor piece and the back item
// bound to that skeleton, weapons on their holster/hand joints, and one
// texture atlas rebuilt the way the game composites it (dyes baked in).
// docs/superpowers/specs/2026-10-04-character-assembly-design.md

#include <cstdint>
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
};

struct AssemblyPart {
    std::string name;    // "body chest", "face", "hair", "Coat", "WeaponA1", ...
    std::string status;  // "used", "hidden" (texture only), "dropped"
    std::string reason;  // why it was hidden/dropped
    uint32_t mesh = 0;
};

struct AssemblyReport {
    bool ok = false;
    std::string error;
    std::vector<AssemblyPart> parts;
    size_t joints = 0;
};

/// Opens its own handle on the dat (safe on a worker thread). `glb_path` is UTF-8.
AssemblyReport assemble_character(const character::CharacterManifest& manifest, const std::string& dat_path,
                                  const std::string& glb_path, const AssemblyOptions& options = {});

const char* to_string(WeaponPlacement w);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ASSEMBLE_H
