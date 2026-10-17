/// @file
/// @brief Internal seam of the exportgltf layer -- the glTF document/buffer
///        builder, transform math and per-concern builders shared between
///        its .cpp files.
///
/// @warning Not a public header. The layer's public API is
///          `castlemist/exportgltf/gltf_export.h`.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/exportgltf/shader_profiles.h"

namespace castlemist::exportgltf {

// ================================================================ transform math ==
//
// Row-major, row-vector (`p' = p * M`) -- matching ModelJoint::invWorld and
// render/detail/math.h's sceneWorld() exactly (both are documented "row-major
// row-vector"). glTF has no per-axis "up axis" declaration the way FBX does,
// so instead of converting every vertex/quaternion from GW2's Z-up into
// glTF's Y-up, the whole exported scene is parented under one synthetic root
// node carrying a fixed -90-degrees-about-X rotation: every joint, vertex and
// instance placement below it stays in GW2's own native numbers untouched,
// and that one root reorients the whole tree on import. This also means
// GW2's non-standard `Scale*RotZ*RotX*RotY*Translate` instance composition
// never needs decomposing into any particular Euler order: glTF nodes may
// carry a raw 4x4 `matrix` directly.

struct Vec3 { float x = 0, y = 0, z = 0; };

/// @brief 16 floats, row-major, row-vector: `m[r*4+c]`, translation in row 3.
struct Mat4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};

Mat4 mat4_mul(const Mat4& a, const Mat4& b); ///< `a` applied first, then `b` (row-vector chaining).
Mat4 mat4_rot_x(float radians);
Mat4 mat4_rot_y(float radians);
Mat4 mat4_rot_z(float radians);
Mat4 mat4_scale(float s);
Mat4 mat4_translate(const Vec3& t);

/// @brief GW2's own per-prop placement, ported 1:1 from `sceneWorld()` in
///        `src/render/detail/math.h` (reimplemented here rather than taking a
///        dependency on the render layer). `rot` is Euler radians; `rot.z` is yaw.
Mat4 scene_world(const Vec3& pos, const Vec3& rot, float scale);

/// @brief Flattens a row-vector Mat4 into glTF's column-major 16-float array
///        (`node.matrix` / an inverse-bind-matrix entry). Perhaps
///        counterintuitively this is a **direct copy** of `m.m`, not a
///        transpose: glTF's column-major storage of the column-vector matrix
///        `transpose(m)` and the mathematical transpose cancel out exactly
///        (see the definition for the full derivation). No decomposition risk
///        either way, since a glTF node may carry this matrix as-is.
std::array<float, 16> flatten_column_major(const Mat4& m);

/// @brief Strip characters that would look odd as a glTF node/animation name
///        in Blender's outliner (not a format requirement -- arbitrary UTF-8
///        is legal in a glTF JSON string -- just hygiene).
std::string sanitize_name(const std::string& name);

// ==================================================================== glTF writer ==
//
// Builds the JSON scene graph with nlohmann::json (already a vendored,
// trusted dependency in this project -- see cmake/Externals.cmake -- so
// string-concatenation JSON-escaping bugs are simply not a risk here) plus
// one flat binary buffer for the .glb's BIN chunk, then assembles both into
// the final container on ::finish.
class GltfWriter {
public:
    GltfWriter();

    /// @brief Appends `data` to the binary buffer (4-byte aligned, which
    ///        satisfies every accessor component size this layer uses) and
    ///        returns a new bufferView index. `target` is the WebGL buffer
    ///        target hint (0 = none, 34962 = ARRAY_BUFFER, 34963 = ELEMENT_ARRAY_BUFFER).
    int add_buffer_view(const void* data, size_t byteLength, int target = 0);

    /// @brief Adds an accessor over a freshly-appended buffer view in one call.
    /// @param componentType glTF component type (5121 UNSIGNED_BYTE, 5123
    ///        UNSIGNED_SHORT, 5125 UNSIGNED_INT, 5126 FLOAT).
    /// @param type          glTF accessor type ("SCALAR", "VEC2", "VEC3", "VEC4", "MAT4").
    /// @param count         Element count (not component count).
    /// @param minMax        When non-null, sets the accessor's `min`/`max`
    ///                      (glTF requires this for POSITION accessors).
    /// @return The new accessor's index.
    int add_accessor(const void* data, size_t byteLength, int componentType, const std::string& type,
                     size_t count, int target,
                     const nlohmann::json* minVal = nullptr, const nlohmann::json* maxVal = nullptr);

    /// @brief Embeds a PNG (already-encoded bytes) as an image + texture,
    ///        deduped by fileId so a shared map export never re-embeds the
    ///        same texture twice. Returns the *texture* index (what a
    ///        material's textureInfo references). `name` becomes the image's
    ///        glTF name -- what Blender calls the image, and the file name
    ///        "Unpack Resources" writes it out as.
    ///
    ///        After ::set_image_output the PNG is written to that folder
    ///        instead and referenced by URI (see ::add_texture_uri); a failed
    ///        write returns -1 and records ::image_error.
    int add_or_reuse_texture(uint32_t fileId, const std::vector<uint8_t>& pngBytes, const std::string& name = {});

    /// @brief Adds an image referenced by `uri` (no bufferView) + texture,
    ///        deduped by fileId exactly as ::add_or_reuse_texture (fileId 0 is
    ///        never shared). Returns the texture index.
    int add_texture_uri(uint32_t fileId, const std::string& uri, const std::string& name = {});

    /// @brief From now on, ::add_or_reuse_texture writes `<dir>/<stem>.png`
    ///        (UTF-8 `dirUtf8`, created on first use) and references it as
    ///        `<uriPrefix><stem>.png`. `stem` is the image name made a legal,
    ///        unique (case-insensitive) file name, else the fileId.
    ///        An empty `dirUtf8` switches back to embedding.
    void set_image_output(const std::string& dirUtf8, const std::string& uriPrefix);
    /// @brief From now on (world export, Task 15d), ::add_or_reuse_texture puts
    ///        the PNG into @p pool by content ("<sha1>.png") and references it as
    ///        `<uriPrefix><sha1>.png`; an image already referenced by that URI in
    ///        this file is reused. Wins over ::set_image_output. Null switches it off.
    void set_generated_pool(SharedImagePool* pool, const std::string& uriPrefix);
    /// @brief Material @p i as added (Task 15d: the key reads it, then adds its extras).
    nlohmann::json& material_json(int i) { return doc_["materials"][static_cast<size_t>(i)]; }
    /// @brief The image URI texture @p textureIndex samples; empty for an embedded
    ///        image or a bad index.
    std::string texture_uri(int textureIndex) const;
    /// @brief The first image file write failure since construction; empty when none.
    const std::string& image_error() const { return imageError_; }
    /// @brief Records @p e as ::image_error unless one is already recorded.
    void set_image_error(const std::string& e) { if (imageError_.empty()) imageError_ = e; }

    int add_node(nlohmann::json node);           ///< Pushes to `nodes`, returns its index.
    /// @brief Appends `childIndex` to an already-added node's `children` list
    ///        (auto-created if this is its first child). Used to wire up a
    ///        skeleton hierarchy without needing every parent's children
    ///        known before it's added -- see write_skeleton()'s two-pass build.
    void add_child(int parentIndex, int childIndex);
    int add_mesh(nlohmann::json mesh);            ///< Pushes to `meshes`, returns its index.
    int add_material(nlohmann::json material);    ///< Pushes to `materials`, returns its index.
    int add_skin(nlohmann::json skin);            ///< Pushes to `skins`, returns its index.
    void add_animation(nlohmann::json animation); ///< Pushes to `animations`.
    void add_scene_root(int nodeIndex);           ///< Adds `nodeIndex` to the (sole) scene's node list.
    /// @brief Lists @p name in `extensionsUsed` (once), for a material or
    ///        node that carries that extension (e.g. KHR_materials_unlit).
    void use_extension(const std::string& name);

    /// @brief Assembles the final .glb: the 12-byte header, the JSON chunk
    ///        (space-padded to 4 bytes) and the BIN chunk (zero-padded to 4 bytes).
    std::vector<uint8_t> finish();

private:
    nlohmann::json doc_;
    std::vector<uint8_t> bin_;
    std::map<uint32_t, int> textureByFileId_; // fileId -> texture index, for dedup
    std::string imageDir_, imageUriPrefix_;   // set_image_output; empty = embed
    std::set<std::string> imageStems_;        // lower-case file stems written, for unique names
    SharedImagePool* generatedPool_ = nullptr; // set_generated_pool
    std::string generatedPrefix_;
    std::map<std::string, int> textureByUri_; // generated-pool URI -> texture index
    std::string imageError_;
};

// ==================================================================== textures ==

/// @brief Encodes and embeds every texture in `textures` into `w`, deduping
///        by fileId so a shared map export never re-encodes the same texture
///        twice.
/// @param texIndices Parallel to `textures`: the resulting glTF *texture*
///        index for each one (-1 if it had no decoded pixels to encode).
///
/// @param imageDirUtf8 Non-empty: switches `w` to ::GltfWriter::set_image_output
///        (`imageDirUtf8`, `uriPrefix`) first, so these and every later image
///        are written as files and referenced by URI. Empty leaves `w` as it is.
/// @return False when an image file could not be written (::GltfWriter::image_error).
/// @param shared     Non-null: textures with a fileId go into this pool once and
///        are referenced as `sharedPrefix + SharedImagePool::file_name(fileId)`
///        (GltfExportOptions::sharedImages); a failed pool write is an image_error.
bool write_model_textures(GltfWriter& w, const std::vector<ModelTextureCPU>& textures,
                          std::vector<int>& texIndices, const std::string& imageDirUtf8 = {},
                          const std::string& uriPrefix = {}, SharedImagePool* shared = nullptr,
                          const std::string& sharedPrefix = {});

/// @brief PNG bytes for an already-decoded texture; empty when it has no
///        pixels or encoding fails. Lets callers write files through a UTF-8
///        `std::filesystem::path` (save_texture_png takes a narrow path).
std::vector<uint8_t> encode_png(const ModelTextureCPU& tex);

/// @brief PNG bytes of @p pixels (width x height, @p channels 1..4 bytes per
///        texel, row 0 first); empty when the size is wrong or encoding fails.
///        The world export writes its RGB terrain albedo with it.
std::vector<uint8_t> encode_png_pixels(int width, int height, int channels, const std::vector<uint8_t>& pixels);

/// @brief Bakes and embeds an emissive texture for an "effect" (additive/glow)
///        material: the diffuse texture's own bright regions (luminance-
///        thresholded, matching castlemist's reconstruction shader's glow
///        split) become a real, portable glTF `emissiveTexture` -- a standard
///        PBR channel that survives glTF -> Blender -> FBX -> Unity/Poiyomi
///        via the shared `_EmissionMap` property name Unity preserves when a
///        material's shader is swapped, so it shows up with no companion
///        script and no manual texture-dragging.
/// @return The new texture's glTF index, or -1 if `diffuse` has no decoded pixels.
int bake_effect_emissive_texture(GltfWriter& w, const ModelTextureCPU& diffuse);

// ==================================================================== materials ==

/// @brief The exported name of a material: the artist's materialName, else
///        `Mat_<amat fileId>`, else `Mat_<index>`.
std::string material_name(const ModelMaterialCPU& mat);

/// @brief A material's shader profile and decoded blend -- the one decision
///        both the .glb's alphaMode and materials.json read.
struct MaterialShading {
    const ShaderProfile* profile = nullptr;
    BlendInfo blend;
};
MaterialShading material_shading(const ModelMaterialCPU& mat);

/// @brief The glTF and Poiyomi alpha cutoff: every clipping GW2 shader
///        discards on `saturate(2a) < 0.5`, i.e. a < 0.25.
inline constexpr double kAlphaCutoff = 0.25;

/// @brief One material as written to `materials[]`; index parallels `model.materials`.
/// @return The glTF material index for each `ModelMaterialCPU`.
std::vector<int> write_materials(GltfWriter& w, const ModelPreview& model,
                                 const std::vector<int>& texIndices);

// ======================================================================== mesh ==

/// @brief One exported submesh: its glTF mesh index and material binding.
struct MeshExportInfo {
    int meshIndex = -1;
    uint32_t materialIndex = 0;
    size_t sourceIndex = 0; // index into ModelPreview::meshes (write_meshes skips empty ones)
    /// (level, glTF mesh) for each level 1.. whose index set is the mesh's own
    /// (world_lods.h mesh_lod_source); levels it repeats are left out. Empty
    /// unless write_meshes was given lodLevels > 1.
    std::vector<std::pair<int, int>> lods;
};

/// @brief Write one glTF `mesh` (one primitive) per `ModelMeshCPU`, with
///        JOINTS_0/WEIGHTS_0 attributes added whenever the mesh `hasSkin`.
///        Geometry stays in the model's own local/bind space -- callers place
///        it via the referencing node's transform, never by pre-transforming
///        vertices.
///        With @p lodLevels > 1 (the world export only, GltfExportOptions::exportLods),
///        each mesh's own LOD1.. index sets become extra glTF meshes whose one
///        primitive shares the LOD0 primitive's attributes, material and morph
///        targets and has only its own `indices`; with 1 (the default) nothing changes.
std::vector<MeshExportInfo> write_meshes(GltfWriter& w, const ModelPreview& model,
                                         const std::vector<int>& materialIndices, int lodLevels = 1);

// =================================================================== skeleton ==

/// @brief One exported joint: its glTF node index.
struct JointExportInfo {
    int nodeIndex = -1;
};

/// @brief Write one glTF `node` per `ModelJoint` (translation/rotation/scale
///        copied straight from GW2's own values -- no conversion, since glTF
///        rotation is already a plain quaternion), parented per
///        `ModelJoint::parent` (roots left unparented; the caller adds them
///        as children of whatever node should own this skeleton).
/// @return Root joints' indices are `out[i]` for every `i` with `model.joints[i].parent < 0`.
std::vector<JointExportInfo> write_skeleton(GltfWriter& w, const ModelPreview& model,
                                            const std::string& namePrefix);

/// @brief Write the `skin` object for a skinned model: `joints` in exactly
///        `model.joints` order (matching `GVertex::bidx`, so no remapping is
///        needed) and `inverseBindMatrices` = each joint's own `invWorld`
///        (glTF wants the inverse bind matrix directly; GW2 already stores
///        exactly that).
/// @return The new skin's index.
int write_skin(GltfWriter& w, const ModelPreview& model, const std::vector<JointExportInfo>& joints);

/// @brief Bake every `ModelPreview::animClips` entry into a glTF `animation`,
///        sampling `castlemist::granny::sample()` at `fps` and writing dense
///        per-joint T/R/S keyframes (seconds, no tick conversion; quaternions
///        copied as-is, no Euler conversion).
void write_animations(GltfWriter& w, const ModelPreview& model,
                      const std::vector<JointExportInfo>& joints, int fps);

// =================================================================== particles ==

/// @brief Writes `<stem>_particles.json` next to `glbPath` describing every
///        baked particle cloud/emitter/effect-light (see particle_export.cpp's
///        file doc for the full field list and what's deliberately not
///        included). `glMaterialIndices` is `write_materials`'s return value,
///        so each cloud can record which already-embedded glTF material it
///        renders with.
/// @return The sidecar path written; empty when the model has no baked
///         effects (nothing is written) or the file couldn't be opened.
std::string write_particle_sidecar(const ModelPreview& model, const std::vector<int>& glMaterialIndices,
                                   const std::string& glbPath);

} // namespace castlemist::exportgltf
