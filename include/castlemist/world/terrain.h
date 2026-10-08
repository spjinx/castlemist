/// @file
/// @brief Terrain geometry: how a map's `trn` height samples become
///        WorldScene::terrain chunks in map space.
///
/// The rules are proven in docs/research/gw2-world-frame.md §3:
/// - each chunk stores (segments + 3)^2 samples; its own (segments + 1)^2
///   are the inner ones (the outer ring is an apron shared with neighbours);
/// - the chunk grid is chunksX x chunksY with chunk index = cy * chunksX + cx,
///   cx growing east from `rect[0]`, cy growing **south** from `rect[3]`;
/// - inside a chunk, column 0 is its west edge and row 0 its **north** edge;
/// - heights are map-space Z as stored (up = -Z, §1).
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::world {

/// @brief The shape of a map's terrain sample array.
struct TerrainLayout {
    int chunksX = 0, chunksY = 0;   ///< chunk grid (east-west, north-south)
    int segments = 0;               ///< quads per chunk side; a chunk has segments + 1 own samples per side
    int stored = 0;                 ///< samples per chunk side in `heightMapArray` (segments + 3)
    bool ok = false;
    std::string why;                ///< why the layout was rejected (empty when ok)
};

/// @brief Work out the chunk layout from `trn.dims`, `trn.verticesPerChunkSide`
///        and the number of stored height samples (§3.2).
/// @param vertsPerChunkSide the template field, or 0 when the map has none; then
///        `segments` is the unique s with dims divisible by s and
///        heightCount == (dimX/s) * (dimY/s) * (s+3)^2.
TerrainLayout terrain_layout(uint32_t dimX, uint32_t dimY, uint32_t vertsPerChunkSide, size_t heightCount);

/// @brief Build WorldScene::terrain from a parsed `trn` + `parm.rect` (§3).
///        Never invents a rect or a layout: a map without a `parm` rect, or
///        whose samples do not fit a layout, gets `present = false` and a
///        message in @p warnings.
Terrain build_terrain(const castlemist::model::Extractor::MapTerrain& t, std::vector<std::string>& warnings);

/// @brief The terrain height (map-space Z, as stored) at map-space (x, y):
///        bilinear over the samples of the chunk that contains the point (§3.4).
/// @param inside set to false, and NaN returned, when (x, y) is outside every
///        chunk (or the terrain is not present); true otherwise. Points on the
///        terrain's outer edge are inside.
float terrain_height_at(const Terrain& t, float x, float y, bool* inside = nullptr);

} // namespace castlemist::world
