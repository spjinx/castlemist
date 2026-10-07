/// @file
/// @brief decode_blend: bgfx blend state word -> Poiyomi rendering preset.
///
/// Pure bit-twiddling over the word's factor/equation fields (the same layout
/// make_blend_state_from_bgfx reads for RGB), pattern-matched against the
/// combinations seen in the game's materials.

#include "castlemist/exportgltf/blend_mode.h"

namespace castlemist::exportgltf {

namespace {

// bgfx factor ids as stored in the state word.
constexpr int kZero = 1, kOne = 2, kSrcColor = 3, kInvSrcColor = 4, kSrcAlpha = 5,
              kInvSrcAlpha = 6, kDstColor = 9, kInvDstColor = 10;
// Equations.
constexpr int kEqAdd = 0, kEqSub = 1, kEqRevSub = 2;

/// Closest preset for a blend no preset reproduces, chosen by destination factor.
BlendPreset nearest_by_destination(int dstRgb) {
    switch (dstRgb) {
        case kOne:         return BlendPreset::Additive;
        case kInvSrcAlpha: return BlendPreset::Fade;
        case kZero:        return BlendPreset::Opaque;
        default:           return BlendPreset::Fade;
    }
}

}  // namespace

BlendInfo decode_blend(uint64_t s, bool hasRenderState, bool isEffect, bool alphaTest) {
    BlendInfo b;
    b.alphaTest = alphaTest;
    auto opaque_or_cutout = [&] { return alphaTest ? BlendPreset::Cutout : BlendPreset::Opaque; };

    if (!hasRenderState) {
        b.preset = b.nearest = isEffect ? BlendPreset::Additive : opaque_or_cutout();
        return b;
    }

    b.srcRgb = int((s >> 12) & 0xF);
    b.dstRgb = int((s >> 16) & 0xF);
    b.srcA = int((s >> 20) & 0xF);
    b.dstA = int((s >> 24) & 0xF);
    b.eqRgb = int((s >> 28) & 0x7);
    b.eqA = int((s >> 31) & 0x7);

    const int src = b.srcRgb, dst = b.dstRgb;
    BlendPreset p = BlendPreset::Custom;
    bool exact = true;

    if (b.eqRgb == kEqAdd) {
        if (src == 0 && dst == 0) {
            p = opaque_or_cutout();
        } else if (src == kSrcAlpha && dst == kInvSrcAlpha) {
            p = alphaTest ? BlendPreset::TransClipping : BlendPreset::Fade;
        } else if (src == kOne && dst == kInvSrcColor) {
            p = BlendPreset::SoftAdditive;
            exact = false;  // 1 - srcColor, not Poiyomi's 1 - dstColor
        } else if (src == kOne && dst == kInvSrcAlpha) {
            p = BlendPreset::Transparent;  // premultiplied alpha
        } else if ((src == kSrcAlpha || src == kOne) && dst == kOne) {
            p = BlendPreset::Additive;
        } else if (src == kDstColor && dst == kSrcColor) {
            p = BlendPreset::Multiplicative2x;
        } else if (src == kDstColor && dst == kZero) {
            p = BlendPreset::Multiplicative;
        } else if (src == kInvDstColor && dst == kOne) {
            p = BlendPreset::SoftAdditive;
        }
    }

    if (p == BlendPreset::Custom) {
        // One/One with subtract or reverse subtract darkens, like a multiply.
        const bool darkens = (b.eqRgb == kEqSub || b.eqRgb == kEqRevSub) && dst == kOne;
        b.preset = BlendPreset::Custom;
        b.nearest = darkens ? BlendPreset::Multiplicative : nearest_by_destination(dst);
        b.exact = false;
    } else {
        b.preset = b.nearest = p;
        b.exact = exact;
    }
    return b;
}

const char* poiyomi_preset_name(BlendPreset p) {
    switch (p) {
        case BlendPreset::Opaque:           return "Opaque";
        case BlendPreset::Cutout:           return "Cutout";
        case BlendPreset::Fade:             return "Fade";
        case BlendPreset::TransClipping:    return "TransClipping";
        case BlendPreset::Transparent:      return "Transparent";
        case BlendPreset::Additive:         return "Additive";
        case BlendPreset::SoftAdditive:     return "Soft Additive";
        case BlendPreset::Multiplicative:   return "Multiplicative";
        case BlendPreset::Multiplicative2x: return "2x Multiplicative";
        case BlendPreset::Custom:           return "Custom";
    }
    return "Custom";
}

int poiyomi_mode_value(BlendPreset p) {
    switch (p) {
        case BlendPreset::Opaque:           return 0;
        case BlendPreset::Cutout:           return 1;
        case BlendPreset::Fade:             return 2;
        case BlendPreset::Transparent:      return 3;
        case BlendPreset::Additive:         return 4;
        case BlendPreset::SoftAdditive:     return 5;
        case BlendPreset::Multiplicative:   return 6;
        case BlendPreset::Multiplicative2x: return 7;
        case BlendPreset::TransClipping:    return 9;
        case BlendPreset::Custom:           return 2;
    }
    return 2;
}

int poiyomi_mode_value(const BlendInfo& b) {
    return poiyomi_mode_value(b.preset == BlendPreset::Custom ? b.nearest : b.preset);
}

GltfAlphaMode gltf_alpha_mode(const BlendInfo& b) {
    switch (b.preset == BlendPreset::Custom ? b.nearest : b.preset) {
        case BlendPreset::Opaque:        return GltfAlphaMode::Opaque;
        case BlendPreset::Cutout:
        case BlendPreset::TransClipping: return GltfAlphaMode::Mask;
        default:                         return GltfAlphaMode::Blend;
    }
}

}  // namespace castlemist::exportgltf
