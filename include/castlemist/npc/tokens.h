/// @file
/// @brief Packed-name tokens (5-bit letter groups in a u64) and clip-name grouping.
/// @ingroup npc

#pragma once

#include <cstdint>
#include <string>

/// @defgroup npc npc -- NPC catalogue
/// @brief Reads GW2 content packs to build a browsable NPC catalogue.

namespace castlemist::npc {

/// Decode a packed name token. The top 4 bits (a variant nibble) are cleared,
/// then 5-bit groups are read from the low end: 1..26 map to 'a'..'z', 0 to a
/// space. Decoding stops once the remaining value is 0. Returns "" if any group
/// is above 26 or the result is empty.
std::string decode_token(uint64_t token);

/// Group key for a clip name, for browsing only. Trailing digits and spaces are
/// stripped; if the name had trailing digits and the remainder is longer than 3
/// with 3 leading letters, those 3 are dropped. Spaces are then trimmed from
/// both ends.
std::string clip_group(const std::string& name);

/// The 3 leading letters clip_group dropped, or "" when none were.
std::string clip_prefix(const std::string& name);

} // namespace castlemist::npc
