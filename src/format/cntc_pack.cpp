#include "castlemist/format/cntc_pack.h"

#include "castlemist/format/content_schema.h"
#include "castlemist/native/cmp_decompress_method0.hpp"

#include <algorithm>
#include <cstring>

namespace castlemist::cntc {

namespace {

constexpr size_t kMaxString = 300;

struct Reader {
    std::span<const uint8_t> d;

    uint32_t u32(size_t p) const {
        if (p > d.size() || d.size() - p < 4) return 0;
        return d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (static_cast<uint32_t>(d[p + 3]) << 24);
    }
    int64_t i64(size_t p) const {
        if (p > d.size() || d.size() - p < 8) return 0;
        int64_t v;
        std::memcpy(&v, d.data() + p, 8);
        return v;
    }
    /// Absolute position of a self-relative pointer stored at `p`, or SIZE_MAX if out of range.
    size_t target(size_t p) const {
        const int64_t t = static_cast<int64_t>(p) + i64(p);
        if (t < 0 || static_cast<uint64_t>(t) > d.size()) return static_cast<size_t>(-1);
        return static_cast<size_t>(t);
    }
};

struct Array {
    uint32_t count = 0;
    size_t off = 0;
};

}  // namespace

const Object* Pack::object_at(uint32_t begin) const {
    auto it = std::lower_bound(objects.begin(), objects.end(), begin,
                               [](const Object& o, uint32_t b) { return o.begin < b; });
    return (it != objects.end() && it->begin == begin) ? &*it : nullptr;
}

uint32_t Pack::u32(const Object& obj, uint32_t field) const {
    const uint64_t at = static_cast<uint64_t>(obj.begin) + field;
    if (at + 4 > obj.end || at + 4 > content.size()) return 0;
    return content[at] | (content[at + 1] << 8) | (content[at + 2] << 16) | (static_cast<uint32_t>(content[at + 3]) << 24);
}

uint64_t Pack::u64(const Object& obj, uint32_t field) const {
    const uint64_t at = static_cast<uint64_t>(obj.begin) + field;
    if (at + 8 > obj.end || at + 8 > content.size()) return 0;
    return u32(obj, field) | (static_cast<uint64_t>(u32(obj, field + 4)) << 32);
}

std::optional<Pack> parse_pack(std::span<const uint8_t> d, uint32_t base_id, uint32_t file_id) {
    const size_t n = d.size();
    if (n < 16 || d[0] != 'P' || d[1] != 'F' || std::memcmp(d.data() + 8, "cntc", 4) != 0) return std::nullopt;
    const Reader r{d};

    size_t pos = d[6] | (d[7] << 8);
    while (pos + 8 <= n && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        const size_t next = pos + 8 + r.u32(pos + 4);
        if (next <= pos) return std::nullopt;
        pos = next;
    }
    constexpr size_t kDescBytes = 4 + 11 * 12;
    if (pos + 16 + kDescBytes > n) return std::nullopt;
    const size_t base = pos + 16;

    // Descriptor i: bounds-checked against the buffer for `elem`-byte entries.
    bool ok = true;
    auto array = [&](int i, size_t elem) {
        Array a;
        const size_t p = base + 4 + static_cast<size_t>(i) * 12;
        a.count = r.u32(p);
        if (a.count == 0) return a;
        a.off = r.target(p + 4);
        if (a.off == static_cast<size_t>(-1) || a.off > n || (n - a.off) / elem < a.count) {
            ok = false;
            a.count = 0;
        }
        return a;
    };
    const Array fr = array(2, 8), ie = array(3, 16), lo = array(4, 4), eo = array(5, 8), fi = array(6, 4),
                si = array(7, 4), st = array(9, 8), ct = array(10, 1);
    if (!ok) return std::nullopt;

    Pack pack;
    pack.base_id = base_id;
    pack.file_id = file_id;
    pack.content.assign(d.begin() + ct.off, d.begin() + ct.off + ct.count);

    pack.file_refs.reserve(fr.count);
    for (uint32_t i = 0; i < fr.count; ++i) {
        const size_t p = fr.off + static_cast<size_t>(i) * 8;
        const size_t q = r.target(p);
        pack.file_refs.push_back(r.i64(p) == 0 || q == static_cast<size_t>(-1)
                                     ? 0u
                                     : castlemist::cschema::decode_fileref_pair(d, q));
    }

    const std::span<const uint8_t> c(pack.content);
    const Reader cr{c};
    const uint32_t csize = static_cast<uint32_t>(c.size());

    // Objects: {offset, type}, sorted by offset, first duplicate wins.
    std::vector<std::pair<uint32_t, uint32_t>> starts;
    starts.reserve(ie.count);
    for (uint32_t i = 0; i < ie.count; ++i) {
        const size_t p = ie.off + static_cast<size_t>(i) * 16;
        const uint32_t off = r.u32(p + 4);
        if (off < csize) starts.emplace_back(off, r.u32(p));
    }
    std::stable_sort(starts.begin(), starts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    starts.erase(std::unique(starts.begin(), starts.end(), [](const auto& a, const auto& b) { return a.first == b.first; }),
                 starts.end());

    auto relocs = [&](const Array& a, size_t stride) {
        std::vector<std::pair<uint32_t, uint32_t>> v;  // {reloc, second word (external tfi)}
        v.reserve(a.count);
        for (uint32_t i = 0; i < a.count; ++i) {
            const size_t p = a.off + static_cast<size_t>(i) * stride;
            v.emplace_back(r.u32(p), stride == 8 ? r.u32(p + 4) : 0u);
        }
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto lov = relocs(lo, 4), eov = relocs(eo, 8), fiv = relocs(fi, 4), siv = relocs(si, 4);

    auto str_at = [&](uint32_t idx) {
        std::string s;
        if (idx >= st.count) return s;
        const size_t ep = st.off + static_cast<size_t>(idx) * 8;
        if (r.i64(ep) == 0) return s;
        const size_t sp = r.target(ep);
        if (sp == static_cast<size_t>(-1)) return s;
        for (size_t q = sp; q + 2 <= n && s.size() < kMaxString; q += 2) {
            const uint32_t ch = d[q] | (d[q + 1] << 8);
            if (!ch) break;
            s += ch < 0x80 ? static_cast<char>(ch) : '?';
        }
        return s;
    };

    // Entries of a sorted {reloc, x} list whose reloc lies in [b, e) with 4 readable bytes.
    auto in_range = [&](const std::vector<std::pair<uint32_t, uint32_t>>& v, uint32_t b, uint32_t e, auto&& fn) {
        for (auto it = std::lower_bound(v.begin(), v.end(), std::make_pair(b, 0u)); it != v.end() && it->first < e; ++it)
            if (static_cast<uint64_t>(it->first) + 4 <= csize) fn(*it);
    };

    pack.objects.reserve(starts.size());
    for (size_t k = 0; k < starts.size(); ++k) {
        Object o;
        o.begin = starts[k].first;
        o.type = starts[k].second;
        o.end = k + 1 < starts.size() ? starts[k + 1].first : csize;
        if (static_cast<uint64_t>(o.begin) + 44 <= csize) o.data_id = cr.u32(o.begin + 40);
        in_range(lov, o.begin, o.end, [&](const auto& e) {
            o.refs.push_back({e.first - o.begin, 0, cr.u32(e.first), false});
        });
        in_range(eov, o.begin, o.end, [&](const auto& e) {
            o.refs.push_back({e.first - o.begin, e.second, cr.u32(e.first), true});
        });
        std::stable_sort(o.refs.begin(), o.refs.end(), [](const Ref& a, const Ref& b) { return a.field < b.field; });
        in_range(fiv, o.begin, o.end, [&](const auto& e) { o.files.emplace_back(e.first - o.begin, cr.u32(e.first)); });
        in_range(siv, o.begin, o.end, [&](const auto& e) {
            const uint32_t idx = cr.u32(e.first);
            if (idx < st.count) o.strings.emplace_back(e.first - o.begin, str_at(idx));
        });
        pack.objects.push_back(std::move(o));
    }
    return pack;
}

void PackSet::finish() {
    std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.file_id < b.file_id; });
    anchor_ = static_cast<size_t>(-1);
    for (size_t i = 0; i < packs.size(); ++i)
        if (!packs[i].file_refs.empty()) {
            anchor_ = i;
            break;
        }
}

const std::vector<uint32_t>& PackSet::shared_file_refs() const {
    static const std::vector<uint32_t> kEmpty;
    return anchor_ < packs.size() ? packs[anchor_].file_refs : kEmpty;
}

std::pair<const Pack*, const Object*> PackSet::resolve(size_t from, const Ref& r) const {
    if (from >= packs.size()) return {nullptr, nullptr};
    const Pack* target = nullptr;
    if (!r.external) {
        target = &packs[from];
    } else if (anchor_ < packs.size() && r.target_file_index < packs.size() - anchor_) {
        target = &packs[anchor_ + r.target_file_index];
    }
    if (!target) return {nullptr, nullptr};
    return {target, target->object_at(r.target_offset)};
}

std::vector<uint8_t> decompress_cntc(std::span<const uint8_t> raw, bool compressed) {
    std::vector<uint8_t> s = castlemist::cmp::strip_crc32(raw);
    if (!compressed) return s;
    if (s.size() < 8) return {};
    uint32_t u = s[4] | (s[5] << 8) | (s[6] << 16) | (static_cast<uint32_t>(s[7]) << 24);
    try {
        return castlemist::cmp::decompress_method0(std::span<const uint8_t>(s).subspan(8), u);
    } catch (const std::exception&) {
        return {};  // corrupt stream: the caller records "unreadable" and continues
    }
}

}  // namespace castlemist::cntc
