/// @file
/// @brief The world folder export against a real Gw2.dat (sub-project 4, Task 8).
///
/// Skips without the archive (GW2_TEST_DAT) or the struct template
/// (GW2_TEST_TEMPLATE, or dumps/packfile/gw2_packfile.json near the build).
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
#include "castlemist/world/dat_read.h"
#include "castlemist/world/load_world.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "nlohmann/json.hpp"

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
    fs::remove_all(d, ec);
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

} // namespace

// The spec's measurement (Task 8): materials.json of model 1766522 with
// wantGame false against true. Measured unequal (no render state without the
// game trace: blend, preset, cull, emission differ), so by the spec's rule the
// dat source defaults to wantGame = true; the test prints the difference and
// holds the default to the traced result.
CM_TEST(world_export_dat, want_game_same_materials) {
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
    fs::remove_all(fs::path(out), ec);
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
