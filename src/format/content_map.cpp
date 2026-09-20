#include "castlemist/format/content_map.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>

#include "castlemist/native/cmp_decompress_method0.hpp"

namespace castlemist::cmap {
namespace {

// key = (contentType << 32) | numericId -> all in-object asset fileIds (capped).
std::unordered_map<uint64_t, std::vector<uint32_t>> g_map;
const std::vector<uint32_t> g_empty;
const std::string g_empty_name;

// Same key -> the baseId of the cntc pack the object was found in. Session-only
// (see content_map.h's content_base_id() docstring): not part of the disk cache.
std::unordered_map<uint64_t, uint32_t> g_base_id;

// fileId -> the codename slug of a content object that references it as an asset
// (see is_content_slug() below). Lets a browser show a real name next to an
// otherwise-bare fileId. Session-only, like g_base_id: rebuilt by build(), not
// part of the on-disk cache, and last-object-wins when more than one content
// object shares an asset (icons in particular are widely reused).
std::unordered_map<uint32_t, std::string> g_fileid_name;

constexpr size_t kMaxRefsPerObject = 16;

// True for an ArenaNet content identifier slug, e.g. "vl8Av.4gynM": two short
// base64-ish groups joined by a dot. Mirrors content_types.h's is_content_slug()
// (extract layer) -- duplicated rather than shared because `format` sits below
// `extract` in the dependency graph (see docs/architecture.md) and this predicate
// is cheap enough that copying it beats introducing a layering exception.
bool looks_like_content_slug(const std::string& s) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot < 3 || dot > 8) return false;
    if (s.size() - dot - 1 < 3 || s.size() - dot - 1 > 8) return false;
    if (s.find(' ') != std::string::npos) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == dot) continue;
        unsigned char c = static_cast<unsigned char>(s[i]);
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

inline uint64_t key(uint32_t type, uint32_t id) { return (static_cast<uint64_t>(type) << 32) | id; }

// Decompress one MFT entry to its raw file bytes (mirrors decompress_by_index in
// entry_extractor.cpp). Own file handle inside read_entry_bytes -> thread-safe.
std::vector<uint8_t> decompress(const std::string& dat_path, const MftData& e) {
    std::vector<uint8_t> raw = read_entry_bytes(dat_path, e);
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    if (e.compression_flag == 0) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | (static_cast<uint32_t>(s[7]) << 24);
    return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
}

// Parse one decompressed cntc packfile, adding every (contentType,id)->fileId it
// finds. Layout (validated): PF "cntc" -> chunk "Main"; base = mainPos+16; then
// 11 arrays {u32 count, i64 self-rel ptr} at base+4+i*12. Array 3 = indexEntries
// (16 B each: object offset at +4), array 6 = fileIndices (u32 reloc into content),
// array 7 = stringIndices (u32 reloc into content, marking a dword that is an
// index into array 9), array 9 = strings (codename UTF-16 string table, each
// entry an 8-byte self-relative pointer), array 10 = content bytes. Each object:
// contentType@+16, id@+20, primary asset fileId at the fileIndices reloc ==
// object+64 (else the first reloc in the object). Also records the object's own
// codename slug (content_store.cpp's fuller parser calls the same field "name")
// against every asset fileId it references, so a browser can show a real name
// next to a bare fileId -- see g_fileid_name / name_for_fileid().
void parse_cntc(const std::vector<uint8_t>& d, uint32_t base_id) {
    const size_t n = d.size();
    if (n < 16 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "cntc", 4) != 0) return;
    auto u16 = [&](size_t p) -> uint32_t { return (p + 2 <= n) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto u32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= n) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    auto i64 = [&](size_t p) -> int64_t { if (p + 8 > n) return 0; int64_t v; std::memcpy(&v, d.data() + p, 8); return v; };

    size_t pos = u16(6);
    while (pos + 8 <= n && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        size_t next = pos + 8 + u32(pos + 4);
        if (next <= pos) return;
        pos = next;
    }
    if (pos + 16 > n) return;
    size_t base = pos + 16;
    auto arr = [&](int i, size_t& off) -> uint32_t {
        size_t p = base + 4 + (size_t)i * 12;
        off = (size_t)((p + 4) + i64(p + 4));
        return u32(p);
    };
    size_t ieOff, fiOff, siOff, stOff, cOff;
    uint32_t ieCnt = arr(3, ieOff);
    uint32_t fiCnt = arr(6, fiOff);
    uint32_t siCnt = arr(7, siOff);
    uint32_t stCnt = arr(9, stOff);
    uint32_t cCnt = arr(10, cOff);
    if (ieCnt == 0 || cCnt == 0 || cOff >= n) return;

    // Object offsets (sorted, unique) so we know each object's extent.
    std::vector<uint32_t> offs;
    offs.reserve(ieCnt);
    for (uint32_t i = 0; i < ieCnt; ++i) offs.push_back(u32(ieOff + (size_t)i * 16 + 4));
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());

    // fileIndices relocs, sorted for range queries.
    std::vector<uint32_t> fi;
    fi.reserve(fiCnt);
    for (uint32_t i = 0; i < fiCnt; ++i) fi.push_back(u32(fiOff + (size_t)i * 4));
    std::sort(fi.begin(), fi.end());

    // stringIndices relocs, same idea as fileIndices but each marks a dword that
    // is an index into the strings table rather than a raw fileId.
    std::vector<uint32_t> si;
    si.reserve(siCnt);
    for (uint32_t i = 0; i < siCnt; ++i) si.push_back(u32(siOff + (size_t)i * 4));
    std::sort(si.begin(), si.end());

    // strings[idx] = an 8-byte self-relative pointer to a UTF-16 codename.
    auto codename = [&](uint32_t idx) -> std::string {
        if (idx >= stCnt) return {};
        size_t ep = stOff + (size_t)idx * 8;
        int64_t rel = i64(ep);
        if (rel == 0) return {};
        size_t sp = (size_t)(ep + rel);
        std::string s;
        for (size_t q = sp; q + 2 <= n && s.size() < 96; q += 2) {
            uint32_t ch = d[q] | (d[q + 1] << 8);
            if (!ch) break;
            s += (ch < 0x80) ? static_cast<char>(ch) : '?';
        }
        return s;
    };

    for (size_t k = 0; k < offs.size(); ++k) {
        uint32_t o = offs[k];
        uint32_t nextOff = (k + 1 < offs.size()) ? offs[k + 1] : cCnt;
        if (cOff + o + 24 > n) continue;
        uint32_t type = u32(cOff + o + 16);
        uint32_t id = u32(cOff + o + 20);

        // Collect every fileIndices reloc inside this object [o, nextOff) as an
        // asset fileId, in object order. The reloc at +64 (the primary/model)
        // sorts first naturally; skins list icon textures + model + variants.
        std::vector<uint32_t> refs;
        for (auto f = std::lower_bound(fi.begin(), fi.end(), o);
             f != fi.end() && *f < nextOff && refs.size() < kMaxRefsPerObject; ++f) {
            uint32_t v = u32(cOff + *f);
            if (v > 0 && v < 0xFFFFFF) refs.push_back(v);
        }

        // The object's own codename slug, if it has one -- same shape check as
        // content_store.cpp's is_content_slug(), first match wins (a record's
        // slug is one specific string among possibly several readable labels).
        std::string name;
        for (auto s = std::lower_bound(si.begin(), si.end(), o); s != si.end() && *s < nextOff; ++s) {
            std::string str = codename(u32(cOff + *s));
            if (!str.empty() && looks_like_content_slug(str)) { name = std::move(str); break; }
        }
        if (!name.empty())
            for (uint32_t v : refs) g_fileid_name[v] = name;
        if (!refs.empty()) {
            uint64_t k = key(type, id);
            g_map[k] = std::move(refs);
            g_base_id[k] = base_id;
        }
    }
}

} // namespace

bool built() { return !g_map.empty(); }
size_t size() { return g_map.size(); }
void clear() { g_map.clear(); g_base_id.clear(); g_fileid_name.clear(); }

size_t build(const std::string& dat_path, const std::vector<MftData>& cntc_entries,
             const std::function<void(size_t, size_t)>& progress) {
    size_t total = cntc_entries.size();
    for (size_t i = 0; i < total; ++i) {
        std::vector<uint8_t> bytes = decompress(dat_path, cntc_entries[i]);
        // baseId == mft_index + 1 (the convention this whole codebase uses --
        // see e.g. entry_extractor.h's extract_entry_indexed()).
        uint32_t base_id = static_cast<uint32_t>(cntc_entries[i].original_index) + 1;
        if (!bytes.empty()) parse_cntc(bytes, base_id);
        if (progress) progress(i + 1, total);
    }
    return g_map.size();
}

uint32_t content_type_for_header(uint8_t header) {
    switch (header) {
    case 0x02: return CONTENT_TYPE_ITEM;    // item link
    case 0x0A: return CONTENT_TYPE_SKIN;    // wardrobe skin link
    case 0x0B: return CONTENT_TYPE_OUTFIT;  // outfit link
    default: return 0;
    }
}

const std::vector<uint32_t>& resolve_all(uint32_t content_type, uint32_t id) {
    auto it = g_map.find(key(content_type, id));
    return it == g_map.end() ? g_empty : it->second;
}

uint32_t resolve(uint32_t content_type, uint32_t id) {
    const std::vector<uint32_t>& v = resolve_all(content_type, id);
    return v.empty() ? 0 : v.front();
}

uint32_t content_base_id(uint32_t content_type, uint32_t id) {
    auto it = g_base_id.find(key(content_type, id));
    return it == g_base_id.end() ? 0 : it->second;
}

const std::string& name_for_fileid(uint32_t file_id) {
    auto it = g_fileid_name.find(file_id);
    return it == g_fileid_name.end() ? g_empty_name : it->second;
}

// ---- binary cache: "GC2N" magic, u32 count, then count * {u64 key, u8 n, n*u32 fileId}.
bool save(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint32_t magic = 0x4E324347;  // 'GC2N'
    uint32_t count = static_cast<uint32_t>(g_map.size());
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&count, 4, 1, f);
    for (const auto& [k, v] : g_map) {
        uint8_t n = static_cast<uint8_t>(std::min<size_t>(v.size(), 255));
        std::fwrite(&k, 8, 1, f);
        std::fwrite(&n, 1, 1, f);
        std::fwrite(v.data(), 4, n, f);
    }
    std::fclose(f);
    return true;
}

bool load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || magic != 0x4E324347) { std::fclose(f); return false; }
    if (std::fread(&count, 4, 1, f) != 1) { std::fclose(f); return false; }
    g_map.reserve(count + 16);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t k; uint8_t n;
        if (std::fread(&k, 8, 1, f) != 1 || std::fread(&n, 1, 1, f) != 1) break;
        std::vector<uint32_t> v(n);
        if (n && std::fread(v.data(), 4, n, f) != n) break;
        g_map[k] = std::move(v);
    }
    std::fclose(f);
    return !g_map.empty();
}

} // namespace castlemist::cmap
