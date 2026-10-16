#include "castlemist/npc/look.h"

#include "castlemist/npc/tokens.h"

#include <algorithm>
#include <cctype>

namespace castlemist::npc {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// The race record for a look's race token: an exact (case-insensitive) name, else the first whose name starts
/// with the token (tokens are cut at 12 letters: "sylvarifemal").
const composite::CompositeRace* find_race(const composite::Composite& c, const std::string& token) {
    const std::string t = lower(token);
    if (t.empty()) return nullptr;
    for (const composite::CompositeRace& r : c.races)
        if (lower(r.name) == t) return &r;
    for (const composite::CompositeRace& r : c.races)
        if (lower(r.name).starts_with(t)) return &r;
    return nullptr;
}

const composite::CompositeVariant* find_variant(const composite::CompositeRace& r, uint64_t token) {
    for (const composite::CompositeVariant& v : r.variants)
        if (v.token == token) return &v;
    return nullptr;
}

const char* slot_name(uint8_t type) {
    switch (type) {
        case 8: return "Boots";
        case 9: return "Coat";
        case 10: return "Gloves";
        case 11: return "Helm";
        case 12: return "Leggings";
        case 13: return "HelmAquatic";
        case 14: return "Shoulders";
        default: return nullptr;
    }
}

/// Index of `token` in `list`, -1 when absent.
int index_of(const std::vector<uint64_t>& list, uint64_t token) {
    auto it = std::find(list.begin(), list.end(), token);
    return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

bool all_zero(const composite::VariantColor& c) {
    return !(c.brightness | c.contrast | c.hue | c.saturation | c.lightness);
}

/// "HumanFemale" -> {"Human", "Female"}; a name with no gender suffix keeps its whole name as the race.
void split_race(const std::string& name, std::string& race, std::string& gender) {
    for (const char* g : {"Female", "Male"}) {
        const std::string suffix = g;
        if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
            race = name.substr(0, name.size() - suffix.size());
            gender = suffix;
            return;
        }
    }
    race = name;
    gender.clear();
}

}  // namespace

character::DyeShift candidate_color_shift(const composite::VariantColor& c) {
    // Measured 2026-10-16 (spec "Colour bytes"): every field is the palette float quantised to a byte, brightness
    // offset by 128 and hue spread over 256 steps for 360 degrees. {128, 128, 0, 128, 128} is the identity shift.
    return {static_cast<float>(c.brightness) - 128.0f, c.contrast / 128.0f, c.hue * 360.0f / 256.0f,
            c.saturation / 128.0f, c.lightness / 128.0f};
}

std::optional<character::DyeShift> variant_color_shift(const composite::VariantColor& c) {
    return candidate_color_shift(c);
}

std::optional<NpcAssembly> npc_assembly(const NpcRecord& npc, size_t look_index, const composite::Composite& comp,
                                        std::string* why) {
    auto fail = [&](std::string reason) -> std::optional<NpcAssembly> {
        if (why) *why = std::move(reason);
        return std::nullopt;
    };
    if (look_index >= npc.looks.size()) return fail("no look record " + std::to_string(look_index));
    const NpcLook& look = npc.looks[look_index];
    const std::string look_name = decode_token(look.look_token);

    const composite::CompositeRace* race = find_race(comp, look.race);
    if (!race) return fail("race " + look.race + ": not in composite");
    const composite::CompositeVariant* variant = find_variant(*race, look.look_token);
    if (!variant && race->variant_ref_race) {
        if (const composite::CompositeRace* ref = find_race(comp, decode_token(race->variant_ref_race)))
            if (ref != race) {
                variant = find_variant(*ref, look.look_token);  // the body stays this race's own
            }
    }
    if (!variant) return fail("variant " + look_name + ": not in composite");

    NpcAssembly out;
    split_race(race->name, out.manifest.race, out.manifest.gender);
    out.manifest.name = npc.name.empty() ? npc.codename : npc.name;
    ripper::AssemblyOptions& opt = out.options;
    bool colours_applied = false;

    // A colour: an all-zero tuple is an unset slot and stays as authored.
    auto tint = [&](const composite::VariantColor& c) -> std::optional<character::DyeShift> {
        if (all_zero(c)) return std::nullopt;
        colours_applied = true;
        return variant_color_shift(c);
    };

    for (const composite::VariantComponent& comp_ref : variant->components) {
        const uint64_t token = comp_ref.token;
        if (int i = index_of(race->faces, token); i >= 0) {
            opt.face = i;
        } else if (int i = index_of(race->hair_styles, token); i >= 0) {
            opt.hair = i;
        } else if (int i = index_of(race->ears, token); i >= 0) {
            opt.ears = i;
        } else if (auto it = race->file_data.find(token); it != race->file_data.end()) {
            const char* slot = slot_name(it->second.type);
            if (!slot) {  // a bare-body part: the skin style covers it
                out.notes.push_back("component " + decode_token(token) + ": type " +
                                    std::to_string(static_cast<int>(it->second.type)) + " not an armour piece");
                continue;
            }
            character::ManifestPiece p;
            p.slot = slot;
            p.skin_token = token;
            p.skin_name = decode_token(token);
            p.skin_type = "Armor";  // assemble_character only dresses pieces typed Armor
            p.status = character::PieceStatus::Ok;
            for (size_t d = 0; d < comp_ref.colors.size(); ++d)
                if (std::optional<character::DyeShift> s = tint(comp_ref.colors[d])) {
                    character::ManifestDye dye;
                    dye.slot = static_cast<int>(d);
                    dye.known = true;
                    dye.shift = s;
                    p.dyes.push_back(dye);
                }
            out.manifest.pieces.push_back(std::move(p));
        } else {
            out.notes.push_back("component " + decode_token(token) + ": not in composite");
        }
    }

    // boneScaleIndex is never applied as a physique: real values run to 153 against 14 presets, so what it
    // indexes is unknown. 0xFFFFFFFF is "none" (743 of 1315 HumanFemale variants) and stays silent.
    opt.physique = -1;
    if (variant->bone_scale_index != 0xFFFFFFFFu)
        out.notes.push_back("physique " + std::to_string(variant->bone_scale_index) + ": not in composite");
    if (variant->skin_style < std::max<size_t>(race->skin_styles.size(), 1))
        opt.skin_style = static_cast<int>(variant->skin_style);
    else
        out.notes.push_back("skin style " + std::to_string(variant->skin_style) + ": not in composite");

    opt.skin_tint = tint(variant->skin);
    opt.hair_tint = tint(variant->hair);
    opt.hair_tint2 = tint(variant->hair2);
    opt.eye_tint = tint(variant->eye);
    opt.pattern_tint = tint(variant->pattern);
    if (colours_applied) out.notes.push_back(kColorsNote);
    return out;
}

}  // namespace castlemist::npc
