/// @file
/// @brief WorldCamera maths: view-projection and pixel rays. Pure, no D3D.
///
/// @warning Not a public header -- internal to `src/render/` (and its tests).

#pragma once

#include "castlemist/render/world_view.h"

#include "detail/math.h"

namespace castlemist::render {

/// @brief Near plane for a far distance: clamp(far * 5e-5, 0.05, 20).
float world_camera_near(float farDist);

/// @brief The camera's orthonormal basis: unit forward, right = cross(up, fwd),
///        and up re-orthogonalized. A degenerate fwd/up falls back to sane axes.
void world_camera_basis(const WorldCamera& c, Vec3& fwd, Vec3& right, Vec3& up);

/// @brief The view half of world_camera_viewproj (row-vector lookAt).
Mat4 world_camera_view(const WorldCamera& c);

/// @brief Row-vector view * proj for a camera (D3D 0..1 depth,
///        near = world_camera_near(c.farDist)).
Mat4 world_camera_viewproj(const WorldCamera& c, float aspect);

/// @brief Ray through the centre of pixel (sx, sy) of a w x h surface
///        (sy = 0 is the top row): origin = camera pos, unit dir.
void world_camera_ray(const WorldCamera& c, int w, int h, int sx, int sy, float origin[3], float dir[3]);

/// @brief General 4x4 inverse (row-vector Mat4); false, @p out untouched,
///        when @p m is singular or not finite.
bool invert4(const Mat4& m, Mat4& out);

/// @brief The inverse of world_camera_viewproj for @p c moved to the origin:
///        the sky pass's NDC -> direction map. An NDC point (x, y, 1, 1)
///        times it is a point along the view ray through that pixel, so its
///        xyz / w is the map-space direction (no camera translation, so
///        float precision does not depend on where on the map the camera is).
///        Identity when the matrix is singular.
Mat4 world_camera_sky_inv_viewproj(const WorldCamera& c, float aspect);

} // namespace castlemist::render
