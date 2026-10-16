/// @file
/// @brief Pure surface rules shared by the map view (render) and the glTF
///        exporter: dark-decal coverage, texture-transform scroll, water colour
///        and depth mix. Moved verbatim from render/world_materials.
/// @ingroup world
#pragma once

#include "castlemist/extract/model_types.h"

#include <cstdint>
#include <vector>

namespace castlemist::world {

/// @brief True when every texel of @p rgba (RGBA8) that survives the clip
///        (alpha >= 64) is black (each channel <= 16), and at least one
///        survives: a darkening decal's coverage mask, not a colour.
bool dark_coverage(const std::vector<uint8_t>& rgba);

/// @brief True when @p t is a plain scroll: columns == rows == count == 1,
///        scrollFreq == scale == scaleFreq == (0, 0), rotate == rotate2 == 0
///        (every entry of the census, note section 4). Anything else (flipbook,
///        scale, rotate) is not animated.
bool tex_transform_animates(const ModelTexTransformCPU& t);

/// @brief The map view's flat water, drawn when the map has a plane but no env
///        water preset (sub-project 2): linear RGB tint and alpha, castlemist's choice.
inline constexpr float kFlatWaterTint[3] = {0.30f, 0.55f, 0.85f};
/// @brief The flat water's alpha, also the preset water's base alpha (castlemist's choice).
inline constexpr float kFlatWaterAlpha = 0.45f;

/// @brief A water preset's stored colour as 0..1: bytes 0, 1, 2 as R, G, B
///        divided by 255 (order UNPROVEN, docs/research/gw2-world-materials.md
///        section 6). Byte 3 is ignored.
void water_colour(const uint8_t stored[4], float rgb[3]);

/// @brief How deep the water is, 0 shallow .. 1 deep: saturate((terrainZ -
///        planeZ) / depthAttenuation) (up is -Z); 0 when depthAttenuation <= 0
///        or not finite.
float water_depth_mix(float terrainZ, float planeZ, float depthAttenuation);

} // namespace castlemist::world
