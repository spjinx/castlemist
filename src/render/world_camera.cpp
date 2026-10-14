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

Mat4 world_camera_viewproj(const WorldCamera& c, float aspect) {
    Vec3 f, r, u;
    world_camera_basis(c, f, r, u);
    const Vec3 eye{c.pos[0], c.pos[1], c.pos[2]};
    const Mat4 view = lookAt(eye, Vec3{eye.x + f.x, eye.y + f.y, eye.z + f.z}, u);
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

} // namespace castlemist::render
