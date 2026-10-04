/// @file
/// @brief Tests for merging models onto one skeleton and attaching weapons
///        (ripper/skeleton_merge.h), on small hand-built ModelPreviews.

#include "test_framework.h"

#include "castlemist/ripper/skeleton_merge.h"

#include <string>

using namespace castlemist::ripper;

namespace {

// A joint at world translation (x,y,z): row-vector invWorld = translate(-x,-y,-z).
ModelJoint joint(const char* name, int parent, float x, float y, float z) {
    ModelJoint j;
    j.name = name;
    j.parent = parent;
    j.pos[0] = x; j.pos[1] = y; j.pos[2] = z;
    j.invWorld[12] = -x; j.invWorld[13] = -y; j.invWorld[14] = -z;
    return j;
}

GVertex vertex(float x, float y, float z, uint32_t bone) {
    GVertex v{};
    v.px = x; v.py = y; v.pz = z;
    v.nx = 0; v.ny = 0; v.nz = 1;
    v.bidx[0] = bone;
    v.bwt[0] = 1;
    return v;
}

ModelPreview model_with(std::vector<ModelJoint> joints, std::vector<GVertex> verts) {
    ModelPreview m;
    m.joints = std::move(joints);
    ModelMeshCPU mesh;
    mesh.vertices = std::move(verts);
    mesh.indices = {0, 0, 0};
    mesh.hasSkin = true;
    mesh.materialIndex = 0;
    m.meshes.push_back(mesh);
    ModelMaterialCPU mat;
    mat.diffuseTex = 0;
    m.materials.push_back(mat);
    ModelTextureCPU tex;
    tex.width = tex.height = 1;
    tex.rgba = {1, 2, 3, 4};
    m.textures.push_back(tex);
    return m;
}

} // namespace

CM_TEST(skeleton, merge_remaps_bones_by_name) {
    ModelPreview body = model_with({joint("bone:root", -1, 0, 0, 0), joint("bone:Spine01", 0, 0, 0, 5)},
                                   {vertex(0, 0, 0, 1)});
    // The coat lists the same bones in another order.
    ModelPreview coat = model_with({joint("bone:Spine01", 1, 0, 0, 5), joint("bone:root", -1, 0, 0, 0)},
                                   {vertex(1, 1, 1, 0)});  // bound to its bone 0 = Spine01
    size_t first = merge_into(body, coat);
    CHECK_EQ(first, size_t{1});
    CHECK_EQ(body.meshes.size(), size_t{2});
    CHECK_EQ(body.joints.size(), size_t{2});
    CHECK_EQ(body.meshes[1].vertices[0].bidx[0], 1u);   // Spine01 in the body's numbering
    CHECK_EQ(body.meshes[1].materialIndex, 1u);         // materials appended
    CHECK_EQ(body.materials[1].diffuseTex, 1);          // textures appended
    CHECK_EQ(body.textures.size(), size_t{2});
}

CM_TEST(skeleton, merge_appends_missing_joints_under_their_named_parent) {
    ModelPreview body = model_with({joint("bone:root", -1, 0, 0, 0), joint("bone:Spine03", 0, 0, 0, 9)}, {});
    ModelPreview cape = model_with({joint("bone:root", -1, 0, 0, 0), joint("bone:Spine03", 0, 0, 0, 9),
                                    joint("bone:Cape01", 1, 0, -1, 9)},
                                   {vertex(0, -1, 9, 2)});
    merge_into(body, cape);
    CHECK_EQ(body.joints.size(), size_t{3});
    CHECK_EQ(body.joints[2].name, std::string("bone:Cape01"));
    CHECK_EQ(body.joints[2].parent, 1);  // Spine03
    CHECK_EQ(body.meshes.back().vertices[0].bidx[0], 2u);
}

CM_TEST(skeleton, merge_into_an_empty_model_takes_the_skeleton) {
    ModelPreview empty;
    ModelPreview body = model_with({joint("bone:root", -1, 0, 0, 0)}, {vertex(0, 0, 0, 0)});
    merge_into(empty, body);
    CHECK_EQ(empty.joints.size(), size_t{1});
    CHECK_EQ(empty.meshes.size(), size_t{1});
}

CM_TEST(skeleton, attach_rigid_moves_the_weapon_point_onto_the_body_point) {
    ModelPreview body = model_with({joint("bone:root", -1, 0, 0, 0), joint("actionpoint:RHolsterBack", 0, 10, 0, 0)}, {});
    ModelPreview sword = model_with({joint("ArenaExport", -1, 0, 0, 0), joint("actionpoint:RStowBack", 0, 0, 2, 0)},
                                    {vertex(0, 2, 0, 0), vertex(1, 2, 0, 0)});
    CHECK(attach_rigid(body, sword, "actionpoint:RStowBack", "actionpoint:RHolsterBack"));
    const ModelMeshCPU& m = body.meshes.back();
    CHECK_NEAR(m.vertices[0].px, 10.0, 1e-4);  // the grip point lands on the holster point
    CHECK_NEAR(m.vertices[0].py, 0.0, 1e-4);
    CHECK_NEAR(m.vertices[1].px, 11.0, 1e-4);
    CHECK_EQ(m.vertices[0].bidx[0], 1u);       // rigidly skinned to the holster joint
    CHECK_NEAR(m.vertices[0].bwt[0], 1.0, 1e-6);
    CHECK_EQ(body.joints.size(), size_t{2});   // the weapon's own joints are not added
}

CM_TEST(skeleton, attach_rigid_fails_without_the_joints) {
    ModelPreview body = model_with({joint("bone:root", -1, 0, 0, 0)}, {});
    ModelPreview sword = model_with({joint("ArenaExport", -1, 0, 0, 0)}, {vertex(0, 0, 0, 0)});
    const size_t before = body.meshes.size();
    CHECK_FALSE(attach_rigid(body, sword, "actionpoint:RStowBack", "actionpoint:RHolsterBack"));
    CHECK_EQ(body.meshes.size(), before);
}
