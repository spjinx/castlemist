/// @file
/// @brief Reads the catalogue-relevant fields of one character object (type 67) from a cntc pack.
///
/// Layer-internal (not installed under include/). Offsets are relative to the object's begin.

#pragma once

#include "pack.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace castlemist::npc::detail {

/// @brief The fields of a character object that the catalogue records.
struct CharacterFields {
    uint32_t id = 0;                                       ///< Object dataId.
    std::string codename, race, modifier;                  ///< Content slug; race token; "undead"/"ghost"/"".
    std::vector<std::pair<uint32_t, uint32_t>> weapons;    ///< {slot field, item dataId}, slot order.
    std::vector<uint32_t> skills;                          ///< Skill dataIds, field order.
};

/// @brief Extracts weapons (item type 35 with u32 @+44 == 24), skills (type 64), race, modifier and codename.
CharacterFields read_character(const PackSet& set, size_t pack_index, const Object& obj);

}  // namespace castlemist::npc::detail
