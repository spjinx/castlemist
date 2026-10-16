/// @file
/// @brief The world export's manifest.json (version 1), the settings it
///        carries and the map-space -> Unity transform of each placement
///        (docs/superpowers/specs/2026-10-15-world-export-design.md
///        "manifest.json (version 1)", D3 "UNPROVEN settings that travel", D5, D7).
///
/// Every Unity value is in the frame the Unity importer sees with
/// ModelImporter.bakeAxisConversion = true (measured by the frame probe, Task 1):
/// world/frame.h's map_to_unity, (x, y, z) -> (x, -z, y), det +1, in map units
/// (unityScale is applied by the importer on the map's root, never here).
///
/// The committed schema is tools/world/castlemist-world.schema.json
/// (`$id` castlemist-world/1); tests/test_exportgltf_world.cpp keeps the writer
/// and the schema in step.
/// @ingroup exportgltf
#pragma once

#include "castlemist/world/cells.h"
#include "castlemist/world/terrain_blend.h"
#include "castlemist/world/world_scene.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

/// @brief The manifest's major version (spec "Versioning"): adding an optional
///        field keeps it; renaming, removing or changing a meaning bumps it.
inline constexpr int kWorldManifestVersion = 1;

/// @brief A placement as a Unity local transform under its Cell node: position
///        t in map units relative to the cell origin (Unity axes; unityScale is
///        not applied here, the importer applies it on the map root, ruling W7),
///        rotation quaternion r (x, y, z, w; w >= 0) and one uniform scale s.
struct UnityTrs { float t[3]; float r[4]; float s; };

/// @brief U = world::map_matrix_to_unity(M with its translation minus @p cellOrigin);
///        t = U's translation, s = |U col0|, r = the rotation U3 / s (Shepperd's
///        method, w >= 0). False -- and nothing a caller may write -- when any input
///        or result is not finite, s <= 0, det(U3) <= 0 (a mirror), the column
///        lengths differ by more than 1e-4 relative (non-uniform scale), the
///        columns are not orthogonal within 1e-4 relative (shear), or the bottom
///        row is not (0, 0, 0, 1): none of those is a uniform scale times a
///        rotation, so a quaternion would silently be wrong.
bool unity_trs(const float mapWorld[16], const float cellOrigin[3], UnityTrs& out);

/// @brief The world folder export's options (spec "Front end"; pinned values in
///        the plan's Global Constraints).
struct WorldFolderOptions {
    int texelsPerChunk = 256, waterTexelsPerChunk = 32, maxTextureSize = 512, threads = 0;   ///< threads 0 = hardware_concurrency
    castlemist::world::CellBudget budget;
    castlemist::world::ChunkRegion region;
    float groundRepeats = castlemist::world::kGroundRepeatsPerChunk;   ///< UNPROVEN, materials note §1.6
    bool srgbColour = true;
    float unityScale = 0.0254f;          ///< UNPROVEN, gltf_export.h:57 "GW2 inches" (spec D7)
    bool sky = true, collision = true, overwrite = false;
    /// When set and it becomes true, export_world_folder stops at the next
    /// model, cell or stage with error "export cancelled" and no manifest.json
    /// (review I5: the GUI sets it when the app is closed during an export).
    const std::atomic<bool>* cancel = nullptr;
};

/// @brief One entry of the manifest's `settings`: status is "UNPROVEN",
///        "castlemist's choice" or "measured"; source names the doc section.
struct ManifestSetting { std::string name; nlohmann::json value; std::string status, source; };

/// @brief The spec D3 table, plus texelsPerChunk, waterTexelsPerChunk,
///        maxTextureSize, the three cell budgets, unityScale, ambient and the
///        measured Unity axis setting. The vector has a fixed order; the
///        manifest's `settings` object is keyed by name, so its JSON is key-sorted.
std::vector<ManifestSetting> world_export_settings(const WorldFolderOptions& o);

/// @brief One model folder as the model stage wrote it. `index` is the
///        scene.models index; empty paths are written as null.
struct ManifestModel { uint32_t index = 0, fileId = 0; bool ok = false; std::string error, folder, glb, materials;
                       uint64_t triangles = 0, textureBytes = 0; std::vector<std::string> clips, darkDecalMaterials;
                       nlohmann::json uvScroll = nlohmann::json::array(); };

/// @brief One written cell. Empty albedo / water paths are written as null.
struct ManifestCell { const castlemist::world::Cell* cell = nullptr; std::string glb, albedo, water; int albedoW = 0, albedoH = 0;
                      size_t colliderParts = 0, colliderTriangles = 0; };

/// @brief Everything world_manifest_json needs. `sky` and `spawn` are null or
///        their spec objects; only the schema's keys are copied (sky: json,
///        default, equirect; spawn: map, with unity always recomputed as
///        map_to_unity(map)), and one missing its required keys is written as null;
///        `light` null is built from scene->environment.light (world_light_json);
///        `counts` supplies chunksUnbaked, animated, animatedResolved and
///        clipsFailedDecode (missing ones are 0; the rest are computed).
///        `warnings` are the export's own, written after the scene's.
struct WorldManifestInput {
    const castlemist::world::WorldScene* scene = nullptr; const WorldFolderOptions* opts = nullptr;
    std::vector<ManifestModel> models; std::vector<ManifestCell> cells;
    nlohmann::json sky, light, spawn, counts; std::vector<std::string> warnings;
    nlohmann::json textures;   ///< {dir: string, count: integer >= 0} (ruling W8) or null = left out
    std::function<std::string(uint32_t propIndex)> clipOf;   ///< "" = null
};

/// @brief The manifest's `light` (spec D5, amended in Task 13): MapEnvLight::sunDir
///        as parsed from the env chunk is the direction the light travels (map
///        space, up = -Z: day presets store z > 0), so unityForward =
///        map_to_unity(sunDir), blenderDirection = map_to_blender(sunDir) and
///        sun.toSun = -sunDir (toward the sun); colours and intensities raw,
///        ambient = fillColor * min(fillIntensity, 1) (castlemist's choice).
nlohmann::json world_light_json(const castlemist::model::Extractor::MapEnvLight& l);

/// @brief The manifest of spec "manifest.json (version 1)".
///
/// - `models` is in.models in order; an instance's `model` is its position in
///   that array (the scene index is `models[k].index`).
/// - An instance is written when its model is in in.models with ok set and
///   unity_trs succeeds. Instances whose unity_trs fails are omitted and counted
///   in counts.propsSkipped with a warning "props: <n> placements with a
///   non-uniform or degenerate transform skipped"; those of a failed or missing
///   model likewise ("props: <n> placements of failed or missing models skipped").
/// - counts.props = sum of the cells' props, propsWritten + propsSkipped = props.
/// - warnings: scene->warnings as is, then in.warnings, then one line per
///   UNPROVEN setting ("settings: <name> ... UNPROVEN ..."), the flat-water note
///   (world_cell.h kFlatWaterNote) when the map has a plane but no preset, and
///   the skip lines above.
/// - Non-finite numbers (ruling W6) are never written, and never as null where
///   the schema wants a number. Each case gets a warning naming the field:
///   a non-finite value in an optional block (light, sky, spawn, water,
///   frameProbe) makes that block null; one in a cell's rect, origin or
///   budget.textureMB drops the cell (its placements count as skipped:
///   "props: <n> placements in dropped cells skipped"); non-finite map bounds
///   make `bounds` null; a non-finite option (unityScale, groundRepeats,
///   budget.maxTextureMB) is replaced by its default; a non-finite water plane z
///   makes the cells' water null; a non-finite chunk size leaves terrain.chunkSize
///   out. The result always validates against the schema.
nlohmann::json world_manifest_json(const WorldManifestInput& in);

/// @brief After the Blender FBX step (Task 11): sets `cells[].fbx`, `models[].fbx`
///        and `frameProbe.fbx` to the relative path of each .fbx that exists next
///        to its .glb (same stem), and to null where none does; rewrites
///        `<exportDir>/manifest.json` atomically (manifest.json.tmp, then rename).
///        Returns the count set. 0, and nothing written, when there is no
///        readable manifest or the write fails.
/// @param exportDirUtf8 The export folder, UTF-8.
size_t set_manifest_fbx(const std::string& exportDirUtf8);

} // namespace castlemist::exportgltf
