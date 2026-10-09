/// @file
/// @brief read_file_bytes: the world layer's one dat read (see dat_read.h).

#include "castlemist/world/dat_read.h"

#include "castlemist/native/cmp_decompress_method0.hpp"

#include <exception>
#include <string>

namespace castlemist::world {

std::optional<std::vector<uint8_t>> read_file_bytes(Gw2Dat& dat, uint32_t fileId) {
    const uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base > dat.mft_data_list.size()) return std::nullopt;
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw;
    try {
        raw = read_entry_bytes(dat.file_info.file_path, e);
    } catch (const std::exception& what) {
        throw DatIoError("file " + std::to_string(fileId) + ": cannot read: " + what.what());
    }
    if (!e.compression_flag) return raw;
    return castlemist::cmp::decompress_entry(raw);   // a failure here is bad data, not I/O
}

} // namespace castlemist::world
