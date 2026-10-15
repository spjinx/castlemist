/// @file
/// @brief WorldView's texture cache: one GPU texture per dat fileId, uploaded
///        from a worker-decoded CappedTexture.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "castlemist/native/granny_pose.hpp"

struct CappedTexture;      // castlemist/extract/model_types.h
struct ModelMaterialCPU;   // castlemist/extract/model_types.h
struct ModelTexTransformCPU;   // castlemist/extract/model_types.h
struct ModelPreview;       // castlemist/extract/model_types.h

namespace castlemist::render {

struct WorldMaterialSettings;   // castlemist/render/world_prep.h

/// @brief How a prop material draws in the material pass (spec PropMaterialPass).
///        Opaque and Clip in pass 1; the blended modes in pass 2, depth writes
///        off, each with its preset's exact factors (ruling P7). Unmapped (a
///        blend no mode reproduces exactly) draws in the AO look and is counted.
enum class WorldAlpha : uint8_t {
    Opaque,          ///< no blend
    Clip,            ///< discard below diffuse alpha 0.25
    Blend,           ///< SrcAlpha / InvSrcAlpha (Fade, TransClipping)
    Additive,        ///< SrcAlpha / One
    AdditiveOne,     ///< One / One
    Premultiplied,   ///< One / InvSrcAlpha (Transparent)
    Multiply,        ///< DstColor / Zero (Multiplicative)
    Multiply2x,      ///< DstColor / SrcColor (Multiplicative2x)
    Unmapped,        ///< Custom or SoftAdditive: no exact factors, drawn in the AO look
    Count
};

/// @brief True when every texel of @p rgba (RGBA8) that survives the clip
///        (alpha >= 64) is black (each channel <= 16), and at least one
///        survives: a darkening decal's coverage mask, not a colour.
bool dark_coverage(const std::vector<uint8_t>& rgba);

/// @brief The alpha mode of @p mat whose diffuse texture has
///        CappedTexture::hasCutout = @p diffuseHasCutout and
///        looksLikeEffect = @p diffuseLooksLikeEffect, decided as the glTF
///        export does (exportgltf: profile_for, alpha_tested, decode_blend).
///
/// isEffect = mat.isEffect, or with no render state the texture's
/// looksLikeEffect (model_preview.cpp's rule for decoded textures, which a
/// header-only load cannot apply). Without a render state: cutout -> Clip,
/// else effect -> Additive, else Opaque (material_export.cpp's order). With
/// one, decode_blend's preset: Opaque -> Opaque, Cutout
/// -> Clip, Fade / TransClipping -> Blend, Additive -> Additive (SrcAlpha / One)
/// or AdditiveOne (One / One), Transparent -> Premultiplied, Multiplicative ->
/// Multiply, Multiplicative2x -> Multiply2x, Custom and SoftAdditive ->
/// Unmapped (ruling P7: no guessed factors). Without a render state, a cutout
/// whose diffuse is @p diffuseDarkCoverage (dark_coverage) blends instead of
/// clipping: clipped, a black decal is a solid black splat (ruling P7).
WorldAlpha world_alpha_mode(const ModelMaterialCPU& mat, bool diffuseHasCutout, bool diffuseLooksLikeEffect,
                            bool diffuseDarkCoverage = false);

/// @brief One prop material's diffuse, as the material pass samples it.
struct PropMaterial {
    uint32_t diffuseFileId = 0;   ///< ModelPreview::textures[diffuseTex].fileId
    uint8_t uvSet = 0;            ///< 0 = MatVtx uv0, 1 = uv1
    bool textured = false;        ///< has a diffuse on UV0 or UV1
    int scroll = -1;              ///< the texTransforms entry that scrolls it (first with uvIndex == uvSet that animates)
    float scrollRate[2] = {0, 0}; ///< that entry's scroll, UV per unit
};

/// @brief True when @p t is a plain scroll: columns == rows == count == 1,
///        scrollFreq == scale == scaleFreq == (0, 0), rotate == rotate2 == 0
///        (every entry of the census, note section 4). Anything else (flipbook,
///        scale, rotate) is not animated.
bool tex_transform_animates(const ModelTexTransformCPU& t);

/// @brief The UV offset of scroll entry @p t at @p seconds:
///        frac(scroll * seconds / unitSeconds), frac(x) = x - floor(x), in
///        double (UNPROVEN unit, note section 4). A unitSeconds <= 0 gives (0, 0).
void uv_scroll_offset(const ModelTexTransformCPU& t, double seconds, float unitSeconds, float out[2]);

/// @brief One PropMaterial per ModelPreview::materials entry (same index):
///        textured when diffuseTex names a texture of @p m and diffuseUv <= 1
///        (MatVtx carries two UV sets; a UV2+ diffuse draws untextured).
///        @p stillEntries (when non-null) gains the count of texTransforms
///        entries that do not animate (drawn still, note section 4).
std::vector<PropMaterial> prop_materials(const ModelPreview& m, int* stillEntries = nullptr);

/// @brief Draw order for pass 2: indices of @p nearestDistance, farthest
///        first; equal distances keep index order.
std::vector<size_t> blend_back_to_front(const std::vector<float>& nearestDistance);

/// @brief One (model, clip) pose source (spec SkinnedPropPass), built in
///        WorldView::add_models from the kept preview: the bones view
///        {name, parent, localPos, localQuat, localScale, invWorld} of each
///        ModelJoint (as skeleton.cpp's pose_bones), the clip and its tracks
///        by name. The pointers borrow the preview, which the view keeps.
struct PoseRig {
    std::vector<castlemist::granny::PoseBone> bones;
    const castlemist::granny::Anim* clip = nullptr;
    std::unordered_map<std::string, int> trackByName;
};

/// @brief @p seconds wrapped into [0, @p duration) (fmod, negatives wrapped
///        up), as gw2bgfx_view.cpp loops a clip. A duration <= 1e-6 or a
///        non-finite input gives 0. Every instance starts at t = 0 (UNPROVEN
///        phase, note section 5).
double clip_time(double seconds, float duration);

/// @brief The skin palette of @p rig at @p seconds (clip_time of the clip's
///        duration): 16 floats per bone, granny::skinMatrix (row-major,
///        row-vector, translation in the last row). A bone whose matrix is
///        not finite gets identity, so the palette is always finite.
void pose_palette(const PoseRig& rig, double seconds, std::vector<float>& palette);

/// @brief A water preset's stored colour (surfaceShallowColor,
///        surfaceDeepColor) as linear-unscaled 0..1: bytes 0, 1, 2 as R, G, B
///        divided by 255, the order parseMapEnv reads env colours
///        (gw2model.hpp:1300). Byte 3 is ignored. The order is UNPROVEN for
///        water (docs/research/gw2-world-materials.md §6: the game shader's
///        wtrclrs/wtrclrd are filled by the client, not stored).
void water_colour(const uint8_t stored[4], float rgb[3]);

/// @brief One bump sample's UV at map position (@p x, @p y):
///        uv = R(@p angle) (x, y) @p tile / @p tileUnit + (@p speed @p seconds, 0),
///        R the counter-clockwise rotation, @p angle in radians. The units of
///        bumpTile / bumpSpeed / bumpAngle are UNPROVEN (note §6: AMAT 187551's
///        VS reads wtrprmv/wtrprmd/wtrprme, which the preset stores as zero and
///        the client fills per frame from fields we cannot see). A tileUnit
///        <= 0 gives (speed seconds, 0).
void water_bump_uv(float x, float y, float tile, float speed, float angle, double seconds, float tileUnit,
                   float uv[2]);

/// @brief How deep the water is, 0 shallow .. 1 deep: saturate((terrainZ -
///        planeZ) / depthAttenuation) (up is -Z, so terrain below the plane
///        has the larger z); 0 when depthAttenuation <= 0 or not finite.
float water_depth_mix(float terrainZ, float planeZ, float depthAttenuation);

/// @brief One line per setting of @p s: its name and value, "UNPROVEN" or
///        "castlemist's choice", and the note section
///        (docs/research/gw2-world-materials.md); then fixed lines for the
///        UNPROVEN rules that are not settings (§5 clip start, §1.6
///        pickerSolid order, §6 water preset pairing, §1.5 uvData).
std::vector<std::string> material_setting_warnings(const WorldMaterialSettings& s);

/// @brief fileId -> texture (spec "render/world_materials"). Each texture is
///        R8G8B8A8_TYPELESS with every mip of the CappedTexture and two views:
///        UNORM (normal maps, pages, masks) and UNORM_SRGB (colour textures).
///        A fileId is taken once: later adds of it, ready or failed, are
///        ignored. Lives on the device's thread.
class TextureCache {
public:
    enum class State { Unknown, Ready, Failed };

    /// @brief Uploads @p t on @p dev, once per fileId. Empty mips (the decode
    ///        failed), mip sizes that do not halve from width x height, or a
    ///        failed texture creation mark the fileId Failed. Nothing happens
    ///        with a null device or fileId 0.
    void add(ID3D11Device* dev, const CappedTexture& t);
    /// @brief The view of @p fileId (UNORM_SRGB when @p srgb), or null unless Ready.
    ID3D11ShaderResourceView* srv(uint32_t fileId, bool srgb) const;
    State state(uint32_t fileId) const;
    /// @brief The CappedTexture's coverage flags, kept with the texture (the
    ///        prop pass resolves its alpha mode from them); all false unless Ready.
    struct Info {
        bool hasCutout = false, looksLikeEffect = false;
        bool darkCoverage = false;   ///< dark_coverage(mip 0), measured on upload
    };
    Info info(uint32_t fileId) const;
    int ready() const { return ready_; }
    int failed() const { return failed_; }
    /// @brief GPU bytes of the Ready textures: the sum over their mips of w*h*4.
    size_t bytes() const { return bytes_; }
    /// @brief Releases every texture; all counts back to 0.
    void clear();

private:
    struct Entry {
        State state = State::Unknown;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> unorm, srgb;
        Info info;
    };
    std::unordered_map<uint32_t, Entry> map_;
    int ready_ = 0, failed_ = 0;
    size_t bytes_ = 0;
};

} // namespace castlemist::render
