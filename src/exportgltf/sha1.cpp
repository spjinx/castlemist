/// @file
/// @brief SHA-1 (FIPS 180-4 section 6.1), one pass over a buffer.

#include "sha1.h"

#include <array>
#include <cstring>

namespace castlemist::exportgltf {

namespace {

inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void block(std::array<uint32_t, 5>& h, const uint8_t* p) {
    uint32_t w[80];
    for (int t = 0; t < 16; ++t)
        w[t] = uint32_t(p[4 * t]) << 24 | uint32_t(p[4 * t + 1]) << 16 | uint32_t(p[4 * t + 2]) << 8 | uint32_t(p[4 * t + 3]);
    for (int t = 16; t < 80; ++t) w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int t = 0; t < 80; ++t) {
        uint32_t f, k;
        if (t < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
        else if (t < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
        const uint32_t tmp = rotl(a, 5) + f + e + k + w[t];
        e = d; d = c; c = rotl(b, 30); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

}  // namespace

std::string sha1_hex(const void* data, size_t n) {
    std::array<uint32_t, 5> h = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto* p = static_cast<const uint8_t*>(data);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) block(h, p + i);
    // The tail, 0x80, zeros, then the length in bits (big-endian, 64 bits).
    uint8_t tail[128] = {};
    const size_t rest = n - i;
    if (rest) std::memcpy(tail, p + i, rest);
    tail[rest] = 0x80;
    const size_t len = rest + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = uint64_t(n) * 8;
    for (int k = 0; k < 8; ++k) tail[len - 1 - k] = uint8_t(bits >> (8 * k));
    block(h, tail);
    if (len == 128) block(h, tail + 64);
    static const char* hex = "0123456789abcdef";
    std::string out(40, '0');
    for (int k = 0; k < 5; ++k)
        for (int j = 0; j < 8; ++j) out[size_t(k * 8 + j)] = hex[(h[size_t(k)] >> (28 - 4 * j)) & 0xF];
    return out;
}

}  // namespace castlemist::exportgltf
