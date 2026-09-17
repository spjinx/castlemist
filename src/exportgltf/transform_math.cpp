/// @file
/// @brief Row-vector 4x4 math (matching ModelJoint::invWorld and sceneWorld()),
///        and the flatten used to hand a matrix to glTF's column-vector `matrix`
///        properties. No quaternion/Euler conversion lives here: glTF rotation
///        is already a plain quaternion, so GW2's own quaternions are copied
///        through untouched (see internal.h's design note).

#include "internal.h"

#include <algorithm>
#include <cmath>

namespace castlemist::exportgltf {

Mat4 mat4_mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            float acc = 0.0f;
            for (int k = 0; k < 4; ++k) acc += a.m[row * 4 + k] * b.m[k * 4 + col];
            r.m[row * 4 + col] = acc;
        }
    }
    return r;
}

Mat4 mat4_rot_x(float a) {
    Mat4 r;
    float c = std::cos(a), s = std::sin(a);
    r.m[5] = c;  r.m[6] = s;
    r.m[9] = -s; r.m[10] = c;
    return r;
}

Mat4 mat4_rot_y(float a) {
    Mat4 r;
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c;  r.m[2] = -s;
    r.m[8] = s;  r.m[10] = c;
    return r;
}

Mat4 mat4_rot_z(float a) {
    Mat4 r;
    float c = std::cos(a), s = std::sin(a);
    r.m[0] = c; r.m[1] = s;
    r.m[4] = -s; r.m[5] = c;
    return r;
}

Mat4 mat4_scale(float s) {
    Mat4 r;
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

Mat4 mat4_translate(const Vec3& t) {
    Mat4 r;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z;
    return r;
}

// Ported 1:1 from src/render/detail/math.h's sceneWorld(): row-vector
// convention, ROTATION IS rotZ(-rot.z) * rotX(-rot.x) * rotY(-rot.y) --
// verified there against the game client and Tyria3D, not guessed.
Mat4 scene_world(const Vec3& pos, const Vec3& rot, float scale) {
    Mat4 R = mat4_mul(mat4_mul(mat4_rot_z(-rot.z), mat4_rot_x(-rot.x)), mat4_rot_y(-rot.y));
    return mat4_mul(mat4_mul(mat4_scale(scale), R), mat4_translate(pos));
}

// glTF matrices are column-major storage of a column-vector matrix M
// (v' = M*v): array[col*4+row] = M[row][col]. Since M is defined as
// transpose(m) (the general "row-vector p*W == column-vector transpose(W)*p"
// identity), M[row][col] = m.m[col*4+row] -- and substituting k = col*4+row
// on both sides gives array[k] = m.m[k]. The mathematical transpose and the
// row-major -> column-major re-indexing cancel out exactly, so this is a
// direct copy, NOT the row-major-flattening formula FBX's TransformLink
// needed (that convention stores the column-vector matrix row-major instead,
// which is a genuinely different target layout).
std::array<float, 16> flatten_column_major(const Mat4& m) {
    std::array<float, 16> out{};
    std::copy(std::begin(m.m), std::end(m.m), out.begin());
    return out;
}

} // namespace castlemist::exportgltf
