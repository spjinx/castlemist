/// @file
/// @brief SHA-1 (FIPS 180-4) as 40 lower-case hex digits: the world export's
///        content names (Task 15d: the generated-map pool textures/gen/ and the
///        shared material keys). Used for naming, not for security; Python's
///        hashlib.sha1 gives the same digest, so the validator can re-check it.
/// @ingroup exportgltf
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace castlemist::exportgltf {

/// The SHA-1 of @p n bytes at @p data, as 40 lower-case hex digits.
std::string sha1_hex(const void* data, size_t n);
inline std::string sha1_hex(const std::vector<uint8_t>& b) { return sha1_hex(b.data(), b.size()); }
inline std::string sha1_hex(std::string_view s) { return sha1_hex(s.data(), s.size()); }

}  // namespace castlemist::exportgltf
