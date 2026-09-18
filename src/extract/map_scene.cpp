/// @file
/// @brief mapc/area packfile to a coordinated MapScene (props, terrain, collision, zones).

#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "castlemist/format/struct_template.h"

#include "castlemist/core/packfile.h"

namespace castlemist::extract {

/// @brief The de-tiled terrain heightfield, for sampling ground height.
///
/// Same de-tiling as build_terrain_model (chunk-major with a 3-vertex overlap);
/// see the long comment there for why a raw sqrt() of heights.size() is wrong.
struct TerrainGrid {
    std::vector<float> hz;
    int Gx = 0, Gy = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, dx = 1, dy = 1;
    bool ok = false;
};

TerrainGrid build_terrain_grid(const castlemist::model::Extractor::MapTerrain& t) {
    TerrainGrid g;
    if (!t.present || t.heights.size() < 4) return g;
    const int TILES_PER_CHUNK = 32;
    bool chunked = false;
    if (t.dimX >= (uint32_t)TILES_PER_CHUNK && t.dimY >= (uint32_t)TILES_PER_CHUNK &&
        t.dimX % TILES_PER_CHUNK == 0 && t.dimY % TILES_PER_CHUNK == 0) {
        int cX = (int)t.dimX / TILES_PER_CHUNK, cY = (int)t.dimY / TILES_PER_CHUNK;
        size_t chunks = (size_t)cX * cY;
        int vps = (int)std::lround(std::sqrt((double)(t.heights.size() / chunks)));
        if (vps > TILES_PER_CHUNK && chunks * (size_t)vps * vps == t.heights.size()) {
            g.Gx = (cX - 1) * TILES_PER_CHUNK + vps;
            g.Gy = (cY - 1) * TILES_PER_CHUNK + vps;
            g.hz.assign((size_t)g.Gx * g.Gy, 0.0f);
            for (int cy = 0; cy < cY; ++cy)
                for (int cx = 0; cx < cX; ++cx) {
                    const float* blk = &t.heights[((size_t)cy * cX + cx) * vps * vps];
                    for (int ly = 0; ly < vps; ++ly)
                        for (int lx = 0; lx < vps; ++lx)
                            g.hz[(size_t)(cy * TILES_PER_CHUNK + ly) * g.Gx + (cx * TILES_PER_CHUNK + lx)] =
                                blk[ly * vps + lx];
                }
            chunked = true;
        }
    }
    if (g.hz.empty()) {
        int G = (int)std::lround(std::sqrt((double)t.heights.size()));
        if (G < 2 || (size_t)G * G > t.heights.size()) return g;
        g.Gx = g.Gy = G;
        g.hz.assign(t.heights.begin(), t.heights.begin() + (size_t)G * G);
    }
    g.x0 = t.rect[0]; g.y0 = t.rect[1]; g.x1 = t.rect[2]; g.y1 = t.rect[3];
    if (!t.hasRect || g.x1 <= g.x0 || g.y1 <= g.y0) { g.x0 = g.y0 = -3072; g.x1 = g.y1 = 3072; }
    g.dx = chunked ? (g.x1 - g.x0) / (float)t.dimX : (g.x1 - g.x0) / (g.Gx - 1);
    g.dy = chunked ? (g.y1 - g.y0) / (float)t.dimY : (g.y1 - g.y0) / (g.Gy - 1);
    g.ok = g.dx > 0 && g.dy > 0;
    return g;
}

/// @brief Bilinear ground height at a world (x, y); false when outside the grid.
///
/// Grid index 0 sits at the rect's near corner (x0,y0) -- confirmed against
/// spjinx/t3d's own terrain renderer, which places chunk index 0 at
/// `rect[0] + cdx/2` (the min corner) and walks toward rect[2]/rect[3] as the
/// chunk index increases, with no axis flip. An earlier version of this
/// function flipped this based on a visual read of one map; that flip
/// disagreed with T3D's own placement code and has been reverted.
bool sample_terrain_height(const TerrainGrid& g, float wx, float wy, float& out) {
    if (!g.ok) return false;
    float fx = (wx - g.x0) / g.dx, fy = (wy - g.y0) / g.dy;
    if (fx < 0 || fy < 0 || fx > g.Gx - 1 || fy > g.Gy - 1) return false;
    int i0 = (int)fx, j0 = (int)fy;
    int i1 = std::min(g.Gx - 1, i0 + 1), j1 = std::min(g.Gy - 1, j0 + 1);
    float tx = fx - i0, ty = fy - j0;
    auto H = [&](int i, int j) { return g.hz[(size_t)j * g.Gx + i]; };
    float a = H(i0, j0) + (H(i1, j0) - H(i0, j0)) * tx;
    float b = H(i0, j1) + (H(i1, j1) - H(i0, j1)) * tx;
    out = a + (b - a) * ty;
    return true;
}

// waterSurfaceZ in the `havk` chunk (see parseMapCollision) -- NOT guessed.
std::shared_ptr<ModelPreview> build_terrain_model(const castlemist::model::Extractor::MapTerrain& t, bool hasWaterZ,
                                                  float waterZ, bool skipFloodFillWater) {
    if (!t.present || t.heights.size() < 4) return nullptr;

    // GW2 terrain heightmaps are stored CHUNKED, not as one flat row-major grid.
    // The map is split into cX*cY square terrain chunks (each covering a fixed
    // TILES_PER_CHUNK = 32 tiles / 3072 world units per side, matching dims). Each
    // chunk stores VERTS_PER_CHUNK_SIDE = 35 height samples per side, laid out
    // chunk-major (row-major over the cY*cX chunk grid) then row-major within a
    // chunk. Adjacent chunks OVERLAP: a chunk's origin advances by only 32 verts
    // (TILES_PER_CHUNK) while it spans 35, so its last 3 columns/rows duplicate the
    // next chunk's first 3 (verified: overlapping samples are bit-identical). The
    // de-tiled grid is therefore Gx = (cX-1)*32 + 35 verts wide, Gy likewise tall.
    //
    // The old code assumed heights.size() was a perfect square and read it as one
    // flat sqrt(n) x sqrt(n) grid -- since a real map (e.g. 4x6 chunks = 29400
    // samples, sqrt ~ 171.5) is neither square nor flat, the rows wrapped at the
    // wrong stride and the whole surface came out as a diagonally-corrugated
    // "wavy mat", horizontally squeezed. De-tile properly here.
    const int TILES_PER_CHUNK = 32;
    int Gx = 0, Gy = 0;
    bool chunked = false;                // true once the de-tiled grid is built
    std::vector<float> hz;               // de-tiled row-major height grid (Gx*Gy)
    if (t.dimX >= (uint32_t)TILES_PER_CHUNK && t.dimY >= (uint32_t)TILES_PER_CHUNK &&
        t.dimX % TILES_PER_CHUNK == 0 && t.dimY % TILES_PER_CHUNK == 0) {
        int cX = (int)t.dimX / TILES_PER_CHUNK, cY = (int)t.dimY / TILES_PER_CHUNK;
        size_t chunks = (size_t)cX * cY;
        int vps = (int)std::lround(std::sqrt((double)(t.heights.size() / chunks)));
        if (vps > TILES_PER_CHUNK && chunks * (size_t)vps * vps == t.heights.size()) {
            Gx = (cX - 1) * TILES_PER_CHUNK + vps;
            Gy = (cY - 1) * TILES_PER_CHUNK + vps;
            hz.assign((size_t)Gx * Gy, 0.0f);
            for (int cy = 0; cy < cY; ++cy)
                for (int cx = 0; cx < cX; ++cx) {
                    const float* blk = &t.heights[((size_t)cy * cX + cx) * vps * vps];
                    for (int ly = 0; ly < vps; ++ly)
                        for (int lx = 0; lx < vps; ++lx) {
                            int gx = cx * TILES_PER_CHUNK + lx, gy = cy * TILES_PER_CHUNK + ly;
                            hz[(size_t)gy * Gx + gx] = blk[ly * vps + lx];
                        }
                }
            chunked = true;
        }
    }
    if (hz.empty()) {
        // Fallback for any layout we don't recognise: treat as a flat square grid.
        int G = static_cast<int>(std::lround(std::sqrt(static_cast<double>(t.heights.size()))));
        if (G < 2 || static_cast<size_t>(G) * G > t.heights.size()) return nullptr;
        Gx = Gy = G;
        hz.assign(t.heights.begin(), t.heights.begin() + (size_t)G * G);
    }

    float x0 = t.rect[0], y0 = t.rect[1], x1 = t.rect[2], y1 = t.rect[3];
    if (!t.hasRect || x1 <= x0 || y1 <= y0) { x0 = y0 = -3072; x1 = y1 = 3072; }
    // Vertex spacing = one terrain tile. When the chunked layout is used this is
    // (rect / dims) (== 96 units on real maps); the far-edge overhang from the
    // chunk overlap simply extends a couple of tiles past the rect, harmlessly.
    float dx = chunked ? (x1 - x0) / (float)t.dimX : (x1 - x0) / (Gx - 1);
    float dy = chunked ? (y1 - y0) / (float)t.dimY : (y1 - y0) / (Gy - 1);
    auto H = [&](int i, int j) {
        i = std::clamp(i, 0, Gx - 1); j = std::clamp(j, 0, Gy - 1);
        return hz[static_cast<size_t>(j) * Gx + i];
    };
    // Median height, to clamp sentinel "no-data" spikes to something sane.
    float med;
    {
        std::vector<float> s = hz;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        med = s[s.size() / 2];
    }
    const float kClamp = 4000.0f; // reject heights absurdly far from the median

    auto model = std::make_shared<ModelPreview>();
    model->meshes.emplace_back();
    ModelMeshCPU& mesh = model->meshes[0];
    mesh.materialIndex = 0;
    mesh.vertices.reserve(static_cast<size_t>(Gx) * Gy);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int j = 0; j < Gy; ++j) {
        for (int i = 0; i < Gx; ++i) {
            float h = H(i, j);
            if (std::abs(h - med) > kClamp) h = med;
            float wx = x0 + i * dx, wy = y0 + j * dy;
            float nzx = (H(i - 1, j) - H(i + 1, j)) / (2 * dx);
            float nzy = (H(i, j - 1) - H(i, j + 1)) / (2 * dy);
            float nl = std::sqrt(nzx * nzx + nzy * nzy + 1.0f);
            // Negated: the sky-facing normal points along -Z, because GW2's +Z is
            // DOWN (see kWorldUp in render/detail/math.h). The gradient normal
            // (-dh/dx, -dh/dy, +1) faces into the ground, so the ground used to be
            // lit from underneath and came out flat and unshaded.
            GVertex v{wx, wy, h, -nzx / nl, -nzy / nl, -1.0f / nl, 0, 0, 0, 0, 0, 0,
                      static_cast<float>(i) / (Gx - 1), static_cast<float>(j) / (Gy - 1)};
            mesh.vertices.push_back(v);
            lo[0] = std::min(lo[0], wx); lo[1] = std::min(lo[1], wy); lo[2] = std::min(lo[2], h);
            hi[0] = std::max(hi[0], wx); hi[1] = std::max(hi[1], wy); hi[2] = std::max(hi[2], h);
        }
    }
    for (int j = 0; j < Gy - 1; ++j) {
        for (int i = 0; i < Gx - 1; ++i) {
            uint32_t a = j * Gx + i, b = j * Gx + i + 1, c = (j + 1) * Gx + i, d = (j + 1) * Gx + i + 1;
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }
    ModelMaterialCPU mat; mat.index = 0; mat.kind = 1; // terrain (grass/rock, procedural)
    model->materials.push_back(mat);

    // Water: emit a translucent quad ONLY over terrain cells that sit below the
    // *real* water plane (parsed from the havk chunk), so land is always what
    // you see and water only fills in where the ground actually dips under it.
    // Previously this guessed a Z from the 20th-percentile of terrain heights,
    // which has no relationship to the map's actual water level -- on a lot of
    // maps that guess sat far too high and flooded huge stretches of normal,
    // dry land, making the whole map look "squeezed" into the water's footprint.
    // With no real water data, we skip water entirely rather than guess wrong.
    if (hasWaterZ && !skipFloodFillWater) {
        ModelMeshCPU wm;
        wm.materialIndex = 1;
        for (int j = 0; j < Gy - 1; ++j)
            for (int i = 0; i < Gx - 1; ++i) {
                float h00 = H(i, j), h10 = H(i + 1, j), h01 = H(i, j + 1), h11 = H(i + 1, j + 1);
                // +Z is DOWN, so a cell is under water when its height is GREATER
                // than waterZ; a corner with a smaller z is dry land and wins.
                if (std::min(std::min(h00, h10), std::min(h01, h11)) <= waterZ) continue;
                uint32_t base = static_cast<uint32_t>(wm.vertices.size());
                float xa = x0 + i * dx, xb = x0 + (i + 1) * dx, ya = y0 + j * dy, yb = y0 + (j + 1) * dy;
                wm.vertices.push_back(GVertex{xa, ya, waterZ, 0,0,-1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xb, ya, waterZ, 0,0,-1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xa, yb, waterZ, 0,0,-1, 0,0,0, 0,0,0, 0,0});
                wm.vertices.push_back(GVertex{xb, yb, waterZ, 0,0,-1, 0,0,0, 0,0,0, 0,0});
                wm.indices.insert(wm.indices.end(), {base+0, base+2, base+1, base+1, base+2, base+3});
            }
        if (!wm.vertices.empty()) {
            model->meshes.push_back(std::move(wm));
            ModelMaterialCPU wmat; wmat.index = 1; wmat.kind = 2; // water (translucent blue)
            model->materials.push_back(wmat);
        }
    }

    model->totalVerts = 0; model->totalTris = 0;
    for (const auto& m : model->meshes) { model->totalVerts += (uint32_t)m.vertices.size(); model->totalTris += (uint32_t)m.indices.size() / 3; }
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    return model;
}

// Builds a renderable model from the Havok collision mesh (walls/floors/ramps).
// Rendered as a translucent orange overlay (material kind 3) so the collidable
// geometry is clearly distinguishable from props/terrain/water.
std::shared_ptr<ModelPreview> build_collision_model(const castlemist::model::Extractor::MapCollision& c) {
    if (!c.present) return nullptr;
    size_t nv = c.verts.size() / 3;
    if (nv < 3 || c.indices.size() < 3) return nullptr;

    std::vector<float> nrm(nv * 3, 0.0f);
    for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
        uint32_t a = c.indices[t], b = c.indices[t + 1], d = c.indices[t + 2];
        if (a >= nv || b >= nv || d >= nv) continue;
        const float* pa = &c.verts[a * 3]; const float* pb = &c.verts[b * 3]; const float* pd = &c.verts[d * 3];
        float u[3] = {pb[0]-pa[0], pb[1]-pa[1], pb[2]-pa[2]}, v[3] = {pd[0]-pa[0], pd[1]-pa[1], pd[2]-pa[2]};
        float fn[3] = {u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]};
        for (uint32_t idx : {a, b, d}) for (int k = 0; k < 3; ++k) nrm[idx * 3 + k] += fn[k];
    }

    auto model = std::make_shared<ModelPreview>();
    model->meshes.emplace_back();
    ModelMeshCPU& mesh = model->meshes[0];
    mesh.materialIndex = 0;
    mesh.vertices.reserve(nv);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < nv; ++i) {
        const float* p = &c.verts[i * 3];
        float n[3] = {nrm[i*3], nrm[i*3+1], nrm[i*3+2]};
        float nl = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]); if (nl < 1e-6f) { n[2] = 1; nl = 1; }
        mesh.vertices.push_back(GVertex{p[0], p[1], p[2], n[0]/nl, n[1]/nl, n[2]/nl, 0,0,0, 0,0,0, 0,0});
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    mesh.indices = c.indices;
    ModelMaterialCPU mat; mat.index = 0; mat.kind = 3; // collision (translucent orange)
    model->materials.push_back(mat);
    model->totalVerts = static_cast<uint32_t>(nv);
    model->totalTris = static_cast<uint32_t>(c.indices.size() / 3);
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0]-lo[0], hi[1]-lo[1], hi[2]-lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0]*ext[0] + ext[1]*ext[1] + ext[2]*ext[2]);
    return model;
}

// Builds a renderable model from the `watr` chunk's real water-surface
// geometry: each surface is a 2D outline at its own Z, triangulated as a
// simple centroid fan. That's exact for a convex outline and a reasonable
// approximation for GW2's gently-curved water surfaces; a genuinely concave
// outline would need real polygon triangulation, which this doesn't attempt.
// Rendered with the same translucent-blue material kind as the terrain's
// flood-fill water quad, since build_map_scene only ever uses one or the
// other per map (see build_terrain_model's skipFloodFillWater).
std::shared_ptr<ModelPreview> build_water_model(const castlemist::model::Extractor::MapWater& w) {
    if (!w.present) return nullptr;

    auto model = std::make_shared<ModelPreview>();
    model->meshes.emplace_back();
    ModelMeshCPU& mesh = model->meshes[0];
    mesh.materialIndex = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};

    for (const auto& s : w.surfaces) {
        size_t n = s.points.size() / 2;
        if (n < 3) continue;
        float cx = 0, cy = 0;
        for (size_t i = 0; i < n; ++i) { cx += s.points[i * 2]; cy += s.points[i * 2 + 1]; }
        cx /= static_cast<float>(n); cy /= static_cast<float>(n);

        uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        mesh.vertices.push_back(GVertex{cx, cy, s.z, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
        lo[0] = std::min(lo[0], cx); lo[1] = std::min(lo[1], cy); lo[2] = std::min(lo[2], s.z);
        hi[0] = std::max(hi[0], cx); hi[1] = std::max(hi[1], cy); hi[2] = std::max(hi[2], s.z);
        for (size_t i = 0; i < n; ++i) {
            float x = s.points[i * 2], y = s.points[i * 2 + 1];
            mesh.vertices.push_back(GVertex{x, y, s.z, 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
            lo[0] = std::min(lo[0], x); lo[1] = std::min(lo[1], y);
            hi[0] = std::max(hi[0], x); hi[1] = std::max(hi[1], y);
        }
        for (size_t i = 0; i < n; ++i) {
            uint32_t a = base, b = base + 1 + static_cast<uint32_t>(i),
                     c = base + 1 + static_cast<uint32_t>((i + 1) % n);
            mesh.indices.insert(mesh.indices.end(), {a, b, c});
        }
    }
    if (mesh.vertices.empty()) return nullptr;

    ModelMaterialCPU mat; mat.index = 0; mat.kind = 2; // water (translucent blue), same as the flood-fill quad
    model->materials.push_back(mat);
    model->totalVerts = static_cast<uint32_t>(mesh.vertices.size());
    model->totalTris = static_cast<uint32_t>(mesh.indices.size() / 3);
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    return model;
}

// Appends an axis-aligned box (12 tris) spanning [mn,mx] to `mesh`.
void add_nav_box(ModelMeshCPU& mesh, const float mn[3], const float mx[3]) {
    uint32_t b = static_cast<uint32_t>(mesh.vertices.size());
    const float c[8][3] = {
        {mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mx[0], mx[1], mn[2]}, {mn[0], mx[1], mn[2]},
        {mn[0], mn[1], mx[2]}, {mx[0], mn[1], mx[2]}, {mx[0], mx[1], mx[2]}, {mn[0], mx[1], mx[2]},
    };
    for (const auto& v : c) mesh.vertices.push_back(GVertex{v[0], v[1], v[2], 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    static const uint32_t idx[36] = {
        0, 1, 2, 0, 2, 3,  // bottom
        4, 6, 5, 4, 7, 6,  // top
        0, 4, 5, 0, 5, 1,  // sides
        1, 5, 6, 1, 6, 2,
        2, 6, 7, 2, 7, 3,
        3, 7, 4, 3, 4, 0,
    };
    for (uint32_t i : idx) mesh.indices.push_back(b + i);
}

// Appends a thin quad ("beam") from `s` to `e`, `halfWidth` wide, as a stand-in
// for a wireframe line segment (this pipeline is triangle-only). The beam lies
// in the plane containing the segment and world-up, so it's visible from
// directly above -- matching how the map is normally viewed.
void add_nav_beam(ModelMeshCPU& mesh, const float s[3], const float e[3], float halfWidth) {
    float d[3] = {e[0] - s[0], e[1] - s[1], e[2] - s[2]};
    float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (len < 1e-4f) return;
    // Perp = d x worldUp (GW2's +Z is down, so worldUp is (0,0,-1) -- see kWorldUp).
    float p[3] = {d[1] * -1 - d[2] * 0, d[2] * 0 - d[0] * -1, d[0] * 0 - d[1] * 0};
    float pl = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
    if (pl < 1e-4f) { p[0] = 1; p[1] = 0; p[2] = 0; pl = 1; }
    for (int k = 0; k < 3; ++k) p[k] = p[k] / pl * halfWidth;
    uint32_t b = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back(GVertex{s[0] - p[0], s[1] - p[1], s[2] - p[2], 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    mesh.vertices.push_back(GVertex{s[0] + p[0], s[1] + p[1], s[2] + p[2], 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    mesh.vertices.push_back(GVertex{e[0] + p[0], e[1] + p[1], e[2] + p[2], 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    mesh.vertices.push_back(GVertex{e[0] - p[0], e[1] - p[1], e[2] - p[2], 0, 0, -1, 0, 0, 0, 0, 0, 0, 0, 0});
    mesh.indices.insert(mesh.indices.end(), {b + 0, b + 2, b + 1, b + 0, b + 3, b + 2});
}

// Builds a renderable model visualizing a map's navigation data -- see this
// function's declaration in internal.h for what is and isn't real geometry
// here (coarse-graph nodes/edges are exact; nm15/pnvm chunk boxes are
// bounding volumes over an undecoded payload).
std::shared_ptr<ModelPreview> build_navmesh_model(const castlemist::model::Extractor::MapNavMesh& nm,
                                                  const castlemist::model::Extractor::MapNavGraph& ng) {
    if (!nm.present && !ng.present) return nullptr;

    auto model = std::make_shared<ModelPreview>();
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    auto track = [&](const float p[3]) {
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    };

    if (nm.present) {
        ModelMeshCPU mesh;
        for (const auto& c : nm.chunks) { add_nav_box(mesh, c.boundsMin, c.boundsMax); track(c.boundsMin); track(c.boundsMax); }
        if (!mesh.vertices.empty()) model->meshes.push_back(std::move(mesh));
    }
    if (ng.present) {
        ModelMeshCPU nodeMesh;
        for (const auto& sec : ng.sections)
            for (const auto& node : sec.nodes) {
                add_nav_box(nodeMesh, node.boundsMin, node.boundsMax);
                track(node.boundsMin); track(node.boundsMax);
            }
        if (!nodeMesh.vertices.empty()) model->meshes.push_back(std::move(nodeMesh));

        ModelMeshCPU beamMesh;
        for (const auto& sec : ng.sections)
            for (const auto& conn : sec.connections)
                for (const auto& e : conn.edges) {
                    add_nav_beam(beamMesh, e.start, e.end, 8.0f);
                    track(e.start); track(e.end);
                }
        if (!beamMesh.vertices.empty()) model->meshes.push_back(std::move(beamMesh));
    }
    if (model->meshes.empty()) return nullptr;

    for (size_t i = 0; i < model->meshes.size(); ++i) {
        model->meshes[i].materialIndex = static_cast<int>(i);
        ModelMaterialCPU mat; mat.index = static_cast<int>(i); mat.kind = 4; // navmesh (translucent magenta)
        model->materials.push_back(mat);
    }
    model->totalVerts = model->totalTris = 0;
    for (const auto& m : model->meshes) {
        model->totalVerts += static_cast<uint32_t>(m.vertices.size());
        model->totalTris += static_cast<uint32_t>(m.indices.size() / 3);
    }
    for (int k = 0; k < 3; ++k) model->center[k] = (lo[k] + hi[k]) * 0.5f;
    float ext[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    model->radius = 0.5f * std::sqrt(ext[0] * ext[0] + ext[1] * ext[1] + ext[2] * ext[2]);
    return model;
}

// Builds a coordinated map scene from a mapc/area packfile: parse the prop
// placement (prp2), load each unique prop model (deduped by fileId, capped), and
// record one instance per placement with its world transform.
std::shared_ptr<MapScene> build_map_scene(const std::vector<uint8_t>& map_bytes, const std::string& dat_path,
                                          const nlohmann::json& tpl) {
    std::vector<castlemist::model::Extractor::MapProp> props;
    castlemist::model::Extractor::MapTerrain terr;
    castlemist::model::Extractor::MapCollision coll;
    castlemist::model::Extractor::MapWater water;
    castlemist::model::Extractor::MapNavMesh navMesh;
    castlemist::model::Extractor::MapNavGraph navGraph;
    castlemist::model::Extractor::MapEnvLight envLight;
    try {
        castlemist::model::Extractor ex(map_bytes, tpl);
        props = ex.parseMapProps();
        terr = ex.parseTerrain();
        coll = ex.parseMapCollision();
        try { water = ex.parseWater(); } catch (const std::exception&) { /* water is optional */ }
        try { navMesh = ex.parseNavMesh(); } catch (const std::exception&) { /* navmesh is optional */ }
        try { navGraph = ex.parseNavGraph(); } catch (const std::exception&) { /* nav graph is optional */ }
        try { envLight = ex.parseMapEnv(); } catch (const std::exception&) { /* env is optional */ }
    } catch (const std::exception&) {
        return nullptr;
    }
    if (props.empty() && !terr.present) return nullptr;

    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception&) {
        return nullptr;
    }

    auto scene = std::make_shared<MapScene>();
    scene->totalProps = static_cast<uint32_t>(props.size());
    if (envLight.present) {
        MapEnvRig& r = scene->env;
        r.present = true; r.sunIntensity = envLight.sunIntensity; r.fillIntensity = envLight.fillIntensity;
        r.lightCount = envLight.lightCount;
        for (int k = 0; k < 3; ++k) { r.sunDir[k] = envLight.sunDir[k]; r.sunColor[k] = envLight.sunColor[k]; r.fillColor[k] = envLight.fillColor[k]; }
    }

    // 1) De-duplicate the prop models -> a list of unique fileIds to load (capped).
    constexpr size_t kMaxModels = 600;
    std::vector<uint32_t> uniqueIds;
    std::unordered_map<uint32_t, int> idSlot; // fileId -> index into uniqueIds (-1 = over cap)
    for (const auto& p : props) {
        if (idSlot.count(p.fileId)) continue;
        if (uniqueIds.size() < kMaxModels) { idSlot[p.fileId] = static_cast<int>(uniqueIds.size()); uniqueIds.push_back(p.fileId); }
        else idSlot[p.fileId] = -1;
    }

    // 2) Load the unique models IN PARALLEL. Each task reads its MODL bytes off a
    //    shared read-only archive (read_entry_bytes uses its own file handle) and
    //    decodes geometry+textures via build_model_preview, reusing the single
    //    already-parsed `dat` (its file handle is already closed by load_dat_file,
    //    so concurrent read-only lookups against it are safe) instead of each
    //    model re-parsing the whole MFT table from scratch -- that redundant
    //    reparse (times up to kMaxModels) used to be the actual "loading lambat"
    //    cost, not a lack of threads.
    std::vector<std::shared_ptr<ModelPreview>> loaded(uniqueIds.size());
    parallel_for(uniqueIds.size(), [&](size_t i) {
        std::vector<uint8_t> modl = load_modl_bytes_by_fileid(dat, uniqueIds[i]);
        if (!modl.empty() && castlemist::core::has_chunk(modl, "GEOM")) {
            auto mp = build_model_preview(modl, dat, tpl);
            if (mp) loaded[i] = std::move(mp);
        }
    });

    // 3) Compact the successful loads into scene->models and map slot -> index.
    std::vector<int> slotToModel(uniqueIds.size(), -1);
    for (size_t i = 0; i < uniqueIds.size(); ++i) {
        if (!loaded[i]) continue;
        slotToModel[i] = static_cast<int>(scene->models.size());
        scene->models.push_back(std::move(*loaded[i]));
        scene->modelFileIds.push_back(uniqueIds[i]); // for lazy game-shader re-extraction
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());

    // 4) One instance per placed prop that resolved to a loaded model.
    for (const auto& p : props) {
        auto it = idSlot.find(p.fileId);
        if (it == idSlot.end() || it->second < 0) continue;
        int mi = slotToModel[it->second];
        if (mi < 0) continue;
        MapInstance in;
        in.model = mi;
        for (int k = 0; k < 3; ++k) { in.pos[k] = p.pos[k]; in.rot[k] = p.rot[k]; }
        in.scale = p.scale;
        scene->instances.push_back(in);
    }

    // 5) Terrain ground surface (layer 1), real water surface geometry when the
    //    map has a `watr` chunk (layer 4, falling back to the terrain's guessed
    //    flood-fill quad otherwise) + Havok collision mesh (layer 2), all
    //    world-space with identity instances. Collision is hidden by default.
    if (auto tm = build_terrain_model(terr, coll.hasWater, coll.waterZ, /*skipFloodFillWater=*/water.present)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 1;
        scene->models.push_back(std::move(*tm));
        scene->instances.push_back(in);
    }
    if (auto wm = build_water_model(water)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 4;
        scene->models.push_back(std::move(*wm));
        scene->instances.push_back(in);
    }
    if (auto cm = build_collision_model(coll)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 2;
        scene->models.push_back(std::move(*cm));
        scene->instances.push_back(in);
    }
    if (auto nm = build_navmesh_model(navMesh, navGraph)) {
        MapInstance in;
        in.model = static_cast<int>(scene->models.size());
        in.scale = 1.0f; in.layer = 5;
        scene->models.push_back(std::move(*nm));
        scene->instances.push_back(in);
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());

    if (scene->instances.empty()) return nullptr;
    return scene;
}


} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

std::shared_ptr<MapScene> build_map_zone_layer(const std::vector<uint8_t>& map_bytes, const std::string& dat_path) {
    auto tplp = castlemist::tpl::get();
    if (!tplp || dat_path.empty()) return nullptr;
    const nlohmann::json& tpl = *tplp;
    std::vector<castlemist::model::Extractor::MapZoneInst> zones;
    TerrainGrid grid;
    try {
        castlemist::model::Extractor ex(map_bytes, tpl);
        zones = ex.parseMapZones();
        // Zone scatter sits ON the ground, so each placement's z comes from the
        // heightfield; parseMapZones only fills in the zone's flat zPos fallback.
        try { grid = build_terrain_grid(ex.parseTerrain()); } catch (const std::exception&) {}
    } catch (const std::exception&) {
        return nullptr;
    }
    if (zones.empty()) return nullptr;
    if (grid.ok)
        for (auto& z : zones) {
            float h = 0;
            if (sample_terrain_height(grid, z.pos[0], z.pos[1], h)) z.pos[2] = h;
        }

    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); } catch (const std::exception&) { return nullptr; }

    auto scene = std::make_shared<MapScene>();
    constexpr size_t kMaxModels = 600;
    std::vector<uint32_t> uniqueIds;
    std::unordered_map<uint32_t, int> idSlot;
    for (const auto& z : zones) {
        if (idSlot.count(z.fileId)) continue;
        if (uniqueIds.size() < kMaxModels) { idSlot[z.fileId] = static_cast<int>(uniqueIds.size()); uniqueIds.push_back(z.fileId); }
        else idSlot[z.fileId] = -1;
    }
    std::vector<std::shared_ptr<ModelPreview>> loaded(uniqueIds.size());
    parallel_for(uniqueIds.size(), [&](size_t i) {
        std::vector<uint8_t> modl = load_modl_bytes_by_fileid(dat, uniqueIds[i]);
        if (!modl.empty() && castlemist::core::has_chunk(modl, "GEOM")) {
            auto mp = build_model_preview(modl, dat, tpl);
            if (mp) loaded[i] = std::move(mp);
        }
    });
    std::vector<int> slotToModel(uniqueIds.size(), -1);
    for (size_t i = 0; i < uniqueIds.size(); ++i)
        if (loaded[i]) { slotToModel[i] = static_cast<int>(scene->models.size()); scene->models.push_back(std::move(*loaded[i])); }
    for (const auto& z : zones) {
        auto it = idSlot.find(z.fileId);
        if (it == idSlot.end() || it->second < 0) continue;
        int mi = slotToModel[it->second];
        if (mi < 0) continue;
        MapInstance in;
        in.model = mi;
        for (int k = 0; k < 3; ++k) in.pos[k] = z.pos[k];
        in.scale = z.scale;
        in.layer = 3;
        scene->instances.push_back(in);
    }
    scene->loadedModels = static_cast<uint32_t>(scene->models.size());
    if (scene->instances.empty()) return nullptr;
    return scene;
}

bool augment_map_scene_game(MapScene& scene, const std::string& dat_path) {
    if (scene.gameMaterialsLoaded) return true;
    auto tplp = castlemist::tpl::get();
    if (!tplp || dat_path.empty()) return false;
    const nlohmann::json& tpl = *tplp;
    if (scene.modelFileIds.size() != scene.models.size()) return false;

    Gw2Dat dat;
    try { load_dat_file(dat, dat_path); } catch (const std::exception&) { return false; }

    std::atomic<bool> any{false};
    parallel_for(scene.models.size(), [&](size_t i) {
        if (!scene.models[i].gameMaterials.empty()) { any.store(true); return; } // already have
        uint32_t fid = scene.modelFileIds[i];
        if (!fid) return;
        std::vector<uint8_t> modl = load_modl_bytes_by_fileid(dat, fid);
        if (modl.empty() || !castlemist::core::has_chunk(modl, "GEOM")) return;
        // Rebuild the model WITH game shaders. Geometry/material indices are
        // identical (same MODL), so the already-uploaded scene geometry stays valid;
        // we only need the new gameMaterials + textures for GPU material building.
        auto mp = build_model_preview(modl, dat, tpl, /*want_game=*/true);
        if (mp && !mp->gameMaterials.empty()) { scene.models[i] = std::move(*mp); any.store(true); }
    });
    scene.gameMaterialsLoaded = any.load();
    return scene.gameMaterialsLoaded;
}
