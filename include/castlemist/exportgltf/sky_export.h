/// @file
/// @brief Skybox export: a map's sky as Unity-ready skybox images plus a
///        `sky.json` sidecar.
/// @ingroup exportgltf
///
/// Two stages. load_sky_inputs() reads the dat (parseMapSky, texture decoding,
/// the env light rig) and must run on the thread that owns the Gw2Dat.
/// write_skybox() is pure: it bakes, writes PNGs and `sky.json`, and can run
/// on a worker thread.
///
/// Output, under `<parentDir>/<name>/`:
///
///     sky.json
///     <mode>/baked/equirect.png             Skybox/Panoramic
///     <mode>/baked/{px,nx,py,ny,pz,nz}.png  Skybox/6 Sided, baked
///     <mode>/skybox/{px,...,nz}.png         the map's own cube, as stored
///
/// `<mode>` is "day" (mode 0), "night" (mode 1), "mode2", "mode3"
/// (docs/research/gw2-sky.md §3).

#pragma once

#include "castlemist/exportgltf/sky_bake.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::exportgltf::sky {

/// Everything write_skybox() needs, read from the dat.
struct SkyInputs {
    castlemist::model::Extractor::MapSky sky;
    TextureMap textures;                          ///< every fileId the sky names that decoded, star atlas included
    /// The `starFile` packfile's STAR chunk (gw2-sky.md §8.2); not present
    /// when the map has no star file or it could not be read.
    castlemist::model::Extractor::MapStars stars;
    std::vector<std::string> decodeWarnings;      ///< "fileId N: not a decodable texture"
    /// parseMapEnv() with the auto (brightest) preset. Only the "day" mode
    /// gets it: no proven link ties the 3 lighting presets to the sky modes.
    /// sky.json's sun.direction is its sunDir in Unity axes: the direction the
    /// light travels (a Directional Light's forward), not toward the sun.
    castlemist::model::Extractor::MapEnvLight daySun;
    uint32_t mapFileId = 0;
    /// Camera height (GW2 z, up = -z) the approximate sky's cloud layers are
    /// seen from; when !hasCameraZ, approx_inputs() takes the terrain's
    /// median height (MapSky::groundZ), else 0. The note says where it came from.
    bool hasCameraZ = false;
    float cameraZ = 0;
    std::string cameraNote;
};

/// Which env block the approximate sky takes its haze from: -1 dataGlobal,
/// else dataLocalArray[zone]; `note` says why (gw2-sky.md amendment
/// 2026-10-17). The block that covers most of parm.rect by its shapes'
/// area wins; dataGlobal covers what no zone shape does.
struct EnvBlockChoice {
    int zone = -1;
    std::string note;
};
EnvBlockChoice choose_env_block(const castlemist::model::Extractor::MapSky& s);

/// The labelled inputs of the approximate sky for sky mode @p mode of @p in
/// (gw2-sky.md amendment 2026-10-17): FogColorFar from the chosen env
/// block's haze slot (0 day / 1 night), the sun and SH stand-ins from
/// dataGlobal lighting[slot], the camera height, the fades, Time and AlphaRef.
ApproxInputs approx_inputs(const SkyInputs& in, size_t mode);

/// Parse the sky of map packfile @p mapBytes and decode every texture it names.
/// Only safe on the thread that owns @p dat.
/// @p approx also decodes what only the approximate sky reads (the flow-fire
/// material card's textures).
SkyInputs load_sky_inputs(Gw2Dat& dat, const std::vector<uint8_t>& mapBytes,
                          const nlohmann::json& tpl, uint32_t mapFileId, bool approx = false);

/// The texture half of load_sky_inputs, for a caller that already holds the
/// parsed sky (e.g. WorldScene::environment.sky): decodes every fileId
/// `in.sky` names, and the star file and its atlas, into `in.textures` /
/// `in.stars`, appending to `in.decodeWarnings`. Nothing when !in.sky.present.
/// Read-only lookups on @p dat, as load_sky_inputs.
void load_sky_textures(Gw2Dat& dat, const nlohmann::json& tpl, SkyInputs& in, bool approx = false);

struct SkyExportOptions {
    int faceSize = 1024;        ///< baked cube faces, pixels per side
    int equirectWidth = 4096;   ///< baked equirect; height = width / 2
    /// Bake the approximate sky (approx_inputs) instead of the proven-only
    /// one; sky.json says which ("approximate") and lists every input's label.
    /// Off by default: its haze, sun and cloud inputs are not all proven.
    bool approximate = false;
};

struct SkyExportReport {
    bool ok = false;
    std::string error;          ///< "no sky" when the map has nothing to write
    std::string folder;         ///< <parentDir>/<name>
    int modesWritten = 0;       ///< modes with their own folder (aliases excluded)
    int rawSkyboxes = 0;        ///< modes whose stored cube went out as skybox/
    std::vector<std::string> warnings;
};

/// Write the export folder `<parentDir>/<name>`, replacing it if it exists.
/// Writes nothing (and leaves an existing folder alone) when there is no sky.
SkyExportReport write_skybox(const SkyInputs& in, const std::string& parentDir,
                             const std::string& name, const SkyExportOptions& opt = {});

/// Sky mode @p j bakes exactly like mode @p i: the same textures and the
/// same day/night attribute values (write_skybox writes it as an alias).
/// False when either index is out of range.
bool same_sky_mode(const castlemist::model::Extractor::MapSky& s, size_t i, size_t j);

/// Folder name of sky mode @p index: "day", "night", "mode2", ...
std::string mode_name(size_t index);

/// The report as JSON, for the CLI.
nlohmann::json report_json(const SkyExportReport& r);

} // namespace castlemist::exportgltf::sky
