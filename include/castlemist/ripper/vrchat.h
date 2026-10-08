#ifndef CASTLEMIST_RIPPER_VRCHAT_H
#define CASTLEMIST_RIPPER_VRCHAT_H

// A character as a VRChat-ready avatar: one combined model in metres, meshes
// merged per material (the character atlas becomes a single mesh), bones named
// the way Unity's humanoid auto-mapper expects (Hips, Spine, LeftUpperArm,
// LeftEye, ...; no ':' -- FBX reads it as a namespace), the face-detail blend
// shapes plus VRChat's Blink / Blink_L / Blink_R and vrc.v_* visemes, and a
// setup note listing the bone chains to give PhysBones. Blender (headless, with
// tools/blender/castlemist_vrchat.py) turns the .glb into the .fbx Unity takes.

#include <string>
#include <vector>

#include "castlemist/character/manifest.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/ripper/assemble.h"

namespace castlemist::ripper {

/// Unity humanoid name for a GW2 race-skeleton joint ("bone:COG" -> "Hips"),
/// or empty when it has none.
std::string humanoid_name(const std::string& gw2_joint);

/// In place: VRChat face keys, meshes merged per material, joints renamed
/// (humanoid names, then every other joint without its "bone:" / ':' prefix,
/// kept unique). Returns the PhysBone chain roots (renamed) -- hair, cloth,
/// stems and back-item bones.
std::vector<std::string> make_vrchat_ready(ModelPreview& model);

/// Writes the atlas materials' maps as PNGs into "<glb stem> Textures" beside
/// `glb_path` (UTF-8), laid out for Unity / Poiyomi: "<Material> - BaseColor",
/// "- Normal", "- Emission", "- MetallicSmoothness" (R = metal, A = smoothness)
/// and "- Detail". Returns the folder, or empty when nothing was written.
std::string write_vrchat_maps(const ModelPreview& model, const std::string& glb_path);

struct VrchatOptions {
    std::string blender_exe;  // empty: find_blender(); none found -> .glb only
    std::string script;       // empty: <castlemist root>/tools/blender/castlemist_vrchat.py
};

struct VrchatReport {
    bool ok = false;
    std::string error;
    std::string glb, fbx, notes;  // written files (fbx empty when Blender didn't run)
    std::string blender;          // the Blender used, or why none
    std::vector<std::string> physbone_chains;
    size_t meshes = 0, materials = 0, joints = 0;
    AssemblyReport assembly;
};

/// Writes <out_dir>/<name>.glb (+ .fbx via Blender) and <name> VRChat setup.txt.
/// `options` supplies the look (apply_look) and weapons; metres and the VRChat
/// preparation are forced on. `out_dir` is UTF-8.
VrchatReport export_vrchat(const character::CharacterManifest& manifest, const std::string& dat_path,
                           const std::string& out_dir, AssemblyOptions options, const VrchatOptions& vrc = {});

struct VrchatModelReport {
    bool ok = false;
    std::string error;
    std::string folder, glb, fbx, blender, blenderLog;  // fbx empty when Blender didn't run or failed
    size_t materials = 0, clips = 0;
    std::vector<std::string> warnings;
};

/// Writes the VRChat folder `<parentDir>/<safe_file_name(name)>` (.glb, Textures,
/// materials.json) and then, via Blender in model mode, `<Name>.fbx` + `.blend`.
/// `vrc.blender_exe == "-"` skips Blender. On a Blender failure the log is kept as
/// `<Name> blender.log` (path in blenderLog); the folder is still written.
VrchatModelReport export_vrchat_model(const ModelPreview& model, const std::string& parentDirUtf8,
                                      const std::string& name, uint32_t modelFileId,
                                      const VrchatOptions& vrc = {});

/// The newest Blender under Program Files\Blender Foundation, or empty.
std::string find_blender();

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_VRCHAT_H
