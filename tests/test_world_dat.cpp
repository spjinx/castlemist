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
