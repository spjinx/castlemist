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
#include "castlemist/world/zone_scatter.h"

#include <chrono>
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

/// @brief A committed T3D reference, tests/world_ref/<fileId>.json. The
///        references are in the repo, so a missing one fails the test.
nlohmann::json world_ref(uint32_t file_id) {
    std::string path = std::string(CM_WORLD_REF_DIR) + "/" + std::to_string(file_id) + ".json";
    std::ifstream f(path);
    if (!f) ::castlemist::test::fail(__FILE__, __LINE__, "committed reference missing: " + path);
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

// A small map whose page grid fits several chunks-per-page values (Mad
// King's Labyrinth: 6x6 chunks on 2x2 pages fits 3, 4 and 5) takes the
// measured 4, says so, and every chunk gets its blend pages.
CM_TEST(world_dat, terrain_pages_tie_takes_measured_four) {
    constexpr uint32_t kLabyrinth = 2499169;
    if (!ensure_template()) SKIP("no struct template");
    std::vector<uint8_t> bytes = packfile_by_file_id(kLabyrinth);
    castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
    std::vector<std::string> warnings;
    castlemist::world::Terrain T = castlemist::world::build_terrain(ex.parseTerrain(), warnings);
    CHECK_EQ(T.chunksX, 6);
    CHECK_EQ(T.chunksY, 6);
    castlemist::world::resolve_terrain_materials(T, ex.parseTerrainMaterials(), shared_dat(),
                                                 *castlemist::tpl::get(), warnings);
    size_t tie = 0;
    for (const auto& w : warnings) {
        CHECK(w.find("no blend pages") == std::string::npos);
        tie += w.find("using 4, the value every uniquely fitting map measures (UNPROVEN here)") != std::string::npos;
    }
    CHECK_EQ(tie, size_t(1));
    for (const auto& ch : T.chunks) {
        CHECK(ch.material.resolved);
        CHECK(ch.material.pickerScale > 0);
    }
}

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
        // The warnings are exactly the UNPROVEN items of note §4.5 (and Spirit
        // Vale's ramp bindings): uvScale, tiling, lo- vs hi-res material on
        // every map; solid-colour pages (128 chunk-layer pairs) and 36 non-null
        // uvData on Spirit Vale only (§4.2, §4.4).
        {
            const bool sv = id == 1151420;
            const std::string n = std::to_string(T.chunks.size());
            std::vector<std::string> want = {
                "terrain materials: uvScale UNPROVEN on all " + n + " chunks",
                "terrain materials: tiling bytes kept as stored on " + n + " chunks",
                "terrain materials: materialFileId is loResMaterial on " + n + " chunks"};
            if (sv) {
                want.push_back("terrain materials: 25 chunk texture bindings not kept (tokens: ramp)");
                want.push_back("terrain materials: 128 chunk blend pages are solid-colour");
                want.push_back("terrain materials: 36 chunks have a uvData");
            }
            CHECK_EQ(warnings.size(), want.size());
            for (const auto& p : want) {
                size_t hits = 0;
                for (const auto& w : warnings) hits += w.rfind(p, 0) == 0;
                CHECK_EQ(hits, size_t(1));
            }
            for (const auto& w : warnings)
                if (w.find("ramp") == std::string::npos) CHECK(w.find("UNPROVEN") != std::string::npos);
        }

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
        // Exactly one warning: the UNPROVEN animations[last] rule, with the
        // per-map count of §7.1 (havk_sequences.mjs: placements on a 2+
        // animation geometry whose own sequence is not the last animation's).
        const std::map<uint32_t, size_t> kOtherSequence = {{192711, 1188}, {191000, 658}, {1151420, 108}};
        const std::string expected = "collision: " + std::to_string(kOtherSequence.at(id)) +
                                     " placements use animations[last] (T3D's rule, UNPROVEN";
        for (const auto& w : scene.warnings) std::printf("    map %u warning: %s\n", id, w.c_str());
        CHECK_EQ(scene.warnings.size(), size_t(1));
        CHECK(!scene.warnings.empty() && scene.warnings[0].rfind(expected, 0) == 0);
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

// ---- water and environment (docs/research/gw2-world-frame.md §6) ----

#include "castlemist/world/load_world.h"

namespace {

/// @brief Attach a map's water and environment to a fresh scene.
castlemist::world::WorldScene load_map_water(uint32_t file_id) {
    if (!ensure_template()) SKIP("no struct template");
    std::vector<uint8_t> bytes = packfile_by_file_id(file_id);
    castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
    castlemist::world::WorldScene scene;
    castlemist::world::attach_water(ex, scene);
    castlemist::world::attach_environment(ex, scene);
    return scene;
}

/// @brief The warning that starts with `prefix`, or nullptr.
const std::string* warning_starting(const castlemist::world::WorldScene& s, const std::string& prefix) {
    for (const auto& w : s.warnings)
        if (w.rfind(prefix, 0) == 0) return &w;
    return nullptr;
}

} // namespace

// §6: the watr surfaces (count and each z), the watr plane and the havk water
// height equal T3D's reading. The references are nearly empty -- 0 surfaces
// on every map, planeZ 0 and havk 0 on Queensdale and Lion's Arch, no watr on
// Spirit Vale -- so this checks the fields are read, not where the water is;
// the next tests do that.
CM_TEST(world_dat, water_matches_reference) {
    for (uint32_t id : kTestMaps) {
        nlohmann::json ref = world_ref(id);
        castlemist::world::WorldScene scene = load_map_water(id);
        const auto& rw = ref["water"];
        const auto& w = scene.water;
        CHECK_EQ(w.surfaces.size(), rw["surfaces"].get<size_t>());
        CHECK_EQ(w.surfaces.size(), rw["z"].size());
        for (size_t i = 0; i < w.surfaces.size() && i < rw["z"].size(); ++i)
            CHECK_NEAR(w.surfaces[i].z, rw["z"][i].get<float>(), 0.01);
        CHECK_EQ(w.hasPlane, rw.contains("planeZ"));
        if (w.hasPlane && rw.contains("planeZ")) CHECK_NEAR(w.planeZ, rw["planeZ"].get<float>(), 0.01);
        CHECK_EQ(w.hasHavkSurfaceZ, rw.contains("havkWaterSurfaceZ"));
        if (w.hasHavkSurfaceZ && rw.contains("havkWaterSurfaceZ"))
            CHECK_NEAR(w.havkSurfaceZ, rw["havkWaterSurfaceZ"].get<float>(), 0.01);
    }
}

// §6.1: what the dat holds about each map's water, beyond the reference
// (counts from `gw2dat_cli parse`, note §6). Queensdale and Lion's Arch: a
// watr V1 plane at z = 0 with waterFlags 1, the havk water height equal to
// it, no surfaces, no rivers, no shore chunk. Spirit Vale: no watr, a havk
// version (14) with no water height, seven river centrelines. Every map's env
// holds water presets (3 per env block), which are not read.
CM_TEST(world_dat, water_sources_per_map) {
    struct Want { uint32_t id; bool plane; size_t rivers; uint32_t envPresets; };
    for (const Want& want : {Want{192711, true, 0, 39}, Want{191000, true, 0, 21}, Want{1151420, false, 7, 27}}) {
        castlemist::world::WorldScene scene = load_map_water(want.id);
        for (const auto& w : scene.warnings) std::printf("    map %u warning: %s\n", want.id, w.c_str());
        const auto& w = scene.water;
        CHECK_EQ(w.hasPlane, want.plane);
        CHECK_EQ(w.hasHavkSurfaceZ, want.plane);
        if (want.plane) {
            CHECK_EQ(w.planeZ, 0.0f);
            CHECK_EQ(w.planeFlags, uint32_t(1));
            CHECK_EQ(w.havkSurfaceZ, w.planeZ);
        }
        CHECK(w.surfaces.empty());
        CHECK(!w.shore.present);
        CHECK_EQ(w.rivers.size(), want.rivers);
        CHECK(warning_starting(scene, "env: " + std::to_string(want.envPresets) + " water presets") != nullptr);
        // The plane is a height with no outline: its coverage is named UNPROVEN.
        CHECK_EQ(warning_starting(scene, "watr: waterPlaneZ has no outline") != nullptr, want.plane);
        CHECK_EQ(warning_starting(scene, "watr: waterFlags 1 kept as stored; meaning UNPROVEN") != nullptr, want.plane);
        CHECK(warning_starting(scene, "watr: no template") == nullptr);
        CHECK_EQ(warning_starting(scene, "rive: ") != nullptr, want.rivers > 0);
        CHECK(warning_starting(scene, "watr: V0 not read") == nullptr);
    }
}

namespace {

/// @brief The terrain on a grid `w` cells wide over its chunk rects: per
///        cell 0 = no terrain, 1 = terrain above (z <) `level`, 2 = below it.
struct WetMask {
    int w = 0, h = 0;
    std::vector<uint8_t> cells;
    size_t wet = 0, dry = 0;
    size_t components = 0, largest = 0;   ///< 4-connected components of wet cells
};

WetMask wet_mask(const castlemist::world::Terrain& T, float level, int w) {
    WetMask m;
    const float x0 = T.chunks.front().rect[0], y1 = T.chunks.front().rect[3];
    const float x1 = T.chunks.back().rect[2], y0 = T.chunks.back().rect[1];
    m.w = w;
    m.h = (int)(w * (y1 - y0) / (x1 - x0));
    m.cells.assign((size_t)m.w * m.h, 0);
    for (int j = 0; j < m.h; ++j)   // row 0 is the north edge
        for (int i = 0; i < m.w; ++i) {
            const float x = x0 + (i + 0.5f) * (x1 - x0) / m.w, y = y1 - (j + 0.5f) * (y1 - y0) / m.h;
            bool inside = false;
            const float h = castlemist::world::terrain_height_at(T, x, y, &inside);
            const uint8_t v = !inside ? 0 : (h > level ? 2 : 1);   // up = -Z: larger z is lower
            m.cells[(size_t)j * m.w + i] = v;
            m.wet += v == 2;
            m.dry += v == 1;
        }
    std::vector<uint8_t> seen(m.cells.size(), 0);
    std::vector<size_t> stack;
    for (size_t s = 0; s < m.cells.size(); ++s) {
        if (m.cells[s] != 2 || seen[s]) continue;
        ++m.components;
        size_t n = 0;
        stack.assign(1, s);
        seen[s] = 1;
        while (!stack.empty()) {
            const size_t c = stack.back();
            stack.pop_back();
            ++n;
            const int i = (int)(c % m.w), j = (int)(c / m.w);
            for (auto [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                const int a = i + di, b = j + dj;
                if (a < 0 || b < 0 || a >= m.w || b >= m.h) continue;
                const size_t k = (size_t)b * m.w + a;
                if (m.cells[k] == 2 && !seen[k]) { seen[k] = 1; stack.push_back(k); }
            }
        }
        m.largest = std::max(m.largest, n);
    }
    return m;
}

/// @brief With CM_WORLD_DIAG_DIR set, write the mask as a PPM image there
///        (blue = below the level, tan = above, black = no terrain), the
///        pictures note §6.2 describes.
void maybe_write_mask(const WetMask& m, const std::string& name) {
    const char* dir = std::getenv("CM_WORLD_DIAG_DIR");
    if (!dir || !*dir) return;
    const std::string path = std::string(dir) + "/" + name + ".ppm";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", m.w, m.h);
    static const uint8_t kRgb[3][3] = {{0, 0, 0}, {150, 130, 90}, {40, 90, 200}};
    for (uint8_t v : m.cells) std::fwrite(kRgb[v], 1, 3, f);
    std::fclose(f);
    std::printf("    wrote %s\n", path.c_str());
}

} // namespace

// §6.2: the terrain below the watr plane is the maps' water bodies. On a
// 600-cell-wide grid, the cells whose terrain lies below planeZ (up = -Z)
// form Queensdale's river network and lakes and Lion's Arch's bay and canals
// (the pictures: set CM_WORLD_DIAG_DIR). What is asserted was chosen after
// looking at those pictures, and says only that the region is one coherent
// body of plausible size: at most half the map (with up = +Z it would be 90%
// and 75%), at least 1%, and one 4-connected component holding at least half
// of it. This places the water at about z = 0. It cannot show the game
// draws at waterPlaneZ: both maps store 0, a float's default, so "drawn at
// waterPlaneZ" and "water at 0, field happens to be 0" look the same
// (UNPROVEN, note §6.2).
CM_TEST(world_dat, water_plane_traces_water_bodies) {
    for (uint32_t id : {192711u, 191000u}) {
        castlemist::world::WorldScene scene = load_map_water(id);
        MapUnderTest m = load_map_terrain(id);
        CHECK(scene.water.hasPlane && m.terrain.present);
        if (!scene.water.hasPlane || !m.terrain.present) continue;
        const WetMask wm = wet_mask(m.terrain, scene.water.planeZ, 600);
        maybe_write_mask(wm, "water_mask_" + std::to_string(id));
        const size_t cells = wm.wet + wm.dry;
        std::printf("    map %u: %zu of %zu cells below the plane (%.1f%%), %zu components, largest %zu (%.1f%% of them)\n",
                    id, wm.wet, cells, 100.0 * wm.wet / cells, wm.components, wm.largest,
                    100.0 * wm.largest / std::max<size_t>(1, wm.wet));
        CHECK(wm.wet * 100 >= cells);
        CHECK(wm.wet * 2 <= cells);
        CHECK(wm.largest * 2 >= wm.wet);
    }
}

// §6.2, a printed MEASUREMENT and REGRESSION GUARD, NOT EVIDENCE of the
// water level. Props that stand over terrain lying at least 64 units below
// the plane and are not on that terrain (|z - h| >= 16), by height. The
// criterion stated before measuring -- the 16-unit band centred on planeZ is
// their most common height -- was FALSE on both maps (Queensdale peaks 8-24
// below the plane, Lion's Arch 24-40 above it, on docks, which are not
// water). The window check below was shaped around this data (a 112-unit
// window chosen after seeing Lion's Arch's dock peak), so it only guards the
// reader against changing these numbers; the water evidence is the terrain
// mask (water_plane_traces_water_bodies).
CM_TEST(world_dat, water_props_over_water_heights) {
    for (uint32_t id : {192711u, 191000u}) {
        castlemist::world::WorldScene scene = load_map_water(id);
        MapUnderTest m = load_map_terrain(id);
        CHECK(scene.water.hasPlane);
        if (!scene.water.hasPlane) continue;
        const float pz = scene.water.planeZ;
        constexpr int kBands = 128, kMid = kBands / 2;   // 16-unit bands; band kMid is centred on pz
        std::vector<size_t> band(kBands, 0);
        size_t over = 0;
        for (const auto& p : m.props) {
            bool inside = false;
            const float h = castlemist::world::terrain_height_at(m.terrain, p.pos[0], p.pos[1], &inside);
            if (!inside || h - pz < 64.0f || std::fabs(p.pos[2] - h) < 16.0f) continue;
            ++over;
            const int b = (int)std::floor((p.pos[2] - pz + 8.0f) / 16.0f) + kMid;
            if (b >= 0 && b < kBands) ++band[b];
        }
        auto window = [&](int centre) {   // bands centre-3 .. centre+3: 112 units
            size_t n = 0;
            for (int b = centre - 3; b <= centre + 3; ++b) n += (b >= 0 && b < kBands) ? band[b] : 0;
            return n;
        };
        const size_t atPlane = window(kMid);
        size_t bestOther = 0;
        for (int c = 0; c < kBands; ++c)
            if (std::abs(c - kMid) >= 7) bestOther = std::max(bestOther, window(c));
        std::printf("    map %u: %zu props over terrain >= 64 below the plane and off it; by 16-unit band of z - planeZ:",
                    id, over);
        for (int b = kMid - 6; b <= kMid + 6; ++b) std::printf(" %+d:%zu", (b - kMid) * 16, band[b]);
        std::printf("\n    map %u: window around the plane %zu, best other window %zu\n", id, atPlane, bestOther);
        CHECK(atPlane >= 2 * bestOther);   // regression guard only (fitted to this data)
    }
}

// §6.3: Spirit Vale's rivers are read as stored. Asserted: the seven names
// and point counts, and the first point, equal the generic template parser's
// reading (`gw2dat_cli parse --file-id 1151420`, rive v5); every point lies
// inside the terrain's rects. Measured and printed, not asserted (note
// §6.3): per river, how far its points sit from the terrain and from the
// nearest placed collision triangle at their (x, y). Stated before
// measuring and FALSE: "75% of points have collision within 128 units".
// Only BanditFall follows the ground; the others float hundreds to 1600
// units above it, so whether they are drawn as water is UNPROVEN.
CM_TEST(world_dat, water_rivers_read_as_stored) {
    const uint32_t id = 1151420;
    castlemist::world::WorldScene scene = load_map_water(id);
    castlemist::world::WorldScene coll = load_map_collision(id);
    MapUnderTest m = load_map_terrain(id);
    const std::vector<std::pair<std::string, size_t>> kRivers = {
        {"Gorseval02", 14}, {"Bandit01", 8}, {"SoulRiverEntrance01", 12}, {"SoulRiverEntrance02", 6},
        {"BanditFall", 5},  {"GorsevalGhostMans", 11}, {"Bandit02", 5}};
    const auto& rivers = scene.water.rivers;
    CHECK_EQ(rivers.size(), kRivers.size());
    for (size_t i = 0; i < rivers.size() && i < kRivers.size(); ++i) {
        CHECK_EQ(rivers[i].name, kRivers[i].first);
        CHECK_EQ(rivers[i].points.size(), 3 * kRivers[i].second);
    }
    if (rivers.empty() || rivers[0].points.size() < 3) return;
    CHECK_NEAR(rivers[0].points[0], 1213.247437, 0.01);
    CHECK_NEAR(rivers[0].points[1], -2816.633789, 0.01);
    CHECK_NEAR(rivers[0].points[2], -3574.578125, 0.01);

    // Every placed collision triangle in map space, for the vertical probe.
    std::vector<std::array<float, 9>> tris;
    for (const auto& inst : coll.collision.instances) {
        const auto& mesh = coll.collision.meshes[inst.mesh];
        const float* M = inst.world;
        auto at = [&](uint32_t v, float* o) {
            for (int r = 0; r < 3; ++r)
                o[r] = M[r] * mesh.verts[3 * v] + M[4 + r] * mesh.verts[3 * v + 1] + M[8 + r] * mesh.verts[3 * v + 2] +
                       M[12 + r];
        };
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            std::array<float, 9> tri;
            at(mesh.indices[t], &tri[0]);
            at(mesh.indices[t + 1], &tri[3]);
            at(mesh.indices[t + 2], &tri[6]);
            tris.push_back(tri);
        }
    }
    // Smallest |z - surface z| over the collision triangles covering (x, y).
    auto collisionGap = [&](float x, float y, float z) {
        float best = 1e30f;
        for (const auto& t : tris) {
            const float* a = &t[0];
            const float* b = &t[3];
            const float* c = &t[6];
            const float d = (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
            if (std::fabs(d) < 1e-6f) continue;   // vertical or degenerate in (x, y)
            const float u = ((x - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (y - a[1])) / d;
            const float w = ((b[0] - a[0]) * (y - a[1]) - (x - a[0]) * (b[1] - a[1])) / d;
            if (u < 0 || w < 0 || u + w > 1) continue;
            best = std::min(best, std::fabs(z - (a[2] + u * (b[2] - a[2]) + w * (c[2] - a[2]))));
        }
        return best;
    };
    size_t inside = 0, total = 0;
    for (const auto& r : rivers) {
        std::vector<float> dh, dc;
        for (size_t k = 0; k + 2 < r.points.size(); k += 3) {
            const float x = r.points[k], y = r.points[k + 1], z = r.points[k + 2];
            bool in = false;
            const float h = castlemist::world::terrain_height_at(m.terrain, x, y, &in);
            ++total;
            inside += in;
            if (in) dh.push_back(z - h);
            dc.push_back(collisionGap(x, y, z));
        }
        std::sort(dh.begin(), dh.end());
        size_t near = 0;
        for (float g : dc) near += g <= 128.0f;
        std::printf("    river %-20s %2zu points: z - terrain median %7.1f, min %7.1f, max %7.1f; collision within 128: %zu\n",
                    r.name.c_str(), r.points.size() / 3, dh.empty() ? 0.0f : dh[dh.size() / 2],
                    dh.empty() ? 0.0f : dh.front(), dh.empty() ? 0.0f : dh.back(), near);
    }
    CHECK_EQ(inside, total);
}

// §6.5: every test map has a sky and a day light rig, and the warning names
// the lighting presets and per-zone blocks that are not attached. The block
// counts are the dataLocalArray lengths of `gw2dat_cli parse` (29, 25, 12;
// no dataOverrideArray entries); the preset count is dataGlobal `lighting`.
CM_TEST(world_dat, environment_sky_present) {
    struct Want { uint32_t id; uint32_t presets, local; };
    for (const Want& want : {Want{192711, 3, 29}, Want{191000, 3, 25}, Want{1151420, 3, 12}}) {
        castlemist::world::WorldScene scene = load_map_water(want.id);
        CHECK(scene.environment.sky.present);
        CHECK(scene.environment.light.present);
        CHECK(warning_starting(scene, "environment: no sky") == nullptr);
        CHECK(warning_starting(scene, "environment: no light rig") == nullptr);
        const std::string expected = "environment: " + std::to_string(want.presets - 1) +
                                     " other dataGlobal lighting presets and " + std::to_string(want.local) +
                                     " per-zone blocks (" + std::to_string(want.local) + " dataLocalArray, 0 ";
        const std::string* w = warning_starting(scene, "environment: " + std::to_string(want.presets - 1) + " other");
        if (w) std::printf("    map %u warning: %s\n", want.id, w->c_str());
        CHECK(warning_starting(scene, expected) != nullptr);
    }
}

// ---- load_world (docs/research/gw2-world-frame.md §1-§7) ----

#include "castlemist/world/load_world.h"

#include <stdexcept>

// Each test map loads whole: terrain with every chunk's material resolved,
// props, and no section failed with an exception. The units warning is there
// (§1.3), and the summary carries the same warnings.
CM_TEST(world_dat, load_world_three_maps) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        CHECK_EQ(w.mapFileId, id);
        CHECK(w.hasBounds);
        CHECK(w.terrain.present);
        CHECK(!w.terrain.chunks.empty());
        CHECK(!w.props.empty());
        size_t unresolved = 0;
        for (const auto& c : w.terrain.chunks) unresolved += !c.material.resolved;
        CHECK_EQ(unresolved, size_t(0));
        for (const std::string& s : w.warnings) {
            CHECK(s.find(": exception: ") == std::string::npos);
            if (s.find("exception") != std::string::npos) std::printf("    map %u warning: %s\n", id, s.c_str());
        }
        // Every line starts with a section or chunk prefix, and every
        // UNPROVEN item says so with the literal token.
        for (const std::string& s : w.warnings) {
            bool prefixed = false;
            for (const char* p : {"terrain: ", "terrain materials: ", "props: ", "collision: ", "water: ", "watr: ",
                                  "havk: ", "rive: ", "shor: ", "env: ", "environment: ", "zone: ", "zones: ", "chunk ",
                                  "units: "})
                prefixed = prefixed || s.rfind(p, 0) == 0;
            if (!prefixed) std::printf("    map %u unprefixed warning: %s\n", id, s.c_str());
            CHECK(prefixed);
        }
        CHECK(warning_starting(w, "terrain materials: uvScale UNPROVEN") != nullptr);
        // Unread chunks are named; the chunks the sections read never are.
        size_t unread = 0;
        for (const std::string& s : w.warnings)
            if (s.rfind("chunk ", 0) == 0) {
                ++unread;
                for (const char* read : {"trn", "parm", "prp2", "havk", "env", "watr", "shor", "rive", "zon2"})
                    CHECK(s.rfind(std::string("chunk ") + read + " ", 0) != 0);
            }
        CHECK(unread > 0);
        CHECK(warning_starting(w, "chunk zon2 v") == nullptr);   // read by the zones section
        const std::string* u = warning_starting(w, "units: ");
        CHECK(u != nullptr);
        if (u) CHECK(u->find("UNPROVEN") != std::string::npos);
        nlohmann::json s = castlemist::world::world_summary(w);
        CHECK_EQ(s["map"].get<uint32_t>(), id);
        CHECK_EQ(s["warnings"].size(), w.warnings.size());
        CHECK_EQ(s["terrain"]["resolvedMaterials"].get<size_t>(), w.terrain.chunks.size());
        // Water: Queensdale and Lion's Arch have the watr plane at 0, Spirit
        // Vale none and seven rivers (water_sources_per_map).
        const bool plane = id != 1151420;
        CHECK_EQ(s["water"]["plane"].get<bool>(), plane);
        CHECK_EQ(s["water"]["planeZ"].is_null(), !plane);
        if (plane) CHECK_EQ(s["water"]["planeZ"].get<float>(), 0.0f);
        CHECK_EQ(s["water"]["rivers"].get<size_t>(), w.water.rivers.size());
        CHECK_EQ(s["water"]["rivers"].get<size_t>(), size_t(plane ? 0 : 7));
        CHECK_EQ(s["water"]["surfaces"].get<size_t>(), size_t(0));
    }
}

// A model packfile has none of trn, parm, prp2 and havk: not a map.
CM_TEST(world_dat, not_a_map_throws) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    bool threw = false;
    try {
        castlemist::world::load_world(dat, 2163020, *castlemist::tpl::get());   // a MODL fileId (test_extract_dat.cpp)
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "file 2163020 is not a map packfile";
    }
    CHECK(threw);
}

// ---- walking (map window) ----

#include "castlemist/world/walk.h"

// A walker dropped at the middle of Queensdale's bounds finds the highest
// surface under it, stays grounded with no input, and keeps its eye inside the
// map rect. The index is built with terrain, so the middle of the map has
// ground even where no collision hull sits.
CM_TEST(world_dat, queensdale_walker_lands) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    castlemist::world::WorldScene w = castlemist::world::load_world(dat, 192711, *castlemist::tpl::get());
    CHECK(w.hasBounds);

    castlemist::world::CollisionIndex idx = castlemist::world::CollisionIndex::build(w, true);
    const size_t collisionTris = idx.triangle_count(castlemist::world::kHitCollision);
    const size_t allTris = idx.triangle_count();
    CHECK(collisionTris > 0);
    CHECK(allTris > collisionTris);   // terrain triangles came along

    float lo[3], hi[3];
    idx.bounds(lo, hi);
    float pos[3] = {(w.bounds[0] + w.bounds[2]) * 0.5f, (w.bounds[1] + w.bounds[3]) * 0.5f, lo[2] - 1000.0f};
    std::printf("    bounds rect [%g %g %g %g]  index lo [%g %g %g] hi [%g %g %g]  tris collision %zu all %zu\n",
                w.bounds[0], w.bounds[1], w.bounds[2], w.bounds[3], lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                collisionTris, allTris);

    castlemist::world::Walker walker(&idx);
    walker.reset(pos);
    CHECK(walker.snap_to_ground(pos));
    std::printf("    landed at eye [%g %g %g]\n", pos[0], pos[1], pos[2]);

    const float none[2] = {0, 0};
    for (int i = 0; i < 300; ++i) walker.step(pos, none, 1.0f / 60.0f);
    std::printf("    after 300 steps eye [%g %g %g]\n", pos[0], pos[1], pos[2]);
    CHECK(walker.grounded());
    CHECK(pos[0] >= w.bounds[0] && pos[0] <= w.bounds[2]);
    CHECK(pos[1] >= w.bounds[1] && pos[1] <= w.bounds[3]);
    CHECK(pos[2] >= lo[2] && pos[2] <= hi[2]);
}

// ---- world materials and motion (docs/research/gw2-world-materials.md) ----

#include "castlemist/world/anim.h"

#include <algorithm>
#include <map>

namespace {

/// Placements of Spirit Vale's propAnimArray whose (model, animSequence) names a
/// clip the model has. Measured 2026-10-15 on client build 1791828055 (PE
/// timestamp); the map window counted 106 resolved + 10 unresolved = 116.
constexpr size_t kSpiritValeResolvedPlacements = 106;

} // namespace

// Note §5: Spirit Vale's animated props. 37 of the 41 distinct (model, clip
// name) pairs resolve exactly; the four that do not are named.
CM_TEST(world_dat, materials_spirit_vale_clips_resolve) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    castlemist::world::WorldScene w = castlemist::world::load_world(dat, 1151420, *castlemist::tpl::get());

    std::map<uint32_t, std::shared_ptr<ModelPreview>> models;   // by prop model index
    std::set<std::pair<uint32_t, uint64_t>> pairs, resolvedPairs;   // (model fileId, animSequence)
    size_t placements = 0, resolvedPlacements = 0;
    for (const auto& p : w.props) {
        if (p.group != "propAnimArray") continue;
        ++placements;
        const uint32_t fid = w.models.at(p.model).fileId;
        auto it = models.find(p.model);
        if (it == models.end())
            it = models.emplace(p.model, load_model_by_fileid(dat, fid, ModelLoadOptions{false, false})).first;
        const bool ok = it->second && castlemist::world::resolve_prop_clip(it->second->animClipTokens, p.animSequence) >= 0;
        pairs.insert({fid, p.animSequence});
        if (ok) { resolvedPairs.insert({fid, p.animSequence}); ++resolvedPlacements; }
    }
    std::set<std::pair<uint32_t, std::string>> misses;
    for (const auto& pr : pairs)
        if (!resolvedPairs.count(pr))
            misses.insert({pr.first, castlemist::model::detokenizeName64(pr.second)});
    CHECK_EQ(pairs.size(), size_t(41));
    CHECK_EQ(resolvedPairs.size(), size_t(37));
    const std::set<std::pair<uint32_t, std::string>> want = {
        {1127039, "zeropose"}, {1151335, "zeropose"}, {740373, "on"}, {1150399, "noarch"}};
    CHECK(misses == want);
    CHECK_EQ(resolvedPlacements, kSpiritValeResolvedPlacements);
}

// Terrain textures: every colour and normal texture a chunk binds, and every
// blend page, decodes capped at 512 with a full mip chain down to 4x4.
CM_TEST(world_dat, materials_terrain_textures_decode) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        std::set<uint32_t> ids;
        size_t solidPages = 0;
        for (const auto& c : w.terrain.chunks) {
            const auto& m = c.material;
            if (!m.resolved) continue;
            for (uint32_t f : m.textureFileIds) if (f) ids.insert(f);
            for (uint32_t f : m.normalFileIds) if (f) ids.insert(f);
            for (uint32_t f : {m.pickerFileId, m.picker2FileId}) {
                if (f) ids.insert(f);
                else { ++solidPages; CHECK(m.pickerScale > 0); }
            }
        }
        size_t failures = 0;
        for (uint32_t f : ids) {
            CappedTexture t;
            const bool ok = decode_texture_capped(dat, f, 512, t);
            bool good = ok && !t.mips.empty() && std::max(t.width, t.height) <= 512;
            if (good) {
                // Last mip is at most 4x4: its byte size is w*h*4 for w, h <= 4.
                good = t.mips.back().size() <= size_t(4 * 4 * 4);
            }
            if (!good && failures++ < 5) std::printf("    map %u: texture %u failed to decode\n", id, f);
        }
        std::printf("    map %u: %zu distinct terrain textures, %zu solid pages, %zu failures\n", id, ids.size(),
                    solidPages, failures);
        CHECK(!ids.empty());
        CHECK_EQ(failures, size_t(0));
    }
}

// Model 1126949 (note §4): material 0 has two texture transforms, scrolling
// 0.2 in v; header-only loading changes no fileId and decodes no pixels.
CM_TEST(world_dat, materials_model_1126949_tex_transforms) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    auto a = load_model_by_fileid(dat, 1126949, ModelLoadOptions{false, false});
    auto b = load_model_by_fileid(dat, 1126949, ModelLoadOptions{false, true});
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (!a || !b) return;
    CHECK(!a->materials.empty());
    if (a->materials.empty()) return;
    const auto& tt = a->materials[0].texTransforms;
    CHECK_EQ(tt.size(), size_t(2));
    if (tt.size() == 2) {
        CHECK_EQ((int)tt[0].uvIndex, 1);
        CHECK_EQ((int)tt[1].uvIndex, 0);
        for (const auto& t : tt) {
            CHECK_NEAR(t.scroll[0], 0.0f, 0.01);
            CHECK_NEAR(t.scroll[1], 0.2f, 0.01);
            CHECK_EQ((int)t.columns, 1);
            CHECK_EQ((int)t.rows, 1);
            CHECK_EQ((int)t.count, 1);
        }
    }
    CHECK_EQ(a->materials.size(), b->materials.size());
    for (size_t i = 0; i < a->materials.size() && i < b->materials.size(); ++i) {
        const int da = a->materials[i].diffuseTex, db = b->materials[i].diffuseTex;
        CHECK_EQ(da >= 0, db >= 0);
        if (da >= 0 && db >= 0) CHECK_EQ(a->textures.at(da).fileId, b->textures.at(db).fileId);
    }
    for (const auto& t : a->textures) CHECK(t.rgba.empty());
}

// Queensdale's dataGlobal water presets, pinned to what the 2026-10-15 client
// (PE timestamp 1791828055) holds. Research note section 6 quotes different
// values (shallow A5DF5C / deep 364F21, fresnel 3.89, depthAttenuation 15.66,
// patternTile 4.58); none of the three dataGlobal presets holds them, so they
// are probably from a per-zone block, which is out of scope here (Task 17
// records it). Colours are in the stored byte order.
CM_TEST(world_dat, materials_queensdale_water_presets) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    castlemist::world::WorldScene w = castlemist::world::load_world(dat, 192711, *castlemist::tpl::get());
    const auto& water = w.environment.water;
    CHECK_EQ(water.size(), size_t(3));
    for (const auto& p : water) CHECK(p.materialFileId == 187551 || p.materialFileId == 188555);
    CHECK_EQ(w.environment.light.presetIndex, 0);
    if (water.size() != 3) return;
    const auto& p = water[0];
    CHECK_NEAR(p.bumpTile[0], 5.013, 0.01);
    CHECK_NEAR(p.bumpTile[1], 13.661, 0.01);
    CHECK_NEAR(p.bumpSpeed[0], 1.31, 0.01);
    CHECK_NEAR(p.bumpSpeed[1], 0.643, 0.01);
    CHECK_NEAR(p.patternTile, 14.283, 0.01);
    CHECK_EQ((int)p.surfaceShallowColor[0], 0xBD);
    CHECK_EQ((int)p.surfaceShallowColor[1], 0xCF);
    CHECK_EQ((int)p.surfaceShallowColor[2], 0x5F);
    CHECK_EQ((int)p.surfaceDeepColor[0], 0x3B);
    CHECK_EQ((int)p.surfaceDeepColor[1], 0x64);
    CHECK_EQ((int)p.surfaceDeepColor[2], 0x85);
    CHECK_NEAR(p.surfaceFresnel, 3.325, 0.01);
    CHECK_NEAR(p.depthAttenuation, 28.47, 0.01);
    CHECK_NEAR(p.distortAmount, 0.01, 0.01);
    CHECK_EQ(water[2].materialFileId, uint32_t(188555));
}

// Scene fields read for the terrain shader: uvData, swapDistance, texIndexArray.
CM_TEST(world_dat, materials_scene_fields) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    castlemist::world::WorldScene sv = castlemist::world::load_world(dat, 1151420, *castlemist::tpl::get());
    size_t uv = 0, scrolling = 0;
    for (const auto& c : sv.terrain.chunks)
        if (c.material.hasUvData) {
            ++uv;
            if (std::fabs(c.material.uvData.translation[0]) <= 0.01f &&
                std::fabs(c.material.uvData.translation[1] - 0.2f) <= 0.01f)
                ++scrolling;
        }
    std::printf("    spirit vale: %zu chunks with uvData, %zu with translation (0, 0.2)\n", uv, scrolling);
    CHECK_EQ(uv, size_t(36));
    CHECK(scrolling >= 1);
    CHECK_EQ(sv.terrain.swapDistance, 24576.0f);

    castlemist::world::WorldScene q = castlemist::world::load_world(dat, 192711, *castlemist::tpl::get());
    CHECK_EQ(q.terrain.swapDistance, 39936.0f);
    size_t resolved = 0, bad = 0;
    for (const auto& c : q.terrain.chunks)
        if (c.material.resolved) {
            ++resolved;
            bad += c.material.texIndexFileIds.size() != 10;
        }
    CHECK(resolved > 0);
    CHECK_EQ(bad, size_t(0));
}

// ---- zon2 (world foliage spec, M1-M3) ----

#include <unordered_map>
#include <unordered_set>

// M1 with the client decode (docs/research/gw2-terrain-to-unity.md §1.3, 2026-10-16; ruling S4):
// the placement counts; M3: every cell's layer and model slot resolve; parseMapZones() is the
// walk at the cell's vertex x0 + 48 c.
CM_TEST(world_dat, zon2_reads_three_maps) {
    if (!ensure_template()) SKIP("no struct template");
    using Ex = castlemist::model::Extractor;
    const std::map<uint32_t, size_t> kVisits = {{192711, 247804}, {191000, 40252}, {1151420, 8670}};
    for (uint32_t id : kTestMaps) {
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        Ex ex(bytes, *castlemist::tpl::get());
        const Ex::MapZon2 z = ex.parseZon2();
        CHECK(z.present);
        CHECK(z.hasRect);
        for (const auto& m : z.missingFields) std::printf("    map %u missing field %s\n", id, m.c_str());
        CHECK(z.missingFields.empty());
        std::unordered_map<uint32_t, const Ex::Zon2Def*> defs;
        for (const auto& d : z.defs) defs[d.token] = &d;
        size_t visits = 0, bad = 0;
        bool haveFirst = false;
        int32_t firstC = 0;
        for (const auto& zone : z.zones) {
            auto it = defs.find(zone.defToken);
            const Ex::Zon2Def* def = it == defs.end() ? nullptr : it->second;
            Ex::walkZoneFlags(zone, [&](uint32_t, int32_t c, int32_t, uint8_t f) {
                ++visits;
                if (!haveFirst) { haveFirst = true; firstC = c; }
                const uint32_t li = (f >> 4) - 1u, slot = (f >> 1) & 7u;
                const bool ok = def && (f >> 4) > 0 && li < def->layers.size() && slot < def->layers[li].models.size();
                bad += !ok;
            });
        }
        std::printf("    map %u: zon2 v%u, %zu defs, %zu zones, %zu visits, %zu unresolved\n", id, z.version,
                    z.defs.size(), z.zones.size(), visits, bad);
        CHECK_EQ(visits, kVisits.at(id));
        CHECK_EQ(bad, size_t(0));
        const auto inst = ex.parseMapZones();
        CHECK_EQ(inst.size(), visits);
        CHECK(haveFirst && !inst.empty());
        if (haveFirst && !inst.empty()) CHECK_EQ(inst[0].pos[0], z.rect[0] + 48.0f * (float)firstC);
    }
}

// M3's prop cross-check: propModels `token` is a prp2 `guid`.
CM_TEST(world_dat, havk_prop_tokens_read) {
    if (!ensure_template()) SKIP("no struct template");
    using Ex = castlemist::model::Extractor;
    std::vector<uint8_t> bytes = packfile_by_file_id(192711);
    Ex ex(bytes, *castlemist::tpl::get());
    const Ex::MapHavok h = ex.parseHavok();
    const auto props = ex.parseMapProps();
    std::unordered_set<uint64_t> guids;
    for (const auto& p : props)
        if (p.guid) guids.insert(p.guid);
    size_t tokens = 0, matched = 0;
    for (const auto& p : h.placements) {
        if (p.group != "prop" || !p.token) continue;
        ++tokens;
        matched += guids.count(p.token) != 0;
    }
    std::printf("    192711: %zu prop guids, %zu propModels tokens, %zu match a guid\n", guids.size(), tokens,
                matched);
    CHECK(tokens > 0);
    CHECK(matched > 0);
}

// ---- zones: phase-1 acceptance (world foliage spec D1, D3, M1-M3) ----

namespace {

/// Share of `a`'s volume that lies inside `b` (0 when `a` is flat).
double containment(const Box& a, const Box& b) {
    double inter = 1, va = 1;
    for (int k = 0; k < 3; ++k) {
        inter *= std::max(0.0, (double)std::min(a.hi[k], b.hi[k]) - std::max(a.lo[k], b.lo[k]));
        va *= a.extent(k);
    }
    return va > 0 ? inter / va : 0;
}

struct ZoneAccept {
    uint32_t colliders, anchor, geometry, geometries, crossChecks;
    size_t byType[3];
};
const std::map<uint32_t, ZoneAccept> kZoneAccept = {
    // The client decode (ruling S4): Queensdale's 3 former geometry-only links are cells of zone 135.
    {192711, {4381, 4381, 0, 45, 37, {193125, 44380, 10299}}},
    {191000, {88, 88, 0, 7, 4, {36939, 2300, 1013}}},
    {1151420, {284, 284, 0, 1, 0, {8386, 0, 284}}},
};

} // namespace

CM_TEST(world_dat, zones_phase1_acceptance) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    for (uint32_t id : kTestMaps) {
        const ZoneAccept& want = kZoneAccept.at(id);
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        const auto& z = w.zones;
        CHECK(z.present);
        size_t byType[3] = {0, 0, 0};
        for (const auto& p : z.placements)
            if (p.layer != UINT32_MAX) {
                const int t = z.layers[p.layer].type;
                if (t >= 1 && t <= 3) ++byType[t - 1];
            }
        std::printf("    map %u: placements by type %zu %zu %zu; colliders %u, anchor %u, geometry %u, unlinked %u, "
                    "geometries %u, crossChecks %u, disagreements %u\n",
                    id, byType[0], byType[1], byType[2], z.colliderEntries, z.zonePlacementsLinked,
                    z.collidersLinkedByGeometry, z.collidersUnlinked, z.colliderGeometries, z.propCrossChecks,
                    z.propDisagreements);
        for (int t = 0; t < 3; ++t) CHECK_EQ(byType[t], want.byType[t]);
        CHECK_EQ(z.colliderEntries, want.colliders);
        CHECK_EQ(z.zonePlacementsLinked, want.anchor);
        CHECK_EQ(z.collidersLinkedByGeometry, want.geometry);
        CHECK_EQ(z.collidersUnlinked, 0u);
        CHECK_EQ(z.colliderGeometries, want.geometries);
        CHECK_EQ(z.propCrossChecks, want.crossChecks);
        CHECK_EQ(z.propDisagreements, 0u);
        CHECK_EQ(z.placementsUnresolved, 0u);

        // Every geometry maps to exactly one fileId; anchor links sit within a cell of their anchor.
        std::map<uint32_t, std::set<uint32_t>> fileOfGeometry;
        {
            std::vector<uint8_t> bytes = packfile_by_file_id(id);
            castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
            const auto h = ex.parseHavok();
            std::vector<uint32_t> geomOf;
            for (const auto& p : h.placements)
                if (p.group == "zone") geomOf.push_back(p.geometryIndex);
            for (const auto& inst : z.instances)
                if (inst.collider < geomOf.size())
                    fileOfGeometry[geomOf[inst.collider]].insert(w.models.at(inst.model).fileId);
        }
        CHECK_EQ(fileOfGeometry.size(), size_t(want.geometries));
        for (const auto& [g, files] : fileOfGeometry) CHECK_EQ(files.size(), size_t(1));

        size_t far = 0;
        for (const auto& inst : z.instances) {
            if (inst.placement == UINT32_MAX || inst.source != castlemist::world::ZoneSource::Collider) continue;
            const auto& pl = z.placements[inst.placement];
            float a[2];
            castlemist::world::zone_anchor(w.bounds[0], w.bounds[1], pl.c, pl.r, a);
            // translation of a column-major world matrix is elements 12..14
            if (std::fabs(inst.world[12] - a[0]) > 24.01f || std::fabs(inst.world[13] - a[1]) > 24.01f) ++far;
        }
        CHECK_EQ(far, size_t(0));
        for (const std::string& s : w.warnings) CHECK(s.rfind("chunk zon2", 0) != 0);
    }
}

CM_TEST(world_dat, zones_every_collider_has_a_tree) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        std::map<uint32_t, uint32_t> instOfCollider;   // collider -> instance count
        for (const auto& i : w.zones.instances) ++instOfCollider[i.collider];
        std::set<uint32_t> models;
        size_t zoneColliders = 0, missing = 0;
        for (const auto& ci : w.collision.instances) {
            if (ci.group != "zone") continue;
            ++zoneColliders;
            if (!instOfCollider.count(ci.placement)) ++missing;
        }
        for (const auto& i : w.zones.instances)
            if (i.source == castlemist::world::ZoneSource::Collider) models.insert(i.model);
        size_t noMesh = 0;
        for (uint32_t m : models) {
            auto mp = load_model_by_fileid(dat, w.models.at(m).fileId, ModelLoadOptions{false, false});
            bool ok = false;
            if (mp)
                for (const auto& mesh : mp->meshes) ok = ok || (!mesh.indices.empty() && !mesh.vertices.empty());
            noMesh += !ok;
        }
        std::printf("    map %u: %zu zone colliders, %zu without a tree, %zu models, %zu without a mesh\n", id,
                    zoneColliders, missing, models.size(), noMesh);
        CHECK(zoneColliders > 0);
        CHECK_EQ(missing, size_t(0));
        CHECK_EQ(noMesh, size_t(0));
    }
}

CM_TEST(world_dat, zones_hull_in_model) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    std::vector<double> shares;
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
        const auto h = ex.parseHavok();
        std::vector<uint32_t> geomOf;
        for (const auto& p : h.placements)
            if (p.group == "zone") geomOf.push_back(p.geometryIndex);
        std::map<uint32_t, uint32_t> modelOfGeometry;
        for (const auto& i : w.zones.instances)
            if (i.collider < geomOf.size()) modelOfGeometry.emplace(geomOf[i.collider], i.model);
        for (const auto& [g, m] : modelOfGeometry) {
            if (g >= h.geometryAnimations.size() || h.geometryAnimations[g].empty()) continue;
            const uint32_t anim = h.geometryAnimations[g].back();
            if (anim >= h.animationCollisions.size()) continue;
            Box hull;
            for (uint32_t ci : h.animationCollisions[anim])
                if (ci < h.hulls.size())
                    for (size_t v = 0; v + 2 < h.hulls[ci].verts.size(); v += 3) {
                        const float* s = &h.hulls[ci].verts[v];
                        const float p[3] = {s[0] * 32.0f, s[1] * 32.0f, s[2] * -32.0f};
                        hull.add(p);
                    }
            auto mp = load_model_by_fileid(dat, w.models.at(m).fileId, ModelLoadOptions{false, false});
            Box vis;
            if (mp)
                for (const auto& mesh : mp->meshes)
                    for (const auto& v : mesh.vertices) {
                        const float p[3] = {v.px, v.py, v.pz};
                        vis.add(p);
                    }
            if (hull.empty() || vis.empty()) { std::printf("    map %u geometry %u: no hull or model box\n", id, g); continue; }
            const double c = containment(hull, vis);
            std::printf("    map %u geometry %u (fileId %u): hull inside model box %.3f\n", id, g,
                        w.models.at(m).fileId, c);
            shares.push_back(c);
        }
    }
    CHECK(!shares.empty());
    CHECK(median(shares) >= 0.95);
    CHECK(*std::min_element(shares.begin(), shares.end()) >= 0.8);
}

// ---- zones: phase-2 measurements (world foliage spec D5; print only, never fail) ----

namespace {

/// One anchor-matched zone collider and everything D5 lists as an input.
struct ZoneSample {
    uint32_t map = 0, entry = 0, zone = 0, placement = 0, k = 0;
    const castlemist::world::Zone* z = nullptr;
    const castlemist::world::ZonePlacement* p = nullptr;
    const castlemist::world::ZoneLayer* L = nullptr;
    const castlemist::model::Extractor::HavokPlacement* e = nullptr;
    float u = 0, v = 0;
};

/// The anchor-matched collider samples of one loaded map, in instance order.
/// `k` is the cell index, from re-walking the zone's flags (the placements are in walk order).
std::vector<ZoneSample> zone_samples(uint32_t id, const castlemist::world::WorldScene& w,
                                     const castlemist::model::Extractor::MapHavok& h,
                                     const castlemist::model::Extractor::MapZon2& raw) {
    namespace cw = castlemist::world;
    std::vector<const castlemist::model::Extractor::HavokPlacement*> entries;
    for (const auto& p : h.placements)
        if (p.group == "zone") entries.push_back(&p);
    std::vector<uint32_t> kOf(w.zones.placements.size(), UINT32_MAX);
    for (uint32_t zi = 0; zi < raw.zones.size() && zi < w.zones.zones.size(); ++zi) {
        uint32_t idx = w.zones.zones[zi].firstPlacement;
        castlemist::model::Extractor::walkZoneFlags(raw.zones[zi], [&](uint32_t k, int32_t, int32_t, uint8_t) {
            if (idx < kOf.size()) kOf[idx] = k;
            ++idx;
        });
    }
    std::vector<ZoneSample> out;
    for (const cw::ZoneInstance& i : w.zones.instances) {
        if (i.placement == UINT32_MAX || i.collider >= entries.size()) continue;
        ZoneSample s;
        s.map = id; s.entry = i.collider; s.placement = i.placement; s.k = kOf[i.placement];
        s.p = &w.zones.placements[i.placement];
        s.zone = s.p->zone; s.z = &w.zones.zones[s.zone]; s.L = &w.zones.layers[s.p->layer];
        s.e = entries[i.collider];
        float a[2];
        cw::zone_anchor(w.bounds[0], w.bounds[1], s.p->c, s.p->r, a);
        s.u = s.e->translate[0] - a[0];
        s.v = s.e->translate[1] - a[1];
        out.push_back(s);
    }
    return out;
}

/// The terrain gradient (dz/dx, dz/dy) at (x, y): central differences of terrain_height_at over +-h.
bool terrain_gradient(const castlemist::world::Terrain& t, float x, float y, float h, float g[2]) {
    bool a = false, b = false, c = false, d = false;
    const float zx1 = castlemist::world::terrain_height_at(t, x + h, y, &a);
    const float zx0 = castlemist::world::terrain_height_at(t, x - h, y, &b);
    const float zy1 = castlemist::world::terrain_height_at(t, x, y + h, &c);
    const float zy0 = castlemist::world::terrain_height_at(t, x, y - h, &d);
    if (!(a && b && c && d)) return false;
    g[0] = (zx1 - zx0) / (2 * h);
    g[1] = (zy1 - zy0) / (2 * h);
    return true;
}

} // namespace

/// Writes the 4750 anchor-matched collider samples as CSV to $CM_ZONE_SAMPLES (spec D5 step 1's
/// data fit). `k` is the cell index (Task 9 wrote the old walk's position, the cell + 1). The task-9 columns come first; after them: the slot's model flags and zOffsets
/// bytes as stored, the layer's layerFlags and tiling, the zone's vertRect, and the terrain
/// height and gradient (dz/dx, dz/dy, +-1 unit) at (tx, ty). Beside it, for the fits in
/// docs/research/gw2-terrain-to-unity.md §1.3 (2026-10-16): <path>.terrain.csv (4 x 4 terrain
/// samples per sample), <path>.zones.csv (every zone's raw flags, encodeData, offsetData,
/// collideData), <path>.layers.csv (every layer and its models) and <path>.unmatched.csv (the
/// colliders with no anchor match). Never fails.
CM_TEST(world_dat, zones_scatter_samples) {
    const char* path = std::getenv("CM_ZONE_SAMPLES");
    if (!path || !*path) SKIP("CM_ZONE_SAMPLES not set");
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    FILE* f = std::fopen(path, "w");
    if (!f) SKIP("cannot write CM_ZONE_SAMPLES");
    std::fprintf(f, "map,entry,zone,seed,placementIndex,k,c,r,flag,layerType,scale0,scale1,rot0,rot1,rot2,rot3,rot4,"
                    "rot5,jitter,range0,range1,zPos,zoneFlags,encodeIndex,encodeOffset,tx,ty,tz,rx,ry,rz,scale,u,v,"
                    "modelFlags,zOff0,zOff1,layerFlags,tiling,vr0,vr1,vr2,vr3,terrainZ,dzdx,dzdy\n");
    // <path>.terrain.csv: the 4 x 4 terrain samples around each sample's (tx, ty), for normal fits.
    FILE* ft = std::fopen((std::string(path) + ".terrain.csv").c_str(), "w");
    if (ft) std::fprintf(ft, "map,entry,X0,Y1,dx,dy,i0,j0,h00..h33 (row j, column i)\n");
    size_t rows = 0;
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
        const auto h = ex.parseHavok();
        const auto raw = ex.parseZon2();
        // <path>.zones.csv, one row per zone: map, zone, defToken, seed, zoneFlags, vertRect, then the
        // counted lists flags (raw bytes), encodeData (index, offset pairs), offsetData and collideData
        // (triples), for the walk, collideData-index and density fits.
        if (FILE* fc = std::fopen((std::string(path) + ".zones.csv").c_str(), id == kTestMaps[0] ? "w" : "a")) {
            for (uint32_t zi = 0; zi < raw.zones.size(); ++zi) {
                const auto& Z = raw.zones[zi];
                std::fprintf(fc, "%u,%u,%u,%u,%u,%d,%d,%d,%d", id, zi, Z.defToken, (unsigned)Z.seed, Z.zoneFlags,
                             Z.vertRect[0], Z.vertRect[1], Z.vertRect[2], Z.vertRect[3]);
                std::fprintf(fc, ",%zu", Z.flags.size());
                for (uint8_t b : Z.flags) std::fprintf(fc, ",%u", (unsigned)b);
                std::fprintf(fc, ",%zu", Z.encodeData.size());
                for (const auto& e : Z.encodeData) std::fprintf(fc, ",%u,%u", (unsigned)e.index, (unsigned)e.offset);
                std::fprintf(fc, ",%zu", Z.offsetData.size());
                for (uint16_t o : Z.offsetData) std::fprintf(fc, ",%u", (unsigned)o);
                std::fprintf(fc, ",%zu", Z.collideData.size() / 3);
                for (float c : Z.collideData) std::fprintf(fc, ",%.9g", c);
                std::fprintf(fc, "\n");
            }
            std::fclose(fc);
        }
        // <path>.layers.csv, one row per layer: map, defToken, layer (0-based), type, tiling, scaleRange,
        // rotRange, layerFlags, then per model: fileId, flags, zOffsets[0], zOffsets[1].
        if (FILE* fl = std::fopen((std::string(path) + ".layers.csv").c_str(), id == kTestMaps[0] ? "w" : "a")) {
            for (const auto& d : raw.defs)
                for (size_t li = 0; li < d.layers.size(); ++li) {
                    const auto& L = d.layers[li];
                    std::fprintf(fl, "%u,%u,%zu,%u,%u,%.9g,%.9g", id, d.token, li, (unsigned)L.type,
                                 (unsigned)L.tiling, L.scaleRange[0], L.scaleRange[1]);
                    for (int a = 0; a < 3; ++a) std::fprintf(fl, ",%.9g,%.9g", L.rotRange[a][0], L.rotRange[a][1]);
                    std::fprintf(fl, ",%u,%zu", L.layerFlags, L.models.size());
                    for (const auto& M : L.models)
                        std::fprintf(fl, ",%u,%u,%g,%g", M.fileId, M.flags, M.zOffsets[0], M.zOffsets[1]);
                    std::fprintf(fl, "\n");
                }
            std::fclose(fl);
        }
        // <path>.unmatched.csv: the zone colliders with no anchor match (linked by geometry only).
        if (FILE* fu = std::fopen((std::string(path) + ".unmatched.csv").c_str(), id == kTestMaps[0] ? "w" : "a")) {
            std::vector<const castlemist::model::Extractor::HavokPlacement*> entries;
            for (const auto& p : h.placements)
                if (p.group == "zone") entries.push_back(&p);
            for (const auto& i : w.zones.instances)
                if (i.placement == UINT32_MAX && i.collider < entries.size()) {
                    const auto* e = entries[i.collider];
                    std::fprintf(fu, "%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n", id, i.collider, e->translate[0],
                                 e->translate[1], e->translate[2], e->rotate[0], e->rotate[1], e->rotate[2], e->scale,
                                 w.models.at(i.model).fileId);
                }
            std::fclose(fu);
        }
        for (const ZoneSample& s : zone_samples(id, w, h, raw)) {
            if (ft && w.terrain.present && !w.terrain.chunks.empty()) {
                const auto& c0 = w.terrain.chunks.front();
                const float X0 = c0.rect[0], Y1 = c0.rect[3];
                const float dx = (c0.rect[2] - c0.rect[0]) / float(c0.samples - 1);
                const float dy = (c0.rect[3] - c0.rect[1]) / float(c0.samples - 1);
                const int i0 = (int)std::floor((s.e->translate[0] - X0) / dx);
                const int j0 = (int)std::floor((Y1 - s.e->translate[1]) / dy);
                std::fprintf(ft, "%u,%u,%.9g,%.9g,%.9g,%.9g,%d,%d", s.map, s.entry, X0, Y1, dx, dy, i0, j0);
                for (int j = j0 - 1; j <= j0 + 2; ++j)
                    for (int i = i0 - 1; i <= i0 + 2; ++i) {
                        bool in = false;
                        const float hz = castlemist::world::terrain_height_at(w.terrain, X0 + i * dx, Y1 - j * dy, &in);
                        std::fprintf(ft, ",%.9g", in ? hz : NAN);
                    }
                std::fprintf(ft, "\n");
            }
            const uint32_t row = uint32_t(s.p->r - s.z->vertRect[1]);
            const auto& enc = raw.zones[s.zone].encodeData;
            const int encIndex = row < enc.size() ? enc[row].index : -1;
            const int encOffset = row < enc.size() ? enc[row].offset : -1;
            const uint32_t slot = (s.p->flag >> 1) & 7;
            const castlemist::world::ZoneLayerModel* m = slot < s.L->models.size() ? &s.L->models[slot] : nullptr;
            bool in = false;
            const float tz = castlemist::world::terrain_height_at(w.terrain, s.e->translate[0], s.e->translate[1], &in);
            float g[2] = {NAN, NAN};
            terrain_gradient(w.terrain, s.e->translate[0], s.e->translate[1], 1.0f, g);
            std::fprintf(f, "%u,%u,%u,%u,%u,%u,%d,%d,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
                            "%u,%d,%d,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%g,%g,%u,%u,%d,%d,%d,%d,%.9g,%.9g,"
                            "%.9g\n",
                         s.map, s.entry, s.zone, (unsigned)s.z->seed, s.placement, s.k, s.p->c, s.p->r,
                         (unsigned)s.p->flag, (unsigned)s.L->type, s.L->scaleRange[0], s.L->scaleRange[1],
                         s.L->rotRange[0][0], s.L->rotRange[0][1], s.L->rotRange[1][0], s.L->rotRange[1][1],
                         s.L->rotRange[2][0], s.L->rotRange[2][1], s.L->instanceScaleJitter, s.z->range[0],
                         s.z->range[1], s.z->zPos, s.z->zoneFlags, encIndex, encOffset, s.e->translate[0],
                         s.e->translate[1], s.e->translate[2], s.e->rotate[0], s.e->rotate[1], s.e->rotate[2],
                         s.e->scale, s.u, s.v, m ? m->flags : 0u, m ? m->zOffsets[0] : 0.0f,
                         m ? m->zOffsets[1] : 0.0f, s.L->layerFlags, (unsigned)s.L->tiling, s.z->vertRect[0],
                         s.z->vertRect[1], s.z->vertRect[2], s.z->vertRect[3], in ? tz : NAN, g[0], g[1]);
            ++rows;
        }
    }
    std::fclose(f);
    std::printf("    %zu samples -> %s\n", rows, path);
}

namespace {

/// The client's decode of a zone's `flags` (Gw2-64.exe 0x140cd0660 / 0x140cd0410, ZnCompress.cpp):
/// with encodeData, (count, value) run-length pairs, `count` cells of `value`; without, one raw
/// byte per vertex. Returns the value per vertex position (row-major over vertRect), 0 = empty.
std::vector<uint8_t> zone_cells_client(const castlemist::model::Extractor::Zon2Zone& z) {
    const int32_t w = z.vertRect[2] - z.vertRect[0], h = z.vertRect[3] - z.vertRect[1];
    std::vector<uint8_t> out(w > 0 && h > 0 ? size_t(w) * h : 0, 0);
    if (z.encodeData.empty()) {
        for (size_t p = 0; p < out.size() && p < z.flags.size(); ++p) out[p] = z.flags[p];
        return out;
    }
    size_t pos = 0;
    for (size_t i = 0; i + 1 < z.flags.size(); i += 2)
        for (uint32_t n = 0; n < z.flags[i] && pos < out.size(); ++n) out[pos++] = z.flags[i + 1];
    return out;
}

/// collideData index of each cell (Gw2-64.exe 0x140cc23c0): offsetData[chunk] for the 32 x 32-vertex
/// chunk, plus the cell's order among the chunk's non-empty cells, rows then columns. UINT32_MAX: none.
std::vector<uint32_t> zone_collide_index(const castlemist::model::Extractor::Zon2Zone& z,
                                         const std::vector<uint8_t>& cells) {
    const int32_t* vr = z.vertRect;
    const int32_t w = vr[2] - vr[0];
    std::vector<uint32_t> out(cells.size(), UINT32_MAX);
    if (w <= 0 || vr[3] <= vr[1]) return out;
    const int32_t cx0 = vr[0] >> 5, cy0 = vr[1] >> 5, cx1 = ((vr[2] - 1) >> 5) + 1, cy1 = ((vr[3] - 1) >> 5) + 1;
    for (int32_t cy = cy0; cy < cy1; ++cy)
        for (int32_t cx = cx0; cx < cx1; ++cx) {
            const size_t ci = size_t(cx - cx0) + size_t(cy - cy0) * size_t(cx1 - cx0);
            if (ci >= z.offsetData.size()) continue;
            uint32_t n = z.offsetData[ci];
            for (int32_t row = std::max(vr[1], 32 * cy); row < std::min(vr[3], 32 * cy + 32); ++row)
                for (int32_t col = std::max(vr[0], 32 * cx); col < std::min(vr[2], 32 * cx + 32); ++col) {
                    const size_t p = size_t(row - vr[1]) * w + size_t(col - vr[0]);
                    if (p < cells.size() && cells[p]) out[p] = n++;
                }
        }
    return out;
}

/// Tilt angles of a ground normal as the client takes them (Gw2-64.exe 0x140cc2af0):
/// rx = -atan2(ny, |(nx, nz)|), ry = atan2(nx, |(ny, nz)|).
void tilt_from_normal(double nx, double ny, double nz, double out[2]) {
    const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
    nx /= l; ny /= l; nz /= l;
    out[0] = -std::atan2(ny, std::sqrt(nz * nz + nx * nx));
    out[1] = std::atan2(nx, std::sqrt(nz * nz + ny * ny));
}

double angle_error(double a, double b) {
    const double twoPi = 6.283185307179586;
    const double d = std::fmod(std::fabs(a - b), twoPi);
    return std::min(d, twoPi - d);
}

/// The terrain quad around (x, y): corner heights q = NW, NE, SW, SE, fractions fx (east) and fy
/// (south), and the sample spacing. False outside the terrain.
bool terrain_quad(const castlemist::world::Terrain& t, float x, float y, float q[4], float& fx, float& fy,
                  float& dx, float& dy) {
    if (!t.present || t.chunks.empty()) return false;
    const auto& c0 = t.chunks.front();
    const float X0 = c0.rect[0], Y1 = c0.rect[3];
    dx = (c0.rect[2] - c0.rect[0]) / float(c0.samples - 1);
    dy = (c0.rect[3] - c0.rect[1]) / float(c0.samples - 1);
    const int i0 = (int)std::floor((x - X0) / dx), j0 = (int)std::floor((Y1 - y) / dy);
    bool in[4] = {};
    q[0] = castlemist::world::terrain_height_at(t, X0 + i0 * dx, Y1 - j0 * dy, &in[0]);
    q[1] = castlemist::world::terrain_height_at(t, X0 + (i0 + 1) * dx, Y1 - j0 * dy, &in[1]);
    q[2] = castlemist::world::terrain_height_at(t, X0 + i0 * dx, Y1 - (j0 + 1) * dy, &in[2]);
    q[3] = castlemist::world::terrain_height_at(t, X0 + (i0 + 1) * dx, Y1 - (j0 + 1) * dy, &in[3]);
    fx = (x - (X0 + i0 * dx)) / dx;
    fy = ((Y1 - j0 * dy) - y) / dy;
    return in[0] && in[1] && in[2] && in[3];
}

struct Tally {
    size_t n = 0, pass = 0;
    double worst = 0;
    void add(double err, double tol) { ++n; pass += err <= tol; worst = std::max(worst, err); }
    const char* status() const { return n && pass == n ? "measured" : "UNPROVEN"; }
};

} // namespace

/// Prints spec D5 step 3's hypotheses on the 4750 anchor-matched colliders, each with its pass count,
/// worst error and status (`measured` only if every entry passes), plus the client-code variants the
/// static search found (docs/research/gw2-terrain-to-unity.md §1.3, 2026-10-16 amendment). Never fails.
CM_TEST(world_dat, zones_hypotheses) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    constexpr float kStep = 0.07734375f;   // 99/1280, Gw2-64.exe 0x141e449c4
    Tally tiltAsPhrased, tiltGroundClient, zTerrain, zTriangleInOffsets, zCollide, scaleStep, collideNormal;
    size_t ground = 0, collideSamples = 0;
    for (uint32_t id : kTestMaps) {
        castlemist::world::WorldScene w = castlemist::world::load_world(dat, id, *castlemist::tpl::get());
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
        const auto h = ex.parseHavok();
        const auto raw = ex.parseZon2();
        std::map<uint32_t, std::pair<std::vector<uint8_t>, std::vector<uint32_t>>> decoded;
        for (const ZoneSample& s : zone_samples(id, w, h, raw)) {
            const float x = s.e->translate[0], y = s.e->translate[1], tz = s.e->translate[2];
            const double rx = s.e->rotate[0], ry = s.e->rotate[1];
            const uint32_t slot = (s.p->flag >> 1) & 7;
            const bool hasSlot = slot < s.L->models.size();
            const uint32_t mf = hasSlot ? s.L->models[slot].flags : 0;
            const float z0 = hasSlot ? s.L->models[slot].zOffsets[0] : 0;
            const float z1 = hasSlot ? s.L->models[slot].zOffsets[1] : 0;
            // zOffset = 36 - (z0 + r (z1 - z0)) / 255 * 72, r in [0, 1) (Gw2-64.exe 0x140cbc110)
            const float zoffA = 36.0f - z0 / 255.0f * 72.0f, zoffB = 36.0f - z1 / 255.0f * 72.0f;
            auto zoffError = [&](float dz) {
                const float lo = std::min(zoffA, zoffB), hi = std::max(zoffA, zoffB);
                return dz < lo ? lo - dz : (dz > hi ? dz - hi : 0.0f);
            };
            // Pack model flag bits (remapped by Gw2-64.exe 0x140c8b9b0): 0x2 / 0x200000 / 0x4 align to
            // the ground, 0x8 / 0x20000 draw the tilt from rotRange (and win); 0x4 scales the tilt by 0.35.
            const bool randomTilt = (mf & (0x8u | 0x20000u)) != 0;
            const bool groundTilt = !randomTilt && (mf & (0x2u | 0x200000u | 0x4u)) != 0;
            const double tiltScale = (mf & 0x4u) ? 0.35 : 1.0;
            const bool collide = (s.z->zoneFlags & 2) != 0;

            // H1 as phrased: tilt = terrain normal (bilinear gradient), clamped to rotRange x/y.
            float g[2];
            if (terrain_gradient(w.terrain, x, y, 1.0f, g)) {
                double t[2];
                tilt_from_normal(g[0], g[1], 1.0, t);
                for (int a = 0; a < 2; ++a)
                    t[a] = std::clamp(t[a], (double)s.L->rotRange[a][0] * kDeg, (double)s.L->rotRange[a][1] * kDeg);
                tiltAsPhrased.add(std::max(angle_error(rx, t[0]), angle_error(ry, t[1])), 1e-4);
            } else tiltAsPhrased.add(1e9, 1e-4);

            // H2 as phrased: z = terrain_height_at.
            bool in = false;
            const float th = castlemist::world::terrain_height_at(w.terrain, x, y, &in);
            zTerrain.add(in ? std::fabs(tz - th) : 1e9, 0.5);

            // H3: scale = scaleRange[0] + (99/1280) k for an integer k.
            {
                const double q = (s.e->scale - s.L->scaleRange[0]) / kStep;
                scaleStep.add(std::fabs(q - std::round(q)) * kStep, 1e-5);
            }

            // H4 (collideData) and the client variants.
            if (collide) {
                ++collideSamples;
                auto& dc = decoded[s.zone];
                if (dc.first.empty()) {
                    dc.first = zone_cells_client(raw.zones[s.zone]);
                    dc.second = zone_collide_index(raw.zones[s.zone], dc.first);
                }
                const uint32_t ci = s.k < dc.second.size() ? dc.second[s.k] : UINT32_MAX;
                const auto& cd = raw.zones[s.zone].collideData;
                if (ci != UINT32_MAX && 3ull * ci + 2 < cd.size()) {
                    const float nx = cd[3ull * ci], ny = cd[3ull * ci + 1], cz = cd[3ull * ci + 2];
                    zCollide.add(zoffError(tz - cz), 0.5);
                    if (groundTilt) {
                        double t[2];
                        tilt_from_normal(nx, ny, std::sqrt(std::max(0.0f, 1 - nx * nx - ny * ny)), t);
                        collideNormal.add(
                            std::max(angle_error(rx, t[0] * tiltScale), angle_error(ry, t[1] * tiltScale)), 1e-4);
                    }
                } else zCollide.add(1e9, 0.5);
                continue;
            }
            float q[4], fx, fy, dx, dy;
            if (!terrain_quad(w.terrain, x, y, q, fx, fy, dx, dy)) { zTriangleInOffsets.add(1e9, 0.5); continue; }
            // height: the NE-SW triangle that holds the point
            const float tri = fx + fy <= 1 ? q[0] + (q[1] - q[0]) * fx + (q[2] - q[0]) * fy
                                           : q[3] + (q[2] - q[3]) * (1 - fx) + (q[1] - q[3]) * (1 - fy);
            zTriangleInOffsets.add(zoffError(tz - tri), 0.5);
            if (groundTilt) {
                ++ground;
                // normal: the plane through (NW, NE, SW) when fx <= fy, else through (NE, SW, SE)
                const double gx = fx <= fy ? (q[1] - q[0]) / dx : (q[3] - q[2]) / dx;
                const double gy = fx <= fy ? -(q[2] - q[0]) / dy : -(q[3] - q[1]) / dy;
                double t[2];
                tilt_from_normal(gx, gy, 1.0, t);
                tiltGroundClient.add(std::max(angle_error(rx, t[0] * tiltScale), angle_error(ry, t[1] * tiltScale)),
                                     1e-4);
            }
        }
    }
    auto line = [](const char* what, const Tally& t, const char* unit) {
        std::printf("    %-60s %4zu/%-4zu worst %.3g %s  %s\n", what, t.pass, t.n, t.worst, unit, t.status());
    };
    std::printf("    D5 step 3, as phrased (all anchor-matched colliders):\n");
    line("tilt = terrain normal, clamped to rotRange x/y", tiltAsPhrased, "rad");
    line("z = terrain_height_at", zTerrain, "units");
    line("scale = scaleRange[0] + (99/1280) k", scaleStep, "");
    std::printf("    collideData = the ground (zoneFlags bit 1; %zu samples):\n", collideSamples);
    line("z = collideData.zPos + the slot's zOffset range", zCollide, "units");
    line("tilt = collideData normal (ground-aligned models)", collideNormal, "rad");
    std::printf("    client variants (zones without collideData; %zu ground-aligned):\n", ground);
    line("z = NE-SW terrain triangle + the slot's zOffset range", zTriangleInOffsets, "units");
    line("tilt = client plane (fx <= fy: NW,NE,SW; else NE,SW,SE)", tiltGroundClient, "rad");
}

// ---- zones: phase 2, the exact scatter (world foliage spec D5; ruling S3) ----

/// Spec D5 step 2 against the rule in force (`exact`, zone_scatter.h): every anchor-matched
/// collider entry is reproduced by the client's rule on its placement, to the D5 tolerances
/// (x, y 0.01; yaw and tilt 1e-4 rad; scale 1e-5; z 0.5). Queensdale's entries 3350-3352 (spec M2's
/// "hand-placed", the three type-3 cells of zone 135 under the client decode) are listed apart: Task 9
/// did not check their tilt and z. Also: every Scatter instance belongs to a placement without a
/// collider (never doubled), every placement without a collider and with a model has one, and the
/// hypotheses Task 9 settled are printed for the amendments.
CM_TEST(world_dat, zones_phase2) {
    namespace cw = castlemist::world;
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    Tally tx, tyaw, ttilt, tscale, tz;
    size_t matched = 0, apart = 0, apartOk = 0;
    for (uint32_t id : kTestMaps) {
        const auto t0 = std::chrono::steady_clock::now();
        cw::WorldScene w = cw::load_world(dat, id, *castlemist::tpl::get());
        const double loadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const cw::ZoneScatter& z = w.zones;
        CHECK(z.mode == cw::ZoneScatterMode::Exact);
        std::vector<uint8_t> bytes = packfile_by_file_id(id);
        castlemist::model::Extractor ex(bytes, *castlemist::tpl::get());
        const auto h = ex.parseHavok();
        std::vector<const castlemist::model::Extractor::HavokPlacement*> entries;
        for (const auto& p : h.placements)
            if (p.group == "zone") entries.push_back(&p);

        std::map<uint32_t, std::vector<uint32_t>> collideIndex;   // zone -> zone_collide_index
        const cw::ZoneScatterRule rule = cw::zone_scatter_rule_in_force();
        std::vector<cw::ZoneScatterDraw> d;
        size_t bySource[3] = {0, 0, 0};
        for (const cw::ZoneInstance& i : z.instances) {
            ++bySource[size_t(i.source) < 3 ? size_t(i.source) : 0];
            if (i.source != cw::ZoneSource::Collider || i.placement == UINT32_MAX || i.collider >= entries.size())
                continue;
            const cw::ZonePlacement& p = z.placements[i.placement];
            auto it = collideIndex.find(p.zone);
            if (it == collideIndex.end())
                it = collideIndex.emplace(p.zone, (z.zones[p.zone].zoneFlags & 2u) ? cw::zone_collide_index(z, p.zone)
                                                                                    : std::vector<uint32_t>())
                         .first;
            d.clear();
            cw::zone_scatter_exact(w, i.placement, rule, it->second, d);
            const auto* e = entries[i.collider];
            if (d.empty()) {
                std::printf("    map %u entry %u: the rule makes no instance\n", id, i.collider);
                tx.add(1e9, 0.01);
                continue;
            }
            const cw::ZoneScatterDraw& q = d[0];
            const double ex_ = std::max(std::fabs(q.pos[0] - e->translate[0]), std::fabs(q.pos[1] - e->translate[1]));
            const double eyaw = angle_error(q.rot[2], e->rotate[2]);
            const double etilt = std::max(angle_error(q.rot[0], e->rotate[0]), angle_error(q.rot[1], e->rotate[1]));
            const double escale = std::fabs(q.scale - e->scale), ez = std::fabs(q.pos[2] - e->translate[2]);
            if (id == 192711 && i.collider >= 3350 && i.collider <= 3352) {
                ++apart;
                const bool ok = ex_ <= 0.01 && eyaw <= 1e-4 && escale <= 1e-5;
                apartOk += ok;
                std::printf("    192711 entry %u (zone %u, cell %d,%d): xy err %.4g, yaw %.3g, scale %.3g (%s); "
                            "tilt %.3g, z %.3g (not in the 4750)\n",
                            i.collider, p.zone, p.c, p.r, ex_, eyaw, escale, ok ? "ok" : "FAIL", etilt, ez);
                continue;
            }
            ++matched;
            tx.add(ex_, 0.01);
            tyaw.add(eyaw, 1e-4);
            ttilt.add(etilt, 1e-4);
            tscale.add(escale, 1e-5);
            tz.add(ez, 0.5);
        }

        // Never doubled; every uncollided placement with a model has its instance(s).
        std::vector<uint32_t> scatterOf(z.placements.size(), 0);
        size_t doubled = 0, missing = 0, expected = 0, subModel = 0;
        for (const cw::ZoneInstance& i : z.instances)
            if (i.source == cw::ZoneSource::Scatter && i.placement < scatterOf.size()) {
                ++scatterOf[i.placement];
                doubled += z.placements[i.placement].collider != UINT32_MAX;
            }
        for (uint32_t pi = 0; pi < z.placements.size(); ++pi) {
            const cw::ZonePlacement& p = z.placements[pi];
            if (p.collider != UINT32_MAX || p.layer >= z.layers.size()) continue;
            const auto& models = z.layers[p.layer].models;
            const uint32_t slot = (p.flag >> 1) & 7u;
            if (slot >= models.size() || !models[slot].fileId) continue;
            ++expected;
            missing += scatterOf[pi] == 0;
        }
        for (const cw::ZoneLayer& L : z.layers) subModel += L.hasSubModel;
        // The scatter's own cost: re-run it on a copy holding the Collider instances only (they come first).
        double scatterMs = 0;
        {
            cw::WorldScene c = w;
            c.zones.instances.resize(bySource[0]);
            const auto s0 = std::chrono::steady_clock::now();
            const size_t again = cw::scatter_instances(c, rule);
            scatterMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
            CHECK_EQ(again, bySource[1]);
        }
        std::printf("    map %u: load_world %.2f s, of it scatter_instances %.0f ms; instances collider %zu, scatter %zu, "
                    "density %zu (%.1f MB); placements without a collider %zu, of them without an instance %zu; "
                    "collided with a Scatter instance %zu; layers with a subModel %zu; models %zu\n",
                    id, loadS, scatterMs, bySource[0], bySource[1], bySource[2],
                    double(z.instances.size() * sizeof(cw::ZoneInstance)) / 1048576.0, expected, missing, doubled,
                    subModel, w.models.size());
        {   // print only: Scatter models that do not load (the Map Window counts them as failed)
            std::map<uint32_t, size_t> scatterModels;   // model -> instances
            for (const cw::ZoneInstance& i : z.instances)
                if (i.source == cw::ZoneSource::Scatter) ++scatterModels[i.model];
            size_t noMesh = 0;
            for (const auto& [m, n] : scatterModels) {
                auto mp = load_model_by_fileid(dat, w.models.at(m).fileId, ModelLoadOptions{false, false});
                bool ok = false;
                if (mp)
                    for (const auto& mesh : mp->meshes) ok = ok || (!mesh.indices.empty() && !mesh.vertices.empty());
                if (!ok) {
                    ++noMesh;
                    std::printf("    map %u: Scatter model fileId %u has no mesh (%zu instances)\n", id,
                                w.models.at(m).fileId, n);
                }
            }
            std::printf("    map %u: %zu distinct Scatter models, %zu without a mesh\n", id, scatterModels.size(), noMesh);
        }
        for (const std::string& s : w.warnings)
            if (s.find("UNPROVEN branches") != std::string::npos || s.find("tiling path") != std::string::npos ||
                s.find("outside the terrain") != std::string::npos || s.find("water-tested") != std::string::npos ||
                s.find("collideData entry") != std::string::npos || s.find("tiling outside") != std::string::npos)
                std::printf("      %s\n", s.c_str());
        CHECK(bySource[1] > 0);
        CHECK_EQ(bySource[2], size_t(0));
        CHECK_EQ(doubled, size_t(0));
        CHECK_EQ(missing, size_t(0));
        CHECK_EQ(bySource[0], size_t(kZoneAccept.at(id).colliders));
    }
    auto line = [](const char* what, const Tally& t, const char* unit) {
        std::printf("    %-8s %4zu/%-4zu worst %.3g %s\n", what, t.pass, t.n, t.worst, unit);
    };
    std::printf("    D5 step 2, exact rule vs the anchor-matched colliders (%zu):\n", matched);
    line("x, y", tx, "units");
    line("yaw", tyaw, "rad");
    line("tilt", ttilt, "rad");
    line("scale", tscale, "");
    line("z", tz, "units");
    CHECK_EQ(matched, size_t(4750));
    for (const Tally* t : {&tx, &tyaw, &ttilt, &tscale, &tz}) CHECK_EQ(t->pass, t->n);
    CHECK_EQ(apart, size_t(3));
    CHECK_EQ(apartOk, size_t(3));
    // The D5 step 3 hypotheses Task 9 settled (live numbers: world_dat.zones_hypotheses).
    std::printf("    hypotheses (Task 9, docs/research/gw2-terrain-to-unity.md \xc2\xa7" "1.3, 2026-10-16):\n"
                "      tilt = terrain normal clamped to rotRange: UNPROVEN as phrased (tilt is per model flag)\n"
                "      z = terrain_height_at: UNPROVEN as phrased (z = NE-SW triangle + zOffset: measured)\n"
                "      scale = scaleRange[0] + (99/1280) k: measured\n"
                "      collideData = the ground (z, normal): measured\n");
}
