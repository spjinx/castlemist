#ifndef CASTLEMIST_RIPPER_DYE_H
#define CASTLEMIST_RIPPER_DYE_H

// GW2 dye color math and texture baking. dye_matrix() is the game's 4x4 color
// shift (brightness/contrast, then a hue/saturation/lightness rotation),
// applied to colors in BGR order; it reproduces /v2/colors' precomputed `rgb`
// from `base_rgb` exactly. See docs/research/gw2-armor-skins-and-dyes.md
// section 4.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "castlemist/character/manifest.h"

namespace castlemist::ripper {

using ColorMatrix = std::array<std::array<double, 4>, 4>;

ColorMatrix dye_matrix(const character::DyeShift& shift);

/// Applies the matrix to one RGB color (truncated, clamped to 0..255).
std::array<uint8_t, 3> apply_dye(const ColorMatrix& m, std::array<uint8_t, 3> rgb);

/// Bakes up to four dye channels into an RGBA8 image in place: for each texel
/// and channel i with both a mask and a dye, `out = lerp(out, M_i(base), w)`
/// where `w` is the mask texel's luminance (masks decode to gray RGB) and
/// `base` is the undyed texel. Alpha is left untouched. Masks must be w*h RGBA8.
void bake_dyes(std::vector<uint8_t>& rgba, int w, int h, const std::array<const std::vector<uint8_t>*, 4>& masks,
               const std::array<std::optional<ColorMatrix>, 4>& dyes);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_DYE_H
