/// @file
/// @brief Prop model streaming order for the map view.
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <cstdint>
#include <vector>

namespace castlemist::world {

/// @brief Model indices ordered nearest-first by the closest instance to @p cam
///        (map space). Ties go to the lower index; models with no instance
///        come last, in index order.
std::vector<uint32_t> model_load_order(const WorldScene& s, const float cam[3]);

} // namespace castlemist::world
