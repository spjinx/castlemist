/// @file
/// @brief The world export's optional `unityTerrain` block: one Unity Terrain
///        tile per block of cell_block_chunks chunks (world-foliage spec D7,
///        "Output folder additions", "manifest.json additions";
///        docs/research/gw2-terrain-to-unity.md §5.1-5.3).
///
/// Files under `terrain/`, `<tile>` = `t<cx0>_<cy0>` (the block's north-west
/// chunk):
/// - `<tile>.r16`: n x n uint16 LE (n = blockChunks * segments + 1, 129 at 4 x 4),
///   row 0 south, column 0 west, `round((up - hMin) / (hMax - hMin) * 65535)` with
///   up = -z and hMin / hMax the map-wide range (world::terrain_up_range). They
///   are vertex samples: no triangle split is assumed (terrainColliderDiagonal).
/// - `<tile>_holes.png`: (n-1) x (n-1) 8-bit grey, white = surface, row 0 north;
///   edge tiles only (the padded quads are black).
/// - `<tile>_albedo.png`: the block's cell albedos assembled pixel-exact
///   (texelsPerChunk per chunk, row 0 north; padded chunks mid-grey).
/// - `<tile>_d<k>.png`: detailResolution^2 8-bit counts of detail prototype k,
///   row 0 north, cell (floor((x - tileX0)/48), floor((tileY1 - y)/48)), clamped to
///   255 (one warning per tile and prototype that clamps).
/// - `<tile>_trees.bin`: 24-byte records, u32 prototype, f32 u = (x - tileX0)/size,
///   v = (y - tileY0)/size, h = (-z - hMin)/(hMax - hMin), yaw = unityYaw,
///   scale = unity.s.
///
/// A record is a detail when its layer type is 1 or 2 and its model is ok with
/// one mesh and one material (foliage.models[].terrainDetail); a Terrain tree
/// when its layer type is 3 and tiltDeg <= kTerrainTreeMaxTiltDeg (it gets flag
/// bit 3). Records with bit 4 (outside the terrain) are neither.
/// @ingroup exportgltf
#pragma once

#include "castlemist/exportgltf/world_foliage.h"
#include "castlemist/exportgltf/world_manifest.h"
#include "castlemist/world/cells.h"
#include "castlemist/world/world_scene.h"

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

inline constexpr float kTerrainTreeMaxTiltDeg = 5.0f;   ///< castlemist's choice (spec D7, terrainTreeMaxTiltDeg)
inline constexpr float kDetailCellUnits = 48.0f;        ///< the zone grid cell (spec D7)
inline constexpr size_t kTerrainTreeRecordBytes = 24;
inline constexpr int kUnityTerrainCheckPoints = 32;

struct UnityTerrainInput {
    const castlemist::world::WorldScene* scene = nullptr; const castlemist::world::CellPlan* plan = nullptr;
    const std::vector<std::vector<uint8_t>>* cellAlbedoRgb = nullptr;   // parallel to plan->cells, T*T*3 per chunk area
    int texelsPerChunk = 256;
    std::vector<std::vector<FoliageRecord>>* records = nullptr;          // per cell; bit 3 set here
    const std::vector<ManifestModel>* models = nullptr;                  // meshes / materialCount for terrainDetail
};

/// Writes terrain/<tile>.r16, _holes.png (edge tiles only), _albedo.png, _d<k>.png and _trees.bin (spec D7,
/// Data formats) and returns the manifest's "unityTerrain" block; json(nullptr) and no files when no terrain.
///
/// - Records name models by their position in @p in.models (as the foliage bins do).
/// - `detailPrototypes` / `treePrototypes` are map-wide, in models[] order, each with the
///   largest stored fadeRange[1] of its records' layers (`fadeFar`, map units, not scaled):
///   the block's `fadeQualityScale` is the settings' zoneFadeQualityScale, which the
///   importers multiply in by default (ruling S5).
/// - `check`: 32 heightmap sample vertices (ruling S6) in the tiles' real (unpadded)
///   area, drawn (tile, column, row) from the LCG
///   state = state * 6364136223846793005 + 1442695040888963407 seeded with the map
///   fileId (each draw takes the top 53 bits as a fraction; padded vertices are
///   redrawn), height = the sample's up value (= -terrain_height_at there): Unity
///   [x, z] = map [x, y] and absolute Unity y, map units before unityScale. On a
///   vertex the expected height holds for any triangle split.
/// - A heightmap or detail resolution Unity cannot take (n not 2^k + 1) gives null
///   and a warning; a failed write gives null and @p error (the export stops).
nlohmann::json write_unity_terrain(const UnityTerrainInput& in, const std::string& dirUtf8,
                                   std::vector<std::string>& warnings, std::string* error = nullptr);

/// @brief The settings rows that come with the unityTerrain block (spec D11):
///        terrainTreeMaxTiltDeg, terrainTreeYawSign, detailPlacement,
///        terrainHeightRowOrder, terrainColliderDiagonal.
std::vector<ManifestSetting> unity_terrain_settings();

} // namespace castlemist::exportgltf
