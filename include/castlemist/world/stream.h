/// @file
/// @brief Prop model streaming order for the map view.
/// @ingroup world
#pragma once

#include "castlemist/world/world_scene.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace castlemist::world {

/// @brief Model indices ordered nearest-first by the closest instance to @p cam
///        (map space). Ties go to the lower index; models with no instance
///        come last, in index order.
std::vector<uint32_t> model_load_order(const WorldScene& s, const float cam[3]);

/// @brief Re-sorts @p models (any subset of model indices) by the rule of
///        model_load_order: nearest instance to @p cam first, ties to the
///        lower index, models with no instance last in index order.
void rank_models(const WorldScene& s, const float cam[3], std::vector<uint32_t>& models);

/// @brief The models still to load, nearest-first from the camera at the time
///        each one is taken: workers pop() from the front while the owner
///        reorder()s the remainder as the camera moves. Thread-safe. Holds a
///        reference to @p s, which must outlive it.
class ModelQueue {
public:
    ModelQueue(const WorldScene& s, const float cam[3]);
    /// @brief Takes the nearest remaining model; false when none is left.
    bool pop(uint32_t& model);
    /// @brief Re-ranks the not-yet-taken models from @p cam. A taken model never returns.
    void reorder(const float cam[3]);
    size_t remaining() const;

private:
    const WorldScene& s_;
    mutable std::mutex m_;
    std::vector<uint32_t> rest_;   ///< [head_, end) still to take, nearest first
    size_t head_ = 0;
};

} // namespace castlemist::world
