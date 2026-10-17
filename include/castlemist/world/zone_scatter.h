/// @file
/// @brief Phase 2 of the zone scatter: the instances of the zon2 placements
///        that have no havk collider (docs/superpowers/specs/2026-10-16-world-foliage-design.md
///        D5).
///
/// The rule in force is `exact`: the client's own expansion, read from
/// Gw2-64.exe and transcribed in docs/research/gw2-terrain-to-unity.md §1.3
/// ("2026-10-16: zone scatter rule"). Its single-instance path (type 3)
/// reproduces all 4750 anchor-matched colliders of the three test maps (spec
/// D5 step 2). Its tiling path (types 1-2) and the branches none of those 4750
/// take are transcribed only, and labelled UNPROVEN (ruling S3).
/// @ingroup world
#pragma once

#include "castlemist/world/zones.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace castlemist::world {

struct WorldScene;

/// @brief The scatter rule (spec D5 step 4).
struct ZoneScatterRule {
    ZoneScatterMode mode = ZoneScatterMode::Colliders;
    bool tiltFromGround = false;   ///< densityMatched only: D5 step 3's tilt hypothesis (UNPROVEN as phrased, Task 9)
    bool zFromTerrain = true;      ///< densityMatched only: z = terrain_height_at (UNPROVEN as phrased, Task 9)
    /// exact only: the client's graphics-quality input to the tiling path and the
    /// subModel test (getter 0x140c58c10, not read). 2 = full tiling (ruling S3).
    int graphicsQuality = 2;
    /// exact only: x and y are clamped to the parm rect inset by this many units
    /// (0x140c8a440's context rect; UNPROVEN, no collider reaches it).
    float clampInset = 1.0f;
};

/// The rule Task 9 settled: `exact` at graphics quality 2
/// (docs/research/gw2-terrain-to-unity.md §1.3, 2026-10-16; rulings S3 and S4).
ZoneScatterRule zone_scatter_rule_in_force();

/// @brief One instance the exact rule makes, before its model is resolved.
struct ZoneScatterDraw {
    uint32_t fileId = 0;
    bool subModel = false;          ///< the layer's subModel second instance (0x140cbc210)
    float pos[3] = {}, rot[3] = {}; ///< map space, as the client's placement (client_world_matrix inputs)
    float scale = 1;
};

/// @brief The client's RNG (Gw2-64.exe 0x140e24b10 family): Park-Miller 48271
///        by Schrage, with the client's +0x80000000 fix-up.
struct ZoneRng {
    uint32_t state = 0x075BD924u;
    explicit ZoneRng(uint32_t seed) : state(seed ? seed : 0x075BD924u) {}
    uint32_t next();
    float fA();                       ///< float(next()) * 2^-31 (0x140e243d0)
    float fB();                       ///< float(next()) * 4.6566126e-10 (0x140e24280)
    uint32_t irange(uint32_t a, uint32_t b);   ///< a + next() % (b - a + 1) (0x140e24b10); b < a gives a
};

/// The per-cell seed (0x140cc23c0): (zoneSeed << 8) ^ T[(lx + w ly + lx ly) mod 256] ^ (((4 ly) ^ lx) << 14),
/// with T the table at 0x142592280, (lx, ly) the cell's offset in vertRect and w its width.
uint32_t zone_cell_seed(uint8_t zoneSeed, uint32_t lx, uint32_t ly, uint32_t w);

/// A model's pack `flags` remapped to the runtime bits the expansion reads (0x140c8b9b0,
/// tables 0x141d6e110 -> 0x141d6e170).
uint32_t zone_runtime_flags(uint32_t packFlags);

/// The tiling count bound T for a layer's `tiling` at @p quality (0x140cbc0d0).
uint32_t zone_tiling_max(uint8_t tiling, int quality);

/// The client's factor on a layer's `fadeRange` at graphics quality @p quality:
/// {0.85, 1, 1.5, 2}[quality] (0x140cbc0b0 / 0x140cbc090, docs/research/gw2-terrain-to-unity.md
/// §1.3 table); 1 out of range. At the quality in force (2) it is 1.5.
float zone_fade_quality_scale(int quality);

/// collideData index of every placement of zone @p zone (0x140cc23c0): offsetData[chunk] for its
/// 32 x 32-vertex chunk plus its order among the chunk's non-empty cells, rows then columns.
/// Indexed by placement - firstPlacement; UINT32_MAX where the chunk has no offsetData entry.
std::vector<uint32_t> zone_collide_index(const ZoneScatter& z, uint32_t zone);

/// @brief The exact rule's instances for placement @p placement, whether or not it has a
///        collider (so the dat test can check the collided ones). Appends to @p out; nothing
///        when the layer or slot does not resolve, or when a collideData zone's cell has no
///        entry (the client skips it). @p collideIndex: the zone's zone_collide_index().
void zone_scatter_exact(const WorldScene& s, uint32_t placement, const ZoneScatterRule& r,
                        const std::vector<uint32_t>& collideIndex, std::vector<ZoneScatterDraw>& out);

/// Adds the Scatter (exact) or Density instances of every placement with collider == UINT32_MAX
/// and a resolvable model (exact: as many as the rule makes; densityMatched: one); none in
/// Colliders mode. Appends their models to scene.models (a model already there keeps its index)
/// and sets ZoneLayerModel::model. Sets scene.zones.mode, and adds the rule's UNPROVEN
/// warnings. Returns the number of instances added. Never doubles a collided placement.
size_t scatter_instances(WorldScene& s, const ZoneScatterRule& r);

/// densityMatched draws: state = (uint64_t(mapFileId) << 32) ^ placementIndex; each draw is
/// (splitmix64_next(state) >> 11) * 2^-53, in [0, 1); order u, v, yaw, scale (castlemist's choice, labelled).
double zone_unit_draw(uint64_t& state);

} // namespace castlemist::world
