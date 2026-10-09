/// @file
/// @brief Water and environment for WorldScene (docs/research/gw2-world-frame.md §6).
/// @ingroup world

#include "castlemist/world/load_world.h"

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/world/collision.h"
#include "castlemist/world/props.h"
#include "castlemist/world/terrain.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace castlemist::world {

namespace {

/// @brief `v` with no trailing zeros ("0", "-12.5").
std::string num(float v) {
    std::string s = std::to_string(v);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

} // namespace

void build_water(const WaterSources& in, WorldScene& out) {
    Water w;
    std::vector<std::string>& warn = out.warnings;

    // watr (§6.1). V0 stores waterFoamData/waterChunks, which nothing reads.
    if (!in.watr.problem.empty()) warn.push_back("watr: " + in.watr.problem);
    if (in.watr.chunk && in.watr.problem.empty() && !in.watr.hasSurfacesField)
        warn.push_back("watr V0 not read (version " + std::to_string(in.watr.version) +
                       ": no waterSurfaces field; its waterChunks/waterFoamData are not decoded)");
    if (in.watr.hasPlane) {
        w.hasPlane = true;
        w.planeZ = in.watr.planeZ;
        w.planeFlags = in.watr.flags;
        warn.push_back("watr: waterPlaneZ has no outline: stored z " + num(in.watr.planeZ) +
                       ". The visible water is at about z 0 on the test maps (terrain below 0 traces their lakes, "
                       "rivers and sea, note §6.2), but every test map stores 0, so that the game draws its water at "
                       "waterPlaneZ is UNPROVEN (proof: a map with a non-zero value that agrees with its terrain); "
                       "the area it covers is UNPROVEN too");
        warn.push_back("watr: waterFlags " + std::to_string(in.watr.flags) + " kept as stored; meaning UNPROVEN");
    }
    w.surfaces = in.watr.surfaces;
    if (!w.surfaces.empty())
        warn.push_back("watr: " + std::to_string(w.surfaces.size()) +
                       " waterSurfaces kept as stored; waterSurfaceFlags meaning UNPROVEN (no test map has a surface)");

    // havk (§6.1).
    if (in.havk.hasSurfaceZ) {
        w.hasHavkSurfaceZ = true;
        w.havkSurfaceZ = in.havk.surfaceZ;
    }
    if (w.hasPlane && w.hasHavkSurfaceZ && w.planeZ != w.havkSurfaceZ)
        warn.push_back("water: watr waterPlaneZ " + num(w.planeZ) + " differs from havk waterSurfaceZ " +
                       num(w.havkSurfaceZ) + "; both kept, which one the game draws is UNPROVEN");
    if (in.havk.waterVolumes)
        warn.push_back("havk: " + std::to_string(in.havk.waterVolumes) +
                       " waterVolumes not read (named polygons with a vertical range; no test map has one)");

    // rive (§6.3).
    for (const auto& r : in.rivers.rivers) w.rivers.push_back(River{r.name, r.points});
    if (!w.rivers.empty())
        warn.push_back("rive: " + std::to_string(w.rivers.size()) +
                       " rivers read as centrelines only; width, tessellation and materials (a properties bag "
                       "keyed by unnamed hashes in rive v" + std::to_string(in.rivers.version) +
                       ") are not read, so no river surface can be built; whether a river is drawn as water is "
                       "UNPROVEN (note §6.3)");
    if (in.rivers.droppedPoints)
        warn.push_back("rive: " + std::to_string(in.rivers.droppedPoints) +
                       " rivers' points not read (element size is not 12 bytes / float3)");

    // shor (§6.4).
    w.shore = in.shore;
    if (w.shore.present)
        warn.push_back("shor: " + std::to_string(w.shore.chains.size()) +
                       " shore chains read with field names after T3D's SHOR.ts; untested on real data "
                       "(no test map has a shor chunk)");

    // env (§6.5).
    if (in.envWaterPresets)
        warn.push_back("env: " + std::to_string(in.envWaterPresets) +
                       " water presets (material, colours, waves per env zone) not read");

    out.water = std::move(w);
}

void attach_water(castlemist::model::Extractor& ex, WorldScene& out) {
    WaterSources in;
    // Each reader is independent: a failure in one is named and the rest still load.
    auto guarded = [&](const char* what, auto&& read) {
        try {
            read();
        } catch (const std::exception& e) {
            out.warnings.push_back(std::string(what) + " not read: " + e.what());
        }
    };
    guarded("watr", [&] { in.watr = ex.parseWater(); });
    guarded("havk water", [&] { in.havk = ex.parseHavokWater(); });
    guarded("rive", [&] { in.rivers = ex.parseRivers(); });
    guarded("shor", [&] { in.shore = ex.parseShore(); });
    guarded("env water presets", [&] { in.envWaterPresets = ex.countEnvWaterPresets(); });
    build_water(in, out);
}

void build_environment(const EnvSources& in, WorldScene& out) {
    Environment env;
    env.sky = in.sky;
    env.light = in.light;
    if (!env.sky.present) out.warnings.push_back("env: no sky (no env chunk or no dataGlobal)");
    if (!env.light.present) out.warnings.push_back("env: no light rig (no readable lighting preset)");
    const uint32_t otherPresets = in.counts.lightingPresets > 1 ? in.counts.lightingPresets - 1 : 0;
    const uint32_t zones = in.counts.localBlocks + in.counts.overrideBlocks;
    if (otherPresets || zones)
        out.warnings.push_back("env: " + std::to_string(otherPresets) +
                               " other dataGlobal lighting presets and " + std::to_string(zones) +
                               " per-zone blocks (" + std::to_string(in.counts.localBlocks) + " dataLocalArray, " +
                               std::to_string(in.counts.overrideBlocks) +
                               " dataOverrideArray: their own sky and lighting) not attached; only the dataGlobal "
                               "sky and the brightest lighting preset are");
    out.environment = std::move(env);
}

void attach_environment(castlemist::model::Extractor& ex, WorldScene& out) {
    EnvSources in;
    try {
        in.sky = ex.parseMapSky();
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("env: sky not read: ") + e.what());
    }
    try {
        in.light = ex.parseMapEnv();   // auto preset: the brightest (the day rig)
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("env: light not read: ") + e.what());
    }
    try {
        in.counts = ex.countEnv();
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("env: preset and zone counts not read: ") + e.what());
    }
    build_environment(in, out);
}

WorldScene load_world(Gw2Dat& dat, uint32_t mapFileId, const nlohmann::json& tpl) {
    using Ex = castlemist::model::Extractor;
    const std::string id = std::to_string(mapFileId);

    const uint32_t base = get_by_base_id(dat, mapFileId);
    if (base == 0 || base > dat.mft_data_list.size()) throw std::runtime_error("file " + id + " is not in the dat");
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);   // I/O failure propagates

    std::vector<uint8_t> bytes;
    std::vector<std::pair<std::string, uint16_t>> chunks;
    std::unique_ptr<Ex> ex;
    try {
        bytes = e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
        ex = std::make_unique<Ex>(bytes, tpl);
        chunks = ex->chunkList();
    } catch (const std::exception& what) {
        if (std::string(what.what()).find("template") != std::string::npos) throw;   // a bad template is an error
        throw std::runtime_error("file " + id + " is not a map packfile");
    }

    // A file with none of the chunks a map is made of is not a map.
    auto has = [&](const char* fourcc) {
        return std::any_of(chunks.begin(), chunks.end(), [&](const auto& c) { return c.first == fourcc; });
    };
    if (!has("trn") && !has("parm") && !has("prp2") && !has("havk"))
        throw std::runtime_error("file " + id + " is not a map packfile");

    WorldScene w;
    w.mapFileId = mapFileId;
    // One failing section leaves its part empty and is named; the rest still load.
    auto section = [&](const char* name, auto&& run) {
        try {
            run();
        } catch (const std::exception& what) {
            w.warnings.push_back(std::string("exception in ") + name + ": " + what.what() + "; section left empty");
        }
    };

    section("terrain", [&] {
        const Ex::MapTerrain trn = ex->parseTerrain();
        w.terrain = build_terrain(trn, w.warnings);
        if (trn.hasRect) {   // bounds = parm.rect as stored (§2)
            std::copy(trn.rect, trn.rect + 4, w.bounds);
            w.hasBounds = true;
        } else {
            w.warnings.push_back("parm: no rect; the map has no bounds (no rect is invented)");
        }
    });
    section("terrain materials", [&] {
        if (!w.terrain.present) return;
        resolve_terrain_materials(w.terrain, ex->parseTerrainMaterials(), dat, tpl, w.warnings);
    });
    section("props", [&] { build_props(ex->parseMapProps(), w); });
    section("collision", [&] { build_collision(ex->parseHavok(), w); });
    section("water", [&] { attach_water(*ex, w); });
    section("environment", [&] { attach_environment(*ex, w); });

    // The chunks the sections above read, by the findChunk calls of the readers
    // they use: parm and trn (parseTerrain, parseTerrainMaterials), prp2
    // (parseMapProps), havk (parseHavok, parseHavokWater), watr, shor, rive
    // (parseWater, parseShore, parseRivers) and env (parseMapSky, parseMapEnv,
    // countEnv, countEnvWaterPresets). Every other chunk is named, not dropped.
    static const std::set<std::string> kRead = {"parm", "trn", "prp2", "havk", "watr", "shor", "rive", "env"};
    for (const auto& [fourcc, ver] : chunks)
        if (!kRead.count(fourcc))
            w.warnings.push_back("chunk " + fourcc + " v" + std::to_string(ver) + " not read");

    w.warnings.push_back(
        "units: map units per metre are UNPROVEN; coordinates are kept as stored (nothing in the template "
        "names a unit; proof: the code that writes fAvatarPosition into the MumbleLink view with its factor, "
        "or a measured in-game distance; note §1.3)");
    return w;
}

nlohmann::json world_summary(const WorldScene& w) {
    using nlohmann::json;
    json j;
    j["map"] = w.mapFileId;
    j["bounds"] = w.hasBounds ? json::array({w.bounds[0], w.bounds[1], w.bounds[2], w.bounds[3]}) : json(nullptr);
    size_t resolved = 0;
    for (const auto& c : w.terrain.chunks) resolved += c.material.resolved;
    j["terrain"] = {{"present", w.terrain.present},
                    {"chunks", json::array({w.terrain.chunksX, w.terrain.chunksY})},
                    {"resolvedMaterials", resolved}};
    j["models"] = w.models.size();
    j["props"] = w.props.size();
    j["animatedProps"] = w.motion.animatedProps.size();
    j["collision"] = {{"meshes", w.collision.meshes.size()}, {"instances", w.collision.instances.size()}};
    j["water"] = {{"surfaces", w.water.surfaces.size()}};
    j["sky"] = {{"present", w.environment.sky.present}, {"modes", w.environment.sky.modes.size()},
                {"clouds", w.environment.sky.clouds.size()}, {"lightPresent", w.environment.light.present}};
    j["warnings"] = w.warnings;
    return j;
}

} // namespace castlemist::world
