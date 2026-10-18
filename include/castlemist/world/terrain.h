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

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

#include <nlohmann/json.hpp>

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

/// @brief One block of the terrain as a Unity heightfield (world-foliage spec D7,
///        docs/research/gw2-terrain-to-unity.md §5.1-5.2): the block's samples
///        resampled into Unity's row order, nothing interpolated.
///
/// The samples are the chunks' own vertex samples, so no triangle split is
/// assumed: the triangulation is the consumer's (Unity's TerrainCollider uses
/// its own diagonal, the mesh export NE-SW; terrainColliderDiagonal, UNPROVEN).
struct TerrainHeightfield {
    int cx0 = 0, cy0 = 0, blockChunks = 4, n = 129;   ///< n = blockChunks * segments + 1
    float rect[4]{};                                    ///< x0, y0, x1, y1 of the full (padded) block, map space
    std::vector<float> up;                              ///< n*n, Unity up = -z, row 0 SOUTH, column 0 west
    std::vector<uint8_t> real;                          ///< (n-1)^2 quads, row 0 SOUTH: 1 = inside the grid, 0 = padding
};

/// @brief The block whose north-west chunk is (@p cx0, @p cy0), @p blockChunks
///        chunks a side. Edge blocks are padded by clamping to the nearest real
///        sample (spec D7); their padded quads have real = 0. Segments come from
///        the chunks (samples - 1, all chunks agree on a built terrain). A
///        chunk with no heights gives up = 0 on its samples and real = 0 on its
///        quads. Empty (n = 0) when the terrain is not present.
TerrainHeightfield terrain_heightfield(const Terrain& t, int cx0, int cy0, int blockChunks = 4);

/// @brief Map-wide min and max of -z over every sample (Unity up, map units).
///        Both 0 when the terrain has no samples.
void terrain_up_range(const Terrain& t, float& hMin, float& hMax);

/// @brief Fill every chunk's TerrainMaterial from `trn.materials` and the
///        terrain's paged image (docs/research/gw2-world-frame.md §4).
///
/// `m.chunks[i]` belongs to `t.chunks[i]` (both index cy * chunksX + cx). Each
/// `texIndices` entry is bound by its texture's token: "color".."colord" ->
/// textureFileIds, "normal".."normald" -> normalFileIds, "blend" / "modx" ->
/// the layer 0 / 1 page of the PIMG file `m.pimgFileId` (read from @p dat with
/// @p tpl), at the entry's coord (in chunks) divided by the chunks per page.
/// A chunk with an index past `texFiles` stays `resolved = false`; it and
/// anything else that cannot be resolved (no PIMG, a missing page, a page
/// grid no chunks-per-page fits, a PIMG that does not decompress or parse)
/// is reported in @p warnings. Never throws for bad map data.
/// @throws DatIoError when the PIMG's bytes cannot be read from @p dat.
void resolve_terrain_materials(Terrain& t, const castlemist::model::Extractor::MapTerrainMaterials& m, Gw2Dat& dat,
                               const nlohmann::json& tpl, std::vector<std::string>& warnings);

/// @brief Distinct non-zero fileIds, first-seen, per chunk in index order:
///        textureFileIds[0..3], normalFileIds[0..3], pickerFileId, picker2FileId.
///        @p chunks (chunk indices) limits it to those chunks; null = all.
std::vector<uint32_t> terrain_texture_ids(const Terrain& t, const std::vector<size_t>* chunks = nullptr);

} // namespace castlemist::world
