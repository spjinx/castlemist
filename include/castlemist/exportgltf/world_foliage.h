/// @file
/// @brief The world export's foliage: one `foliage/<cellId>.bin` of 100-byte
///        `castlemist-foliage-1` records per cell, the manifest's `foliage`
///        block and each foliage model's wind flag
///        (docs/superpowers/specs/2026-10-16-world-foliage-design.md D7, D9, D10,
///        "manifest.json additions").
///
/// Record layout (little-endian, no padding; spec D7):
///
/// | offset | type    | field |
/// |--------|---------|-------|
/// | 0      | u32     | model: index into the manifest's models[] |
/// | 4      | u16     | layer: index into foliage.layers[]; 0xFFFF for a collider linked by geometry only |
/// | 6      | u16     | flags: bit 0 has a collider; bits 1-2 source (0 collider, 1 scatter, 2 density); bit 3 Terrain tree; bit 4 outside the terrain |
/// | 8      | u32     | placement: index into the map's zone placements; 0xFFFFFFFF for a geometry-only collider |
/// | 12     | f32[12] | m: map-space 3 x 4, column-major, absolute |
/// | 60     | f32[3]  | unity.t: local to the cell node, before unityScale |
/// | 72     | f32[4]  | unity.r: quaternion x, y, z, w |
/// | 88     | f32     | unity.s: uniform scale |
/// | 92     | f32     | unityYaw: radians about Unity +Y (sign UNPROVEN, terrainTreeYawSign) |
/// | 96     | f32     | tiltDeg: degrees between the instance's up and map up |
/// @ingroup exportgltf
#pragma once

#include "castlemist/exportgltf/world_manifest.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/world/cells.h"
#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

inline constexpr size_t kFoliageRecordBytes = 100;
inline constexpr const char* kFoliageRecordLayout = "castlemist-foliage-1";

/// Record flag bits (spec D7).
inline constexpr uint16_t kFoliageFlagCollider = 1u << 0;
inline constexpr uint16_t kFoliageFlagTerrainTree = 1u << 3;   ///< set by the unityTerrain writer
inline constexpr uint16_t kFoliageFlagOutsideTerrain = 1u << 4;
inline constexpr int kFoliageSourceShift = 1;                  ///< bits 1-2: world::ZoneSource

// FoliageExport (Off, Colliders, All = whatever scene.zones.mode built) is in world_manifest.h.

/// @brief One record, decoded.
struct FoliageRecord {
    uint32_t model = 0; uint16_t layer = 0xFFFF; uint16_t flags = 0; uint32_t placement = 0xFFFFFFFF;
    float m[12]{};          ///< map-space 3x4, column-major: world[0..2], [4..6], [8..10], [12..14]
    UnityTrs unity{};       ///< unity_trs(world, cell.origin)
    float unityYaw = 0;     ///< f = R_unity * (0,0,1); atan2(f.x, f.z)  (sign UNPROVEN, terrainTreeYawSign)
    float tiltDeg = 0;      ///< degrees between normalize(M3 * (0,0,-1)) and (0,0,-1)
};

/// Little-endian, at the spec D7 offsets.
void encode_foliage_record(const FoliageRecord& r, uint8_t out[kFoliageRecordBytes]);
FoliageRecord decode_foliage_record(const uint8_t* in);

/// @brief One cell's records, sorted by (model, placement, collider). `model` is
///        the scene.models index here: export_world_folder maps it to the
///        manifest position (the manifest lists models in scene order, so the
///        order holds) and drops the records of failed models. Off gives none;
///        Colliders keeps the Collider-source instances. A layer index past
///        0xFFFE is written as 0xFFFF. Bit 4 is set when chunk_of_point clamps
///        the translation (world::point_in_chunk_grid false on a map with
///        terrain). Instances whose unity_trs fails (scale 0, a mirror, shear,
///        a non-finite number) are skipped and counted in @p skipped.
std::vector<FoliageRecord> cell_foliage_records(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c,
                                                FoliageExport mode, size_t& skipped);

/// @brief true / false / null per spec D7: true when any ok game material's
///        vsUniforms names "VegPara", false when there are ok game materials and
///        none does, null when there is no ok game material (not traced).
nlohmann::json model_wind(const ModelPreview& p);

/// @brief The manifest's "foliage" block (spec Data formats): scatter,
///        recordLayout, recordBytes, every scene.zones layer (its models by slot
///        as positions in @p models, null for a slot not in it), one entry per
///        model of @p models that a zone layer names or a zone instance uses
///        (zoneTypes and fadeFar from the layers that name it, wind from the
///        ManifestModel, terrainDetail = ok, one mesh, one material and a layer
///        type 1 or 2), and the collider link counts. Only the models that hold
///        records belong in @p models' foliage entries: pass @p used (parallel
///        to @p models, true where a record was written) to restrict them; empty
///        = every model a zone instance uses.
nlohmann::json foliage_manifest_json(const castlemist::world::WorldScene& s, const std::vector<ManifestModel>& models,
                                     const std::vector<char>& used = {});

} // namespace castlemist::exportgltf
