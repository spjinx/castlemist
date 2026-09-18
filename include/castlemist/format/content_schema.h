#ifndef CONTENT_SCHEMA_H
#define CONTENT_SCHEMA_H

// Typed field decoding for GW2's "cntc" / PackContent content database --
// the layer above content_map.h's flat (type,id)->fileId index. Where
// content_map answers "what asset does this chat-link id point at", this
// answers "what IS this content object": item type/rarity/level/armor,
// skin/map asset-reference slots, map codename/region text.
//
// Reverse-engineered from spjinx/t3d's independently-derived PackContent
// schema (parser/definitions/MAIN_4.ts for the chunk layout; the Filename()
// compressed-fileref formula in parser/src/data-parser.ts; and
// library/src/cntc/schemas/{items,skins,maps}.ts for the per-type field
// offsets), cross-checked against this project's own cntc walk in
// content_map.cpp everywhere the two overlap (the "Main" chunk header size,
// the indexEntries/fileIndices array shapes, and the object header's
// embeddedType@+16).
//
// PackContent "Main" chunk: a u32 `flags` field, then 10 arrays of
// {u32 count, i64 self-relative pointer} (content_map.cpp calls this
// pattern out as "11 arrays" -- flags plus the 10 below):
//   0 typeInfos        {guidOffset,uidOffset,dataIdOffset,nameOffset:u32,trackReferences:u8}
//   1 namespaces       {name:RefString16, domain,parentIndex:u32}
//   2 fileRefs         array of `Fileref` (self-relative ptr -> compressed fileId, see decode_fileref_pair)
//   3 indexEntries     {type,offset,namespaceIndex,rootIndex:u32}          (16 B; content_map.cpp already reads this)
//   4 localOffsets     {relocOffset:u32}
//   5 externalOffsets  {relocOffset,targetFileIndex:u32}                  (cross-file fixups, e.g. item -> skin)
//   6 fileIndices      {relocOffset:u32}                                  (content_map.cpp already reads this)
//   7 stringIndices    {relocOffset:u32}
//   8 trackedReferences{sourceOffset,targetFileIndex,targetOffset:u32}
//   9 strings          array of RefString16 (self-relative ptr -> UTF-16LE, NUL-terminated)
//  10 content          raw bytes                                         (content_map.cpp already reads this)
//
// Arrays 0, 1, 4, 7, 8 are not decoded here: nothing in this pass (or in
// t3d's own schema code) resolves an object's field offsets through
// typeInfos at runtime -- both projects use offsets empirically observed
// per content type instead -- so parsing their exact struct stride isn't
// needed yet and would be a guess without a live capture to check it
// against.
//
// An object's TYPE comes from its PackContentIndexEntry (array 3) itself --
// {type, offset, namespaceIndex, rootIndex}, all u32, offset at +4 -- NOT
// from `content + 16`, which castlemist::extract::parse_cntc_objects()
// (src/extract/content_store.cpp) found only *coincidentally* agrees for
// some objects when measured against a live cntc: 103 clean type values via
// the indexEntry vs 6573 via content+16. get_objects() below follows that
// project-verified finding.
//
// An object's numeric id is the u32 at content-relative +20. This is the
// one field two independent parts of this codebase already agree on against
// real data: content_map.h's docstring says it was empirically validated
// against real chat links, and content_store.cpp's live-cntc-measured
// parse_cntc_objects() reads the same offset. unique_id() below is that
// field; spjinx/t3d's schema additionally names a "dataId" at +40, but nothing
// in this codebase corroborates it, so it is not exposed here.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace castlemist::cschema {

constexpr uint32_t CONTENT_TYPE_ITEMS = 35;
constexpr uint32_t CONTENT_TYPE_MAPS = 45;
constexpr uint32_t CONTENT_TYPE_SKINS = 66;

struct IndexEntry {
    uint32_t type = 0;    // @+0
    uint32_t offset = 0;  // @+4 -- object's begin offset within ContentPack::content
};

struct ExternalOffsetFixup {
    uint32_t reloc_offset = 0;
    uint32_t target_file_index = 0;
};

struct FileIndexFixup {
    uint32_t reloc_offset = 0;
};

/// One fully decoded "Main" / PackContent chunk (already CRC32-stripped and
/// method0-decompressed -- exactly what content_map.cpp's internal
/// decompress() produces).
struct ContentPack {
    std::vector<uint32_t> file_refs;                    // array 2, already decoded to real fileIds
    std::vector<IndexEntry> index_entries;               // array 3
    std::vector<ExternalOffsetFixup> external_offsets;   // array 5
    std::vector<FileIndexFixup> file_indices;            // array 6
    std::vector<std::wstring> strings;                   // array 9
    std::vector<uint8_t> content;                        // array 10
};

/// One content object's [begin,end) slice within a ContentPack::content.
/// `bytes` aliases into the owning ContentPack -- keep the pack alive.
struct ContentObject {
    uint32_t type = 0;  // from its IndexEntry, not content+16 -- see header comment
    uint32_t begin = 0, end = 0;
    std::span<const uint8_t> bytes;

    uint32_t unique_id() const;  // @+20 -- see header comment
};

/// Decodes GW2's compressed Fileref/Filename pointer pair -- two u16 halves,
/// each biased by 0x100 -- at `data[at]`. Returns 0 for a terminator/unset
/// slot (either half < 0x100).
uint32_t decode_fileref_pair(std::span<const uint8_t> data, size_t at);

/// Parses one decompressed cntc "Main" chunk. nullopt if it isn't one.
std::optional<ContentPack> parse_content_pack(std::span<const uint8_t> decompressed);

std::vector<ContentObject> get_objects(const ContentPack& pack);
std::vector<ContentObject> get_objects_of_type(const ContentPack& pack, uint32_t type);

/// Resolves an asset-reference field: reads a u32 index at `field_offset`
/// within `object.bytes` and looks it up in `pack.file_refs`. nullopt if the
/// index is out of range or the slot decoded to 0 (unset).
std::optional<uint32_t> resolve_asset_ref(const ContentPack& pack, const ContentObject& object, size_t field_offset);

/// Human label for a discovered asset-reference offset within a content
/// object of the given type (from spjinx/t3d's per-type assetReference()
/// tables) -- "file ref" if nothing more specific is known.
const char* asset_ref_label(uint32_t content_type, uint32_t offset);

// ---------------------------------------------------------------- Items --
enum class ItemType : uint32_t {
    Armor = 0, Back = 2, Bag = 3, Consumable = 4, Container = 5, CraftingMaterial = 6,
    Gathering = 9, Gizmo = 10, JadeTechModule = 11, MiniPet = 15, PowerCore = 17,
    Relic = 18, Tool = 19, Trinket = 21, Trophy = 22, UpgradeComponent = 23, Weapon = 24,
};
enum class ItemRarity : uint32_t {
    Junk = 0, Basic = 1, Fine = 2, Masterwork = 3, Rare = 4, Exotic = 5, Ascended = 6,
};
enum class ArmorSlot : uint32_t { Coat = 0, Leggings = 1, Gloves = 2, Helm = 3, HelmAquatic = 4, Boots = 5, Shoulders = 6 };
enum class ArmorWeightClass : uint32_t { Light = 1, Medium = 2, Heavy = 3 };

/// nullptr if `value` isn't one of the known enumerators above.
const char* to_string(ItemType value);
const char* to_string(ItemRarity value);
const char* to_string(ArmorSlot value);
const char* to_string(ArmorWeightClass value);

struct ItemFields {
    uint32_t item_type_raw = 0;
    uint32_t rarity_raw = 0;
    uint32_t level = 0;
    std::optional<uint32_t> armor_slot_raw;         // only decoded when item_type_raw == Armor (0)
    std::optional<uint32_t> armor_weight_class_raw; // only decoded when item_type_raw == Armor (0)
};

/// Field offsets: itemType@44, rarity@96, level@116, armorSlot@184,
/// armorWeightClass@280 (the last two only meaningful for Armor).
ItemFields decode_item_fields(const ContentObject& item);

// ---------------------------------------------------------------- Maps ---
struct MapFields {
    uint32_t map_data_file_ref_index = 0;  // index into ContentPack::file_refs
    std::wstring codename;                 // resolved via the owning pack's string table
    std::wstring region;
};

/// Field offsets: mapDataFileRefIndex@104, codename string index@176,
/// region string index@272. `owning_pack` supplies the string table.
MapFields decode_map_fields(const ContentObject& map, const ContentPack& owning_pack);

// ------------------------------------------------- Cross-file: item->skin --
struct SkinResolution {
    uint32_t skin_base_id = 0;
    uint32_t skin_id = 0;  // the resolved skin object's unique_id() (@+20)
};

/// Walks an item's externalOffsets fixups to find its skin's content object,
/// mirroring spjinx/t3d's CntcResolver.resolveSkinReference: GW2 shares one
/// fileRefs table across a contiguous run of small per-object cntc entries,
/// so the item's own baseId is not necessarily where that table lives -- an
/// "anchor" pack is found by walking backward from `item_base_id` (at most
/// 64 entries) for the nearest baseId whose own pack declares file_refs.
///
/// `load_pack_bytes(base_id)` should decompress and return the raw cntc
/// bytes for that baseId (nullopt if not found) -- wire this to whatever
/// baseId/dat access the caller already has (e.g. castlemist::db's index +
/// read_entry_bytes()). Not cached here: a caller resolving many items
/// should memoize `load_pack_bytes` itself, since neighbouring items likely
/// share the same anchor.
std::optional<SkinResolution> resolve_item_skin(
    uint32_t item_base_id, const ContentPack& item_pack, const ContentObject& item,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t base_id)>& load_pack_bytes);

} // namespace castlemist::cschema

#endif // CONTENT_SCHEMA_H
