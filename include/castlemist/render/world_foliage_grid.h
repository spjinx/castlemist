/// @file
/// @brief FoliageGrid: the Map window's CPU index over a scene's zone
///        instances (docs/superpowers/specs/2026-10-16-world-foliage-design.md
///        D6): square tiles over the map bounds, per-instance fade ranges and
///        world spheres, and the per-frame cull with the distance fade.
///
/// Pure and D3D-free: the map window's world worker builds the grid
/// (build_foliage_grid) and WorldView::set_world takes ownership of it;
/// WorldView::add_models fills the spheres as models arrive
/// (foliage_set_sphere). Public, as world_prep.h is, because both the ui
/// worker and the view see it.
/// @ingroup render
#pragma once

#include <cstdint>
#include <vector>

namespace castlemist::world {
struct WorldScene;
}

namespace castlemist::render {

/// castlemist's choice (spec D6 `foliageTileUnits`): tiles are 1536 map units square (half a chunk).
inline constexpr float kFoliageTileUnits = 1536.0f;
/// castlemist's choice (spec D6 `foliageMaxVisible`): about 4x M6's max in view.
inline constexpr uint32_t kFoliageMaxVisible = 20000;

/// @brief The distance fade (spec D6): 1 for @p d <= @p n, 0 for @p d >= @p f,
///        linear in between. With @p n >= @p f it is a step at @p f (1 below, 0 from f on).
float foliage_fade(float d, float n, float f);

/// @brief The screen-door threshold of pixel (@p px, @p py):
///        (M[py & 3][px & 3] + 0.5) / 16 with the 4x4 Bayer matrix
///        M = {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}. The foliage pixel
///        shaders discard where fade < this (the same table).
float bayer4x4(int px, int py);

/// @brief One square tile of the grid.
struct FoliageTile {
    std::vector<uint32_t> instances;   ///< zone instance indices, sorted by (model, index)
    /// The union of the tile's instance world spheres set so far (hasAabb once one is).
    float aabbMin[3]{}, aabbMax[3]{};
    bool hasAabb = false;
    float farMax = 0;                  ///< the largest fadeFar of its instances, map units (unscaled)
};

/// @brief Tiles over the scene's bounds and per-instance cull data, index-aligned
///        with WorldScene::zones.instances.
struct FoliageGrid {
    /// Tile (tx, ty) spans x0 + tx*tileUnits .. +tileUnits and y0 + ty*tileUnits .. +tileUnits
    /// (ty grows north, with y). Instances outside fall into the nearest edge tile.
    float x0 = 0, y0 = 0, tileUnits = kFoliageTileUnits;
    int tilesX = 0, tilesY = 0;
    std::vector<FoliageTile> tiles;    ///< ty * tilesX + tx
    std::vector<uint32_t> tileOf;      ///< per zone instance: its tile
    /// Per instance, map units: its layer's fadeRange. An unresolved layer
    /// takes the map's largest type-3 fadeRange (UNPROVEN `zoneFade`, spec D6).
    std::vector<float> fadeNear, fadeFar;
    std::vector<float> sphere;         ///< 4 per instance (cx, cy, cz, r); r < 0 until its model arrives
    std::vector<uint8_t> collider;     ///< per instance: 1 when its source is ZoneSource::Collider
    uint32_t colliders = 0;            ///< how many instances have collider == 1

    // The draw data, built here (on the worker) so WorldView::set_world only
    // takes it over (spec Performance: set_world < 50 ms at full density).
    /// 16 per instance: its row-vector world, the scene's column-major bytes
    /// as they are (colmajor_to_rowvec is the same bytes).
    std::vector<float> world;
    std::vector<uint32_t> mesh;        ///< per instance: its model; UINT32_MAX = never drawn (bad model or matrix)
    uint32_t models = 0;               ///< the scene's model count the draw data was built for
    /// Model m's drawable instances, ascending: byModel[modelStart[m] .. modelStart[m + 1]).
    std::vector<uint32_t> modelStart, byModel;
};

/// @brief Builds the grid over @p s.zones.instances, tiles of @p tileUnits
///        (<= 0: kFoliageTileUnits) over `s.bounds` (no bounds: over the
///        instances' translations), and the draw data (world, mesh, byModel).
///        Every sphere starts unset (r = -1). Pure; safe on any thread.
FoliageGrid build_foliage_grid(const castlemist::world::WorldScene& s, float tileUnits = kFoliageTileUnits);

/// @brief Sets instance @p i's world sphere and grows its tile's AABB by it;
///        an out-of-range @p i, a negative or non-finite radius or a
///        non-finite centre is ignored.
void foliage_set_sphere(FoliageGrid& g, uint32_t i, const float centre[3], float radius);

/// @brief One instance that survived foliage_cull, with its fade.
struct FoliageVisible {
    uint32_t instance = 0;
    float fade = 0;
};

/// @brief foliage_cull's counts: tiles kept, and instances dropped by the cap.
struct FoliageCullStats {
    int tiles = 0;
    int capped = 0;
};

/// @brief Spec D6 "Cull" steps 1-2 and the cap. A tile without an AABB (no
///        sphere yet) is skipped, as is one farther from @p cam (distance to
///        its AABB; 0 inside) than farMax * @p distanceScale or wholly outside
///        @p planes (inside: dot(n, p) + d >= -r, the box's nearest corner per
///        plane). Kept tiles are visited nearest first; each instance is
///        dropped when its sphere is unset, its fade (foliage_fade on the
///        distance to its sphere centre, with its range times
///        @p distanceScale) is 0, or its sphere is outside a plane. The
///        survivors go into @p out (cleared first) until @p maxVisible; the
///        rest are counted in `st.capped`. @p collidersOnly keeps collider == 1 only.
void foliage_cull(const FoliageGrid& g, const float cam[3], const float planes[6][4], float distanceScale,
                  bool collidersOnly, uint32_t maxVisible, std::vector<FoliageVisible>& out, FoliageCullStats& st);

/// @brief Verification hook (GW2_MAPWIN_FOCUS=foliage): a ground-level view
///        into the foliage. The tiles holding a placed (sphere set) and
///        drawable instance are ranked by how many they hold; the one at
///        @p quantile (0 = sparsest, 1 = densest, nearest rank) is taken. The
///        camera stands kFoliageFocusEye units above the translation of that
///        tile's placed instance nearest the tile's mean placed translation
///        (up is -Z), and looks kFoliageFocusPitch radians below the
///        horizon along the one of eight headings (east first, then
///        counter-clockwise by 45 degrees) whose 60-degree wedge holds the
///        most placed instances within 3000 units (the first on a tie).
///        @p collidersOnly counts (and stands on) Collider instances only: a
///        view into the trees. False, @p pos and @p fwd untouched, when
///        nothing (that counts) is placed.
bool foliage_focus_view(const FoliageGrid& g, float quantile, bool collidersOnly, float pos[3], float fwd[3]);
/// castlemist's choice: the focus camera's height over the ground, map units.
inline constexpr float kFoliageFocusEye = 150.0f;
/// castlemist's choice: the focus camera's pitch below the horizon, radians (about 8 degrees).
inline constexpr float kFoliageFocusPitch = 0.14f;

/// @brief Which zone instances the Foliage layer shows: the Collider ones only, or all.
enum class FoliageShow : uint8_t { Colliders, All };

} // namespace castlemist::render
