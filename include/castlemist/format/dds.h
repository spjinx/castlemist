#ifndef GW2_DDS_H
#define GW2_DDS_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace castlemist::dds {

/// Just enough info to hand a decoded block off to Direct3D 11: the texture
/// dimensions actually used by the GPU, the DXGI format, the byte offset of
/// pixel data inside the buffer, and the D3D11 SysMemPitch (bytes per row of
/// blocks -- NOT the same as a DDS file's dwPitchOrLinearSize field, which for
/// block-compressed textures holds the *total* mip size instead).
struct DdsInfo {
    uint32_t dxgi_format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sys_mem_pitch = 0;
    size_t data_offset = 0;
};

/// Parses a standalone "DDS " file buffer (a real DDS stored directly in the
/// archive) into a DdsInfo, so its native BCn blocks can be uploaded straight
/// to the GPU. Returns std::nullopt if the buffer isn't a recognized BC1-BC7 DDS.
std::optional<DdsInfo> parse_dds(const uint8_t* data, size_t size);

/// Decodes the top mip of an uncompressed 32-bit DDS (B8G8R8A8, R8G8B8A8 or
/// their X8 forms, legacy masks or DX10 header) into RGBA8, row 0 = the first
/// stored row. A layout without an alpha mask comes out opaque. Returns false
/// for block-compressed or other formats and for a file too short for its
/// pixels. Used for GW2's plain-DDS star atlas (docs/research/gw2-sky.md §8.2),
/// which the ATEX decoder does not read.
bool decode_rgba8(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);

} // namespace castlemist::dds

#endif // GW2_DDS_H
