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

#include "castlemist/format/content_schema.h"

#include <cstdint>
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
