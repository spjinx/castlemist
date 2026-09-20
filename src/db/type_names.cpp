/// @file
/// @brief Implementation of the coarse-type / container friendly-name lookups.

#include "castlemist/db/type_names.h"

namespace castlemist::db {

const char* coarse_type_name(const std::string& type) {
    if (type == "packfile") return "Packfile";
    if (type == "texture") return "Texture";
    if (type == "dds") return "Texture (DDS)";
    if (type == "strs") return "String Table";
    if (type == "riff") return "Audio (RIFF/WAV)";
    if (type == "png") return "Image (PNG)";
    if (type == "jpeg") return "Image (JPEG)";
    if (type == "exe") return "Executable";
    if (type == "asnd") return "Sound (raw)";
    if (type == "binary") return "Binary";
    if (type == "empty") return "Empty";
    return nullptr;
}

const char* container_type_name(const std::string& container) {
    // Model family: "MODL" and the older "ROOT" both nest ModelFileData/
    // ModelFileAnimation(Bank)/ModelFileCollision chunks in the struct
    // template (dumps/packfile/gw2_packfile.json).
    if (container == "MODL" || container == "ROOT") return "Model";
    // A dedicated animation state-machine container: chunks "mach"/"fall"/
    // "seqn"/"cnfg" resolve to PackAnimMachine(State/Transition/Action)/
    // PackAnimFallback/PackAnimSequence/PackAnimConfig -- GW2's animation
    // graph, i.e. what T3D calls an anim blend tree.
    if (container == "mach") return "Anim Blend Tree";
    if (container == "anim") return "Emote Animations";       // PackEmoteAnimationsVn
    if (container == "comp") return "Composite";               // PackCompositeVn (appearance layers)
    if (container == "PHYS") return "Scene Physics";           // SceneFilePhysics/Animation/Game/Skeleton
    if (container == "CSCN") return "Cinematic Scene";         // SceneDataVn
    // AMAT-family material shaders: the struct template resolves this
    // container's chunks to AmatGr/AmatMaterial/AmatDx9Material/AmatToolParams.
    if (container == "GRMT" || container == "AMAT") return "Material";
    if (container == "havk") return "Map Collision";           // PackMapCollideVn (Havok)
    if (container == "nm15") return "Map Navmesh";              // PackMapNavMeshChunkVn
    if (container == "area" || container == "mapc") return "Map";  // PackMapAreas/env/props/terrain/...
    if (container == "Main") return "Map Metadata";             // PackMapMetadata
    if (container == "ARMF") return "Asset Root Manifest";
    if (container == "MFST") return "Asset Manifest";
    if (container == "main") return "Collision Model Manifest"; // CollideModelManifest
    if (container == "mfst" || container == "prlt") return "Portal Manifest"; // ContentPortalManifest
    if (container == "txtm") return "Text Pack Manifest";
    if (container == "txtv") return "Text Pack Voices";
    if (container == "txtV" || container == "vari") return "Text Pack Variants";
    if (container == "txtp") return "Text Pack Passwords";
    if (container == "BIDX") return "Bank Index";
    if (container == "TKAC") return "Key Table";
    if (container == "eula") return "EULA Text";
    // Audio (docs/research/filetypes.md section 1): ASND = a single sound, ABNK/BKCK
    // = a bank of many, AMSP = a sound bank/script referencing external ASNDs.
    if (container == "ASND") return "Sound";
    if (container == "ABNK" || container == "BKCK") return "Audio Bank";
    if (container == "AMSP") return "Sound Script";
    if (container == "PIMG" || container == "PGTB") return "Image Atlas";
    if (container == "cntc") return "Content Database";
    return nullptr;
}

} // namespace castlemist::db
