#ifndef CASTLEMIST_FORMAT_COMPOSITE_H
#define CASTLEMIST_FORMAT_COMPOSITE_H

// The character Composite file (packfile container "cmpc", chunk "comp" v19 =
// PackCompositeV20) -- how GW2 dresses a character. Per race/gender it maps an
// armor appearance token (a skin's u64 at +208, see content_map.h's
// skin_token()) to that race's model, its dyeable diffuse, normal map, the four
// per-channel dye masks and a cut mask; and it lists the 1024x1024 character
// atlas rects ("blit rects") the armor textures are composited into.
//
// Layout and every measured fact: docs/research/gw2-armor-skins-and-dyes.md.
// The generic struct-template parser misreads this file: in 64-bit packfiles a
// fileref is an 8-byte self-relative pointer, not a 4-byte value.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace castlemist::composite {

struct BlitRect {
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // atlas pixels, [x0,x1) x [y0,y1)
};

struct BlitRectSet {
    std::string name;  // "ArmorHeavy", "ArmorLight", "ArmorCharrHeavy", ...
    uint32_t width = 0, height = 0;
    std::vector<BlitRect> rects;
};

/// One armor appearance for one race/gender. fileIds are 0 when unset.
struct CompositeFileData {
    uint64_t token = 0;
    uint8_t type = 0;  // piece kind: 8 boots, 9 coat, 10 gloves, 11 helm, 12 leggings, 13 aquatic helm, 14 shoulders
    uint32_t mesh_base = 0, mesh_overlap = 0;
    std::array<uint32_t, 4> mask_dye{};  // dye channel 1..4 masks
    uint32_t mask_cut = 0, texture_base = 0, texture_normal = 0;
    uint32_t dye_flags = 0, hide_flags = 0, skin_flags = 0;
    uint8_t blit_set = 0;  // index into Composite::blit_sets
};

struct CompositeRace {
    std::string name;  // "SylvariFemale" = API race + gender; also NPC variants ("CreatureCM", ...)
    uint32_t skeleton_file = 0;
    std::unordered_map<uint64_t, CompositeFileData> file_data;  // keyed by token
};

struct Composite {
    std::vector<BlitRectSet> blit_sets;
    std::vector<CompositeRace> races;
    const CompositeRace* race(std::string_view name) const;
};

/// Parses the decompressed Composite packfile. nullopt if it isn't one or any
/// pointer/array falls outside the buffer.
std::optional<Composite> parse_composite(std::span<const uint8_t> decompressed);

} // namespace castlemist::composite

#endif // CASTLEMIST_FORMAT_COMPOSITE_H
