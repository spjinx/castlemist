/// @file
/// @brief Tests for the skillfx layer: skill effects in content, skill sets, arcdps logs, pairing, export.

#include "test_framework.h"

#include "castlemist/format/cntc_pack.h"
#include "castlemist/skillfx/effects.h"
#include "castlemist/skillfx/evtc.h"
#include "castlemist/skillfx/pairing.h"
#include "castlemist/skillfx/skill_set.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
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

// ---- synthetic arcdps logs (EVTC revision 1)

struct LogBuilder {
    std::vector<uint8_t> d;
    void u8(uint8_t v) { d.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) d.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void fixed(const std::string& s, size_t n) {
        for (size_t i = 0; i < n; ++i) d.push_back(i < s.size() ? static_cast<uint8_t>(s[i]) : 0);
    }
    struct Ev { uint64_t time, src, dst; uint32_t skill; uint8_t statechange; };
    /// Header, agents {addr, elite, name}, skills {id, name}, then events.
    LogBuilder(const std::vector<std::tuple<uint64_t, uint32_t, std::string>>& agents,
               const std::vector<std::pair<int32_t, std::string>>& skills, const std::vector<Ev>& events) {
        fixed("EVTC", 4);
        fixed("20261021", 8);
        u8(1);
        u16(16199);
        u8(0);
        u32(static_cast<uint32_t>(agents.size()));
        for (auto& [addr, elite, name] : agents) {
            u64(addr);
            u32(1);
            u32(elite);
            fixed("", 12);
            fixed(name, 64);
            fixed("", 4);
        }
        u32(static_cast<uint32_t>(skills.size()));
        for (auto& [id, name] : skills) {
            u32(static_cast<uint32_t>(id));
            fixed(name, 64);
        }
        for (const Ev& e : events) {
            u64(e.time);
            u64(e.src);
            u64(e.dst);
            u32(0);  // value
            u32(0);  // buff_dmg
            u32(0);  // overstack
            u32(e.skill);
            for (int i = 0; i < 4; ++i) u16(0);  // instids
            fixed("", 8);                       // iff .. is_moving
            u8(e.statechange);
            fixed("", 7);                       // is_flanking .. pad
        }
    }
};

constexpr uint64_t kPlayer = 0x7d0;

/// The Task 4 log: IDTOGUID 9001, a Searing Spell cast at 1000, its effect at 1400.
std::vector<uint8_t> searing_spell_log() {
    LogBuilder b({{kPlayer, 62, "Musa"}}, {{41258, "Chapter 1: Searing Spell"}},
                 {{0, 0x0011223344556677ull, 0x8899aabbccddeeffull, 9001, evtc::kIdToGuid},
                  {1000, kPlayer, 0, 41258, evtc::kAnimationStart},
                  {1400, kPlayer, 0, 9001, evtc::kEffectAgent}});
    for (int i = 0; i < 10; ++i) b.u8(0xAB);  // a cut-off trailing event
    return b.d;
}

/// Wraps `data` as the first entry of a zip, stored (method 0).
std::vector<uint8_t> stored_zip(const std::vector<uint8_t>& data) {
    LogBuilder z({}, {}, {});
    z.d.clear();
    z.u32(0x04034b50);
    z.u16(20);
    z.u16(0);
    z.u16(0);  // method: stored
    z.u16(0);
    z.u16(0);
    z.u32(0);  // crc (not checked)
    z.u32(static_cast<uint32_t>(data.size()));
    z.u32(static_cast<uint32_t>(data.size()));
    z.u16(1);
    z.u16(0);
    z.u8('x');
    z.d.insert(z.d.end(), data.begin(), data.end());
    return z.d;
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

CM_TEST(skillfx, set_from_namespace_with_bundle) {
    // 0 seed skill AAAAA.aaaaa -> effect 6; 1 skill AAAAA.bbbbb; 2 skill CCCCC.ccccc (other namespace);
    // 3 item AAAAA.item0 +264 -> bar 4; bar +192 -> 1, +304 -> 0; 5 effect EEEEE.22222 (unlinked);
    // 6 effect EEEEE.11111
    cntc::PackSet s = make_set({
        {kSkillType, 5001, {{500, "AAAAA.aaaaa"}}, {}, {{184, 6}}},
        {kSkillType, 5002, {{500, "AAAAA.bbbbb"}}, {}, {}},
        {kSkillType, 5003, {{500, "CCCCC.ccccc"}}, {}, {}},
        {kItemType, 83470, {{500, "AAAAA.item0"}}, {}, {{264, 4}}},
        {kBarType, 5887, {}, {}, {{192, 1}, {304, 0}}},
        {kEffectType, 6, {{100, "EEEEE.22222"}}, {{48, 1000022}}, {}},
        {kEffectType, 6, {{100, "EEEEE.11111"}}, {{48, 1000011}}, {}},
    });
    SkillSet set = build_skill_set(s, {5001}, test_container);
    CHECK_EQ(set.skill_namespace, std::string("AAAAA."));
    CHECK_EQ(set.effect_namespace, std::string("EEEEE."));
    CHECK_EQ(set.skills.size(), size_t{2});
    if (set.skills.size() == 2) {
        CHECK_EQ(set.skills[0].id, 5001u);
        CHECK_EQ(set.skills[0].bundle_item, 83470u);
        CHECK_EQ(set.skills[0].bundle_slot, 1);
        CHECK_EQ(set.skills[0].links.effects.size(), size_t{1});
        CHECK_EQ(set.skills[1].id, 5002u);
        CHECK_EQ(set.skills[1].bundle_slot, 0);
    }
    CHECK_EQ(set.unlinked.size(), size_t{1});
    if (!set.unlinked.empty()) {
        CHECK_EQ(set.unlinked[0].codename, std::string("EEEEE.22222"));
        CHECK_EQ(set.unlinked[0].model, 1000022u);
    }
}

CM_TEST(skillfx, effect_index_by_guid) {
    cntc::PackSet s = make_set({
        {kSkillType, 5001, {}, {}, {}},
        {kEffectType, 6, {}, {}, {}},
        {kEffectType, 6, {}, {}, {}},
    });
    auto idx = effect_index(s);
    CHECK_EQ(idx.size(), size_t{2});
    auto a = idx.find(test_guid(1));
    auto b = idx.find(test_guid(2));
    CHECK(a != idx.end() && a->second.second == &obj(s, 1));
    CHECK(b != idx.end() && b->second.second == &obj(s, 2));
}

namespace {
void check_searing_spell_log(const std::optional<EvtcLog>& log) {
    CHECK(log.has_value());
    if (!log) return;
    CHECK_EQ(log->build, std::string("20261021"));
    CHECK_EQ(log->revision, uint8_t{1});
    CHECK_EQ(log->events.size(), size_t{3});
    CHECK_EQ(log->agents.size(), size_t{1});
    if (!log->agents.empty()) {
        CHECK_EQ(log->agents[0].elite, 62u);
        CHECK_EQ(log->agents[0].name, std::string("Musa"));
    }
    CHECK_EQ(log->skills.at(41258), std::string("Chapter 1: Searing Spell"));
    const Guid want{0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00, 0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88};
    auto g = effect_guids(*log);
    CHECK(g.count(9001) && g.at(9001) == want);
}
}  // namespace

CM_TEST(skillfx, evtc_reads_plain_log) {
    check_searing_spell_log(read_evtc(searing_spell_log()));
}

CM_TEST(skillfx, evtc_reads_stored_zip) {
    check_searing_spell_log(read_evtc(stored_zip(searing_spell_log())));
}

// One of the user's real (deflated) golem logs, when present.
CM_TEST(skillfx, evtc_reads_real_zevtc) {
    const std::filesystem::path p =
        "C:/Users/vital/Documents/Guild Wars 2/addons/arcdps/arcdps.cbtlogs/Standard Kitty Golem (16199)/20261022-065900.zevtc";
    if (!std::filesystem::exists(p)) SKIP("no arcdps log on this machine");
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::optional<EvtcLog> log = read_evtc(bytes);
    CHECK(log.has_value());
    if (!log) return;
    CHECK_EQ(log->build, std::string("20261021"));
    bool firebrand = false;
    for (const EvtcAgent& a : log->agents) firebrand = firebrand || a.elite == 62;
    CHECK(firebrand);
    CHECK(effect_guids(*log).size() >= 200);
}

namespace {
/// The GUID an IDTOGUID event with these src/dst words carries.
Guid guid_from(uint64_t src, uint64_t dst) {
    Guid g{};
    std::memcpy(g.data(), &src, 8);
    std::memcpy(g.data() + 8, &dst, 8);
    return g;
}
/// A Firebrand log: IDTOGUID for content ids 1..4 (src = id, dst = 0x1000 + id), then `events`.
EvtcLog firebrand_log(std::vector<LogBuilder::Ev> events) {
    std::vector<LogBuilder::Ev> all;
    for (uint32_t id = 1; id <= 4; ++id) all.push_back({0, id, 0x1000u + id, id, evtc::kIdToGuid});
    all.insert(all.end(), events.begin(), events.end());
    return *read_evtc(LogBuilder({{kPlayer, 62, "Musa"}}, {{41258, "Chapter 1: Searing Spell"}}, all).d);
}
LogBuilder::Ev cast(uint64_t t, uint32_t skill) { return {t, kPlayer, 0, skill, evtc::kAnimationStart}; }
LogBuilder::Ev buff(uint64_t t, uint32_t skill) { return {t, kPlayer, 0, skill, evtc::kBuffApply}; }
LogBuilder::Ev fx(uint64_t t, uint32_t id) { return {t, kPlayer, 0, id, evtc::kEffectAgent}; }
Guid G(uint32_t id) { return guid_from(id, 0x1000u + id); }
}  // namespace

CM_TEST(skillfx, pairs_cast_then_effect) {
    auto p = pair_effects({firebrand_log({cast(1000, 41258), fx(1400, 1), cast(9000, 41258), fx(9500, 1)})}, 62, {});
    CHECK_EQ(p.size(), size_t{1});
    if (p.size() == 1) {
        CHECK_EQ(p[0].skill_id, 41258u);
        CHECK(p[0].effect == G(1));
        CHECK_EQ(p[0].count, 2u);
        CHECK_EQ(p[0].skill_name, std::string("Chapter 1: Searing Spell"));
    }
}

CM_TEST(skillfx, instant_skill_via_buff_marker) {
    // The effect event is written before the buff at the same millisecond: time decides, not file order.
    auto p = pair_effects({firebrand_log({cast(4000, 44602), fx(4470, 2), buff(4470, 45447), cast(8000, 44602),
                                          buff(8470, 45447), fx(8470, 2)})},
                          62, {45447});
    CHECK_EQ(p.size(), size_t{1});
    if (p.size() == 1) {
        CHECK_EQ(p[0].skill_id, 45447u);
        CHECK(p[0].effect == G(2));
        CHECK_EQ(p[0].count, 2u);
    }
}

CM_TEST(skillfx, single_sighting_dropped_and_shared_kept) {
    auto p = pair_effects({firebrand_log({cast(1000, 42898), fx(1200, 4), cast(5000, 42925), fx(5200, 4),
                                          cast(9000, 42898), fx(9200, 4), cast(13000, 42925), fx(13200, 4),
                                          cast(17000, 41258), fx(17100, 3), fx(17150, 3)})},
                          62, {});
    CHECK_EQ(p.size(), size_t{2});
    if (p.size() == 2) {
        CHECK_EQ(p[0].skill_id, 42898u);
        CHECK(p[0].effect == G(4));
        CHECK_EQ(p[1].skill_id, 42925u);
        CHECK(p[1].effect == G(4));
    }
}

CM_TEST(skillfx, ei_guids_parse) {
    std::istringstream cs(
        "    public static readonly Guid FirebrandTomeOfJusticeOpen = new(\"D573910FDB59434ABF6E7433061995BD\");\n"
        "    // not a guid line\n");
    auto ei = read_ei_guids(cs);
    const Guid want{0xd5, 0x73, 0x91, 0x0f, 0xdb, 0x59, 0x43, 0x4a, 0xbf, 0x6e, 0x74, 0x33, 0x06, 0x19, 0x95, 0xbd};
    CHECK_EQ(ei.size(), size_t{1});
    CHECK(ei.count(want) && ei.at(want) == "FirebrandTomeOfJusticeOpen");
}
