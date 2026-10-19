/// @file
/// @brief Orchestrates a world engine WorldScene -> one .glb document:
///        terrain chunks, every placed prop and (optionally) collision.
///
/// Everything below the root stays in map space as stored (world/frame.h):
/// terrain vertices, PropInstance::world and CollisionInstance::world are
/// written as-is, and the one root node carries the map -> glTF axis change.
/// That change is a reflection, and glTF reverses the front-face winding
/// under a negative-determinant transform, so a triangle that faces outward
/// in map space (cross product (b-a)x(c-a) along its normal, the client's
/// clockwise-front convention in a left-handed frame) still faces outward
/// after import.

#include "internal.h"

#include "castlemist/exportgltf/world_export.h"
#include "castlemist/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;
using castlemist::world::WorldScene;

std::array<float, 16> map_to_gltf_root(double scale) {
    const float s = static_cast<float>(scale);
    // Column-major: the columns are the images of map +X, +Y, +Z.
    return {s, 0, 0, 0,     // +X east  -> glTF +X
            0, 0, -s, 0,    // +Y north -> glTF -Z
            0, -s, 0, 0,    // +Z down  -> glTF -Y
            0, 0, 0, 1};
}

namespace {

json matrix_json(const float m[16]) { return json(std::vector<float>(m, m + 16)); }

// One glTF mesh for one terrain chunk: positions in map space, normals from
// the height field (sampled across chunk edges, so shading has no seams),
// TEXCOORD_0 the map-wide position over [u0, u1] x [v0, v1] with v = 0 at the
// north edge (a top-down image's first row).
int write_terrain_chunk(GltfWriter& w, const castlemist::world::Terrain& t, const castlemist::world::TerrainChunk& c,
                        const float extent[4], int material) {
    const int n = c.samples;
    const float x0 = c.rect[0], y0 = c.rect[1], x1 = c.rect[2], y1 = c.rect[3];
    const float dx = (x1 - x0) / static_cast<float>(n - 1);
    const float dy = (y1 - y0) / static_cast<float>(n - 1);
    const float ew = extent[2] - extent[0], eh = extent[3] - extent[1];

    auto height = [&](int i, int j) { return c.heights[static_cast<size_t>(j) * n + i]; };
    // Height at map (x, y), falling back to this chunk's clamped sample off the terrain.
    auto height_near = [&](float x, float y, int i, int j) {
        bool inside = false;
        float h = castlemist::world::terrain_height_at(t, x, y, &inside);
        return inside ? h : height(std::clamp(i, 0, n - 1), std::clamp(j, 0, n - 1));
    };

    std::vector<float> pos, nrm, uv;
    pos.reserve(static_cast<size_t>(n) * n * 3);
    nrm.reserve(pos.capacity());
    uv.reserve(static_cast<size_t>(n) * n * 2);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float x = x0 + i * dx, y = y1 - j * dy, z = height(i, j);
            const float p[3] = {x, y, z};
            for (int k = 0; k < 3; ++k) { pos.push_back(p[k]); lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }

            // Tangents along +i (east) and +j (south); their cross product
            // points up (-Z) on flat ground, the outward side.
            const float ex = 2 * dx, ez = height_near(x + dx, y, i + 1, j) - height_near(x - dx, y, i - 1, j);
            const float sy = -2 * dy, sz = height_near(x, y - dy, i, j + 1) - height_near(x, y + dy, i, j - 1);
            float nv[3] = {0 * sz - ez * sy, ez * 0 - ex * sz, ex * sy - 0 * 0};
            const float len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
            if (len > 0) for (float& v : nv) v /= len; else { nv[0] = 0; nv[1] = 0; nv[2] = -1; }
            nrm.insert(nrm.end(), nv, nv + 3);

            uv.push_back(ew > 0 ? (x - extent[0]) / ew : 0.0f);
            uv.push_back(eh > 0 ? (extent[3] - y) / eh : 0.0f);
        }
    }

    // (i,j) (i+1,j) (i,j+1) has its cross product along -Z (up): outward.
    std::vector<uint32_t> idx;
    idx.reserve(static_cast<size_t>(n - 1) * (n - 1) * 6);
    for (int j = 0; j + 1 < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * n + i), b = a + 1;
            const uint32_t c2 = a + static_cast<uint32_t>(n), d = c2 + 1;
            idx.insert(idx.end(), {a, b, c2, b, d, c2});
        }
    }

    const size_t vcount = pos.size() / 3;
    json mn = {lo[0], lo[1], lo[2]}, mx = {hi[0], hi[1], hi[2]};
    int aPos = w.add_accessor(pos.data(), pos.size() * 4, 5126, "VEC3", vcount, 34962, &mn, &mx);
    int aNrm = w.add_accessor(nrm.data(), nrm.size() * 4, 5126, "VEC3", vcount, 34962);
    int aUv = w.add_accessor(uv.data(), uv.size() * 4, 5126, "VEC2", vcount, 34962);
    int aIdx = w.add_accessor(idx.data(), idx.size() * 4, 5125, "SCALAR", idx.size(), 34963);

    char name[48];
    std::snprintf(name, sizeof(name), "terrain_%d_%d", c.cx, c.cy);
    return w.add_mesh(json{
        {"name", name},
        {"primitives", json::array({json{
            {"attributes", {{"POSITION", aPos}, {"NORMAL", aNrm}, {"TEXCOORD_0", aUv}}},
            {"indices", aIdx},
            {"material", material},
        }})},
    });
}

// One glTF mesh for one hull, positions only (hull-local).
int write_collision_mesh(GltfWriter& w, const castlemist::world::CollisionMesh& m, size_t index, int material) {
    const size_t vcount = m.verts.size() / 3;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < vcount; ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], m.verts[i * 3 + k]);
            hi[k] = std::max(hi[k], m.verts[i * 3 + k]);
        }
    json mn = {lo[0], lo[1], lo[2]}, mx = {hi[0], hi[1], hi[2]};
    int aPos = w.add_accessor(m.verts.data(), vcount * 12, 5126, "VEC3", vcount, 34962, &mn, &mx);
    int aIdx = w.add_accessor(m.indices.data(), m.indices.size() * 4, 5125, "SCALAR", m.indices.size(), 34963);
    return w.add_mesh(json{
        {"name", "collision_" + std::to_string(index)},
        {"primitives", json::array({json{{"attributes", {{"POSITION", aPos}}}, {"indices", aIdx}, {"material", material}}})},
    });
}

} // namespace

WorldExportReport export_world_gltf(const WorldScene& scene, const std::vector<std::shared_ptr<ModelPreview>>& models,
                                    const std::string& glbPath, const WorldExportOptions& opts) {
    WorldExportReport rep;
    rep.glbPath = glbPath;

    GltfWriter w;
    json rootChildren = json::array();
    auto report = [&](const char* stage, size_t done, size_t total) {
        if (opts.progress) opts.progress(stage, done, total);
    };
    // About every 5%, and always the last item.
    auto tick = [&](const char* stage, size_t done, size_t total) {
        const size_t step = std::max<size_t>(1, total / 20);
        if (done % step == 0 || done == total) report(stage, done, total);
    };

    // ---- terrain ----
    if (opts.terrain && scene.terrain.present && !scene.terrain.chunks.empty()) {
        float extent[4] = {1e30f, 1e30f, -1e30f, -1e30f};
        for (const auto& c : scene.terrain.chunks) {
            extent[0] = std::min(extent[0], c.rect[0]); extent[1] = std::min(extent[1], c.rect[1]);
            extent[2] = std::max(extent[2], c.rect[2]); extent[3] = std::max(extent[3], c.rect[3]);
        }
        int mat = w.add_material(json{
            {"name", "Terrain"},
            {"pbrMetallicRoughness", {{"baseColorFactor", {0.42, 0.40, 0.34, 1.0}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
        });
        json children = json::array();
        const size_t total = scene.terrain.chunks.size();
        size_t done = 0;
        for (const auto& c : scene.terrain.chunks) {
            tick("terrain", ++done, total);
            if (c.samples < 2 || c.heights.size() != static_cast<size_t>(c.samples) * c.samples) {
                rep.warnings.push_back("terrain chunk " + std::to_string(c.cx) + "," + std::to_string(c.cy) +
                                       " has no usable samples; skipped");
                continue;
            }
            int mesh = write_terrain_chunk(w, scene.terrain, c, extent, mat);
            children.push_back(w.add_node(json{{"name", "chunk_" + std::to_string(c.cx) + "_" + std::to_string(c.cy)}, {"mesh", mesh}}));
            ++rep.terrainChunks;
        }
        if (!children.empty()) rootChildren.push_back(w.add_node(json{{"name", "Terrain"}, {"children", children}}));
    } else if (opts.terrain) {
        rep.warnings.push_back("terrain: the map has none; not written");
    }

    // ---- props ----
    if (opts.props) {
        // Mesh/material/texture data is written once per model, on first use,
        // and shared by every placement through glTF node instancing.
        std::vector<std::vector<MeshExportInfo>> meshCache(scene.models.size());
        std::vector<bool> written(scene.models.size(), false);
        json children = json::array();
        for (size_t pi = 0; pi < scene.props.size(); ++pi) {
            tick("props", pi + 1, scene.props.size());
            const auto& inst = scene.props[pi];
            const ModelPreview* model = inst.model < models.size() ? models[inst.model].get() : nullptr;
            if (!model || model->meshes.empty() || inst.model >= scene.models.size()) { ++rep.propsSkipped; continue; }
            if (!written[inst.model]) {
                std::vector<int> texIndices;
                if (opts.textures) write_model_textures(w, model->textures, texIndices);
                else texIndices.assign(model->textures.size(), -1);
                std::vector<int> matIndices = write_materials(w, *model, texIndices);
                meshCache[inst.model] = write_meshes(w, *model, matIndices);
                written[inst.model] = true;
                ++rep.modelsWritten;
            }
            const auto& meshes = meshCache[inst.model];

            char name[48];
            std::snprintf(name, sizeof(name), "prop%zu_%u", pi, scene.models[inst.model].fileId);

            // A skinned prop gets its own skeleton and skin per placement, so
            // placements never share a pose; its mesh data is still shared.
            std::vector<JointExportInfo> joints;
            int skin = -1;
            if (!model->joints.empty()) {
                joints = write_skeleton(w, *model, name);
                skin = write_skin(w, *model, joints);
                if (opts.animations) write_animations(w, *model, joints, opts.animFps);
            }

            json kids = json::array();
            for (const MeshExportInfo& m : meshes) {
                json node{{"mesh", m.meshIndex}};
                if (skin >= 0 && model->meshes[m.sourceIndex].hasSkin) node["skin"] = skin;
                kids.push_back(w.add_node(std::move(node)));
            }
            for (size_t ji = 0; ji < model->joints.size(); ++ji)
                if (model->joints[ji].parent < 0) kids.push_back(joints[ji].nodeIndex);

            children.push_back(w.add_node(json{{"name", name}, {"matrix", matrix_json(inst.world)}, {"children", kids}}));
            ++rep.propsWritten;
        }
        if (!children.empty()) rootChildren.push_back(w.add_node(json{{"name", "Props"}, {"children", children}}));
        if (rep.propsSkipped)
            rep.warnings.push_back("props: " + std::to_string(rep.propsSkipped) +
                                   " placements skipped (model not loaded or has no mesh)");
    }

    // ---- collision ----
    if (opts.collision && !scene.collision.instances.empty()) {
        int mat = w.add_material(json{
            {"name", "Collision"},
            {"pbrMetallicRoughness", {{"baseColorFactor", {0.9, 0.25, 0.55, 0.35}}, {"metallicFactor", 0.0}, {"roughnessFactor", 1.0}}},
            {"alphaMode", "BLEND"},
            {"doubleSided", true},  // hull winding is not proven against the client's culling
        });
        std::vector<int> meshOf(scene.collision.meshes.size(), -1);
        json children = json::array();
        for (size_t ci = 0; ci < scene.collision.instances.size(); ++ci) {
            tick("collision", ci + 1, scene.collision.instances.size());
            const auto& inst = scene.collision.instances[ci];
            if (inst.mesh >= scene.collision.meshes.size()) continue;
            const auto& m = scene.collision.meshes[inst.mesh];
            if (m.verts.empty() || m.indices.empty()) continue;
            if (meshOf[inst.mesh] < 0) meshOf[inst.mesh] = write_collision_mesh(w, m, inst.mesh, mat);
            children.push_back(w.add_node(json{{"name", inst.group + "_" + std::to_string(ci)},
                                               {"mesh", meshOf[inst.mesh]},
                                               {"matrix", matrix_json(inst.world)}}));
            ++rep.collisionInstances;
        }
        if (!children.empty()) rootChildren.push_back(w.add_node(json{{"name", "Collision"}, {"children", children}}));
    }

    if (rootChildren.empty()) {
        rep.error = "Nothing to export: the map has no terrain, props or collision that could be written.";
        return rep;
    }

    const std::array<float, 16> root = map_to_gltf_root(opts.rootScale);
    w.add_scene_root(w.add_node(json{
        {"name", "GW2_MapToGltf"},
        {"matrix", std::vector<float>(root.begin(), root.end())},
        {"children", rootChildren},
    }));

    fs::path outPath = fs::path(std::u8string(glbPath.begin(), glbPath.end()));  // glbPath is UTF-8
    std::error_code ec;
    if (outPath.has_parent_path()) fs::create_directories(outPath.parent_path(), ec);

    std::vector<uint8_t> glb = w.finish();
    report("save", glb.size(), glb.size());
    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        rep.error = "Could not open '" + glbPath + "' for writing.";
        return rep;
    }
    out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    if (!out) {
        rep.error = "Failed writing '" + glbPath + "'.";
        return rep;
    }
    rep.ok = true;
    return rep;
}

} // namespace castlemist::exportgltf
