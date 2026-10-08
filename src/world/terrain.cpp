/// @file
/// @brief Terrain geometry (docs/research/gw2-world-frame.md §3).

#include "castlemist/world/terrain.h"

#include <cmath>
#include <limits>
#include <string>

namespace castlemist::world {

TerrainLayout terrain_layout(uint32_t dimX, uint32_t dimY, uint32_t vertsPerChunkSide, size_t heightCount) {
    TerrainLayout l;
    if (dimX == 0 || dimY == 0 || heightCount == 0) {
        l.why = "empty trn (dims " + std::to_string(dimX) + "x" + std::to_string(dimY) + ", " +
                std::to_string(heightCount) + " samples)";
        return l;
    }

    // §3.2. With the field, segments = verticesPerChunkSide and each chunk
    // stores (segments + 3)^2 samples (after spjinx/t3d TerrainRenderer.ts
    // getChunkResolution). Without it, solve for the one segment count the
    // dims and the sample count agree on (dims = chunks * segments, proven on
    // every map that has the field) -- T3D instead assumes 32 there.
    int segments = 0;
    if (vertsPerChunkSide > 0) {
        segments = (int)vertsPerChunkSide;
    } else {
        int found = 0;
        for (uint32_t s = 1; s <= dimX && s <= dimY; ++s) {
            if (dimX % s || dimY % s) continue;
            if ((size_t)(dimX / s) * (dimY / s) * (s + 3) * (s + 3) != heightCount) continue;
            if (!found) segments = (int)s;
            ++found;
        }
        if (found != 1) {
            l.why = "no verticesPerChunkSide and " + std::to_string(found) + " segment counts fit dims " +
                    std::to_string(dimX) + "x" + std::to_string(dimY) + " and " + std::to_string(heightCount) +
                    " samples";
            return l;
        }
    }
    const size_t stored = (size_t)segments + 3, perChunk = stored * stored;
    if (heightCount % perChunk) {
        l.why = std::to_string(heightCount) + " samples are not a whole number of " + std::to_string(stored) + "x" +
                std::to_string(stored) + " chunks";
        return l;
    }
    const size_t count = heightCount / perChunk;

    // Chunk grid: chunksX = sqrt(dims[0] * count / dims[1]), chunksY = count / chunksX
    // (after spjinx/t3d TerrainRenderer.ts parseNumChunks), required to be whole.
    const double cxF = std::sqrt((double)dimX * (double)count / (double)dimY);
    const long cx = std::lround(cxF);
    if (cx <= 0 || std::fabs(cxF - (double)cx) > 1e-9 || count % (size_t)cx) {
        l.why = std::to_string(count) + " chunks do not form a grid with aspect " + std::to_string(dimX) + ":" +
                std::to_string(dimY);
        return l;
    }
    l.chunksX = (int)cx;
    l.chunksY = (int)(count / (size_t)cx);
    l.segments = segments;
    l.stored = (int)stored;
    l.ok = true;
    return l;
}

Terrain build_terrain(const castlemist::model::Extractor::MapTerrain& t, std::vector<std::string>& warnings) {
    Terrain out;
    if (t.heights.empty() || t.dimX == 0 || t.dimY == 0) {
        warnings.push_back("terrain: no trn height samples; map has no terrain");
        return out;
    }
    // §2: the terrain is placed by parm.rect; without it there is nothing to place it by.
    if (!t.hasRect) {
        warnings.push_back("terrain: no parm rect; terrain left out (no rect is invented)");
        return out;
    }
    const float X0 = t.rect[0], Y0 = t.rect[1], X1 = t.rect[2], Y1 = t.rect[3];
    if (!(X1 > X0) || !(Y1 > Y0)) {
        warnings.push_back("terrain: parm rect is empty or inverted; terrain left out");
        return out;
    }
    const TerrainLayout l = terrain_layout(t.dimX, t.dimY, t.vertsPerChunkSide, t.heights.size());
    if (!l.ok) {
        warnings.push_back("terrain: " + l.why + "; terrain left out");
        return out;
    }

    out.present = true;
    out.chunksX = l.chunksX;
    out.chunksY = l.chunksY;
    out.chunks.reserve((size_t)l.chunksX * l.chunksY);
    const int S = l.stored, n = l.segments + 1;
    for (int cy = 0; cy < l.chunksY; ++cy) {
        for (int cx = 0; cx < l.chunksX; ++cx) {
            TerrainChunk ch;
            ch.cx = cx;
            ch.cy = cy;
            // §3.3: cx runs east from rect[0], cy runs south from rect[3]. On
            // the test maps this is where spjinx/t3d TerrainRenderer.ts
            // (chunk position, odd/even branch) puts every chunk.
            ch.rect[0] = X0 + (X1 - X0) * (float)cx / (float)l.chunksX;
            ch.rect[2] = X0 + (X1 - X0) * (float)(cx + 1) / (float)l.chunksX;
            ch.rect[3] = Y1 - (Y1 - Y0) * (float)cy / (float)l.chunksY;
            ch.rect[1] = Y1 - (Y1 - Y0) * (float)(cy + 1) / (float)l.chunksY;
            ch.samples = n;
            // §3.1: the chunk's own samples are the inner (segments+1)^2; the
            // outer ring is dropped (after spjinx/t3d TerrainRenderer.ts, chunk
            // vertex loop). Row 0 = north edge, column 0 = west edge, as stored.
            ch.heights.resize((size_t)n * n);
            const float* blk = &t.heights[((size_t)cy * l.chunksX + cx) * (size_t)S * S];
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) ch.heights[(size_t)j * n + i] = blk[(size_t)(j + 1) * S + (i + 1)];
            out.chunks.push_back(std::move(ch));
        }
    }
    return out;
}

float terrain_height_at(const Terrain& t, float x, float y, bool* inside) {
    if (inside) *inside = false;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!t.present || t.chunksX <= 0 || t.chunksY <= 0 || t.chunks.size() != (size_t)t.chunksX * t.chunksY)
        return nan;
    const TerrainChunk& nw = t.chunks.front();
    const TerrainChunk& se = t.chunks.back();
    const float X0 = nw.rect[0], Y1 = nw.rect[3], X1 = se.rect[2], Y0 = se.rect[1];
    if (!(x >= X0 && x <= X1 && y >= Y0 && y <= Y1)) return nan;

    // The chunk holding (x, y); a point on a shared edge may go either way,
    // the heights there are equal (§3.1).
    int cx = (int)std::floor((x - X0) / (X1 - X0) * (float)t.chunksX);
    int cy = (int)std::floor((Y1 - y) / (Y1 - Y0) * (float)t.chunksY);
    cx = cx < 0 ? 0 : (cx >= t.chunksX ? t.chunksX - 1 : cx);
    cy = cy < 0 ? 0 : (cy >= t.chunksY ? t.chunksY - 1 : cy);
    const TerrainChunk& c = t.chunks[(size_t)cy * t.chunksX + cx];
    const int n = c.samples;
    if (n < 2 || c.heights.size() != (size_t)n * n) return nan;

    // §3.4: bilinear over the chunk's samples; column i at x0 + i*w/(n-1),
    // row j at y1 - j*h/(n-1).
    const int seg = n - 1;
    float u = (x - c.rect[0]) / (c.rect[2] - c.rect[0]) * (float)seg;
    float v = (c.rect[3] - y) / (c.rect[3] - c.rect[1]) * (float)seg;
    u = u < 0 ? 0 : (u > (float)seg ? (float)seg : u);
    v = v < 0 ? 0 : (v > (float)seg ? (float)seg : v);
    int i0 = (int)u, j0 = (int)v;
    if (i0 >= seg) i0 = seg - 1;
    if (j0 >= seg) j0 = seg - 1;
    const float tx = u - (float)i0, ty = v - (float)j0;
    auto H = [&](int i, int j) { return c.heights[(size_t)j * n + i]; };
    const float a = H(i0, j0) + (H(i0 + 1, j0) - H(i0, j0)) * tx;
    const float b = H(i0, j0 + 1) + (H(i0 + 1, j0 + 1) - H(i0, j0 + 1)) * tx;
    if (inside) *inside = true;
    return a + (b - a) * ty;
}

} // namespace castlemist::world
