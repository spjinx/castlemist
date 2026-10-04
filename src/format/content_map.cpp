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

// The datastore's one shared fileRefs table (array 2, decoded to fileIds). Every
// fileIndices slot in every pack holds an INDEX into this table, not a fileId --
// measured against a live Gw2.dat: through the table, item+64 / skin+88 are 100%
// textures and skin+48 100% MODLs; read literally they're a random mix. Only one
// pack carries it, and it may come after the packs that index it, so refs are
// collected raw and translated once every pack has been parsed (finalize()).
std::vector<uint32_t> g_file_refs;

// Objects parsed but not yet translated through g_file_refs.
struct PendingObject {
    uint64_t key;
    uint32_t base_id;
    std::vector<uint32_t> ref_indices;
    std::string name;
};
std::vector<PendingObject> g_pending;

// item dataId -> the appearance content it grants, in field order (see
// item_links()). Part of the disk cache.
std::unordered_map<uint32_t, std::vector<ContentLink>> g_item_links;
std::unordered_map<uint32_t, uint64_t> g_skin_tokens;  // skin dataId -> appearance token (+200)
constexpr size_t kMaxLinksPerItem = 64;

// Object graph behind item_links(). Items and containers point at other objects
// through localOffsets (the dword is an object offset in the same pack) or
// externalOffsets {reloc, targetFileIndex} (the dword is an object offset in the
// pack at targetFileIndex, counted in fileId order from the pack that carries
// fileRefs -- the cntc packs' fileIds are contiguous). Pointers always land on an
// object's first byte. Measured against a live Gw2.dat + the GW2 API:
//   Astralaria 76158         +176 external -> skin 6506
//   Claw of the Khan-Ur 87109 +176 local   -> skin 8051
//   Dark Monarch Skyscale Skin 93703 +264  -> mount skin 292
//   Holographic Dragon Helm 91359 +248     -> container -> items 91284/91357/91273
//                                             -> skins 8826/8817/8829
// Packs are identified by parse ordinal; edges resolve in finalize_links().
struct ObjRef {
    uint32_t type;
    uint32_t id;  // dataId (meaningless for containers)
};
struct Edge {
    uint32_t src_pack, src_off;
    bool external;
    uint32_t tfi;  // targetFileIndex (external only)
    uint32_t target_off;
};
std::unordered_map<uint64_t, ObjRef> g_obj_at;  // key(pack ordinal, offset)
std::vector<Edge> g_edges;
std::vector<uint32_t> g_pack_file_ids;     // by pack ordinal; 0 = unknown
uint32_t g_anchor_pack = UINT32_MAX;       // ordinal of the pack carrying fileRefs

bool is_appearance(uint32_t type) {
    return type == CONTENT_TYPE_SKIN || type == CONTENT_TYPE_MOUNT_SKIN || type == CONTENT_TYPE_OUTFIT;
}
bool is_graph_node(uint32_t type) {
    return type == CONTENT_TYPE_ITEM || type == CONTENT_TYPE_CONTAINER || is_appearance(type);
}

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

// Parse one decompressed cntc packfile, queueing every (contentType,dataId) object
// with its raw fileRefs indices (see g_file_refs). Layout (validated): PF "cntc" ->
// chunk "Main"; base = mainPos+16; then 11 arrays {u32 count, i64 self-rel ptr} at
// base+4+i*12. Array 2 = fileRefs (8-byte self-rel ptr -> compressed fileId pair),
// array 3 = indexEntries (16 B each: object offset at +4), array 6 = fileIndices
// (u32 reloc into content, marking a dword that indexes fileRefs),
// array 7 = stringIndices (u32 reloc into content, marking a dword that is an
// index into array 9), array 9 = strings (codename UTF-16 string table, each
// entry an 8-byte self-relative pointer), array 10 = content bytes. Each object:
// contentType@+16, dataId@+40 -- the API / chat-link id (+20 is an internal uid
// that only coincidentally overlaps API ids; verified against the GW2 API for
// items, skins and outfits). Also records the object's own
// codename slug (content_store.cpp's fuller parser calls the same field "name")
// against every asset fileId it references, so a browser can show a real name
// next to a bare fileId -- see g_fileid_name / name_for_fileid(). Array 5 =
// externalOffsets ({u32 reloc, u32 targetFileIndex}) supplies item->skin links.
void parse_cntc(const std::vector<uint8_t>& d, uint32_t base_id, uint32_t file_id) {
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
    size_t frOff, ieOff, loOff, eoOff, fiOff, siOff, stOff, cOff;
    uint32_t frCnt = arr(2, frOff);
    uint32_t ieCnt = arr(3, ieOff);
    uint32_t loCnt = arr(4, loOff);
    uint32_t eoCnt = arr(5, eoOff);
    uint32_t fiCnt = arr(6, fiOff);
    uint32_t siCnt = arr(7, siOff);
    uint32_t stCnt = arr(9, stOff);
    uint32_t cCnt = arr(10, cOff);
    if (frCnt != 0 && g_file_refs.empty()) {
        g_file_refs.reserve(frCnt);
        for (uint32_t i = 0; i < frCnt; ++i) {
            size_t p = frOff + (size_t)i * 8;
            int64_t rel = i64(p);
            size_t q = (size_t)(p + rel);
            uint32_t lo = u16(q), hi = u16(q + 2);
            // Same biased two-u16 encoding as cschema::decode_fileref_pair().
            bool unset = rel == 0 || lo < 0x100 || hi < 0x100;
            g_file_refs.push_back(unset ? 0u : 0xff00u * (hi - 0x100u) + (lo - 0x100u) + 1u);
        }
    }
    const uint32_t pack = static_cast<uint32_t>(g_pack_file_ids.size());
    g_pack_file_ids.push_back(file_id);
    if (frCnt != 0 && g_anchor_pack == UINT32_MAX) g_anchor_pack = pack;
    if (ieCnt == 0 || cCnt == 0 || cOff >= n) return;

    // externalOffsets as {reloc, targetFileIndex}, sorted by reloc.
    std::vector<std::pair<uint32_t, uint32_t>> eo;
    eo.reserve(eoCnt);
    for (uint32_t i = 0; i < eoCnt; ++i) eo.emplace_back(u32(eoOff + (size_t)i * 8), u32(eoOff + (size_t)i * 8 + 4));
    std::sort(eo.begin(), eo.end());

    // localOffsets relocs (in-pack pointers), sorted for range queries.
    std::vector<uint32_t> lo;
    lo.reserve(loCnt);
    for (uint32_t i = 0; i < loCnt; ++i) lo.push_back(u32(loOff + (size_t)i * 4));
    std::sort(lo.begin(), lo.end());

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
        if (cOff + o + 44 > n) continue;
        uint32_t type = u32(cOff + o + 16);
        uint32_t id = u32(cOff + o + 40);

        if (is_graph_node(type)) g_obj_at[key(pack, o)] = {type, id};
        if (type == CONTENT_TYPE_ITEM || type == CONTENT_TYPE_CONTAINER) {
            // Every pointer out of this object, local and external, in field order.
            std::vector<std::pair<uint32_t, Edge>> out;  // {reloc, edge}
            for (auto r = std::lower_bound(lo.begin(), lo.end(), o); r != lo.end() && *r < nextOff; ++r)
                out.push_back({*r, {pack, o, false, 0, u32(cOff + *r)}});
            for (auto e = std::lower_bound(eo.begin(), eo.end(), std::make_pair(o, 0u));
                 e != eo.end() && e->first < nextOff; ++e)
                out.push_back({e->first, {pack, o, true, e->second, u32(cOff + e->first)}});
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [reloc, edge] : out) g_edges.push_back(edge);
        }

        if (type == CONTENT_TYPE_SKIN && o + 208 <= nextOff && cOff + o + 208 <= n) {
            uint64_t token = u32(cOff + o + 200) | (static_cast<uint64_t>(u32(cOff + o + 204)) << 32);
            if (token) g_skin_tokens[id] = token;
        }

        // Collect every fileIndices reloc inside this object [o, nextOff), in
        // object order: item +64 = icon; skin +48 = model, +88 = icon, then
        // variants. These are fileRefs indices (0 is a valid one).
        std::vector<uint32_t> refs;
        for (auto f = std::lower_bound(fi.begin(), fi.end(), o);
             f != fi.end() && *f < nextOff && refs.size() < kMaxRefsPerObject; ++f) {
            refs.push_back(u32(cOff + *f));
        }

        // The object's own codename slug, if it has one -- same shape check as
        // content_store.cpp's is_content_slug(), first match wins (a record's
        // slug is one specific string among possibly several readable labels).
        std::string name;
        for (auto s = std::lower_bound(si.begin(), si.end(), o); s != si.end() && *s < nextOff; ++s) {
            std::string str = codename(u32(cOff + *s));
            if (!str.empty() && looks_like_content_slug(str)) { name = std::move(str); break; }
        }
        if (!refs.empty()) g_pending.push_back({key(type, id), base_id, std::move(refs), std::move(name)});
    }
}

// Resolve every queued pointer, then derive each item's links: appearance
// objects it points at directly, then (one level) those of the items inside any
// container it points at.
void finalize_links() {
    if (g_edges.empty()) return;

    // Pack ordinals in fileId order, for external targets. Needs the anchor's
    // fileId; without fileIds only local pointers resolve.
    std::vector<std::pair<uint32_t, uint32_t>> by_fid;  // {fileId, ordinal}
    for (uint32_t p = 0; p < g_pack_file_ids.size(); ++p)
        if (g_pack_file_ids[p]) by_fid.push_back({g_pack_file_ids[p], p});
    std::sort(by_fid.begin(), by_fid.end());
    size_t anchor_rank = SIZE_MAX;
    if (g_anchor_pack != UINT32_MAX && g_pack_file_ids[g_anchor_pack])
        anchor_rank = std::lower_bound(by_fid.begin(), by_fid.end(),
                                       std::make_pair(g_pack_file_ids[g_anchor_pack], 0u)) - by_fid.begin();

    std::unordered_map<uint64_t, std::vector<uint64_t>> adj;  // source object -> targets, field order
    for (const Edge& e : g_edges) {
        uint32_t tpack = e.src_pack;
        if (e.external) {
            if (anchor_rank == SIZE_MAX || anchor_rank + e.tfi >= by_fid.size()) continue;
            tpack = by_fid[anchor_rank + e.tfi].second;
        }
        uint64_t t = key(tpack, e.target_off);
        if (g_obj_at.count(t)) adj[key(e.src_pack, e.src_off)].push_back(t);
    }
    g_edges.clear();

    auto add = [](std::vector<ContentLink>& out, const ObjRef& r, uint32_t via) {
        for (const ContentLink& l : out)
            if (l.type == r.type && l.id == r.id) return;
        if (out.size() < kMaxLinksPerItem) out.push_back({r.type, r.id, via});
    };
    for (const auto& [src, targets] : adj) {
        const ObjRef& s = g_obj_at[src];
        if (s.type != CONTENT_TYPE_ITEM) continue;
        std::vector<ContentLink> links;
        for (uint64_t t : targets) {
            const ObjRef& r = g_obj_at[t];
            if (is_appearance(r.type)) add(links, r, 0);
            if (r.type != CONTENT_TYPE_CONTAINER) continue;
            auto c = adj.find(t);
            if (c == adj.end()) continue;
            for (uint64_t ci : c->second) {
                const ObjRef& item = g_obj_at[ci];
                if (item.type != CONTENT_TYPE_ITEM) continue;
                auto it = adj.find(ci);
                if (it == adj.end()) continue;
                for (uint64_t a : it->second)
                    if (is_appearance(g_obj_at[a].type)) add(links, g_obj_at[a], item.id);
            }
        }
        if (!links.empty()) g_item_links[s.id] = std::move(links);
    }
}

// Translate every queued object's fileRefs indices to fileIds and publish them.
// Objects stay queued if the fileRefs pack hasn't been seen yet.
void finalize() {
    finalize_links();
    if (g_file_refs.empty()) return;
    for (PendingObject& p : g_pending) {
        std::vector<uint32_t> fids;
        for (uint32_t idx : p.ref_indices)
            if (idx < g_file_refs.size() && g_file_refs[idx]) fids.push_back(g_file_refs[idx]);
        if (fids.empty()) continue;
        if (!p.name.empty())
            for (uint32_t v : fids) g_fileid_name[v] = p.name;
        g_map[p.key] = std::move(fids);
        g_base_id[p.key] = p.base_id;
    }
    g_pending.clear();
}

} // namespace

bool built() { return !g_map.empty(); }
size_t size() { return g_map.size(); }
void clear() {
    g_map.clear();
    g_base_id.clear();
    g_fileid_name.clear();
    g_file_refs.clear();
    g_pending.clear();
    g_item_links.clear();
    g_skin_tokens.clear();
    g_obj_at.clear();
    g_edges.clear();
    g_pack_file_ids.clear();
    g_anchor_pack = UINT32_MAX;
}

size_t build(const std::string& dat_path, const std::vector<MftData>& cntc_entries,
             const std::vector<uint32_t>& file_ids, const std::function<void(size_t, size_t)>& progress) {
    size_t total = cntc_entries.size();
    for (size_t i = 0; i < total; ++i) {
        std::vector<uint8_t> bytes = decompress(dat_path, cntc_entries[i]);
        // baseId == mft_index + 1 (the convention this whole codebase uses --
        // see e.g. entry_extractor.h's extract_entry_indexed()).
        uint32_t base_id = static_cast<uint32_t>(cntc_entries[i].original_index) + 1;
        uint32_t file_id = i < file_ids.size() ? file_ids[i] : 0;
        if (!bytes.empty()) parse_cntc(bytes, base_id, file_id);
        if (progress) progress(i + 1, total);
    }
    finalize();
    return g_map.size();
}

size_t build_from_packs(const std::vector<PackBytes>& packs) {
    for (const PackBytes& p : packs)
        if (!p.bytes.empty()) parse_cntc(p.bytes, p.base_id, p.file_id);
    finalize();
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

uint64_t skin_token(uint32_t skin_id) {
    auto it = g_skin_tokens.find(skin_id);
    return it == g_skin_tokens.end() ? 0 : it->second;
}

const std::vector<ContentLink>& item_links(uint32_t item_id) {
    static const std::vector<ContentLink> none;
    auto it = g_item_links.find(item_id);
    return it == g_item_links.end() ? none : it->second;
}

const std::string& name_for_fileid(uint32_t file_id) {
    auto it = g_fileid_name.find(file_id);
    return it == g_fileid_name.end() ? g_empty_name : it->second;
}

// ---- binary cache: "GC5N" magic, u32 count, then count * {u64 key, u8 n, n*u32 fileId},
// then u32 count2, count2 * {u32 item dataId, u8 n, n*{u32 type, u32 id, u32 via}}.
// (GC2N caches were keyed by uid@+20 and stored raw fileRefs indices as fileIds;
// GC3N/GC4N caches held only partial item->skin links. load() rejects them all,
// so the map is rebuilt.)
constexpr uint32_t kCacheMagic = 0x4E364347;  // 'GC6N' (adds skin tokens)
bool save(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    uint32_t magic = kCacheMagic;
    uint32_t count = static_cast<uint32_t>(g_map.size());
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&count, 4, 1, f);
    for (const auto& [k, v] : g_map) {
        uint8_t n = static_cast<uint8_t>(std::min<size_t>(v.size(), 255));
        std::fwrite(&k, 8, 1, f);
        std::fwrite(&n, 1, 1, f);
        std::fwrite(v.data(), 4, n, f);
    }
    uint32_t count2 = static_cast<uint32_t>(g_item_links.size());
    std::fwrite(&count2, 4, 1, f);
    for (const auto& [item, links] : g_item_links) {
        uint8_t n = static_cast<uint8_t>(std::min<size_t>(links.size(), 255));
        std::fwrite(&item, 4, 1, f);
        std::fwrite(&n, 1, 1, f);
        for (size_t i = 0; i < n; ++i) {
            uint32_t rec[3] = {links[i].type, links[i].id, links[i].via_item};
            std::fwrite(rec, 4, 3, f);
        }
    }
    uint32_t count3 = static_cast<uint32_t>(g_skin_tokens.size());
    std::fwrite(&count3, 4, 1, f);
    for (const auto& [skin, token] : g_skin_tokens) {
        std::fwrite(&skin, 4, 1, f);
        std::fwrite(&token, 8, 1, f);
    }
    std::fclose(f);
    return true;
}

bool load(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    uint32_t magic = 0, count = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || magic != kCacheMagic) { std::fclose(f); return false; }
    if (std::fread(&count, 4, 1, f) != 1) { std::fclose(f); return false; }
    g_map.reserve(count + 16);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t k; uint8_t n;
        if (std::fread(&k, 8, 1, f) != 1 || std::fread(&n, 1, 1, f) != 1) break;
        std::vector<uint32_t> v(n);
        if (n && std::fread(v.data(), 4, n, f) != n) break;
        g_map[k] = std::move(v);
    }
    uint32_t count2 = 0;
    if (std::fread(&count2, 4, 1, f) == 1) {
        for (uint32_t i = 0; i < count2; ++i) {
            uint32_t item;
            uint8_t n;
            if (std::fread(&item, 4, 1, f) != 1 || std::fread(&n, 1, 1, f) != 1) break;
            std::vector<ContentLink> links(n);
            bool ok = true;
            for (auto& l : links) {
                uint32_t rec[3];
                if (std::fread(rec, 4, 3, f) != 3) { ok = false; break; }
                l = {rec[0], rec[1], rec[2]};
            }
            if (!ok) break;
            g_item_links[item] = std::move(links);
        }
    }
    uint32_t count3 = 0;
    if (std::fread(&count3, 4, 1, f) == 1) {
        for (uint32_t i = 0; i < count3; ++i) {
            uint32_t skin;
            uint64_t token;
            if (std::fread(&skin, 4, 1, f) != 1 || std::fread(&token, 8, 1, f) != 1) break;
            g_skin_tokens[skin] = token;
        }
    }
    std::fclose(f);
    return !g_map.empty();
}

} // namespace castlemist::cmap
