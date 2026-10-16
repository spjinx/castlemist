/// @file
/// @brief One export cell -> one .glb: the cell's welded terrain (baked
///        albedo), its water plane and its collision split into parts
///        (docs/superpowers/specs/2026-10-15-world-export-design.md D1, D3, D4).
///
/// Every mesh is cell-local map space (map position minus Cell::origin); the
/// "Cell" node carries the origin as its translation and the root
/// "GW2_MapToGltf" the map -> glTF axis change (map_to_gltf_root(1)). A
/// triangle faces up when its cross product (b-a)x(c-a) points along map -Z;
/// the root's reflection keeps that face the front one after import
/// (world_export.cpp, world_probe.cpp).
/// @ingroup exportgltf
#pragma once

#include "castlemist/world/cells.h"
#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::exportgltf {

/// @brief Texels per chunk side of a cell's water texture (spec D3, castlemist's choice).
inline constexpr int kWaterTexelsPerChunk = 32;

/// @brief The note a flat-colour Water material carries (glTF `extras.note`) and
///        the manifest repeats: the map has a plane but no env water preset.
inline constexpr const char* kFlatWaterNote = "water: no preset; flat colour (note \xc2\xa7" "6)";

/// @brief A triangle mesh: positions (xyz), normals (xyz, may be empty), uvs (may be empty), indices.
struct CellMesh {
    std::vector<float> pos, nrm, uv;
    std::vector<uint32_t> idx;
    size_t vertices() const { return pos.size() / 3; }
};

/// @brief Welded: one vertex per distinct sample of the cell's chunks
///        ((w*seg+1) x (h*seg+1) for a full cell, row-major from the north-west
///        corner), positions minus cell.origin, normals from world::chunk_normals
///        of the owning chunk, uv (0..1 across the cell rect, v = 0 north).
///        Winding as world_export.cpp (cross product along -Z = up). A missing
///        chunk leaves its quads (and the samples only it owns) out. Empty for a
///        cell with no chunks (c_all). A chunk that is present but unusable (no
///        samples, wrong height count) or whose samples per side differ from the
///        cell's first usable chunk is left out too, with one line in @p warnings
///        naming the cell and the chunk (cx, cy).
CellMesh build_cell_terrain(const castlemist::world::Terrain& t, const castlemist::world::Cell& c,
                            std::vector<std::string>* warnings = nullptr);

/// @brief The cell rect's full sample grid at z = planeZ (cell-local), normals
///        up, uv as the terrain. Empty without a plane, when the cell has no
///        chunks (c_all: a known limit), or when !world::water_below_plane at
///        @p waterTexelsPerChunk, the bake's own grid (pass the export's
///        WorldFolderOptions::waterTexelsPerChunk, so the mesh and the bake agree;
///        review M5). A missing env preset does not empty it (spec D3 "No
///        preset"): write_cell_gltf then gives it the flat colour.
CellMesh build_cell_water(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c,
                          int waterTexelsPerChunk = kWaterTexelsPerChunk);

/// @brief The cell's collision instances' triangles (world::collision_instance_triangles),
///        cell-local, unwelded (3 vertices per triangle, no normals or uvs), split
///        into parts of at most floor(maxVertices/3) triangles.
std::vector<CellMesh> build_cell_colliders(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c,
                                           size_t maxVertices = 65535);

/// @brief Image URIs, relative to the .glb. An empty albedoUri leaves Terrain untextured;
///        an empty waterUri (no env preset, so no baked water texture) makes Water the
///        flat colour world::kFlatWaterTint / kFlatWaterAlpha with extras.note kFlatWaterNote.
struct CellImages { std::string albedoUri, waterUri; };

/// @brief Root "GW2_MapToGltf" (map_to_gltf_root(1)) -> "Cell" (translation = origin)
///        -> "Terrain", "Water", "Collider_k" (each only when its mesh has vertices).
///        Materials "Terrain" (baseColor albedo), "Water" (baseColor water or the flat colour, BLEND),
///        "Collision" (no texture, double-sided: hull winding is UNPROVEN). Indices
///        are 16-bit when a mesh has at most 65535 vertices. Parent directories are created.
bool write_cell_gltf(const std::string& glbPathUtf8, const castlemist::world::Cell& c, const CellMesh& terrain,
                     const CellMesh* water, const std::vector<CellMesh>& colliders, const CellImages& images,
                     std::string* error = nullptr);

} // namespace castlemist::exportgltf
