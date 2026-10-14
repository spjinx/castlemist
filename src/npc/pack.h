/// @file
/// @brief Read-only reader for cntc (PackContent) packs: objects, their refs, file and string slots.
///
/// Layer-internal (not installed under include/). Layout: PF header; `u16 @6` = first chunk; chunks
/// `{fourcc, u32 size}` + body until "Main"; at Main+16 a u32 flags then 11 array descriptors
/// `{u32 count, i64 pointer}` (pointer relative to its own position). Arrays used here: 2 fileRefs,
/// 3 indexEntries, 4 localOffsets, 5 externalOffsets, 6 fileIndices, 7 stringIndices, 9 strings,
/// 10 content. See format/content_schema.h for the full field table.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace castlemist::npc::detail {

/// @brief A pointer out of an object into another object (same pack or another pack).
struct Ref {
    uint32_t field = 0;             ///< Pointer slot, relative to the owning object's begin.
    uint32_t target_file_index = 0; ///< External refs: pack index counted from the fileRefs pack.
    uint32_t target_offset = 0;     ///< Begin offset of the target object in the target pack's content.
    bool external = false;
};

/// @brief One object in a pack's content: [begin, end) plus the slots that fall inside it.
struct Object {
    uint32_t type = 0, begin = 0, end = 0;                   ///< [begin,end) in Pack::content.
    std::vector<Ref> refs;                                   ///< localOffsets + externalOffsets inside the object.
    std::vector<std::pair<uint32_t, uint32_t>> files;        ///< {field, fileRefs index}.
    std::vector<std::pair<uint32_t, std::string>> strings;   ///< {field, string}.
    uint32_t data_id = 0;                                    ///< u32 @+40, read at parse.
};

/// @brief One parsed cntc pack.
struct Pack {
    uint32_t base_id = 0, file_id = 0;
    std::vector<uint32_t> file_refs;  ///< Array 2, decoded via cschema::decode_fileref_pair.
    std::vector<uint8_t> content;     ///< Array 10.
    std::vector<Object> objects;      ///< Sorted by begin, duplicates dropped.

    /// @brief The object starting exactly at `begin`, else nullptr.
    const Object* object_at(uint32_t begin) const;
    /// @brief u32 at content[obj.begin + field]; 0 when it would run past obj.end.
    uint32_t u32(const Object& obj, uint32_t field) const;
    /// @brief u64 at content[obj.begin + field]; 0 when it would run past obj.end.
    uint64_t u64(const Object& obj, uint32_t field) const;
};

/// @brief Parses one decompressed cntc packfile; nullopt for anything not a well-formed cntc.
std::optional<Pack> parse_pack(std::span<const uint8_t> bytes, uint32_t base_id, uint32_t file_id);

/// @brief All packs of the datastore, ordered by file_id, with cross-pack ref resolution.
struct PackSet {
    std::vector<Pack> packs;
    /// @brief Orders packs by file_id and locates the pack carrying the fileRefs table.
    void finish();
    /// @brief The shared fileRefs table (empty if no pack carries one).
    const std::vector<uint32_t>& shared_file_refs() const;
    /// @brief Resolves a ref from packs[from]: local -> same pack; external -> the pack at position
    /// target_file_index counted in file_id order from the fileRefs pack. {pack, nullptr} when the
    /// target offset is not an object start; {nullptr, nullptr} when the pack is unknown.
    std::pair<const Pack*, const Object*> resolve(size_t from, const Ref& r) const;

private:
    size_t anchor_ = static_cast<size_t>(-1);  ///< Index of the fileRefs pack after finish().
};

}  // namespace castlemist::npc::detail
