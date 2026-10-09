/// @file
/// @brief Loading a map into a WorldScene. For now: the water and environment
///        steps (docs/research/gw2-world-frame.md §6).
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

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

} // namespace castlemist::world
