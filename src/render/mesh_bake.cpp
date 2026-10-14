/// @file
/// @brief AO bake, submesh flattener and frustum planes shared by the fly view
///        and WorldView (moved verbatim from map_fly.cpp). See detail/mesh_bake.h.

#include "detail/mesh_bake.h"

#include <cmath>

namespace castlemist::render {
namespace {

// ---------------------------------------------------------------------------
// Ambient occlusion bake
//
// The cheapest AO that still reads as real occlusion: voxelize the model once
// into a coarse occupancy grid, then march a handful of hemisphere rays per
// vertex through it. Everything is model space, so the result is shared by every
// instance of that model and can live in the vertex -- which is the whole reason
// this view can instance at all. It cannot see between objects (a prop does not
// darken the terrain under it); that would need a screen-space pass, and this
// buys most of the readability for none of the per-frame cost.
// ---------------------------------------------------------------------------

constexpr int kAoDirs = 16;  ///< Hemisphere rays per vertex.
constexpr int kAoSteps = 8;  ///< Voxels marched per ray.

/// @brief Cosine-weighted directions in the +Z hemisphere, built once.
///
/// A spiral (Fibonacci) distribution rather than random: for as few as 16 rays,
/// stratification matters far more than randomness, and a fixed set means the
/// bake is deterministic.
const std::vector<Vec3>& hemisphere_dirs() {
    static const std::vector<Vec3> dirs = [] {
        std::vector<Vec3> d;
        d.reserve(kAoDirs);
        const float ga = 2.399963f; // golden angle
        for (int i = 0; i < kAoDirs; ++i) {
            float t = (static_cast<float>(i) + 0.5f) / kAoDirs;
            float z = std::sqrt(1.0f - t);   // cosine-weighted
            float r = std::sqrt(t);
            float a = ga * i;
            d.push_back({r * std::cos(a), r * std::sin(a), z});
        }
        return d;
    }();
    return dirs;
}

/// @brief A model-space occupancy grid, plus the mapping from world to cell.
struct VoxelGrid {
    int n = 0;                       ///< Cells per axis.
    float lo[3] = {0, 0, 0};
    float inv[3] = {0, 0, 0};        ///< 1 / cell size, per axis.
    float cell = 1.0f;               ///< Mean cell size, for the march step.
    std::vector<uint8_t> occ;

    bool occupied(int x, int y, int z) const {
        if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) return false;
        return occ[(static_cast<size_t>(z) * n + y) * n + x] != 0;
    }
    void mark(float px, float py, float pz) {
        int x = static_cast<int>((px - lo[0]) * inv[0]);
        int y = static_cast<int>((py - lo[1]) * inv[1]);
        int z = static_cast<int>((pz - lo[2]) * inv[2]);
        if (x < 0 || y < 0 || z < 0 || x >= n || y >= n || z >= n) return;
        occ[(static_cast<size_t>(z) * n + y) * n + x] = 1;
    }
};

/// @brief Point-samples every triangle into the grid.
///
/// Sample count follows the triangle's size in cells, so a huge terrain quad and
/// a tiny prop face both cost about what they should. The per-triangle cap keeps
/// one pathological triangle (a skybox-sized plane, say) from stalling the bake.
VoxelGrid voxelize(const std::vector<MapVtx>& v, const std::vector<uint32_t>& idx, int n) {
    VoxelGrid g;
    g.n = n;
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    g.lo[0] = g.lo[1] = g.lo[2] = 1e30f;
    for (const auto& x : v) {
        const float p[3] = {x.px, x.py, x.pz};
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(p[k])) continue;
            g.lo[k] = std::min(g.lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    float sum = 0;
    for (int k = 0; k < 3; ++k) {
        if (g.lo[k] > hi[k]) { g.lo[k] = 0; hi[k] = 1; }
        float ext = std::max(hi[k] - g.lo[k], 1e-4f);
        // A hair of padding so surface samples never land outside the grid.
        g.lo[k] -= ext * 0.01f;
        ext *= 1.02f;
        g.inv[k] = static_cast<float>(n) / ext;
        sum += ext / static_cast<float>(n);
    }
    g.cell = sum / 3.0f;
    g.occ.assign(static_cast<size_t>(n) * n * n, 0);

    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        uint32_t ia = idx[t], ib = idx[t + 1], ic = idx[t + 2];
        if (ia >= v.size() || ib >= v.size() || ic >= v.size()) continue;
        const MapVtx& A = v[ia];
        const MapVtx& B = v[ib];
        const MapVtx& C = v[ic];
        float e0 = std::fabs(B.px - A.px) * g.inv[0] + std::fabs(B.py - A.py) * g.inv[1] +
                   std::fabs(B.pz - A.pz) * g.inv[2];
        float e1 = std::fabs(C.px - A.px) * g.inv[0] + std::fabs(C.py - A.py) * g.inv[1] +
                   std::fabs(C.pz - A.pz) * g.inv[2];
        int steps = static_cast<int>(std::ceil(std::max(e0, e1)));
        steps = std::clamp(steps, 1, 24);
        for (int i = 0; i <= steps; ++i) {
            for (int j = 0; i + j <= steps; ++j) {
                float u = static_cast<float>(i) / steps, w = static_cast<float>(j) / steps;
                float s = 1.0f - u - w;
                g.mark(A.px * s + B.px * u + C.px * w, A.py * s + B.py * u + C.py * w,
                       A.pz * s + B.pz * u + C.pz * w);
            }
        }
    }
    return g;
}

} // namespace

/// @brief Marches the hemisphere and writes the AO byte back into each vertex.
void bake_ao(std::vector<MapVtx>& v, const std::vector<uint32_t>& idx) {
    if (v.empty() || idx.size() < 3) {
        for (auto& x : v) x.ao = 255;
        return;
    }
    // A denser grid for bigger meshes, but capped -- 48^3 is 110k cells, still
    // trivial next to the vertex work.
    int n = v.size() > 60000 ? 48 : 32;
    VoxelGrid g = voxelize(v, idx, n);
    const std::vector<Vec3>& dirs = hemisphere_dirs();

    for (auto& x : v) {
        Vec3 N{x.nx / 127.5f - 1.0f, x.ny / 127.5f - 1.0f, x.nz / 127.5f - 1.0f};
        float len = std::sqrt(N.x * N.x + N.y * N.y + N.z * N.z);
        if (len < 1e-4f) { x.ao = 255; continue; }
        N = {N.x / len, N.y / len, N.z / len};

        // Orthonormal frame about the normal (Hughes-Moller: cross with the
        // least-aligned basis axis, so it never degenerates).
        Vec3 a = (std::fabs(N.x) < 0.9f) ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 T = norm(cross(a, N));
        Vec3 B = cross(N, T);

        int blocked = 0;
        for (const Vec3& d : dirs) {
            Vec3 w{T.x * d.x + B.x * d.y + N.x * d.z, T.y * d.x + B.y * d.y + N.y * d.z,
                   T.z * d.x + B.z * d.y + N.z * d.z};
            // Start clear of the surface the vertex sits on, or every ray would
            // instantly hit the vertex's own triangle.
            float t = g.cell * 1.5f;
            for (int s = 0; s < kAoSteps; ++s, t += g.cell) {
                float px = x.px + w.x * t, py = x.py + w.y * t, pz = x.pz + w.z * t;
                int cx = static_cast<int>((px - g.lo[0]) * g.inv[0]);
                int cy = static_cast<int>((py - g.lo[1]) * g.inv[1]);
                int cz = static_cast<int>((pz - g.lo[2]) * g.inv[2]);
                if (g.occupied(cx, cy, cz)) { ++blocked; break; }
            }
        }
        float ao = 1.0f - static_cast<float>(blocked) / kAoDirs;
        ao = ao * ao * (3.0f - 2.0f * ao);          // smoothstep: firm up the contrast
        ao = 0.30f + 0.70f * ao;                     // keep occluded areas readable, not black
        x.ao = static_cast<uint8_t>(std::clamp(ao, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

uint8_t enc_n(float c) {
    return static_cast<uint8_t>(std::clamp(c * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/// @brief Flattens every submesh of a ModelPreview into one vertex/index pair.
///
/// Concatenating is safe here precisely because there are no materials: with
/// nothing to rebind between submeshes, the model collapses to a single draw.
/// LOD k merges each submesh's LOD k, falling back to that submesh's coarsest
/// level when it has fewer than the model as a whole.
void flatten(const ModelPreview& mp, std::vector<MapVtx>& verts,
             std::vector<uint32_t>& indices,
             std::vector<std::pair<uint32_t, uint32_t>>& lodRanges) {
    size_t maxLods = 1;
    for (const auto& m : mp.meshes) maxLods = std::max(maxLods, m.lodIndices.size() + 1);

    uint32_t vbase = 0;
    std::vector<uint32_t> base;
    base.reserve(mp.meshes.size());
    for (const auto& m : mp.meshes) {
        base.push_back(vbase);
        for (const auto& gv : m.vertices) {
            MapVtx o{};
            o.px = gv.px; o.py = gv.py; o.pz = gv.pz;
            o.nx = enc_n(gv.nx); o.ny = enc_n(gv.ny); o.nz = enc_n(gv.nz);
            o.ao = 255;
            verts.push_back(o);
        }
        vbase += static_cast<uint32_t>(m.vertices.size());
    }

    for (size_t lod = 0; lod < maxLods; ++lod) {
        uint32_t start = static_cast<uint32_t>(indices.size());
        for (size_t mi = 0; mi < mp.meshes.size(); ++mi) {
            const ModelMeshCPU& m = mp.meshes[mi];
            const std::vector<uint32_t>* src = &m.indices;
            if (lod > 0) {
                size_t k = std::min(lod - 1, m.lodIndices.empty() ? size_t(0) : m.lodIndices.size() - 1);
                if (!m.lodIndices.empty()) src = &m.lodIndices[k];
            }
            for (uint32_t i : *src) {
                if (i < m.vertices.size()) indices.push_back(base[mi] + i);
            }
        }
        lodRanges.emplace_back(start, static_cast<uint32_t>(indices.size()) - start);
    }
}

/// @brief Six frustum planes (xyz = normal, w = d) from a row-vector view-proj.
void frustum_planes(const Mat4& vp, float pl[6][4]) {
    // Gribb-Hartmann: rows of the matrix combine into the clip planes. With the
    // row-vector convention the "rows" are the strided columns m[k], m[4+k]...
    auto col = [&](int c, int r) { return vp.m[r * 4 + c]; };
    for (int i = 0; i < 6; ++i) {
        int c = i >> 1;
        float sgn = (i & 1) ? -1.0f : 1.0f;
        for (int r = 0; r < 4; ++r) {
            // near plane is z (not w+z) in the D3D 0..1 depth range
            float wcomp = (c == 2 && !(i & 1)) ? 0.0f : col(3, r);
            pl[i][r] = wcomp + sgn * col(c, r);
        }
        float n = std::sqrt(pl[i][0] * pl[i][0] + pl[i][1] * pl[i][1] + pl[i][2] * pl[i][2]);
        if (n > 1e-8f) for (int r = 0; r < 4; ++r) pl[i][r] /= n;
    }
}

} // namespace castlemist::render
