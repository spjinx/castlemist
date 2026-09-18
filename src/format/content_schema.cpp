#include "castlemist/format/content_schema.h"

#include <algorithm>
#include <cstring>

namespace castlemist::cschema {
namespace {

uint32_t read_u32(std::span<const uint8_t> bytes, size_t offset) {
    if (offset + 4 > bytes.size()) return 0;
    return bytes[offset] | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

constexpr uint32_t kItemSkinFixupHint = 176;  // t3d's ITEM_SKIN_FIXUP_HINT
constexpr uint32_t kPackSearchSpan = 64;      // t3d's PACK_SEARCH_SPAN

} // namespace

uint32_t ContentObject::unique_id() const { return read_u32(bytes, 20); }

uint32_t decode_fileref_pair(std::span<const uint8_t> data, size_t at) {
    if (at + 4 > data.size()) return 0;
    uint32_t low = data[at] | (static_cast<uint32_t>(data[at + 1]) << 8);
    uint32_t high = data[at + 2] | (static_cast<uint32_t>(data[at + 3]) << 8);
    if (low < 0x100 || high < 0x100) return 0;  // terminator / unset slot
    return 0xff00u * (high - 0x100u) + (low - 0x100u) + 1u;
}

std::optional<ContentPack> parse_content_pack(std::span<const uint8_t> d) {
    const size_t n = d.size();
    if (n < 16 || d[0] != 'P' || d[1] != 'F' || n < 12 || std::memcmp(d.data() + 8, "cntc", 4) != 0) {
        return std::nullopt;
    }
    auto u16 = [&](size_t p) -> uint32_t { return (p + 2 <= n) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto u32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= n) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | (static_cast<uint32_t>(d[p + 3]) << 24)) : 0;
    };
    auto i64 = [&](size_t p) -> int64_t {
        if (p + 8 > n) return 0;
        int64_t v;
        std::memcpy(&v, d.data() + p, 8);
        return v;
    };

    // Walk PF chunks looking for "Main" -- same walk content_map.cpp does.
    size_t pos = u16(6);
    while (pos + 8 <= n && std::memcmp(d.data() + pos, "Main", 4) != 0) {
        size_t next = pos + 8 + u32(pos + 4);
        if (next <= pos) return std::nullopt;
        pos = next;
    }
    if (pos + 16 > n) return std::nullopt;
    size_t base = pos + 16;

    // array i's {count, self-relative pointer} descriptor sits at base+4+i*12
    // (the leading 4 bytes at `base` are PackContent::flags, unused here).
    auto arr = [&](int i, size_t& elem_off) -> uint32_t {
        size_t p = base + 4 + static_cast<size_t>(i) * 12;
        elem_off = static_cast<size_t>((p + 4) + i64(p + 4));
        return u32(p);
    };

    size_t frOff = 0, ieOff = 0, eoOff = 0, fiOff = 0, strOff = 0, cOff = 0;
    uint32_t frCnt = arr(2, frOff);
    uint32_t ieCnt = arr(3, ieOff);
    uint32_t eoCnt = arr(5, eoOff);
    uint32_t fiCnt = arr(6, fiOff);
    uint32_t strCnt = arr(9, strOff);
    uint32_t cCnt = arr(10, cOff);
    if (ieCnt == 0 || cCnt == 0 || cOff + cCnt > n) return std::nullopt;

    ContentPack pack;

    pack.file_refs.reserve(frCnt);
    for (uint32_t i = 0; i < frCnt; ++i) {
        size_t p = frOff + static_cast<size_t>(i) * 8;
        int64_t rel = i64(p);
        pack.file_refs.push_back(rel == 0 ? 0u : decode_fileref_pair(d, static_cast<size_t>(p + rel)));
    }

    pack.index_entries.reserve(ieCnt);
    for (uint32_t i = 0; i < ieCnt; ++i) {
        size_t p = ieOff + static_cast<size_t>(i) * 16;
        pack.index_entries.push_back({u32(p), u32(p + 4)});
    }

    pack.external_offsets.reserve(eoCnt);
    for (uint32_t i = 0; i < eoCnt; ++i) {
        size_t p = eoOff + static_cast<size_t>(i) * 8;
        pack.external_offsets.push_back({u32(p), u32(p + 4)});
    }

    pack.file_indices.reserve(fiCnt);
    for (uint32_t i = 0; i < fiCnt; ++i) {
        pack.file_indices.push_back({u32(fiOff + static_cast<size_t>(i) * 4)});
    }

    pack.strings.reserve(strCnt);
    for (uint32_t i = 0; i < strCnt; ++i) {
        size_t p = strOff + static_cast<size_t>(i) * 8;
        int64_t rel = i64(p);
        std::wstring s;
        if (rel != 0) {
            size_t sp = static_cast<size_t>(p + rel);
            while (sp + 2 <= n) {
                uint32_t ch = u16(sp);
                if (ch == 0) break;
                s.push_back(static_cast<wchar_t>(ch));
                sp += 2;
            }
        }
        pack.strings.push_back(std::move(s));
    }

    pack.content.assign(d.begin() + static_cast<ptrdiff_t>(cOff), d.begin() + static_cast<ptrdiff_t>(cOff + cCnt));
    return pack;
}

std::vector<ContentObject> get_objects(const ContentPack& pack) {
    // Sort by offset and drop duplicate offsets (keeping the first entry seen
    // for that offset), matching castlemist::extract::parse_cntc_objects().
    std::vector<IndexEntry> entries = pack.index_entries;
    std::sort(entries.begin(), entries.end(), [](const IndexEntry& a, const IndexEntry& b) {
        return a.offset < b.offset;
    });
    entries.erase(std::unique(entries.begin(), entries.end(),
                              [](const IndexEntry& a, const IndexEntry& b) { return a.offset == b.offset; }),
                  entries.end());

    std::vector<ContentObject> out;
    out.reserve(entries.size());
    for (size_t k = 0; k < entries.size(); ++k) {
        uint32_t begin = entries[k].offset;
        uint32_t end = (k + 1 < entries.size()) ? entries[k + 1].offset : static_cast<uint32_t>(pack.content.size());
        if (begin > end || begin + 24 > pack.content.size()) continue;
        std::span<const uint8_t> bytes(pack.content.data() + begin, end - begin);
        out.push_back({entries[k].type, begin, end, bytes});
    }
    return out;
}

std::vector<ContentObject> get_objects_of_type(const ContentPack& pack, uint32_t type) {
    std::vector<ContentObject> out;
    for (auto& o : get_objects(pack)) {
        if (o.type == type) out.push_back(o);
    }
    return out;
}

std::optional<uint32_t> resolve_asset_ref(const ContentPack& pack, const ContentObject& object, size_t field_offset) {
    uint32_t idx = read_u32(object.bytes, field_offset);
    // idx == 0 is a valid fileRefs[0] index, not an "unset" sentinel -- only
    // an out-of-range index or a fileRefs slot that itself decoded to 0
    // (decode_fileref_pair's terminator case) means "no reference here".
    if (idx >= pack.file_refs.size()) return std::nullopt;
    uint32_t fid = pack.file_refs[idx];
    return fid == 0 ? std::optional<uint32_t>() : std::optional<uint32_t>(fid);
}

const char* asset_ref_label(uint32_t content_type, uint32_t offset) {
    switch (content_type) {
    case CONTENT_TYPE_SKINS:
        if (offset == 48) return "model";
        if (offset == 88) return "icon";
        if (offset >= 192 && (offset - 192) % 32 == 0) return "model variant";
        break;
    case CONTENT_TYPE_MAPS:
        if (offset == 104) return "map data";
        if (offset == 96) return "portal";
        if (offset == 88 || offset == 120) return "image";
        if (offset == 56 || offset == 64 || offset == 72 || offset == 80) return "audio";
        break;
    case CONTENT_TYPE_ITEMS:
        if (offset == 64) return "icon";
        break;
    default:
        break;
    }
    return "file ref";
}

const char* to_string(ItemType value) {
    switch (value) {
    case ItemType::Armor: return "Armor";
    case ItemType::Back: return "Back";
    case ItemType::Bag: return "Bag";
    case ItemType::Consumable: return "Consumable";
    case ItemType::Container: return "Container";
    case ItemType::CraftingMaterial: return "CraftingMaterial";
    case ItemType::Gathering: return "Gathering";
    case ItemType::Gizmo: return "Gizmo";
    case ItemType::JadeTechModule: return "JadeTechModule";
    case ItemType::MiniPet: return "MiniPet";
    case ItemType::PowerCore: return "PowerCore";
    case ItemType::Relic: return "Relic";
    case ItemType::Tool: return "Tool";
    case ItemType::Trinket: return "Trinket";
    case ItemType::Trophy: return "Trophy";
    case ItemType::UpgradeComponent: return "UpgradeComponent";
    case ItemType::Weapon: return "Weapon";
    default: return nullptr;
    }
}

const char* to_string(ItemRarity value) {
    switch (value) {
    case ItemRarity::Junk: return "Junk";
    case ItemRarity::Basic: return "Basic";
    case ItemRarity::Fine: return "Fine";
    case ItemRarity::Masterwork: return "Masterwork";
    case ItemRarity::Rare: return "Rare";
    case ItemRarity::Exotic: return "Exotic";
    case ItemRarity::Ascended: return "Ascended";
    default: return nullptr;
    }
}

const char* to_string(ArmorSlot value) {
    switch (value) {
    case ArmorSlot::Coat: return "Coat";
    case ArmorSlot::Leggings: return "Leggings";
    case ArmorSlot::Gloves: return "Gloves";
    case ArmorSlot::Helm: return "Helm";
    case ArmorSlot::HelmAquatic: return "HelmAquatic";
    case ArmorSlot::Boots: return "Boots";
    case ArmorSlot::Shoulders: return "Shoulders";
    default: return nullptr;
    }
}

const char* to_string(ArmorWeightClass value) {
    switch (value) {
    case ArmorWeightClass::Light: return "Light";
    case ArmorWeightClass::Medium: return "Medium";
    case ArmorWeightClass::Heavy: return "Heavy";
    default: return nullptr;
    }
}

ItemFields decode_item_fields(const ContentObject& item) {
    ItemFields f;
    f.item_type_raw = read_u32(item.bytes, 44);
    f.rarity_raw = read_u32(item.bytes, 96);
    f.level = read_u32(item.bytes, 116);
    if (f.item_type_raw == static_cast<uint32_t>(ItemType::Armor)) {
        f.armor_slot_raw = read_u32(item.bytes, 184);
        f.armor_weight_class_raw = read_u32(item.bytes, 280);
    }
    return f;
}

MapFields decode_map_fields(const ContentObject& map, const ContentPack& owning_pack) {
    MapFields f;
    f.map_data_file_ref_index = read_u32(map.bytes, 104);
    uint32_t codename_idx = read_u32(map.bytes, 176);
    uint32_t region_idx = read_u32(map.bytes, 272);
    if (codename_idx < owning_pack.strings.size()) f.codename = owning_pack.strings[codename_idx];
    if (region_idx < owning_pack.strings.size()) f.region = owning_pack.strings[region_idx];
    return f;
}

std::optional<SkinResolution> resolve_item_skin(
    uint32_t item_base_id, const ContentPack& item_pack, const ContentObject& item,
    const std::function<std::optional<std::vector<uint8_t>>(uint32_t)>& load_pack_bytes) {
    // Candidate externalOffsets fixups inside this item's byte range, closest
    // to +176 first.
    std::vector<ExternalOffsetFixup> candidates;
    for (const auto& fx : item_pack.external_offsets) {
        if (fx.reloc_offset >= item.begin && fx.reloc_offset < item.end) candidates.push_back(fx);
    }
    uint64_t hint = static_cast<uint64_t>(item.begin) + kItemSkinFixupHint;
    auto dist = [&](uint32_t reloc) -> uint64_t { return reloc > hint ? reloc - hint : hint - reloc; };
    std::sort(candidates.begin(), candidates.end(),
              [&](const auto& a, const auto& b) { return dist(a.reloc_offset) < dist(b.reloc_offset); });
    if (candidates.empty()) return std::nullopt;

    // The anchor pack that owns `targetFileIndex`: the nearest baseId at or
    // below item_base_id whose own cntc pack actually declares file_refs.
    uint32_t floor_id = item_base_id > kPackSearchSpan ? item_base_id - kPackSearchSpan : 0;
    std::optional<uint32_t> anchor_base_id;
    for (uint32_t candidate = item_base_id;; --candidate) {
        auto bytes = load_pack_bytes(candidate);
        if (bytes) {
            auto pack = parse_content_pack(*bytes);
            if (pack && !pack->file_refs.empty()) {
                anchor_base_id = candidate;
                break;
            }
        }
        if (candidate == floor_id) break;
    }
    if (!anchor_base_id) return std::nullopt;

    for (const auto& fx : candidates) {
        uint32_t target_offset = read_u32(item_pack.content, fx.reloc_offset);
        uint32_t target_base_id = *anchor_base_id + fx.target_file_index;
        auto bytes = load_pack_bytes(target_base_id);
        if (!bytes) continue;
        auto target_pack = parse_content_pack(*bytes);
        if (!target_pack) continue;
        for (const auto& obj : get_objects(*target_pack)) {
            if (target_offset >= obj.begin && target_offset < obj.end) {
                return SkinResolution{target_base_id, obj.unique_id()};
            }
        }
    }
    return std::nullopt;
}

} // namespace castlemist::cschema
