/// @file
/// @brief CollisionIndex: XY grid over map-space triangles, DDA ray cast.

#include "castlemist/world/walk.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace castlemist::world {

namespace {

void transform_point(const float* m, const float* p, float* out) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

/// Moller-Trumbore, both faces. Returns t or a negative value on a miss.
float intersect(const float* o, const float* d, const float* tri) {
    const float* a = tri;
    const float e1[3] = {tri[3] - a[0], tri[4] - a[1], tri[5] - a[2]};
    const float e2[3] = {tri[6] - a[0], tri[7] - a[1], tri[8] - a[2]};
    const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
    const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
    if (std::fabs(det) < 1e-12f) return -1;
    const float inv = 1.0f / det;
    const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
    const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
    if (u < 0 || u > 1) return -1;
    const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
    const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
    if (v < 0 || u + v > 1) return -1;
    return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

void add_terrain(const WorldScene& scene, std::vector<float>& out) {
    for (const TerrainChunk& c : scene.terrain.chunks) {
        const int n = c.samples;
        if (n < 2 || c.heights.size() < (size_t)n * (size_t)n) continue;
        const float dx = (c.rect[2] - c.rect[0]) / (n - 1), dy = (c.rect[3] - c.rect[1]) / (n - 1);
        auto at = [&](int i, int j, float* p) {
            p[0] = c.rect[0] + i * dx;
            p[1] = c.rect[3] - j * dy;
            p[2] = c.heights[(size_t)j * n + i];
        };
        for (int j = 0; j + 1 < n; ++j) {
            for (int i = 0; i + 1 < n; ++i) {
                float p00[3], p10[3], p01[3], p11[3];
                at(i, j, p00); at(i + 1, j, p10); at(i, j + 1, p01); at(i + 1, j + 1, p11);
                for (const float* v : {p00, p10, p11, p00, p11, p01}) out.insert(out.end(), v, v + 3);
            }
        }
    }
}

} // namespace

CollisionIndex CollisionIndex::build(const WorldScene& scene, bool includeTerrain) {
    CollisionIndex idx;
    for (const CollisionInstance& inst : scene.collision.instances) {
        if (inst.mesh >= scene.collision.meshes.size()) continue;
        const CollisionMesh& m = scene.collision.meshes[inst.mesh];
        const size_t nv = m.verts.size() / 3;
        for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
            if (m.indices[k] >= nv || m.indices[k + 1] >= nv || m.indices[k + 2] >= nv) continue;
            for (int c = 0; c < 3; ++c) {
                float p[3];
                transform_point(inst.world, &m.verts[(size_t)m.indices[k + c] * 3], p);
                idx.tris_.insert(idx.tris_.end(), p, p + 3);
            }
        }
    }
    idx.collisionCount_ = idx.tris_.size() / 9;
    if (includeTerrain) add_terrain(scene, idx.tris_);
    idx.finish();
    return idx;
}

CollisionIndex CollisionIndex::from_triangles(const std::vector<float>& collisionTris,
                                              const std::vector<float>& terrainTris) {
    CollisionIndex idx;
    idx.tris_.assign(collisionTris.begin(), collisionTris.begin() + collisionTris.size() / 9 * 9);
    idx.collisionCount_ = idx.tris_.size() / 9;
    idx.tris_.insert(idx.tris_.end(), terrainTris.begin(), terrainTris.begin() + terrainTris.size() / 9 * 9);
    idx.finish();
    return idx;
}

void CollisionIndex::finish() {
    const size_t n = tris_.size() / 9;
    if (n == 0) return;
    for (int a = 0; a < 3; ++a) { lo_[a] = std::numeric_limits<float>::max(); hi_[a] = std::numeric_limits<float>::lowest(); }
    for (size_t k = 0; k < tris_.size(); k += 3)
        for (int a = 0; a < 3; ++a) { lo_[a] = std::min(lo_[a], tris_[k + a]); hi_[a] = std::max(hi_[a], tris_[k + a]); }

    const float ex = hi_[0] - lo_[0], ey = hi_[1] - lo_[1];
    const float div = std::clamp(std::sqrt((float)n / 4.0f), 1.0f, 1024.0f);
    cell_ = std::max(ex, ey) / div;
    if (!(cell_ > 0)) cell_ = 1;
    nx_ = std::max(1, (int)std::floor(ex / cell_) + 1);
    ny_ = std::max(1, (int)std::floor(ey / cell_) + 1);

    auto range = [&](size_t t, int& x0, int& x1, int& y0, int& y1) {
        const float* v = &tris_[t * 9];
        const float mnx = std::min({v[0], v[3], v[6]}), mxx = std::max({v[0], v[3], v[6]});
        const float mny = std::min({v[1], v[4], v[7]}), mxy = std::max({v[1], v[4], v[7]});
        x0 = std::clamp((int)std::floor((mnx - lo_[0]) / cell_), 0, nx_ - 1);
        x1 = std::clamp((int)std::floor((mxx - lo_[0]) / cell_), 0, nx_ - 1);
        y0 = std::clamp((int)std::floor((mny - lo_[1]) / cell_), 0, ny_ - 1);
        y1 = std::clamp((int)std::floor((mxy - lo_[1]) / cell_), 0, ny_ - 1);
    };

    // Two passes: count, then fill (CSR layout).
    cellStart_.assign((size_t)nx_ * ny_ + 1, 0);
    for (size_t t = 0; t < n; ++t) {
        int x0, x1, y0, y1;
        range(t, x0, x1, y0, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) ++cellStart_[(size_t)y * nx_ + x + 1];
    }
    for (size_t c = 1; c < cellStart_.size(); ++c) cellStart_[c] += cellStart_[c - 1];
    cellTris_.resize(cellStart_.back());
    std::vector<uint32_t> fill(cellStart_.begin(), cellStart_.end() - 1);
    for (size_t t = 0; t < n; ++t) {
        int x0, x1, y0, y1;
        range(t, x0, x1, y0, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) cellTris_[fill[(size_t)y * nx_ + x]++] = (uint32_t)t;
    }
}

size_t CollisionIndex::triangle_count(uint8_t mask) const {
    const size_t total = tris_.size() / 9;
    size_t n = 0;
    if (mask & kHitCollision) n += collisionCount_;
    if (mask & kHitTerrain) n += total - collisionCount_;
    return n;
}

void CollisionIndex::bounds(float lo[3], float hi[3]) const {
    for (int a = 0; a < 3; ++a) { lo[a] = lo_[a]; hi[a] = hi_[a]; }
}

std::optional<RayHit> CollisionIndex::raycast(const float origin[3], const float dir[3], float maxT,
                                              uint8_t mask) const {
    if (tris_.empty() || !(maxT > 0)) return std::nullopt;
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(len > 0)) return std::nullopt;
    const float d[3] = {dir[0] / len, dir[1] / len, dir[2] / len};

    float bestT = maxT;
    int bestTri = -1;
    auto test_cell = [&](int cx, int cy) {
        const size_t c = (size_t)cy * nx_ + cx;
        for (uint32_t k = cellStart_[c]; k < cellStart_[c + 1]; ++k) {
            const uint32_t t = cellTris_[k];
            const bool isColl = t < collisionCount_;
            if (!(mask & (isColl ? kHitCollision : kHitTerrain))) continue;
            const float h = intersect(origin, d, &tris_[(size_t)t * 9]);
            if (h >= 0 && h <= bestT) { bestT = h; bestTri = (int)t; }
        }
    };

    // Clip the ray's XY projection to the grid, giving the entry t.
    float tEnter = 0, tExit = maxT;
    for (int a = 0; a < 2; ++a) {
        const float extent = (a == 0 ? nx_ : ny_) * cell_;
        if (std::fabs(d[a]) < 1e-12f) {
            if (origin[a] < lo_[a] || origin[a] > lo_[a] + extent) return std::nullopt;
        } else {
            float t0 = (lo_[a] - origin[a]) / d[a], t1 = (lo_[a] + extent - origin[a]) / d[a];
            if (t0 > t1) std::swap(t0, t1);
            tEnter = std::max(tEnter, t0);
            tExit = std::min(tExit, t1);
        }
    }
    if (tEnter > tExit) return std::nullopt;

    const bool vertical = std::fabs(d[0]) < 1e-12f && std::fabs(d[1]) < 1e-12f;
    if (vertical) {
        test_cell(std::clamp((int)std::floor((origin[0] - lo_[0]) / cell_), 0, nx_ - 1),
                  std::clamp((int)std::floor((origin[1] - lo_[1]) / cell_), 0, ny_ - 1));
    } else {
        const float sx = origin[0] + d[0] * tEnter, sy = origin[1] + d[1] * tEnter;
        int cx = std::clamp((int)std::floor((sx - lo_[0]) / cell_), 0, nx_ - 1);
        int cy = std::clamp((int)std::floor((sy - lo_[1]) / cell_), 0, ny_ - 1);
        const float inf = std::numeric_limits<float>::infinity();
        const int stepX = d[0] > 0 ? 1 : -1, stepY = d[1] > 0 ? 1 : -1;
        float tMaxX = inf, tMaxY = inf, tDeltaX = inf, tDeltaY = inf;
        if (std::fabs(d[0]) >= 1e-12f) {
            tDeltaX = cell_ / std::fabs(d[0]);
            tMaxX = (lo_[0] + (cx + (stepX > 0 ? 1 : 0)) * cell_ - origin[0]) / d[0];
        }
        if (std::fabs(d[1]) >= 1e-12f) {
            tDeltaY = cell_ / std::fabs(d[1]);
            tMaxY = (lo_[1] + (cy + (stepY > 0 ? 1 : 0)) * cell_ - origin[1]) / d[1];
        }
        float entry = tEnter;
        while (entry <= bestT) {
            test_cell(cx, cy);
            if (tMaxX < tMaxY) { entry = tMaxX; cx += stepX; tMaxX += tDeltaX; }
            else               { entry = tMaxY; cy += stepY; tMaxY += tDeltaY; }
            if (cx < 0 || cx >= nx_ || cy < 0 || cy >= ny_) break;
        }
    }

    if (bestTri < 0) return std::nullopt;
    RayHit hit;
    hit.t = bestT;
    for (int a = 0; a < 3; ++a) hit.point[a] = origin[a] + d[a] * bestT;
    hit.source = (size_t)bestTri < collisionCount_ ? HitSource::Collision : HitSource::Terrain;
    const float* v = &tris_[(size_t)bestTri * 9];
    const float e1[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]}, e2[3] = {v[6] - v[0], v[7] - v[1], v[8] - v[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl > 0) for (float& c : n) c /= nl;
    if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0) for (float& c : n) c = -c;
    for (int a = 0; a < 3; ++a) hit.normal[a] = n[a];
    return hit;
}

// ---- Walker ----------------------------------------------------------------
// Behaviour follows T3D's PhysicsController (explorer/src/scene/physics-controller.ts,
// commit b3428b2; GPL-3, re-derived here, not copied), adapted from Y-up to up = -Z.

Walker::Walker(const CollisionIndex* index, WalkParams p) : index_(index), p_(p) {}

void Walker::reset(const float pos[3]) {
    for (int a = 0; a < 3; ++a) fallback_[a] = pos[a];
    hasSafe_ = false;
    grounded_ = false;
    jumpRequested_ = false;
    vy_ = 0;
}

bool Walker::snap_to_ground(float pos[3]) {
    if (!index_ || index_->empty()) return false;
    float lo[3], hi[3];
    index_->bounds(lo, hi);
    const float o[3] = {pos[0], pos[1], lo[2] - 1.0f};
    const float down[3] = {0, 0, 1};
    const auto hit = index_->raycast(o, down, hi[2] - lo[2] + 2.0f, kHitAll);
    if (!hit) return false;
    pos[2] = hit->point[2] - p_.eyeHeight;
    grounded_ = true;
    vy_ = 0;
    for (int a = 0; a < 3; ++a) safe_[a] = fallback_[a] = pos[a];
    hasSafe_ = true;
    return true;
}

bool Walker::blocked(const float pos[3], float mx, float my) const {
    const float dist = std::sqrt(mx * mx + my * my);
    if (!(dist > 0)) return false;
    const float dir[3] = {mx, my, 0};
    for (float off : p_.probeBelowEye) {
        const float o[3] = {pos[0], pos[1], pos[2] + off};
        if (index_->raycast(o, dir, dist + p_.padding, kHitCollision)) return true;
    }
    return false;
}

bool Walker::ground_check(float pos[3], float prevZ) {
    // Deliberately differs from T3D's findGroundY: the ray starts at the eye, not
    // eyeHeight above it, so a ceiling within eyeHeight above the eye is never
    // taken for ground (T3D would teleport the walker onto it). Steps the wall
    // probes allow lie between the eye and the feet, so step-ups still work.
    // The ray is swept from the higher of the old/new eye so a fast fall cannot
    // skip the floor; length = this frame's travel + 2*eyeHeight.
    const float top = std::min(prevZ, pos[2]), bottom = std::max(prevZ, pos[2]);
    const float o[3] = {pos[0], pos[1], top};
    const float down[3] = {0, 0, 1};
    const auto hit = index_->raycast(o, down, (bottom - top) + 2 * p_.eyeHeight, kHitAll);
    if (!hit) return false;
    const float target = hit->point[2] - p_.eyeHeight;
    if (target - pos[2] > p_.groundSnap) return false;   // eye is still above the ground
    pos[2] = target;                                     // standing, or snapped up/onto a step
    return true;
}

StepResult Walker::step(float pos[3], const float horizontalDelta[2], float dt) {
    StepResult res;
    if (!index_) return res;
    dt = std::clamp(dt, 0.0f, p_.maxDt);

    // Horizontal: full delta, else each axis on its own (slide).
    const float dx = horizontalDelta[0], dy = horizontalDelta[1];
    if (dx != 0 || dy != 0) {
        if (!blocked(pos, dx, dy)) { pos[0] += dx; pos[1] += dy; }
        else {
            if (dx != 0 && !blocked(pos, dx, 0)) pos[0] += dx;
            if (dy != 0 && !blocked(pos, 0, dy)) pos[1] += dy;
        }
    }

    // Vertical.
    if (jumpRequested_ && grounded_) vy_ = p_.jumpVelocity;
    jumpRequested_ = false;
    const float prevZ = pos[2];
    // maxFall doubles as terminal velocity (units/s): with dt <= maxDt a frame moves
    // at most maxFall*maxDt, which the swept ground ray covers.
    vy_ = std::max(vy_ - p_.gravity * dt, -p_.maxFall);
    pos[2] -= vy_ * dt;

    grounded_ = false;
    if (vy_ <= 0 && ground_check(pos, prevZ)) {
        grounded_ = true;
        vy_ = 0;
        for (int a = 0; a < 3; ++a) safe_[a] = pos[a];
        hasSafe_ = true;
    }

    // Fall safety.
    if (!grounded_) {
        const float* ref = hasSafe_ ? safe_ : fallback_;
        if (pos[2] > ref[2] + p_.maxFall) {
            for (int a = 0; a < 3; ++a) pos[a] = ref[a];
            vy_ = 0;
            if (ground_check(pos, pos[2])) {
                grounded_ = true;
                res.recovered = true;
            } else {
                res.gaveUp = true;
            }
        }
    }
    res.grounded = grounded_;
    return res;
}

} // namespace castlemist::world
