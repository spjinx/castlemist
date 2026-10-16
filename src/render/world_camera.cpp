/// @file
/// @brief WorldCamera maths (see world_camera.h).

#include "world_camera.h"

#include <algorithm>
#include <cmath>

namespace castlemist::render {

float world_camera_near(float farDist) {
    return std::clamp(farDist * 5e-5f, 0.05f, 20.0f);
}

void world_camera_basis(const WorldCamera& c, Vec3& fwd, Vec3& right, Vec3& up) {
    fwd = {c.fwd[0], c.fwd[1], c.fwd[2]};
    if (!(dot(fwd, fwd) > 1e-12f)) fwd = {1, 0, 0};
    fwd = norm(fwd);
    Vec3 u{c.up[0], c.up[1], c.up[2]};
    right = cross(u, fwd);   // the lookAt convention: x = cross(up, z)
    if (!(dot(right, right) > 1e-10f)) {
        // up parallel to fwd (or zero): use world up, then any axis not along fwd.
        right = cross(kWorldUp, fwd);
        if (!(dot(right, right) > 1e-10f)) right = cross(Vec3{0, 1, 0}, fwd);
    }
    right = norm(right);
    up = cross(fwd, right);
}

Mat4 world_camera_view(const WorldCamera& c) {
    Vec3 f, r, u;
    world_camera_basis(c, f, r, u);
    const Vec3 eye{c.pos[0], c.pos[1], c.pos[2]};
    return lookAt(eye, Vec3{eye.x + f.x, eye.y + f.y, eye.z + f.z}, u);
}

Mat4 world_camera_viewproj(const WorldCamera& c, float aspect) {
    const Mat4 view = world_camera_view(c);
    const float farD = std::max(c.farDist, 1.0f);
    const float a = (aspect > 0 && std::isfinite(aspect)) ? aspect : 1.0f;
    return mul(view, perspective(c.fovY, a, world_camera_near(farD), farD));
}

void world_camera_ray(const WorldCamera& c, int w, int h, int sx, int sy, float origin[3], float dir[3]) {
    Vec3 f, r, u;
    world_camera_basis(c, f, r, u);
    w = std::max(w, 1);
    h = std::max(h, 1);
    // Pixel centre -> NDC (y up), then scale by the frustum half-extents at distance 1.
    const float nx = 2.0f * (static_cast<float>(sx) + 0.5f) / static_cast<float>(w) - 1.0f;
    const float ny = 1.0f - 2.0f * (static_cast<float>(sy) + 0.5f) / static_cast<float>(h);
    const float ty = std::tan(c.fovY * 0.5f);
    const float tx = ty * static_cast<float>(w) / static_cast<float>(h);
    const Vec3 d = norm(Vec3{f.x + r.x * nx * tx + u.x * ny * ty, f.y + r.y * nx * tx + u.y * ny * ty,
                             f.z + r.z * nx * tx + u.z * ny * ty});
    origin[0] = c.pos[0]; origin[1] = c.pos[1]; origin[2] = c.pos[2];
    dir[0] = d.x; dir[1] = d.y; dir[2] = d.z;
}

bool invert4(const Mat4& M, Mat4& out) {
    // Cofactor expansion (the MESA gluInvertMatrix layout), in double.
    double m[16], inv[16];
    for (int k = 0; k < 16; ++k) {
        if (!std::isfinite(M.m[k])) return false;
        m[k] = M.m[k];
    }
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::fabs(det) > 1e-300) || !std::isfinite(det)) return false;
    for (int k = 0; k < 16; ++k) out.m[k] = static_cast<float>(inv[k] / det);
    return true;
}

Mat4 world_camera_sky_inv_viewproj(const WorldCamera& c, float aspect) {
    WorldCamera at0 = c;
    at0.pos[0] = at0.pos[1] = at0.pos[2] = 0;
    Mat4 inv = identity();
    if (!invert4(world_camera_viewproj(at0, aspect), inv)) return identity();
    return inv;
}

} // namespace castlemist::render
