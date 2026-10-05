/// @file
/// @brief Tests for character looks (ripper/look.h): palette table and names.

#include "test_framework.h"

#include "castlemist/ripper/look.h"

#include <string>

using namespace castlemist::ripper;

CM_TEST(look, names_creator_and_dye_colours) {
    CHECK_EQ(color_name(1097), std::string("Banana"));          // sylvari skin
    CHECK_EQ(color_name(80), std::string("Midnight Green"));    // sylvari hair (a dye too)
    CHECK_EQ(color_name(1513), std::string("Aquamarine"));      // makeover-kit eye colour
    CHECK_EQ(color_name(975), std::string("Dark Pine"));        // sylvari pattern
    CHECK_EQ(color_name(1), std::string("Dye Remover"));        // API dye
    CHECK(color_name(999999).empty());
}

CM_TEST(look, race_palettes_cover_every_playable_race) {
    RacePalettes s = race_palettes("Sylvari", "Female");
    CHECK_EQ(s.skin, 70u);
    CHECK_EQ(s.hair, 50u);
    CHECK_EQ(s.eye, 25u);
    CHECK_EQ(s.pattern, 75u);
    CHECK_EQ(s.glow, 52u);
    CHECK_EQ(race_palettes("Norn", "Male").pattern, 66u);   // tattoos
    CHECK_EQ(race_palettes("Charr", "Female").hair, 57u);
    CHECK_EQ(race_palettes("Human", "Male").eye, 6u);
    CHECK_EQ(race_palettes("Asura", "Female").eye, 15u);
    CHECK_EQ(race_palettes("Kodan", "Male").skin, 0u);
}

CM_TEST(look, eye_swatches_use_the_iris_red_when_the_palette_has_no_base) {
    castlemist::cmap::Palette eyes;  // base 0,0,0
    castlemist::cmap::PaletteColor plain{1, {castlemist::cmap::ColorShift{}}};
    CHECK(swatch_rgb(eyes, plain) == (std::array<uint8_t, 3>{192, 0, 0}));
}

#include "castlemist/ripper/thumbnail.h"

CM_TEST(look, thumbnail_draws_a_textured_front_facing_triangle) {
    ModelPreview m;
    ModelMeshCPU mesh;
    auto vert = [](float x, float z) {
        GVertex v{};
        v.px = x; v.py = 0; v.pz = z;  // in the x/-z plane, facing the camera (-y)
        v.ny = -1;
        v.u = 0.5f; v.v = 0.5f;
        return v;
    };
    mesh.vertices = {vert(-1, 1), vert(1, 1), vert(0, -1)};
    mesh.indices = {0, 1, 2};
    m.meshes.push_back(mesh);
    ModelMaterialCPU mat;
    mat.diffuseTex = 0;
    m.materials.push_back(mat);
    ModelTextureCPU tex;
    tex.width = tex.height = 1;
    tex.rgba = {200, 20, 20, 255};
    m.textures.push_back(tex);
    ImageRgba img = render_thumbnail(m, 32);
    const uint8_t* c = img.px.data() + (16 * 32 + 16) * 4;  // the middle: inside the triangle
    CHECK(c[0] > 100 && c[1] < 60);                          // textured red, lit
    const uint8_t* corner = img.px.data();
    CHECK_EQ(int(corner[0]), 118);                           // background
}

CM_TEST(look, views_stay_upright_and_unmirrored) {
    const ThumbnailView f = front_view();
    CHECK_NEAR(f.right[0], 1.0, 1e-6);   // the default basis
    CHECK_NEAR(f.up[2], -1.0, 1e-6);
    const ThumbnailView s = side_view();
    CHECK_NEAR(s.forward[0], -1.0, 1e-6);
    CHECK_NEAR(s.up[2], -1.0, 1e-6);
    const ThumbnailView t = three_quarter_top_view();
    CHECK(t.forward[2] > 0.5);            // looking down (+Z is down)
    CHECK(t.up[2] < -0.5);                // still upright
    const float dot = t.up[0] * t.forward[0] + t.up[1] * t.forward[1] + t.up[2] * t.forward[2];
    CHECK_NEAR(dot, 0.0, 1e-5);
}

#include "castlemist/ripper/vrchat.h"

CM_TEST(vrchat, humanoid_names_follow_unity) {
    CHECK_EQ(humanoid_name("bone:COG"), std::string("Hips"));
    CHECK_EQ(humanoid_name("bone:Spine03"), std::string("UpperChest"));
    CHECK_EQ(humanoid_name("bone:ShoulderL"), std::string("LeftUpperArm"));
    CHECK_EQ(humanoid_name("bone:KneeR"), std::string("RightLowerLeg"));
    CHECK_EQ(humanoid_name("bone:PinkyL02"), std::string("LeftLittleIntermediate"));
    CHECK_EQ(humanoid_name("bone:EyeR"), std::string("RightEye"));
    CHECK(humanoid_name("bone:Hair01").empty());
}

CM_TEST(vrchat, prepares_merges_prunes_and_renames) {
    ModelPreview m;
    auto joint = [](const char* n, int parent) { ModelJoint j; j.name = n; j.parent = parent; return j; };
    m.joints = {joint("bone:root", -1), joint("bone:COG", 0), joint("actionpoint:BodyCam", 0), joint("bone:Hair01", 1),
                joint("bone:Hair02", 3)};
    auto mesh = [](uint32_t bone) {
        ModelMeshCPU me;
        GVertex v{};
        v.bidx[0] = bone;
        v.bwt[0] = 1;
        me.vertices = {v, v, v};
        me.indices = {0, 1, 2};
        me.hasSkin = true;
        return me;
    };
    m.meshes = {mesh(1), mesh(4)};  // same material: merged
    m.materials.resize(1);
    m.materials[0].materialName = "import2:AmatShader1";
    const std::vector<std::string> chains = make_vrchat_ready(m);
    CHECK_EQ(m.meshes.size(), size_t{1});
    CHECK_EQ(m.meshes[0].vertices.size(), size_t{6});
    CHECK_EQ(m.joints.size(), size_t{4});               // the unweighted camera point is gone
    CHECK_EQ(m.joints[1].name, std::string("Hips"));
    CHECK_EQ(m.joints[2].name, std::string("Hair01"));  // kept: an ancestor of a weighted bone
    CHECK_EQ(m.materials[0].materialName, std::string("AmatShader1"));
    CHECK_EQ(chains.size(), size_t{1});
    CHECK_EQ(chains[0], std::string("Hair01"));
    CHECK_EQ(m.meshes[0].vertices[3].bidx[0], 3u);       // Hair02, remapped
}
