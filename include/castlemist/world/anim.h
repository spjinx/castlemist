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

/// @brief The clips a placement plays for animSequence, in order: the exact clip when one
///        matches; else the clips whose token is animSequence plus a suffix in the top nibble
///        (token64's 2-digit suffix), the 04 clip then the 08 one. Read from map 506592's vortex
///        506583, whose "on" placements name 0x1CF while its clips are on04 / on08 / on12: its
///        animated texmult rises over 04, holds over 08 and falls over 12, so 04 is the intro,
///        08 the loop and 12 the way out (UNPROVEN: the client's sequence code is not traced).
///        Empty for a token of 0 or a miss.
std::vector<int> resolve_prop_sequence(const std::vector<uint64_t>& clipTokens, uint64_t animSequence);

} // namespace castlemist::world
