/// @file
/// @brief export_world_folder: one WorldScene -> the world export folder
///        (docs/superpowers/specs/2026-10-15-world-export-design.md,
///        "Architecture", "Output folder", "Error handling").
///
/// Order of work (spec "Order of work" 1-8; Blender --to-fbx is the caller's step 9):
///  1. the cells the region keeps (a first partition without stats), so the
///     needed models are known;
///  2. the model stage: each needed model loaded through ModelSource, its
///     textures filled from ModelSource::texture at the cap, written as a
///     VRChat folder models/<fileId>/ (root map_to_gltf_root(1)), measured
///     (triangles, textures, clips, dark decals, uv scroll) and released -- on
///     a bounded pool. Dat textures go once into the shared pool textures/
///     (<fileId>.png, referenced as ../../textures/<fileId>.png; ruling W8);
///     generated images stay in models/<fileId>/Images/. The source's texture
///     cache is then released (ruling W9);
///  3. partition_cells with those measurements;
///  4. the kept cells' terrain textures, on the pool;
///  5. each cell baked (albedo, water) and written (cells/<id>.glb), on the pool;
///  6. write_skybox into sky/;
///  7. probe.glb and the spawn (a ray down through CollisionIndex);
///  8. manifest.json, written last (manifest.json.tmp, then renamed).
///
/// Errors: a model that fails to load or write is counted, marked failed and
/// its placements skipped; it never fails the export. A world::DatIoError from
/// the ModelSource, a folder that may not be written to, and a failed cell,
/// probe or manifest write stop it, and leave no manifest.json.
/// @ingroup exportgltf
#pragma once

#include "castlemist/exportgltf/sky_export.h"
#include "castlemist/exportgltf/world_manifest.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

/// @brief Where the export's models and textures come from.
class ModelSource {
public:
    virtual ~ModelSource() = default;
    /// Model `scene.models[index]`, textures header-only or decoded; null = failed. Thread-safe.
    virtual std::shared_ptr<ModelPreview> load(uint32_t index, uint32_t fileId) = 0;
    /// decode_texture_capped result; null or empty mips = failed. Thread-safe.
    virtual std::shared_ptr<const CappedTexture> texture(uint32_t fileId, int maxSize) = 0;
    /// Drops every cached texture (ruling W9: called after the model stage and
    /// after the terrain textures are fetched, so peak memory does not grow
    /// with the map). Callers keep the shared_ptrs they hold. Not concurrent
    /// with texture().
    virtual void release_textures() {}
};

/// @brief What one export did. `dir` and `manifest` are UTF-8 paths.
struct WorldFolderReport {
    bool ok = false; std::string error, dir, manifest;
    size_t cells = 0, modelsWritten = 0, modelsFailed = 0, propsWritten = 0, propsSkipped = 0,
           chunksUnbaked = 0, animated = 0, animatedResolved = 0, clipsFailedDecode = 0;
    std::map<std::string, double> seconds;    ///< world-cells, models, terrain-textures, cells, sky, manifest
    std::map<std::string, uint64_t> bytes;    ///< prop-textures, terrain, geometry, sky
    std::vector<std::string> warnings;        ///< the manifest's warnings
};

/// @brief Writes the world export folder @p dirUtf8 (see the file comment).
///
/// - Output folder: absent or empty, the export goes ahead; holding
///   manifest.json with opts.overwrite set, its contents are removed first;
///   anything else is `error = "output folder not empty: <dir>"`.
/// - @p sky may be null (no sky); so may opts.sky be false. write_skybox's
///   "no sky" (or any failure of its own) gives `sky: null` and a warning.
/// - opts.threads workers (0 = hardware_concurrency) share @p src; results are
///   kept by model / cell index, so the output does not depend on the count.
WorldFolderReport export_world_folder(const castlemist::world::WorldScene& scene, ModelSource& src,
                                      const sky::SkyInputs* sky, const std::string& dirUtf8,
                                      const WorldFolderOptions& opts);

/// @brief The report as JSON (for the CLI and export.log).
nlohmann::json report_json(const WorldFolderReport& r);

} // namespace castlemist::exportgltf
