#include "castlemist/world/cells.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace castlemist::world {
namespace {

struct Grid {
    int nx = 0, ny = 0;
    float x0 = 0, y1 = 0, cdx = 1, cdy = 1;
    std::vector<const TerrainChunk*> at;   // ny*nx
    const TerrainChunk* get(int cx, int cy) const { return at[size_t(cy) * nx + cx]; }
};

Grid make_grid(const Terrain& t) {
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

void point_chunk(const Grid& g, float x, float y, int& cx, int& cy) {
    cx = int(std::floor((x - g.x0) / g.cdx));
    cy = int(std::floor((g.y1 - y) / g.cdy));
    cx = std::clamp(cx, 0, std::max(g.nx - 1, 0));
    cy = std::clamp(cy, 0, std::max(g.ny - 1, 0));
}

std::string cell_id(int cx0, int cy0, int w, int h) {
    return "c" + std::to_string(cx0) + "_" + std::to_string(cy0) + "_" + std::to_string(w) + "x" + std::to_string(h);
}

/// 1/pickerScale as an integer block size, or 0 when unresolved, disagreeing, or out of range.
int page_block(const Terrain& t, int maxChunks) {
    float scale = 0;
    for (const auto& c : t.chunks) {
        const float p = c.material.pickerScale;
        if (p <= 0) continue;
        if (scale == 0) scale = p; else if (p != scale) return 0;
    }
    if (scale <= 0) return 0;
    const float inv = 1.0f / scale;
    const int n = int(std::lround(inv));
    if (std::fabs(inv - float(n)) > 1e-3f || n < 1 || n > maxChunks) return 0;
    return n;
}

struct Ctx {
    const WorldScene& s; const CellStats& st; const CellBudget& b; const Grid& g;
    uint64_t terrTrisPerChunk;
    std::vector<std::vector<uint32_t>> chunkProps, chunkColl;   // by cy*nx+cx
    std::vector<Cell> out;
    std::vector<std::string> warnings;
};

void measure(const Ctx& c, int cx0, int cy0, int w, int h, uint64_t& tris, double& mb) {
    tris = c.terrTrisPerChunk * uint64_t(w) * uint64_t(h);
    std::set<uint32_t> files;
    for (int y = cy0; y < cy0 + h; ++y)
        for (int x = cx0; x < cx0 + w; ++x)
            for (uint32_t p : c.chunkProps[size_t(y) * c.g.nx + x]) {
                const uint32_t m = c.s.props[p].model;
                if (m < c.st.modelTriangles.size()) tris += c.st.modelTriangles[m];
                if (m < c.st.modelTextures.size()) files.insert(c.st.modelTextures[m].begin(), c.st.modelTextures[m].end());
            }
    uint64_t bytes = c.st.bakedBytesPerChunk * uint64_t(w) * uint64_t(h);
    for (uint32_t f : files) {
        auto it = c.st.textureBytes.find(f);
        if (it != c.st.textureBytes.end()) bytes += it->second;
    }
    mb = double(bytes) / 1048576.0;
}

// Terminates: every recursion halves (rounding up) a side that is > 1, and a 1x1 never recurses.
void emit(Ctx& c, int cx0, int cy0, int w, int h, bool split) {
    uint64_t tris; double mb;
    measure(c, cx0, cy0, w, h, tris, mb);
    const bool over = tris > c.b.maxTriangles || mb > c.b.maxTextureMB;
    if (over && (w > 1 || h > 1)) {
        const int wh = (w + 1) / 2, hh = (h + 1) / 2;
        for (int qy = 0; qy < 2; ++qy)
            for (int qx = 0; qx < 2; ++qx) {
                const int qw = qx ? w - wh : wh, qh = qy ? h - hh : hh;
                if (qw <= 0 || qh <= 0) continue;
                emit(c, cx0 + qx * wh, cy0 + qy * hh, qw, qh, true);
            }
        return;
    }
    Cell cell;
    cell.cx0 = cx0; cell.cy0 = cy0; cell.w = w; cell.h = h;
    cell.id = cell_id(cx0, cy0, w, h);
    cell.split = split;
    cell.triangles = tris; cell.textureMB = mb;
    if (over) {
        cell.overBudget = true;
        char buf[256];
        std::snprintf(buf, sizeof buf, "cells: %s over budget (%llu triangles, %.1f MB) at 1x1",
                      cell.id.c_str(), (unsigned long long)tris, mb);
        c.warnings.push_back(buf);
    }
    const TerrainChunk* a = c.g.get(cx0, cy0);
    const TerrainChunk* z = c.g.get(cx0 + w - 1, cy0 + h - 1);
    cell.rect[0] = a ? a->rect[0] : c.g.x0 + float(cx0) * c.g.cdx;
    cell.rect[3] = a ? a->rect[3] : c.g.y1 - float(cy0) * c.g.cdy;
    cell.rect[2] = z ? z->rect[2] : c.g.x0 + float(cx0 + w) * c.g.cdx;
    cell.rect[1] = z ? z->rect[1] : c.g.y1 - float(cy0 + h) * c.g.cdy;
    cell.origin[0] = (cell.rect[0] + cell.rect[2]) * 0.5f;
    cell.origin[1] = (cell.rect[1] + cell.rect[3]) * 0.5f;
    for (int y = cy0; y < cy0 + h; ++y)
        for (int x = cx0; x < cx0 + w; ++x) {
            const auto& p = c.chunkProps[size_t(y) * c.g.nx + x];
            const auto& q = c.chunkColl[size_t(y) * c.g.nx + x];
            cell.props.insert(cell.props.end(), p.begin(), p.end());
            cell.collision.insert(cell.collision.end(), q.begin(), q.end());
        }
    std::sort(cell.props.begin(), cell.props.end());
    std::sort(cell.collision.begin(), cell.collision.end());
    c.out.push_back(std::move(cell));
}

} // namespace

int cell_block_chunks(const Terrain& t, int maxChunks) {
    const int n = page_block(t, maxChunks);
    return n > 0 ? n : maxChunks;
}

void chunk_of_point(const Terrain& t, float x, float y, int& cx, int& cy) {
    point_chunk(make_grid(t), x, y, cx, cy);
}

CellPlan partition_cells(const WorldScene& s, const CellStats& st, const CellBudget& b, const ChunkRegion& r) {
    CellPlan plan;
    plan.blockChunks = b.maxChunks;

    if (!s.terrain.present || s.terrain.chunks.empty()) {
        Cell c;
        c.id = "c_all"; c.all = true;
        if (s.hasBounds) {
            for (int i = 0; i < 4; ++i) c.rect[i] = s.bounds[i];
        } else if (!s.props.empty()) {
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (const auto& p : s.props) {
                x0 = std::min(x0, p.world[12]); x1 = std::max(x1, p.world[12]);
                y0 = std::min(y0, p.world[13]); y1 = std::max(y1, p.world[13]);
            }
            c.rect[0] = x0; c.rect[1] = y0; c.rect[2] = x1; c.rect[3] = y1;
        }
        c.origin[0] = (c.rect[0] + c.rect[2]) * 0.5f;
        c.origin[1] = (c.rect[1] + c.rect[3]) * 0.5f;
        for (uint32_t i = 0; i < s.props.size(); ++i) c.props.push_back(i);
        for (uint32_t i = 0; i < s.collision.instances.size(); ++i) c.collision.push_back(i);
        uint64_t tris = 0, bytes = 0; std::set<uint32_t> files;
        for (const auto& p : s.props) {
            if (p.model < st.modelTriangles.size()) tris += st.modelTriangles[p.model];
            if (p.model < st.modelTextures.size())
                files.insert(st.modelTextures[p.model].begin(), st.modelTextures[p.model].end());
        }
        for (uint32_t f : files) { auto it = st.textureBytes.find(f); if (it != st.textureBytes.end()) bytes += it->second; }
        c.triangles = tris; c.textureMB = double(bytes) / 1048576.0;
        plan.cells.push_back(std::move(c));
        plan.warnings.push_back("cells: no terrain; one cell c_all holds every placement");
        return plan;
    }

    const int pageBlock = page_block(s.terrain, b.maxChunks);
    plan.blockChunks = pageBlock > 0 ? pageBlock : b.maxChunks;
    if (pageBlock == 0)
        plan.warnings.push_back("cells: picker pages did not resolve to a block size; using " +
                                std::to_string(b.maxChunks) + " chunks per cell side");

    const Grid g = make_grid(s.terrain);
    const int segs = std::max(s.terrain.chunks.front().samples - 1, 0);
    Ctx c{s, st, b, g, uint64_t(segs) * uint64_t(segs) * 2, {}, {}, {}, {}};
    c.chunkProps.assign(size_t(g.nx) * g.ny, {});
    c.chunkColl.assign(size_t(g.nx) * g.ny, {});
    for (uint32_t i = 0; i < s.props.size(); ++i) {
        int cx, cy; point_chunk(g, s.props[i].world[12], s.props[i].world[13], cx, cy);
        c.chunkProps[size_t(cy) * g.nx + cx].push_back(i);
    }
    for (uint32_t i = 0; i < s.collision.instances.size(); ++i) {
        int cx, cy; point_chunk(g, s.collision.instances[i].world[12], s.collision.instances[i].world[13], cx, cy);
        c.chunkColl[size_t(cy) * g.nx + cx].push_back(i);
    }

    const int B = plan.blockChunks;
    for (int by = 0; by < g.ny; by += B)
        for (int bx = 0; bx < g.nx; bx += B)
            emit(c, bx, by, std::min(B, g.nx - bx), std::min(B, g.ny - by), false);

    // The region snaps outward to whole blocks and is clamped to the grid, so
    // the kept cells (each inside one block) tile plan.region exactly.
    ChunkRegion k = r;
    bool none = false;
    if (!r.whole()) {
        auto lo = [B](int v) { return v < 0 ? 0 : v / B * B; };
        auto hi = [B](int v, int n) { return std::min(v < 0 ? -1 : (v / B + 1) * B - 1, n - 1); };
        k.cx0 = lo(r.cx0); k.cy0 = lo(r.cy0); k.cx1 = hi(r.cx1, g.nx); k.cy1 = hi(r.cy1, g.ny);
        none = r.cy1 < r.cy0 || k.cx0 > k.cx1 || k.cy0 > k.cy1;
        if (none) k = r;   // outside the grid (or inverted): nothing is kept
    }
    for (auto& cell : c.out) {
        const bool keep = r.whole() || (!none && k.cx0 <= cell.cx0 && cell.cx0 + cell.w - 1 <= k.cx1 &&
                                        k.cy0 <= cell.cy0 && cell.cy0 + cell.h - 1 <= k.cy1);
        if (keep) plan.cells.push_back(std::move(cell));
        else plan.propsOutsideRegion += cell.props.size() + cell.collision.size();
    }
    std::sort(plan.cells.begin(), plan.cells.end(), [](const Cell& a, const Cell& d) {
        return a.cy0 != d.cy0 ? a.cy0 < d.cy0 : a.cx0 < d.cx0;
    });
    plan.region = k;
    for (auto& w : c.warnings) plan.warnings.push_back(std::move(w));
    return plan;
}

} // namespace castlemist::world
