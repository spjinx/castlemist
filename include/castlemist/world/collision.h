/// @file
/// @brief Collision: how a map's havk hulls and placements become
///        WorldScene::collision (docs/research/gw2-world-frame.md §7).
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

namespace castlemist::world {

/// @brief Fill `out.collision`: one CollisionMesh per havk hull (same index,
///        hull-local), and one CollisionInstance per (placement, collision
///        index) the placement resolves to, placements in `h.placements` order.
///
/// A placement resolves through `geometries[geometryIndex].animations[last]`
/// to that animation's `collisionIndices[]` (§7.1). Its world matrix is the
/// client's placement transform (props.h, client_world_matrix) with scale
/// `32 * scale` (§7.2), times diag(1, 1, -1) (§7.3): a reflection, so a
/// hull triangle's winding reverses in map space. References out of range
/// are skipped with one aggregated warning in `out.warnings`; so are hull
/// triangles that index past their hull. One warning each also counts
/// placements whose `sequence` names an animation other than the last
/// (§7.1, the rule is UNPROVEN), geometries with no animations, and scale 0.
/// Never throws for bad data.
/// Replaces anything previously in `out.collision`.
void build_collision(const castlemist::model::Extractor::MapHavok& h, WorldScene& out);

} // namespace castlemist::world
