/// @file
/// @brief Props: how a map's prop placements become WorldScene::models and
///        WorldScene::props, with world matrices in map space.
///
/// The transform is the client's own, proven in
/// docs/research/gw2-world-frame.md §5 (Gw2-64.exe's prop transform helper,
/// cross-checked against T3D on three maps).
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/world_scene.h"

#include <vector>

namespace castlemist::world {

/// @brief Fill `out.models` (distinct fileIds, in first-seen order),
///        `out.props` (one per input, in input order) and
///        `out.motion.animatedProps` (indices of `group == "propAnimArray"`).
///
/// `PropInstance::world` is column-major for column vectors (p' = M * p),
/// map space: the client's 3x3 rotation times `scale`, translation `pos` in
/// elements 12-14. Replaces anything previously in those three members.
void build_props(const std::vector<castlemist::model::Extractor::MapProp>& in, WorldScene& out);

} // namespace castlemist::world
