/// @file
/// @brief The Texture Gallery's pure parts (texture_gallery_model.cpp): entry
///        kinds, texture headers, the filter and its id-list parsing, image
///        decode for thumbnails and exports, and the thumbnail cache.
///
/// The window itself (texture_gallery.cpp) owns the threads and the drawing;
/// everything here is plain data in, data out, so it can be tested without a
/// dat or a window.
///
/// @warning Not a public header -- internal to `src/ui/`.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "castlemist/native/gw2dat.h"

namespace castlemist::ui::gallery {

/// @brief The gallery's four kind checkboxes.
enum class Kind : uint8_t {
    Ui,     ///< ATEU (and its CTEU sibling): the game's UI art.
    World,  ///< ATEX / ATEP / CTEX and the rest of the ArenaNet family.
    Web,    ///< RIFF (WebP) and PNG: mostly launcher art.
    Dds,    ///< plain "DDS " files.
};
constexpr int kKindCount = 4;

/// @brief The kind of an index row, from its `magic` and `type` columns.
Kind kind_of(std::string_view magic, std::string_view type);

/// @name Format codes
/// @brief Header::format: the ATEX format enum (0..37, see gw2_atex.hpp's
///        format_name) for ArenaNet textures and DDS files alike, or one of these.
/// @{
constexpr uint8_t kFmtPng = 40;
constexpr uint8_t kFmtWebp = 41;
constexpr uint8_t kFmtUnknown = 255;
/// @}

/// "DXT5", "BC7", "PNG", "WebP", ... ("?" for unknown).
std::string format_label(uint8_t format);

/// @brief What a texture's first bytes say about it.
///
/// Packs into 64 bits (pack/unpack) so the scan workers can publish it through
/// a std::atomic<uint64_t> per entry; 0 is "not read yet".
struct Header {
    uint16_t width = 0, height = 0;
    uint8_t format = kFmtUnknown;
    bool known = false;   ///< read (successfully or not)
    bool image = false;   ///< decodes as an image; known && !image entries are never shown
    bool alpha = false;   ///< the format carries alpha (DXT2-5, BC7, PNG/WebP with alpha, ...)

    uint64_t pack() const;
    static Header unpack(uint64_t v);
    int longest() const { return width > height ? width : height; }
};

/// @brief Reads a Header from the start of a decompressed entry: the ATEX
///        family (and CTEX siblings), DDS, PNG and WebP. Anything else, or a
///        prefix too short to tell, comes back known and not an image.
Header read_header(const uint8_t* d, size_t n);

/// @brief A search box query: one id, ranges (`21900-22100`) or a pasted list,
///        separated by spaces, commas, semicolons or newlines.
struct IdQuery {
    std::vector<std::pair<uint32_t, uint32_t>> ranges;  ///< sorted, merged, inclusive
    bool empty() const { return ranges.empty(); }
    bool contains(uint32_t id) const;
};

/// @brief Parses the search box. Empty text is an empty query (matches all).
///        False (and @p error set, @p out untouched) on a token that is not an
///        id or a range.
bool parse_id_query(std::string_view text, IdQuery& out, std::string& error);

/// @brief The square icon sizes "exclude square icon sizes" drops: 16, 32, 64, 128.
bool is_icon_size(int width, int height);

/// @brief The top bar's settings.
struct Filter {
    bool kinds[kKindCount] = {true, false, false, false};
    int min_side = 0;      ///< longest side at least this (0 = no minimum)
    int max_side = 0;      ///< longest side at most this (0 = no maximum)
    bool exclude_icons = false;
    bool alpha_only = false;
    IdQuery ids;

    /// The size, icon or alpha settings need the header to decide.
    bool needs_header() const { return min_side > 0 || max_side > 0 || exclude_icons || alpha_only; }
};

/// @brief Whether an entry passes. An entry whose header is known not to be an
///        image never passes; one whose header is not read yet passes only when
///        the filter does not need it (the list refines as headers arrive).
bool matches(const Filter& f, Kind kind, uint32_t base_id, const Header& h);

/// @brief A decoded image, RGBA8, rows top-down.
struct Rgba {
    int width = 0, height = 0;
    std::vector<uint8_t> px;
};

/// @brief Decodes a whole decompressed entry. With @p min_side > 0 an ATEX
///        texture decodes the smallest mip whose longest side is still at
///        least that (a thumbnail does not need mip 0); 0 = full size.
///        Never throws; false when it is not a decodable image.
bool decode_image(std::vector<uint8_t> bytes, int min_side, Rgba& out);

/// @brief A thumbnail: BGRA (alpha 255), rows top-down, ready for StretchDIBits.
struct Thumb {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
    size_t bytes() const { return bgra.size() + sizeof(Thumb); }
};

/// @brief Fits @p img into @p box x @p box (box filter, never upscales) over a
///        checkerboard, so alpha shows. An empty image gives an empty thumb.
Thumb make_thumb(const Rgba& img, int box);

/// @brief A memory-capped LRU of thumbnails by base id. UI thread only.
class ThumbCache {
public:
    explicit ThumbCache(size_t cap_bytes) : cap_(cap_bytes) {}
    /// The thumb, marked most recently used; nullptr when absent.
    const Thumb* get(uint32_t id);
    bool contains(uint32_t id) const { return map_.count(id) != 0; }
    /// Adds (or replaces) and evicts least recently used thumbs until the cache
    /// is within its cap; the one just added is never evicted.
    void put(uint32_t id, Thumb t);
    void clear();
    size_t bytes() const { return bytes_; }
    size_t size() const { return map_.size(); }
    size_t cap() const { return cap_; }

private:
    using Item = std::pair<uint32_t, Thumb>;
    std::list<Item> lru_;  ///< front = most recently used
    std::unordered_map<uint32_t, std::list<Item>::iterator> map_;
    size_t bytes_ = 0, cap_;
};

/// @name Reading entries
/// @brief Off any thread, through the caller's own stream on the dat.
/// @{
/// The first @p want decompressed bytes (fewer when the entry is shorter):
/// reads only the start of the entry and inflates no further than needed.
std::vector<uint8_t> read_prefix(std::ifstream& dat, const MftData& e, size_t want);
/// The whole decompressed entry.
std::vector<uint8_t> read_whole(std::ifstream& dat, const MftData& e);
/// @}

/// @name The header side file
/// @brief Headers read by the scan, kept next to the index so the next gallery
///        opens with them. Tied to the dat fingerprint: after a patch the file
///        is ignored and the scan runs again.
/// @{
bool save_headers(const std::filesystem::path& path, const std::string& fingerprint,
                  const std::vector<std::pair<uint32_t, uint64_t>>& headers);
/// False when the file is missing, damaged or for another fingerprint.
bool load_headers(const std::filesystem::path& path, const std::string& fingerprint,
                  std::vector<std::pair<uint32_t, uint64_t>>& headers);
/// @}

} // namespace castlemist::ui::gallery
