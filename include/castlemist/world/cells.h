/// @file
/// @brief Export cells: page-aligned blocks of terrain chunks, split by a
///        triangle and texture budget, with props and collision bucketed in.
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace castlemist::world {

/// @brief Per-cell budget. castlemist's choice; VRChat states no per-area limit (UNPROVEN).
struct CellBudget { int maxChunks = 4; uint64_t maxTriangles = 1'000'000; double maxTextureMB = 256.0; };

/// @brief Measured costs the partition needs; filled by the caller.
struct CellStats {
    std::vector<uint64_t> modelTriangles;                ///< parallel to scene.models (LOD0)
    std::vector<std::vector<uint32_t>> modelTextures;    ///< distinct texture fileIds per model
    std::unordered_map<uint32_t, uint64_t> textureBytes; ///< RGBA8 + mips at the cap: w*h*4*4/3
    uint64_t bakedBytesPerChunk = 0;                     ///< albedo + water textures per chunk
};

/// @brief Inclusive chunk rectangle; default (cx1 < cx0) means the whole map.
struct ChunkRegion {
    int cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1;
    bool whole() const { return cx1 < cx0; }
};

struct Cell {
    std::string id;                  ///< c<cx0>_<cy0>_<w>x<h> | c_all
    int cx0 = 0, cy0 = 0, w = 0, h = 0; bool all = false;
    float rect[4] = {0, 0, 0, 0};    ///< map space x0,y0,x1,y1
    float origin[3] = {0, 0, 0};     ///< rect centre, z 0
    std::vector<uint32_t> props, collision;   ///< indices into scene.props / scene.collision.instances
    uint64_t triangles = 0; double textureMB = 0; bool split = false, overBudget = false;
};

struct CellPlan {
    std::vector<Cell> cells;
    int blockChunks = 4;
    size_t propsOutsideRegion = 0;   ///< props + collision instances in cells the region dropped
    std::vector<std::string> warnings;
};

/// @brief 1/pickerScale when every chunk with a pickerScale agrees on it and it is an
///        integer in [1, maxChunks]; else maxChunks.
int cell_block_chunks(const Terrain& t, int maxChunks);

/// @brief Chunk (cx, cy) containing (x, y): cx = floor((x - x0)/cdx), cy = floor((y1 - y)/cdy),
///        each clamped to the grid.
void chunk_of_point(const Terrain& t, float x, float y, int& cx, int& cy);

CellPlan partition_cells(const WorldScene& s, const CellStats& st, const CellBudget& b,
                         const ChunkRegion& r = {});

} // namespace castlemist::world
