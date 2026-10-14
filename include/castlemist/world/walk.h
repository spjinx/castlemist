/// @file
/// @brief CollisionIndex: map-space collision and terrain triangles in an XY
///        grid, with ray casts (the basis of walking and picking on a map).
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace castlemist::world {

/// @brief Which triangle set a ray hit.
enum class HitSource : uint8_t { Collision = 1, Terrain = 2 };

/// @brief Bit mask selecting triangle sets for a query.
enum HitMask : uint8_t { kHitCollision = 1, kHitTerrain = 2, kHitAll = 3 };

/// @brief The nearest ray hit.
struct RayHit {
    float t = 0;                    ///< distance along the (normalised) ray
    float point[3]{};               ///< hit position, map space
    float normal[3]{};              ///< unit geometric normal, facing the ray origin
    HitSource source = HitSource::Collision;
};

/// @brief Triangles of a map in map space (+X east, +Y north, up = -Z), bucketed
///        into a uniform XY grid.
class CollisionIndex {
public:
    /// @brief Collision instances (transformed by `instance.world`) and, if
    ///        `includeTerrain`, two triangles per terrain sample quad.
    static CollisionIndex build(const WorldScene& scene, bool includeTerrain);

    /// @brief Build from raw map-space triangles (9 floats each), for tests and tools.
    static CollisionIndex from_triangles(const std::vector<float>& collisionTris,
                                         const std::vector<float>& terrainTris);

    /// @brief Nearest hit within `maxT`; `dir` need not be unit (t is in units of the
    ///        normalised direction). Triangles are hit from both faces.
    std::optional<RayHit> raycast(const float origin[3], const float dir[3], float maxT,
                                  uint8_t mask = kHitAll) const;

    size_t triangle_count(uint8_t mask = kHitAll) const;
    bool empty() const { return triangle_count() == 0; }

    /// @brief Axis-aligned bounds of all triangles (zeros when empty).
    void bounds(float lo[3], float hi[3]) const;

private:
    std::vector<float> tris_;           ///< collision triangles first, then terrain; 9 floats each
    size_t collisionCount_ = 0;         ///< triangles in the collision set
    float lo_[3] = {0, 0, 0}, hi_[3] = {0, 0, 0};
    float cell_ = 1;                    ///< grid cell edge
    int nx_ = 0, ny_ = 0;
    std::vector<uint32_t> cellStart_;   ///< nx_*ny_+1 offsets into cellTris_
    std::vector<uint32_t> cellTris_;

    void finish();                      ///< compute bounds and fill the grid
};

} // namespace castlemist::world
