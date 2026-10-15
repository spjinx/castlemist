/// @file
/// @brief Tests for the world export (sub-project 4,
///        docs/superpowers/specs/2026-10-15-world-export-design.md): the frame
///        probe first (spec D6 "Frame probe").

#include "test_framework.h"

#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_cell.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/world/frame.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/native/granny_anim.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
