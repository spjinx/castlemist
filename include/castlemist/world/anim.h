/// @file
/// @brief Prop animation clip resolution (pure CPU).
/// @ingroup world
#pragma once

#include <cstdint>
#include <vector>

namespace castlemist::world {

/// @brief Index of the clip whose token equals animSequence, note §5. A token of
///        0 means "none" and gives -1; a miss gives -1. No fallback: clips
///        resolve exactly or not at all.
int resolve_prop_clip(const std::vector<uint64_t>& clipTokens, uint64_t animSequence);

} // namespace castlemist::world
