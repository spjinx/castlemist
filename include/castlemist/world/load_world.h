/// @file
/// @brief Loading a map into a WorldScene: load_world assembles terrain,
///        props, collision, water and environment; the water and environment
///        steps are docs/research/gw2-world-frame.md §6.
/// @ingroup world
#pragma once

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/dat_read.h"
#include "castlemist/world/world_scene.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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
/// `out.warnings`: a V0 `watr` ("watr: V0 not read"), a chunk or struct the
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

/// @brief Run one load step; an exception becomes the warning
///        "<name>: exception: <what>; section left empty" and the caller
///        carries on with the next step -- except DatIoError (dat_read.h),
///        which is rethrown: dat I/O failure is an error, not a warning.
void run_section(std::vector<std::string>& warnings, const char* name, const std::function<void()>& run);

/// @brief One "chunk <fourcc> v<ver> not read" warning per distinct chunk in
///        @p chunks (fourcc, version) that no section reads. The chunks that
///        are read: parm, trn, prp2, havk, watr, shor, rive, env.
std::vector<std::string> unread_chunk_warnings(const std::vector<std::pair<std::string, uint16_t>>& chunks);

/// @brief The warnings for a map that lacks a section's chunk: "props: no prp2
///        chunk; map has no props" and "collision: no havk chunk; map has no
///        collision". Such a map is valid; it is warned, never filled in.
///        (A map without terrain is warned by build_terrain.)
std::vector<std::string> absent_chunk_warnings(const std::vector<std::pair<std::string, uint16_t>>& chunks);

/// @brief Load a map packfile into a WorldScene.
///
/// Runs terrain, terrain materials, props, collision, water and environment,
/// each in its own `try`: one failing leaves its part empty and adds a named
/// warning. `bounds` is `parm.rect` (§2). Every chunk in the packfile that no
/// section reads is listed as `"chunk <fourcc> v<ver> not read"`, a missing
/// `prp2` or `havk` chunk is warned (absent_chunk_warnings), and a
/// `"units: ..."` warning records that map units per metre are UNPROVEN
/// (§1.3).
/// @throws DatIoError when the dat cannot be read, for the map file
///         ("file <id>: cannot read: ...") or any file a section reads (the
///         terrain's PIMG).
/// @throws std::runtime_error when @p tpl has no `types` ("struct template
///         missing 'types'", checked first), when the map file does not
///         decompress ("file <id>: cannot decompress: ..."), when @p mapFileId
///         is not in the dat, or when the file is not a map: no `trn`, `parm`,
///         `prp2` or `havk` chunk (`"file <id> is not a map packfile"`). Bad
///         map data never throws; a secondary file that does not decompress
///         or parse is a section warning.
WorldScene load_world(Gw2Dat& dat, uint32_t mapFileId, const nlohmann::json& tpl);

/// @brief A compact JSON description of a scene (no geometry):
///        {map, bounds ([x0, y0, x1, y1] or null), terrain:{present,
///        chunks:[x, y], resolvedMaterials}, models, props, animatedProps,
///        collision:{meshes, instances}, water:{plane, planeZ (null without
///        a plane), surfaces, rivers}, sky:{present, modes, clouds,
///        lightPresent}, warnings}. Counts only: no per-chunk materials and
///        no frame (map space as stored, frame.h).
nlohmann::json world_summary(const WorldScene& w);

} // namespace castlemist::world
