/// @file
/// @brief The GW2 client's per-particle maths, as pure functions.
/// @ingroup fx

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

/// @defgroup fx fx -- the client's effect maths
/// @brief Transcriptions of Gw2-64.exe's particle, streak and lightning routines.
///
/// Every function names the client function it transcribes. Addresses are those of the
/// 2026-09 build the `gw2re` Ghidra project holds (the exe on disk is newer and its
/// addresses differ); docs/research/gw2-particle-system.md. No globals, no renderer: B's sim, the
/// browser and the Unity translation all run the same code.
/// @{

namespace castlemist::fx {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

/// @brief A runtime range: `base + u * span`, u in [0, 1).
///
/// The file stores `{min, max}`; the loader keeps `base = min, span = max - min`
/// (GrCloud.cpp, FUN_140ac8c40), so a reversed pair is a negative span.
struct Range {
    float base = 0, span = 0;
    static Range from_min_max(float lo, float hi);
    float at(float u) const;
};

/// @brief The begin -> end colour weight at life fraction @p life (FUN_140ad3a30).
///
/// `m = life mod period`, `n = floor(life / period)`; `x = m / period` on even n,
/// else `1 - m / period` (a ping-pong); `x = 0` when the period is 0. Then
/// `w = clamp((x - lo) / (hi - lo))` with @p falloff as `{lo, hi}` (base = lo,
/// base + span = hi); `hi == lo` is a step at lo.
float colour_weight(float life, float period, Range falloff);

/// @brief Spawn velocity: normalize(direction sample) * magnitude sample.
///
/// @p u holds the four uniforms for dirX, dirY, dirZ and the magnitude. Every
/// direction range zero means no velocity at all (FUN_140ace900).
Vec3 spawn_velocity(const Range dir[3], Range magnitude, const float u[4]);

/// @brief A stored lifetime curve at @p t: linear between keys, clamped 0..1
///        (the baked curve, FUN_140acbe30). No keys -> @p dflt.
float curve_eval(const std::vector<std::pair<float, float>>& keys, float t, float dflt);

/// @brief Curve preset @p preset at @p t: the client's table of 18 easing functions
///        (0x14257d870 -> 0x1409c4930..0x1409c4d50, 2026-09 build). 0 constant 1, 1 t,
///        2..5 a quartic bump and its 0.75/0.5/0.25-lifted copies, 6..8 three more bumps,
///        9 t^2, 10 t^4, 11..14 a cubic ease and its lifted copies, 15..17 (1-|2t-1|)^2, ^4, ^1.
///        An id outside the table (the client would jump to garbage) gives 1.
float curve_preset(uint32_t preset, float t);

/// @brief The scale preset as the builders apply it (0x140acf880, 0x140ad11b0): file
///        `flags` 0x400 runs t backwards (runtime 0x200), 0x200 takes 1 - value (runtime 0x400).
float scale_preset_value(uint32_t preset, float t, uint32_t fileFlags);

/// @brief The opacity preset as the colour routine applies it (0x140ad3a30): file `flags`
///        0x80 takes 1 - value (runtime 0x80).
float opacity_preset_value(uint32_t preset, float t, uint32_t fileFlags);

/// @brief The emitter `flags` word as the client keeps it: FUN_140d52160's bit permutation.
uint32_t runtime_emitter_flags(uint32_t fileFlags);

/// @brief Which branch of the sprite builder (0x140ad11b0) a plane emitter's file
///        `alignmentType` takes (FUN_140d52110, table 0x141ee8df0): 0->1, 1->5, 2->4, 3->2,
///        4->0, 5->3. 255 for a value past 5 (the client asserts). The branches are described
///        in docs/research/gw2-particle-system.md, 2026-10-28.
uint8_t plane_alignment_case(uint8_t fileAlignmentType);

struct Vec2 {
    float x = 0, y = 0;
};
/// @brief A quad's two half-axes.
struct SpriteAxes {
    Vec2 u, v;
};
/// @brief Branch 1 (file alignment 0): the quad's half-axes in the view plane,
///        u = (cos r, sin r) sx, v = (-sin r, cos r) sy; negative sizes clamp to 0.
SpriteAxes camera_sprite_axes(float rotation, float sizeX, float sizeY);

}  // namespace castlemist::fx

/// @}
