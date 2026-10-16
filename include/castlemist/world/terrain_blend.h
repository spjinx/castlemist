/// @file
/// @brief The game's terrain texture blend and the chunk UV rules (pure CPU).
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

namespace castlemist::world {

/// @brief Ground repeats per chunk. UNPROVEN: T3D's hard-coded guess
///        (TerrainRenderer.ts:405-406), note §1.6. The render and export default.
inline constexpr float kGroundRepeatsPerChunk = 8.0f;

/// @brief One texture sample, linear 0..1, as decoded.
struct BlendTexel { float rgba[4]; };

/// @brief Inputs of the terrain blend (note §1.2).
struct TerrainBlendInput {
    BlendTexel picker, modx;                      ///< page samples at the chunk's page UV
    BlendTexel ground[4];                         ///< color, colorb, colorc, colord at the ground UV
    bool present[4] = {true, true, true, true};   ///< false: that slot binds no texture (fileId 0)
};

/// @brief The lo-res PS 48 blend, note §1.2, no clamp. Picker R weights
///        ground[3] (colord), G ground[2] (colorc), B ground[1] (colorb), A
///        ground[0] (color). Ground alpha biases the weight (h = a + 0.01).
///        An absent slot gets weight 0 (castlemist rule); if every weight is
///        then 0, the note's 1/255 goes to color, or to the first present slot.
///        Then modx.g saturation and modx.r brightness.
/// @param weightsOut weight of ground[i], summing to 1.
void terrain_blend(const TerrainBlendInput&, float albedoOut[3], float weightsOut[4]);

/// @brief Page UV = pickerOffset + pickerScale * (s, t); s from the west edge,
///        t from the north edge, both in [0,1]. No edge inset (T3D's 1/1024 is
///        UNPROVEN, note §1.6).
void page_uv(const TerrainMaterial&, float s, float t, float uv[2]);

/// @brief Ground UV = (s, t) * repeats (note §1.5-1.6).
void ground_uv(float s, float t, float repeats, float uv[2]);

} // namespace castlemist::world
