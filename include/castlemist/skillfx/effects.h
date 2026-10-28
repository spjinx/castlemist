/// @file
/// @brief The visual effects a GW2 skill links to in the cntc content datastore.
///
/// A skill is a type-64 content object; its effects are type-183 objects it points at
/// (an optional MODL effect model and an optional AMSP sound script each). A kind-1
/// effect is a group whose refs are further effects. Measured on Firebrand, see
/// docs/research/gw2-skill-effects.md.

#pragma once

#include "castlemist/format/cntc_pack.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace castlemist::skillfx {

/// @brief An object's first 16 bytes: the content GUID arcdps logs and Elite Insights use.
using Guid = std::array<uint8_t, 16>;

constexpr uint32_t kSkillType = 64;
constexpr uint32_t kEffectType = 183;
constexpr uint32_t kItemType = 35;
constexpr uint32_t kBarType = 65;
/// @brief Skill dataIds below this are boons and conditions (717 Protection, 737 Burning, ...).
constexpr uint32_t kFirstOwnSkillId = 2000;

/// @brief The client class an effect `kind` builds (EfCliContext's factory 0x141345920,
///        2026-09 build). Each class's vtable sits just before its source file's name in
///        .rdata; kinds 3/5 and 8/9 are two classes each in EfCliEffectMaterial.cpp and
///        EfCliEffectShakeSpring.cpp. docs/research/gw2-skill-effects.md, 2026-10-28.
enum class EffectClass : uint8_t { Unknown, Audio, Compound, Decal, Material, Model, PostProc, ShakeSpring };

/// @brief 0 Audio; 1, 11 Compound; 2 Decal; 3, 5 Material; 4, 6, 10 Model; 7 PostProc;
///        8, 9 ShakeSpring; anything else Unknown (the factory asserts).
EffectClass effect_class_of_kind(uint32_t kind);

/// @brief Lower-case name ("audio", "compound", ..., "unknown").
const char* effect_class_name(EffectClass c);

/// @brief True for the kinds whose refs are child effects (1 and 11).
bool is_compound_kind(uint32_t kind);

/// @brief One effect of a skill.
struct Effect {
    uint32_t field = 0;              ///< Slot in the referring object (0 for unlinked/paired effects).
    uint32_t kind = 0;               ///< The effect's dataId (@+40): the client class it builds (cls).
    EffectClass cls = EffectClass::Unknown;
    std::string codename;            ///< Content slug, e.g. "Bmt2N.a7wVK".
    uint32_t model = 0, sound = 0;   ///< First MODL / AMSP fileId; 0 = none.
    Guid guid{};
    std::string source = "content";  ///< "content" (linked in content) or "arcdps" (paired from logs).
    std::string ei_name;             ///< Elite Insights' name for the GUID, if known.
    uint32_t count = 0;              ///< arcdps: number of casts the pairing was seen in.
};

/// @brief fileId -> its container fourcc ("MODL", "AMSP", ...), "" when unknown (the index DB, injected).
using ContainerOf = std::function<std::string(uint32_t)>;

/// @brief The first string slot shaped like a content slug ("Bp5rI.EWIp2"); "" if none.
std::string codename(const cntc::Object& o);

/// @brief The object's first 16 bytes.
Guid guid_of(const cntc::Pack& p, const cntc::Object& o);

/// @brief Reads one type-183 object.
Effect read_effect(const cntc::PackSet& s, size_t pack, const cntc::Object& o, const ContainerOf& container);

/// @brief What one skill points at.
struct SkillLinks {
    std::vector<Effect> effects;            ///< Field order; compound (kind 1 / 11) groups expanded in place.
    std::vector<uint32_t> related_skills;   ///< Other skills (flip skills, charges, ...).
    std::vector<uint32_t> applies;          ///< Boons/conditions (dataId < kFirstOwnSkillId).
};

/// @brief The effects, related skills and boons a skill points at directly.
SkillLinks find_effects(const cntc::PackSet& s, size_t pack, const cntc::Object& skill, const ContainerOf& container);

}  // namespace castlemist::skillfx
