/// @file
/// @brief CPU bake of each export cell's terrain albedo (on terrain_blend) and
///        water colour texture (pure CPU).
/// @ingroup world
#pragma once

#include "castlemist/extract/model_types.h"
#include "castlemist/world/terrain_blend.h"
#include "castlemist/world/world_scene.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace castlemist::world {

/// @brief Texture by dat file id; null = not available.
using TextureLookup = std::function<const CappedTexture*(uint32_t fileId)>;

/// @brief Colour space: with srgbColour (the default, the map view's
///        srgbColourTextures) ground textures and water colours are decoded
///        sRGB -> linear, blended in linear and the result is encoded back, so the
///        output bytes are sRGB (what a Unity albedo PNG expects). Picker and modx
///        pages are data and never decoded. With srgbColour false nothing is
///        decoded or encoded (raw bytes in, raw bytes out).
struct BakeSettings {
    int texelsPerChunk = 256;
    float groundRepeats = kGroundRepeatsPerChunk;
    bool srgbColour = true;
};

/// @brief clamp(lround(log2(repeats * groundWidth / texelsPerChunk)), 0, mipCount - 1).
int ground_mip_level(int groundWidth, float repeats, int texelsPerChunk, int mipCount);

/// @brief Bilinear on mip @p mip (clamped to the chain) at (u, v), texel centres at
///        (i+0.5)/w; wrap or clamp; sRGB -> linear on RGB (before filtering) when @p srgb.
BlendTexel sample_bilinear(const CappedTexture& t, int mip, float u, float v, bool wrap, bool srgb);

/// @brief 12.92 / 1.055 pow(1/2.4) - 0.055; inverse of the render's srgb_to_linear.
float linear_to_srgb(float c);

/// @brief T*T*3 bytes RGB, row 0 north, T = texelsPerChunk. Texel (i, j) samples
///        s = (i+0.5)/T, t = (j+0.5)/T. A page without a file (or whose texture is
///        unavailable) uses pickerSolid / picker2Solid bytes / 255 (stored order,
///        UNPROVEN), as the map view does. Returns false and writes byte 128
///        everywhere when the chunk is unresolved or no colour slot is available.
bool bake_chunk_albedo(const TerrainChunk& c, const TextureLookup& tex, const BakeSettings& s,
                       std::vector<uint8_t>& rgbOut);

/// @brief (cw*T) x (ch*T) RGBA8 of the cell rect (x0, y0, x1, y1; row 0 north):
///        lerp(shallow, deep, water_depth_mix(terrainZ, planeZ, depthAttenuation)) with the
///        env preset light.presetIndex; alpha 115 (0.45). Texels off the terrain count as
///        shallow. Returns false (nothing written) when there is no preset or no texel's
///        terrain is below the plane (z > planeZ).
bool bake_cell_water(const WorldScene& s, const float rect[4], int texelsW, int texelsH, bool srgbColour,
                     std::vector<uint8_t>& rgbaOut);

} // namespace castlemist::world
