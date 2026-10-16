/// @file
/// @brief Maps an NPC's look preset (a composite variant) onto the Character Ripper's inputs.
/// @ingroup npc

#pragma once

#include "castlemist/character/manifest.h"
#include "castlemist/format/composite.h"
#include "castlemist/npc/catalogue.h"
#include "castlemist/ripper/assemble.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace castlemist::npc {

/// What the Character Ripper needs to dress one NPC look.
struct NpcAssembly {
    character::CharacterManifest manifest;  ///< Armour pieces (status Ok, skin_token set), race and gender.
    ripper::AssemblyOptions options;        ///< Face, hair, ears, skin style, physique and tints.
    std::vector<std::string> notes;         ///< Everything that could not be resolved.
};

/// Maps look `look_index` of `npc` through `composite`. nullopt (never a throw) when the look cannot be
/// resolved at all: no such look record, no race record for its race token, or no variant of that name.
/// `why`, when non-null, then receives the reason (e.g. "variant xpkurzwar: not in composite").
/// Anything partial (an unknown component, colours not applied) is a note on a returned result instead.
std::optional<NpcAssembly> npc_assembly(const NpcRecord& npc, size_t look_index,
                                        const composite::Composite& composite, std::string* why = nullptr);

/// Note added when the colour bytes are not applied.
inline constexpr const char* kColorsUnprovenNote = "colours not applied: byte format unproven";

/// A variant colour as a dye shift, or nullopt while the byte format is unproven (see
/// docs/superpowers/specs/2026-10-16-npc-looks-design.md, "Colour bytes": the measured mapping reaches
/// only ~79% against the real palettes, below the 90% bar).
std::optional<character::DyeShift> variant_color_shift(const composite::VariantColor& c);

/// The best-measured mapping, whether or not it is proven: brightness byte - 128, contrast / 128,
/// hue * 360 / 256 degrees, saturation / 128, lightness / 128. Kept so the mapping can be switched on
/// by one line in variant_color_shift once it is proven.
character::DyeShift candidate_color_shift(const composite::VariantColor& c);

}  // namespace castlemist::npc
