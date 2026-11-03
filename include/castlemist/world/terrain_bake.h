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

/// @brief T*T*3 bytes: the chunk's ground normal maps ("normal", "normalb", "normalc", "normald",
///        TerrainMaterial::normalFileIds, slot k with colour slot k) blended with the albedo's
///        weights (terrain_blend), as a Unity tangent-space normal map: R = x, G = y flipped to
///        Unity's Y+ (as the props' maps, vrchat_maps kFlipNormalGreen), B = z. Same texel grid as
///        bake_chunk_albedo. An absent normal slot counts as flat. Returns false (flat 128, 128, 255
///        everywhere) when the chunk has no normal map or no colour slot (2026-10-27).
bool bake_chunk_normal(const TerrainChunk& c, const TextureLookup& tex, const BakeSettings& s,
                       std::vector<uint8_t>& rgbOut, std::vector<uint8_t>* specOut = nullptr,
                       std::vector<uint8_t>* wetOut = nullptr);

/// @brief The hi-res terrain shader's specular strength (AMAT 184988 ps 73, read 2026-10-27): the
///        normal maps' alpha, blended with the colour's weights, x this; x the sun colour and the
///        light buffer's specular term. @p specOut of bake_chunk_normal holds it per texel (0..255 =
///        0..1, T*T bytes; 0 where no normal map).
inline constexpr float kTerrainSpecularScale = 0.596078f;
// @p wetOut of bake_chunk_normal: the same shader's reflection mask, saturate((modx.g - 0.5) x 2) (its
// sky-cube reflection weight, x saturate(1 - N.V)); T*T bytes, 0..255 = 0..1 (2026-10-27).

/// @brief True when light.presetIndex names one of environment.water (pairing UNPROVEN).
bool water_has_preset(const WorldScene& s);

/// @brief True when the terrain under any texel centre of a texelsW x texelsH grid
///        over @p rect (row 0 north) is below the water plane (z > planeZ, up is -Z).
///        Texels off the terrain never count. The one rule for "this cell has water".
bool water_below_plane(const WorldScene& s, const float rect[4], int texelsW, int texelsH);

/// @brief (cw*T) x (ch*T) RGBA8 of the cell rect (x0, y0, x1, y1; row 0 north):
///        lerp(shallow, deep, water_depth_mix(terrainZ, planeZ, depthAttenuation)) with the
///        env preset light.presetIndex; alpha kFlatWaterAlpha + (1 - kFlatWaterAlpha) x the mix
///        (2026-10-25: the water shader's depth; 115 at the shore). Texels off the terrain count as
///        shallow. Returns false (nothing written) when !water_has_preset or
///        !water_below_plane.
bool bake_cell_water(const WorldScene& s, const float rect[4], int texelsW, int texelsH, bool srgbColour,
                     std::vector<uint8_t>& rgbaOut);

} // namespace castlemist::world
