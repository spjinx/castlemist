/// @file
/// @brief Tests for the cntc/PackContent typed field decoder (content_schema.h).
///
/// There's no real Gw2.dat to pull a "Main" chunk from here, so these hand-
/// assemble the smallest possible packfile that satisfies the layout content_
/// schema.h's header comment describes: a PF/"cntc" wrapper, a "Main" chunk,
/// the flags + 11 array descriptors, and just enough payload in each array a
/// given test actually reads. PackBuilder below places those arrays at named
/// offsets so each test can point at the field it cares about by name instead
/// of a raw byte position.

#include "test_framework.h"

#include "castlemist/format/content_map.h"
#include "castlemist/format/content_schema.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace castlemist::cschema;

namespace {

// PF header (2) .. "cntc" tag (@8) .. "Main" chunk (@12, 8-byte chunk header +
// 8-byte inner sub-header, unused here) .. flags (@28) .. 11 array
// descriptors of {u32 count, i64 self-relative pointer} (@32, 12 bytes each).
constexpr size_t kChunkPos = 12;
constexpr size_t kBase = kChunkPos + 16;      // 28
constexpr size_t kArrDescBase = kBase + 4;    // 32
constexpr size_t kArrDescEnd = kArrDescBase + 11 * 12;  // 164: first free byte after the descriptor block

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

    /// @brief Sets array descriptor `i`'s count and self-relative pointer to
    /// `target` (an absolute position in `d`). count == 0 leaves the pointer
    /// at 0 (an empty array), matching how the real format marks "unused".
    void set_dynarray(int i, uint32_t count, size_t target) {
        size_t p = arr_desc(i);
        put_u32(p, count);
        if (count != 0) put_i64(p + 4, static_cast<int64_t>(target) - static_cast<int64_t>(p + 4));
    }

    PackBuilder() {
        put_bytes(0, {'P', 'F'});
        put_u32(6, kChunkPos);  // put_u32 writes 4 bytes; only the low u16 (offset 6-7) is read as `pos`
        put_bytes(8, {'c', 'n', 't', 'c'});
        put_bytes(kChunkPos, {'M', 'a', 'i', 'n'});
        put_u32(kChunkPos + 4, 1000);  // chunk size; never consulted once "Main" matches on the first try
        put_u32(kBase, 0);             // flags
        for (int i = 0; i <= 10; ++i) set_dynarray(i, 0, 0);  // start every array empty; tests fill in what they need
    }
};

} // namespace

CM_TEST(content_schema, decode_fileref_pair_reconstructs_a_biased_two_u16_id) {
    // low=0x02F3 (755), high=0x0100 (256): filename = 0xff00*0 + (755-256) + 1 = 500.
    std::vector<uint8_t> pair{0xF3, 0x02, 0x00, 0x01};
    CHECK_EQ(decode_fileref_pair(pair, 0), 500u);
}

CM_TEST(content_schema, decode_fileref_pair_treats_a_sub_256_half_as_a_terminator) {
    std::vector<uint8_t> unset{0x00, 0x00, 0x00, 0x00};
    CHECK_EQ(decode_fileref_pair(unset, 0), 0u);
}

CM_TEST(content_schema, parses_item_fields_and_resolves_its_icon_through_file_refs) {
    PackBuilder b;

    // fileRefs (array 2): two Fileref pointers at kArrDescEnd, then their
    // {lowPart,highPart} pairs right after -- fileRefs[0] = 500, [1] = 777.
    size_t frPtrs = kArrDescEnd;             // 164
    size_t frPairs = frPtrs + 2 * 8;         // 180
    b.put_i64(frPtrs + 0, static_cast<int64_t>(frPairs) - static_cast<int64_t>(frPtrs));            // -> 180
    b.put_i64(frPtrs + 8, static_cast<int64_t>(frPairs + 4) - static_cast<int64_t>(frPtrs + 8));    // -> 184
    b.put_bytes(frPairs + 0, {0xF3, 0x02, 0x00, 0x01});  // fileId 500
    b.put_bytes(frPairs + 4, {0x08, 0x04, 0x00, 0x01});  // fileId 777
    b.set_dynarray(2, 2, frPtrs);

    // indexEntries (array 3): one object, type@+0, beginning at content-relative
    // offset 0 (the default @+4 -- PackBuilder zero-fills).
    size_t ieTable = frPairs + 8;  // 188
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);

    // content (array 10): one Items object, 288 bytes.
    size_t cOff = 300;
    uint32_t cLen = 288;
    b.put_u32(cOff + 44, static_cast<uint32_t>(ItemType::Armor));
    b.put_u32(cOff + 64, 0);   // icon field: fileRefs[0]
    b.put_u32(cOff + 96, static_cast<uint32_t>(ItemRarity::Exotic));
    b.put_u32(cOff + 116, 80);  // level
    b.put_u32(cOff + 184, static_cast<uint32_t>(ArmorSlot::Coat));
    b.put_u32(cOff + 280, static_cast<uint32_t>(ArmorWeightClass::Heavy));
    b.ensure(cOff + cLen);
    b.set_dynarray(10, cLen, cOff);

    auto pack = parse_content_pack(b.d);
    CHECK(pack.has_value());
    CHECK_EQ(pack->file_refs.size(), size_t{2});
    CHECK_EQ(pack->file_refs[0], 500u);
    CHECK_EQ(pack->file_refs[1], 777u);

    std::vector<ContentObject> items = get_objects_of_type(*pack, CONTENT_TYPE_ITEMS);
    CHECK_EQ(items.size(), size_t{1});
    const ContentObject& item = items[0];
    CHECK_EQ(item.type, CONTENT_TYPE_ITEMS);

    ItemFields fields = decode_item_fields(item);
    CHECK_EQ(fields.item_type_raw, static_cast<uint32_t>(ItemType::Armor));
    CHECK_EQ(fields.rarity_raw, static_cast<uint32_t>(ItemRarity::Exotic));
    CHECK_EQ(fields.level, 80u);
    CHECK(fields.armor_slot_raw.has_value());
    CHECK_EQ(*fields.armor_slot_raw, static_cast<uint32_t>(ArmorSlot::Coat));
    CHECK(fields.armor_weight_class_raw.has_value());
    CHECK_EQ(*fields.armor_weight_class_raw, static_cast<uint32_t>(ArmorWeightClass::Heavy));

    CHECK_EQ(std::string(to_string(ItemType::Armor)), std::string("Armor"));
    CHECK_EQ(std::string(to_string(ItemRarity::Exotic)), std::string("Exotic"));
    CHECK_EQ(std::string(asset_ref_label(CONTENT_TYPE_ITEMS, 64)), std::string("icon"));

    std::optional<uint32_t> icon = resolve_asset_ref(*pack, item, 64);
    CHECK(icon.has_value());
    CHECK_EQ(*icon, 500u);  // regression check for the idx==0-is-not-"unset" fix
}

CM_TEST(content_schema, non_armor_items_do_not_decode_armor_only_fields) {
    PackBuilder b;
    size_t cOff = 300;
    b.put_u32(cOff + 44, static_cast<uint32_t>(ItemType::Weapon));
    b.ensure(cOff + 288);
    size_t ieTable = kArrDescEnd;
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    b.set_dynarray(10, 288, cOff);

    auto pack = parse_content_pack(b.d);
    CHECK(pack.has_value());
    ItemFields fields = decode_item_fields(get_objects_of_type(*pack, CONTENT_TYPE_ITEMS)[0]);
    CHECK_EQ(fields.item_type_raw, static_cast<uint32_t>(ItemType::Weapon));
    CHECK_FALSE(fields.armor_slot_raw.has_value());
    CHECK_FALSE(fields.armor_weight_class_raw.has_value());
}

CM_TEST(content_schema, resolves_an_item_to_its_skin_across_a_shared_anchor_pack) {
    PackBuilder b;

    // fileRefs: non-empty just so this pack qualifies as its own "anchor"
    // (resolve_item_skin's pack-search accepts the first pack it finds with
    // a non-empty file_refs table).
    size_t frPtrs = kArrDescEnd;
    size_t frPairs = frPtrs + 8;
    b.put_i64(frPtrs, static_cast<int64_t>(frPairs) - static_cast<int64_t>(frPtrs));
    b.put_bytes(frPairs, {0xF3, 0x02, 0x00, 0x01});  // fileId 500, unused by this test
    b.set_dynarray(2, 1, frPtrs);

    // indexEntries: two objects -- item @ content-relative 0, skin @ 288.
    size_t ieTable = frPairs + 4;
    b.ensure(ieTable + 32);
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);          // entry[0].type
    b.put_u32(ieTable + 16, CONTENT_TYPE_SKINS);     // entry[1].type
    b.put_u32(ieTable + 16 + 4, 288);                // entry[1].offset
    b.set_dynarray(3, 2, ieTable);

    // externalOffsets: one fixup inside the item object, at +176 (exactly
    // ITEM_SKIN_FIXUP_HINT, so it's the closest -- and only -- candidate),
    // pointing at file index 0 (this same pack, since target_base_id =
    // anchor_base_id + target_file_index).
    size_t eoTable = ieTable + 32;
    b.ensure(eoTable + 8);
    b.put_u32(eoTable + 0, 176);  // reloc_offset
    b.put_u32(eoTable + 4, 0);    // target_file_index
    b.set_dynarray(5, 1, eoTable);

    // content: item [0,288) references skin-object-offset 288 at its own
    // +176; skin [288,336) carries unique_id 9999 at its own +20.
    size_t cOff = 300;
    uint32_t cLen = 336;
    b.put_u32(cOff + 176, 288);  // the raw offset the skin fixup points at
    b.put_u32(cOff + 288 + 20, 9999);  // skin's unique_id
    b.ensure(cOff + cLen);
    b.set_dynarray(10, cLen, cOff);

    auto pack = parse_content_pack(b.d);
    CHECK(pack.has_value());
    std::vector<ContentObject> items = get_objects_of_type(*pack, CONTENT_TYPE_ITEMS);
    CHECK_EQ(items.size(), size_t{1});

    const uint32_t kItemBaseId = 5000;
    std::vector<uint8_t> saved = b.d;  // load_pack_bytes hands back the same bytes for its own baseId
    auto load = [&](uint32_t base_id) -> std::optional<std::vector<uint8_t>> {
        return base_id == kItemBaseId ? std::optional<std::vector<uint8_t>>(saved) : std::nullopt;
    };

    std::optional<SkinResolution> skin = resolve_item_skin(kItemBaseId, *pack, items[0], load);
    CHECK(skin.has_value());
    CHECK_EQ(skin->skin_base_id, kItemBaseId);
    CHECK_EQ(skin->skin_id, 9999u);
}

CM_TEST(content_schema, item_with_no_external_offsets_resolves_no_skin) {
    PackBuilder b;
    size_t cOff = 300;
    b.ensure(cOff + 288);
    size_t ieTable = kArrDescEnd;
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    b.set_dynarray(10, 288, cOff);

    auto pack = parse_content_pack(b.d);
    CHECK(pack.has_value());
    auto load = [](uint32_t) -> std::optional<std::vector<uint8_t>> { return std::nullopt; };
    std::optional<SkinResolution> skin =
        resolve_item_skin(5000, *pack, get_objects_of_type(*pack, CONTENT_TYPE_ITEMS)[0], load);
    CHECK_FALSE(skin.has_value());
}

// ---- content_map (castlemist::cmap) ----------------------------------------
// Lives here to reuse PackBuilder. Mirrors the real layout measured against a
// live Gw2.dat (Astralaria: API skin 6506 is the cntc object with dataId@+40 ==
// 6506 and uid@+20 == 13497; its +48 model slot holds fileRefs index 27344,
// not a fileId, and only one pack in the whole datastore carries fileRefs).

namespace {

// One Skins object (dataId@+40, uid@+20) whose +48 slot is a fileIndices reloc
// holding `ref_index` -- an index into the shared fileRefs table.
std::vector<uint8_t> skin_pack(uint32_t uid, uint32_t data_id, uint32_t ref_index) {
    PackBuilder b;
    size_t ieTable = kArrDescEnd;
    b.put_u32(ieTable, CONTENT_TYPE_SKINS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    size_t fiTable = ieTable + 16;
    b.put_u32(fiTable, 48);  // reloc: content-relative offset of the model slot
    b.set_dynarray(6, 1, fiTable);
    size_t cOff = 300;
    b.put_u32(cOff + 16, CONTENT_TYPE_SKINS);
    b.put_u32(cOff + 20, uid);
    b.put_u32(cOff + 40, data_id);
    b.put_u32(cOff + 48, ref_index);
    b.ensure(cOff + 128);
    b.set_dynarray(10, 128, cOff);
    return b.d;
}

// A full-size (312-byte) skin object carrying a composite appearance token at +208.
std::vector<uint8_t> skin_pack_with_token(uint32_t data_id, uint32_t ref_index, uint64_t token) {
    PackBuilder b;
    size_t ieTable = kArrDescEnd;
    b.put_u32(ieTable, CONTENT_TYPE_SKINS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    size_t fiTable = ieTable + 16;
    b.put_u32(fiTable, 48);
    b.set_dynarray(6, 1, fiTable);
    size_t cOff = 300;
    b.put_u32(cOff + 16, CONTENT_TYPE_SKINS);
    b.put_u32(cOff + 40, data_id);
    b.put_u32(cOff + 48, ref_index);
    b.put_i64(cOff + 208, static_cast<int64_t>(token));
    b.ensure(cOff + 312);
    b.set_dynarray(10, 312, cOff);
    return b.d;
}

// A pack carrying only the shared fileRefs table: [0] = 500, [1] = 777.
std::vector<uint8_t> file_refs_pack() {
    PackBuilder b;
    size_t frPtrs = kArrDescEnd;
    size_t frPairs = frPtrs + 2 * 8;
    b.put_i64(frPtrs + 0, static_cast<int64_t>(frPairs) - static_cast<int64_t>(frPtrs));
    b.put_i64(frPtrs + 8, static_cast<int64_t>(frPairs + 4) - static_cast<int64_t>(frPtrs + 8));
    b.put_bytes(frPairs + 0, {0xF3, 0x02, 0x00, 0x01});  // fileId 500
    b.put_bytes(frPairs + 4, {0x08, 0x04, 0x00, 0x01});  // fileId 777
    b.set_dynarray(2, 2, frPtrs);
    size_t ieTable = frPairs + 8;
    b.put_u32(ieTable, 1);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    b.ensure(400 + 64);
    b.set_dynarray(10, 64, 400);
    return b.d;
}

// One Items object (dataId@+40) whose +176 skin slot is an externalOffsets
// fixup to file index `skin_file_index`, object offset `skin_offset`; its +64
// icon slot is fileRefs index 0.
std::vector<uint8_t> item_pack(uint32_t data_id, uint32_t skin_file_index, uint32_t skin_offset) {
    PackBuilder b;
    size_t ieTable = kArrDescEnd;
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);
    b.ensure(ieTable + 16);
    b.set_dynarray(3, 1, ieTable);
    size_t eoTable = ieTable + 16;
    b.put_u32(eoTable + 0, 176);
    b.put_u32(eoTable + 4, skin_file_index);
    b.set_dynarray(5, 1, eoTable);
    size_t fiTable = eoTable + 8;
    b.put_u32(fiTable, 64);
    b.set_dynarray(6, 1, fiTable);
    size_t cOff = 300;
    b.put_u32(cOff + 16, CONTENT_TYPE_ITEMS);
    b.put_u32(cOff + 40, data_id);
    b.put_u32(cOff + 64, 0);
    b.put_u32(cOff + 176, skin_offset);
    b.ensure(cOff + 288);
    b.set_dynarray(10, 288, cOff);
    return b.d;
}

// dataId of the first item_links() entry of `type`, 0 if none.
uint32_t first_link(uint32_t item_id, uint32_t type) {
    for (const auto& l : castlemist::cmap::item_links(item_id))
        if (l.type == type) return l.id;
    return 0;
}

// One pack of objects wired by in-pack (localOffsets) pointers. Each object is
// {type, dataId}; each link is {from object, field, to object}.
struct LocalGraph {
    struct Obj { uint32_t type, data_id; };
    struct Link { size_t from; uint32_t field; size_t to; };
    std::vector<Obj> objs;
    std::vector<Link> links;

    std::vector<uint8_t> build() const {
        constexpr uint32_t kObjSize = 288;
        PackBuilder b;
        size_t ieTable = kArrDescEnd;
        b.ensure(ieTable + 16 * objs.size());
        for (size_t i = 0; i < objs.size(); ++i) {
            b.put_u32(ieTable + 16 * i, objs[i].type);
            b.put_u32(ieTable + 16 * i + 4, static_cast<uint32_t>(i * kObjSize));
        }
        b.set_dynarray(3, static_cast<uint32_t>(objs.size()), ieTable);
        size_t loTable = ieTable + 16 * objs.size();
        for (size_t i = 0; i < links.size(); ++i)
            b.put_u32(loTable + 4 * i, static_cast<uint32_t>(links[i].from * kObjSize + links[i].field));
        b.set_dynarray(4, static_cast<uint32_t>(links.size()), loTable);
        size_t cOff = loTable + 4 * links.size() + 16;
        for (size_t i = 0; i < objs.size(); ++i) {
            b.put_u32(cOff + i * kObjSize + 16, objs[i].type);
            b.put_u32(cOff + i * kObjSize + 40, objs[i].data_id);
        }
        for (const Link& l : links)
            b.put_u32(cOff + l.from * kObjSize + l.field, static_cast<uint32_t>(l.to * kObjSize));
        uint32_t cLen = static_cast<uint32_t>(objs.size() * kObjSize);
        b.ensure(cOff + cLen);
        b.set_dynarray(10, cLen, cOff);
        return b.d;
    }
};

} // namespace

CM_TEST(content_map, keys_by_data_id_and_resolves_refs_through_the_shared_file_refs_table) {
    namespace cmap = castlemist::cmap;
    cmap::clear();
    // Object pack first, fileRefs pack second: resolution must not depend on order.
    cmap::build_from_packs({{20, 0, skin_pack(13497, 6506, 1)}, {10, 0, file_refs_pack()}});

    const std::vector<uint32_t>& fids = cmap::resolve_all(cmap::CONTENT_TYPE_SKIN, 6506);
    CHECK_EQ(fids.size(), size_t{1});
    if (!fids.empty()) CHECK_EQ(fids[0], 777u);  // fileRefs[1], not the raw index 1
    CHECK(cmap::resolve_all(cmap::CONTENT_TYPE_SKIN, 13497).empty());  // +20 uid is not the chat-link id
    CHECK_EQ(cmap::content_base_id(cmap::CONTENT_TYPE_SKIN, 6506), 20u);
    cmap::clear();
}

CM_TEST(content_map, follows_an_items_skin_link_by_file_id_order_from_the_file_refs_pack) {
    namespace cmap = castlemist::cmap;
    cmap::clear();
    // fileIds 900 (fileRefs), 901 (skin), 902 (item); baseIds deliberately out of
    // order -- the link's file index counts fileIds, not baseIds.
    cmap::build_from_packs({{7, 902, item_pack(76158, 1, 0)},
                            {50, 901, skin_pack(13497, 6506, 1)},
                            {30, 900, file_refs_pack()}});

    CHECK_EQ(first_link(76158, cmap::CONTENT_TYPE_SKIN), 6506u);
    const std::vector<uint32_t>& icon = cmap::resolve_all(cmap::CONTENT_TYPE_ITEM, 76158);
    CHECK_EQ(icon.size(), size_t{1});
    if (!icon.empty()) CHECK_EQ(icon[0], 500u);
    CHECK_EQ(cmap::resolve(cmap::CONTENT_TYPE_SKIN, first_link(76158, cmap::CONTENT_TYPE_SKIN)), 777u);
    cmap::clear();
}

CM_TEST(content_map, follows_an_items_skin_link_to_a_skin_in_the_same_pack) {
    // Claw of the Khan-Ur (item 87109 -> skin 8051): skin in the item's own pack,
    // so +176 is a localOffsets pointer to the skin object, not an external fixup.
    namespace cmap = castlemist::cmap;
    cmap::clear();
    PackBuilder b;
    size_t ieTable = kArrDescEnd;
    b.ensure(ieTable + 32);
    b.put_u32(ieTable, CONTENT_TYPE_ITEMS);
    b.put_u32(ieTable + 16, CONTENT_TYPE_SKINS);
    b.put_u32(ieTable + 16 + 4, 288);  // skin object at content offset 288
    b.set_dynarray(3, 2, ieTable);
    size_t loTable = ieTable + 32;
    b.put_u32(loTable, 176);  // localOffsets: item+176 is an in-pack pointer
    b.set_dynarray(4, 1, loTable);
    size_t fiTable = loTable + 4;
    b.put_u32(fiTable, 288 + 48);  // skin's model slot
    b.set_dynarray(6, 1, fiTable);
    size_t cOff = 400;
    b.put_u32(cOff + 16, CONTENT_TYPE_ITEMS);
    b.put_u32(cOff + 40, 87109);
    b.put_u32(cOff + 176, 288);
    b.put_u32(cOff + 288 + 16, CONTENT_TYPE_SKINS);
    b.put_u32(cOff + 288 + 40, 8051);
    b.put_u32(cOff + 288 + 48, 1);
    b.ensure(cOff + 416);
    b.set_dynarray(10, 416, cOff);

    cmap::build_from_packs({{5, 0, b.d}, {10, 0, file_refs_pack()}});  // no fileIds needed in-pack
    CHECK_EQ(first_link(87109, cmap::CONTENT_TYPE_SKIN), 8051u);
    CHECK_EQ(cmap::resolve(cmap::CONTENT_TYPE_SKIN, 8051), 777u);
    cmap::clear();
}

CM_TEST(content_map, an_unlock_item_links_to_its_mount_skin) {
    // Dark Monarch Skyscale Skin (item 93703): +264 points at mount skin 292.
    namespace cmap = castlemist::cmap;
    cmap::clear();
    LocalGraph g;
    g.objs = {{CONTENT_TYPE_ITEMS, 93703}, {cmap::CONTENT_TYPE_MOUNT_SKIN, 292}};
    g.links = {{0, 264, 1}};
    cmap::build_from_packs({{5, 0, g.build()}, {10, 0, file_refs_pack()}});
    CHECK_EQ(first_link(93703, cmap::CONTENT_TYPE_MOUNT_SKIN), 292u);
    cmap::clear();
}

CM_TEST(content_map, a_container_item_links_to_the_skins_of_the_items_inside_it) {
    // Holographic Dragon Helm (container item 91359): +248 -> contents list ->
    // the heavy/light helm items -> their skins.
    namespace cmap = castlemist::cmap;
    cmap::clear();
    LocalGraph g;
    g.objs = {{CONTENT_TYPE_ITEMS, 91359}, {cmap::CONTENT_TYPE_CONTAINER, 0},
              {CONTENT_TYPE_ITEMS, 91284}, {CONTENT_TYPE_ITEMS, 91357},
              {CONTENT_TYPE_SKINS, 8826},  {CONTENT_TYPE_SKINS, 8817}};
    g.links = {{0, 248, 1}, {1, 64, 2}, {1, 88, 3}, {2, 176, 4}, {3, 176, 5}};
    cmap::build_from_packs({{5, 0, g.build()}, {10, 0, file_refs_pack()}});

    const auto& links = cmap::item_links(91359);
    CHECK_EQ(links.size(), size_t{2});
    if (links.size() == 2) {
        CHECK_EQ(links[0].id, 8826u);
        CHECK_EQ(links[0].via_item, 91284u);
        CHECK_EQ(links[1].id, 8817u);
        CHECK_EQ(links[1].via_item, 91357u);
    }
    CHECK_EQ(first_link(91284, cmap::CONTENT_TYPE_SKIN), 8826u);  // the inner items still resolve directly
    cmap::clear();
}

CM_TEST(content_map, cmap_skin_token_recorded) {
    namespace cmap = castlemist::cmap;
    cmap::clear();
    cmap::build_from_packs({{20, 0, skin_pack_with_token(517, 1, 0x00000348C28A32A3ull)}, {10, 0, file_refs_pack()}});
    CHECK_EQ(cmap::skin_token(517), 0x00000348C28A32A3ull);
    CHECK_EQ(cmap::skin_token(999), 0ull);

    std::wstring path = (std::filesystem::temp_directory_path() / "cm_test_cmap_token.bin").wstring();
    CHECK(cmap::save(path));
    cmap::clear();
    CHECK_EQ(cmap::skin_token(517), 0ull);
    CHECK(cmap::load(path));
    CHECK_EQ(cmap::skin_token(517), 0x00000348C28A32A3ull);
    cmap::clear();
    std::filesystem::remove(path);
}

namespace {

// A colour object (type 9) at content offset `o` of a pack: dataId, one material
// shift stored the game's way (floats x128, brightness +128) inline at +80.
void put_color(PackBuilder& b, size_t c_off, uint32_t o, uint32_t data_id, float brightness, float hue) {
    auto put_f = [&](size_t pos, float v) {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        b.put_u32(pos, u);
    };
    b.put_u32(c_off + o + 16, castlemist::cmap::CONTENT_TYPE_COLOR);
    b.put_u32(c_off + o + 40, data_id);
    b.put_i64(c_off + o + 48, o + 80);  // absolute content offset of the material array
    b.put_u32(c_off + o + 56, 1);
    put_f(c_off + o + 80, brightness + 128);
    put_f(c_off + o + 84, 1.5f * 128);
    put_f(c_off + o + 88, hue);
    put_f(c_off + o + 92, 0.25f * 128);
    put_f(c_off + o + 96, 2.0f * 128);
}

} // namespace

CM_TEST(content_map, palettes_resolve_local_and_external_colours) {
    namespace cmap = castlemist::cmap;
    cmap::clear();
    // Pack 902: palette (uid 70, base RGB 128,26,26) at 0 listing its own colour
    // at 288 and an external one at offset 0 of file index 1 (fileId 901).
    PackBuilder p;
    size_t ie = kArrDescEnd;
    p.put_u32(ie, cmap::CONTENT_TYPE_PALETTE);
    p.put_u32(ie + 16, cmap::CONTENT_TYPE_COLOR);
    p.put_u32(ie + 20, 288);
    p.set_dynarray(3, 2, ie);
    size_t eo = ie + 32;
    p.put_u32(eo, 64 + 24);  // the second entry's pointer is external
    p.put_u32(eo + 4, 1);
    p.set_dynarray(5, 1, eo);
    size_t c = 400;
    p.put_u32(c + 16, cmap::CONTENT_TYPE_PALETTE);
    p.put_u32(c + 20, 70);
    p.put_bytes(c + 40, {26, 26, 128});  // BGR
    p.put_i64(c + 48, 64);
    p.put_u32(c + 56, 2);
    p.put_i64(c + 64, 288);  // local colour
    p.put_i64(c + 88, 0);    // external colour's offset in its pack
    put_color(p, c, 288, 1234, 7, 100);
    p.ensure(c + 576);
    p.set_dynarray(10, 576, c);

    PackBuilder q;  // fileId 901: the external colour
    q.put_u32(kArrDescEnd, cmap::CONTENT_TYPE_COLOR);
    q.set_dynarray(3, 1, kArrDescEnd);
    put_color(q, 300, 0, 4321, -5, 30);
    q.ensure(300 + 288);
    q.set_dynarray(10, 288, 300);

    cmap::build_from_packs({{3, 902, p.d}, {2, 901, q.d}, {1, 900, file_refs_pack()}});
    const cmap::Palette* pal = cmap::palette(70);
    CHECK(pal != nullptr);
    if (!pal) return;
    CHECK_EQ(pal->base[0], uint8_t{128});
    CHECK_EQ(pal->base[2], uint8_t{26});
    CHECK_EQ(pal->colors.size(), size_t{2});
    CHECK_EQ(pal->colors[0].id, 1234u);
    CHECK_EQ(pal->colors[1].id, 4321u);
    CHECK_NEAR(pal->colors[0].materials[0].brightness, 7.0, 1e-5);
    CHECK_NEAR(pal->colors[0].materials[0].contrast, 1.5, 1e-5);
    CHECK_NEAR(pal->colors[0].materials[0].hue, 100.0, 1e-5);
    CHECK_NEAR(pal->colors[0].materials[0].saturation, 0.25, 1e-5);
    CHECK_NEAR(pal->colors[1].materials[0].brightness, -5.0, 1e-5);

    // Survives the disk cache.
    const auto path = std::filesystem::temp_directory_path() / "cm_test_palette_cache.bin";
    CHECK(cmap::save(path.wstring()));
    cmap::clear();
    CHECK(cmap::palette(70) == nullptr);
    CHECK(cmap::load(path.wstring()));
    pal = cmap::palette(70);
    CHECK(pal != nullptr);
    if (pal) CHECK_NEAR(pal->colors[1].materials[0].hue, 30.0, 1e-5);
    std::filesystem::remove(path);
    cmap::clear();
}
