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
    TextureMap textures;                          ///< every fileId the sky names that decoded
    std::vector<std::string> decodeWarnings;      ///< "fileId N: not a decodable texture"
    /// parseMapEnv() with the auto (brightest) preset. Only the "day" mode
    /// gets it: no proven link ties the 3 lighting presets to the sky modes.
    castlemist::model::Extractor::MapEnvLight daySun;
    uint32_t mapFileId = 0;
};

/// Parse the sky of map packfile @p mapBytes and decode every texture it names.
/// Only safe on the thread that owns @p dat.
SkyInputs load_sky_inputs(Gw2Dat& dat, const std::vector<uint8_t>& mapBytes,
                          const nlohmann::json& tpl, uint32_t mapFileId);

struct SkyExportOptions {
    int faceSize = 1024;        ///< baked cube faces, pixels per side
    int equirectWidth = 4096;   ///< baked equirect; height = width / 2
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

/// Folder name of sky mode @p index: "day", "night", "mode2", ...
std::string mode_name(size_t index);

/// The report as JSON, for the CLI.
nlohmann::json report_json(const SkyExportReport& r);

} // namespace castlemist::exportgltf::sky
