/// @file
/// @brief Model LODs for the world export (Task 15b): the client's switch-distance
///        rules, the LOD levels a model's meshes give, and the manifest's `lods`.
///
/// Research: docs/research/gw2-model-lods.md (amendment 2026-10-17: the client's
/// rule, from static disassembly of Gw2-64.exe). In short:
///   * a MODL mesh has LOD0 (`indices`) plus `lods[]` index sets over the same
///     vertices; the client selects level 0, 1 or 2 (two distances), so at most
///     3 levels are exported (kMaxLodLevels);
///   * the model's distances are 3500 / 7500 model units, each replaced by
///     `ModelFileData.lodOverride[k]` when it is > 0, clamped to 100 000
///     (model ctor 0x140cf8184, model load 0x140cf7b50);
///   * a prop with MapPropFlags bit 16 (OVERRIDE_LOD) and lod1 / lod2 not both 0
///     replaces them with lod1 / lod2, negatives made 0 and clamped to 100 000
///     (PrProp 0x140cb3f05 -> model setter 0x140cfa0e0);
///   * the level is picked from the camera's 3D distance to the model's bounds
///     against distance_k x (the instance's largest axis scale) x a global 1.0
///     (0x140cf9010; the global is .data 0x1425927f8, initial 1.0, UNPROVEN that
///     it stays 1), with a 75-unit hysteresis that Unity's LODGroup has no
///     counterpart for.
/// Because the switch distance scales with the instance, a Unity screen-relative
/// height computed from the prefab's own size and the model distance is the same
/// for every instance (UnityLods.ScreenHeight in the importer).
/// @ingroup exportgltf
#pragma once

#include "castlemist/extract/model_types.h"

#include <cstdint>
#include <vector>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

/// MapPropFlags bit 16, OVERRIDE_LOD (g_MapPropFlagNames[16]; Gw2-64.exe 0x140cb3f05).
inline constexpr uint32_t kPropFlagOverrideLod = 1u << 16;
/// The client's model defaults, model units (Gw2-64.exe 0x141ae20d0, 0x141ee186c).
inline constexpr float kLodDefaultDistance[2] = {3500.0f, 7500.0f};
/// The client's clamp on every switch distance (0x141b991f4).
inline constexpr float kLodMaxDistance = 100000.0f;
/// LOD0..LOD2: the client selects from two distances.
inline constexpr int kMaxLodLevels = 3;

/// @brief The model's switch distances: lodOverride[k] when > 0 (clamped to
///        kLodMaxDistance), else kLodDefaultDistance[k]. @p fromFile[k] says which.
void model_lod_distances(const float lodOverride[2], float out[2], bool fromFile[2]);

/// @brief A placement's own distances: true, with @p out, when @p flags has
///        OVERRIDE_LOD and lod1, lod2 are not both 0; each is then made 0 when
///        negative and clamped to kLodMaxDistance (the client's setter). False
///        (the model's distances apply) otherwise, or when either is not finite.
bool prop_lod_override(float lod1, float lod2, uint32_t flags, float out[2]);

/// @brief A mesh is drawn (and exported) when it has vertices and >= 3 LOD0 indices
///        (write_meshes' rule).
bool mesh_drawable(const ModelMeshCPU& m);

/// @brief The index set mesh @p m draws at @p level: the highest level j <= level
///        whose set has >= 3 indices; -1 = LOD0 (`indices`), else an index into
///        `lodIndices` (j - 1). A mesh without its own level repeats its coarser one,
///        as the map renderer's flatten does (whether the client hides it is UNPROVEN).
int mesh_lod_source(const ModelMeshCPU& m, int level);

/// @brief 1 + the highest level any drawable mesh has its own set for, capped at
///        kMaxLodLevels; 1 = no LODs.
int model_lod_levels(const ModelPreview& m);

/// @brief Triangles drawn at each of @p levels levels (mesh_lod_source per mesh).
std::vector<uint64_t> model_lod_triangles(const ModelPreview& m, int levels);

/// @brief The manifest's models[].lods: {levels, distances [2], distanceSource
///        ["lodOverride" | "default"] x2, lodOverride ([2] as stored, or null when
///        the MODL version has no field), triangles [levels]}.
nlohmann::json model_lods_json(const ModelPreview& m);

} // namespace castlemist::exportgltf
