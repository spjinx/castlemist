/// @file
/// @brief Public API: a world engine WorldScene -> one binary glTF (.glb):
///        terrain, placed props and (optionally) collision.
/// @ingroup exportgltf

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "castlemist/extract/model_types.h"
#include "castlemist/world/world_scene.h"

namespace castlemist::exportgltf {

/// @brief Tunables for export_world_gltf.
struct WorldExportOptions {
    bool terrain = true;      ///< One mesh per terrain chunk.
    bool props = true;        ///< Every prop whose model was loaded.
    bool collision = false;   ///< Every havk collision instance (positions only).
    bool textures = true;     ///< Embed prop textures; false writes materials without images.
    bool animations = false;  ///< Bake skinned props' clips (written per instance, so it multiplies size).
    int animFps = 30;
    /// @brief Uniform scale on the root node; 1 writes the map's own units.
    ///        Map units per metre are UNPROVEN (docs/research/gw2-world-frame.md §1.3).
    double rootScale = 1.0;
    /// @brief Called as the export advances, on the calling thread: @p stage
    ///        is "terrain", "props", "collision" or "save"; @p done of @p total
    ///        items so far (for "save", total is the .glb's size in bytes and
    ///        it is called once, before writing). Empty = no reports.
    std::function<void(const char* stage, size_t done, size_t total)> progress;
};

/// @brief What export_world_gltf wrote.
struct WorldExportReport {
    bool ok = false;
    std::string error;
    std::string glbPath;
    size_t terrainChunks = 0;       ///< Chunk meshes written.
    size_t propsWritten = 0;        ///< Prop instances written.
    size_t propsSkipped = 0;        ///< Props whose model was not loaded or has no mesh.
    size_t modelsWritten = 0;       ///< Distinct prop models embedded.
    size_t collisionInstances = 0;  ///< Collision instances written.
    std::vector<std::string> warnings;
};

/// @brief The column-major matrix the export's root node carries: map space
///        (left-handed, +X east, +Y north, +Z down; world/frame.h) -> glTF
///        (right-handed, +Y up, +Z south): (x, y, z) -> (x, -z, -y), times
///        @p scale. Its determinant is negative -- the handedness change --
///        which glTF answers by reversing the front-face winding below it, so
///        every mesh and placement stays in map-space numbers untouched.
std::array<float, 16> map_to_gltf_root(double scale);

/// @brief Export a loaded map to one .glb.
///
/// The scene is one root node (map_to_gltf_root) with up to three children:
/// "Terrain" (a node per chunk, its vertices in map space, TEXCOORD_0 the
/// map-wide 0..1 position across the terrain's extent, so one top-down image
/// covers it), "Props" (a node per placement carrying PropInstance::world,
/// sharing one glTF mesh set per model) and "Collision" (a node per
/// CollisionInstance carrying its world matrix). Terrain is untextured: the
/// chunk materials (world/terrain.h) blend four layers through paged blend
/// maps, which glTF's material model cannot express.
///
/// @param scene   load_world's output.
/// @param models  Parallel to `scene.models`: each prop model as loaded, or
///                null where it failed to load (its placements are skipped and
///                counted). May be shorter than `scene.models`; the rest count
///                as not loaded.
/// @param glbPath Destination, UTF-8. Parent directories are created.
WorldExportReport export_world_gltf(const castlemist::world::WorldScene& scene,
                                    const std::vector<std::shared_ptr<ModelPreview>>& models,
                                    const std::string& glbPath, const WorldExportOptions& opts = {});

} // namespace castlemist::exportgltf
