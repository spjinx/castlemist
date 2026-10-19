/// @file
/// @brief Material thumbnails: a sphere drawn offscreen with a material's own
///        game shaders, for the Materials window's previews and pickers.

#pragma once

#include <cstdint>
#include <vector>

#include "castlemist/extract/model_types.h"

namespace castlemist::render {

/// @brief Draws a unit sphere with @p mat's game shaders (VS, PS, textures,
///        constants and blend state) under the current lighting rig, through
///        the same post pass as Shader mode, into @p rgba (`size * size * 4`,
///        RGBA8, top row first).
///
/// @p textures supplies the pixels GameSamplerCPU::gameTex indexes. Deferred
/// materials get a light buffer computed for the sphere (SH ambient + sun),
/// as the light pre-pass would give them. Needs initialize() to have run;
/// false when the device is missing or @p mat has no usable shader.
bool render_material_preview(const GameMaterial& mat, const ModelPreview& textures, int size,
                             std::vector<uint8_t>& rgba);

} // namespace castlemist::render
