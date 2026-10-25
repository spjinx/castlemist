/// @file
/// @brief The Texture Gallery's pure parts: see detail/texture_gallery.h.

#include "detail/texture_gallery.h"

#include <algorithm>
#include <cstring>
#include <span>

#include "castlemist/format/dds.h"
#include "castlemist/format/sdk_image.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"

namespace castlemist::ui::gallery {
namespace {

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// The ArenaNet texture magics gw2_atex decodes, and their C-prefixed siblings
// (same layout; the decoder wants the A form).
bool is_anet_texture(const uint8_t* d, size_t n) {
    if (n < 4 || (d[0] != 'A' && d[0] != 'C')) return false;
    static const char* const kTails[] = {"TEX", "TTX", "TEC", "TEP", "TEU", "TET"};
    for (const char* t : kTails)
        if (std::memcmp(d + 1, t, 3) == 0) return true;
    return false;
}

bool format_alpha(int fmt) {
    switch (fmt) {
    case 0: case 1: case 2: case 3: case 5: case 6:   // ARGB float / 2101010 / 8888 / 4444 / 1555
    case 15: case 16: case 19:                        // AL88, AL44, A8
    case 23: case 24: case 25: case 26:               // DXT2-5
    case 32: case 35:                                 // BC7, ABGR8888
        return true;
    default:
        return false;
    }
}

// DXGI_FORMAT -> the ATEX enum that decodes the same blocks (-1: none).
int dxgi_to_atex(uint32_t dxgi) {
    switch (dxgi) {
    case 71: case 72: return 22;  // BC1
    case 74: case 75: return 24;  // BC2
    case 77: case 78: return 26;  // BC3
    case 80: case 81: return 27;  // BC4
    case 83: case 84: return 31;  // BC5
    case 98: case 99: return 32;  // BC7
    case 28: case 29: return 35;  // R8G8B8A8
    case 87: case 91: return 3;   // B8G8R8A8
    case 88: case 93: return 4;   // B8G8R8X8
    default: return -1;
    }
}

Header dds_header(const uint8_t* d, size_t n) {
    Header h;
    h.known = true;
    if (n < 128) return h;
    h.height = static_cast<uint16_t>(std::min<uint32_t>(le32(d + 12), 0xFFFF));
    h.width = static_cast<uint16_t>(std::min<uint32_t>(le32(d + 16), 0xFFFF));
    const uint32_t pf_flags = le32(d + 80);
    int fmt = -1;
    if (pf_flags & 0x4) {  // DDPF_FOURCC
        const uint8_t* cc = d + 84;
        if (std::memcmp(cc, "DX10", 4) == 0) {
            if (n >= 148) fmt = dxgi_to_atex(le32(d + 128));
        } else if (std::memcmp(cc, "ATI1", 4) == 0 || std::memcmp(cc, "BC4U", 4) == 0) {
            fmt = 27;
        } else if (std::memcmp(cc, "ATI2", 4) == 0 || std::memcmp(cc, "BC5U", 4) == 0) {
            fmt = 31;
        } else if (cc[0] == 'D' && cc[1] == 'X' && cc[2] == 'T' && cc[3] >= '1' && cc[3] <= '5') {
            fmt = 22 + (cc[3] - '1');
        }
    } else if ((pf_flags & 0x40) && le32(d + 88) == 32) {  // DDPF_RGB, 32 bits
        fmt = (pf_flags & 0x1) && le32(d + 104) ? 3 : 4;
    }
    h.format = fmt < 0 ? kFmtUnknown : static_cast<uint8_t>(fmt);
    h.image = fmt >= 0 && h.width > 0 && h.height > 0;
    h.alpha = fmt >= 0 && format_alpha(fmt);
    return h;
}

Header png_header(const uint8_t* d, size_t n) {
    Header h;
    h.known = true;
    if (n < 33 || std::memcmp(d + 12, "IHDR", 4) != 0) return h;
    h.width = static_cast<uint16_t>(std::min<uint32_t>(be32(d + 16), 0xFFFF));
    h.height = static_cast<uint16_t>(std::min<uint32_t>(be32(d + 20), 0xFFFF));
    h.format = kFmtPng;
    h.image = h.width > 0 && h.height > 0;
    const uint8_t colour = d[25];
    h.alpha = colour == 4 || colour == 6;
    // A palette or grey image can still carry alpha in a tRNS chunk, which sits
    // before the first IDAT.
    for (size_t p = 8; !h.alpha && p + 8 <= n;) {
        const uint32_t len = be32(d + p);
        if (std::memcmp(d + p + 4, "tRNS", 4) == 0) h.alpha = true;
        if (std::memcmp(d + p + 4, "IDAT", 4) == 0) break;
        p += 12 + static_cast<size_t>(len);
    }
    return h;
}

Header webp_header(const uint8_t* d, size_t n) {
    Header h;
    h.known = true;
    if (n < 30 || std::memcmp(d + 8, "WEBP", 4) != 0) return h;
    const uint8_t* c = d + 12;
    if (std::memcmp(c, "VP8 ", 4) == 0) {
        h.width = static_cast<uint16_t>((d[26] | (d[27] << 8)) & 0x3FFF);
        h.height = static_cast<uint16_t>((d[28] | (d[29] << 8)) & 0x3FFF);
    } else if (std::memcmp(c, "VP8L", 4) == 0) {
        const uint32_t b = le32(d + 21);
        h.width = static_cast<uint16_t>((b & 0x3FFF) + 1);
        h.height = static_cast<uint16_t>(((b >> 14) & 0x3FFF) + 1);
        h.alpha = (b >> 28) & 1;
    } else if (std::memcmp(c, "VP8X", 4) == 0) {
        h.alpha = (d[20] & 0x10) != 0;
        h.width = static_cast<uint16_t>(std::min<uint32_t>((d[24] | (d[25] << 8) | (d[26] << 16)) + 1, 0xFFFF));
        h.height = static_cast<uint16_t>(std::min<uint32_t>((d[27] | (d[28] << 8) | (d[29] << 16)) + 1, 0xFFFF));
    } else {
        return h;
    }
    h.format = kFmtWebp;
    h.image = h.width > 0 && h.height > 0;
    return h;
}

// The bytes a block-compressed surface of this size needs.
size_t surface_bytes(int fmt, int w, int h) {
    const bool block = atex::FORMAT_FLAGS[fmt] & 1;
    if (!block) return static_cast<size_t>(w) * h * 4;
    const size_t blocks = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4);
    return blocks * (fmt == 22 || fmt == 27 ? 8 : 16);
}

bool decode_dds(const std::vector<uint8_t>& b, Rgba& out) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;
    if (dds::decode_rgba8(b.data(), b.size(), w, h, px)) {
        out.width = static_cast<int>(w);
        out.height = static_cast<int>(h);
        out.px = std::move(px);
        return true;
    }
    const Header hd = dds_header(b.data(), b.size());
    if (!hd.image || hd.format > 37 || !(atex::FORMAT_FLAGS[hd.format] & 1)) return false;
    const size_t offset = le32(b.data() + 84) == 0x30315844u /* "DX10" */ ? 148 : 128;
    if (b.size() < offset + surface_bytes(hd.format, hd.width, hd.height)) return false;
    atex::Image im = atex::decode_surface(hd.format, b.data() + offset, b.size() - offset, hd.width, hd.height);
    out.width = im.width;
    out.height = im.height;
    out.px = std::move(im.rgba);
    return true;
}

} // namespace

Kind kind_of(std::string_view magic, std::string_view type) {
    if (type == "dds") return Kind::Dds;
    if (type == "riff" || type == "png") return Kind::Web;
    if (magic.size() == 4 && magic.substr(1) == "TEU") return Kind::Ui;
    return Kind::World;
}

std::string format_label(uint8_t format) {
    if (format == kFmtPng) return "PNG";
    if (format == kFmtWebp) return "WebP";
    if (format <= 37) return atex::format_name(format);
    return "?";
}

uint64_t Header::pack() const {
    if (!known) return 0;
    return (uint64_t{1} << 63) | (static_cast<uint64_t>(image) << 62) | (static_cast<uint64_t>(alpha) << 61) |
           (static_cast<uint64_t>(format) << 32) | (static_cast<uint64_t>(height) << 16) | width;
}

Header Header::unpack(uint64_t v) {
    Header h;
    if (!(v >> 63)) return h;
    h.known = true;
    h.image = (v >> 62) & 1;
    h.alpha = (v >> 61) & 1;
    h.format = static_cast<uint8_t>(v >> 32);
    h.height = static_cast<uint16_t>(v >> 16);
    h.width = static_cast<uint16_t>(v);
    return h;
}

Header read_header(const uint8_t* d, size_t n) {
    if (is_anet_texture(d, n)) {
        Header h;
        h.known = true;
        if (n < 12) return h;
        const int fmt = atex::fourcc_to_enum(d + 4);
        h.width = static_cast<uint16_t>(d[8] | (d[9] << 8));
        h.height = static_cast<uint16_t>(d[10] | (d[11] << 8));
        h.format = fmt < 0 ? kFmtUnknown : static_cast<uint8_t>(fmt);
        h.image = fmt >= 0 && h.width > 0 && h.height > 0;
        h.alpha = fmt >= 0 && format_alpha(fmt);
        return h;
    }
    if (n >= 4 && std::memcmp(d, "DDS ", 4) == 0) return dds_header(d, n);
    if (n >= 8 && std::memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return png_header(d, n);
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0) return webp_header(d, n);
    Header h;
    h.known = true;
    return h;
}

bool IdQuery::contains(uint32_t id) const {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), id,
                               [](uint32_t v, const std::pair<uint32_t, uint32_t>& r) { return v < r.first; });
    return it != ranges.begin() && id <= std::prev(it)->second;
}

bool parse_id_query(std::string_view text, IdQuery& out, std::string& error) {
    // "21900 - 22100" is one range: glue spaces around a dash before splitting.
    std::string s;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const bool sep = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (sep) {
            size_t j = i;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) ++j;
            const bool before_dash = j < text.size() && text[j] == '-';
            const bool after_dash = !s.empty() && s.back() == '-';
            if (!before_dash && !after_dash) s += ' ';
            i = j - 1;
        } else {
            s += (c == ',' || c == ';') ? ' ' : c;
        }
    }
    auto number = [](std::string_view t, uint32_t& v) {
        if (t.empty() || t.size() > 10) return false;
        uint64_t x = 0;
        for (char c : t) {
            if (c < '0' || c > '9') return false;
            x = x * 10 + static_cast<uint64_t>(c - '0');
        }
        if (x > 0xFFFFFFFFull) return false;
        v = static_cast<uint32_t>(x);
        return true;
    };
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    size_t p = 0;
    while (p < s.size()) {
        if (s[p] == ' ') { ++p; continue; }
        size_t e = s.find(' ', p);
        if (e == std::string::npos) e = s.size();
        const std::string_view tok(s.data() + p, e - p);
        const size_t dash = tok.find('-');
        uint32_t lo = 0, hi = 0;
        const bool ok = dash == std::string_view::npos
                            ? (number(tok, lo) && (hi = lo, true))
                            : number(tok.substr(0, dash), lo) && number(tok.substr(dash + 1), hi);
        if (!ok) {
            error = "not an id or a range: " + std::string(tok);
            return false;
        }
        if (lo > hi) std::swap(lo, hi);
        ranges.emplace_back(lo, hi);
        p = e;
    }
    std::sort(ranges.begin(), ranges.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto& r : ranges) {
        if (!merged.empty() && (merged.back().second == 0xFFFFFFFFu || r.first <= merged.back().second + 1))
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    out.ranges = std::move(merged);
    return true;
}

bool is_icon_size(int width, int height) {
    return width == height && (width == 16 || width == 32 || width == 64 || width == 128);
}

bool matches(const Filter& f, Kind kind, uint32_t base_id, const Header& h) {
    if (!f.kinds[static_cast<int>(kind)]) return false;
    if (h.known && !h.image) return false;
    if (!f.ids.empty() && !f.ids.contains(base_id)) return false;
    if (!f.needs_header()) return true;
    if (!h.known) return false;
    const int side = h.longest();
    if (f.min_side > 0 && side < f.min_side) return false;
    if (f.max_side > 0 && side > f.max_side) return false;
    if (f.exclude_icons && is_icon_size(h.width, h.height)) return false;
    if (f.alpha_only && !h.alpha) return false;
    return true;
}

bool decode_image(std::vector<uint8_t> bytes, int min_side, Rgba& out) {
    try {
        if (is_anet_texture(bytes.data(), bytes.size())) {
            bytes[0] = 'A';
            const atex::Texture t = atex::parse(bytes.data(), bytes.size());
            if (t.mips.empty()) return false;
            size_t level = 0;
            if (min_side > 0)
                while (level + 1 < t.mips.size() &&
                       std::max(t.mips[level + 1].width, t.mips[level + 1].height) >= min_side)
                    ++level;
            atex::Image im = atex::decode(t, static_cast<int>(level));
            out.width = im.width;
            out.height = im.height;
            out.px = std::move(im.rgba);
            return out.width > 0 && out.height > 0;
        }
        if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) return decode_dds(bytes, out);
        if (img::is_image(bytes.data(), bytes.size())) {
            std::optional<img::Image> im = img::decode(bytes.data(), bytes.size());
            if (!im) return false;
            out.width = static_cast<int>(im->width);
            out.height = static_cast<int>(im->height);
            out.px = std::move(im->rgba);
            return out.width > 0 && out.height > 0;
        }
    } catch (const std::exception&) {
    }
    return false;
}

Thumb make_thumb(const Rgba& img, int box) {
    Thumb t;
    if (img.width <= 0 || img.height <= 0 || box <= 0 ||
        img.px.size() < static_cast<size_t>(img.width) * img.height * 4)
        return t;
    const double scale = std::min(1.0, static_cast<double>(box) / std::max(img.width, img.height));
    t.width = std::max(1, static_cast<int>(img.width * scale + 0.5));
    t.height = std::max(1, static_cast<int>(img.height * scale + 0.5));
    t.bgra.resize(static_cast<size_t>(t.width) * t.height * 4);
    for (int y = 0; y < t.height; ++y) {
        const int y0 = y * img.height / t.height;
        const int y1 = std::max(y0 + 1, (y + 1) * img.height / t.height);
        for (int x = 0; x < t.width; ++x) {
            const int x0 = x * img.width / t.width;
            const int x1 = std::max(x0 + 1, (x + 1) * img.width / t.width);
            uint32_t acc[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const uint8_t* s = &img.px[(static_cast<size_t>(sy) * img.width + sx) * 4];
                    // Weight colour by alpha so transparent texels don't bleed their RGB.
                    acc[0] += s[0] * s[3];
                    acc[1] += s[1] * s[3];
                    acc[2] += s[2] * s[3];
                    acc[3] += s[3];
                }
            const uint32_t count = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            const uint32_t a = acc[3] / count;
            const uint8_t check = ((x / 8 + y / 8) & 1) ? 0x9C : 0x64;  // 8 px checkerboard
            uint8_t* d = &t.bgra[(static_cast<size_t>(y) * t.width + x) * 4];
            for (int c = 0; c < 3; ++c) {
                const uint32_t col = acc[3] ? acc[c] / acc[3] : 0;
                d[2 - c] = static_cast<uint8_t>((col * a + check * (255 - a)) / 255);
            }
            d[3] = 255;
        }
    }
    return t;
}

const Thumb* ThumbCache::get(uint32_t id) {
    auto it = map_.find(id);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);
    return &it->second->second;
}

void ThumbCache::put(uint32_t id, Thumb t) {
    if (auto it = map_.find(id); it != map_.end()) {
        bytes_ -= it->second->second.bytes();
        lru_.erase(it->second);
        map_.erase(it);
    }
    bytes_ += t.bytes();
    lru_.emplace_front(id, std::move(t));
    map_[id] = lru_.begin();
    while (bytes_ > cap_ && lru_.size() > 1) {
        bytes_ -= lru_.back().second.bytes();
        map_.erase(lru_.back().first);
        lru_.pop_back();
    }
}

void ThumbCache::clear() {
    lru_.clear();
    map_.clear();
    bytes_ = 0;
}

std::vector<uint8_t> read_prefix(std::ifstream& dat, const MftData& e, size_t want) {
    // Gw2.dat puts a CRC32C every 0x10000 raw bytes; stay below the first so the
    // prefix needs no unframing beyond the trailing one strip_crc32 assumes.
    constexpr size_t kPrefixRaw = 0x8000;
    if (e.size <= kPrefixRaw) {
        std::vector<uint8_t> all = read_whole(dat, e);
        if (all.size() > want) all.resize(want);
        return all;
    }
    std::vector<uint8_t> raw(kPrefixRaw + 4);  // + 4: the bytes strip_crc32 takes as a trailing CRC
    dat.clear();
    dat.seekg(static_cast<std::streamoff>(e.offset));
    dat.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (dat.gcount() != static_cast<std::streamsize>(raw.size())) throw std::runtime_error("short read");
    std::vector<uint8_t> stripped = cmp::strip_crc32(raw);
    if (e.compression_flag == 0) {
        if (stripped.size() > want) stripped.resize(want);
        return stripped;
    }
    if (stripped.size() < 8) throw std::runtime_error("entry too small for a Method0 header");
    const size_t usize = le32(stripped.data() + 4);
    return cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), std::min(usize, want));
}

std::vector<uint8_t> read_whole(std::ifstream& dat, const MftData& e) {
    if (e.size == 0) return {};
    std::vector<uint8_t> raw(e.size);
    dat.clear();
    dat.seekg(static_cast<std::streamoff>(e.offset));
    dat.read(reinterpret_cast<char*>(raw.data()), e.size);
    if (dat.gcount() != static_cast<std::streamsize>(e.size)) throw std::runtime_error("short read");
    std::vector<uint8_t> stripped = cmp::strip_crc32(raw);
    if (e.compression_flag == 0) return stripped;
    if (stripped.size() < 8) throw std::runtime_error("entry too small for a Method0 header");
    return cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), le32(stripped.data() + 4));
}

namespace {
constexpr char kHeaderMagic[4] = {'C', 'M', 'T', 'H'};
constexpr uint32_t kHeaderVersion = 1;
}

bool save_headers(const std::filesystem::path& path, const std::string& fingerprint,
                  const std::vector<std::pair<uint32_t, uint64_t>>& headers) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        f.write(kHeaderMagic, 4);
        u32(kHeaderVersion);
        u32(static_cast<uint32_t>(fingerprint.size()));
        f.write(fingerprint.data(), static_cast<std::streamsize>(fingerprint.size()));
        u32(static_cast<uint32_t>(headers.size()));
        for (const auto& [id, packed] : headers) {
            u32(id);
            f.write(reinterpret_cast<const char*>(&packed), 8);
        }
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool load_headers(const std::filesystem::path& path, const std::string& fingerprint,
                  std::vector<std::pair<uint32_t, uint64_t>>& headers) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4];
    uint32_t version = 0, fp_len = 0, count = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&fp_len), 4);
    if (!f || std::memcmp(magic, kHeaderMagic, 4) != 0 || version != kHeaderVersion || fp_len > 4096) return false;
    std::string fp(fp_len, '\0');
    f.read(fp.data(), fp_len);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || fp != fingerprint || count > (1u << 24)) return false;
    std::vector<std::pair<uint32_t, uint64_t>> out(count);
    for (auto& [id, packed] : out) {
        f.read(reinterpret_cast<char*>(&id), 4);
        f.read(reinterpret_cast<char*>(&packed), 8);
    }
    if (!f) return false;
    headers = std::move(out);
    return true;
}

} // namespace castlemist::ui::gallery
