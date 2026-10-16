/// @file
/// @brief The world export's frame probe (world_probe.h; spec
///        docs/superpowers/specs/2026-10-15-world-export-design.md D6).

#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/world/frame.h"

#include "internal.h"
#include "long_path.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

/// One axis-aligned box [lo, hi] in map space as a glTF mesh: 24 vertices
/// (flat normals) and 36 indices, counter-clockwise seen from outside in the
/// raw numbers. Under the root's reflection (det -1) glTF makes clockwise the
/// front face (glTF 2.0, "Meshes": winding follows the sign of the node's
/// determinant), so the faces stay outward after it.
int write_box(GltfWriter& w, const float lo[3], const float hi[3], int material, const std::string& name) {
    std::vector<float> pos, nrm;
    std::vector<uint16_t> idx;
    for (int axis = 0; axis < 3; ++axis) {
        for (int side = 0; side < 2; ++side) {
            const float sgn = side ? 1.0f : -1.0f;
            const int u = (axis + 1) % 3, v = (axis + 2) % 3;  // u x v = +axis
            const float c = side ? hi[axis] : lo[axis];
            // Corners in (u, v): CCW about +axis; reversed for the -axis face.
            float uv[4][2] = {{lo[u], lo[v]}, {hi[u], lo[v]}, {hi[u], hi[v]}, {lo[u], hi[v]}};
            const auto base = static_cast<uint16_t>(pos.size() / 3);
            for (int k = 0; k < 4; ++k) {
                float p[3];
                p[axis] = c; p[u] = uv[k][0]; p[v] = uv[k][1];
                pos.insert(pos.end(), p, p + 3);
                float n[3] = {0, 0, 0};
                n[axis] = sgn;
                nrm.insert(nrm.end(), n, n + 3);
            }
            if (side) idx.insert(idx.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base, uint16_t(base + 2), uint16_t(base + 3)});
            else      idx.insert(idx.end(), {base, uint16_t(base + 2), uint16_t(base + 1), base, uint16_t(base + 3), uint16_t(base + 2)});
        }
    }
    const json mn = {lo[0], lo[1], lo[2]}, mx = {hi[0], hi[1], hi[2]};
    const int aPos = w.add_accessor(pos.data(), pos.size() * sizeof(float), 5126, "VEC3", pos.size() / 3, 34962, &mn, &mx);
    const int aNrm = w.add_accessor(nrm.data(), nrm.size() * sizeof(float), 5126, "VEC3", nrm.size() / 3, 34962);
    const int aIdx = w.add_accessor(idx.data(), idx.size() * sizeof(uint16_t), 5123, "SCALAR", idx.size(), 34963);
    return w.add_mesh(json{
        {"name", name},
        {"primitives", json::array({json{{"attributes", {{"POSITION", aPos}, {"NORMAL", aNrm}}},
                                         {"indices", aIdx},
                                         {"material", material}}})},
    });
}

} // namespace

bool write_frame_probe(const std::string& glbPathUtf8, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (error) error->clear();

    struct Bar { const char* name; int axis; float sign; float rgb[3]; };
    // Map space: east +X, north +Y, up -Z.
    const Bar bars[3] = {{"Probe_East", 0, 1.0f, {1, 0, 0}},
                         {"Probe_North", 1, 1.0f, {0, 1, 0}},
                         {"Probe_Up", 2, -1.0f, {0, 0, 1}}};
    const float h = kProbeThickness * 0.5f;

    GltfWriter w;
    w.use_extension("KHR_materials_unlit");
    json children = json::array();
    for (int b = 0; b < 3; ++b) {
        const Bar& bar = bars[b];
        float lo[3] = {-h, -h, -h}, hi[3] = {h, h, h};
        if (bar.sign > 0) { lo[bar.axis] = 0.0f; hi[bar.axis] = kProbeLengths[b]; }
        else              { lo[bar.axis] = -kProbeLengths[b]; hi[bar.axis] = 0.0f; }
        const int mat = w.add_material(json{
            {"name", std::string(bar.name) + "_Mat"},
            {"pbrMetallicRoughness", {{"baseColorFactor", {bar.rgb[0], bar.rgb[1], bar.rgb[2], 1.0}},
                                      {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
            {"extensions", {{"KHR_materials_unlit", json::object()}}},
        });
        const int mesh = write_box(w, lo, hi, mat, bar.name);
        children.push_back(w.add_node(json{{"name", bar.name}, {"mesh", mesh}}));
    }
    const std::array<float, 16> root = map_to_gltf_root(1.0);
    w.add_scene_root(w.add_node(json{
        {"name", "GW2_MapToGltf"},
        {"matrix", std::vector<float>(root.begin(), root.end())},
        {"children", children},
    }));

    const fs::path outPath = fs::path(std::u8string(glbPathUtf8.begin(), glbPathUtf8.end()));
    std::error_code ec;
    if (outPath.has_parent_path()) make_dirs(outPath.parent_path(), ec);
    const std::vector<uint8_t> glb = w.finish();
    std::ofstream out(long_path(outPath), std::ios::binary);
    if (!out) return fail("Could not open '" + glbPathUtf8 + "' for writing.");
    out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    out.close();
    if (!out) return fail("Failed writing '" + glbPathUtf8 + "'.");
    return true;
}

json frame_probe_expect() {
    const float map[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1}};  // east, north, up
    const char* names[3] = {"east", "north", "up"};
    json unity = json::object(), blender = json::object();
    for (int a = 0; a < 3; ++a) {
        float u[3], b[3];
        castlemist::world::map_to_unity(map[a], u);
        castlemist::world::map_to_blender(map[a], b);
        // + 0.0f turns -0 into 0, so the JSON reads [1,0,0] and not [1,-0.0,0].
        unity[names[a]] = {u[0] + 0.0f, u[1] + 0.0f, u[2] + 0.0f};
        blender[names[a]] = {b[0] + 0.0f, b[1] + 0.0f, b[2] + 0.0f};
    }
    return json{{"unity", unity}, {"blender", blender}, {"lengths", {3, 2, 1}}};
}

} // namespace castlemist::exportgltf
