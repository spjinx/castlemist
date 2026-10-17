/// @file
/// @brief The world export's `unityTerrain` tiles (world_unity_terrain.h;
///        world-foliage spec D7, docs/research/gw2-terrain-to-unity.md §5).
#include "castlemist/exportgltf/world_unity_terrain.h"

#include "castlemist/world/terrain.h"
#include "castlemist/world/zone_scatter.h"

#include "internal.h"
#include "long_path.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace castlemist::exportgltf {

namespace {

namespace fs = std::filesystem;
namespace cw = castlemist::world;
using nlohmann::json;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

bool write_file(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(long_path(p), std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    f.close();   // the final flush can fail (disk full): check after it
    return !f.fail();
}

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(uint8_t(v >> (8 * k)));
}
void put_f32(std::vector<uint8_t>& b, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put_u32(b, v);
}

/// A map-wide detail or tree prototype: one models[] position.
struct Proto {
    uint32_t model = 0;
    bool anyFade = false; float fadeFar = 0;
    bool anyScale = false; float minScale = 0, maxScale = 0;
    uint32_t index = 0;   ///< position in detailPrototypes / treePrototypes
};

enum class Kind { None, Detail, Tree };

/// The LCG of the check points (world_unity_terrain.h): a fraction in [0, 1).
struct Lcg {
    uint64_t state;
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return double(state >> 11) * (1.0 / 9007199254740992.0);
    }
};

bool pow2_plus_one(int n) { return n >= 3 && ((n - 1) & (n - 2)) == 0; }

} // namespace

std::vector<ManifestSetting> unity_terrain_settings() {
    const std::string kU = "UNPROVEN", kC = "castlemist's choice";
    const std::string F = "docs/superpowers/specs/2026-10-16-world-foliage-design.md ";
    const std::string R = "docs/research/gw2-terrain-to-unity.md \xc2\xa7";
    return {
        {"terrainTreeMaxTiltDeg", 5, kC, F + "D7"},
        {"terrainTreeYawSign", "unityYaw = yaw of unity.r about +Y", kU,
         F + "D11; until the checklist compares a Terrain tree with its GameObject"},
        {"detailPlacement", "count per 48-unit cell, Unity positions", kC, F + "D8"},
        {"terrainHeightRowOrder", "row 0 south", kU, R + "5.2; until the terrain frame check passes"},
        {"terrainColliderDiagonal", "Unity's", kU,
         R + "5.5: Unity's TerrainCollider split against CollisionIndex's NW-SE (the mesh export and the Map "
             "Window); the client's scatter z uses NE-SW (" + R + "1.3, 2026-10-16). The .r16 heights are the vertex "
             "heights and assume no split"},
    };
}

json write_unity_terrain(const UnityTerrainInput& in, const std::string& dirUtf8, std::vector<std::string>& warnings,
                         std::string* error) {
    if (!in.scene || !in.plan || !in.records || !in.models) return nullptr;
    const cw::WorldScene& s = *in.scene;
    const cw::Terrain& t = s.terrain;
    const cw::CellPlan& plan = *in.plan;
    const std::vector<ManifestModel>& models = *in.models;
    if (!t.present || t.chunksX <= 0 || t.chunksY <= 0 || t.chunks.size() != size_t(t.chunksX) * t.chunksY)
        return nullptr;
    const int bc = plan.blockChunks;
    const int T = in.texelsPerChunk;
    if (bc < 1 || T < 1) return nullptr;

    // The tiles: one per block holding a kept cell (cells never cross a block).
    std::map<std::pair<int, int>, std::vector<size_t>> blocks;   // (cy0, cx0) -> plan.cells indices
    for (size_t k = 0; k < plan.cells.size(); ++k) {
        const cw::Cell& c = plan.cells[k];
        if (c.all || c.w <= 0 || c.h <= 0) continue;
        blocks[{(c.cy0 / bc) * bc, (c.cx0 / bc) * bc}].push_back(k);
    }
    if (blocks.empty()) return nullptr;

    int seg = 0;
    for (const cw::TerrainChunk& c : t.chunks)
        if (c.samples >= 2 && c.heights.size() == size_t(c.samples) * c.samples) { seg = c.samples - 1; break; }
    if (seg < 1) return nullptr;
    const int n = bc * seg + 1;
    const cw::TerrainChunk& nw = t.chunks.front();
    const float cw_ = nw.rect[2] - nw.rect[0], ch_ = nw.rect[3] - nw.rect[1];
    const double tileW = double(bc) * cw_, tileH = double(bc) * ch_;
    const int detailRes = int(std::lround(tileW / kDetailCellUnits));
    if (!pow2_plus_one(n) || n > 4097 || !(cw_ > 0) || std::fabs(cw_ - ch_) > 1e-3f * cw_ ||
        std::fabs(detailRes * double(kDetailCellUnits) - tileW) > 1e-3 || detailRes < 1) {
        warnings.push_back("unityTerrain: a " + std::to_string(bc) + " x " + std::to_string(bc) + "-chunk tile has " +
                           std::to_string(n) + " samples a side and chunks " + std::to_string(cw_) + " x " +
                           std::to_string(ch_) + " units, which a Unity Terrain cannot take (2^k + 1 square samples, "
                           "48-unit detail cells); unityTerrain left out");
        return nullptr;
    }

    float hMin = 0, hMax = 0;
    cw::terrain_up_range(t, hMin, hMax);
    if (!(hMax - hMin >= 1e-3f)) hMax = hMin + 1.0f;   // a flat map: a 1-unit range keeps the normalisation finite
    const double range = double(hMax) - double(hMin);

    // Pass 1: classify every record of the kept tiles; set bit 3 on the Terrain trees.
    const auto& layers = s.zones.layers;
    std::map<uint32_t, Proto> details, trees;
    std::vector<std::vector<Kind>> kinds(plan.cells.size());
    auto tile_rect_real = [&](int cx0, int cy0, double r[4]) {   // x0, y0, x1, y1 of the real chunks
        const int cx1 = std::min(cx0 + bc, t.chunksX), cy1 = std::min(cy0 + bc, t.chunksY);
        r[0] = double(nw.rect[0]) + double(cx0) * cw_;
        r[3] = double(nw.rect[3]) - double(cy0) * ch_;
        r[2] = r[0] + double(cx1 - cx0) * cw_;
        r[1] = r[3] - double(cy1 - cy0) * ch_;
    };
    auto note = [](Proto& p, uint32_t model, const cw::ZoneLayer& L, float scale) {
        p.model = model;
        if (std::isfinite(L.fadeRange[1])) {
            p.fadeFar = p.anyFade ? std::max(p.fadeFar, L.fadeRange[1]) : L.fadeRange[1];
            p.anyFade = true;
        }
        if (std::isfinite(scale)) {
            p.minScale = p.anyScale ? std::min(p.minScale, scale) : scale;
            p.maxScale = p.anyScale ? std::max(p.maxScale, scale) : scale;
            p.anyScale = true;
        }
    };
    for (const auto& [key, cellIdx] : blocks) {
        double rr[4];
        tile_rect_real(key.second, key.first, rr);
        for (size_t k : cellIdx) {
            if (k >= in.records->size()) continue;
            std::vector<FoliageRecord>& recs = (*in.records)[k];
            kinds[k].assign(recs.size(), Kind::None);
            for (size_t i = 0; i < recs.size(); ++i) {
                FoliageRecord& r = recs[i];
                if (r.flags & kFoliageFlagOutsideTerrain) continue;
                if (r.layer == 0xFFFF || r.layer >= layers.size()) continue;   // no layer type
                if (r.model >= models.size() || !models[r.model].ok) continue;
                const double x = r.m[9], y = r.m[10];
                if (!(x >= rr[0] && x <= rr[2] && y >= rr[1] && y <= rr[3])) continue;
                const cw::ZoneLayer& L = layers[r.layer];
                const ManifestModel& mm = models[r.model];
                if ((L.type == 1 || L.type == 2) && mm.meshes == 1 && mm.materialCount == 1) {
                    kinds[k][i] = Kind::Detail;
                    note(details[r.model], r.model, L, r.unity.s);
                } else if (L.type == 3 && r.tiltDeg <= kTerrainTreeMaxTiltDeg && std::isfinite(r.m[11])) {
                    kinds[k][i] = Kind::Tree;
                    r.flags = uint16_t(r.flags | kFoliageFlagTerrainTree);
                    note(trees[r.model], r.model, L, r.unity.s);
                }
            }
        }
    }
    json detailProtos = json::array(), treeProtos = json::array();
    {
        uint32_t k = 0;
        for (auto& [m, p] : details) {
            p.index = k++;
            detailProtos.push_back({{"model", m},
                                    {"minScale", p.anyScale ? json(p.minScale) : json(nullptr)},
                                    {"maxScale", p.anyScale ? json(p.maxScale) : json(nullptr)},
                                    {"fadeFar", p.anyFade ? json(p.fadeFar) : json(nullptr)}});
        }
        k = 0;
        for (auto& [m, p] : trees) {
            p.index = k++;
            treeProtos.push_back({{"model", m}, {"fadeFar", p.anyFade ? json(p.fadeFar) : json(nullptr)}});
        }
    }

    const fs::path dir = u8path(dirUtf8);
    const fs::path tdir = dir / "terrain";
    std::error_code ec;
    make_dirs(tdir, ec);
    auto fail = [&](const std::string& e) -> json {
        if (error) *error = e;
        return nullptr;
    };
    if (ec) return fail("cannot create " + utf8(tdir) + ": " + ec.message());
    auto write = [&](const std::string& name, const std::vector<uint8_t>& b) {
        return !b.empty() && write_file(tdir / name, b);
    };

    json tiles = json::array();
    std::vector<std::array<double, 4>> realRects;
    std::vector<double> spreads;   // per real quad: the two diagonals' height difference at its centre
    size_t treesOutsideRange = 0;
    const int q = n - 1;
    for (const auto& [key, cellIdx] : blocks) {
        const int cy0 = key.first, cx0 = key.second;
        const std::string id = "t" + std::to_string(cx0) + "_" + std::to_string(cy0);
        const cw::TerrainHeightfield hf = cw::terrain_heightfield(t, cx0, cy0, bc);
        if (hf.n != n || hf.up.size() != size_t(n) * n || hf.real.size() != size_t(q) * q)
            return fail("unityTerrain: " + id + ": heightfield of the wrong size");
        double rr[4];
        tile_rect_real(cx0, cy0, rr);
        realRects.push_back({rr[0], rr[1], rr[2], rr[3]});
        const double X0 = hf.rect[0], Y0 = hf.rect[1], Y1 = hf.rect[3];

        // Heights.
        std::vector<uint8_t> r16;
        r16.reserve(size_t(n) * n * 2);
        for (float up : hf.up) {
            const double v = std::clamp((double(up) - hMin) / range, 0.0, 1.0);
            const uint32_t u = uint32_t(std::lround(v * 65535.0));
            r16.push_back(uint8_t(u & 0xFF));
            r16.push_back(uint8_t(u >> 8));
        }
        if (!write(id + ".r16", r16)) return fail("cannot write " + utf8(tdir / (id + ".r16")));
        // The two diagonals' height difference at a quad centre, |h00 + h11 - h01 - h10| / 2 (the diagonal note).
        for (int r = 0; r < q; ++r)
            for (int c = 0; c < q; ++c) {
                if (!hf.real[size_t(r) * q + c]) continue;
                const double a = hf.up[size_t(r) * n + c], b = hf.up[size_t(r) * n + c + 1];
                const double d = hf.up[size_t(r + 1) * n + c], e = hf.up[size_t(r + 1) * n + c + 1];
                spreads.push_back(std::fabs(a + e - b - d) * 0.5);
            }

        // Holes (edge tiles only): row 0 north.
        json holes = nullptr;
        if (std::find(hf.real.begin(), hf.real.end(), uint8_t(0)) != hf.real.end()) {
            std::vector<uint8_t> px(size_t(q) * q);
            for (int y = 0; y < q; ++y)
                for (int x = 0; x < q; ++x) px[size_t(y) * q + x] = hf.real[size_t(q - 1 - y) * q + x] ? 255 : 0;
            const std::string name = id + "_holes.png";
            if (!write(name, encode_png_pixels(q, q, 1, px))) return fail("cannot write " + utf8(tdir / name));
            holes = "terrain/" + name;
        }

        // Albedo: the block's cells, pixel-exact; padded chunks mid-grey (the bake's default).
        const int A = bc * T;
        std::vector<uint8_t> rgb(size_t(A) * A * 3, 128);
        for (size_t k : cellIdx) {
            if (!in.cellAlbedoRgb || k >= in.cellAlbedoRgb->size()) continue;
            const cw::Cell& c = plan.cells[k];
            const std::vector<uint8_t>& src = (*in.cellAlbedoRgb)[k];
            const size_t W = size_t(c.w) * T, H = size_t(c.h) * T;
            if (src.size() != W * H * 3) continue;
            const size_t ox = size_t(c.cx0 - cx0) * T, oy = size_t(c.cy0 - cy0) * T;
            for (size_t j = 0; j < H; ++j)
                std::copy_n(src.begin() + ptrdiff_t(j * W * 3), W * 3, rgb.begin() + ptrdiff_t(((oy + j) * A + ox) * 3));
        }
        const std::string albedo = id + "_albedo.png";
        if (!write(albedo, encode_png_pixels(A, A, 3, rgb))) return fail("cannot write " + utf8(tdir / albedo));

        // Details and trees.
        std::map<uint32_t, std::pair<std::vector<uint32_t>, uint64_t>> counts;   // prototype -> (cells, records)
        std::vector<uint8_t> treeBytes;
        size_t treeRecords = 0;
        for (size_t k : cellIdx) {
            if (k >= in.records->size()) continue;
            const std::vector<FoliageRecord>& recs = (*in.records)[k];
            for (size_t i = 0; i < recs.size() && i < kinds[k].size(); ++i) {
                const FoliageRecord& r = recs[i];
                const double x = r.m[9], y = r.m[10];
                if (kinds[k][i] == Kind::Detail) {
                    auto& [cells, nrec] = counts[details[r.model].index];
                    if (cells.empty()) cells.assign(size_t(detailRes) * detailRes, 0);
                    const int dx = std::clamp(int(std::floor((x - X0) / kDetailCellUnits)), 0, detailRes - 1);
                    const int dy = std::clamp(int(std::floor((Y1 - y) / kDetailCellUnits)), 0, detailRes - 1);
                    ++cells[size_t(dy) * detailRes + dx];
                    ++nrec;
                } else if (kinds[k][i] == Kind::Tree) {
                    const double h = (-double(r.m[11]) - hMin) / range;
                    if (!(h >= 0.0 && h <= 1.0)) ++treesOutsideRange;
                    put_u32(treeBytes, trees[r.model].index);
                    put_f32(treeBytes, float((x - X0) / tileW));
                    put_f32(treeBytes, float((y - Y0) / tileH));
                    put_f32(treeBytes, float(h));
                    put_f32(treeBytes, r.unityYaw);
                    put_f32(treeBytes, r.unity.s);
                    ++treeRecords;
                }
            }
        }
        json det = json::array();
        for (auto& [proto, cr] : counts) {
            const std::vector<uint32_t>& cells = cr.first;
            std::vector<uint8_t> px(cells.size());
            size_t over = 0;
            uint32_t most = 0;
            for (size_t i = 0; i < cells.size(); ++i) {
                most = std::max(most, cells[i]);
                if (cells[i] > 255) ++over;
                px[i] = uint8_t(std::min<uint32_t>(cells[i], 255));
            }
            const std::string name = id + "_d" + std::to_string(proto) + ".png";
            if (over)
                warnings.push_back("unityTerrain: " + id + " detail prototype " + std::to_string(proto) + ": " +
                                   std::to_string(over) + " of the 48-unit cells hold more than 255 records (at most " +
                                   std::to_string(most) + "); clamped to 255 in terrain/" + name);
            if (!write(name, encode_png_pixels(detailRes, detailRes, 1, px))) return fail("cannot write " + utf8(tdir / name));
            det.push_back({{"prototype", proto}, {"counts", "terrain/" + name}, {"records", cr.second}});
        }
        json treesJson = nullptr;
        if (treeRecords) {
            const std::string name = id + "_trees.bin";
            if (!write(name, treeBytes)) return fail("cannot write " + utf8(tdir / name));
            treesJson = {{"file", "terrain/" + name}, {"records", treeRecords}};
        }
        const int cx1 = std::min(cx0 + bc, t.chunksX) - 1, cy1 = std::min(cy0 + bc, t.chunksY) - 1;
        tiles.push_back({{"id", id},
                         {"chunks", {cx0, cy0, cx1, cy1}},
                         {"position", {X0, double(hMin), Y0}},   // Unity axes (x, up, z = map y), before unityScale
                         {"size", {tileW, range, tileH}},
                         {"heights", "terrain/" + id + ".r16"},
                         {"holes", holes},
                         {"albedo", "terrain/" + albedo},
                         {"albedoSize", {A, A}},
                         {"details", det},
                         {"trees", treesJson}});
    }

    // The terrain frame check's points: uniform in the tiles' real area.
    json check = json::array();
    Lcg lcg{uint64_t(s.mapFileId)};
    for (int tries = 0; int(check.size()) < kUnityTerrainCheckPoints && tries < 32 * kUnityTerrainCheckPoints; ++tries) {
        const size_t ti = std::min(realRects.size() - 1, size_t(lcg.next() * double(realRects.size())));
        const auto& r = realRects[ti];
        const float x = float(r[0] + lcg.next() * (r[2] - r[0]));
        const float y = float(r[1] + lcg.next() * (r[3] - r[1]));
        bool inside = false;
        const float z = cw::terrain_height_at(t, x, y, &inside);
        if (!inside || !std::isfinite(z)) continue;
        check.push_back({{"unity", {x, y}}, {"height", -z}});
    }

    // The diagonal note: |h00 + h11 - h01 - h10| / 2 over the tiles' real quads (p50, p99, max).
    auto pct = [&](double f) {
        if (spreads.empty()) return 0.0;
        const size_t k = std::min(spreads.size() - 1, size_t(f * double(spreads.size() - 1) + 0.5));
        std::nth_element(spreads.begin(), spreads.begin() + ptrdiff_t(k), spreads.end());
        return spreads[k];
    };
    auto units = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", v);
        return std::string(b);
    };
    const double p50 = pct(0.5), p99 = pct(0.99), pmax = pct(1.0);
    warnings.push_back("unityTerrain: the .r16 heights are vertex samples (no triangle split); Unity's TerrainCollider "
                       "triangulates them its own way, the mesh terrain splits NW-SE (CollisionIndex) and the client's "
                       "scatter z uses NE-SW. At a quad centre the two splits differ by |h00 + h11 - h01 - h10| / 2: p50 " +
                       units(p50) + ", p99 " + units(p99) + ", max " + units(pmax) +
                       " map units over the tiles' quads (terrainColliderDiagonal UNPROVEN)");
    if (treesOutsideRange)
        warnings.push_back("unityTerrain: " + std::to_string(treesOutsideRange) +
                           " Terrain trees stand outside the terrain's height range (h outside 0..1 in their trees.bin)");

    return {{"tileChunks", bc},
            {"heightmapResolution", n},
            {"detailResolution", detailRes},
            {"detailCellUnits", int(kDetailCellUnits)},
            {"heightRange", {{"min", hMin}, {"max", hMax}}},
            {"fadeQualityScale", cw::zone_fade_quality_scale(cw::zone_scatter_rule_in_force().graphicsQuality)},
            {"tiles", tiles},
            {"detailPrototypes", detailProtos},
            {"treePrototypes", treeProtos},
            {"check", check}};
}

} // namespace castlemist::exportgltf
