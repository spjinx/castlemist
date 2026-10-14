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

float ray_model_hit(const ModelPreview& m, const float w[16], const float o[3], const float d[3]) {
    for (int k = 0; k < 16; ++k)
        if (!std::isfinite(w[k])) return -1;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(o[k]) || !std::isfinite(d[k])) return -1;
    // A(r,c) = w[c*4 + r]; its inverse by cofactors, in double.
    auto A = [&](int r, int c) { return static_cast<double>(w[c * 4 + r]); };
    const double c00 = A(1, 1) * A(2, 2) - A(1, 2) * A(2, 1), c01 = A(1, 2) * A(2, 0) - A(1, 0) * A(2, 2),
                 c02 = A(1, 0) * A(2, 1) - A(1, 1) * A(2, 0);
    const double det = A(0, 0) * c00 + A(0, 1) * c01 + A(0, 2) * c02;
    if (!std::isfinite(det) || std::fabs(det) < 1e-30) return -1;
    const double id = 1.0 / det;
    const double inv[3][3] = {
        {c00 * id, (A(0, 2) * A(2, 1) - A(0, 1) * A(2, 2)) * id, (A(0, 1) * A(1, 2) - A(0, 2) * A(1, 1)) * id},
        {c01 * id, (A(0, 0) * A(2, 2) - A(0, 2) * A(2, 0)) * id, (A(0, 2) * A(1, 0) - A(0, 0) * A(1, 2)) * id},
        {c02 * id, (A(0, 1) * A(2, 0) - A(0, 0) * A(2, 1)) * id, (A(0, 0) * A(1, 1) - A(0, 1) * A(1, 0)) * id},
    };
    // Model-space ray: o' = A^-1 (o - T), d' = A^-1 d, d' left unnormalized
    // so that o' + t d' maps back to o + t d (t in the caller's units).
    const double rel[3] = {o[0] - static_cast<double>(w[12]), o[1] - static_cast<double>(w[13]),
                           o[2] - static_cast<double>(w[14])};
    double mo[3], md[3];
    for (int r = 0; r < 3; ++r) {
        mo[r] = inv[r][0] * rel[0] + inv[r][1] * rel[1] + inv[r][2] * rel[2];
        md[r] = inv[r][0] * d[0] + inv[r][1] * d[1] + inv[r][2] * d[2];
    }

    // Moller-Trumbore, two-sided, over every mesh's LOD0 triangles.
    double best = -1;
    for (const ModelMeshCPU& mesh : m.meshes) {
        const size_t nv = mesh.vertices.size();
        const std::vector<uint32_t>& ix = mesh.indices;
        for (size_t t = 0; t + 2 < ix.size(); t += 3) {
            if (ix[t] >= nv || ix[t + 1] >= nv || ix[t + 2] >= nv) continue;
            const GVertex& a = mesh.vertices[ix[t]];
            const GVertex& b = mesh.vertices[ix[t + 1]];
            const GVertex& c = mesh.vertices[ix[t + 2]];
            const double e1[3] = {double(b.px) - a.px, double(b.py) - a.py, double(b.pz) - a.pz};
            const double e2[3] = {double(c.px) - a.px, double(c.py) - a.py, double(c.pz) - a.pz};
            const double p[3] = {md[1] * e2[2] - md[2] * e2[1], md[2] * e2[0] - md[0] * e2[2],
                                 md[0] * e2[1] - md[1] * e2[0]};
            const double det2 = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (std::fabs(det2) < 1e-20) continue;   // parallel or degenerate
            const double inv2 = 1.0 / det2;
            const double s[3] = {mo[0] - a.px, mo[1] - a.py, mo[2] - a.pz};
            const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv2;
            if (u < 0 || u > 1) continue;
            const double q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            const double v = (md[0] * q[0] + md[1] * q[1] + md[2] * q[2]) * inv2;
            if (v < 0 || u + v > 1) continue;
            const double tt = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv2;
            if (tt >= 0 && std::isfinite(tt) && (best < 0 || tt < best)) best = tt;
        }
    }
    return best < 0 ? -1.0f : static_cast<float>(best);
}

} // namespace castlemist::render
