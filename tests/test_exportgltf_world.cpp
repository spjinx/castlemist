/// @file
/// @brief Tests for the world export (sub-project 4,
///        docs/superpowers/specs/2026-10-15-world-export-design.md): the frame
///        probe first (spec D6 "Frame probe").

#include "test_framework.h"

#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/world/frame.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/native/granny_anim.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

using namespace castlemist::exportgltf;

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

fs::path fresh_dir(const char* label) {
    fs::path dir = fs::temp_directory_path() / (std::string("cm_world_export_") + label);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

/// The JSON chunk of a .glb (copied from test_exportgltf_vrchat.cpp).
json glb_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t i) {
        return static_cast<uint32_t>(b[i]) | (static_cast<uint32_t>(b[i + 1]) << 8) |
               (static_cast<uint32_t>(b[i + 2]) << 16) | (static_cast<uint32_t>(b[i + 3]) << 24);
    };
    if (b.size() < 20) return json();
    const uint32_t len = u32(12);
    return json::parse(std::string(reinterpret_cast<const char*>(&b[20]), len));
}

/// The POSITION accessor of the first primitive of the mesh on the node named @p name.
const json* position_accessor(const json& g, const std::string& name) {
    for (const json& n : g["nodes"]) {
        if (n.value("name", std::string()) != name || !n.contains("mesh")) continue;
        const json& prim = g["meshes"][n["mesh"].get<size_t>()]["primitives"][0];
        return &g["accessors"][prim["attributes"]["POSITION"].get<size_t>()];
    }
    return nullptr;
}

} // namespace

CM_TEST(world_probe, bars_in_map_space) {
    const fs::path dir = fresh_dir("probe");
    const fs::path glb = dir / "probe.glb";
    std::string err;
    CHECK(write_frame_probe(glb.string(), &err));
    CHECK(err.empty());
    const json g = glb_json(glb);
    CHECK(g.is_object());

    // One scene root, GW2_MapToGltf, carrying map_to_gltf_root(1) exactly.
    const json& roots = g["scenes"][0]["nodes"];
    CHECK_EQ(roots.size(), size_t(1));
    const json& root = g["nodes"][roots[0].get<size_t>()];
    CHECK(root["name"] == "GW2_MapToGltf");
    const std::array<float, 16> want = map_to_gltf_root(1.0);
    CHECK_EQ(root["matrix"].size(), size_t(16));
    for (size_t i = 0; i < 16; ++i) CHECK_EQ(root["matrix"][i].get<float>(), want[i]);

    const json* east = position_accessor(g, "Probe_East");
    const json* north = position_accessor(g, "Probe_North");
    const json* up = position_accessor(g, "Probe_Up");
    CHECK(east && north && up);
    CHECK_EQ((*east)["min"][0].get<float>(), 0.0f);
    CHECK_EQ((*east)["max"][0].get<float>(), 300.0f);
    CHECK_EQ((*east)["min"][1].get<float>(), -10.0f);
    CHECK_EQ((*east)["max"][1].get<float>(), 10.0f);
    CHECK_EQ((*north)["max"][1].get<float>(), 200.0f);
    CHECK_EQ((*up)["min"][2].get<float>(), -100.0f);
    CHECK_EQ((*up)["max"][2].get<float>(), 0.0f);

    // One unlit material per bar.
    CHECK_EQ(g["materials"].size(), size_t(3));
    for (const json& m : g["materials"]) CHECK(m["extensions"].contains("KHR_materials_unlit"));
}

CM_TEST(world_probe, expect_matches_frame) {
    const json e = frame_probe_expect();
    const float dirs[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};  // east, north, up in map space
    const char* names[3] = {"east", "north", "up"};
    for (int a = 0; a < 3; ++a) {
        float u[3], b[3];
        castlemist::world::map_to_unity(dirs[a], u);
        castlemist::world::map_to_blender(dirs[a], b);
        for (int i = 0; i < 3; ++i) {
            CHECK_EQ(e["unity"][names[a]][i].get<float>(), u[i]);
            CHECK_EQ(e["blender"][names[a]][i].get<float>(), b[i]);
        }
    }
    CHECK(e["lengths"] == json::array({3, 2, 1}));
}

// ------------------------------------------------------- world model files --
// Task 5: GltfExportOptions::rootMatrix / imageDirUtf8 and VrchatFolderOptions.

namespace {

std::vector<uint8_t> file_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

GVertex wvtx(float x, float y, float u, float v) {
    GVertex g{};
    g.px = x; g.py = y; g.pz = 0;
    g.nz = 1; g.tx = 1; g.by = 1;
    g.u = u; g.v = v;
    return g;
}

ModelTextureCPU wtex(int w, int h, const std::vector<std::array<uint8_t, 4>>& texels, uint32_t fileId) {
    ModelTextureCPU t;
    t.fileId = fileId;
    t.width = w;
    t.height = h;
    for (const auto& p : texels) t.rgba.insert(t.rgba.end(), p.begin(), p.end());
    return t;
}

int wadd_tex(ModelPreview& m, ModelTextureCPU t) {
    m.textures.push_back(std::move(t));
    return static_cast<int>(m.textures.size()) - 1;
}

/// glow_quad() of test_exportgltf_vrchat.cpp: one weapon-glow material with a
/// banded diffuse, a normal and a glow layer, plus one clip.
ModelPreview glow_quad() {
    ModelPreview model;
    ModelMeshCPU m;
    m.vertices = {wvtx(0, 0, 0, 0), wvtx(1, 0, 1, 0), wvtx(1, 1, 1, 1), wvtx(0, 1, 0, 1)};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.vertexCount = 4;
    m.hasTangents = true;
    m.materialIndex = 0;
    model.meshes.push_back(m);
    ModelMaterialCPU mat;
    mat.materialFile = 561567;
    mat.materialName = "Blade";
    mat.hasRenderState = true;
    mat.renderState = 0;
    mat.diffuseTex = wadd_tex(model, wtex(2, 2,
                                          {{100, 100, 100, 30}, {100, 100, 100, 90}, {100, 100, 100, 128},
                                           {100, 100, 100, 255}},
                                          1000));
    mat.normalTex = wadd_tex(model, wtex(2, 2, std::vector<std::array<uint8_t, 4>>(4, {128, 128, 255, 255}), 3000));
    ModelMaterialCPU::ExtraTexture x;
    x.texIndex = wadd_tex(model, wtex(2, 2, std::vector<std::array<uint8_t, 4>>(4, {255, 64, 0, 255}), 2001));
    x.uvIndex = 0;
    x.fileId = 2001;
    x.role = "glow";
    mat.extraTextures.push_back(x);
    mat.namedConstants = {{"specstr", 0.5f}, {"glofade", 0.7f}, {"gloptrb", 0.4f}};
    model.materials.push_back(mat);
    castlemist::granny::Anim clip;
    clip.name = "StowedA";
    model.animClips.push_back(clip);
    return model;
}

const json& scene_root(const json& g) { return g["nodes"][g["scenes"][0]["nodes"][0].get<size_t>()]; }

} // namespace

CM_TEST(world_gltf, default_vrchat_folder_unchanged) {
    const ModelPreview model = glow_quad();
    const fs::path a = fresh_dir("default_4arg");
    const fs::path b = fresh_dir("default_5arg");
    const VrchatFolderResult ra = write_vrchat_folder(model, a.string(), "Dagger", 1766522);
    const VrchatFolderResult rb = write_vrchat_folder(model, b.string(), "Dagger", 1766522, VrchatFolderOptions{});
    CHECK(ra.ok);
    CHECK(rb.ok);
    const std::vector<uint8_t> glbA = file_bytes(a / "Dagger.glb");
    CHECK(!glbA.empty());
    CHECK(glbA == file_bytes(b / "Dagger.glb"));
    CHECK(file_bytes(a / "materials.json") == file_bytes(b / "materials.json"));
    CHECK_FALSE(fs::exists(b / "Images"));

    const json g = glb_json(a / "Dagger.glb");
    const json& root = scene_root(g);
    CHECK(root["name"] == "GW2_ZupToYup");
    CHECK(root.contains("rotation"));
    CHECK_FALSE(root.contains("matrix"));
    for (const json& im : g["images"]) {
        CHECK(im.contains("bufferView"));
        CHECK_FALSE(im.contains("uri"));
    }
}

CM_TEST(world_gltf, root_matrix_replaces_rotation) {
    const ModelPreview model = glow_quad();
    const fs::path dir = fresh_dir("root_matrix");
    VrchatFolderOptions opts;
    opts.gltf.rootMatrix = map_to_gltf_root(1.0);
    const VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1766522, opts);
    CHECK(r.ok);
    const json g = glb_json(dir / "Dagger.glb");
    const json& root = scene_root(g);
    CHECK(root["name"] == "GW2_MapToGltf");
    CHECK_FALSE(root.contains("rotation"));
    CHECK_FALSE(root.contains("scale"));
    const std::array<float, 16> want = map_to_gltf_root(1.0);
    CHECK_EQ(root["matrix"].size(), size_t(16));
    for (size_t i = 0; i < 16; ++i) CHECK_EQ(root["matrix"][i].get<float>(), want[i]);
    CHECK(!root["children"].empty());
}

CM_TEST(world_gltf, images_by_uri) {
    const ModelPreview model = glow_quad();
    const fs::path dir = fresh_dir("images_uri");
    VrchatFolderOptions opts;
    opts.gltf.rootMatrix = map_to_gltf_root(1.0);
    opts.gltf.imageDirUtf8 = (dir / "Images").string();
    opts.gltf.imageUriPrefix = "Images/";
    const VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1766522, opts);
    CHECK(r.ok);
    const json g = glb_json(dir / "Dagger.glb");
    CHECK(g["images"].size() >= size_t(3));
    for (const json& im : g["images"]) {
        CHECK_FALSE(im.contains("bufferView"));
        CHECK(im.contains("uri"));
        const std::string uri = im.value("uri", std::string());
        CHECK(uri.rfind("Images/", 0) == 0);
        CHECK(uri.size() > 4 && uri.substr(uri.size() - 4) == ".png");
        CHECK(fs::exists(dir / fs::path(uri)));
    }
    // The dat textures are named by their fileId.
    for (const char* id : {"1000", "3000", "2001"}) {
        bool found = false;
        for (const json& im : g["images"])
            if (im.value("uri", std::string()) == std::string("Images/") + id + ".png") found = true;
        CHECK(found);
        CHECK(fs::exists(dir / "Images" / (std::string(id) + ".png")));
    }
    // The Poiyomi maps are still written beside them.
    CHECK(fs::exists(dir / "Textures" / "Blade - BaseColor.png"));
    CHECK(fs::exists(dir / "materials.json"));
}

CM_TEST(world_gltf, image_uris_need_no_escaping) {
    // Generated images are named after the material: the metal-rough map here
    // would be "Blade #1 %metal ü_metalRough" without the uri sanitising.
    ModelPreview model = glow_quad();
    model.materials[0].materialName = "Blade #1 %metal \xC3\xBC";
    const fs::path dir = fresh_dir("images_uri_escape");
    VrchatFolderOptions opts;
    opts.gltf.rootMatrix = map_to_gltf_root(1.0);
    opts.gltf.imageDirUtf8 = (dir / "Images").string();
    opts.gltf.imageUriPrefix = "Images/";
    const VrchatFolderResult r = write_vrchat_folder(model, dir.string(), "Dagger", 1766522, opts);
    CHECK(r.ok);
    const json g = glb_json(dir / "Dagger.glb");
    bool sawGenerated = false;
    for (const json& im : g["images"]) {
        const std::string uri = im.value("uri", std::string());
        CHECK(!uri.empty());
        bool clean = true;
        for (char c : uri) {
            const auto u = static_cast<unsigned char>(c);
            if (!(u < 0x80 && std::isalnum(u)) && c != '.' && c != '_' && c != '/' && c != '-') clean = false;
        }
        CHECK(clean);
        CHECK(fs::exists(dir / fs::path(uri)));
        if (uri.find("metalRough") != std::string::npos) sawGenerated = true;
    }
    CHECK(sawGenerated);
}
