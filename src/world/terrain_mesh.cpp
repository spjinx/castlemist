#include "castlemist/world/terrain_mesh.h"

#include <cmath>

namespace castlemist::world {

const TerrainChunk* find_chunk(const Terrain& t, int cx, int cy) {
    for (const TerrainChunk& c : t.chunks)
        if (c.cx == cx && c.cy == cy) return &c;
    return nullptr;
}

namespace {

/// Height at sample (i, j) of @p c, where i or j may be -1 or n (one step past
/// the edge). Neighbours share the edge sample, so the next sample in is used.
/// Returns false when that neighbour is missing.
bool sample(const Terrain& t, const TerrainChunk& c, int i, int j, float& out) {
    const int n = c.samples;
    const TerrainChunk* k = &c;
    if (i < 0)       { k = find_chunk(t, c.cx - 1, c.cy); i = n - 2; }
    else if (i >= n) { k = find_chunk(t, c.cx + 1, c.cy); i = 1; }
    else if (j < 0)  { k = find_chunk(t, c.cx, c.cy - 1); j = n - 2; }
    else if (j >= n) { k = find_chunk(t, c.cx, c.cy + 1); j = 1; }
    if (!k || k->samples != c.samples ||
        k->heights.size() != size_t(n) * size_t(n)) return false;
    out = k->heights[size_t(j) * n + i];
    return true;
}

/// Slope from the samples either side of @p mid; one-sided when one is missing.
float slope(bool hasLo, float lo, float mid, bool hasHi, float hi, float step) {
    if (hasLo && hasHi) return (hi - lo) / (2 * step);
    if (hasHi) return (hi - mid) / step;
    if (hasLo) return (mid - lo) / step;
    return 0;
}

} // namespace

std::vector<float> chunk_normals(const Terrain& t, size_t chunkIndex) {
    std::vector<float> out;
    if (chunkIndex >= t.chunks.size()) return out;
    const TerrainChunk& c = t.chunks[chunkIndex];
    const int n = c.samples;
    if (n < 2 || c.heights.size() != size_t(n) * size_t(n)) return out;
    const float dx = (c.rect[2] - c.rect[0]) / float(n - 1);
    const float dy = (c.rect[3] - c.rect[1]) / float(n - 1);
    out.resize(size_t(n) * n * 3);
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float mid = c.heights[size_t(j) * n + i];
            float w = 0, e = 0, no = 0, so = 0;
            const bool hw = sample(t, c, i - 1, j, w), he = sample(t, c, i + 1, j, e);
            const bool hn = sample(t, c, i, j - 1, no), hs = sample(t, c, i, j + 1, so);
            const float fx = slope(hw, w, mid, he, e, dx);    // dz/dx, +x east
            const float fy = slope(hs, so, mid, hn, no, dy);  // dz/dy, +y north (row j-1)
            // Surface z = f(x, y): normal is proportional to (fx, fy, -1), up (-Z) dominant.
            const float len = std::sqrt(fx * fx + fy * fy + 1.0f);
            float* o = &out[(size_t(j) * n + i) * 3];
            o[0] = fx / len; o[1] = fy / len; o[2] = -1.0f / len;
        }
    }
    return out;
}

} // namespace castlemist::world
