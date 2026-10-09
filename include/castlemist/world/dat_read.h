/// @file
/// @brief Reading one file's bytes out of the dat for the world layer, with
///        dat I/O failure kept apart from bad data.
///
/// The world layer's error policy (spec, global constraints): dat I/O failure
/// is an error everywhere, bad data never is. read_file_bytes is the one place
/// the layer reads the dat; it throws DatIoError for I/O, and run_section
/// (load_world.h) rethrows that type instead of turning it into a warning.
/// @ingroup world
#pragma once

#include "castlemist/native/gw2dat.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace castlemist::world {

/// @brief The dat's bytes could not be read (open, seek or short read).
///        The world layer never turns it into a warning: it reaches
///        load_world's caller.
class DatIoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief Look @p fileId up by base id, read its entry and decompress it.
/// @return std::nullopt when the dat has no such file (a data question the
///         caller words itself).
/// @throws DatIoError "file <id>: cannot read: ..." when reading the entry fails.
/// @throws std::runtime_error (not DatIoError) when decompression fails: that
///         is bad data, which a caller may turn into a warning.
std::optional<std::vector<uint8_t>> read_file_bytes(Gw2Dat& dat, uint32_t fileId);

} // namespace castlemist::world
