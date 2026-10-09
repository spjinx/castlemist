/// @file
/// @brief Water and environment for WorldScene (docs/research/gw2-world-frame.md §6).
/// @ingroup world

#include "castlemist/world/load_world.h"

#include <exception>
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
    if (in.watr.chunk && !in.watr.hasSurfacesField)
        warn.push_back("watr V0 not read (version " + std::to_string(in.watr.version) +
                       ": no waterSurfaces field; its waterChunks/waterFoamData are not decoded)");
    if (in.watr.hasPlane) {
        w.hasPlane = true;
        w.planeZ = in.watr.planeZ;
        w.planeFlags = in.watr.flags;
        warn.push_back("watr: waterPlaneZ has no outline: z " + num(in.watr.planeZ) +
                       " is the map's stored water level (terrain below it traces the map's lakes, rivers and sea, "
                       "note §6.2), but the area the game draws it over is UNPROVEN");
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

    // shor (§6.4).
    w.shore = in.shore;

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

void attach_environment(castlemist::model::Extractor& ex, WorldScene& out) {
    Environment env;
    try {
        env.sky = ex.parseMapSky();
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("env: sky not read: ") + e.what());
    }
    try {
        env.light = ex.parseMapEnv();   // auto preset: the brightest (the day rig)
    } catch (const std::exception& e) {
        out.warnings.push_back(std::string("env: light not read: ") + e.what());
    }
    if (!env.sky.present) out.warnings.push_back("env: no sky (no env chunk or no dataGlobal)");
    if (!env.light.present) out.warnings.push_back("env: no light rig (no readable lighting preset)");
    out.environment = std::move(env);
}

} // namespace castlemist::world
