/// @file
/// @brief Public API: ModelPreview -> binary glTF (.glb) on disk.
/// @ingroup exportgltf

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "castlemist/extract/map_types.h"
#include "castlemist/extract/model_types.h"

/// @defgroup exportgltf exportgltf -- binary glTF (.glb) writer
/// @brief Turns already-extracted CPU model/map data into a Blender/Unity-ready .glb.
///
/// glTF rather than FBX: FBX's *binary* container (the only form Blender's
/// importer reliably reads -- ASCII FBX is technically legal but not
/// supported in practice) has no official public specification, and hand
/// matching it byte-for-byte against an undocumented format proved fragile.
/// glTF 2.0 is a precisely specified, Khronos-ratified format with
/// unambiguous skinning (a skin is just a joint list + inverse bind
/// matrices) and animation (channels target a node's translation/rotation/
/// scale directly, sampled from an explicit keyframe accessor -- no
/// implicit connection graph to get subtly wrong), and Blender's importer is
/// first-party/core rather than a community addon.
///
/// A single .glb embeds geometry, materials, textures, the skeleton/skin and
/// animation clips in one file. For the final Unity/VRChat hop, import the
/// .glb into Blender and use Blender's own FBX exporter -- one extra manual
/// step that trades a hand-rolled binary writer for Blender's battle-tested
/// one on that leg.
///
/// Pure CPU-in, files-out -- no UI or Direct3D dependency, safe to call from
/// a background thread exactly like `extract_entry`.
/// @{

namespace castlemist::exportgltf {

/// @brief A folder of dat textures shared by many exports (the world export's
///        `textures/`, ruling W8): each fileId is written once, as
///        `<dir>/<fileId>.png`. Thread-safe: the first caller for a fileId
///        encodes and writes it (to `<fileId>.png.tmp`, then renamed), and
///        concurrent callers for the same fileId wait until it is in place, so
///        nobody references a half-written file.
class SharedImagePool {
public:
    explicit SharedImagePool(std::string dirUtf8);
    ~SharedImagePool();
    SharedImagePool(const SharedImagePool&) = delete;
    SharedImagePool& operator=(const SharedImagePool&) = delete;

    /// @brief `<fileId>.png` exists in the pool after this returns true:
    ///        written now from @p encodePng (PNG bytes; empty = failure), or
    ///        earlier. False (with @p error) when encoding or writing failed;
    ///        a failed fileId stays failed.
    bool ensure(uint32_t fileId, const std::function<std::vector<uint8_t>()>& encodePng,
                std::string* error = nullptr);
    /// @brief The pool's file name of @p fileId: "<fileId>.png".
    static std::string file_name(uint32_t fileId);

    /// @brief Content-named images (Task 15d, the world export's `textures/gen/`):
    ///        @p png is written once as "<sha1(png)><suffix>.png", the SHA-1 of the
    ///        PNG bytes in lower-case hex, with the same claim, wait and temp-rename
    ///        as ensure(). Returns that file name, or "" (with @p error) when @p png
    ///        is empty or the write failed. Every call adds png.size() to
    ///        bytes_requested(); the one that writes a name adds it to bytes_written().
    ///        @p suffix (e.g. "_BaseColor") must be a legal file-name part.
    std::string ensure_content(const std::vector<uint8_t>& png, const std::string& suffix = {},
                               std::string* error = nullptr);
    const std::string& dir() const;
    size_t written() const;   ///< files written successfully so far
    uint64_t bytes_requested() const;   ///< ensure_content: the bytes every call asked for
    uint64_t bytes_written() const;     ///< ensure_content: the bytes written once each

private:
    bool ensure_file(const std::string& name, const std::function<std::vector<uint8_t>()>& encodePng,
                     std::string* error, uint64_t* bytes);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// @brief Tunables for one export call.
struct GltfExportOptions {
    /// @brief Fixed sample rate used to bake every animation clip's keyframes.
    ///
    /// GW2's Granny curves come in a dozen native formats (constant, spline,
    /// dense keyframe); rather than translate each one into a glTF
    /// interpolation mode, every clip is resampled at this rate via
    /// `castlemist::granny::sample()`, which is format-agnostic and is what
    /// most game-asset exporters do.
    int animFps = 30;

    /// @brief The scene root's rotation (glTF xyzw quaternion). The default is
    ///        the fixed -90-degrees-about-X Z-up -> Y-up turn every export has
    ///        always used; the character assembler passes an upright-facing
    ///        turn instead (GW2 characters are authored with height along -Z).
    std::array<double, 4> rootRotation = {-0.70710678, 0.0, 0.0, 0.70710678};
    /// @brief Uniform scale on the scene root; 1 writes no scale at all.
    ///        0.0254 turns GW2's inch units into glTF metres.
    double rootScale = 1.0;

    /// @brief When set, the scene root is named `GW2_MapToGltf` and carries
    ///        this glTF (column-major) `matrix`; @ref rootRotation and
    ///        @ref rootScale are then ignored. The world export passes
    ///        `map_to_gltf_root(1)` (world_export.h), a reflection, which no
    ///        quaternion can express.
    std::optional<std::array<float, 16>> rootMatrix;

    /// @brief Non-empty: every image is written as a PNG file in this folder
    ///        (UTF-8; created if missing) and referenced by `uri` instead of
    ///        being embedded in the .glb. A dat texture's file is
    ///        `<fileId>.png`; a generated one (baked emissive, metal-rough,
    ///        atlas) is named by its image name. Empty (the default) embeds.
    std::string imageDirUtf8;
    /// @brief Prepended to the file name to form the image `uri`, e.g.
    ///        "Images/" for a folder beside the .glb. Only read with @ref imageDirUtf8.
    std::string imageUriPrefix;

    /// @brief Set (world export only): every dat texture of the model
    ///        (ModelPreview::textures, fileId != 0) goes into this pool once
    ///        and is referenced as @ref sharedImageUriPrefix + "<fileId>.png";
    ///        generated images still follow @ref imageDirUtf8. Null (the
    ///        default) changes nothing.
    std::shared_ptr<SharedImagePool> sharedImages;
    /// @brief The pool folder relative to the .glb, e.g. "../../textures/".
    std::string sharedImageUriPrefix;

    /// @brief World export only (Task 15b): every mesh's own LOD1/LOD2 index sets
    ///        (ModelMeshCPU::lodIndices, at most world_lods.h kMaxLodLevels levels)
    ///        are written as extra meshes sharing the LOD0 vertex accessors, on nodes
    ///        named "<LOD0 node name>_LOD<k>" after the LOD0 nodes. False (the
    ///        default) writes LOD0 only, byte for byte as before.
    bool exportLods = false;

    /// @brief Set (world export only, Task 15d): generated images (the emissive
    ///        bake, the metal-rough map) go into this content pool once each, as
    ///        "<sha1 of the PNG>.png" (SharedImagePool::ensure_content), referenced
    ///        as @ref generatedImageUriPrefix + that name, instead of into
    ///        @ref imageDirUtf8. Null (the default) changes nothing.
    std::shared_ptr<SharedImagePool> generatedImages;
    /// @brief The content pool folder relative to the .glb, e.g. "../../textures/gen/".
    std::string generatedImageUriPrefix;

    /// @brief World export only (Task 15d): one key input per model material (the
    ///        caller's side of the key, e.g. its materials.json entry). When it has
    ///        exactly ModelPreview::materials.size() entries, each glTF material gets
    ///        `extras.castlemistMaterialKey` = SHA-1 of {gltf: the material without
    ///        its name, every texture as its image URI, vrchat: the input}, and
    ///        GltfExportResult::materialKeys lists them. Empty (the default) writes no key.
    std::vector<std::string> materialKeyInputs;
};

/// @brief Result of one export call.
struct GltfExportResult {
    bool ok = false;             ///< False on any hard failure; see @ref error.
    std::string error;           ///< Human-readable failure reason when !ok.
    std::string glbPath;         ///< The .glb actually written.
    /// @brief `<stem>_particles.json` written alongside the .glb when the
    ///        model carries baked particle clouds/effect lights (see
    ///        exportgltf's particle_export.cpp). Empty when the model has
    ///        none -- not an error, just nothing to export.
    std::string particlesJsonPath;
    /// @brief Per model material, its key (GltfExportOptions::materialKeyInputs);
    ///        empty when no inputs were given.
    std::vector<std::string> materialKeys;
};

/// @brief Export one model (as shown in the Model preview tab) to a .glb.
///
/// @param model   A fully built model preview (meshes, materials, textures,
///                skeleton and animation clips already decoded).
/// @param glbPath Destination .glb path, UTF-8.
GltfExportResult export_model_gltf(const ModelPreview& model, const std::string& glbPath,
                                   const GltfExportOptions& opts = {});

/// @brief Result of one standalone texture save.
struct TextureSaveResult {
    bool ok = false;    ///< False on any hard failure; see @ref error.
    std::string error;  ///< Human-readable failure reason when !ok.
};

/// @brief Encodes one already-decoded model texture as a standalone PNG file.
///
/// Reuses the same stb_image_write path the glTF exporter embeds textures
/// with (the sole stb_image_write "implementation" translation unit lives in
/// this layer -- see texture_export.cpp), so a texture saved this way is
/// pixel-identical to what a full model export would have embedded.
///
/// @param tex     An already-decoded texture (`ModelPreview::textures[i]`).
/// @param pngPath Destination .png path.
TextureSaveResult save_texture_png(const ModelTextureCPU& tex, const std::string& pngPath);

} // namespace castlemist::exportgltf

/// @}
