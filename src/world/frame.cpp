/// @file
/// @brief Map space -> Unity / Blender conversions, exactly as
///        docs/research/gw2-world-frame.md §1.4 states them.
///
/// Map space: left-handed, +X east, +Y north, +Z down. The Unity map is the
/// same formula as exportgltf's sky gw2_to_unity (the sky's frame is the map's,
/// §1.2), written here because world does not depend on exportgltf.

#include "castlemist/world/frame.h"

namespace castlemist::world {

namespace {

/// A signed axis permutation: output axis i = sign[i] * input axis src[i].
struct AxisMap {
    int src[3];
    float sign[3];
};

/// (x, y, z) -> (x, -z, y): east -> +X, up (-Z) -> +Y, north -> +Z. det +1.
constexpr AxisMap kUnity{{0, 2, 1}, {1.0f, -1.0f, 1.0f}};
/// (x, y, z) -> (x, y, -z): east -> +X, north -> +Y, up (-Z) -> +Z. det -1.
constexpr AxisMap kBlender{{0, 1, 2}, {1.0f, 1.0f, -1.0f}};

void convert_vec(const AxisMap& c, const float in[3], float out[3]) {
    const float v[3] = {in[0], in[1], in[2]};
    for (int i = 0; i < 3; ++i) out[i] = c.sign[i] * v[c.src[i]];
}

/// C M C^-1 for a column-major 4x4 M, C the signed permutation padded to 4x4.
/// (C M C^-1)[i][j] = sign[i] * sign[j] * M[src[i]][src[j]] on the 3x3 part;
/// the translation converts like a position; the bottom row is kept.
void convert_mat(const AxisMap& c, const float in[16], float out[16]) {
    float m[16];
    for (int k = 0; k < 16; ++k) m[k] = in[k];
    // Column-major: element (row r, col k) is m[k * 4 + r].
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            out[col * 4 + row] = c.sign[row] * c.sign[col] * m[c.src[col] * 4 + c.src[row]];
    for (int row = 0; row < 3; ++row) {
        out[12 + row] = c.sign[row] * m[12 + c.src[row]];       // translation
        out[row * 4 + 3] = m[c.src[row] * 4 + 3] * c.sign[row]; // bottom row (0 for affine)
    }
    out[15] = m[15];
}

} // namespace

void map_to_unity(const float in[3], float out[3]) { convert_vec(kUnity, in, out); }

void map_to_blender(const float in[3], float out[3]) { convert_vec(kBlender, in, out); }

void map_matrix_to_unity(const float in[16], float out[16]) { convert_mat(kUnity, in, out); }

void map_matrix_to_blender(const float in[16], float out[16]) { convert_mat(kBlender, in, out); }

} // namespace castlemist::world
