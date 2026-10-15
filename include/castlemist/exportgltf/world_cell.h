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
///        cell with no chunks (c_all).
CellMesh build_cell_terrain(const castlemist::world::Terrain& t, const castlemist::world::Cell& c);

/// @brief The cell rect's full sample grid at z = planeZ (cell-local), normals
///        up, uv as the terrain. Empty when bake_cell_water would return false
///        (no plane, no water preset, or no terrain below the plane at
///        kWaterTexelsPerChunk), or the cell has no chunks.
CellMesh build_cell_water(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c);

/// @brief The cell's collision instances' triangles (world::collision_instance_triangles),
///        cell-local, unwelded (3 vertices per triangle, no normals or uvs), split
///        into parts of at most floor(maxVertices/3) triangles.
std::vector<CellMesh> build_cell_colliders(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c,
                                           size_t maxVertices = 65535);

/// @brief Image URIs, relative to the .glb. An empty URI leaves that material untextured.
struct CellImages { std::string albedoUri, waterUri; };

/// @brief Root "GW2_MapToGltf" (map_to_gltf_root(1)) -> "Cell" (translation = origin)
///        -> "Terrain", "Water", "Collider_k" (each only when its mesh has vertices).
///        Materials "Terrain" (baseColor albedo), "Water" (baseColor water, BLEND),
///        "Collision" (no texture, double-sided: hull winding is UNPROVEN). Indices
///        are 16-bit when a mesh has at most 65535 vertices. Parent directories are created.
bool write_cell_gltf(const std::string& glbPathUtf8, const castlemist::world::Cell& c, const CellMesh& terrain,
                     const CellMesh* water, const std::vector<CellMesh>& colliders, const CellImages& images,
                     std::string* error = nullptr);

} // namespace castlemist::exportgltf
