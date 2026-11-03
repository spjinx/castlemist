/// @file
/// @brief Tests for ripper/weld.h: one normal where separate meshes meet.

#include "test_framework.h"

#include "castlemist/ripper/weld.h"

#include <cmath>

using namespace castlemist::ripper;

namespace {
GVertex v(float x, float y, float z, float nx, float ny, float nz) {
    GVertex g{};
    g.px = x; g.py = y; g.pz = z;
    g.nx = nx; g.ny = ny; g.nz = nz;
    return g;
}
ModelMeshCPU mesh(std::vector<GVertex> vs) {
    ModelMeshCPU m;
    m.vertices = std::move(vs);
    return m;
}
}  // namespace

CM_TEST(weld, normals_meet_across_meshes) {
    ModelPreview m;
    const float s = std::sqrt(0.5f);
    m.meshes.push_back(mesh({v(0, 0, 0, 1, 0, 0), v(5, 0, 0, 1, 0, 0)}));   // face edge
    m.meshes.push_back(mesh({v(0, 0, 0, s, s, 0)}));                       // neck edge, 45 degrees off
    CHECK_EQ(weld_normals(m, {0, 1}), size_t{2});
    const GVertex& a = m.meshes[0].vertices[0];
    const GVertex& b = m.meshes[1].vertices[0];
    CHECK_NEAR(a.nx, b.nx, 1e-5);
    CHECK_NEAR(a.ny, b.ny, 1e-5);
    CHECK_NEAR(std::sqrt(a.nx * a.nx + a.ny * a.ny + a.nz * a.nz), 1.0, 1e-5);
    CHECK_NEAR(m.meshes[0].vertices[1].nx, 1.0, 1e-6);   // a vertex alone keeps its normal
}

CM_TEST(weld, sharp_creases_and_other_meshes_are_left) {
    ModelPreview m;
    m.meshes.push_back(mesh({v(0, 0, 0, 1, 0, 0)}));
    m.meshes.push_back(mesh({v(0, 0, 0, 0, 1, 0)}));   // 90 degrees: a crease
    m.meshes.push_back(mesh({v(0, 0, 0, 1, 0, 0)}));   // not in the list
    CHECK_EQ(weld_normals(m, {0, 1}), size_t{0});
    CHECK_NEAR(m.meshes[0].vertices[0].nx, 1.0, 1e-6);
    CHECK_NEAR(m.meshes[1].vertices[0].ny, 1.0, 1e-6);
}
