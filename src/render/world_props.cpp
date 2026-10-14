/// @file
/// @brief CPU-side preparation of WorldView's prop models.

#include "world_props.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace castlemist::render {

PreparedModel prepare_world_model(const ModelPreview& m) {
    PreparedModel p;
    if (m.meshes.empty()) return p;
    if (!std::isfinite(m.radius) || !std::isfinite(m.center[0]) || !std::isfinite(m.center[1]) ||
        !std::isfinite(m.center[2]))
        return p;
    flatten(m, p.verts, p.indices, p.lodRanges);
    if (p.verts.empty() || p.lodRanges.empty() || p.lodRanges[0].second == 0) {
        p = PreparedModel{};
        return p;
    }
    bake_ao(p.verts, p.indices);
    for (int k = 0; k < 3; ++k) p.center[k] = m.center[k];
    p.radius = m.radius;
    if (p.radius <= 0) {
        float r2 = 0;
        for (const MapVtx& v : p.verts) {
            const float dx = v.px - p.center[0], dy = v.py - p.center[1], dz = v.pz - p.center[2];
            r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
        }
        p.radius = std::max(std::sqrt(r2), 1.0f);
    }
    p.ok = true;
    return p;
}

PreparedModel prepare_collision_mesh(const castlemist::world::CollisionMesh& m) {
    PreparedModel p;
    const size_t nv = m.verts.size() / 3;
    if (nv == 0) return p;
    for (float f : m.verts)
        if (!std::isfinite(f)) return p;
    p.indices.reserve(m.indices.size());
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        if (a >= nv || b >= nv || c >= nv) continue;
        p.indices.insert(p.indices.end(), {a, b, c});
    }
    if (p.indices.empty()) return p;

    // Area-weighted vertex normals: the unnormalized cross product is twice
    // the triangle's area, so summing it weights each face by its size.
    std::vector<float> acc(nv * 3, 0.0f);
    const float* v = m.verts.data();
    for (size_t t = 0; t < p.indices.size(); t += 3) {
        const uint32_t i0 = p.indices[t], i1 = p.indices[t + 1], i2 = p.indices[t + 2];
        const Vec3 e1{v[i1 * 3] - v[i0 * 3], v[i1 * 3 + 1] - v[i0 * 3 + 1], v[i1 * 3 + 2] - v[i0 * 3 + 2]};
        const Vec3 e2{v[i2 * 3] - v[i0 * 3], v[i2 * 3 + 1] - v[i0 * 3 + 1], v[i2 * 3 + 2] - v[i0 * 3 + 2]};
        const Vec3 n = cross(e1, e2);
        for (uint32_t i : {i0, i1, i2}) {
            acc[i * 3] += n.x; acc[i * 3 + 1] += n.y; acc[i * 3 + 2] += n.z;
        }
    }
    float lo[3] = {v[0], v[1], v[2]}, hi[3] = {v[0], v[1], v[2]};
    p.verts.resize(nv);
    for (size_t i = 0; i < nv; ++i) {
        MapVtx& o = p.verts[i];
        o.px = v[i * 3]; o.py = v[i * 3 + 1]; o.pz = v[i * 3 + 2];
        const Vec3 n = norm({acc[i * 3], acc[i * 3 + 1], acc[i * 3 + 2]});
        o.nx = enc_n(n.x); o.ny = enc_n(n.y); o.nz = enc_n(n.z);
        o.ao = 255;
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], v[i * 3 + k]);
            hi[k] = std::max(hi[k], v[i * 3 + k]);
        }
    }
    for (int k = 0; k < 3; ++k) p.center[k] = (lo[k] + hi[k]) * 0.5f;
    float r2 = 0;
    for (const MapVtx& o : p.verts) {
        const float dx = o.px - p.center[0], dy = o.py - p.center[1], dz = o.pz - p.center[2];
        r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    p.radius = std::max(std::sqrt(r2), 1.0f);
    p.lodRanges.emplace_back(0u, static_cast<uint32_t>(p.indices.size()));
    p.ok = true;
    return p;
}

Mat4 colmajor_to_rowvec(const float w[16]) {
    Mat4 r;
    std::memcpy(r.m, w, sizeof r.m);   // see the header: R = M^T is the same bytes
    return r;
}

float max_column_length(const float w[16]) {
    float best = 0;
    for (int c = 0; c < 3; ++c) {
        const float* col = w + c * 4;
        best = std::max(best, std::sqrt(col[0] * col[0] + col[1] * col[1] + col[2] * col[2]));
    }
    return best;
}

} // namespace castlemist::render
