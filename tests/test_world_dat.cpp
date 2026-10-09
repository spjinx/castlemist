/// @file
/// @brief World-layer tests that need a real Gw2.dat. Every test here skips
///        rather than fails when the archive is missing; point `GW2_TEST_DAT`
///        at a different install to override the default location.

#include "test_framework.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

/// The install this project is developed against.
constexpr const char* kDefaultDat =
    R"(C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat)";

/// @brief Path to the archive under test, or an empty string when unavailable.
[[maybe_unused]] std::string dat_path() {
    const char* env = std::getenv("GW2_TEST_DAT");
    std::string path = (env && *env) ? env : kDefaultDat;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return path;
    }
    return {};
}

/// @brief Open the archive once per process; skip the test when it is missing.
[[maybe_unused]] Gw2Dat& shared_dat() {
    static Gw2Dat dat;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        std::string path = dat_path();
        if (!path.empty()) {
            try {
                load_dat_file(dat, path);
                ok = dat.mft_data_list.size() > 0;
            } catch (const std::exception&) {
                ok = false;
            }
        }
    }
    if (!ok) SKIP("no Gw2.dat (set GW2_TEST_DAT)");
    return dat;
}

/// @brief Load the struct template, trying the usual spots relative to the build tree.
///
/// `castlemist::tpl::auto_load` searches next to the exe and the working
/// directory, neither of which is the repository root when ctest runs.
/// `GW2_TEST_TEMPLATE` overrides.
[[maybe_unused]] bool ensure_template() {
    if (castlemist::tpl::get()) return true;
    if (castlemist::tpl::auto_load()) return true;

    std::string err;
    if (const char* env = std::getenv("GW2_TEST_TEMPLATE"); env && *env)
        if (castlemist::tpl::load_from_file(env, err)) return true;

    // Generated output, so it lives under dumps/ -- see tools/structs.
    for (const char* candidate : {
             "../../../dumps/packfile/gw2_packfile.json",
             "dumps/packfile/gw2_packfile.json",
             "../dumps/packfile/gw2_packfile.json",
         }) {
        if (castlemist::tpl::load_from_file(candidate, err)) return true;
    }
    return false;
}

/// @brief The decompressed packfile for a fileId. Skips when the fileId is not in this dat.
[[maybe_unused]] std::vector<uint8_t> packfile_by_file_id(uint32_t file_id) {
    Gw2Dat& dat = shared_dat();
    uint32_t base = get_by_base_id(dat, file_id);
    if (base == 0 || base > dat.mft_data_list.size()) SKIP("fileId not in this dat");
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    return e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
}

} // namespace

CM_TEST(world_dat, dat_opens) {
    Gw2Dat& dat = shared_dat();
    CHECK(dat.mft_data_list.size() > 100000);
}

// ---- terrain (docs/research/gw2-world-frame.md §3) ----

#include "castlemist/world/terrain.h"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

/// @brief A committed T3D reference, tests/world_ref/<fileId>.json.
nlohmann::json world_ref(uint32_t file_id) {
    std::string path = std::string(CM_WORLD_REF_DIR) + "/" + std::to_string(file_id) + ".json";
    std::ifstream f(path);
    if (!f) SKIP("no reference file");
    return nlohmann::json::parse(f);
}

struct MapUnderTest {
    castlemist::model::Extractor::MapTerrain raw;
    std::vector<castlemist::model::Extractor::MapProp> props;
    castlemist::world::Terrain terrain;
    std::vector<std::string> warnings;
};

/// @brief Parse a map's terrain and props, and build its Terrain.
MapUnderTest load_map_terrain(uint32_t file_id) {
    if (!ensure_template()) SKIP("no struct template");
    std::vector<uint8_t> bytes = packfile_by_file_id(file_id);
    castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
    MapUnderTest m;
    m.raw = ex.parseTerrain();
    m.props = ex.parseMapProps();
    m.terrain = castlemist::world::build_terrain(m.raw, m.warnings);
    return m;
}

constexpr uint32_t kTestMaps[] = {192711, 191000, 1151420};   // Queensdale, Lion's Arch, Spirit Vale

} // namespace

// §3.2: the chunk grid equals T3D's on every test map.
CM_TEST(world_dat, terrain_chunk_grid_matches_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        MapUnderTest m = load_map_terrain(id);
        CHECK(m.terrain.present);
        CHECK(m.warnings.empty());
        CHECK_EQ(m.terrain.chunksX, ref["chunks"][0].get<int>());
        CHECK_EQ(m.terrain.chunksY, ref["chunks"][1].get<int>());
        CHECK_EQ((int)m.terrain.chunks.size(), m.terrain.chunksX * m.terrain.chunksY);
        if (!m.terrain.chunks.empty()) CHECK_EQ(m.terrain.chunks[0].samples, ref["segments"].get<int>() + 1);
    }
}

// §3.3/§3.4: terrain_height_at agrees with T3D's sampled heights at all 256
// reference positions, within 0.01, on every test map. T3D's chunk placement
// (TerrainRenderer.ts:490-502) coincides with §3.3's rule on all three maps
// (Spirit Vale included), so the comparison holds everywhere.
CM_TEST(world_dat, terrain_heights_match_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        MapUnderTest m = load_map_terrain(id);
        int compared = 0, off = 0;
        for (const auto& row : ref["heights"]) {
            float x = row[0].get<float>(), y = row[1].get<float>();
            bool inside = false;
            float h = castlemist::world::terrain_height_at(m.terrain, x, y, &inside);
            if (row[2].is_null()) { CHECK_FALSE(inside); continue; }
            ++compared;
            CHECK(inside);
            float want = row[2].get<float>();
            if (!(std::fabs(h - want) <= 0.01f)) {
                if (off++ < 3) CHECK_NEAR(h, want, 0.01);   // show the first few
            }
        }
        CHECK_EQ(compared, 256);
        CHECK_EQ(off, 0);
    }
}

// §3.1: every pair of adjacent chunks shares its edge heights exactly.
CM_TEST(world_dat, terrain_adjacent_chunks_share_edges) {
    for (uint32_t id : kTestMaps) {
        MapUnderTest m = load_map_terrain(id);
        const auto& T = m.terrain;
        CHECK(T.present);
        size_t pairs = 0, unequal = 0;
        for (int cy = 0; cy < T.chunksY; ++cy)
            for (int cx = 0; cx < T.chunksX; ++cx) {
                const auto& a = T.chunks[(size_t)cy * T.chunksX + cx];
                const int n = a.samples;
                if (cx + 1 < T.chunksX) {   // east neighbour: a's last column == b's first
                    const auto& b = T.chunks[(size_t)cy * T.chunksX + cx + 1];
                    ++pairs;
                    for (int j = 0; j < n; ++j) unequal += a.heights[j * n + n - 1] != b.heights[j * n];
                }
                if (cy + 1 < T.chunksY) {   // south neighbour: a's last row == b's first
                    const auto& b = T.chunks[(size_t)(cy + 1) * T.chunksX + cx];
                    ++pairs;
                    for (int i = 0; i < n; ++i) unequal += a.heights[(n - 1) * n + i] != b.heights[i];
                }
            }
        CHECK_EQ(pairs, size_t(T.chunksX * (T.chunksY - 1) + (T.chunksX - 1) * T.chunksY));
        CHECK_EQ(unequal, size_t(0));
    }
}

// §3.3: props sit on the terrain where §3.3 places it better than where the
// alternatives would: T3D's other odd/even branch (the whole grid one chunk
// north or south), the grid flipped north-south, or mirrored east-west. "On"
// means |z - h| < 16 units. The bound "at least twice as many" is stated, not
// fitted: measured, the rule wins by 7.6x to 27x on every map (note §3.3),
// Spirit Vale included -- its median gap is large because most of its props
// stand on prop-built floors high above the terrain, not because the terrain
// is misplaced.
CM_TEST(world_dat, terrain_props_sit_on_terrain) {
    for (uint32_t id : kTestMaps) {
        MapUnderTest m = load_map_terrain(id);
        const auto& T = m.terrain;
        CHECK(T.present);
        if (!T.present) continue;
        const float x0 = T.chunks.front().rect[0], y1 = T.chunks.front().rect[3];
        const float x1 = T.chunks.back().rect[2], y0 = T.chunks.back().rect[1];
        const float cdy = (y1 - y0) / T.chunksY;
        auto on = [&](auto place) {
            size_t n = 0;
            for (const auto& p : m.props) {
                float x = p.pos[0], y = p.pos[1];
                place(x, y);
                bool inside = false;
                float h = castlemist::world::terrain_height_at(T, x, y, &inside);
                n += inside && std::fabs(p.pos[2] - h) < 16.0f;
            }
            return n;
        };
        size_t rule = on([](float&, float&) {});
        size_t north = on([&](float&, float& y) { y -= cdy; });
        size_t south = on([&](float&, float& y) { y += cdy; });
        size_t flipNS = on([&](float&, float& y) { y = y0 + y1 - y; });
        size_t mirrorEW = on([&](float& x, float&) { x = x0 + x1 - x; });
        std::printf("    map %u: props on terrain %zu (shifted a chunk N %zu, S %zu; flipped N-S %zu; mirrored E-W %zu) of %zu\n",
                    id, rule, north, south, flipNS, mirrorEW, m.props.size());
        CHECK(rule > 2 * north);
        CHECK(rule > 2 * south);
        CHECK(rule > 2 * flipNS);
        CHECK(rule > 2 * mirrorEW);
    }
}

// ---- terrain materials (docs/research/gw2-world-frame.md §4) ----

#include <cstring>
#include <tuple>

// §4: every chunk resolves; the first 16 chunks' colour textures equal
// T3D's; every chunk's blend pages are the PIMG pages the reference's
// pickerPage names.
//
// The reference `textures` is T3D's first half of loResMaterial.texIndexArray
// (TerrainRenderer.ts:362-363): the four colour textures, then the "blend"
// page reference, which has no filename (T3D records 0). §4.1 binds by token,
// so textureFileIds is the four colour textures; the test checks the fifth
// reference entry is that page reference (0) rather than dropping it unseen.
CM_TEST(world_dat, terrain_materials_match_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        if (!ensure_template()) SKIP("no struct template");
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
        std::vector<std::string> warnings;
        castlemist::world::Terrain T = castlemist::world::build_terrain(ex.parseTerrain(), warnings);
        auto mats = ex.parseTerrainMaterials();
        CHECK(mats.present);
        CHECK_EQ(mats.chunks.size(), T.chunks.size());
        castlemist::world::resolve_terrain_materials(T, mats, shared_dat(), *castlemist::tpl::get(), warnings);
        for (const auto& w : warnings) std::printf("    map %u warning: %s\n", id, w.c_str());

        // The terrain's paged image, read independently of resolve_terrain_materials.
        std::vector<uint8_t> pbytes = packfile_by_file_id(mats.pimgFileId);
        auto pimg = castlemist::model::Extractor(pbytes, *castlemist::tpl::get()).parsePagedImage();
        CHECK(pimg.present);
        auto page = [&](uint32_t layer, uint32_t px, uint32_t py) -> const castlemist::model::Extractor::MapPagedImage::Page* {
            for (const auto& p : pimg.strippedPages)
                if (p.layer == layer && p.coord[0] == px && p.coord[1] == py) return &p;
            return nullptr;
        };

        size_t unresolved = 0, noPage = 0, solid = 0;
        for (const auto& c : T.chunks) {
            unresolved += !c.material.resolved;
            const auto* p0 = page(0, (uint32_t)c.cx / 4, (uint32_t)c.cy / 4);
            const auto* p1 = page(1, (uint32_t)c.cx / 4, (uint32_t)c.cy / 4);
            CHECK(p0 && p1);
            if (!p0 || !p1) continue;
            // A page with no file is a solid-colour page (Spirit Vale); its colour is kept.
            for (auto [pg, fid, sc] : {std::tuple{p0, c.material.pickerFileId, c.material.pickerSolid},
                                       std::tuple{p1, c.material.picker2FileId, c.material.picker2Solid}}) {
                if (pg->fileId) { noPage += fid != pg->fileId; continue; }
                ++solid;
                noPage += fid != 0 || std::memcmp(sc, pg->solidColor, 4) != 0;
                CHECK((pg->solidColor[0] | pg->solidColor[1] | pg->solidColor[2] | pg->solidColor[3]) != 0);
            }
            CHECK_NEAR(c.material.pickerScale, 0.25, 1e-6);
            CHECK_NEAR(c.material.pickerOffset[0], (c.cx % 4) * 0.25, 1e-6);
            CHECK_NEAR(c.material.pickerOffset[1], (c.cy % 4) * 0.25, 1e-6);
        }
        std::printf("    map %u: %zu chunks, %zu unresolved, %zu chunk pages solid-colour\n", id, T.chunks.size(),
                    unresolved, solid);
        CHECK_EQ(unresolved, size_t(0));
        CHECK_EQ(noPage, size_t(0));

        const auto& rm = ref["terrainMaterials"];
        CHECK_EQ(rm.size(), size_t(16));
        for (const auto& r : rm) {
            const size_t k = r["chunk"].get<size_t>();
            if (k >= T.chunks.size()) { CHECK(k < T.chunks.size()); continue; }
            const auto& c = T.chunks[k];
            std::vector<uint32_t> want = r["textures"].get<std::vector<uint32_t>>();
            CHECK_EQ(want.size(), size_t(5));
            if (want.size() != 5) continue;
            CHECK_EQ(want[4], uint32_t(0));   // the "blend" page reference
            want.pop_back();
            CHECK(c.material.textureFileIds == want);
            // The reference page is T3D's [floor(cx/4), floor(cy/4)] for chunk k.
            const auto* p0 = page(0, r["pickerPage"][0].get<uint32_t>(), r["pickerPage"][1].get<uint32_t>());
            CHECK(p0 != nullptr);
            if (p0 && p0->fileId) CHECK_EQ(c.material.pickerFileId, p0->fileId);
        }
    }
}

// ---- props (docs/research/gw2-world-frame.md §5) ----

#include "castlemist/world/props.h"

#include <algorithm>
#include <map>

// §5: per group, the first 50 props equal T3D's (fileId, pos within 0.01,
// world within 1e-4). Compared per group because castlemist concatenates the
// groups as propArray, propAnimArray, propMetaArray, propInstanceArray and
// T3D as propArray, propAnimArray, propInstanceArray, propMetaArray.
CM_TEST(world_dat, props_match_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        MapUnderTest m = load_map_terrain(id);
        castlemist::world::WorldScene scene;
        castlemist::world::build_props(m.props, scene);
        CHECK_EQ(scene.props.size(), m.props.size());

        std::map<std::string, std::vector<const castlemist::world::PropInstance*>> mine;
        for (const auto& p : scene.props) mine[p.group].push_back(&p);
        std::map<std::string, std::vector<const nlohmann::json*>> theirs;
        for (const auto& r : ref["props"]) theirs[r["group"].get<std::string>()].push_back(&r);

        size_t compared = 0, bad = 0;
        double maxWorld = 0, maxPos = 0;
        for (const auto& [group, rows] : theirs) {
            CHECK(mine.count(group) == 1);
            if (!mine.count(group)) continue;
            const auto& mp = mine[group];
            CHECK_EQ(rows.size(), std::min<size_t>(50, ref["propCounts"][group].get<size_t>()));
            CHECK(mp.size() >= rows.size());
            for (size_t k = 0; k < rows.size() && k < mp.size(); ++k) {
                const auto& r = *rows[k];
                const auto& p = *mp[k];
                ++compared;
                bool ok = scene.models[p.model].fileId == r["fileId"].get<uint32_t>();
                for (int i = 0; i < 3; ++i) ok = ok && std::fabs(p.pos[i] - r["pos"][i].get<float>()) <= 0.01f;
                for (int i = 0; i < 16; ++i) {
                    double d = std::fabs(p.world[i] - r["world"][i].get<float>());
                    maxWorld = std::max(maxWorld, d);
                    ok = ok && d <= 1e-4;
                }
                for (int i = 0; i < 3; ++i)
                    maxPos = std::max(maxPos, (double)std::fabs(p.pos[i] - r["pos"][i].get<float>()));
                if (!ok && bad++ < 3) {
                    std::printf("    map %u %s[%zu] disagrees\n", id, group.c_str(), k);
                    for (int i = 0; i < 16; ++i)
                        std::printf("      world[%d] mine %.6f ref %.6f\n", i, p.world[i], r["world"][i].get<float>());
                }
            }
        }
        std::printf("    map %u: %zu props compared with T3D, %zu disagree (max |dworld| %.3g, max |dpos| %.3g)\n",
                    id, compared, bad, maxWorld, maxPos);
        CHECK(compared > 0);
        CHECK_EQ(bad, size_t(0));
    }
}

// §5: at least 99% of prop positions fall inside the terrain's union of chunk rects.
CM_TEST(world_dat, props_inside_terrain_rects) {
    for (uint32_t id : kTestMaps) {
        MapUnderTest m = load_map_terrain(id);
        CHECK(m.terrain.present);
        castlemist::world::WorldScene scene;
        castlemist::world::build_props(m.props, scene);
        size_t in = 0;
        for (const auto& p : scene.props) {
            for (const auto& c : m.terrain.chunks)
                if (p.pos[0] >= c.rect[0] && p.pos[0] <= c.rect[2] && p.pos[1] >= c.rect[1] && p.pos[1] <= c.rect[3]) {
                    ++in;
                    break;
                }
        }
        std::printf("    map %u: %zu of %zu props inside the terrain rects\n", id, in, scene.props.size());
        CHECK(!scene.props.empty());
        CHECK(in * 100 >= scene.props.size() * 99);
    }
}

// ---- collision (docs/research/gw2-world-frame.md §7) ----

#include "castlemist/world/collision.h"

#include <array>
#include <set>

namespace {

/// @brief Parse a map's havk chunk and build its Collision.
castlemist::world::WorldScene load_map_collision(uint32_t file_id,
                                                 castlemist::model::Extractor::MapHavok* raw = nullptr) {
    if (!ensure_template()) SKIP("no struct template");
    std::vector<uint8_t> bytes = packfile_by_file_id(file_id);
    castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
    castlemist::model::Extractor::MapHavok h = ex.parseHavok();
    castlemist::world::WorldScene scene;
    castlemist::world::build_collision(h, scene);
    if (raw) *raw = std::move(h);
    return scene;
}

} // namespace

// §7.4: the instance count equals T3D's, per group and in total, and every
// reference row (`sample`, the first 20 overall, and `sampleByGroup`, the
// first 20 of each group) has an instance with the same (group, placement
// index, collision index) whose world matrix equals T3D's within 1e-4.
// The reference's `world` maps a hull vertex as stored to where T3D draws it,
// z flip included (tools/world/README.md); §7.3 proves that flip, so the two
// are compared as they are.
CM_TEST(world_dat, collision_matches_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        castlemist::world::WorldScene scene = load_map_collision(id);
        // The reference has no collision warning on any test map.
        for (const auto& w : scene.warnings) std::printf("    map %u warning: %s\n", id, w.c_str());
        CHECK(scene.warnings.empty());
        const auto& inst = scene.collision.instances;
        CHECK_EQ(inst.size(), ref["collision"]["instances"].get<size_t>());
        std::map<std::string, size_t> byGroup;
        for (const auto& c : inst) ++byGroup[c.group];
        for (const char* g : {"obs", "prop", "zone"})
            CHECK_EQ(byGroup[g], ref["collision"]["instancesByGroup"][g].get<size_t>());

        std::vector<const nlohmann::json*> rows;
        for (const auto& r : ref["collision"]["sample"]) rows.push_back(&r);
        for (const char* g : {"obs", "prop", "zone"})
            for (const auto& r : ref["collision"]["sampleByGroup"][g]) rows.push_back(&r);
        CHECK(rows.size() >= 20);

        size_t compared = 0, bad = 0;
        double maxWorld = 0;
        for (const nlohmann::json* rp : rows) {
            const auto& r = *rp;
            const castlemist::world::CollisionInstance* mine = nullptr;
            for (const auto& c : inst)
                if (c.group == r["group"].get<std::string>() && c.placement == r["index"].get<uint32_t>() &&
                    c.mesh == r["collisionIndex"].get<uint32_t>()) {
                    mine = &c;
                    break;
                }
            CHECK(mine != nullptr);
            if (!mine) continue;
            ++compared;
            bool ok = true;
            for (int i = 0; i < 16; ++i) {
                const double d = std::fabs(mine->world[i] - r["world"][i].get<double>());
                maxWorld = std::max(maxWorld, d);
                ok = ok && d <= 1e-4;
            }
            if (!ok && bad++ < 3) {
                std::printf("    map %u %s[%u] collision %u disagrees\n", id, mine->group.c_str(), mine->placement,
                            mine->mesh);
                for (int i = 0; i < 16; ++i)
                    std::printf("      world[%d] mine %.6f ref %.6f\n", i, mine->world[i], r["world"][i].get<float>());
            }
        }
        std::printf("    map %u: %zu instances, %zu reference rows compared, %zu disagree (max |dworld| %.3g)\n",
                    id, inst.size(), compared, bad, maxWorld);
        CHECK_EQ(compared, rows.size());
        CHECK_EQ(bad, size_t(0));
    }
}

// §7: at least 99% of collision instance origins fall inside the terrain's
// union of chunk rects (the old reader left every hull at the map origin).
CM_TEST(world_dat, collision_inside_terrain_rects) {
    for (uint32_t id : kTestMaps) {
        MapUnderTest m = load_map_terrain(id);
        castlemist::world::WorldScene scene = load_map_collision(id);
        size_t in = 0;
        for (const auto& c : scene.collision.instances) {
            const float x = c.world[12], y = c.world[13];
            for (const auto& ch : m.terrain.chunks)
                if (x >= ch.rect[0] && x <= ch.rect[2] && y >= ch.rect[1] && y <= ch.rect[3]) {
                    ++in;
                    break;
                }
        }
        std::printf("    map %u: %zu of %zu collision instances inside the terrain rects\n", id, in,
                    scene.collision.instances.size());
        CHECK(!scene.collision.instances.empty());
        CHECK(in * 100 >= scene.collision.instances.size() * 99);
    }
}

namespace {

/// Axis-aligned box; empty until a point is added.
struct Box {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    void add(const float* p) {
        for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
    }
    bool empty() const { return lo[0] > hi[0]; }
    float extent(int k) const { return hi[k] - lo[k]; }
};

/// Intersection over union of two boxes (0 when either is flat or they miss).
double box_iou(const Box& a, const Box& b) {
    double inter = 1, va = 1, vb = 1;
    for (int k = 0; k < 3; ++k) {
        inter *= std::max(0.0, (double)std::min(a.hi[k], b.hi[k]) - std::max(a.lo[k], b.lo[k]));
        va *= a.extent(k);
        vb *= b.extent(k);
    }
    const double u = va + vb - inter;
    return u > 0 ? inter / u : 0;
}

double median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

/// The model's vertex box in its own (file) space, or an empty Box when the
/// fileId is missing or does not parse.
Box model_box(uint32_t file_id) {
    Box b;
    Gw2Dat& dat = shared_dat();
    const uint32_t base = get_by_base_id(dat, file_id);
    if (base == 0 || base > dat.mft_data_list.size()) return b;
    try {
        const MftData& e = dat.mft_data_list[base - 1];
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> bytes = e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
        castlemist::model::Model mdl = castlemist::model::Extractor(bytes, *castlemist::tpl::get()).extract();
        for (const auto& mesh : mdl.meshes)
            for (const auto& v : mesh.vertices) {
                const float p[3] = {v.px, v.py, v.pz};
                b.add(p);
            }
    } catch (const std::exception&) {
        return Box{};
    }
    return b;
}

} // namespace

// §7.2-7.3: the hull scale and z sign, measured. A havk prop placement whose
// translate, rotate and scale equal a prp2 prop's places that prop's model and
// the placement's hulls with the same rotation and translation, so in the
// placement's own frame the hull box times the factor must fit the model's
// vertex box. On Queensdale, for up to 300 distinct (model, geometry) pairs:
// the per-axis extent ratio model / hull, and the box IoU under each axis
// sign map, diag(1, 1, -1) (z flipped) among them.
CM_TEST(world_dat, collision_hulls_fit_visual_props) {
    const uint32_t id = 192711;
    castlemist::model::Extractor::MapHavok h;
    load_map_collision(id, &h);
    MapUnderTest m = load_map_terrain(id);

    std::map<std::array<long, 3>, std::vector<const castlemist::model::Extractor::MapProp*>> byPos;
    for (const auto& p : m.props)
        byPos[{std::lround(p.pos[0] * 100), std::lround(p.pos[1] * 100), std::lround(p.pos[2] * 100)}].push_back(&p);

    std::map<uint32_t, Box> models;
    std::set<std::pair<uint32_t, uint32_t>> seen;
    // iou[signs]: bit k set = axis k negated; kZFlip = diag(1, 1, -1).
    constexpr int kZFlip = 4;
    std::vector<double> ratio[3], iou[8];
    size_t linked = 0, flipBetter = 0;
    for (const auto& pl : h.placements) {
        if (pl.group != "prop" || seen.size() >= 300) continue;
        auto it = byPos.find({std::lround(pl.translate[0] * 100), std::lround(pl.translate[1] * 100),
                              std::lround(pl.translate[2] * 100)});
        if (it == byPos.end()) continue;
        const castlemist::model::Extractor::MapProp* prop = nullptr;
        for (const auto* q : it->second) {
            bool same = std::fabs(q->scale - pl.scale) <= 1e-4f;
            for (int k = 0; k < 3; ++k) same = same && std::fabs(q->rot[k] - pl.rotate[k]) <= 1e-4f;
            if (same) { prop = q; break; }
        }
        if (!prop) continue;
        ++linked;
        if (pl.geometryIndex >= h.geometryAnimations.size() || h.geometryAnimations[pl.geometryIndex].empty())
            continue;
        if (!seen.insert({prop->fileId, pl.geometryIndex}).second) continue;
        const uint32_t anim = h.geometryAnimations[pl.geometryIndex].back();
        if (anim >= h.animationCollisions.size()) continue;
        Box hull;
        for (uint32_t ci : h.animationCollisions[anim])
            if (ci < h.hulls.size())
                for (size_t v = 0; v + 2 < h.hulls[ci].verts.size(); v += 3) hull.add(&h.hulls[ci].verts[v]);
        if (hull.empty()) continue;
        auto [mit, added] = models.try_emplace(prop->fileId);
        if (added) mit->second = model_box(prop->fileId);
        const Box& vis = mit->second;
        if (vis.empty()) continue;

        for (int k = 0; k < 3; ++k)
            if (hull.extent(k) > 1e-3f) ratio[k].push_back(vis.extent(k) / hull.extent(k));
        // The hull box under each of the 8 axis-sign maps diag(+-1, +-1, +-1), at 32x.
        for (int signs = 0; signs < 8; ++signs) {
            Box b;
            for (int k = 0; k < 3; ++k) {
                const float s = (signs >> k) & 1 ? -32.0f : 32.0f;
                b.lo[k] = std::min(s * hull.lo[k], s * hull.hi[k]);
                b.hi[k] = std::max(s * hull.lo[k], s * hull.hi[k]);
            }
            iou[signs].push_back(box_iou(vis, b));
        }
        if (iou[kZFlip].back() > iou[0].back()) ++flipBetter;
    }
    const size_t pairs = iou[0].size();
    std::printf("    map %u: %zu prop placements linked to a prp2 prop, %zu (model, geometry) pairs measured\n", id,
                linked, pairs);
    std::printf("    model/hull extent ratio, median: x %.3f  y %.3f  z %.3f\n", median(ratio[0]),
                median(ratio[1]), median(ratio[2]));
    std::printf("    box IoU at 32x, median, by axis signs (x y z):");
    for (int signs = 0; signs < 8; ++signs)
        std::printf(" %c%c%c %.3f", signs & 1 ? '-' : '+', signs & 2 ? '-' : '+', signs & 4 ? '-' : '+',
                    median(iou[signs]));
    std::printf("\n    z flipped beats z kept in %zu of %zu pairs\n", flipBetter, pairs);
    CHECK(pairs >= 100);
    // The factor is measured, not fitted: each axis's median ratio must land
    // within 1 of 32 (which excludes inches-per-metre 39.37, 16 and 64).
    for (int k = 0; k < 3; ++k) CHECK(std::fabs(median(ratio[k]) - 32.0) <= 1.0);
    // diag(1, 1, -1) fits, and fits better than every other sign map.
    CHECK(median(iou[kZFlip]) >= 0.5);
    for (int signs = 0; signs < 8; ++signs)
        if (signs != kZFlip) CHECK(median(iou[signs]) < median(iou[kZFlip]));
    CHECK(flipBetter * 10 >= pairs * 9);
}
