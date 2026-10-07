/// @file
/// @brief Per-shader (AMAT) texture conventions for the VRChat export.
///
/// GW2 packs different things into the same texture roles depending on the
/// shader: the "mask" layer is metal/gloss/sheen/glow on armor, a glow gate on
/// legacy weapons, an opacity map on effects. The table behind profile_for is
/// keyed by the AMAT fileId and comes from docs/research/gw2-material-channels.md
/// sections 4 and 5.
/// @ingroup exportgltf

#pragma once

#include "castlemist/extract/model_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::exportgltf {

/// What the diffuse alpha channel carries.
enum class AlphaUse {
    HolesAndShine,   ///< 0-63 holes, 128-255 shine
    Shine,           ///< shine only, never holes
    ReflectionOnly,  ///< shine drives reflection strength only
    Intensity,       ///< premultiplied effect intensity
    Opacity,
    /// 44709: opacity = `saturate(2a)` (x ramp x diffade, not mapped) in the lower
    /// half, unlit self-illumination `rgb * saturate(2a-1) * 2` in the upper half.
    OpacityAndGlow,
    InteriorWeight,  ///< jade: lerp(diffuse, interior parallax, a)
    Unused
};

/// A channel of a texture, or None when the profile does not read one.
enum class Channel : int8_t { None = -1, R = 0, G = 1, B = 2, A = 3 };

/// A specular layer whose alpha carries gloss or exponent (weapon-spec / legacy-spec).
enum class SpecLayer { None, GlossInAlpha, ExponentInAlpha };

/// How a shader blends its `decal` layer (usually on UV1) with the diffuse (UV0).
/// docs/research/gw2-material-channels.md sections 4 (54632) and 8 (prop-decal, 57131).
enum class DecalMode {
    None,
    /// map props: `albedo = lerp(diffuse, decal, coverage)`, coverage =
    /// `saturate(2 * decal.a)` [x the decal mask channel]; the shine lerps
    /// toward `saturate(2 * decal.a - 1)` by the same coverage.
    DecalOverDiffuse,
    /// 54632 / 57131: `albedo = lerp(decal, diffuse, decal.a)`, so the decal's
    /// coverage is `1 - decal.a` (alpha 1 shows the diffuse).
    DiffuseOverDecal
};

/// Where a decal also glows.
enum class DecalGlow {
    None,
    /// 57131: emission = `decal.rgb * glowcol * 2 * (1 - decal.a)`.
    BelowHalf,
    /// 57806: adds `decal.rgb * saturate(2 * decal.a - 1)` unlit.
    AboveHalf
};

/// Which channels of a `cutout` layer make the clip value (note sections 8.2, 8.3).
enum class CutoutChannels {
    R,    ///< 53858: `cutout.R` (x `saturate(2 * diffuse.a)`, kept in BaseColor)
    RxA   ///< 511663: `cutout.R * cutout.A` (x `cutfade`)
};

/// How one shader family uses its textures.
struct ShaderProfile {
    std::string name;
    bool supported = true;  ///< false: needs its own pass, export with a warning
    bool clips = false;     ///< alpha-tested (holes below alpha 64)
    AlphaUse diffuseAlpha = AlphaUse::Shine;
    Channel maskMetal = Channel::None, maskGloss = Channel::None, maskSheen = Channel::None,
            maskGlow = Channel::None, maskGlowGate = Channel::None;
    /// The layer role the maskMetal/maskGloss/maskSheen/maskGlow/maskGlowGate channels
    /// read ("mask" on armor; 3121953 reads "metalmask").
    std::string maskRole = "mask";
    SpecLayer specLayer = SpecLayer::None;
    int opacityTexture = -1;               ///< index into textureFileIds whose R is opacity (legacy-untagged: 2)
    bool premultiplyRgbByAlpha = false;
    bool glowOnUv2MaskOnUv0 = false;       ///< weapon-spec swaps them
    bool animatedGlowLayers = false;       ///< legendary: animated glow layers, not mapped
    DecalMode decalMode = DecalMode::None;
    /// DecalOverDiffuse only: a layer whose channel multiplies the decal coverage
    /// (19910 "decalmask".R, 69887 "mask".R, 60530 "blend".G); empty when none.
    std::string decalMaskRole;
    Channel decalMaskChannel = Channel::None;
    DecalGlow decalGlow = DecalGlow::None;
    bool decalParallax = false;  ///< decal UV parallax-offset by `pardist` (54632, 57131): not mapped
    /// A `mask` channel that multiplies the specular (69887: B): not mapped, warned.
    Channel maskSpecular = Channel::None;
    /// A layer, on its own UV, whose value the shader discards below 0.5
    /// (511663, 53858: role "cutout"); empty when none. Its value ships as
    /// MaterialMaps::alphaMask; the material is alpha-tested (alpha_tested).
    std::string cutoutRole;
    CutoutChannels cutoutChannels = CutoutChannels::R;
    /// The layer role whose RGBA is the base colour; empty = the diffuse (842652:
    /// "parallax"). `diffuseAlpha` then describes that layer's alpha, and the
    /// castlemist diffuse ships raw as an extra with the hint `diffuseUse`.
    std::string baseColorRole;
    /// With baseColorRole: what the castlemist diffuse really is (842652: "uv-offset").
    std::string diffuseUse;
    /// A layer, on its own UV, whose channel multiplies the opacity (842652:
    /// "mask" R); empty when none. Ships as MaterialMaps::alphaMask with cutoff
    /// -1 (opacity). A cutout layer takes the alphaMask first.
    std::string opacityRole;
    Channel opacityChannel = Channel::None;
    /// The colour pass applies no lighting (fog only): not mapped, warned.
    bool unlit = false;
};

/// True when the material discards: on its diffuse alpha (`clips`) or on a cutout layer.
inline bool alpha_tested(const ShaderProfile& p) { return p.clips || !p.cutoutRole.empty(); }

/// Profile for a material: by AMAT fileId, else the legacy-untagged trait
/// (materialId 0, flags 0, every extra texture role empty, SrcAlpha/InvSrcAlpha
/// in `renderState`), else default_profile().
const ShaderProfile& profile_for(const ModelMaterialCPU& mat, uint64_t renderState);

/// Every AMAT fileId in the table, in table order, duplicates preserved (for integrity tests).
std::vector<uint32_t> all_profile_amats();

/// Name "default": Shine, no clip, nothing interpreted.
const ShaderProfile& default_profile();

}  // namespace castlemist::exportgltf
