/// @file
/// @brief Tests for the skillfx layer: skill effects in content, skill sets, arcdps logs, pairing, export.

#include "test_framework.h"

#include "castlemist/format/cntc_pack.h"
#include "castlemist/skillfx/effects.h"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace castlemist;
using namespace castlemist::skillfx;

namespace {

// Synthetic cntc packs (same layout as tests/test_npc.cpp's PackBuilder): PF header,
// "cntc" tag @8, "Main" chunk @12, flags @28, 11 array descriptors
// {u32 count, i64 self-relative pointer} from @32.
constexpr size_t kChunkPos = 12;
constexpr size_t kBase = kChunkPos + 16;
constexpr size_t kArrDescBase = kBase + 4;

struct PackBuilder {
    std::vector<uint8_t> d;

    void ensure(size_t sz) {
        if (d.size() < sz) d.resize(sz, 0);
    }
    void put_u16(size_t pos, uint16_t v) {
        ensure(pos + 2);
        d[pos] = static_cast<uint8_t>(v);
        d[pos + 1] = static_cast<uint8_t>(v >> 8);
    }
    void put_u32(size_t pos, uint32_t v) {
        ensure(pos + 4);
        for (int i = 0; i < 4; ++i) d[pos + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    void put_i64(size_t pos, int64_t v) {
        ensure(pos + 8);
        uint64_t u = static_cast<uint64_t>(v);
        for (int i = 0; i < 8; ++i) d[pos + i] = static_cast<uint8_t>(u >> (8 * i));
    }
    void put_bytes(size_t pos, std::initializer_list<uint8_t> bytes) {
        ensure(pos + bytes.size());
        size_t i = 0;
        for (uint8_t b : bytes) d[pos + (i++)] = b;
    }
    void set_dynarray(int i, uint32_t count, size_t target) {
        size_t p = kArrDescBase + static_cast<size_t>(i) * 12;
        put_u32(p, count);
        if (count != 0) put_i64(p + 4, static_cast<int64_t>(target) - static_cast<int64_t>(p + 4));
    }
    void put_selfrel(size_t pos, size_t target) {
        put_i64(pos, static_cast<int64_t>(target) - static_cast<int64_t>(pos));
    }
    size_t put_utf16(size_t pos, const std::string& s) {
        ensure(pos + (s.size() + 1) * 2);
        for (size_t i = 0; i < s.size(); ++i) {
            d[pos + i * 2] = static_cast<uint8_t>(s[i]);
            d[pos + i * 2 + 1] = 0;
        }
        return pos + (s.size() + 1) * 2;
    }
    PackBuilder() {
        put_bytes(0, {'P', 'F'});
        put_u32(6, kChunkPos);
        put_bytes(8, {'c', 'n', 't', 'c'});
        put_bytes(kChunkPos, {'M', 'a', 'i', 'n'});
        put_u32(kChunkPos + 4, 1000);
        put_u32(kBase, 0);
        for (int i = 0; i <= 10; ++i) set_dynarray(i, 0, 0);
    }
};

/// One synthetic content object. Objects sit 512 bytes apart; refs name another object by index.
struct TObj {
    uint32_t type = 0, data_id = 0;
    std::vector<std::pair<uint32_t, std::string>> strings;  // {field, text}
    std::vector<std::pair<uint32_t, uint32_t>> files;        // {field, fileId}
    std::vector<std::pair<uint32_t, size_t>> refs;           // {field, target object index}
};
constexpr uint32_t kObjSize = 512;

/// The GUID the builder writes at an object's +0: 16 bytes of (index * 16 + k).
Guid test_guid(size_t index) {
    Guid g{};
    for (size_t k = 0; k < 16; ++k) g[k] = static_cast<uint8_t>(index * 16 + k);
    return g;
}

/// Encodes a fileId the way cntc fileRefs store it (two u16 halves biased by 0x100).
void put_fileref_pair(PackBuilder& b, size_t pos, uint32_t file_id) {
    const uint32_t v = file_id - 1;
    b.put_u16(pos, static_cast<uint16_t>(v % 0xff00 + 0x100));
    b.put_u16(pos + 2, static_cast<uint16_t>(v / 0xff00 + 0x100));
}

/// A one-pack PackSet holding `objs` (the pack also carries the shared fileRefs table).
cntc::PackSet make_set(const std::vector<TObj>& objs) {
    PackBuilder b;
    std::vector<uint32_t> file_ids, lo, fi, si;
    std::vector<std::string> strs;
    const size_t content_size = objs.size() * kObjSize;
    std::vector<std::pair<uint32_t, uint32_t>> u32s;  // content-relative {pos, value}
    for (size_t i = 0; i < objs.size(); ++i) {
        const uint32_t at = static_cast<uint32_t>(i * kObjSize);
        u32s.push_back({at + 16, objs[i].type});
        u32s.push_back({at + 40, objs[i].data_id});
        for (auto& [field, target] : objs[i].refs) {
            lo.push_back(at + field);
            u32s.push_back({at + field, static_cast<uint32_t>(target * kObjSize)});
        }
        for (auto& [field, fid] : objs[i].files) {
            fi.push_back(at + field);
            u32s.push_back({at + field, static_cast<uint32_t>(file_ids.size())});
            file_ids.push_back(fid);
        }
        for (auto& [field, text] : objs[i].strings) {
            si.push_back(at + field);
            u32s.push_back({at + field, static_cast<uint32_t>(strs.size())});
            strs.push_back(text);
        }
    }
    size_t pos = 400;
    const size_t ie = pos;
    pos += objs.size() * 16;
    const size_t lop = pos;
    pos += lo.size() * 4;
    const size_t fip = pos;
    pos += fi.size() * 4;
    const size_t sip = pos;
    pos += si.size() * 4;
    const size_t frp = pos;
    pos += file_ids.size() * 8;
    const size_t pairs = pos;
    pos += file_ids.size() * 4;
    const size_t stp = pos;
    pos += strs.size() * 8;
    size_t text = pos;
    for (auto& s : strs) text += (s.size() + 1) * 2;
    const size_t content = (text + 15) / 16 * 16;

    b.set_dynarray(3, static_cast<uint32_t>(objs.size()), ie);
    for (size_t i = 0; i < objs.size(); ++i) {
        b.put_u32(ie + i * 16, objs[i].type);
        b.put_u32(ie + i * 16 + 4, static_cast<uint32_t>(i * kObjSize));
    }
    b.set_dynarray(4, static_cast<uint32_t>(lo.size()), lop);
    for (size_t i = 0; i < lo.size(); ++i) b.put_u32(lop + i * 4, lo[i]);
    b.set_dynarray(6, static_cast<uint32_t>(fi.size()), fip);
    for (size_t i = 0; i < fi.size(); ++i) b.put_u32(fip + i * 4, fi[i]);
    b.set_dynarray(7, static_cast<uint32_t>(si.size()), sip);
    for (size_t i = 0; i < si.size(); ++i) b.put_u32(sip + i * 4, si[i]);
    b.set_dynarray(2, static_cast<uint32_t>(file_ids.size()), frp);
    for (size_t i = 0; i < file_ids.size(); ++i) {
        b.put_selfrel(frp + i * 8, pairs + i * 4);
        put_fileref_pair(b, pairs + i * 4, file_ids[i]);
    }
    b.set_dynarray(9, static_cast<uint32_t>(strs.size()), stp);
    size_t t = stp + strs.size() * 8;
    for (size_t i = 0; i < strs.size(); ++i) {
        b.put_selfrel(stp + i * 8, t);
        t = b.put_utf16(t, strs[i]);
    }
    b.set_dynarray(10, static_cast<uint32_t>(content_size), content);
    b.ensure(content + content_size);
    for (size_t i = 0; i < objs.size(); ++i) {
        const Guid g = test_guid(i);
        for (size_t k = 0; k < 16; ++k) b.d[content + i * kObjSize + k] = g[k];
    }
    for (auto& [p, v] : u32s) b.put_u32(content + p, v);

    cntc::PackSet set;
    set.packs.push_back(*cntc::parse_pack(b.d, 1, 900));
    set.finish();
    return set;
}

/// Index DB stand-in: fileIds >= 1000000 are models, 9xxxxx are sound scripts.
std::string test_container(uint32_t fid) {
    if (fid >= 1000000) return "MODL";
    if (fid >= 900000) return "AMSP";
    return "";
}

const cntc::Object& obj(const cntc::PackSet& s, size_t index) {
    return *s.packs[0].object_at(static_cast<uint32_t>(index * kObjSize));
}

}  // namespace

CM_TEST(skillfx, codename_skips_fact_labels) {
    cntc::PackSet s = make_set({{kSkillType, 44364, {{100, "Base Cooldown"}, {108, "Trait Cooldown"}, {116, "Bp5rI.Bw99b"}}, {}, {}}});
    CHECK_EQ(codename(obj(s, 0)), std::string("Bp5rI.Bw99b"));
}

CM_TEST(skillfx, effect_reads_model_and_sound) {
    cntc::PackSet s = make_set({{kEffectType, 6, {{100, "Bmt2N.a7wVK"}}, {{48, 1764329}, {56, 901764}}, {}}});
    Effect e = read_effect(s, 0, obj(s, 0), test_container);
    CHECK_EQ(e.kind, 6u);
    CHECK_EQ(e.model, 1764329u);
    CHECK_EQ(e.sound, 901764u);
    CHECK_EQ(e.codename, std::string("Bmt2N.a7wVK"));
    CHECK(e.guid == test_guid(0));
}

CM_TEST(skillfx, find_effects_direct_group_boon_related) {
    // 0 skill 44364: +184 -> A, +200 -> group G, +300 -> boon 737, +400 -> skill 68647
    // 1 A (kind 6), 2 G (kind 1) -> B and -> G itself, 3 B (kind 6), 4 boon 737, 5 skill 68647
    cntc::PackSet s = make_set({
        {kSkillType, 44364, {{500, "Bp5rI.Bw99b"}}, {}, {{184, 1}, {200, 2}, {300, 4}, {400, 5}}},
        {kEffectType, 6, {{100, "Bmt2N.aaaaa"}}, {{48, 1000001}}, {}},
        {kEffectType, 1, {{100, "Bmt2N.ggggg"}}, {}, {{112, 3}, {120, 2}}},
        {kEffectType, 6, {{100, "Bmt2N.bbbbb"}}, {{48, 1000002}}, {}},
        {kSkillType, 737, {}, {}, {}},
        {kSkillType, 68647, {}, {}, {}},
    });
    SkillLinks l = find_effects(s, 0, obj(s, 0), test_container);
    CHECK_EQ(l.effects.size(), size_t{2});
    if (l.effects.size() == 2) {
        CHECK_EQ(l.effects[0].codename, std::string("Bmt2N.aaaaa"));
        CHECK_EQ(l.effects[0].field, 184u);
        CHECK_EQ(l.effects[1].codename, std::string("Bmt2N.bbbbb"));
        CHECK_EQ(l.effects[1].field, 200u);
        CHECK_EQ(l.effects[1].model, 1000002u);
    }
    CHECK(l.applies == std::vector<uint32_t>{737});
    CHECK(l.related_skills == std::vector<uint32_t>{68647});
}
