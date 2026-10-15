/// @file
/// @brief Tests for the world export (sub-project 4,
///        docs/superpowers/specs/2026-10-15-world-export-design.md): the frame
///        probe first (spec D6 "Frame probe").

#include "test_framework.h"

#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/world/frame.h"

#include <array>
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
