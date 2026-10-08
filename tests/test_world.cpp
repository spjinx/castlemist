/// @file
/// @brief Tests for the world layer (WorldScene types).

#include "test_framework.h"

#include "castlemist/world/world_scene.h"

CM_TEST(world, default_scene_is_empty) {
    castlemist::world::WorldScene scene;
    CHECK_FALSE(scene.terrain.present);
    CHECK(scene.props.empty());
    CHECK(scene.models.empty());
    CHECK(scene.collision.instances.empty());
    CHECK(scene.warnings.empty());
    CHECK_FALSE(scene.hasBounds);
}

// ---- frame (docs/research/gw2-world-frame.md §1) ----

#include "castlemist/world/frame.h"

#include <cmath>

namespace {

using ConvVec = void (*)(const float*, float*);
using ConvMat = void (*)(const float*, float*);

/// Determinant of the 3x3 whose columns are conv(east), conv(north), conv(down).
float conv_det(ConvVec conv) {
    const float e[3] = {1, 0, 0}, n[3] = {0, 1, 0}, d[3] = {0, 0, 1};
    float a[3], b[3], c[3];
    conv(e, a); conv(n, b); conv(d, c);
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - b[0] * (a[1] * c[2] - a[2] * c[1]) +
           c[0] * (a[1] * b[2] - a[2] * b[1]);
}

/// Determinant of a column-major 4x4's upper-left 3x3.
float linear_det(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
           m[8] * (m[1] * m[6] - m[2] * m[5]);
}

/// p' = M p for a column-major 4x4 and a point (w = 1).
void apply(const float m[16], const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

void check_vec(ConvVec conv, float x, float y, float z, float ex, float ey, float ez) {
    const float in[3] = {x, y, z};
    float out[3] = {9, 9, 9};
    conv(in, out);
    CHECK_EQ(out[0], ex);
    CHECK_EQ(out[1], ey);
    CHECK_EQ(out[2], ez);
}

/// conv_mat(M) * conv(p) == conv(M * p), on a rotated, scaled, translated M.
void check_matrix_commutes(ConvVec conv, ConvMat conv_mat) {
    const float a = 0.7f, b = -0.4f, s = 1.5f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    // Rz(a) * Rx(b) * s, column-major, translation (100, -200, 300).
    const float m[16] = {
        s * ca,      s * sa,      0,      0,
        -s * sa * cb, s * ca * cb, s * sb, 0,
        s * sa * sb, -s * ca * sb, s * cb, 0,
        100,         -200,        300,    1};
    const float p[3] = {3, -5, 7};
    float mp[3], want[3], cp[3], got[3], cm[16];
    apply(m, p, mp);
    conv(mp, want);
    conv(p, cp);
    conv_mat(m, cm);
    apply(cm, cp, got);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(got[i], want[i], 1e-3);
    CHECK_NEAR(cm[3], 0, 0); CHECK_NEAR(cm[7], 0, 0); CHECK_NEAR(cm[11], 0, 0); CHECK_EQ(cm[15], 1.0f);
    CHECK_NEAR(linear_det(cm), linear_det(m), 1e-4);
}

void check_identity_is_proper_rotation(ConvMat conv_mat) {
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float out[16];
    conv_mat(id, out);
    for (int i = 0; i < 16; ++i) CHECK_EQ(out[i], id[i]);
    CHECK_EQ(linear_det(out), 1.0f);
}

} // namespace

// §1.4: map east +X, north +Y, up -Z -> Unity +X, +Z, +Y; det +1 (LH -> LH).
CM_TEST(world, frame_to_unity_axes) {
    using castlemist::world::map_to_unity;
    using castlemist::world::map_matrix_to_unity;
    check_vec(map_to_unity, 1, 0, 0, 1, 0, 0);    // east  -> +X
    check_vec(map_to_unity, 0, 1, 0, 0, 0, 1);    // north -> +Z (forward)
    check_vec(map_to_unity, 0, 0, -1, 0, 1, 0);   // up    -> +Y
    check_vec(map_to_unity, 2, 3, 4, 2, -4, 3);   // a point
    CHECK_EQ(conv_det(map_to_unity), 1.0f);        // proper rotation
    float v[3] = {2, 3, 4};
    map_to_unity(v, v);                            // in == out
    CHECK_EQ(v[0], 2.0f); CHECK_EQ(v[1], -4.0f); CHECK_EQ(v[2], 3.0f);
    check_identity_is_proper_rotation(map_matrix_to_unity);
    check_matrix_commutes(map_to_unity, map_matrix_to_unity);
}

// §1.4: map east +X, north +Y, up -Z -> Blender +X, +Y, +Z; det -1 is the
// LH -> RH handedness change, carried by the vectors, not the matrices.
CM_TEST(world, frame_to_blender_axes) {
    using castlemist::world::map_to_blender;
    using castlemist::world::map_matrix_to_blender;
    check_vec(map_to_blender, 1, 0, 0, 1, 0, 0);   // east  -> +X
    check_vec(map_to_blender, 0, 1, 0, 0, 1, 0);   // north -> +Y
    check_vec(map_to_blender, 0, 0, -1, 0, 0, 1);  // up    -> +Z
    check_vec(map_to_blender, 2, 3, 4, 2, 3, -4);  // a point
    CHECK_EQ(conv_det(map_to_blender), -1.0f);      // the handedness change
    float v[3] = {2, 3, 4};
    map_to_blender(v, v);
    CHECK_EQ(v[0], 2.0f); CHECK_EQ(v[1], 3.0f); CHECK_EQ(v[2], -4.0f);
    check_identity_is_proper_rotation(map_matrix_to_blender);
    check_matrix_commutes(map_to_blender, map_matrix_to_blender);
}
