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

/// @brief Tunables for the Walker; defaults follow T3D's PhysicsController.
struct WalkParams {
    float gravity = 2800, jumpVelocity = 900, eyeHeight = 120, padding = 18,
          groundSnap = 6, maxFall = 8000;
    float probeBelowEye[3] = {0, 60, 108};   ///< wall probes, distance below the eye
    float maxDt = 0.1f;                      ///< frame time clamp
};

/// @brief Outcome of one Walker step.
struct StepResult {
    bool grounded = false;
    bool recovered = false;   ///< fell too far and was returned to a safe position
    bool gaveUp = false;      ///< recovery failed: no ground at the safe/fallback position
};

/// @brief First-person walk controller over a CollisionIndex (up = -Z). `pos` is
///        the eye position. Behaviour re-derived from T3D's PhysicsController
///        (explorer/src/scene/physics-controller.ts, commit b3428b2).
class Walker {
public:
    explicit Walker(const CollisionIndex* index, WalkParams p = {});
    void reset(const float pos[3]);          ///< fallback = pos; clears motion
    bool snap_to_ground(float pos[3]);       ///< highest surface under XY; eye = ground - eyeHeight
    void jump() { jumpRequested_ = true; }   ///< honoured on the next step only if grounded
    StepResult step(float pos[3], const float horizontalDelta[2], float dt);
    bool grounded() const { return grounded_; }
    float vertical_velocity() const { return vy_; }   ///< positive = moving up (toward -Z)

private:
    const CollisionIndex* index_;
    WalkParams p_;
    bool grounded_ = false, jumpRequested_ = false, hasSafe_ = false;
    float vy_ = 0;
    float fallback_[3] = {0, 0, 0}, safe_[3] = {0, 0, 0};

    bool blocked(const float pos[3], float mx, float my) const;
    bool ground_check(float pos[3], float prevZ);   ///< snaps pos z when within groundSnap
};

} // namespace castlemist::world
