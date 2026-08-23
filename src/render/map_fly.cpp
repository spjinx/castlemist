/// @file
/// @brief Mesh-only fly-through map view: compact upload, baked AO, instanced draw.
///
/// See `detail/map_fly.h` for why this exists as a second path rather than a
/// mode of `render_scene()`.

#include "detail/map_fly.h"
#include "detail/state.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>

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

/// @brief Runs `fn(i)` for i in [0,count) across the machine's cores.
template <typename F>
void parallel_for(size_t count, F fn) {
    unsigned hw = std::thread::hardware_concurrency();
    size_t workers = std::min<size_t>(count, hw ? hw : 4);
    if (workers <= 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (size_t i = next++; i < count; i = next++) fn(i);
        });
    }
    for (auto& t : pool) t.join();
}

/// @brief CPU-side result of preparing one model, before any D3D call.
struct Prepared {
    std::vector<MapVtx> verts;
    std::vector<uint32_t> indices;
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;
};

bool create_model_buffers(const Prepared& p, const ModelPreview& mp, FlyModelGPU& out) {
    if (p.verts.empty() || p.indices.empty()) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(p.verts.size() * sizeof(MapVtx));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{p.verts.data(), 0, 0};
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.vb))) return false;

    bd.ByteWidth = static_cast<UINT>(p.indices.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = p.indices.data();
    if (FAILED(g_dev->CreateBuffer(&bd, &sd, &out.ib))) { out.vb.Reset(); return false; }

    out.lodRanges = p.lodRanges;
    out.vertCount = static_cast<uint32_t>(p.verts.size());
    for (int k = 0; k < 3; ++k) out.center[k] = mp.center[k];
    out.radius = (std::isfinite(mp.radius) && mp.radius > 0) ? mp.radius : 1.0f;
    out.ok = true;
    return true;
}

/// @brief Grows the shared per-instance vertex buffer to hold `need` entries.
bool ensure_inst_capacity(uint32_t need) {
    if (g_flyInstVB && g_flyInstCap >= need) return true;
    uint32_t cap = std::max<uint32_t>(need + need / 2, 4096);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = cap * sizeof(MapInstGPU);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> nb;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &nb))) return false;
    g_flyInstVB = nb;
    g_flyInstCap = cap;
    return true;
}

/// @brief Packs a row-vector world matrix into the three columns the VS wants.
void pack_inst(const Mat4& w, int layer, MapInstGPU& o) {
    o.r0[0] = w.m[0]; o.r0[1] = w.m[4]; o.r0[2] = w.m[8];  o.r0[3] = w.m[12];
    o.r1[0] = w.m[1]; o.r1[1] = w.m[5]; o.r1[2] = w.m[9];  o.r1[3] = w.m[13];
    o.r2[0] = w.m[2]; o.r2[1] = w.m[6]; o.r2[2] = w.m[10]; o.r2[3] = w.m[14];
    int l = (layer >= 0 && layer < LAYER_COUNT) ? layer : LAYER_PROP;
    for (int k = 0; k < 3; ++k) o.tint[k] = kFlyLayerTint[l][k];
    o.tint[3] = 1.0f;
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

} // namespace

// ---------------------------------------------------------------------------
// Build / teardown
// ---------------------------------------------------------------------------

void clear_fly_scene() {
    g_fly_models.clear();
    g_fly_insts.clear();
    g_fly_built = false;
}

/// @brief Prepares compact buffers + AO for `models`, appending at `base`.
///
/// The expensive half (flatten + voxelize + march) touches no D3D and is run
/// across every core; buffer creation is serial afterwards because the device
/// context is not free-threaded.
void fly_add_models(const std::vector<ModelPreview>& models, size_t base) {
    if (!g_dev || models.empty()) return;
    if (g_fly_models.size() < base + models.size()) g_fly_models.resize(base + models.size());

    std::vector<Prepared> prep(models.size());
    parallel_for(models.size(), [&](size_t i) {
        flatten(models[i], prep[i].verts, prep[i].indices, prep[i].lodRanges);
        bake_ao(prep[i].verts, prep[i].indices);
    });
    for (size_t i = 0; i < models.size(); ++i)
        create_model_buffers(prep[i], models[i], g_fly_models[base + i]);
}

void fly_add_instance(int model, const Mat4& world, float radius, int layer) {
    if (model < 0 || model >= static_cast<int>(g_fly_models.size())) return;
    const FlyModelGPU& fm = g_fly_models[model];
    if (!fm.ok) return;
    FlyInstCPU in;
    in.model = model;
    in.layer = layer;
    in.world = world;
    Vec3 c = transformPoint({fm.center[0], fm.center[1], fm.center[2]}, world);
    in.centerW[0] = c.x; in.centerW[1] = c.y; in.centerW[2] = c.z;
    in.radiusW = radius;
    g_fly_insts.push_back(in);
    g_fly_built = true;
}

void fly_frame_scene() {
    // Frame on the FULL scene box, terrain included. g_scene_center/g_scene_radius
    // track the prop-only box (so the orbit camera is not dragged out by a
    // map-sized ground plane), and using those here dropped the camera at the
    // props' own scale -- which on a map whose props cluster inside a 6k-unit
    // terrain plane starts you *underneath* the ground looking up.
    Vec3 lo = g_scene_lo, hi = g_scene_hi;
    Vec3 c{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    float ex = hi.x - lo.x, ey = hi.y - lo.y, ez = hi.z - lo.z;
    float R = 0.5f * std::sqrt(ex * ex + ey * ey + ez * ez);
    if (!std::isfinite(R) || R < 1.0f) { R = std::max(g_scene_radius, 1.0f); c = g_scene_center; }

    // Stand back along -X at the scene's own scale, and clearly ABOVE the highest
    // point in it rather than above its centre. R is the box half-DIAGONAL, which
    // overestimates what actually has to fit on screen, so the pull-back is well
    // under 1R -- at 1.15R the map sat in the middle of the view as a postage stamp.
    g_fly_pos = {c.x - R * 0.62f, c.y, hi.z + R * 0.28f};
    Vec3 aim{c.x - g_fly_pos.x, c.y - g_fly_pos.y, c.z - g_fly_pos.z};
    g_fly_fwd = norm(aim);
    // Level horizon: up is world +Z projected perpendicular to forward.
    Vec3 right = cross(g_fly_fwd, Vec3{0, 0, 1});
    if (dot(right, right) < 1e-6f) right = Vec3{0, 1, 0};
    g_fly_up = norm(cross(norm(right), g_fly_fwd));

    // Cross the scene in a few seconds, and see across all of it.
    g_fly_speed = std::clamp(R * 0.5f, 20.0f, 20000.0f);
    g_fly_view_dist = std::max(R * 3.0f, 5000.0f);
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

void render_fly() {
    if (!g_fly_pipeline_ok || g_fly_insts.empty()) return;

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    // --- camera --------------------------------------------------------------
    // Built from the camera's own basis, so there is no world up-vector to go
    // parallel with and no orientation the view cannot reach.
    Vec3 at{g_fly_pos.x + g_fly_fwd.x, g_fly_pos.y + g_fly_fwd.y, g_fly_pos.z + g_fly_fwd.z};
    Mat4 view = lookAt(g_fly_pos, at, g_fly_up);
    float aspect = static_cast<float>(g_w) / std::max(1, g_h);
    // Near/far scaled to the map: a fixed 0.1 near plane on a 30k-unit map is
    // all the depth precision gone.
    float far_ = std::max(g_fly_view_dist * 1.2f, 1000.0f);
    float near_ = std::clamp(far_ * 0.00005f, 0.05f, 20.0f);
    Mat4 proj = perspective(1.15f, aspect, near_, far_);
    Mat4 VP = mul(view, proj);

    float planes[6][4];
    frustum_planes(VP, planes);

    // --- cull + LOD select, bucketed by (model, lod) ------------------------
    struct Bucket { int model, lod; std::vector<const FlyInstCPU*> insts; };
    static std::vector<Bucket> buckets;      // reused across frames; no per-frame churn
    static std::vector<int> bucketOf;
    buckets.clear();
    bucketOf.assign(g_fly_models.size() * 4, -1);

    int visible = 0;
    float cullDistSq = g_fly_view_dist * g_fly_view_dist;
    for (const auto& in : g_fly_insts) {
        if (!g_layer_visible[in.layer]) continue;
        float dx = in.centerW[0] - g_fly_pos.x;
        float dy = in.centerW[1] - g_fly_pos.y;
        float dz = in.centerW[2] - g_fly_pos.z;
        float d2 = dx * dx + dy * dy + dz * dz;
        // Distance cull first: it is one compare and rejects most of a big map.
        if (d2 > cullDistSq + in.radiusW * in.radiusW) continue;

        bool out = false;
        for (int p = 0; p < 6 && !out; ++p) {
            float dist = planes[p][0] * in.centerW[0] + planes[p][1] * in.centerW[1] +
                         planes[p][2] * in.centerW[2] + planes[p][3];
            if (dist < -in.radiusW) out = true;
        }
        if (out) continue;

        // Screen-relative size picks the LOD: a distant prop that covers a few
        // pixels has no business submitting its full-detail index set.
        const FlyModelGPU& fm = g_fly_models[in.model];
        int maxLod = static_cast<int>(fm.lodRanges.size()) - 1;
        int lod = 0;
        if (maxLod > 0) {
            float d = std::sqrt(std::max(d2, 1e-4f));
            float sr = in.radiusW / d;             // ~ tan(angular radius)
            lod = (sr > 0.20f) ? 0 : (sr > 0.06f ? 1 : (sr > 0.02f ? 2 : 3));
            lod = std::min(lod, maxLod);
        }
        size_t key = static_cast<size_t>(in.model) * 4 + lod;
        if (key >= bucketOf.size()) continue;
        if (bucketOf[key] < 0) {
            bucketOf[key] = static_cast<int>(buckets.size());
            buckets.push_back({in.model, lod, {}});
        }
        buckets[bucketOf[key]].insts.push_back(&in);
        ++visible;
    }

    g_fly_stat_visible = visible;
    if (visible == 0) {
        g_fly_stat_draws = 0; g_fly_stat_tris = 0;
        QueryPerformanceCounter(&t1);
        if (g_qpc_freq.QuadPart)
            g_fly_stat_ms = static_cast<float>(1000.0 * double(t1.QuadPart - t0.QuadPart) / g_qpc_freq.QuadPart);
        return;
    }
    if (!ensure_inst_capacity(static_cast<uint32_t>(visible))) return;

    // --- one upload for the whole visible set -------------------------------
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_flyInstVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    auto* dst = static_cast<MapInstGPU*>(ms.pData);
    uint32_t off = 0;
    std::vector<uint32_t> bucketOffset(buckets.size());
    for (size_t b = 0; b < buckets.size(); ++b) {
        bucketOffset[b] = off;
        for (const FlyInstCPU* in : buckets[b].insts) pack_inst(in->world, in->layer, dst[off++]);
    }
    g_ctx->Unmap(g_flyInstVB.Get(), 0);

    // --- constants ----------------------------------------------------------
    FlyCB cb{};
    cb.viewProj = VP;
    const MapEnvRig& env = g_env_rig;
    Vec3 sun = norm({env.sunDir[0], env.sunDir[1], env.sunDir[2]});
    cb.sunDir[0] = sun.x; cb.sunDir[1] = sun.y; cb.sunDir[2] = sun.z;
    // Budget: ambient peaks near 0.30 and the sun adds up to ~0.55, so a fully
    // lit unoccluded surface lands around 0.85 instead of clipping to white --
    // which is what the first build did, flattening the whole map into a sheet
    // of paper with black holes in it.
    for (int k = 0; k < 3; ++k) cb.sunCol[k] = env.sunColor[k] * 0.55f;
    // Sky/ground come from the map's own fill colour so different zones do not
    // all read as the same grey box.
    cb.skyCol[0] = env.fillColor[0] * 0.26f; cb.skyCol[1] = env.fillColor[1] * 0.28f;
    cb.skyCol[2] = env.fillColor[2] * 0.32f;
    cb.groundCol[0] = 0.07f; cb.groundCol[1] = 0.065f; cb.groundCol[2] = 0.06f;
    cb.fogCol[0] = 0.10f; cb.fogCol[1] = 0.11f; cb.fogCol[2] = 0.13f;  // == the clear colour
    cb.exposure = g_light_intensity;
    cb.aoStrength = g_fly_ao_strength;
    cb.fogStart = g_fly_fog ? g_fly_view_dist * 0.55f : 1e9f;
    cb.fogEnd = g_fly_fog ? g_fly_view_dist : 1e9f + 1.0f;
    cb.camPos[0] = g_fly_pos.x; cb.camPos[1] = g_fly_pos.y; cb.camPos[2] = g_fly_pos.z;
    if (SUCCEEDED(g_ctx->Map(g_flyCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, &cb, sizeof cb);
        g_ctx->Unmap(g_flyCB.Get(), 0);
    }

    // --- draw ---------------------------------------------------------------
    g_ctx->IASetInputLayout(g_flyIL.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_flyVS.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_flyPS.Get(), nullptr, 0);
    ID3D11Buffer* cbs[] = {g_flyCB.Get()};
    g_ctx->VSSetConstantBuffers(0, 1, cbs);
    g_ctx->PSSetConstantBuffers(0, 1, cbs);
    g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blendOpaque.Get(), bf, 0xffffffff);
    g_ctx->RSSetState(g_fly_wire ? g_rsWire.Get() : g_rsSolid.Get());

    int draws = 0;
    size_t tris = 0;
    for (size_t b = 0; b < buckets.size(); ++b) {
        const Bucket& bk = buckets[b];
        const FlyModelGPU& fm = g_fly_models[bk.model];
        if (!fm.ok || bk.lod >= static_cast<int>(fm.lodRanges.size())) continue;
        auto range = fm.lodRanges[bk.lod];
        if (range.second == 0) continue;

        ID3D11Buffer* vbs[] = {fm.vb.Get(), g_flyInstVB.Get()};
        UINT strides[] = {sizeof(MapVtx), sizeof(MapInstGPU)};
        UINT offsets[] = {0, bucketOffset[b] * sizeof(MapInstGPU)};
        g_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        g_ctx->IASetIndexBuffer(fm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_ctx->DrawIndexedInstanced(range.second, static_cast<UINT>(bk.insts.size()), range.first, 0, 0);
        ++draws;
        tris += static_cast<size_t>(range.second / 3) * bk.insts.size();
    }

    g_fly_stat_draws = draws;
    g_fly_stat_tris = static_cast<int>(tris / 1000);
    QueryPerformanceCounter(&t1);
    if (g_qpc_freq.QuadPart)
        g_fly_stat_ms = static_cast<float>(1000.0 * double(t1.QuadPart - t0.QuadPart) / g_qpc_freq.QuadPart);
}

} // namespace castlemist::render
