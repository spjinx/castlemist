/// @file
/// @brief The world folder export against a real Gw2.dat (sub-project 4, Tasks 8 and 13).
///
/// Skips without the archive (GW2_TEST_DAT) or the struct template
/// (GW2_TEST_TEMPLATE, or dumps/packfile/gw2_packfile.json near the build).
///
/// world_export_dat.spirit_vale exports Spirit Vale with the defaults (no FBX)
/// and holds the folder to its WorldScene (spec "Testing", Dat).
/// world_export_dat.measure (prints only, never fails) exports Queensdale,
/// Lion's Arch and Spirit Vale and prints the budget and size measurements; it
/// writes several GB and takes minutes, so it runs only with
/// CM_WORLD_EXPORT_MEASURE=1.
///
/// world_export_dat.spirit_vale_smoke runs only when CM_WORLD_EXPORT_SMOKE_DIR
/// names an output folder (replaced); CM_WORLD_EXPORT_SMOKE_WANTGAME=1 loads
/// the models with the game shaders traced. It prints the report.

#include "test_framework.h"

#include "castlemist/exportgltf/sky_export.h"
#include "castlemist/exportgltf/world_dat_source.h"
#include "castlemist/exportgltf/world_folder.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/world/cells.h"
#include "castlemist/world/collision.h"
#include "castlemist/world/dat_read.h"
#include "castlemist/world/load_world.h"
#include "castlemist/world/walk.h"
#include "long_path.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "stb_image.h"

namespace {

namespace fs = std::filesystem;
namespace cw = castlemist::world;
namespace eg = castlemist::exportgltf;
using nlohmann::json;

constexpr const char* kDefaultDat = R"(C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat)";

Gw2Dat& shared_dat() {
    static Gw2Dat dat;
    static bool tried = false, ok = false;
    if (!tried) {
        tried = true;
        const char* env = std::getenv("GW2_TEST_DAT");
        const std::string path = (env && *env) ? env : kDefaultDat;
        if (FILE* f = std::fopen(path.c_str(), "rb")) {
            std::fclose(f);
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

bool ensure_template() {
    if (castlemist::tpl::get()) return true;
    if (castlemist::tpl::auto_load()) return true;
    std::string err;
    if (const char* env = std::getenv("GW2_TEST_TEMPLATE"); env && *env)
        if (castlemist::tpl::load_from_file(env, err)) return true;
    for (const char* c : {"../../../dumps/packfile/gw2_packfile.json", "dumps/packfile/gw2_packfile.json",
                          "../dumps/packfile/gw2_packfile.json"})
        if (castlemist::tpl::load_from_file(c, err)) return true;
    return false;
}

fs::path fresh_dir(const std::string& label) {
    const fs::path d = fs::temp_directory_path() / ("cm_world_export_dat_" + label);
    std::error_code ec;
    // remove_tree, not fs::remove_all: libstdc++'s remove_all spins forever on a
    // tree holding paths past MAX_PATH, which the export now writes (review I4).
    eg::remove_tree(d, ec);
    fs::create_directories(d, ec);
    return d;
}

json read_json(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return json();
    try { return json::parse(in); } catch (...) { return json(); }
}

/// One prop of @p fileId at the origin, no terrain (one cell c_all).
cw::WorldScene one_model_scene(uint32_t fileId) {
    cw::WorldScene s;
    s.models = {cw::PropModel{fileId}};
    cw::PropInstance p;
    p.model = 0;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.group = "propArray";
    s.props.push_back(p);
    return s;
}

/// A .glb's JSON chunk and BIN chunk (empty when absent or unreadable).
struct Glb { json j; std::vector<uint8_t> bin; };

Glb read_glb(const fs::path& p) {
    Glb g;
    std::ifstream in(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t i) {
        return static_cast<uint32_t>(b[i]) | (static_cast<uint32_t>(b[i + 1]) << 8) |
               (static_cast<uint32_t>(b[i + 2]) << 16) | (static_cast<uint32_t>(b[i + 3]) << 24);
    };
    if (b.size() < 20) return g;
    const size_t jl = u32(12);
    if (20 + jl > b.size()) return g;
    try { g.j = json::parse(std::string(reinterpret_cast<const char*>(&b[20]), jl)); } catch (...) { return g; }
    const size_t bo = 20 + jl;
    if (bo + 8 <= b.size()) {
        const size_t bl = u32(bo);
        if (u32(bo + 4) == 0x004E4942u && bo + 8 + bl <= b.size())
            g.bin.assign(b.begin() + static_cast<std::ptrdiff_t>(bo + 8), b.begin() + static_cast<std::ptrdiff_t>(bo + 8 + bl));
    }
    return g;
}

/// Accessor @p ai as floats (FLOAT components), @p comps per element.
std::vector<float> accessor_floats(const Glb& g, size_t ai, int comps) {
    std::vector<float> out;
    const json& a = g.j["accessors"][ai];
    if (a.value("componentType", 0) != 5126) return out;
    const json& bv = g.j["bufferViews"][a["bufferView"].get<size_t>()];
    const size_t n = a["count"].get<size_t>();
    const size_t base = bv.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
    const size_t stride = bv.value("byteStride", size_t(comps) * 4);
    out.resize(n * size_t(comps));
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < comps; ++c) {
            const size_t o = base + i * stride + size_t(c) * 4;
            if (o + 4 <= g.bin.size()) std::memcpy(&out[i * size_t(comps) + size_t(c)], &g.bin[o], 4);
        }
    return out;
}

/// Accessor @p ai as indices (UNSIGNED_BYTE, SHORT or INT).
std::vector<uint32_t> accessor_indices(const Glb& g, size_t ai) {
    std::vector<uint32_t> out;
    const json& a = g.j["accessors"][ai];
    const int ct = a.value("componentType", 0);
    const size_t w = ct == 5121 ? 1 : ct == 5123 ? 2 : ct == 5125 ? 4 : 0;
    if (!w) return out;
    const json& bv = g.j["bufferViews"][a["bufferView"].get<size_t>()];
    const size_t n = a["count"].get<size_t>();
    const size_t base = bv.value("byteOffset", size_t{0}) + a.value("byteOffset", size_t{0});
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        const size_t o = base + i * w;
        if (o + w <= g.bin.size()) std::memcpy(&v, &g.bin[o], w);   // little-endian host
        out[i] = v;
    }
    return out;
}

/// Appends the triangles (9 floats each) of every primitive of the meshes on the
/// nodes whose name satisfies @p want, each vertex plus @p origin (de-localised).
template <typename Want>
void glb_triangles(const Glb& g, Want want, const float origin[3], std::vector<float>& out) {
    if (!g.j.contains("nodes")) return;
    for (const json& n : g.j["nodes"]) {
        if (!n.contains("mesh") || !want(n.value("name", std::string()))) continue;
        for (const json& prim : g.j["meshes"][n["mesh"].get<size_t>()]["primitives"]) {
            if (prim.value("mode", 4) != 4) continue;
            const std::vector<float> pos = accessor_floats(g, prim["attributes"]["POSITION"].get<size_t>(), 3);
            std::vector<uint32_t> idx;
            if (prim.contains("indices")) idx = accessor_indices(g, prim["indices"].get<size_t>());
            else for (uint32_t i = 0; i < pos.size() / 3; ++i) idx.push_back(i);
            for (size_t t = 0; t + 2 < idx.size(); t += 3)
                for (size_t v = 0; v < 3; ++v)
                    for (size_t c = 0; c < 3; ++c) out.push_back(pos[size_t(idx[t + v]) * 3 + c] + origin[c]);
        }
    }
}

/// The texture pool fileIds a model glb references (../../textures/<fileId>.png).
std::vector<uint32_t> glb_pool_textures(const Glb& g) {
    std::vector<uint32_t> ids;
    if (!g.j.contains("images")) return ids;
    for (const json& im : g.j["images"]) {
        const std::string u = im.value("uri", std::string());
        const size_t k = u.rfind("textures/");
        if (k == std::string::npos || u.size() < k + 13 || u.compare(u.size() - 4, 4, ".png") != 0) continue;
        try { ids.push_back(static_cast<uint32_t>(std::stoul(u.substr(k + 9, u.size() - 4 - k - 9)))); } catch (...) {}
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

/// True when a primitive of the glb has no material (Unity gives it Default-Material).
bool glb_has_unmaterialled_primitive(const Glb& g) {
    if (!g.j.contains("meshes")) return false;
    for (const json& m : g.j["meshes"])
        for (const json& p : m["primitives"])
            if (!p.contains("material")) return true;
    return false;
}

/// A PNG's IHDR size; false when it is not a PNG.
bool png_size(const fs::path& p, int& w, int& h) {
    std::ifstream in(p, std::ios::binary);
    unsigned char b[24];
    if (!in.read(reinterpret_cast<char*>(b), 24) || b[1] != 'P' || b[2] != 'N' || b[3] != 'G') return false;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return true;
}

/// A manifest-relative path (UTF-8, '/') under @p dir.
fs::path under(const fs::path& dir, const json& rel) {
    const std::string s = rel.get<std::string>();
    return dir / fs::path(std::u8string(s.begin(), s.end()));
}

/// A fixed LCG in [0, 1) (Numerical Recipes constants), so the rays repeat run to run.
struct Lcg {
    uint32_t s;
    float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / float(1u << 24); }
};

/// One map through the CLI's `world-export --out-dir --no-fbx` path: load_world,
/// the sky inputs, the dat model source (wantGame default) and default options.
struct MapExport {
    cw::WorldScene scene;
    eg::WorldFolderReport rep;
    json manifest;
    double tLoad = 0, tSky = 0, tExport = 0;
};

MapExport export_map(Gw2Dat& dat, uint32_t mapId, const fs::path& dir) {
    MapExport x;
    const json& tpl = *castlemist::tpl::get();
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&] {
        const auto now = std::chrono::steady_clock::now();
        const double s = std::chrono::duration<double>(now - t0).count();
        t0 = now;
        return s;
    };
    x.scene = cw::load_world(dat, mapId, tpl);
    x.tLoad = lap();
    std::optional<eg::sky::SkyInputs> sky;
    try {
        if (auto bytes = cw::read_file_bytes(dat, mapId)) sky = eg::sky::load_sky_inputs(dat, *bytes, tpl, mapId);
    } catch (const std::exception&) {}
    x.tSky = lap();
    auto src = eg::make_dat_model_source(dat);
    if (!src) return x;
    eg::WorldFolderOptions o;
    x.rep = eg::export_world_folder(x.scene, *src, sky ? &*sky : nullptr, dir.string(), o);
    x.tExport = lap();
    x.manifest = read_json(dir / "manifest.json");
    return x;
}

/// The CellStats the export partitioned with, rebuilt from the folder: model
/// triangles from the manifest, each model's textures from its glb's pool URIs,
/// their bytes (RGBA8 + mips at the cap) from the pool PNG sizes. The measure
/// test checks the rebuild by partitioning again and comparing every cell.
cw::CellStats stats_from_folder(const cw::WorldScene& s, const json& man, const fs::path& dir,
                                const eg::WorldFolderOptions& o) {
    cw::CellStats st;
    st.modelTriangles.assign(s.models.size(), 0);
    st.modelTextures.assign(s.models.size(), {});
    for (const json& m : man["models"]) {
        if (m["status"] != "ok") continue;
        const size_t i = m["index"].get<size_t>();
        if (i >= s.models.size()) continue;
        st.modelTriangles[i] = m["triangles"].get<uint64_t>();
        st.modelTextures[i] = glb_pool_textures(read_glb(under(dir, m["glb"])));
        for (uint32_t f : st.modelTextures[i]) {
            if (st.textureBytes.count(f)) continue;
            int w = 0, h = 0;
            if (png_size(dir / "textures" / (std::to_string(f) + ".png"), w, h))
                st.textureBytes[f] = uint64_t(w) * uint64_t(h) * 4 * 4 / 3;
        }
    }
    st.bakedBytesPerChunk = uint64_t(o.texelsPerChunk) * o.texelsPerChunk * 3 +
                            uint64_t(o.waterTexelsPerChunk) * o.waterTexelsPerChunk * 4;
    return st;
}

} // namespace

// The spec's measurement (Task 8): materials.json of model 1766522 with
// wantGame false against true. Measured unequal (no render state without the
// game trace: blend, preset, cull, emission differ), so by the spec's rule the
// dat source defaults to wantGame = true; the test prints the difference and
// holds the default to the traced result.
CM_TEST(world_export_dat, want_game_default_traced) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    constexpr uint32_t kModel = 1766522;
    const cw::WorldScene s = one_model_scene(kModel);
    eg::WorldFolderOptions o;
    o.threads = 1;
    json mats[3];   // wantGame false, true, the default
    const char* labels[3] = {"want_game_false", "want_game_true", "want_game_default"};
    for (int g = 0; g < 3; ++g) {
        auto src = g == 2 ? eg::make_dat_model_source(dat) : eg::make_dat_model_source(dat, /*wantGame=*/g == 1);
        CHECK(src != nullptr);
        if (!src) return;
        const fs::path dir = fresh_dir(labels[g]);
        const auto t0 = std::chrono::steady_clock::now();
        const eg::WorldFolderReport r = eg::export_world_folder(s, *src, nullptr, dir.string(), o);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.ok);
        CHECK_EQ(r.modelsWritten, size_t(1));
        std::printf("    %s: %.2f s\n", labels[g], sec);
        mats[g] = read_json(dir / "models" / "1766522" / "materials.json");
        CHECK(mats[g].is_object());
        mats[g].erase("castlemist");
    }
    // The basis of the default: false and true differ (no render state without
    // the trace). Equal again would mean the decision should be revisited.
    CHECK(mats[0] != mats[1]);
    if (mats[0] != mats[1]) {
        const json patch = json::diff(mats[0], mats[1]);
        std::printf("    wantGame false -> true: materials.json differs (%zu ops): %s\n", patch.size(),
                    patch.dump().substr(0, 600).c_str());
    } else {
        std::printf("    wantGame false and true give the same materials.json\n");
    }
    CHECK(mats[2] == mats[1]);
}

CM_TEST(world_export_dat, spirit_vale_smoke) {
    const char* out = std::getenv("CM_WORLD_EXPORT_SMOKE_DIR");
    if (!out || !*out) SKIP("set CM_WORLD_EXPORT_SMOKE_DIR to run the Spirit Vale smoke export");
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    const char* wg = std::getenv("CM_WORLD_EXPORT_SMOKE_WANTGAME");
    const bool wantGame = wg && std::string(wg) == "1";
    constexpr uint32_t kSpiritVale = 1151420;
    const auto t0 = std::chrono::steady_clock::now();
    auto secs = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    const json& tpl = *castlemist::tpl::get();
    const cw::WorldScene scene = cw::load_world(dat, kSpiritVale, tpl);
    const double tWorld = secs();
    eg::sky::SkyInputs sky;
    if (auto bytes = cw::read_file_bytes(dat, kSpiritVale)) sky = eg::sky::load_sky_inputs(dat, *bytes, tpl, kSpiritVale);
    const double tSky = secs();
    auto src = eg::make_dat_model_source(dat, wantGame);
    CHECK(src != nullptr);
    if (!src) return;
    std::error_code ec;
    eg::remove_tree(fs::path(out), ec);
    eg::WorldFolderOptions o;
    const eg::WorldFolderReport r = eg::export_world_folder(scene, *src, &sky, out, o);
    const double tAll = secs();
    json j = eg::report_json(r);
    j.erase("warnings");
    std::printf("    wantGame=%d load_world %.2f s, sky inputs %.2f s, export %.2f s, total %.2f s\n", int(wantGame),
                tWorld, tSky - tWorld, tAll - tSky, tAll);
    std::printf("    %s\n    warnings: %zu\n", j.dump().c_str(), r.warnings.size());
    size_t files = 0;
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(fs::path(out), ec); it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) { ++files; total += it->file_size(ec); }
    std::printf("    files %zu, bytes %llu\n", files, static_cast<unsigned long long>(total));
    CHECK(r.ok);
}

// Spec "Testing", Dat: Spirit Vale exported with the defaults (no FBX) against
// its WorldScene. 106 is sub-project 3's measured animatedResolved.
CM_TEST(world_export_dat, spirit_vale) {
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    constexpr uint32_t kSpiritVale = 1151420;
    const fs::path dir = fresh_dir("spirit_vale");
    const MapExport x = export_map(dat, kSpiritVale, dir);
    const cw::WorldScene& scene = x.scene;
    const json& man = x.manifest;
    std::printf("    load_world %.1f s, sky inputs %.1f s, export %.1f s; %s\n", x.tLoad, x.tSky, x.tExport,
                man.is_object() ? man["counts"].dump().c_str() : "no manifest");
    if (!x.rep.ok) std::printf("    error: %s\n", x.rep.error.c_str());
    CHECK(x.rep.ok);
    CHECK(man.is_object());
    const json& counts = man["counts"];
    CHECK_EQ(counts["props"].get<size_t>(), scene.props.size());
    CHECK_EQ(counts["chunks"].get<int>(), 250);
    CHECK_EQ(counts["chunksUnbaked"].get<int>(), 0);
    CHECK_EQ(counts["animatedResolved"].get<int>(), 106);

    // Every scene.models fileId appears exactly once in `models`.
    std::map<uint32_t, int> seen;
    for (const json& m : man["models"]) ++seen[m["fileId"].get<uint32_t>()];
    size_t missing = 0, repeated = 0;
    for (const cw::PropModel& pm : scene.models) {
        const auto it = seen.find(pm.fileId);
        if (it == seen.end()) ++missing;
        else if (it->second != 1) ++repeated;
    }
    std::printf("    scene.models %zu, manifest models %zu, missing %zu, repeated %zu\n", scene.models.size(),
                man["models"].size(), missing, repeated);
    for (const json& m : man["models"])
        if (m["status"] != "ok")
            std::printf("    failed model %u: %s\n", m["fileId"].get<uint32_t>(), m.value("error", std::string()).c_str());
    CHECK_EQ(missing, size_t(0));
    CHECK_EQ(repeated, size_t(0));
    CHECK_EQ(man["models"].size(), scene.models.size());

    // The cells' collider triangles total the scene's collision triangles.
    size_t cellTris = 0;
    for (const json& c : man["cells"]) cellTris += c["colliderTriangles"].get<size_t>();
    size_t sceneTris = 0;
    {
        std::vector<float> t;
        for (size_t i = 0; i < scene.collision.instances.size(); ++i) {
            t.clear();
            cw::collision_instance_triangles(scene, i, t);
            sceneTris += t.size() / 9;
        }
    }
    std::printf("    collider triangles: cells %zu, scene %zu\n", cellTris, sceneTris);
    CHECK_EQ(cellTris, sceneTris);

    // No baked terrain albedo is all black.
    size_t black = 0, albedos = 0;
    for (const json& c : man["cells"]) {
        if (!c["albedo"].is_string()) continue;
        ++albedos;
        int w = 0, h = 0, comp = 0;
        const std::string p = under(dir, c["albedo"]).string();
        std::ifstream in(under(dir, c["albedo"]), std::ios::binary);
        const std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // castlemist's stb_image is built with STBI_NO_STDIO: decode from memory.
        unsigned char* px = file.empty() ? nullptr
                                         : stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &w, &h, &comp, 3);
        bool lit = false;
        if (px) {
            for (size_t i = 0, n = size_t(w) * size_t(h) * 3; i < n && !lit; ++i) lit = px[i] != 0;
            stbi_image_free(px);
        }
        if (!lit) { ++black; std::printf("    all-black (or unreadable) albedo: %s\n", p.c_str()); }
    }
    std::printf("    albedos %zu, all black %zu\n", albedos, black);
    CHECK(albedos > 0);
    CHECK_EQ(black, size_t(0));

    // 32 rays straight down: the exported cell meshes (Terrain and Collider_k,
    // de-localised by the cell origin) against the scene's own CollisionIndex.
    std::vector<float> coll, terr;
    for (const json& c : man["cells"]) {
        const Glb g = read_glb(under(dir, c["glb"]));
        const float origin[3] = {c["origin"][0].get<float>(), c["origin"][1].get<float>(), c["origin"][2].get<float>()};
        glb_triangles(g, [](const std::string& n) { return n.rfind("Collider_", 0) == 0; }, origin, coll);
        glb_triangles(g, [](const std::string& n) { return n == "Terrain"; }, origin, terr);
    }
    const cw::CollisionIndex fromCells = cw::CollisionIndex::from_triangles(coll, terr);
    const cw::CollisionIndex fromScene = cw::CollisionIndex::build(scene, true);
    std::printf("    triangles: cells %zu collision + %zu terrain, scene %zu collision + %zu terrain\n",
                fromCells.triangle_count(cw::kHitCollision), fromCells.triangle_count(cw::kHitTerrain),
                fromScene.triangle_count(cw::kHitCollision), fromScene.triangle_count(cw::kHitTerrain));
    CHECK(scene.hasBounds);
    Lcg rng{20261015u};
    const float dirDown[3] = {0, 0, 1};
    int hits = 0, agree = 0;
    for (int r = 0; r < 32; ++r) {
        const float o[3] = {scene.bounds[0] + rng.next() * (scene.bounds[2] - scene.bounds[0]),
                            scene.bounds[1] + rng.next() * (scene.bounds[3] - scene.bounds[1]), -20000.0f};
        const auto a = fromCells.raycast(o, dirDown, 1e7f);
        const auto b = fromScene.raycast(o, dirDown, 1e7f);
        const bool same = a.has_value() == b.has_value() && (!a || std::fabs(a->t - b->t) <= 0.01f);
        if (b) ++hits;
        if (same) ++agree;
        else std::printf("    ray %d at (%.1f, %.1f): cells %s %.3f, scene %s %.3f\n", r, o[0], o[1], a ? "hit" : "miss",
                         a ? a->t : 0.0f, b ? "hit" : "miss", b ? b->t : 0.0f);
    }
    std::printf("    rays: %d of 32 hit the scene, %d agree\n", hits, agree);
    CHECK(hits > 0);
    CHECK_EQ(agree, 32);

    // The validator, when Python is on PATH.
    if (std::system("python --version >NUL 2>&1") == 0) {
        const std::string cmd = std::string("python -I \"") + CM_SOURCE_DIR + "/tools/world/validate_manifest.py\" \"" +
                                dir.string() + "\" --plan";
        const int rc = std::system(cmd.c_str());
        std::printf("    validate_manifest.py: %d\n", rc);
        CHECK_EQ(rc, 0);
    } else {
        std::printf("    python not on PATH: validate_manifest.py not run\n");
    }
    std::error_code ec;
    eg::remove_tree(dir, ec);
}

namespace {

/// min, median, p90 (nearest rank) and max of @p v (sorted in place).
template <typename T>
void print_distribution(const char* label, std::vector<T> v) {
    if (v.empty()) { std::printf("      %s: none\n", label); return; }
    std::sort(v.begin(), v.end());
    const T median = v[(v.size() - 1) / 2];
    const T p90 = v[std::max<size_t>(1, size_t(std::ceil(0.9 * double(v.size())))) - 1];
    std::printf("      %s: min %.6g, median %.6g, p90 %.6g, max %.6g (n %zu)\n", label, double(v.front()),
                double(median), double(p90), double(v.back()), v.size());
}

} // namespace

// Spec "Performance and size" and the plan's budget rule: per map, the
// distribution of the 4x4 blocks' triangles and texture MB (the partition with
// no budget), the blocks the default budget splits, the folder bytes by kind
// and the stage seconds. Prints only.
CM_TEST(world_export_dat, measure) {
    const char* on = std::getenv("CM_WORLD_EXPORT_MEASURE");
    if (!on || std::string(on) != "1") SKIP("set CM_WORLD_EXPORT_MEASURE=1 to export and measure three maps");
    Gw2Dat& dat = shared_dat();
    if (!ensure_template()) SKIP("no struct template");
    const struct { uint32_t id; const char* name; } kMaps[] = {
        {192711, "Queensdale"}, {191000, "Lion's Arch"}, {1151420, "Spirit Vale"}};
    for (const auto& map : kMaps) {
        const fs::path dir = fresh_dir("measure_" + std::to_string(map.id));
        const MapExport x = export_map(dat, map.id, dir);
        std::printf("    == %s (%u): ok %d %s\n", map.name, map.id, int(x.rep.ok), x.rep.error.c_str());
        if (!x.rep.ok || !x.manifest.is_object()) continue;
        const json& man = x.manifest;
        const eg::WorldFolderOptions o;
        std::printf("      counts %s\n", man["counts"].dump().c_str());

        // The CellStats rebuilt from the folder must give back the export's cells.
        const cw::CellStats st = stats_from_folder(x.scene, man, dir, o);
        const cw::CellPlan real = cw::partition_cells(x.scene, st, o.budget, o.region);
        size_t mismatched = 0;
        std::map<std::string, const json*> manCell;
        for (const json& c : man["cells"]) manCell[c["id"].get<std::string>()] = &c;
        for (const cw::Cell& c : real.cells) {
            const auto it = manCell.find(c.id);
            if (it == manCell.end() || (*it->second)["budget"]["triangles"].get<uint64_t>() != c.triangles ||
                std::fabs((*it->second)["budget"]["textureMB"].get<double>() - c.textureMB) > 1e-6)
                ++mismatched;
        }
        if (real.cells.size() != manCell.size()) mismatched += 1;
        std::printf("      rebuilt partition: %zu cells, %zu differ from the manifest\n", real.cells.size(), mismatched);

        // Blocks: the same partition with no budget (each page block whole).
        cw::CellBudget none = o.budget;
        none.maxTriangles = std::numeric_limits<uint64_t>::max();
        none.maxTextureMB = std::numeric_limits<double>::infinity();
        const cw::CellPlan blocks = cw::partition_cells(x.scene, st, none, o.region);
        std::vector<uint64_t> tris;
        std::vector<double> mb;
        size_t splitBlocks = 0, overBudget = 0;
        for (const cw::Cell& b : blocks.cells) {
            tris.push_back(b.triangles);
            mb.push_back(b.textureMB);
            bool split = false;
            for (const cw::Cell& c : real.cells) {
                if (c.cx0 >= b.cx0 && c.cx0 < b.cx0 + b.w && c.cy0 >= b.cy0 && c.cy0 < b.cy0 + b.h && c.split) split = true;
            }
            if (split) ++splitBlocks;
        }
        for (const cw::Cell& c : real.cells) if (c.overBudget) ++overBudget;
        std::printf("      blocks (%dx%d pages): %zu; split by the default budget (%llu triangles, %.0f MB): %zu "
                    "(%.1f %%); cells %zu, over budget at 1x1 %zu\n",
                    blocks.blockChunks, blocks.blockChunks, blocks.cells.size(),
                    static_cast<unsigned long long>(o.budget.maxTriangles), o.budget.maxTextureMB, splitBlocks,
                    blocks.cells.empty() ? 0.0 : 100.0 * double(splitBlocks) / double(blocks.cells.size()),
                    real.cells.size(), overBudget);
        print_distribution("block triangles", tris);
        print_distribution("block texture MB", mb);
        {
            std::vector<uint64_t> t2(tris);
            std::sort(t2.begin(), t2.end());
            if (!t2.empty()) {
                const uint64_t p90 = t2[std::max<size_t>(1, size_t(std::ceil(0.9 * double(t2.size())))) - 1];
                std::printf("      p90 block triangles rounded up to 250 000: %llu\n",
                            static_cast<unsigned long long>((p90 + 249999) / 250000 * 250000));
            }
        }

        // Folder bytes: the report's kinds and the folder's top-level directories.
        std::string kinds;
        for (const auto& [k, b] : x.rep.bytes) kinds += " " + k + " " + std::to_string(b);
        std::printf("      report bytes:%s\n", kinds.c_str());
        std::map<std::string, uint64_t> top;
        std::map<std::string, size_t> topFiles;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const fs::path rel = fs::relative(it->path(), dir, ec);
            const std::string head = rel.has_parent_path() ? rel.begin()->string() : std::string("(root)");
            const std::string ext = it->path().extension().string();
            top[head + " " + ext] += it->file_size(ec);
            ++topFiles[head + " " + ext];
        }
        uint64_t all = 0;
        for (const auto& [k, b] : top) {
            all += b;
            std::printf("      folder %-24s %6zu files %12llu bytes\n", k.c_str(), topFiles[k], static_cast<unsigned long long>(b));
        }
        std::printf("      folder total %llu bytes\n", static_cast<unsigned long long>(all));

        // Stage seconds.
        std::string secs;
        for (const auto& [k, v] : x.rep.seconds) { char b[64]; std::snprintf(b, sizeof b, " %s %.2f", k.c_str(), v); secs += b; }
        std::printf("      seconds: load_world %.2f, sky inputs %.2f, export %.2f;%s\n", x.tLoad, x.tSky, x.tExport, secs.c_str());

        // Models without instances (failed, or every placement skipped), and models
        // whose glb has a primitive with no material (Unity: Default-Material).
        std::vector<char> placed(man["models"].size(), 0);
        for (const json& c : man["cells"])
            for (const json& in : c["instances"]) placed[in["model"].get<size_t>()] = 1;
        size_t unplacedOk = 0, unplacedFailed = 0, noMaterial = 0;
        std::string noMatIds;
        for (size_t k = 0; k < man["models"].size(); ++k) {
            const json& m = man["models"][k];
            const bool ok = m["status"] == "ok";
            if (!placed[k]) (ok ? unplacedOk : unplacedFailed)++;
            if (ok && glb_has_unmaterialled_primitive(read_glb(under(dir, m["glb"])))) {
                ++noMaterial;
                noMatIds += " " + std::to_string(m["fileId"].get<uint32_t>());
            }
        }
        std::printf("      models %zu: without instances %zu (ok %zu, failed %zu); with an unmaterialled primitive %zu:%s\n",
                    man["models"].size(), unplacedOk + unplacedFailed, unplacedOk, unplacedFailed, noMaterial,
                    noMatIds.c_str());

        // The sun as the env chunk stores it (the light's travel direction, Task 13).
        const auto& L = x.scene.environment.light;
        std::printf("      sun: present %d preset %d stored (%.3f, %.3f, %.3f); unityForward %s\n", int(L.present),
                    L.presetIndex, L.sunDir[0], L.sunDir[1], L.sunDir[2],
                    man["light"].is_object() ? man["light"]["sun"]["unityForward"].dump().c_str() : "null");
        eg::remove_tree(dir, ec);
    }
}
