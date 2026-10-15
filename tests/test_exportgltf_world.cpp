/// @file
/// @brief Tests for the world export (sub-project 4,
///        docs/superpowers/specs/2026-10-15-world-export-design.md): the frame
///        probe first (spec D6 "Frame probe").

#include "test_framework.h"

#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_cell.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_manifest.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/world/frame.h"
#include "castlemist/world/props.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/native/granny_anim.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
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

// ------------------------------------------------------------- cell glTF --
// Task 6: world_cell.h, one .glb per export cell.

namespace {

namespace cw = castlemist::world;

constexpr float kCX0 = -15360.f, kCY1 = 39936.f, kCC = 3072.f;

/// A height that depends only on the global sample (gi, gj), so shared chunk
/// edges are bit-identical as on real maps (gw2-world-frame.md §3.1).
float sample_height(int gi, int gj) { return 10.0f + 0.5f * float(gi) - 0.25f * float(gj); }

/// grid() of test_world_cells.cpp (copied), with heights filled in.
cw::WorldScene cell_grid(int nx, int ny, float pickerScale = 0.25f) {
    cw::WorldScene s;
    s.terrain.present = true;
    s.terrain.chunksX = nx; s.terrain.chunksY = ny;
    for (int cy = 0; cy < ny; ++cy)
        for (int cx = 0; cx < nx; ++cx) {
            cw::TerrainChunk c;
            c.cx = cx; c.cy = cy; c.samples = 33;
            c.rect[0] = kCX0 + cx * kCC; c.rect[2] = kCX0 + (cx + 1) * kCC;
            c.rect[3] = kCY1 - cy * kCC; c.rect[1] = kCY1 - (cy + 1) * kCC;
            c.material.pickerScale = pickerScale;
            c.heights.resize(33 * 33);
            for (int j = 0; j < 33; ++j)
                for (int i = 0; i < 33; ++i) c.heights[size_t(j) * 33 + i] = sample_height(cx * 32 + i, cy * 32 + j);
            s.terrain.chunks.push_back(c);
        }
    return s;
}

cw::Cell cell_by_id(const cw::WorldScene& s, const std::string& id) {
    const cw::CellPlan p = cw::partition_cells(s, cw::CellStats{}, cw::CellBudget{});
    for (const auto& c : p.cells) if (c.id == id) return c;
    return cw::Cell{};
}

void set_all_heights(cw::WorldScene& s, float z) {
    for (auto& c : s.terrain.chunks) std::fill(c.heights.begin(), c.heights.end(), z);
}

void add_water(cw::WorldScene& s) {
    s.water.hasPlane = true; s.water.planeZ = 0;
    castlemist::model::Extractor::MapEnvWater p;
    p.depthAttenuation = 10;
    p.surfaceShallowColor[2] = 255;
    p.surfaceDeepColor[0] = 255;
    s.environment.water.push_back(p);
    s.environment.light.presetIndex = 0;
}

/// One hull of @p tris triangles (all the same one) placed once, identity transform.
void add_hull(cw::WorldScene& s, size_t tris) {
    cw::CollisionMesh m;
    m.verts = {-9000.f, 33000.f, 1.f, -8000.f, 33000.f, 2.f, -9000.f, 34000.f, 3.f};
    for (size_t k = 0; k < tris; ++k) m.indices.insert(m.indices.end(), {0u, 1u, 2u});
    s.collision.meshes.push_back(m);
    cw::CollisionInstance inst;
    inst.mesh = uint32_t(s.collision.meshes.size() - 1);
    inst.world[0] = inst.world[5] = inst.world[10] = inst.world[15] = 1;
    s.collision.instances.push_back(inst);
}

void cross3(const float a[3], const float b[3], const float c[3], float out[3]) {
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    out[0] = u[1] * v[2] - u[2] * v[1];
    out[1] = u[2] * v[0] - u[0] * v[2];
    out[2] = u[0] * v[1] - u[1] * v[0];
}

/// Column-major 4x4 times a direction.
void mul_dir(const std::array<float, 16>& m, const float v[3], float out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2];
}

/// The BIN chunk of a .glb.
std::vector<uint8_t> glb_bin(const fs::path& p) {
    const std::vector<uint8_t> b = file_bytes(p);
    auto u32 = [&](size_t i) {
        return static_cast<uint32_t>(b[i]) | (static_cast<uint32_t>(b[i + 1]) << 8) |
               (static_cast<uint32_t>(b[i + 2]) << 16) | (static_cast<uint32_t>(b[i + 3]) << 24);
    };
    if (b.size() < 20) return {};
    const size_t binAt = 20 + u32(12);
    if (b.size() < binAt + 8) return {};
    const uint32_t len = u32(binAt);
    if (b.size() < binAt + 8 + len) return {};
    return std::vector<uint8_t>(b.begin() + std::ptrdiff_t(binAt + 8), b.begin() + std::ptrdiff_t(binAt + 8 + len));
}

/// Accessor @p a as numbers: FLOAT, UNSIGNED_SHORT or UNSIGNED_INT components.
std::vector<double> read_accessor(const json& g, const std::vector<uint8_t>& bin, size_t a) {
    const json& acc = g["accessors"][a];
    const json& bv = g["bufferViews"][acc["bufferView"].get<size_t>()];
    const size_t off = bv.value("byteOffset", size_t(0)) + acc.value("byteOffset", size_t(0));
    const std::string type = acc["type"];
    const size_t comps = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : 4;
    const size_t n = acc["count"].get<size_t>() * comps;
    const int ct = acc["componentType"];
    const size_t cs = ct == 5123 ? 2 : 4;
    std::vector<double> out;
    if (off + n * cs > bin.size()) return out;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        if (ct == 5126) { float f; std::memcpy(&f, &bin[off + i * 4], 4); out[i] = f; }
        else if (ct == 5123) { uint16_t v; std::memcpy(&v, &bin[off + i * 2], 2); out[i] = v; }
        else { uint32_t v; std::memcpy(&v, &bin[off + i * 4], 4); out[i] = v; }
    }
    return out;
}

const json* node_named(const json& g, const std::string& name) {
    for (const json& n : g["nodes"]) if (n.value("name", std::string()) == name) return &n;
    return nullptr;
}

} // namespace

CM_TEST(world_cell, terrain_welded_vertex_count) {
    const cw::WorldScene s = cell_grid(4, 4);
    const cw::Cell c = cell_by_id(s, "c0_0_4x4");
    CHECK(c.id == "c0_0_4x4");
    const CellMesh m = build_cell_terrain(s.terrain, c);
    CHECK_EQ(m.vertices(), size_t(16641));  // (4*32+1)^2: fits 16-bit indices (spec D1)
    CHECK_EQ(m.idx.size(), size_t(32768 * 3));
    CHECK_EQ(m.nrm.size(), m.pos.size());
    CHECK_EQ(m.uv.size(), m.vertices() * 2);
    if (m.vertices() != 16641 || m.idx.empty()) return;
    bool small = true;
    for (uint32_t i : m.idx) if (i >= 65536) small = false;
    CHECK(small);
    CHECK_EQ(m.pos[0], -15360.f - c.origin[0]);
    CHECK_EQ(m.pos[1], 39936.f - c.origin[1]);
    CHECK_EQ(m.pos[2], sample_height(0, 0));
    // Last vertex: the south-east corner.
    const size_t last = m.vertices() - 1;
    CHECK_EQ(m.pos[last * 3 + 0], -15360.f + 4 * kCC - c.origin[0]);
    CHECK_EQ(m.pos[last * 3 + 1], 39936.f - 4 * kCC - c.origin[1]);
    CHECK_EQ(m.pos[last * 3 + 2], sample_height(128, 128));
    CHECK_EQ(m.uv[0], 0.f);
    CHECK_EQ(m.uv[1], 0.f);
    CHECK_EQ(m.uv[m.uv.size() - 2], 1.f);
    CHECK_EQ(m.uv[m.uv.size() - 1], 1.f);
    float n[3];
    cross3(&m.pos[m.idx[0] * 3], &m.pos[m.idx[1] * 3], &m.pos[m.idx[2] * 3], n);
    CHECK(n[2] < 0);   // up is -Z
    CHECK(m.nrm[2] < 0);
}

CM_TEST(world_cell, shared_edge_welded) {
    const cw::WorldScene s = cell_grid(2, 1);
    const cw::Cell c = cell_by_id(s, "c0_0_2x1");
    CHECK(c.id == "c0_0_2x1");
    const CellMesh m = build_cell_terrain(s.terrain, c);
    CHECK_EQ(m.vertices(), size_t(65 * 33));
    std::vector<std::array<float, 3>> p;
    for (size_t i = 0; i < m.vertices(); ++i) p.push_back({m.pos[i * 3], m.pos[i * 3 + 1], m.pos[i * 3 + 2]});
    std::sort(p.begin(), p.end());
    CHECK(std::adjacent_find(p.begin(), p.end()) == p.end());
    CHECK_EQ(m.idx.size(), size_t(2 * 32 * 32 * 6));
}

CM_TEST(world_cell, missing_chunk_leaves_its_quads_out) {
    cw::WorldScene s = cell_grid(2, 1);
    const cw::Cell c = cell_by_id(s, "c0_0_2x1");
    s.terrain.chunks.pop_back();   // chunk (1, 0)
    const CellMesh m = build_cell_terrain(s.terrain, c);
    CHECK_EQ(m.vertices(), size_t(33 * 33));
    CHECK_EQ(m.idx.size(), size_t(32 * 32 * 6));
    for (uint32_t i : m.idx) CHECK(i < m.vertices());
    if (m.uv.size() >= 2) CHECK_EQ(m.uv[m.uv.size() - 2], 0.5f);  // its east edge is the cell's middle
}

CM_TEST(world_cell, collider_parts_under_65535) {
    cw::WorldScene s = cell_grid(4, 4);
    add_hull(s, 50000);
    cw::Cell c = cell_by_id(s, "c0_0_4x4");
    CHECK_EQ(c.collision.size(), size_t(1));
    const std::vector<CellMesh> parts = build_cell_colliders(s, c);
    CHECK_EQ(parts.size(), size_t(3));
    if (parts.size() != 3) return;
    CHECK_EQ(parts[0].idx.size() / 3, size_t(21845));
    CHECK_EQ(parts[1].idx.size() / 3, size_t(21845));
    CHECK_EQ(parts[2].idx.size() / 3, size_t(6310));
    for (const CellMesh& p : parts) {
        CHECK(p.vertices() <= size_t(65535));
        CHECK_EQ(p.vertices(), p.idx.size());
        bool seq = true;
        for (size_t i = 0; i < p.idx.size(); ++i) if (p.idx[i] != uint32_t(i)) seq = false;
        CHECK(seq);
    }
    CHECK_EQ(parts[0].pos[0], -9000.f - c.origin[0]);
    CHECK_EQ(parts[0].pos[1], 33000.f - c.origin[1]);
    CHECK_EQ(parts[0].pos[2], 1.f);
}

CM_TEST(world_cell, water_only_where_below) {
    cw::WorldScene s = cell_grid(1, 1);
    add_water(s);
    const cw::Cell c = cell_by_id(s, "c0_0_1x1");
    set_all_heights(s, -5.f);   // all above the plane (up is -Z)
    CHECK(build_cell_water(s, c).pos.empty());
    set_all_heights(s, 5.f);    // all below it
    const CellMesh w = build_cell_water(s, c);
    CHECK_EQ(w.vertices(), size_t(33 * 33));
    CHECK_EQ(w.idx.size(), size_t(32 * 32 * 6));
    if (w.vertices() != 33 * 33) return;
    bool flat = true;
    for (size_t i = 0; i < w.vertices(); ++i) if (w.pos[i * 3 + 2] != 0.f) flat = false;
    CHECK(flat);
    CHECK_EQ(w.uv[w.uv.size() - 1], 1.f);
    s.water.hasPlane = false;
    CHECK(build_cell_water(s, c).pos.empty());
}

CM_TEST(world_cell, gltf_layout) {
    cw::WorldScene s = cell_grid(4, 4);
    add_water(s);
    add_hull(s, 4);
    const cw::Cell c = cell_by_id(s, "c0_0_4x4");
    const CellMesh terrain = build_cell_terrain(s.terrain, c);
    set_all_heights(s, 5.f);
    const CellMesh water = build_cell_water(s, c);
    CHECK(!water.pos.empty());
    const std::vector<CellMesh> colliders = build_cell_colliders(s, c);
    CHECK_EQ(colliders.size(), size_t(1));

    const fs::path dir = fresh_dir("cell_layout");
    const fs::path glb = dir / "c0_0_4x4.glb";
    std::string err;
    CHECK(write_cell_gltf(glb.string(), c, terrain, &water, colliders,
                          CellImages{"c0_0_4x4_albedo.png", "c0_0_4x4_water.png"}, &err));
    CHECK(err.empty());
    const json g = glb_json(glb);
    CHECK(g.is_object());
    if (!g.is_object()) return;

    const json& root = scene_root(g);
    CHECK(root["name"] == "GW2_MapToGltf");
    const std::array<float, 16> M = map_to_gltf_root(1.0);
    CHECK_EQ(root["matrix"].size(), size_t(16));
    for (size_t i = 0; i < 16; ++i) CHECK_EQ(root["matrix"][i].get<float>(), M[i]);
    CHECK_EQ(root["children"].size(), size_t(1));
    const json& cell = g["nodes"][root["children"][0].get<size_t>()];
    CHECK(cell["name"] == "Cell");
    for (size_t k = 0; k < 3; ++k) CHECK_EQ(cell["translation"][k].get<float>(), c.origin[k]);
    std::vector<std::string> kids;
    for (const json& k : cell["children"]) kids.push_back(g["nodes"][k.get<size_t>()]["name"]);
    CHECK(kids == std::vector<std::string>({"Terrain", "Water", "Collider_0"}));

    CHECK(g["images"][0]["uri"] == "c0_0_4x4_albedo.png");
    CHECK(g["images"][1]["uri"] == "c0_0_4x4_water.png");
    std::map<std::string, json> mats;
    for (const json& m : g["materials"]) mats[m["name"].get<std::string>()] = m;
    CHECK(mats.count("Terrain") && mats.count("Water") && mats.count("Collision"));
    CHECK(mats["Water"]["alphaMode"] == "BLEND");
    CHECK(mats["Terrain"]["pbrMetallicRoughness"].contains("baseColorTexture"));
    CHECK(mats["Water"]["pbrMetallicRoughness"].contains("baseColorTexture"));
    CHECK_FALSE(mats["Collision"]["pbrMetallicRoughness"].contains("baseColorTexture"));

    // Faces up after import, read back from the .glb. The root has det < 0, so
    // glTF takes clockwise as the front face below it: the front normal of a
    // triangle is M * ((b-a)x(c-a)) of its raw numbers (world_probe.cpp).
    const json* tnode = node_named(g, "Terrain");
    CHECK(tnode != nullptr);
    if (!tnode) return;
    const std::vector<uint8_t> bin = glb_bin(glb);
    const json& tprim = g["meshes"][(*tnode)["mesh"].get<size_t>()]["primitives"][0];
    CHECK_EQ(g["accessors"][tprim["indices"].get<size_t>()]["componentType"].get<int>(), 5123);   // 16-bit
    const std::vector<double> pos = read_accessor(g, bin, tprim["attributes"]["POSITION"].get<size_t>());
    const std::vector<double> nrm = read_accessor(g, bin, tprim["attributes"]["NORMAL"].get<size_t>());
    const std::vector<double> idx = read_accessor(g, bin, tprim["indices"].get<size_t>());
    CHECK_EQ(pos.size(), size_t(16641 * 3));
    if (pos.size() != 16641 * 3 || nrm.size() != pos.size() || idx.size() < 3) return;
    float a[3], b[3], cc[3];
    for (size_t k = 0; k < 3; ++k) {
        a[k] = float(pos[size_t(idx[0]) * 3 + k]);
        b[k] = float(pos[size_t(idx[1]) * 3 + k]);
        cc[k] = float(pos[size_t(idx[2]) * 3 + k]);
    }
    float raw[3], front[3], n0[3];
    cross3(a, b, cc, raw);
    mul_dir(M, raw, front);
    CHECK(front[1] > 0);   // glTF +Y is up
    bool allUp = true;
    for (size_t v = 0; v * 3 < nrm.size(); ++v) {
        const float rn[3] = {float(nrm[v * 3]), float(nrm[v * 3 + 1]), float(nrm[v * 3 + 2])};
        mul_dir(M, rn, n0);
        if (!(n0[1] > 0)) allUp = false;
    }
    CHECK(allUp);
    // det(M) < 0: the handedness change the front-face rule above relies on.
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1}, o[3] = {0, 0, 0};
    float mx[3], my[3], mz[3], xy[3];
    mul_dir(M, ex, mx); mul_dir(M, ey, my); mul_dir(M, ez, mz);
    cross3(o, mx, my, xy);
    CHECK(xy[0] * mz[0] + xy[1] * mz[1] + xy[2] * mz[2] < 0);
}

CM_TEST(world_cell, water_flat_without_preset) {
    // Spec D3 "No preset": a plane without an env water preset still gets its
    // water, in the map view's flat colour.
    cw::WorldScene s = cell_grid(1, 1);
    s.water.hasPlane = true; s.water.planeZ = 0;
    const cw::Cell c = cell_by_id(s, "c0_0_1x1");
    set_all_heights(s, -5.f);
    CHECK(build_cell_water(s, c).pos.empty());   // still depth-tested
    set_all_heights(s, 5.f);
    const CellMesh water = build_cell_water(s, c);
    CHECK_EQ(water.vertices(), size_t(33 * 33));
    const CellMesh terrain = build_cell_terrain(s.terrain, c);

    const fs::path glb = fresh_dir("cell_flat_water") / "c0_0_1x1.glb";
    CHECK(write_cell_gltf(glb.string(), c, terrain, &water, {}, CellImages{"c0_0_1x1_albedo.png", ""}));
    const json g = glb_json(glb);
    CHECK(g.is_object());
    if (!g.is_object()) return;
    CHECK_EQ(g["images"].size(), size_t(1));
    const json* wm = nullptr;
    for (const json& m : g["materials"]) if (m["name"] == "Water") wm = &m;
    CHECK(wm != nullptr);
    if (!wm) return;
    CHECK((*wm)["alphaMode"] == "BLEND");
    const json& pbr = (*wm)["pbrMetallicRoughness"];
    CHECK_FALSE(pbr.contains("baseColorTexture"));
    CHECK_EQ(pbr["baseColorFactor"].size(), size_t(4));
    for (size_t k = 0; k < 3; ++k) CHECK_EQ(pbr["baseColorFactor"][k].get<float>(), cw::kFlatWaterTint[k]);
    CHECK_EQ(pbr["baseColorFactor"][3].get<float>(), cw::kFlatWaterAlpha);
    CHECK((*wm)["extras"]["note"] == std::string(kFlatWaterNote));
    CHECK(std::string(kFlatWaterNote).rfind("water: no preset; flat colour", 0) == 0);
}

CM_TEST(world_cell, odd_chunk_warned_not_dropped_silently) {
    cw::WorldScene s = cell_grid(2, 1);
    const cw::Cell c = cell_by_id(s, "c0_0_2x1");
    cw::TerrainChunk& odd = s.terrain.chunks[1];   // chunk (1, 0)
    odd.samples = 17;
    odd.heights.assign(17 * 17, 0.f);
    std::vector<std::string> warn;
    const CellMesh m = build_cell_terrain(s.terrain, c, &warn);
    CHECK_EQ(m.vertices(), size_t(33 * 33));
    CHECK_EQ(warn.size(), size_t(1));
    if (warn.size() == 1) {
        CHECK(warn[0].find("c0_0_2x1") != std::string::npos);
        CHECK(warn[0].find("(1, 0)") != std::string::npos);
        CHECK(warn[0].find("17") != std::string::npos);
    }
    // A present chunk with no usable samples is warned too.
    odd.heights.clear();
    warn.clear();
    build_cell_terrain(s.terrain, c, &warn);
    CHECK_EQ(warn.size(), size_t(1));
    // A full cell warns nothing.
    warn.clear();
    build_cell_terrain(cell_grid(2, 1).terrain, c, &warn);
    CHECK(warn.empty());
}

// ------------------------------------------------------------ world manifest
// Spec "manifest.json (version 1)"; schema tools/world/castlemist-world.schema.json.

namespace {

/// The spec's prop (pos {10, 20, 30}, rot {0.3, -0.2, 1.1}, scale 2), built as
/// test_world.cpp's prop_transform_matches_client builds it.
cw::PropInstance spec_prop() {
    castlemist::model::Extractor::MapProp p;
    p.fileId = 101;
    p.pos[0] = 10; p.pos[1] = 20; p.pos[2] = 30;
    p.rot[0] = 0.3f; p.rot[1] = -0.2f; p.rot[2] = 1.1f;
    p.scale = 2.0f;
    p.group = "propArray";
    cw::WorldScene s;
    cw::build_props({p}, s);
    return s.props.empty() ? cw::PropInstance{} : s.props[0];
}

/// Resolve a local "#/$defs/<name>" $ref against @p root.
const json& schema_deref(const json& root, const json& s) {
    if (s.is_object() && s.contains("$ref")) {
        const std::string ref = s["$ref"].get<std::string>();
        const std::string pre = "#/$defs/";
        if (ref.rfind(pre, 0) == 0 && root["$defs"].contains(ref.substr(pre.size())))
            return schema_deref(root, root["$defs"][ref.substr(pre.size())]);
    }
    return s;
}

/// Keys of @p obj are a superset of the schema's `required` and a subset of its `properties`.
void check_keys(const json& root, const json& schema, const json& obj, const std::string& where) {
    const json& s = schema_deref(root, schema);
    CHECK(obj.is_object());
    CHECK(s.contains("properties"));
    if (!obj.is_object() || !s.contains("properties")) return;
    for (const auto& r : s.value("required", json::array()))
        if (!obj.contains(r.get<std::string>()))
            ::castlemist::test::fail(__FILE__, __LINE__, where + ": missing required " + r.get<std::string>());
    for (auto it = obj.begin(); it != obj.end(); ++it)
        if (!s["properties"].contains(it.key()))
            ::castlemist::test::fail(__FILE__, __LINE__, where + ": key not in schema " + it.key());
}

/// The same rule wherever the schema names properties: walks the whole manifest
/// through properties / additionalProperties / items, so no nested object drifts.
void check_keys_deep(const json& root, const json& schema, const json& v, const std::string& where) {
    const json& s = schema_deref(root, schema);
    if (!s.is_object()) return;
    if (v.is_object()) {
        if (s.contains("properties")) {
            check_keys(root, s, v, where);
            for (auto it = v.begin(); it != v.end(); ++it)
                if (s["properties"].contains(it.key()))
                    check_keys_deep(root, s["properties"][it.key()], it.value(), where + "." + it.key());
        } else if (s.contains("additionalProperties") && s["additionalProperties"].is_object()) {
            for (auto it = v.begin(); it != v.end(); ++it)
                check_keys_deep(root, s["additionalProperties"], it.value(), where + "." + it.key());
        }
    } else if (v.is_array() && s.contains("items")) {
        for (size_t k = 0; k < v.size(); ++k)
            check_keys_deep(root, s["items"], v[k], where + "[" + std::to_string(k) + "]");
    }
}

json read_schema() {
    std::ifstream in(fs::path(CM_SOURCE_DIR) / "tools" / "world" / "castlemist-world.schema.json");
    if (!in) return json();
    try { return json::parse(in); } catch (...) { return json(); }
}

/// A one-cell scene: 1 x 1 chunk under water, two props of model 101 (the
/// second non-uniformly scaled), one of model 102 whose folder failed.
struct SyntheticManifest {
    cw::WorldScene scene;
    cw::Cell cell;
    WorldFolderOptions opts;
    WorldManifestInput in;
};

void make_synthetic(SyntheticManifest& m) {
    m.scene = cell_grid(1, 1);
    m.scene.mapFileId = 1151420;
    m.scene.hasBounds = true;
    m.scene.bounds[0] = kCX0; m.scene.bounds[1] = kCY1 - kCC; m.scene.bounds[2] = kCX0 + kCC; m.scene.bounds[3] = kCY1;
    add_water(m.scene);
    m.scene.environment.light.present = true;
    m.scene.warnings.push_back("scene: a synthetic warning");
    cw::PropInstance a = spec_prop();
    a.world[12] = kCX0 + 100; a.world[13] = kCY1 - 100; a.world[14] = -10;
    cw::PropInstance b = a;
    b.world[4] *= 2; b.world[5] *= 2; b.world[6] *= 2;   // non-uniform
    cw::PropInstance c = a;
    c.model = 1;
    c.group = "propAnimArray";
    m.scene.models = {cw::PropModel{101}, cw::PropModel{102}};
    m.scene.props = {a, b, c};
    m.cell = cell_by_id(m.scene, "c0_0_1x1");
    m.cell.props = {0, 1, 2};

    ManifestModel m0;
    m0.index = 0; m0.fileId = 101; m0.ok = true;
    m0.folder = "models/101"; m0.glb = "models/101/101.glb"; m0.materials = "models/101/materials.json";
    m0.triangles = 12; m0.textureBytes = 1398101; m0.clips = {"idle"}; m0.darkDecalMaterials = {"Mat_58246"};
    m0.uvScroll = json::array({json{{"material", "Mat_1"}, {"uvIndex", 1}, {"scroll", {0, 0.2}}}});
    ManifestModel m1;
    m1.index = 1; m1.fileId = 102; m1.ok = false; m1.error = "model did not load";

    ManifestCell mc;
    mc.cell = &m.cell; mc.glb = "cells/c0_0_1x1.glb"; mc.albedo = "cells/c0_0_1x1_albedo.png";
    mc.water = "cells/c0_0_1x1_water.png"; mc.albedoW = 256; mc.albedoH = 256;
    mc.colliderParts = 1; mc.colliderTriangles = 12;

    m.in.scene = &m.scene; m.in.opts = &m.opts;
    m.in.models = {m0, m1};
    m.in.cells = {mc};
    m.in.sky = json{{"json", "sky/sky.json"}, {"default", "day"}, {"equirect", "sky/day/baked/equirect.png"}};
    m.in.spawn = json{{"map", {kCX0 + 1536, kCY1 - 1536, 5}}};
    m.in.counts = json{{"animated", 1}, {"animatedResolved", 0}};
    m.in.warnings = {"export: a synthetic warning"};
    m.in.clipOf = [](uint32_t) { return std::string(); };
}

/// A test-local checker for exactly the schema's keyword subset (Task 9 builds
/// the real one in Python): $ref (local #/$defs), type (string or union),
/// properties, required, additionalProperties (false or a schema), items, enum,
/// const, minimum, maximum, minItems, maxItems; the annotations $schema, $id,
/// $defs, title and description are ignored. Any other keyword is an error.
bool schema_type_is(const json& v, const std::string& t) {
    if (t == "null") return v.is_null();
    if (t == "boolean") return v.is_boolean();
    if (t == "object") return v.is_object();
    if (t == "array") return v.is_array();
    if (t == "string") return v.is_string();
    if (t == "number") return v.is_number();
    if (t == "integer")
        return v.is_number_integer() || (v.is_number_float() && std::isfinite(v.get<double>()) &&
                                         v.get<double>() == std::floor(v.get<double>()));
    return false;
}

void schema_check(const json& root, const json& s, const json& v, const std::string& path,
                  std::vector<std::string>& errs) {
    static const std::vector<std::string> kKnown = {
        "$schema", "$id", "$defs", "$ref", "title", "description", "type", "properties", "required",
        "additionalProperties", "items", "enum", "const", "minimum", "maximum", "minItems", "maxItems"};
    if (!s.is_object()) { errs.push_back(path + ": schema is not an object"); return; }
    for (auto it = s.begin(); it != s.end(); ++it)
        if (std::find(kKnown.begin(), kKnown.end(), it.key()) == kKnown.end())
            errs.push_back("schema: unsupported keyword " + it.key() + " at " + path);
    if (s.contains("$ref")) {
        const std::string ref = s["$ref"].get<std::string>(), pre = "#/$defs/";
        if (ref.rfind(pre, 0) != 0 || !root["$defs"].contains(ref.substr(pre.size())))
            errs.push_back(path + ": unresolved $ref " + ref);
        else
            schema_check(root, root["$defs"][ref.substr(pre.size())], v, path, errs);
    }
    if (s.contains("type")) {
        const json types = s["type"].is_array() ? s["type"] : json::array({s["type"]});
        bool ok = false;
        for (const auto& t : types) ok = ok || schema_type_is(v, t.get<std::string>());
        if (!ok) { errs.push_back(path + ": " + v.dump() + " is not of type " + s["type"].dump()); return; }
    }
    if (s.contains("const") && v != s["const"]) errs.push_back(path + ": not the const " + s["const"].dump());
    if (s.contains("enum") && std::find(s["enum"].begin(), s["enum"].end(), v) == s["enum"].end())
        errs.push_back(path + ": " + v.dump() + " not in enum");
    if (v.is_number()) {
        if (s.contains("minimum") && v.get<double>() < s["minimum"].get<double>()) errs.push_back(path + ": below minimum");
        if (s.contains("maximum") && v.get<double>() > s["maximum"].get<double>()) errs.push_back(path + ": above maximum");
    }
    if (v.is_object()) {
        for (const auto& r : s.value("required", json::array()))
            if (!v.contains(r.get<std::string>())) errs.push_back(path + ": missing required " + r.get<std::string>());
        const json props = s.value("properties", json::object());
        for (auto it = v.begin(); it != v.end(); ++it) {
            const std::string p = path + "." + it.key();
            if (props.contains(it.key())) {
                schema_check(root, props[it.key()], it.value(), p, errs);
            } else if (s.contains("additionalProperties")) {
                const json& ap = s["additionalProperties"];
                if (ap.is_boolean() && !ap.get<bool>()) errs.push_back(path + ": unexpected property " + it.key());
                else if (ap.is_object()) schema_check(root, ap, it.value(), p, errs);
            }
        }
    }
    if (v.is_array()) {
        if (s.contains("minItems") && v.size() < s["minItems"].get<size_t>()) errs.push_back(path + ": too few items");
        if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>()) errs.push_back(path + ": too many items");
        if (s.contains("items"))
            for (size_t k = 0; k < v.size(); ++k)
                schema_check(root, s["items"], v[k], path + "[" + std::to_string(k) + "]", errs);
    }
}

/// Every violation of the committed schema by @p man (empty = valid).
std::vector<std::string> schema_violations(const json& man) {
    static const json schema = read_schema();
    std::vector<std::string> errs;
    if (!schema.is_object()) return {"schema: not readable"};
    schema_check(schema, schema, man, "manifest", errs);
    return errs;
}

/// Fails the test once per violation, naming @p variant.
void check_valid(const json& man, const std::string& variant) {
    for (const auto& e : schema_violations(man))
        ::castlemist::test::fail(__FILE__, __LINE__, variant + ": " + e);
    CHECK(json::parse(man.dump()) == man);   // no NaN / inf survives a dump
}

bool has_warning(const json& man, const std::string& needle) {
    if (!man.contains("warnings")) return false;
    for (const auto& w : man["warnings"]) if (w.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

CM_TEST(world_manifest, unity_trs_roundtrip) {
    const cw::PropInstance p = spec_prop();
    const float origin[3] = {5, 5, 0};
    UnityTrs u{};
    CHECK(unity_trs(p.world, origin, u));
    CHECK_NEAR(u.s, 2.0, 1e-5);
    CHECK(u.r[3] >= 0);
    CHECK_NEAR(double(u.r[0]) * u.r[0] + double(u.r[1]) * u.r[1] + double(u.r[2]) * u.r[2] + double(u.r[3]) * u.r[3],
               1.0, 1e-5);
    // Expected: map_matrix_to_unity(M with its translation minus the origin).
    float local[16], want[16];
    std::memcpy(local, p.world, sizeof local);
    for (int k = 0; k < 3; ++k) local[12 + k] -= origin[k];
    cw::map_matrix_to_unity(local, want);
    // Rebuilt: T(t) * R(r) * S(s), R the standard (Unity Quaternion) rotation matrix.
    const double x = u.r[0], y = u.r[1], z = u.r[2], w = u.r[3];
    const double R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) CHECK_NEAR(u.s * R[r][c], want[c * 4 + r], 1e-4);
    for (int r = 0; r < 3; ++r) CHECK_NEAR(u.t[r], want[12 + r], 1e-4);
    // And the Unity transform applied to map_to_unity(v) lands on map_to_unity(M' v).
    const float v[3] = {1, 2, 3};
    float mv[3], uv[3], umv[3];
    for (int r = 0; r < 3; ++r) mv[r] = local[r] * v[0] + local[4 + r] * v[1] + local[8 + r] * v[2] + local[12 + r];
    cw::map_to_unity(v, uv);
    cw::map_to_unity(mv, umv);
    for (int r = 0; r < 3; ++r)
        CHECK_NEAR(u.s * (R[r][0] * uv[0] + R[r][1] * uv[1] + R[r][2] * uv[2]) + u.t[r], umv[r], 1e-3);
    // Every branch of Shepperd's method: identity and the three 180-degree turns.
    const float axes[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (const auto& ax : axes) {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (ax[0] + ax[1] + ax[2] > 0)
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r) m[c * 4 + r] = 2 * ax[r] * ax[c] - (r == c ? 1.f : 0.f);
        const float zero[3] = {0, 0, 0};
        UnityTrs q{};
        CHECK(unity_trs(m, zero, q));
        float um[16];
        cw::map_matrix_to_unity(m, um);
        const double qx = q.r[0], qy = q.r[1], qz = q.r[2], qw = q.r[3];
        const double Q[3][3] = {{1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qz * qw), 2 * (qx * qz + qy * qw)},
                                {2 * (qx * qy + qz * qw), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qx * qw)},
                                {2 * (qx * qz - qy * qw), 2 * (qy * qz + qx * qw), 1 - 2 * (qx * qx + qy * qy)}};
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r) CHECK_NEAR(Q[r][c], um[c * 4 + r], 1e-5);
        CHECK(qw >= 0);
        CHECK_NEAR(q.s, 1.0, 1e-6);
    }
}

CM_TEST(world_manifest, unity_trs_rejects_bad) {
    const float zero[3] = {0, 0, 0};
    UnityTrs u{};
    float nonUniform[16] = {1, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // scale (1, 2, 1)
    CHECK_FALSE(unity_trs(nonUniform, zero, u));
    float mirrored[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // det < 0
    CHECK_FALSE(unity_trs(mirrored, zero, u));
    float negUniform[16] = {-2, 0, 0, 0, 0, -2, 0, 0, 0, 0, -2, 0, 0, 0, 0, 1};   // scale -2: det < 0
    CHECK_FALSE(unity_trs(negUniform, zero, u));
    float nan[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    nan[13] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(unity_trs(nan, zero, u));
    float inf[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    inf[0] = std::numeric_limits<float>::infinity();
    CHECK_FALSE(unity_trs(inf, zero, u));
    const float nanOrigin[3] = {0, std::numeric_limits<float>::quiet_NaN(), 0};
    float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK_FALSE(unity_trs(id, nanOrigin, u));
    float zeroScale[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    CHECK_FALSE(unity_trs(zeroScale, zero, u));
    float shear[16] = {1, 0, 0, 0, 0.6f, 0.8f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // equal lengths, not orthogonal
    CHECK_FALSE(unity_trs(shear, zero, u));
    float projective[16] = {1, 0, 0, 0.5f, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK_FALSE(unity_trs(projective, zero, u));
    // The spec prop with one axis doubled: non-uniform after a rotation.
    cw::PropInstance p = spec_prop();
    for (int r = 0; r < 3; ++r) p.world[8 + r] *= 2;
    CHECK_FALSE(unity_trs(p.world, zero, u));
    CHECK(unity_trs(spec_prop().world, zero, u));
}

CM_TEST(world_manifest, keys_match_schema) {
    const json schema = read_schema();
    CHECK(schema.is_object());
    if (!schema.is_object()) return;
    CHECK(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
    CHECK(schema["$id"] == "castlemist-world/1");
    SyntheticManifest m;
    make_synthetic(m);
    const json man = world_manifest_json(m.in);
    CHECK(man.is_object());
    if (!man.is_object()) return;
    CHECK_EQ(man.value("version", 0), kWorldManifestVersion);
    CHECK_EQ(man.value("version", 0), 1);
    CHECK(man.value("format", std::string()) == "castlemist-world");
    check_keys(schema, schema, man, "manifest");
    const bool shaped = man.contains("cells") && man["cells"].is_array() && !man["cells"].empty() &&
                        man.contains("models") && man["models"].is_array() && !man["models"].empty();
    CHECK(shaped);
    if (!shaped) return;
    const json& cellsS = schema_deref(schema, schema["properties"]["cells"]);
    check_keys(schema, cellsS["items"], man["cells"][0], "cells[0]");
    CHECK(!man["cells"][0]["instances"].empty());
    if (man["cells"][0]["instances"].empty()) return;
    const json& cellS = schema_deref(schema, cellsS["items"]);
    check_keys(schema, schema_deref(schema, cellS["properties"]["instances"])["items"], man["cells"][0]["instances"][0],
               "cells[0].instances[0]");
    check_keys(schema, schema_deref(schema, schema["properties"]["models"])["items"], man["models"][0], "models[0]");
    check_keys_deep(schema, schema, man, "manifest");
    check_valid(man, "keys_match_schema");
    // The output dumps and parses again (no NaN / inf); kept on disk for inspection.
    CHECK(json::parse(man.dump()) == man);
    std::ofstream(fresh_dir("manifest") / "manifest.json") << man.dump(2);

    // Content: one instance written (prop 0); prop 1 is non-uniform, prop 2's model failed.
    const json& cell = man["cells"][0];
    CHECK_EQ(cell["instances"].size(), size_t(1));
    CHECK_EQ(cell["instances"][0]["prop"].get<int>(), 0);
    CHECK_EQ(cell["instances"][0]["model"].get<int>(), 0);
    CHECK(cell["instances"][0]["clip"].is_null());
    CHECK_EQ(cell["instances"][0]["m"].size(), size_t(16));
    CHECK_EQ(man["counts"]["props"].get<int>(), 3);
    CHECK_EQ(man["counts"]["propsWritten"].get<int>(), 1);
    CHECK_EQ(man["counts"]["propsSkipped"].get<int>(), 2);
    CHECK_EQ(man["counts"]["models"].get<int>(), 2);
    CHECK_EQ(man["counts"]["modelsFailed"].get<int>(), 1);
    CHECK_EQ(man["counts"]["cells"].get<int>(), 1);
    CHECK_EQ(man["counts"]["chunks"].get<int>(), 1);
    CHECK_EQ(man["counts"]["animated"].get<int>(), 1);
    CHECK_EQ(man["counts"]["clipsFailedDecode"].get<int>(), 0);
    CHECK(has_warning(man, "props: 1 placements with a non-uniform or degenerate transform skipped"));
    CHECK(has_warning(man, "props: 1 placements of failed or missing models skipped"));
    CHECK(man["warnings"][0] == "scene: a synthetic warning");
    CHECK(has_warning(man, "export: a synthetic warning"));
    CHECK(man["models"][1]["status"] == "failed");
    CHECK(man["models"][1]["error"] == "model did not load");
    CHECK(man["models"][0]["status"] == "ok");
    CHECK(man["models"][0]["fbx"].is_null());
    CHECK(man["models"][0]["uvScroll"][0]["unit"] == "per second, UNPROVEN");
    CHECK(cell["water"]["texture"] == "cells/c0_0_1x1_water.png");
    CHECK(cell["chunks"] == json({0, 0, 0, 0}));
    CHECK(man["map"]["fileId"] == 1151420);
    // The unity block is unity_trs relative to the cell origin.
    UnityTrs u{};
    CHECK(unity_trs(m.scene.props[0].world, m.cell.origin, u));
    for (size_t k = 0; k < 3; ++k) CHECK_EQ(cell["instances"][0]["unity"]["t"][k].get<float>(), u.t[k]);
    for (size_t k = 0; k < 4; ++k) CHECK_EQ(cell["instances"][0]["unity"]["r"][k].get<float>(), u.r[k]);
    CHECK_EQ(cell["instances"][0]["unity"]["s"].get<float>(), u.s);
    // Frame: the rows of map_to_unity / map_to_blender; units and scale UNPROVEN.
    CHECK(man["frame"]["toUnity"] == json({{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}));
    CHECK(man["frame"]["toBlender"] == json({{1, 0, 0}, {0, 1, 0}, {0, 0, -1}}));
    CHECK(man["frame"]["units"]["perMetre"].is_null());
    CHECK(man["frame"]["units"]["status"] == "UNPROVEN");
    CHECK_NEAR(man["frame"]["units"]["unityScale"]["value"].get<double>(), 0.0254, 1e-7);
    CHECK(man["frame"]["units"]["unityScale"]["status"] == "UNPROVEN");
    const auto root = map_to_gltf_root(1);
    for (size_t k = 0; k < 16; ++k) CHECK_EQ(man["frame"]["gltfRoot"][k].get<float>(), root[k]);
    // Light (spec D5, the brief's "Settled"): toSun as stored, unityForward = -map_to_unity(toSun).
    const auto& L = m.scene.environment.light;
    float uf[3], bd[3];
    cw::map_to_unity(L.sunDir, uf);
    cw::map_to_blender(L.sunDir, bd);
    for (size_t k = 0; k < 3; ++k) {
        CHECK_EQ(man["light"]["sun"]["toSun"][k].get<float>(), L.sunDir[k]);
        CHECK_EQ(man["light"]["sun"]["unityForward"][k].get<float>(), -uf[k]);
        CHECK_EQ(man["light"]["sun"]["blenderDirection"][k].get<float>(), -bd[k]);
        CHECK_NEAR(man["light"]["ambient"][k].get<float>(), L.fillColor[k] * std::min(L.fillIntensity, 1.0f), 1e-6);
    }
    // Spawn: unity filled in from map; the probe sits at the spawn.
    float su[3];
    const float sm[3] = {kCX0 + 1536, kCY1 - 1536, 5};
    cw::map_to_unity(sm, su);
    for (size_t k = 0; k < 3; ++k) CHECK_EQ(man["spawn"]["unity"][k].get<float>(), su[k]);
    CHECK(man["frameProbe"]["map"] == man["spawn"]["map"]);
    CHECK(man["frameProbe"]["expect"] == frame_probe_expect());
    CHECK(man["water"]["plane"] == true);
    CHECK_EQ(man["water"]["preset"].get<int>(), 0);
    CHECK(man["terrain"]["present"] == true);
    CHECK_EQ(man["terrain"]["segments"].get<int>(), 32);
}

CM_TEST(world_manifest, settings_labelled) {
    SyntheticManifest m;
    make_synthetic(m);
    const json man = world_manifest_json(m.in);
    const bool shaped = man.contains("settings") && man["settings"].is_object() && !man["settings"].empty();
    CHECK(shaped);
    if (!shaped) return;
    const std::vector<std::string> allowed = {"UNPROVEN", "castlemist's choice", "measured"};
    size_t unproven = 0;
    for (auto it = man["settings"].begin(); it != man["settings"].end(); ++it) {
        const std::string st = it.value().value("status", std::string());
        if (std::find(allowed.begin(), allowed.end(), st) == allowed.end())
            ::castlemist::test::fail(__FILE__, __LINE__, "setting " + it.key() + " has status '" + st + "'");
        CHECK(it.value().contains("value"));
        CHECK(!it.value().value("source", std::string()).empty());
        if (st != "UNPROVEN") continue;
        ++unproven;
        bool found = false;
        for (const auto& w : man["warnings"]) {
            const std::string s = w.get<std::string>();
            if (s.find(it.key()) != std::string::npos && s.find("UNPROVEN") != std::string::npos) found = true;
        }
        if (!found) ::castlemist::test::fail(__FILE__, __LINE__, "no UNPROVEN warning for " + it.key());
    }
    CHECK(unproven >= 8);
    CHECK(man["settings"]["groundRepeatsPerChunk"]["value"] == 8);
    CHECK(man["settings"]["groundRepeatsPerChunk"]["status"] == "UNPROVEN");
    CHECK(man["settings"]["unityScale"]["status"] == "UNPROVEN");
    CHECK(man["settings"]["ambient"]["status"] == "castlemist's choice");
    CHECK(man["settings"]["texelsPerChunk"]["value"] == 256);
    CHECK(man["settings"]["cellMaxTriangles"]["value"] == 1000000);
    // The settings are world_export_settings and follow the options.
    WorldFolderOptions o;
    o.texelsPerChunk = 128;
    const auto set = world_export_settings(o);
    CHECK(!set.empty());
    bool saw = false;
    for (const auto& s : set) if (s.name == "texelsPerChunk") { saw = true; CHECK(s.value == 128); }
    CHECK(saw);
}

CM_TEST(world_manifest, validator_sees_mismatches) {
    // The test-local validator is not vacuous: each kind of drift is caught.
    SyntheticManifest m;
    make_synthetic(m);
    const json man = world_manifest_json(m.in);
    CHECK(schema_violations(man).empty());
    json bad = man;
    bad["light"]["sun"]["intensity"] = nullptr;   // null where the schema wants a number
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["map"]["extra"] = 1;                      // additionalProperties: false
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["cells"][0]["instances"][0]["unity"]["r"].push_back(0);   // maxItems
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["counts"].erase("props");                 // required
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["settings"]["texelsPerChunk"]["status"] = "guess";   // enum via $ref
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["version"] = 2;                           // const
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["counts"]["props"] = -1;                  // minimum
    CHECK(!schema_violations(bad).empty());
    bad = man;
    bad["cells"][0]["glb"] = nullptr;             // type without null
    CHECK(!schema_violations(bad).empty());
}

CM_TEST(world_manifest, variants_validate) {
    // normal (with caller extras in uvScroll, sky and spawn that must not leak)
    {
        SyntheticManifest m;
        make_synthetic(m);
        m.in.models[0].uvScroll = json::array(
            {json{{"material", "Mat_1"}, {"uvIndex", 1}, {"scroll", {0, 0.2}}, {"unit", "per frame"}, {"extra", 1}},
             json{{"material", 7}}});
        m.in.sky["mode"] = 3;
        m.in.spawn["unity"] = {1, 2, 3};
        m.in.spawn["note"] = "extra";
        const json man = world_manifest_json(m.in);
        check_valid(man, "normal");
        CHECK_EQ(man["models"][0]["uvScroll"].size(), size_t(1));
        CHECK(man["models"][0]["uvScroll"][0]["unit"] == "per second, UNPROVEN");
        CHECK_FALSE(man["models"][0]["uvScroll"][0].contains("extra"));
        CHECK(has_warning(man, "malformed uvScroll"));
        CHECK_FALSE(man["sky"].contains("mode"));
        CHECK_FALSE(man["spawn"].contains("note"));
        CHECK(man["spawn"]["unity"] != json({1, 2, 3}));   // recomputed from map
    }
    // flat water: plane, no preset -> water.flatColour + note, cell water texture null + note
    {
        SyntheticManifest m;
        make_synthetic(m);
        m.scene.environment.water.clear();
        set_all_heights(m.scene, 5.f);
        m.in.cells[0].water.clear();
        const json man = world_manifest_json(m.in);
        check_valid(man, "flat water");
        CHECK(man["water"]["preset"].is_null());
        CHECK_EQ(man["water"]["flatColour"].size(), size_t(4));
        CHECK(man["water"]["note"] == std::string(kFlatWaterNote));
        CHECK(man["cells"][0]["water"]["texture"].is_null());
        CHECK(man["cells"][0]["water"]["note"] == std::string(kFlatWaterNote));
        CHECK(has_warning(man, kFlatWaterNote));
    }
    // no plane
    {
        SyntheticManifest m;
        make_synthetic(m);
        m.scene.water.hasPlane = false;
        m.in.cells[0].water.clear();
        const json man = world_manifest_json(m.in);
        check_valid(man, "no plane");
        CHECK(man["water"] == json({{"plane", false}}));
        CHECK(man["cells"][0]["water"].is_null());
    }
    // no terrain (c_all), sky / spawn null, bounds null
    {
        SyntheticManifest m;
        make_synthetic(m);
        m.scene.terrain = cw::Terrain{};
        m.scene.hasBounds = false;
        m.cell = cw::Cell{};
        m.cell.id = "c_all"; m.cell.all = true;
        m.cell.props = {0, 1, 2};
        m.in.cells[0].albedo.clear();
        m.in.cells[0].water.clear();
        m.in.sky = nullptr;
        m.in.spawn = nullptr;
        const json man = world_manifest_json(m.in);
        check_valid(man, "no terrain");
        CHECK(man["terrain"] == json({{"present", false}}));
        CHECK(man["map"]["bounds"].is_null());
        CHECK(man["sky"].is_null());
        CHECK(man["spawn"].is_null());
        CHECK(man["cells"][0]["chunks"].is_null());
        CHECK(man["cells"][0]["albedoSize"].is_null());
        CHECK(man["frameProbe"]["map"] == json({0.0, 0.0, 0.0}));
        CHECK_EQ(man["counts"]["chunks"].get<int>(), 0);
    }
}

CM_TEST(world_manifest, non_finite_never_written) {
    // Ruling W6: never null for a number; optional blocks go null, bad cells are dropped.
    SyntheticManifest m;
    make_synthetic(m);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    m.scene.props[0].world[13] = nan;                 // placement: skipped
    m.scene.environment.light.sunIntensity = inf;     // light -> null
    m.scene.bounds[2] = nan;                          // bounds -> null
    m.scene.water.planeZ = nan;                       // water -> null, cell water -> null
    m.in.spawn = json{{"map", {1.0, nan, 2.0}}};      // nlohmann keeps the NaN: spawn -> null
    m.opts.unityScale = inf;                          // option -> default
    cw::Cell badCell = m.cell;                        // a second cell with a NaN rect: dropped
    badCell.id = "c9_9_1x1";
    badCell.rect[0] = nan;
    badCell.props = {0};
    ManifestCell bad = m.in.cells[0];
    bad.cell = &badCell;
    m.in.cells.push_back(bad);
    const json man = world_manifest_json(m.in);
    check_valid(man, "non-finite");
    CHECK(man["light"].is_null());
    CHECK(has_warning(man, "light.sun.intensity"));
    CHECK(man["map"]["bounds"].is_null());
    CHECK(has_warning(man, "map.bounds"));
    CHECK(man["water"].is_null());
    CHECK(has_warning(man, "water.z"));
    CHECK(man["cells"][0]["water"].is_null());
    CHECK(man["spawn"].is_null());
    CHECK(has_warning(man, "spawn.map"));
    CHECK_NEAR(man["frame"]["units"]["unityScale"]["value"].get<double>(), 0.0254, 1e-7);
    CHECK_NEAR(man["settings"]["unityScale"]["value"].get<double>(), 0.0254, 1e-7);
    CHECK(has_warning(man, "option unityScale"));
    CHECK(man["frameProbe"].is_object());   // placed at the kept cell's centre
    CHECK_EQ(man["cells"].size(), size_t(1));
    CHECK(has_warning(man, "cells: c9_9_1x1 dropped: non-finite number in rect"));
    CHECK(man["cells"][0]["instances"].empty());
    CHECK_EQ(man["counts"]["props"].get<int>(), 4);
    CHECK_EQ(man["counts"]["propsWritten"].get<int>(), 0);
    CHECK_EQ(man["counts"]["propsSkipped"].get<int>(), 4);
    CHECK_EQ(man["counts"]["cells"].get<int>(), 1);
    CHECK(has_warning(man, "props: 2 placements with a non-uniform or degenerate transform skipped"));
    CHECK(has_warning(man, "props: 1 placements in dropped cells skipped"));
    const std::string text = man.dump();
    CHECK(text.find("NaN") == std::string::npos);
}
