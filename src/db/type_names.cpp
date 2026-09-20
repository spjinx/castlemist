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
    // Every entry below is confirmed against a real gw2index build by joining
    // entries.container to chunks.struct_variant (the struct name
    // index_builder.cpp's own resolve_variant() already resolved from
    // dumps/packfile/gw2_packfile.json for that exact container) -- e.g.
    // container "anic" carries chunks mach/fall/seqn/cnfg resolving to
    // PackAnimMachine(State/Transition/Action)/PackAnimFallback/
    // PackAnimSequence/PackAnimConfig, GW2's animation graph (what T3D calls
    // an anim blend tree). An earlier pass here instead read
    // gw2_packfile.json's top-level keys as if they were literal container
    // fourccs, which is wrong for several of them -- e.g. "mach"/"anim"/
    // "comp"/"havk"/"CSCN"/"GRMT"/"BKCK"/"PGTB"/"ROOT" are real fourccs, but
    // as *chunks nested inside* anic/emoc/cmpc/hvkC/CINP/AMAT/ABNK/PIMG/MODL
    // respectively, never as a container fourcc on their own (0 occurrences
    // of any of them in entries.container across a real archive). Fixed by
    // querying a real index directly instead of assuming the JSON's grouping
    // matches container bytes 1:1.
    if (container == "MODL") return "Model";                 // GEOM/ANIM/COLL/ROOT/PRPS/SKEL chunks
    if (container == "anic") return "Anim Blend Tree";        // mach/fall/seqn/cnfg chunks
    if (container == "emoc") return "Emote Animations";       // anim -> PackEmoteAnimationsVn
    if (container == "cmpc") return "Composite";              // comp -> PackCompositeVn (appearance layers)
    // Confirmed by extracting a real "bone" entry and reading its bytes directly
    // (its lone chunk, "scal", has no template-resolved struct_variant, but the
    // payload itself contains the literal readable strings
    // "PackCompositeBoneScaleV20"/"PackCompositeBoneScaleParamV20"/
    // "PackCompositeBoneScaleRegionV20"/"PackCompositeMorphWeightV20" --
    // per-body-region bone scale/morph params for the same appearance
    // Composite system as "cmpc" above).
    if (container == "bone") return "Composite Bone Scale";
    if (container == "CINP") return "Cinematic Scene";        // CSCN -> SceneDataVn
    if (container == "AMAT") return "Material";               // GRMT/BGFX/DX9S -> Amat*MaterialVn
    if (container == "hvkC") return "Map Collision";          // havk -> PackMapCollideVn (Havok)
    if (container == "mapc") return "Map";                    // area/audi/cube/dcal/env./msn. chunks
    if (container == "mpsd") return "Map Shadow";             // shad -> PackMapShadowVn
    if (container == "prlt") return "Portal Manifest";        // mfst -> ContentPortalManifest
    // txtp/TextPackPasswords is IDA-confirmed (docs/research/filetypes.md
    // section 2a, vtable sub_1410CABA0) rather than struct-template-confirmed --
    // it happens not to appear in the sampled archive, unlike the rest of this
    // function's entries, all of which are struct-variant confirmed above.
    if (container == "txtm") return "Text Pack Manifest";
    if (container == "txtv") return "Text Pack Voices";
    if (container == "txtV") return "Text Pack Variants";     // vari -> TextPackVariants
    if (container == "txtp") return "Text Pack Passwords";
    if (container == "ABIX") return "Bank Index";             // BIDX -> BankIndexDataV0
    if (container == "eula") return "EULA Text";              // eula -> PackEulaV0
    // Audio (docs/research/filetypes.md section 1): ASND = a single sound, ABNK
    // = a bank of many (chunk BKCK), AMSP = a sound bank/script referencing
    // external ASNDs.
    if (container == "ASND") return "Sound";
    if (container == "ABNK") return "Audio Bank";
    if (container == "AMSP") return "Sound Script";
    if (container == "PIMG") return "Image Atlas";            // PGTB -> PagedImageTableDataVn
    if (container == "cntc") return "Content Database";

    // Circumstantial, NOT struct-variant confirmed under their own real
    // container key (unlike every entry above) -- both extracted clean from a
    // real archive with no readable strings, so this is contextual inference,
    // not evidence read out of the file the way "bone" was:
    //   - "cmaC"'s only chunk is "main" v1. The struct template has no
    //     "cmaC" entry, but it resolves that exact chunk+version under a
    //     DIFFERENT container key, "main", to CollideModelManifest_30680 --
    //     same chunk fourcc and version, just filed under the wrong key.
    //   - "mMet"'s only chunk is "Main" v0 (empty struct_variant, no
    //     template entry either). The container's own name is a plain
    //     abbreviation of "Map Metadata", and it is a small (~4 KB),
    //     per-entry file, consistent with one map's metadata blob.
    if (container == "cmaC") return "Collision Model Manifest";
    if (container == "mMet") return "Map Metadata";
    return nullptr;
}

} // namespace castlemist::db
