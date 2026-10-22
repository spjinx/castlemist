// The first entry of a zip (arcdps' .zevtc), stored or deflated. Deflate goes through
// stb_image's zlib decoder: a private (static) copy, like the other layers keep theirs.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "castlemist/skillfx/evtc.h"

#include <cstdlib>
#include <cstring>

namespace castlemist::skillfx {

std::vector<uint8_t> unzip_first(std::span<const uint8_t> z) {
    auto u16 = [&](size_t p) -> uint32_t { return p + 2 <= z.size() ? z[p] | (z[p + 1] << 8) : 0; };
    auto u32 = [&](size_t p) -> uint32_t { return p + 4 <= z.size() ? u16(p) | (u16(p + 2) << 16) : 0; };
    if (z.size() < 30 || u32(0) != 0x04034b50) return {};
    const uint32_t method = u16(8);
    uint32_t csize = u32(18);
    const size_t data = 30 + static_cast<size_t>(u16(26)) + u16(28);
    if (data > z.size()) return {};
    // Bit 3 (sizes in a trailing data descriptor) leaves 0 here: take the rest of the file.
    if (csize == 0 || data + csize > z.size()) csize = static_cast<uint32_t>(z.size() - data);
    if (method == 0) return {z.begin() + static_cast<std::ptrdiff_t>(data), z.begin() + static_cast<std::ptrdiff_t>(data + csize)};
    if (method != 8) return {};
    int n = 0;
    char* out = stbi_zlib_decode_noheader_malloc(reinterpret_cast<const char*>(z.data() + data), static_cast<int>(csize), &n);
    if (!out) return {};
    std::vector<uint8_t> v(reinterpret_cast<uint8_t*>(out), reinterpret_cast<uint8_t*>(out) + n);
    STBI_FREE(out);
    return v;
}

}  // namespace castlemist::skillfx
