/// @file
/// @brief Tests for the npc layer: packed clip-name tokens, clip grouping and the cntc pack reader.

#include "test_framework.h"

#include "castlemist/format/content_map.h"
#include "castlemist/npc/catalogue.h"
#include "castlemist/npc/tokens.h"
#include "character.h"
#include "pack.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

using namespace castlemist::npc;
using namespace castlemist::npc::detail;

namespace {

// Synthetic cntc packs: PF header (2) .. "cntc" tag (@8) .. "Main" chunk (@12) ..
// flags (@28) .. 11 array descriptors {u32 count, i64 self-relative pointer} (@32).
constexpr size_t kChunkPos = 12;
constexpr size_t kBase = kChunkPos + 16;
constexpr size_t kArrDescBase = kBase + 4;

size_t arr_desc(int i) { return kArrDescBase + static_cast<size_t>(i) * 12; }

/// @brief Builds one synthetic PF "cntc" -> "Main" (PackContent) buffer.
struct PackBuilder {
    std::vector<uint8_t> d;

    void ensure(size_t sz) {
        if (d.size() < sz) d.resize(sz, 0);
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
    /// Array `i` = `count` entries starting at absolute position `target` (count 0 = empty).
    void set_dynarray(int i, uint32_t count, size_t target) {
        size_t p = arr_desc(i);
        put_u32(p, count);
        if (count != 0) put_i64(p + 4, static_cast<int64_t>(target) - static_cast<int64_t>(p + 4));
    }
    /// An 8-byte self-relative pointer at `pos` to absolute `target`.
    void put_selfrel(size_t pos, size_t target) {
        put_i64(pos, static_cast<int64_t>(target) - static_cast<int64_t>(pos));
    }
    /// NUL-terminated UTF-16LE ASCII string at `pos`; returns the position after it.
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

/// One object at content offset 0 (64 bytes); optional fileRefs (one entry, id 500) and an
/// external ref at +20 with target file index 2.
std::vector<uint8_t> tiny_pack(uint32_t type, bool with_file_refs, bool with_external_ref) {
    PackBuilder b;
    const size_t ie = 200, eo = 220, fr = 240, frp = 260, content = 300;
    b.set_dynarray(3, 1, ie);
    b.put_u32(ie, type);
    b.put_u32(ie + 4, 0);
    if (with_external_ref) {
        b.set_dynarray(5, 1, eo);
        b.put_u32(eo, 20);
        b.put_u32(eo + 4, 2);
    }
    if (with_file_refs) {
        b.set_dynarray(2, 1, fr);
        b.put_selfrel(fr, frp);
        b.put_bytes(frp, {0xF3, 0x02, 0x00, 0x01});
    }
    b.set_dynarray(10, 64, content);
    b.ensure(content + 64);
    return b.d;
}

/// Inverse of decode_token: 5-bit groups, low first, 'a' = 1.
uint64_t pack_token(const std::string& s) {
    uint64_t v = 0;
    for (size_t i = 0; i < s.size(); ++i) v |= static_cast<uint64_t>(s[i] - 'a' + 1) << (5 * i);
    return v;
}

struct TObj { uint32_t type, begin, data_id; };
struct TRef { uint32_t slot, target; };  ///< Absolute content offsets; local ref.

/// A pack of `objs` (content size 1200) with local refs, parsed.
Pack make_char_pack(const std::vector<TObj>& objs, const std::vector<TRef>& refs,
                    const std::vector<std::pair<uint32_t, uint64_t>>& u64s,
                    const std::vector<std::pair<uint32_t, uint32_t>>& u32s) {
    PackBuilder b;
    const size_t ie = 100, lo = 400, content = 600;
    b.set_dynarray(3, static_cast<uint32_t>(objs.size()), ie);
    for (size_t i = 0; i < objs.size(); ++i) {
        b.put_u32(ie + i * 16, objs[i].type);
        b.put_u32(ie + i * 16 + 4, objs[i].begin);
    }
    b.set_dynarray(4, static_cast<uint32_t>(refs.size()), lo);
    for (size_t i = 0; i < refs.size(); ++i) b.put_u32(lo + i * 4, refs[i].slot);
    b.set_dynarray(10, 1200, content);
    b.ensure(content + 1200);
    for (auto& o : objs) b.put_u32(content + o.begin + 40, o.data_id);
    for (auto& r : refs) b.put_u32(content + r.slot, r.target);
    for (auto& [pos, v] : u64s) b.put_i64(content + pos, static_cast<int64_t>(v));
    for (auto& [pos, v] : u32s) b.put_u32(content + pos, v);
    auto pack = parse_pack(b.d, 1, 1);
    return *pack;
}

// ---- catalogue fixtures ----
// fileRefs pack: [0] = 500, [1] = 777.
std::vector<uint8_t> cat_file_refs_pack() {
    PackBuilder b;
    const size_t ptrs = 200, pairs = ptrs + 16;
    b.put_selfrel(ptrs, pairs);
    b.put_selfrel(ptrs + 8, pairs + 4);
    b.put_bytes(pairs, {0xF3, 0x02, 0x00, 0x01});
    b.put_bytes(pairs + 4, {0x08, 0x04, 0x00, 0x01});
    b.set_dynarray(2, 2, ptrs);
    b.put_u32(260, 1);
    b.ensure(280);
    b.set_dynarray(3, 1, 260);
    b.ensure(400 + 64);
    b.set_dynarray(10, 64, 400);
    return b.d;
}

// One skin (type 66, dataId) with model slot +48 = fileRefs index 1 (777).
std::vector<uint8_t> cat_skin_pack(uint32_t data_id) {
    PackBuilder b;
    b.put_u32(200, 66);
    b.ensure(216);
    b.set_dynarray(3, 1, 200);
    b.put_u32(220, 48);
    b.set_dynarray(6, 1, 220);
    const size_t c = 300;
    b.put_u32(c + 16, 66);
    b.put_u32(c + 40, data_id);
    b.put_u32(c + 48, 1);
    b.ensure(c + 128);
    b.set_dynarray(10, 128, c);
    return b.d;
}

// One weapon item (type 35, +44 = 24) with a +176 external skin link to file index 1, offset 0.
std::vector<uint8_t> cat_item_pack(uint32_t data_id) {
    PackBuilder b;
    b.put_u32(200, 35);
    b.ensure(216);
    b.set_dynarray(3, 1, 200);
    b.put_u32(220, 176);
    b.put_u32(224, 1);
    b.set_dynarray(5, 1, 220);
    b.put_u32(240, 64);
    b.set_dynarray(6, 1, 240);
    const size_t c = 300;
    b.put_u32(c + 16, 35);
    b.put_u32(c + 40, data_id);
    b.put_u32(c + 44, 24);
    b.put_u32(c + 64, 0);
    b.put_u32(c + 176, 0);
    b.ensure(c + 288);
    b.set_dynarray(10, 288, c);
    return b.d;
}

// One character (type 67): optional file slots +224/+240 (fileRefs 0, 1) and a +200 external
// weapon ref to file index 2, offset 0.
std::vector<uint8_t> cat_char_pack(uint32_t data_id, bool with_files, bool with_weapon) {
    PackBuilder b;
    b.put_u32(200, 67);
    b.ensure(216);
    b.set_dynarray(3, 1, 200);
    if (with_weapon) {
        b.put_u32(220, 200);
        b.put_u32(224, 2);
        b.set_dynarray(5, 1, 220);
    }
    if (with_files) {
        b.put_u32(240, 224);
        b.put_u32(244, 240);
        b.set_dynarray(6, 2, 240);
    }
    const size_t c = 400;
    b.put_u32(c + 16, 67);
    b.put_u32(c + 40, data_id);
    b.put_u32(c + 200, 0);
    b.put_u32(c + 224, 0);
    b.put_u32(c + 240, 1);
    b.ensure(c + 300);
    b.set_dynarray(10, 300, c);
    return b.d;
}

std::string stub_container(uint32_t fid) { return fid == 500 ? "MODL" : fid == 777 ? "AMSP" : ""; }

}  // namespace

namespace cmap = castlemist::cmap;

CM_TEST(npc_catalogue, builds_records_from_the_content_map_and_character_fields) {
    cmap::clear();
    std::vector<cmap::PackBytes> packs = {{1, 900, cat_file_refs_pack()},
                                          {2, 901, cat_skin_pack(6506)},
                                          {3, 902, cat_item_pack(76158)},
                                          {4, 903, cat_char_pack(4242, true, true)}};
    cmap::build_from_packs(packs);
    Catalogue c = build_catalogue(packs, stub_container);
    CHECK_EQ(c.npcs.size(), size_t(1));
    const NpcRecord* r = c.find(4242);
    CHECK(r != nullptr);
    if (r) {
        CHECK_EQ(r->model, 500u);
        CHECK_EQ(r->sound_script, 777u);
        CHECK_EQ(r->weapons.size(), size_t(1));
        if (!r->weapons.empty()) {
            CHECK_EQ(r->weapons[0].slot, 200u);
            CHECK_EQ(r->weapons[0].item, 76158u);
            CHECK_EQ(r->weapons[0].skin, 6506u);
            CHECK_EQ(r->weapons[0].model, 777u);
        }
        CHECK_EQ(c.by_model(500).size(), size_t(1));
    }
    Coverage cov = c.coverage();
    CHECK_EQ(cov.npcs, size_t(1));
    CHECK_EQ(cov.with_model, size_t(1));
    CHECK_EQ(cov.with_weapons, size_t(1));
    cmap::clear();
}

CM_TEST(npc_catalogue, lists_a_character_the_content_map_does_not_hold) {
    cmap::clear();
    std::vector<cmap::PackBytes> packs = {{1, 900, cat_file_refs_pack()},
                                          {2, 901, cat_skin_pack(6506)},
                                          {4, 903, cat_char_pack(99, false, false)}};
    cmap::build_from_packs(packs);
    Catalogue c = build_catalogue(packs, stub_container);
    CHECK_EQ(c.npcs.size(), size_t(1));
    const NpcRecord* r = c.find(99);
    CHECK(r != nullptr);
    if (r) CHECK_EQ(r->model, 0u);
    CHECK_EQ(c.coverage().with_model, size_t(0));
    cmap::clear();
}

CM_TEST(npc_catalogue, without_a_content_map_the_catalogue_is_empty_with_a_warning) {
    cmap::clear();
    std::vector<cmap::PackBytes> packs = {{4, 903, cat_char_pack(99, false, false)}};
    Catalogue c = build_catalogue(packs, stub_container);
    CHECK(c.npcs.empty());
    CHECK_EQ(c.warnings.size(), size_t(1));
    if (!c.warnings.empty()) CHECK_EQ(c.warnings[0], std::string("content map not loaded"));
}

CM_TEST(npc_catalogue, round_trips_through_json) {
    Catalogue c;
    NpcRecord r;
    r.id = 7; r.codename = "abc.def"; r.model = 11; r.sound_script = 12;
    r.race = "charrmale"; r.modifier = "ghost";
    r.weapons = {{200, 1, 2, 3}};
    r.skills = {5, 6};
    c.npcs.push_back(r);
    c.warnings.push_back("w");
    auto path = std::filesystem::temp_directory_path() / "cm_npc_catalogue_rt.json";
    CHECK(save(c, path));
    auto l = load(path);
    CHECK(l.has_value());
    if (l) {
        CHECK_EQ(l->npcs.size(), size_t(1));
        const NpcRecord& g = l->npcs[0];
        CHECK_EQ(g.id, 7u);
        CHECK_EQ(g.codename, std::string("abc.def"));
        CHECK_EQ(g.model, 11u);
        CHECK_EQ(g.sound_script, 12u);
        CHECK_EQ(g.race, std::string("charrmale"));
        CHECK_EQ(g.modifier, std::string("ghost"));
        CHECK_EQ(g.weapons.size(), size_t(1));
        CHECK_EQ(g.weapons[0].model, 3u);
        CHECK_EQ(g.skills.size(), size_t(2));
        CHECK_EQ(l->warnings.size(), size_t(1));
    }
    std::filesystem::remove(path);
}

CM_TEST(npc_catalogue, load_rejects_wrong_version) {
    auto path = std::filesystem::temp_directory_path() / "cm_npc_catalogue_bad.json";
    { std::ofstream(path) << R"({"version":2,"warnings":[],"npcs":[]})"; }
    CHECK(!load(path).has_value());
    { std::ofstream(path) << "not json"; }
    CHECK(!load(path).has_value());
    std::filesystem::remove(path);
    CHECK(!load(path).has_value());
}

CM_TEST(npc, decodes_a_packed_clip_name) {
    CHECK_EQ(decode_token(0x80000029dd5604f7ULL), std::string("wgalunge"));
    CHECK_EQ(decode_token(0x2cdf07c8baULL), std::string("zeropose"));
}

CM_TEST(npc, rejects_groups_above_z) {
    CHECK_EQ(decode_token(0x1f), std::string());
}

CM_TEST(npc, groups_clips_by_action) {
    CHECK_EQ(clip_group("wgalunge08"), std::string("lunge"));
    CHECK_EQ(clip_prefix("wgalunge08"), std::string("wga"));
    CHECK_EQ(clip_group("uamland    l08"), std::string("land    l"));
    CHECK_EQ(clip_group("zeropose"), std::string("zeropose"));
    CHECK_EQ(clip_prefix("zeropose"), std::string());
}

CM_TEST(npc_pack, parses_objects_refs_files_and_strings) {
    PackBuilder b;
    const size_t ie = 200, lo = 240, fi = 248, si = 256, st = 264, str = 280, fr = 320, frp = 340, content = 400;
    // Two objects: type 67 at content offset 0, type 35 at 300; content is 400 bytes.
    b.set_dynarray(3, 2, ie);
    b.put_u32(ie + 0, 67);
    b.put_u32(ie + 4, 0);
    b.put_u32(ie + 16, 35);
    b.put_u32(ie + 20, 300);
    b.set_dynarray(4, 1, lo);
    b.put_u32(lo, 200);  // reloc inside the 67
    b.set_dynarray(6, 1, fi);
    b.put_u32(fi, 240);
    b.set_dynarray(7, 1, si);
    b.put_u32(si, 248);
    b.set_dynarray(9, 1, st);
    b.put_selfrel(st, str);
    b.put_utf16(str, "vl8Av.4gynM");
    b.set_dynarray(2, 2, fr);
    b.put_selfrel(fr, frp);
    b.put_selfrel(fr + 8, frp + 4);
    b.put_bytes(frp, {0xF3, 0x02, 0x00, 0x01});      // fileId 500
    b.put_bytes(frp + 4, {0x0A, 0x03, 0x00, 0x01});  // fileId 523
    b.set_dynarray(10, 400, content);
    b.ensure(content + 400);
    b.put_u32(content + 40, 777);   // data id of the 67
    b.put_u32(content + 200, 300);  // local ref -> the 35
    b.put_u32(content + 240, 1);    // fileRefs index
    b.put_u32(content + 248, 0);    // strings index

    auto pack = parse_pack(b.d, 7, 900);
    CHECK(pack.has_value());
    CHECK_EQ(pack->objects.size(), size_t(2));
    CHECK_EQ(pack->file_refs.size(), size_t(2));
    CHECK_EQ(pack->file_refs[0], 500u);
    CHECK_EQ(pack->file_refs[1], 523u);  // low 0x030A - 0x100 + 1
    const Object* a = pack->object_at(0);
    CHECK(a != nullptr);
    CHECK_EQ(a->type, 67u);
    CHECK_EQ(a->end, 300u);
    CHECK_EQ(a->data_id, 777u);
    CHECK_EQ(a->refs.size(), size_t(1));
    CHECK_EQ(a->refs[0].field, 200u);
    CHECK_EQ(a->refs[0].target_offset, 300u);
    CHECK(!a->refs[0].external);
    CHECK_EQ(a->files.size(), size_t(1));
    CHECK_EQ(a->files[0].first, 240u);
    CHECK_EQ(a->files[0].second, 1u);
    CHECK_EQ(a->strings.size(), size_t(1));
    CHECK_EQ(a->strings[0].first, 248u);
    CHECK_EQ(a->strings[0].second, std::string("vl8Av.4gynM"));
    CHECK_EQ(pack->u32(*a, 200), 300u);
    CHECK_EQ(pack->u32(*a, 5000), 0u);

    PackSet set;
    set.packs.push_back(std::move(*pack));
    set.finish();
    auto [rp, ro] = set.resolve(0, set.packs[0].objects[0].refs[0]);
    CHECK(rp == &set.packs[0]);
    CHECK(ro != nullptr);
    CHECK_EQ(ro->type, 35u);
    CHECK(set.packs[0].object_at(5) == nullptr);
}

CM_TEST(npc_pack, resolves_external_refs_by_file_id_order_from_the_file_refs_pack) {
    PackSet set;
    set.packs.push_back(*parse_pack(tiny_pack(11, false, true), 5, 901));
    set.packs.push_back(*parse_pack(tiny_pack(22, false, false), 3, 902));
    set.packs.push_back(*parse_pack(tiny_pack(33, true, false), 4, 900));
    set.finish();
    CHECK_EQ(set.shared_file_refs().size(), size_t(1));
    CHECK_EQ(set.shared_file_refs()[0], 500u);
    size_t from = 99;
    for (size_t i = 0; i < set.packs.size(); ++i)
        if (set.packs[i].file_id == 901) from = i;
    CHECK(from != 99);
    const Object& o = set.packs[from].objects[0];
    CHECK_EQ(o.refs.size(), size_t(1));
    CHECK(o.refs[0].external);
    CHECK_EQ(o.refs[0].target_file_index, 2u);
    auto [rp, ro] = set.resolve(from, o.refs[0]);
    CHECK(rp != nullptr);
    CHECK_EQ(rp->file_id, 902u);
    CHECK(ro != nullptr);
    CHECK_EQ(ro->type, 22u);
}

CM_TEST(npc_pack, a_truncated_pack_is_a_warning_not_a_crash) {
    std::vector<uint8_t> full = tiny_pack(1, false, false);
    std::vector<uint8_t> cut(full.begin(), full.begin() + 40);
    CHECK(!parse_pack(cut, 1, 1).has_value());
    CHECK(!parse_pack({}, 1, 1).has_value());
}

CM_TEST(npc_character, reads_weapons_skills_race_and_modifier) {
    PackSet set;
    set.packs.push_back(make_char_pack(
        {{9, 0, 4242}, {35, 700, 76158}, {64, 800, 5491}},
        {{200, 700}, {120, 800}},
        {{632, pack_token("humanfemale")}, {88, pack_token("undead")}},
        {{700 + 44, 24}}));
    set.finish();
    const Object* c = set.packs[0].object_at(0);
    CHECK(c != nullptr);
    CharacterFields f = read_character(set, 0, *c);
    CHECK_EQ(f.id, 4242u);
    CHECK_EQ(f.weapons.size(), size_t(1));
    CHECK_EQ(f.weapons[0].first, 200u);
    CHECK_EQ(f.weapons[0].second, 76158u);
    CHECK_EQ(f.skills.size(), size_t(1));
    CHECK_EQ(f.skills[0], 5491u);
    CHECK_EQ(f.race, std::string("humanfemale"));
    CHECK_EQ(f.modifier, std::string("undead"));
}

CM_TEST(npc_character, drops_a_slot_ref_whose_target_has_the_wrong_type) {
    PackSet set;
    set.packs.push_back(make_char_pack(
        {{9, 0, 1}, {140, 700, 2}, {35, 800, 3}},
        {{192, 700}, {168, 800}}, {}, {{800 + 44, 24}}));
    set.finish();
    CharacterFields f = read_character(set, 0, *set.packs[0].object_at(0));
    CHECK(f.weapons.empty());
    CHECK(f.skills.empty());
}

CM_TEST(npc_character, a_non_weapon_item_is_not_a_weapon) {
    PackSet set;
    set.packs.push_back(make_char_pack({{9, 0, 1}, {35, 700, 2}}, {{200, 700}}, {}, {{700 + 44, 0}}));
    set.finish();
    CharacterFields f = read_character(set, 0, *set.packs[0].object_at(0));
    CHECK(f.weapons.empty());
}
