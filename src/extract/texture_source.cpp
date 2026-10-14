/// @file
/// @brief Reading and decoding model textures out of the archive.

#include "internal.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <span>
#include <string>

#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2_atex.hpp"

#include "castlemist/core/packfile.h"

namespace castlemist::extract {

// Texture resolution preference. GW2 stores many textures as a full/reduced pair
// at consecutive MFT indices (baseId B = full resolution, B-1 = the exact-half
// version, same format). A material may reference EITHER member of the pair, so the
// "full" texture is sometimes at the resolved index and sometimes one entry above
// it. When true (default) we load the full-resolution member; when false the reduced
// one (faster / less VRAM). Read on the bg thread; set from the UI.
static std::atomic<bool> g_tex_full_res{true};

// Decompress one MFT entry (by 0-based array index) into its raw ATEX/CTEX bytes.
static bool read_mft_atex_bytes(Gw2Dat& dat, size_t i0, std::vector<uint8_t>& bytes) {
    if (i0 >= dat.mft_data_list.size()) return false;
    const MftData& e = dat.mft_data_list[i0];
    try {
        std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
        std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
        if (e.compression_flag == 0) {
            bytes = std::move(stripped);
        } else {
            if (stripped.size() < 8) return false;
            uint32_t usz = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
            bytes = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usz);
        }
        if (bytes.size() >= 4 && bytes[0] == 'C') bytes[0] = 'A'; // CTEX -> ATEX alias
        return bytes.size() >= 12;
    } catch (const std::exception&) {
        return false;
    }
}

bool parse_atex_header(const uint8_t* data, size_t n, AtexHeader& out) {
    if (n < 12) return false;
    uint8_t head[8];
    std::memcpy(head, data, 8);
    if (head[0] == 'C') head[0] = 'A';   // CTEX -> ATEX alias
    static const char* MAG[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET"};
    bool ok = false;
    for (auto* m : MAG) if (!std::memcmp(head, m, 4)) ok = true;
    if (!ok) return false;
    const int fe = castlemist::atex::fourcc_to_enum(head + 4);
    if (fe < 0) return false;
    out.fmt = castlemist::atex::format_name(fe);
    out.width = data[8] | (data[9] << 8);
    out.height = data[10] | (data[11] << 8);
    out.firstMipSize = 0;
    if (n >= 16) std::memcpy(&out.firstMipSize, data + 12, 4);
    return out.width > 0 && out.height > 0;
}

int estimate_mip_count(const AtexHeader& h, size_t totalSize) {
    if (h.firstMipSize >= 8 && static_cast<size_t>(12) + h.firstMipSize + 8 > totalSize) return 1;
    int n = 1;
    for (int m = std::max(h.width, h.height); m > 1; m >>= 1) ++n;
    return n;
}

// Header of an entry's ATEX without reading or inflating the whole thing: the
// first 0x10000 raw bytes only, and for compressed entries just the first few
// output bytes of the bitstream (decompress_method0 stops at output_size). Any
// failure falls back to the full read, so the answer never differs.
static bool peek_mft_header(Gw2Dat& dat, size_t i0, AtexHeader& h, size_t* totalSize = nullptr) {
    if (i0 >= dat.mft_data_list.size()) return false;
    const MftData& e = dat.mft_data_list[i0];
    try {
        if (e.size > 8) {
            const size_t want = std::min<size_t>(e.size, 0x10000);
            std::vector<uint8_t> raw(want);
            std::ifstream file(dat.file_info.file_path, std::ios::binary);
            if (file.is_open()) {
                file.seekg(static_cast<std::streamoff>(e.offset));
                file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(want));
                if (file.gcount() == static_cast<std::streamsize>(want)) {
                    std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
                    std::vector<uint8_t> head;
                    size_t total = 0;
                    if (e.compression_flag == 0) {
                        head = std::move(stripped);
                        total = e.size - (e.size / 0x10000 + 1) * 4;
                    } else if (stripped.size() >= 8) {
                        total = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | (static_cast<size_t>(stripped[7]) << 24);
                        head = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8),
                                                                    std::min<size_t>(total, 16));
                    }
                    if (parse_atex_header(head.data(), head.size(), h)) {
                        if (totalSize) *totalSize = total;
                        return true;
                    }
                }
            }
        }
    } catch (const std::exception&) { /* fall through to the full read */ }
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    if (!parse_atex_header(bytes.data(), bytes.size(), h)) return false;
    if (totalSize) *totalSize = bytes.size();
    return true;
}

// Peek an entry's ATEX dimensions + format from the header only (no pixel decode).
static bool peek_mft_atex(Gw2Dat& dat, size_t i0, int& w, int& h, std::string& fmt) {
    AtexHeader hd;
    if (!peek_mft_header(dat, i0, hd)) return false;
    w = hd.width; h = hd.height; fmt = hd.fmt;
    return true;
}

// Given the entry a fileId resolved to (i0), find the full/reduced member of its
// pair: the same-format texture at the next / previous fileId with exactly
// double / half dimensions (reduced = F, full = F+1). Returns i0 unchanged if there
// is no paired sibling.
// The two entries hold consecutive fileIds (reduced F, full F+1). A same-format,
// double-size neighbour alone is not enough: the MFT order can put an unrelated
// texture next to it (sylvari male hair mask 151249 sits beside 151234, a different
// 1024x1024 mask, while its real full copy is 151250 elsewhere in the table).
static bool consecutive_files(Gw2Dat& dat, size_t lower, size_t upper) {
    const std::vector<uint32_t> a = get_by_file_id(dat, static_cast<uint32_t>(lower + 1));
    const std::vector<uint32_t> b = get_by_file_id(dat, static_cast<uint32_t>(upper + 1));
    for (uint32_t x : a)
        for (uint32_t y : b)
            if (y == x + 1) return true;
    return false;
}

static size_t find_entry_of_file(Gw2Dat& dat, uint32_t file_id) {
    const uint32_t base = get_by_base_id(dat, file_id);
    return base == 0 ? SIZE_MAX : static_cast<size_t>(base - 1);
}

static size_t resolve_res_index(Gw2Dat& dat, size_t i0, bool wantFull) {
    int w0, h0; std::string f0;
    if (!peek_mft_atex(dat, i0, w0, h0, f0)) return i0;
    auto is_pair = [&](size_t lo, size_t hi) {  // lo reduced, hi full
        int wl, hl, wh, hh; std::string fl, fh;
        return peek_mft_atex(dat, lo, wl, hl, fl) && peek_mft_atex(dat, hi, wh, hh, fh) && fl == fh &&
               wh == 2 * wl && hh == 2 * hl && consecutive_files(dat, lo, hi);
    };
    // The sibling by fileId: F+1 is the full copy of F, F-1 the reduced one of F.
    for (uint32_t fid : get_by_file_id(dat, static_cast<uint32_t>(i0 + 1))) {
        const size_t up = find_entry_of_file(dat, fid + 1);
        if (up != SIZE_MAX && up < dat.mft_data_list.size() && is_pair(i0, up)) return wantFull ? up : i0;
        if (fid > 1) {
            const size_t down = find_entry_of_file(dat, fid - 1);
            if (down != SIZE_MAX && down < dat.mft_data_list.size() && is_pair(down, i0)) return wantFull ? i0 : down;
        }
    }
    return i0;
}

// Does this texture's alpha channel act as GW2's alpha-test coverage mask?
//
// The game's opaque material (and normal-prepass) pixel shaders discard where the
// diffuse alpha is below 0.25 -- they spell it `saturate(2a) < 0.5`. Applying that
// blindly is unsafe: plenty of opaque materials ship a diffuse whose alpha is
// uniformly 0 because the shader variant they use never reads it, and cutting those
// out would erase the mesh. So require the alpha channel to genuinely partition the
// image: a real-but-minority set of texels below the threshold, and a solid majority
// above it. Fully-opaque maps (the common case) return false and cost nothing.
static bool compute_alpha_cutout(const std::vector<uint8_t>& rgba) {
    if (rgba.size() < 4) return false;
    size_t n = rgba.size() / 4;
    size_t step = n > 65536 ? n / 65536 : 1;   // cap the scan at ~64k samples
    size_t below = 0, sampled = 0;
    for (size_t p = 0; p < n; p += step) {
        if (rgba[p * 4 + 3] < 64) below++;      // 64/255 ~= the 0.25 threshold
        sampled++;
    }
    if (sampled == 0) return false;
    double frac = static_cast<double>(below) / static_cast<double>(sampled);
    return frac > 0.005 && frac < 0.95;
}

// Which channels of the decoded RGBA actually carry information.
//
// The container format is only an upper bound: every BCn decode lands in an
// RGBA8888 buffer, and plenty of DXT5 diffuses ship a uniformly-opaque alpha, so
// "DXT5" alone would report RGBA for what is really an RGB texture. Measure it
// from the pixels instead, with one format-driven exception: a BC5/3Dc normal map
// stores only two channels and the decoder *synthesises* blue, so reporting RGB
// for it would be a lie about the file.
//
// Alpha counts as a channel only when it varies. A constant alpha -- all 255
// (opaque) or all 0 (the "shader never reads it" case that `compute_alpha_cutout`
// also guards against) -- carries nothing.
static std::string compute_channels(const std::vector<uint8_t>& rgba, bool isNormal) {
    if (rgba.size() < 4) return "";
    size_t n = rgba.size() / 4;
    size_t step = n > 65536 ? n / 65536 : 1;   // cap the scan at ~64k samples
    bool colored = false;
    uint8_t aMin = 255, aMax = 0;
    for (size_t p = 0; p < n; p += step) {
        const uint8_t* t = &rgba[p * 4];
        if (t[0] != t[1] || t[1] != t[2]) colored = true;
        aMin = std::min(aMin, t[3]);
        aMax = std::max(aMax, t[3]);
    }
    std::string ch = isNormal ? "RG" : (colored ? "RGB" : "R");
    if (aMin != aMax) ch += "A";
    return ch;
}

// Decode the ATEX at a 0-based MFT index to RGBA.
static bool decode_mft_atex(Gw2Dat& dat, size_t i0, ModelTextureCPU& out) {
    std::vector<uint8_t> bytes;
    if (!read_mft_atex_bytes(dat, i0, bytes)) return false;
    try {
        castlemist::atex::Texture t = castlemist::atex::parse(bytes.data(), bytes.size());
        castlemist::atex::Image im = castlemist::atex::decode(t, 0);
        if (im.width <= 0 || im.height <= 0 || im.rgba.empty()) return false;
        out.width = im.width;
        out.height = im.height;
        out.fmt = t.fmt_name;
        out.mipCount = static_cast<int>(t.mips.size());
        out.baseId = static_cast<uint32_t>(i0 + 1);   // MFT array index -> baseId
        out.rgba = std::move(im.rgba);
        out.isNormal = is_normal_format(t.fmt_name);
        out.channels = compute_channels(out.rgba, out.isNormal);
        out.hasCutout = !out.isNormal && compute_alpha_cutout(out.rgba);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Decompress one MFT entry addressed by fileId, then decode its ATEX to RGBA,
// choosing the full- or reduced-resolution member of its pair per g_tex_full_res.
// Returns false if the fileId isn't found or isn't a decodable texture.
bool decode_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out) {
    return decode_texture_by_fileid_res(dat, fileId, out, g_tex_full_res.load());
}

bool decode_texture_exact(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return false;
    if (!decode_mft_atex(dat, base - 1, out)) return false;
    out.fileId = fileId;
    return true;
}

bool decode_texture_full_res(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out, int* exact_width) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return false;
    if (exact_width) {
        int w = 0, h = 0;
        std::string fmt;
        *exact_width = peek_mft_atex(dat, base - 1, w, h, fmt) ? w : 0;
    }
    if (!decode_mft_atex(dat, resolve_res_index(dat, base - 1, /*wantFull=*/true), out)) return false;
    out.fileId = fileId;
    return true;
}

bool peek_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return false;
    AtexHeader hd;
    size_t total = 0;
    if (!peek_mft_header(dat, base - 1, hd, &total)) return false;
    out.fileId = fileId;
    out.baseId = base;
    out.width = hd.width;
    out.height = hd.height;
    out.fmt = hd.fmt;
    out.mipCount = estimate_mip_count(hd, total);
    out.isNormal = is_normal_format(hd.fmt);
    return true;
}

bool decode_texture_by_fileid_res(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out, bool want_full) {
    uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) {
        return false;
    }
    size_t i0 = resolve_res_index(dat, base - 1, want_full);
    if (!decode_mft_atex(dat, i0, out)) return false;
    out.fileId = fileId;
    return true;
}

} // namespace castlemist::extract

// ---- public API (declared in castlemist/extract/entry_extractor.h) ----

using namespace castlemist::extract;

// Public texture-resolution API (g_tex_full_res lives in the anonymous namespace
// above but is reachable throughout this translation unit).
void set_texture_full_res(bool full) { g_tex_full_res.store(full); }
bool texture_full_res() { return g_tex_full_res.load(); }

size_t texture_entry(Gw2Dat& dat, uint32_t file_id, bool full) {
    const uint32_t base = get_by_base_id(dat, file_id);
    if (base == 0 || base - 1 >= dat.mft_data_list.size()) return SIZE_MAX;
    return resolve_res_index(dat, base - 1, full);
}
