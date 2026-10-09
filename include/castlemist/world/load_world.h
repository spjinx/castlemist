/// @file
/// @brief Loading a map into a WorldScene: load_world assembles terrain,
///        props, collision, water and environment; the water and environment
///        steps are docs/research/gw2-world-frame.md §6.
/// @ingroup world
#pragma once

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

#include <nlohmann/json.hpp>

#include <cstdint>

namespace castlemist::world {

/// @brief Everything attach_water reads from a map, as the Extractor parsed it.
struct WaterSources {
    castlemist::model::Extractor::MapWater watr;
    castlemist::model::Extractor::MapHavokWater havk;
    castlemist::model::Extractor::MapRivers rivers;
    castlemist::model::Extractor::MapShore shore;
    uint32_t envWaterPresets = 0;   ///< PackMapEnvDataWater entries in `env` (Extractor::countEnvWaterPresets)
};

/// @brief Fill `out.water` from parsed sources; the rules of attach_water,
///        without the dat. Replaces anything previously in `out.water`.
void build_water(const WaterSources& in, WorldScene& out);

/// @brief Fill `out.water` from the map's `watr`, `havk`, `rive` and `shor`
///        chunks, as stored (map space, up = -Z).
///
/// Reads the `watr` V1 water plane height and flags and its per-surface
/// outlines, the `havk` water surface height, river centrelines and shore
/// chains. Nothing is invented: no flood fill, no plane where the dat has
/// none. What the dat holds and this does not read is named in
/// `out.warnings`: a V0 `watr` ("watr V0 not read"), a chunk or struct the
/// template cannot read, river widths and materials, `havk` water volumes,
/// `env` water presets, the untested shore reader, the meaning of
/// `waterFlags`, and that the water is drawn at `waterPlaneZ` over some
/// area -- both UNPROVEN (§6.2). Never throws for bad data.
/// Replaces anything previously in `out.water`. See build_water.
void attach_water(castlemist::model::Extractor& ex, WorldScene& out);

/// @brief Everything attach_environment reads from a map, as the Extractor parsed it.
struct EnvSources {
    castlemist::model::Extractor::MapSky sky;
    castlemist::model::Extractor::MapEnvLight light;
    castlemist::model::Extractor::MapEnvCounts counts;
};

/// @brief Fill `out.environment` from parsed sources; the rules of
///        attach_environment, without the dat.
///
/// Keeps the dataGlobal sky and the one auto lighting preset. Warns when
/// either is missing, and names what is not attached: the other dataGlobal
/// lighting presets and the per-zone `dataLocalArray` / `dataOverrideArray`
/// blocks (their own sky and lighting).
void build_environment(const EnvSources& in, WorldScene& out);

/// @brief Fill `out.environment`: the sky (parseMapSky) and the day light rig
///        (parseMapEnv with the auto preset, the brightest), with the
///        warnings of build_environment. Never throws for bad data.
void attach_environment(castlemist::model::Extractor& ex, WorldScene& out);

/// @brief Load a map packfile into a WorldScene.
///
/// Runs terrain, terrain materials, props, collision, water and environment,
/// each in its own `try`: one failing leaves its part empty and adds a named
/// warning. `bounds` is `parm.rect` (§2). Every chunk in the packfile that no
/// section reads is listed as `"chunk <fourcc> v<ver> not read"`, and a
/// `"units: ..."` warning records that map units per metre are UNPROVEN
/// (§1.3).
/// @throws std::runtime_error on dat I/O failure, when @p mapFileId is not in
///         the dat, or when the file is not a map: no `trn`, `parm`, `prp2`
///         or `havk` chunk (`"file <id> is not a map packfile"`). Bad map
///         data never throws.
WorldScene load_world(Gw2Dat& dat, uint32_t mapFileId, const nlohmann::json& tpl);

/// @brief A compact JSON description of a scene (no geometry):
///        {map, bounds, terrain:{chunks:[x,y], resolvedMaterials}, models,
///        props, animatedProps, collision:{meshes, instances},
///        water:{surfaces}, sky, warnings}.
nlohmann::json world_summary(const WorldScene& w);

} // namespace castlemist::world
