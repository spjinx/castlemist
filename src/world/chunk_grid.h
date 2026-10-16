/// @file
/// @brief The terrain chunk grid by (cx, cy) and the chunk under a map point,
///        shared by cells.cpp (bucketing) and terrain_bake.cpp (water depth).
///        Built once per pass, so a per-texel lookup allocates nothing (review M5).
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace castlemist::world::detail {

struct Grid {
    int nx = 0, ny = 0;
    float x0 = 0, y1 = 0, cdx = 1, cdy = 1;
    std::vector<const TerrainChunk*> at;   // ny*nx; null where the map has no chunk
    const TerrainChunk* get(int cx, int cy) const { return at[size_t(cy) * nx + cx]; }
};

inline Grid make_grid(const Terrain& t) {
    Grid g;
    for (const auto& c : t.chunks) { g.nx = std::max(g.nx, c.cx + 1); g.ny = std::max(g.ny, c.cy + 1); }
    g.at.assign(size_t(g.nx) * g.ny, nullptr);
    for (const auto& c : t.chunks)
        if (c.cx >= 0 && c.cy >= 0) g.at[size_t(c.cy) * g.nx + c.cx] = &c;
    for (const auto* c : g.at) if (c) {
        g.x0 = c->rect[0]; g.y1 = c->rect[3];
        g.cdx = c->rect[2] - c->rect[0]; g.cdy = c->rect[3] - c->rect[1];
        break;
    }
    // x0 / y1 are the map's west / north edges: take them from chunk (0, 0) when present.
    if (g.nx && g.ny)
        if (const auto* c = g.get(0, 0)) { g.x0 = c->rect[0]; g.y1 = c->rect[3]; }
    if (g.cdx <= 0) g.cdx = 1;
    if (g.cdy <= 0) g.cdy = 1;
    return g;
}

/// Chunk (cx, cy) containing (x, y), each clamped to the grid.
inline void point_chunk(const Grid& g, float x, float y, int& cx, int& cy) {
    cx = int(std::floor((x - g.x0) / g.cdx));
    cy = int(std::floor((g.y1 - y) / g.cdy));
    cx = std::clamp(cx, 0, std::max(g.nx - 1, 0));
    cy = std::clamp(cy, 0, std::max(g.ny - 1, 0));
}

} // namespace castlemist::world::detail
