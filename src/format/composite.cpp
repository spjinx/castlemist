#include "castlemist/format/composite.h"

#include <cstring>

#include "castlemist/format/content_schema.h"

namespace castlemist::composite {
namespace {

// PackCompositeV20 field offsets (packed structs; see the research note).
constexpr size_t kRoot = 28;               // 12-byte PF header + 16-byte chunk header
constexpr size_t kRootBlitRects = 12;
constexpr size_t kRootRaceSexData = 36;
constexpr size_t kBlitSetSize = 40;        // name w, size u32x2, rectIndex a, rectArray a
constexpr size_t kRaceSize = 224;
constexpr size_t kRaceFileData = 104;
constexpr size_t kRaceSkeleton = 140;
constexpr size_t kRaceEars = 60;
constexpr size_t kRaceFaces = 92;
constexpr size_t kRaceHairStyles = 120;
constexpr size_t kRaceSkinPatterns = 148;  // 48-byte records: 6 filerefs
constexpr size_t kRaceSkinStyles = 176;  // {chest, feet, hands, legs} u64 each
constexpr size_t kFileDataSize = 103;
constexpr size_t kRaceBodyBoneScales = 36;
constexpr size_t kRaceFaceBoneScales = 80;
constexpr size_t kBoneScaleSubSize = 53;  // packed: u64 bone, u8 flag, f32 max, f32 min, 9 x f32

struct Reader {
    std::span<const uint8_t> d;
    bool ok = true;

    bool has(size_t at, size_t n) {
        if (at > d.size() || n > d.size() - at) ok = false;
        return ok;
    }
    uint32_t u32(size_t at) {
        if (!has(at, 4)) return 0;
        uint32_t v;
        std::memcpy(&v, d.data() + at, 4);
        return v;
    }
    uint64_t u64(size_t at) {
        if (!has(at, 8)) return 0;
        uint64_t v;
        std::memcpy(&v, d.data() + at, 8);
        return v;
    }
    uint8_t u8(size_t at) { return has(at, 1) ? d[at] : 0; }
    // i64 self-relative pointer at `at`; 0 = null.
    size_t ptr(size_t at) {
        int64_t rel = static_cast<int64_t>(u64(at));
        if (!ok || rel == 0) return 0;
        int64_t target = static_cast<int64_t>(at) + rel;
        if (target < 0 || static_cast<uint64_t>(target) >= d.size()) {
            ok = false;
            return 0;
        }
        return static_cast<size_t>(target);
    }
    // {u32 count, i64 rel}; the elements must fit in the buffer.
    std::pair<uint32_t, size_t> arr(size_t at, size_t elem_size) {
        uint32_t n = u32(at);
        if (!ok || n == 0) return {0, 0};
        size_t p = ptr(at + 4);
        if (!ok || !has(p, static_cast<size_t>(n) * elem_size)) return {0, 0};
        return {n, p};
    }
    std::string wstr(size_t at) {
        size_t p = ptr(at);
        std::string s;
        for (; p && has(p, 2) && s.size() < 256; p += 2) {
            uint32_t c = d[p] | (d[p + 1] << 8);
            if (!c) break;
            s += c < 0x80 ? static_cast<char>(c) : '?';
        }
        return s;
    }
    uint32_t fileref(size_t at) {
        size_t p = ptr(at);
        if (!p || !has(p, 4)) return 0;
        return cschema::decode_fileref_pair(d, p);
    }
};

CompositeFileData read_file_data(Reader& r, size_t p) {
    CompositeFileData f;
    f.token = r.u64(p + 0);
    f.type = r.u8(p + 8);
    f.mesh_base = r.fileref(p + 18);
    f.mesh_overlap = r.fileref(p + 26);
    for (size_t i = 0; i < 4; ++i) f.mask_dye[i] = r.fileref(p + 34 + 8 * i);
    f.mask_cut = r.fileref(p + 66);
    f.texture_base = r.fileref(p + 74);
    f.texture_normal = r.fileref(p + 82);
    f.dye_flags = r.u32(p + 90);
    f.hide_flags = r.u32(p + 94);
    f.skin_flags = r.u32(p + 98);
    f.blit_set = r.u8(p + 102);
    return f;
}

} // namespace

const CompositeRace* Composite::race(std::string_view name) const {
    for (const CompositeRace& r : races)
        if (r.name == name) return &r;
    return nullptr;
}

std::optional<Composite> parse_composite(std::span<const uint8_t> d) {
    if (d.size() < kRoot + 50 || std::memcmp(d.data(), "PF", 2) != 0 || std::memcmp(d.data() + 8, "cmpc", 4) != 0 ||
        std::memcmp(d.data() + 12, "comp", 4) != 0)
        return std::nullopt;
    Reader r{d};
    Composite c;

    auto [nsets, psets] = r.arr(kRoot + kRootBlitRects, kBlitSetSize);
    for (uint32_t i = 0; i < nsets && r.ok; ++i) {
        size_t q = psets + i * kBlitSetSize;
        BlitRectSet s;
        s.name = r.wstr(q);
        s.width = r.u32(q + 8);
        s.height = r.u32(q + 12);
        auto [nrect, prect] = r.arr(q + 28, 16);
        for (uint32_t k = 0; k < nrect && r.ok; ++k) {
            size_t e = prect + k * 16;
            s.rects.push_back({r.u32(e), r.u32(e + 4), r.u32(e + 8), r.u32(e + 12)});
        }
        c.blit_sets.push_back(std::move(s));
    }

    auto [nraces, praces] = r.arr(kRoot + kRootRaceSexData, kRaceSize);
    for (uint32_t i = 0; i < nraces && r.ok; ++i) {
        size_t q = praces + i * kRaceSize;
        CompositeRace race;
        race.name = r.wstr(q);
        race.skeleton_file = r.fileref(q + kRaceSkeleton);
        auto tokens = [&](size_t field) {
            std::vector<uint64_t> out;
            auto [n, p] = r.arr(q + field, 8);
            for (uint32_t k = 0; k < n && r.ok; ++k) out.push_back(r.u64(p + 8 * k));
            return out;
        };
        race.faces = tokens(kRaceFaces);
        race.hair_styles = tokens(kRaceHairStyles);
        race.ears = tokens(kRaceEars);
        auto [npat, ppat] = r.arr(q + kRaceSkinPatterns, 48);
        for (uint32_t k = 0; k < npat && r.ok; ++k) {
            std::array<uint32_t, 6> files{};
            for (size_t m = 0; m < 6; ++m) files[m] = r.fileref(ppat + 48 * k + 8 * m);
            race.skin_patterns.push_back(files);
        }
        auto [ns, ps] = r.arr(q + kRaceSkinStyles, 32);
        for (uint32_t k = 0; k < ns && r.ok; ++k)
            race.skin_styles.push_back({r.u64(ps + 32 * k), r.u64(ps + 32 * k + 8), r.u64(ps + 32 * k + 16),
                                        r.u64(ps + 32 * k + 24)});
        auto presets = [&](size_t field) {
            std::vector<BoneScalePreset> out;
            auto [n, p] = r.arr(q + field, 24);
            for (uint32_t k = 0; k < n && r.ok; ++k) {
                BoneScalePreset pre;
                const size_t rec = p + 24 * k;
                auto [ng, pg] = r.arr(rec, 24);
                pre.second_count = r.u32(rec + 12);
                for (uint32_t g = 0; g < ng && r.ok; ++g) {
                    BoneScaleGroup grp;
                    const size_t e = pg + 24 * g;
                    grp.token = r.u64(e);
                    const uint32_t w = r.u32(e + 8);
                    std::memcpy(&grp.weight, &w, 4);
                    auto [ns, psub] = r.arr(e + 12, kBoneScaleSubSize);
                    for (uint32_t s2 = 0; s2 < ns && r.ok; ++s2) {
                        BoneScaleSub sub;
                        const size_t b = psub + kBoneScaleSubSize * s2;
                        sub.bone = r.u64(b);
                        sub.flag = r.u8(b + 8);
                        uint32_t v = r.u32(b + 9);
                        std::memcpy(&sub.max, &v, 4);
                        v = r.u32(b + 13);
                        std::memcpy(&sub.min, &v, 4);
                        for (size_t f = 0; f < 9; ++f) {
                            v = r.u32(b + 17 + 4 * f);
                            std::memcpy(&sub.values[f], &v, 4);
                        }
                        grp.subs.push_back(sub);
                    }
                    pre.groups.push_back(std::move(grp));
                }
                out.push_back(std::move(pre));
            }
            return out;
        };
        race.body_bone_scales = presets(kRaceBodyBoneScales);
        race.face_bone_scales = presets(kRaceFaceBoneScales);
        auto [nfd, pfd] = r.arr(q + kRaceFileData, kFileDataSize);
        race.file_data.reserve(nfd);
        for (uint32_t k = 0; k < nfd && r.ok; ++k) {
            CompositeFileData f = read_file_data(r, pfd + k * kFileDataSize);
            race.file_data.emplace(f.token, f);
        }
        c.races.push_back(std::move(race));
    }
    if (!r.ok) return std::nullopt;
    return c;
}

} // namespace castlemist::composite
