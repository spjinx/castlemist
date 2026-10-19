/// @file
/// @brief Terrain shading helpers: seam-aware per-sample normals.
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <vector>

namespace castlemist::world {

/// @brief The chunk at grid (cx, cy), or nullptr.
const TerrainChunk* find_chunk(const Terrain& t, int cx, int cy);

/// @brief One unit normal per sample (3 floats each, samples*samples*3), map
///        space, up-dominant (n.z < 0 on flat ground). Central differences,
///        reaching into the neighbouring chunk at edges (it shares our edge
///        sample, so its next sample in is used); one-sided without a neighbour.
std::vector<float> chunk_normals(const Terrain& t, size_t chunkIndex);

} // namespace castlemist::world
