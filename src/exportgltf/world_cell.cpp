/// @file
/// @brief One export cell -> one .glb (world_cell.h; spec
///        docs/superpowers/specs/2026-10-15-world-export-design.md D1, D3, D4).

#include "castlemist/exportgltf/world_cell.h"

#include "castlemist/exportgltf/world_export.h"
#include "castlemist/world/collision.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain_bake.h"
#include "castlemist/world/terrain_mesh.h"

#include "internal.h"
#include "long_path.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;
namespace cw = castlemist::world;

namespace {

bool usable(const cw::TerrainChunk* k) {
    return k && k->samples >= 2 && k->heights.size() == size_t(k->samples) * size_t(k->samples);
}

/// Samples per chunk side shared by the cell's chunks (the first usable one's), or 0.
int cell_samples(const cw::Terrain& t, const cw::Cell& c) {
    if (c.all || c.w <= 0 || c.h <= 0) return 0;
    for (int cy = c.cy0; cy < c.cy0 + c.h; ++cy)
        for (int cx = c.cx0; cx < c.cx0 + c.w; ++cx) {
            const cw::TerrainChunk* k = cw::find_chunk(t, cx, cy);
            if (usable(k)) return k->samples;
        }
    return 0;
}

/// Two triangles per quad of a W-wide row-major grid block, a..d the quad's
/// north-west, north-east, south-west and south-east vertices, split along
/// b-c (NE-SW) as CollisionIndex::build splits a terrain quad (walk.cpp
/// add_terrain), so the exported terrain is the surface castlemist walks on.
/// NE-SW is the client's ground query (zone_scatter.cpp Ground::height:
/// 4535 / 4535 collider z; final review I2); the renderer's own index buffer
/// is UNPROVEN. (a, b, c) and (b, d, c) have their cross products along -Z (up).
void push_quad(std::vector<uint32_t>& idx, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    idx.insert(idx.end(), {a, b, c, b, d, c});
}

} // namespace

CellMesh build_cell_terrain(const cw::Terrain& t, const cw::Cell& c, std::vector<std::string>* warnings) {
    CellMesh m;
    const int n = cell_samples(t, c);
    if (n < 2) return m;
    const int seg = n - 1;
    const int W = c.w * seg + 1, H = c.h * seg + 1;

    // The chunks of the cell (row-major from the north-west), null where missing.
    struct Slot { const cw::TerrainChunk* k = nullptr; std::vector<float> nrm; };
    std::vector<Slot> slots(size_t(c.w) * size_t(c.h));
    for (int ly = 0; ly < c.h; ++ly)
        for (int lx = 0; lx < c.w; ++lx) {
            const cw::TerrainChunk* k = cw::find_chunk(t, c.cx0 + lx, c.cy0 + ly);
            if (!k) continue;
            if (!usable(k) || k->samples != n) {
                if (warnings) {
                    const std::string at = "cell " + c.id + ": terrain chunk (" + std::to_string(k->cx) + ", " +
                                           std::to_string(k->cy) + ") ";
                    warnings->push_back(usable(k) ? at + "has " + std::to_string(k->samples) +
                                                        " samples per side, not the cell's " + std::to_string(n) +
                                                        "; left out"
                                                  : at + "has no usable samples; left out");
                }
                continue;
            }
            Slot& s = slots[size_t(ly) * c.w + lx];
            s.k = k;
            s.nrm = cw::chunk_normals(t, size_t(k - t.chunks.data()));
        }

    // Owner of each grid sample: the first chunk (row-major) that has it. Shared
    // edge samples are bit-identical (gw2-world-frame.md §3.1), so either owner gives the same.
    std::vector<int32_t> owner(size_t(W) * size_t(H), -1);
    for (size_t si = 0; si < slots.size(); ++si) {
        if (!slots[si].k) continue;
        const int lx = int(si % size_t(c.w)), ly = int(si / size_t(c.w));
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                int32_t& o = owner[size_t(ly * seg + j) * W + size_t(lx * seg + i)];
                if (o < 0) o = int32_t(si);
            }
    }

    // One vertex per owned sample, row-major over the cell grid.
    std::vector<uint32_t> vert(owner.size(), UINT32_MAX);
    for (int gj = 0; gj < H; ++gj)
        for (int gi = 0; gi < W; ++gi) {
            const int32_t o = owner[size_t(gj) * W + gi];
            if (o < 0) continue;
            const Slot& s = slots[size_t(o)];
            const cw::TerrainChunk& k = *s.k;
            const int lx = o % c.w, ly = o / c.w;
            const int i = gi - lx * seg, j = gj - ly * seg;
            const size_t si = size_t(j) * n + size_t(i);
            const double dx = (double(k.rect[2]) - k.rect[0]) / seg, dy = (double(k.rect[3]) - k.rect[1]) / seg;
            vert[size_t(gj) * W + gi] = uint32_t(m.vertices());
            m.pos.push_back(float(double(k.rect[0]) + i * dx - c.origin[0]));
            m.pos.push_back(float(double(k.rect[3]) - j * dy - c.origin[1]));
            m.pos.push_back(float(double(k.heights[si]) - c.origin[2]));
            if (s.nrm.size() == size_t(n) * n * 3) m.nrm.insert(m.nrm.end(), &s.nrm[si * 3], &s.nrm[si * 3] + 3);
            else m.nrm.insert(m.nrm.end(), {0.f, 0.f, -1.f});
            m.uv.push_back(float(gi) / float(W - 1));
            m.uv.push_back(float(gj) / float(H - 1));
        }

    // Each present chunk's quads.
    for (size_t si = 0; si < slots.size(); ++si) {
        if (!slots[si].k) continue;
        const int gx = int(si % size_t(c.w)) * seg, gy = int(si / size_t(c.w)) * seg;
        for (int j = 0; j < seg; ++j)
            for (int i = 0; i < seg; ++i) {
                const size_t a = size_t(gy + j) * W + size_t(gx + i);
                push_quad(m.idx, vert[a], vert[a + 1], vert[a + W], vert[a + W + 1]);
            }
    }
    return m;
}

CellMesh build_cell_water(const cw::WorldScene& s, const cw::Cell& c, int waterTexelsPerChunk) {
    CellMesh m;
    const int n = cell_samples(s.terrain, c);
    if (n < 2 || !s.water.hasPlane) return m;
    if (!cw::water_below_plane(s, c.rect, c.w * waterTexelsPerChunk, c.h * waterTexelsPerChunk)) return m;
    const int seg = n - 1;
    const int W = c.w * seg + 1, H = c.h * seg + 1;
    const double dx = (double(c.rect[2]) - c.rect[0]) / (W - 1), dy = (double(c.rect[3]) - c.rect[1]) / (H - 1);
    const float z = float(double(s.water.planeZ) - c.origin[2]);
    for (int gj = 0; gj < H; ++gj)
        for (int gi = 0; gi < W; ++gi) {
            m.pos.push_back(float(double(c.rect[0]) + gi * dx - c.origin[0]));
            m.pos.push_back(float(double(c.rect[3]) - gj * dy - c.origin[1]));
            m.pos.push_back(z);
            m.nrm.insert(m.nrm.end(), {0.f, 0.f, -1.f});
            m.uv.push_back(float(gi) / float(W - 1));
            m.uv.push_back(float(gj) / float(H - 1));
        }
    for (int gj = 0; gj + 1 < H; ++gj)
        for (int gi = 0; gi + 1 < W; ++gi) {
            const uint32_t a = uint32_t(gj * W + gi);
            push_quad(m.idx, a, a + 1, a + uint32_t(W), a + uint32_t(W) + 1);
        }
    return m;
}

std::vector<CellMesh> build_cell_colliders(const cw::WorldScene& s, const cw::Cell& c, size_t maxVertices) {
    std::vector<float> tris;
    for (uint32_t i : c.collision) cw::collision_instance_triangles(s, i, tris);
    const size_t perPart = std::max<size_t>(1, maxVertices / 3);
    const size_t count = tris.size() / 9;
    std::vector<CellMesh> parts;
    for (size_t first = 0; first < count; first += perPart) {
        const size_t last = std::min(count, first + perPart);
        CellMesh p;
        p.pos.reserve((last - first) * 9);
        for (size_t f = first * 9; f < last * 9; f += 3)
            for (int k = 0; k < 3; ++k) p.pos.push_back(float(double(tris[f + k]) - c.origin[k]));
        p.idx.resize((last - first) * 3);
        for (size_t v = 0; v < p.idx.size(); ++v) p.idx[v] = uint32_t(v);
        parts.push_back(std::move(p));
    }
    return parts;
}

namespace {

/// One primitive: POSITION (with min/max), NORMAL and TEXCOORD_0 when present;
/// 16-bit indices when every vertex is addressable by one.
int write_cell_mesh(GltfWriter& w, const CellMesh& m, const std::string& name, int material) {
    const size_t vcount = m.vertices();
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < vcount; ++i)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], m.pos[i * 3 + k]);
            hi[k] = std::max(hi[k], m.pos[i * 3 + k]);
        }
    const json mn = {lo[0], lo[1], lo[2]}, mx = {hi[0], hi[1], hi[2]};
    json attrs = json::object();
    attrs["POSITION"] = w.add_accessor(m.pos.data(), m.pos.size() * 4, 5126, "VEC3", vcount, 34962, &mn, &mx);
    if (m.nrm.size() == m.pos.size())
        attrs["NORMAL"] = w.add_accessor(m.nrm.data(), m.nrm.size() * 4, 5126, "VEC3", vcount, 34962);
    if (m.uv.size() == vcount * 2)
        attrs["TEXCOORD_0"] = w.add_accessor(m.uv.data(), m.uv.size() * 4, 5126, "VEC2", vcount, 34962);
    int aIdx;
    if (vcount <= 65535) {   // 65535 itself is never an index then (glTF forbids the type's max)
        std::vector<uint16_t> idx16(m.idx.begin(), m.idx.end());
        aIdx = w.add_accessor(idx16.data(), idx16.size() * 2, 5123, "SCALAR", idx16.size(), 34963);
    } else {
        aIdx = w.add_accessor(m.idx.data(), m.idx.size() * 4, 5125, "SCALAR", m.idx.size(), 34963);
    }
    return w.add_mesh(json{
        {"name", name},
        {"primitives", json::array({json{{"attributes", attrs}, {"indices", aIdx}, {"material", material}}})},
    });
}

json textured_pbr(int tex) {
    json pbr = {{"metallicFactor", 0.0}, {"roughnessFactor", 1.0}};
    if (tex >= 0) pbr["baseColorTexture"] = json{{"index", tex}};
    return pbr;
}

} // namespace

bool write_cell_gltf(const std::string& glbPathUtf8, const cw::Cell& c, const CellMesh& terrain, const CellMesh* water,
                     const std::vector<CellMesh>& colliders, const CellImages& images, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (error) error->clear();

    GltfWriter w;
    json children = json::array();
    const bool hasTerrain = terrain.vertices() > 0 && !terrain.idx.empty();
    const bool hasWater = water && water->vertices() > 0 && !water->idx.empty();
    // Images in a fixed order: albedo, then water.
    const int albedoTex = hasTerrain && !images.albedoUri.empty()
                              ? w.add_texture_uri(0, images.albedoUri, c.id + "_albedo") : -1;
    const int waterTex = hasWater && !images.waterUri.empty()
                             ? w.add_texture_uri(0, images.waterUri, c.id + "_water") : -1;

    if (hasTerrain) {
        const int mat = w.add_material(json{{"name", "Terrain"}, {"pbrMetallicRoughness", textured_pbr(albedoTex)}});
        children.push_back(w.add_node(json{{"name", "Terrain"}, {"mesh", write_cell_mesh(w, terrain, "Terrain", mat)}}));
    }
    if (hasWater) {
        json mat = {{"name", "Water"}, {"pbrMetallicRoughness", textured_pbr(waterTex)}, {"alphaMode", "BLEND"}};
        if (waterTex < 0) {   // no env preset, so no baked texture: the map view's flat water
            mat["pbrMetallicRoughness"]["baseColorFactor"] = {cw::kFlatWaterTint[0], cw::kFlatWaterTint[1],
                                                              cw::kFlatWaterTint[2], cw::kFlatWaterAlpha};
            mat["extras"] = {{"note", kFlatWaterNote}};
        }
        const int matIndex = w.add_material(mat);
        children.push_back(w.add_node(json{{"name", "Water"}, {"mesh", write_cell_mesh(w, *water, "Water", matIndex)}}));
    }
    int collisionMat = -1;
    for (size_t k = 0; k < colliders.size(); ++k) {
        if (colliders[k].vertices() == 0 || colliders[k].idx.empty()) continue;
        if (collisionMat < 0) {
            json pbr = textured_pbr(-1);
            pbr["baseColorFactor"] = {0.8, 0.2, 0.8, 1.0};
            // Hull winding in map space is UNPROVEN (world_export.cpp), so both sides.
            collisionMat = w.add_material(json{{"name", "Collision"}, {"pbrMetallicRoughness", pbr}, {"doubleSided", true}});
        }
        const std::string name = "Collider_" + std::to_string(k);
        children.push_back(w.add_node(json{{"name", name}, {"mesh", write_cell_mesh(w, colliders[k], name, collisionMat)}}));
    }

    json cell = {{"name", "Cell"}, {"translation", {c.origin[0], c.origin[1], c.origin[2]}}};
    if (!children.empty()) cell["children"] = children;
    const int cellNode = w.add_node(cell);
    const std::array<float, 16> root = map_to_gltf_root(1.0);
    w.add_scene_root(w.add_node(json{
        {"name", "GW2_MapToGltf"},
        {"matrix", std::vector<float>(root.begin(), root.end())},
        {"children", json::array({cellNode})},
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

} // namespace castlemist::exportgltf
