/// @file
/// @brief Layer-internal: decompression of one raw cntc MFT entry, tolerant of corrupt streams.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace castlemist::npc::detail {

/// @brief Turns the raw bytes of one MFT entry (as read from the dat) into file bytes.
/// @param raw Entry bytes including the periodic CRC32 framing.
/// @param compressed True when the entry's compression flag is set.
/// @return The decompressed bytes; empty (never throws) when the stream is too short or corrupt.
std::vector<uint8_t> decompress_cntc(std::span<const uint8_t> raw, bool compressed);

}  // namespace castlemist::npc::detail
