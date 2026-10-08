/// @file
/// @brief Decode a GW2 material's bgfx blend state word into a Poiyomi Toon
///        rendering preset (and the matching glTF alphaMode).
/// @ingroup exportgltf

#pragma once

#include <cstdint>

namespace castlemist::exportgltf {

/// Poiyomi Toon rendering presets. Custom means no preset reproduces the
/// blend; `BlendInfo::nearest` then names the closest one.
enum class BlendPreset {
    Opaque, Cutout, Fade, TransClipping, Transparent,
    Additive, SoftAdditive, Multiplicative, Multiplicative2x, Custom
};

/// Result of decode_blend: the chosen preset plus the raw bgfx factors.
struct BlendInfo {
    BlendPreset preset = BlendPreset::Opaque;
    BlendPreset nearest = BlendPreset::Opaque;  ///< == preset unless preset is Custom
    bool exact = true;                          ///< false: preset is only an approximation
    /// Raw bgfx factors (1 Zero, 2 One, 3 SrcColor, 4 InvSrcColor, 5 SrcAlpha,
    /// 6 InvSrcAlpha, 7 DstAlpha, 8 InvDstAlpha, 9 DstColor, 10 InvDstColor; 0 unset).
    int srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0;
    int eqRgb = 0, eqA = 0;  ///< 0 Add, 1 Sub, 2 RevSub, 3 Min, 4 Max
    bool alphaTest = false;  ///< from the shader profile
};

/// Decode a bgfx state word. With `hasRenderState == false` (no game shader)
/// the word is ignored: Additive when `isEffect`, else Opaque/Cutout.
BlendInfo decode_blend(uint64_t bgfxState, bool hasRenderState, bool isEffect, bool alphaTest);

/// Poiyomi preset display name ("Soft Additive", "2x Multiplicative", ...).
const char* poiyomi_preset_name(BlendPreset);

/// Poiyomi `_Mode` value for the preset. Custom has no value of its own, so
/// this returns Fade's; use the BlendInfo overload to resolve Custom.
int poiyomi_mode_value(BlendPreset);

/// `_Mode` value for a decoded blend: Custom resolves to its `nearest`.
int poiyomi_mode_value(const BlendInfo&);

enum class GltfAlphaMode { Opaque, Mask, Blend };

/// glTF alphaMode for a decoded blend (Custom uses `nearest`).
GltfAlphaMode gltf_alpha_mode(const BlendInfo&);

}  // namespace castlemist::exportgltf
