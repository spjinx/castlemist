# World Foliage — Design (sub-project 5 of the World Engine)

Date: 2026-10-16
Status: draft for review. Built in three phases, in order. Following the user's
rule of 2026-10-15, there are no per-task reviews, only one whole-branch review
at the end.

## Goal

Draw, export and import a GW2 map's zone-scatter layer (`zon2`: grass, plants,
shrubs and trees) from the same `WorldScene` that already holds terrain, props,
collision, water and sky.

- **Phase 1** places every zone tree that has a `havk` collider, exactly, and
  draws it.
- **Phase 2** decodes the scatter so the remaining zone instances are placed as
  well. The placement is exact where the rule is proven; otherwise it is
  density-matched and labelled UNPROVEN.
- **Phase 3** carries the foliage through the world export to Unity (Unity
  Terrain details and trees, or instances on the mesh terrain) and Blender
  (geometry nodes), with density and distance controls for Quest.

Sub-project 1's rule still holds: **no invented conventions**. A value the data
does not settle is a labelled setting with its source, written into the
manifest's `settings` and `warnings`, or it is left out.

**Static client only, never a running game.** By the user's rule of
2026-10-16, static disassembly of `Gw2-64.exe` (the repo's IDB / Ghidra
project) is allowed. Launching the game client to read anything from it is
not: no hooks, no live-client or frame captures, no in-game linking. Where
neither the dat nor the static code settles a value, it stays a labelled
UNPROVEN setting. The collider transforms in `havk` are the ground truth that
any rule must reproduce.

**Numbering.** The world export spec's "Out of scope" names Udon and gameplay
data as sub-project 5. Foliage takes number 5, and Udon moves to 6. The world
export spec's "Zone models" out-of-scope line is what this sub-project picks up.

## Why

**The user's bug report.** In the Map Window, walking through Queensdale, the
colliders do not line up with the trees, and many trees are missing.

**The cause.** `havk` has three placement arrays (`gw2-world-frame.md` §7):
`obsModels`, `propModels` and `zoneModels`. `build_collision` places all three,
so the walker bumps into zone-tree trunks. But `zon2` is not in `WorldScene`.
`load_world` warns `chunk zon2 v22 not read`, so the map window never draws the
trees those trunks belong to.

The zone collision instances per map (frame §7.4) are 4391 on Queensdale, 88 on
Lion's Arch and 568 on Spirit Vale. Those are hull instances. The placements
behind them are counted below (M1).

**What exists.**

- `Extractor::parseMapZones` (`gw2model.hpp:960-1090`, ported from Tyria3D; the
  user has ruled that licence is not a concern) walks the run-length `flags[]`
  on a 48-unit grid. It plants one unrotated instance per cell, at scale 1,
  using the layer's `modelArray[0]` only.
- `gw2dat_cli map` prints those placements.
- Nothing else uses them.

The research note `docs/research/gw2-terrain-to-unity.md` (§1.3, §6, §8) sets
the targets and lists the unknowns. The user accepted its recommendations:

- mesh terrain stays the default;
- Unity Terrain is an opt-in importer mode, starting with one baked layer on PC;
- Quest stays on mesh terrain, with little or no foliage;
- zone types 1-2 become Unity details, and type 3 becomes Unity Terrain trees;
- `prp2` props stay GameObject instances.

## Measurements (2026-10-16, before this spec)

All of these come from `gw2dat_cli parse --max-depth 8` on the three test maps
(Queensdale 192711, Lion's Arch 191000, Spirit Vale 1151420) and
`gw2dat_cli model`, run from the main checkout's build. Gw2-64.exe was not
running. The output was walked by small Python scripts that are kept in the
session scratchpad and not committed. The phase-1 dat test (Testing) re-proves
every number in C++.

**Notation.** The parm rect is `(x0, y0, x1, y1)`, and `(x0, y0)` is its
south-west corner. `vr` is a zone's `vertRect`, and `w = vr[2] − vr[0]`. The
run-length walk's position after adding a skip byte is `k`. A non-zero flag
byte `f` at that position names the grid point `(c, r) = (vr[0] + k mod w,
vr[1] + ⌊k / w⌋)`. The point's **anchor** is
`A(c, r) = (x0 + 48·(c − 1), y0 + 48·r)` in map space.

### M1. Placement counts

| | Queensdale | Lion's Arch | Spirit Vale |
|---|---|---|---|
| zon2 placements (client decode, S4) | 247 804 | 40 252 | 8 670 |
| of layer type 1 / 2 / 3 | 193 125 / 44 380 / 10 299 | 36 939 / 2 300 / 1 013 | 8 386 / 0 / 284 |
| `havk.zoneModels` entries | **4381** | **88** | **284** |
| zone collision instances (hulls, frame §7.4) | 4391 | 88 | 568 |
| distinct `geometryIndex` in `zoneModels` | 45 | 7 | 1 |
| of those, also used by `propModels` / `obsModels` | 37 | 4 | 0 |

**Corrected 2026-10-18 (ruling S4, Task 10).** The counts above were first
measured with a (skip, flag) walk (217 114 / 32 448 / 7 642 placements; type
1 / 2 / 3: 167 169 / 39 649 / 10 296 on Queensdale). That walk was wrong: the
client stores `flags` as run-length (count, value) pairs, and as raw bytes, one
per vertex, in a zone with no `encodeData` (34 zones). The table is the client
decode (`walkZoneFlags`); type 3 changed by only 3, 2 and 0 placements.

A `zoneModels` entry is one placement. A geometry can carry several hulls,
which is why Spirit Vale shows 284 entries but 568 hull instances.

### M2. Each zone tree's position, against the zone grid

- Of Queensdale's 4381 entries, 4378 lie within ±24 map units (Chebyshev,
  ±0.5 grid cell) of the anchor of exactly one type-3 placement. No placement
  is claimed twice.
  - The offsets fill the square: x from −23.6 to +23.7 and y from −23.5 to
    +23.4 at p1-p99, with both means within 3 units of 0.
- Lion's Arch matches 88 of 88 and Spirit Vale 284 of 284.
- **Corrected 2026-10-18 (S4, Tasks 9-10).** These matches were made against the
  old walk's type-3 placements. With the client decode, **4381 of 4381**
  Queensdale entries link by anchor (4750 of 4750 on the three maps). The three
  "unmatched" entries are the three type-3 cells of raw zone 135, and the
  scatter rule gives their x, y, yaw and scale exactly. The old walk's position
  `k` is one past the run, so the true cell is `k − 1`; this is the "c − 1"
  below, and it settles the open question U4.
- With rows counted from the north edge instead, almost nothing matches. So
  rows count **north from `y0`**.
- The anchor's column is `c − 1`, one cell west of the grid point
  `parseMapZones` uses today (`x0 + 48·c`). Whether the walk's true position is
  `k − 1` (it wraps to the previous row at a row start) or `c − 1` within the
  row cannot be told apart: no type-3 placement sits at a row start. This
  question is open for phase 2 (U4).
- Queensdale's three unmatched entries (indices 3350-3352) have scale 0.84 and
  sit about 12 cells from any type-3 placement. One sits at exactly
  (17184, 3648). They look hand-placed, and they are linked by their geometry
  (M3).

### M3. Which model a zone tree is

- **The model is `layerDefArray[(f >> 4) − 1].modelArray[(f >> 1) & 7]`.**
  - Under this rule, every geometry maps to exactly one model fileId on all
    three maps (45 of 45, 7 of 7, 1 of 1). No fileId has two geometries.
  - On no map, for no placement, does `(f >> 1) & 7` fall past the layer's
    `modelArray`.
  - `modelArray[0]` alone (today's parser) is wrong for 167 277 of Queensdale's
    217 114 placements: those sit on layers with more than one model.
- **Independent check through props.** Some geometries are also used by
  `propModels` entries whose `token` is the `guid` of a `prp2` prop (frame
  §7.1). For each such geometry, the prop's fileId equals the fileId the rule
  above gives: 37 of 37 on Queensdale, 4 of 4 on Lion's Arch, 0 disagreements.
- **The model's box and the hull's box agree.** For 12 linked Queensdale
  models, the geometry's last-animation hulls were taken ×32 with z negated
  (frame §7.2-7.3). The share of each hull box that lies inside the model's
  vertex box is 1.0 for 11 models and 0.93 for one.
  - Box IoU is about 0.005 for trees: hulls are trunks inside a wide canopy.
    So IoU is the wrong measure.
  - Model bottom and hull bottom agree to the unit on 6 of the 12.
- **Distinct zone models.** Queensdale uses 154 under this rule, against 54
  under `modelArray[0]` alone.

### M4. Flag bits and colliders

- **Bit 0** of `f` is set on every type-1 and type-2 placement and on no type-3
  placement, on all three maps. Its meaning is UNPROVEN.
- **Colliders go by model.** On Queensdale, 45 of the 87 type-3 models have a
  `havk` geometry, and **every** placement of those 45 models has a
  `zoneModels` entry. No model is partly collided. The other 42 models (5 918
  placements) have no collider. On Lion's Arch, 923 type-3 placements have no
  collider; on Spirit Vale, none lack one.
- Collided zones have `zoneFlags` 0 (113 zones), 2 (1) or 6 (1). Most of them
  have `seed` 0.

### M5. The transforms the colliders carry (inputs for phase 2)

- **Yaw.** `rotate.z` covers 0 to 2π.
- **Tilt.** `rotate.x` and `rotate.y` are non-zero on 3687 of Queensdale's
  4381 entries. Their magnitude, `|(rotate.x, rotate.y)|`, is p50 3.2°, p90
  9.5°, p99 22.8° and max 62°.
  - Most of these layers have `rotRange` x and y = [0, 90]. A uniform draw from
    that range would give a p50 near 45°. So the tilt is not drawn from
    `rotRange`, and ground alignment is the hypothesis (U3).
- **Scale.**
  - 4271 of 4378 matched entries lie inside their layer's `scaleRange`; why the
    other 107 do not is UNPROVEN.
  - Only 83 distinct values occur on Queensdale. On a `[0.5, 1.5]` layer they
    step by 0.0773 (99/1280).
- **zPos and seed.** `zone.zPos` is 0 on the zones seen. 89 of the 248
  Queensdale zones have a non-zero `seed`.
- **Fields `parseMapZones` ignores.**
  - `encodeData`: one `{index: word, offset: byte}` per zone row (22 791 on
    Queensdale), presumably a row index into `flags[]`.
  - `collideData`: `{normalX, normalY, zPos}`, 5 668 entries in 5 zones.
  - `offsetData`: words, 68 entries in 5 zones.

### M6. What a camera sees within the fade range (Queensdale)

From 200 random placements taken as camera points, the number of placements
within their own layer's `fadeRange` far value is p50 2 610, p90 3 658 and max
4 955. This counts one instance per placement (U2).

The `fadeRange` far values cluster by type:

| type | fade far (units) |
|---|---|
| 1 | 2000-3550 |
| 2 | 3000 |
| 3 | 7500 (most); up to 17 000 in the research note |

## Decisions

### D1. Three phases, each with its own acceptance

| phase | what | acceptance |
|---|---|---|
| 1 | `zon2` decoded into `WorldScene` (raw). Every `zoneModels` entry is linked to its model and placed exactly from its `havk` transform. Drawn in the Map Window. Exported, and placed by both importers as instances. | **Every zone collider has a visible tree** (Testing, dat and headless). |
| 2 | The scatter rule for the remaining placements: exact if proven against the colliders, otherwise density-matched (UNPROVEN). Map Window performance at full density. | The colliders reproduce exactly under `exact`; otherwise `densityMatched` is labelled. The Map Window budgets hold on Queensdale. |
| 3 | The manifest's full foliage block and the `unityTerrain` block. Unity Terrain mode (baked layer, details, trees), mesh-terrain instances and patches, Blender geometry nodes, Quest knobs. | Schema, dry run, Blender headless and the Unity batchmode or checklist runs. |

Phase 2 starts when phase 1's acceptance passes. Phase 3's exporter and
importer work is written against phase 1's records. It does not wait for phase
2, because records from phase 2 have the same layout.

### D2. Data model: `WorldScene` gains zones

New header `include/castlemist/world/zones.h`. Everything is in map space, as
stored, with fields named as in the template.

```cpp
struct ZoneLayerModel {
    uint32_t fileId = 0;            ///< modelArray[i].filename
    uint32_t model = UINT32_MAX;    ///< index into WorldScene::models once the model is used; else UINT32_MAX
    float probability = 0; uint32_t flags = 0; float hslOffset[3]{}; float zOffsets[2]{}; uint64_t permutation = 0;
};
struct ZoneLayer {                  ///< one layerDefArray entry
    uint32_t defToken = 0; uint32_t indexInDef = 0;     ///< 0-based
    uint8_t type = 0, width = 0, height = 0, radiusGround = 0, sortGroup = 0, tiling = 0, noise = 0;
    float scaleRange[2]{}, probability = 0, fadeRange[2]{}, rotRange[3][2]{}, hslRanges[4][2]{};
    float instanceScaleJitter = 0; uint32_t layerFlags = 0; uint32_t materialFileId = 0;
    std::vector<ZoneLayerModel> models;
};
struct Zone {                       ///< one zoneArray entry
    uint32_t defToken = 0; int32_t vertRect[4]{}; uint8_t seed = 0; float range[2]{}, zPos = 0, waterHeight = 0;
    uint32_t zoneFlags = 0; uint16_t broadId = 0;
    uint32_t firstPlacement = 0, placementCount = 0;    ///< into ZoneScatter::placements
    std::vector<float> collideData;                     ///< normalX, normalY, zPos triples, as stored
    std::vector<uint16_t> offsetData;                   ///< as stored
};
struct ZonePlacement {              ///< one non-zero flag byte of the run-length walk
    uint32_t zone = 0, layer = UINT32_MAX;              ///< layer: index into ZoneScatter::layers; UINT32_MAX = unresolved
    int32_t c = 0, r = 0;                               ///< grid point (Measurements, notation)
    uint8_t flag = 0;                                   ///< as stored: layer (f>>4), slot ((f>>1)&7), bit 0
    uint32_t collider = UINT32_MAX;                     ///< index of the havk zoneModels entry linked to it, else UINT32_MAX
};
enum class ZoneSource : uint8_t { Collider, Scatter, Density };
struct ZoneInstance {
    uint32_t model = 0;             ///< index into WorldScene::models
    uint32_t layer = UINT32_MAX;    ///< index into ZoneScatter::layers (UINT32_MAX: a collider linked by geometry only)
    uint32_t placement = UINT32_MAX;///< index into placements (UINT32_MAX: as above)
    uint32_t collider = UINT32_MAX; ///< havk zoneModels index (Collider source only)
    float world[16] = {};           ///< column-major, model-local -> map space (D4)
    ZoneSource source = ZoneSource::Collider;
};
struct ZoneScatter {
    bool present = false;
    std::vector<ZoneLayer> layers;
    std::vector<Zone> zones;
    std::vector<ZonePlacement> placements;
    std::vector<ZoneInstance> instances;
    uint32_t zonePlacementsLinked = 0, collidersLinkedByGeometry = 0, collidersUnlinked = 0;
};
```

`WorldScene` gains `ZoneScatter zones;`.

**The model table is shared.** Zone models join `WorldScene::models`. A model
that is already a prop keeps its index, and the new ones are appended after the
prop models in first-seen order. So `PropInstance::model` indices do not change.
The models that phase 1 and phase 2 need are added; unused `modelArray` entries
keep `model = UINT32_MAX`.

Consumers that assumed "every model has a prop instance" are updated:

- `model_load_order` and `rank_models` rank a model by its nearest prop **or
  zone** instance.
- The export counts zone-only models as used.

**The reader.** A new `Extractor::parseZon2()` reads every field above by
template name, version-agnostic, as `parseHavok` does. It is built on the
existing `parseMapZones` code, and an array that would run past the file reads
as empty. `parseMapZones` keeps its signature and is reimplemented over
`parseZon2`, with the two measured fixes:

- x is the anchor (`c − 1`);
- the model is `(f >> 1) & 7` (M2, M3).

So `gw2dat_cli map` prints the corrected positions and models (a change in
output, noted in the CLI's help text).

**Where it is built.** `world::build_zones(const MapZon2&, const MapHavok&,
const std::vector<MapProp>&, WorldScene&)` in `src/world/zones.cpp`, called by
`load_world` after `build_props` and `build_collision`. The `chunk zon2 … not
read` warning goes away.

For the prop cross-check (D3 step 4), `Extractor::MapProp` gains
`uint64_t guid`, read by template name. `parseHavok`'s `HavokPlacement` gains
`uint64_t token` for `propModels`. Neither changes an existing field.

### D3. Phase 1: linking `zoneModels` to zone models

The rule is applied in order. Every step is counted, and every count reaches
`WorldScene::warnings` when it is non-zero, or always where the table says.

1. **Decode** (the client decode, S4: run-length (count, value) pairs) every placement: zone, layer by `defToken` and `(f >> 4) − 1`,
   grid point `(c, r)`, and flag. A placement whose def or layer index does not
   resolve keeps `layer = UINT32_MAX`. These are counted, and none occur on the
   test maps.
2. **Anchor match.** For each `zoneModels` entry `e`, the candidates are the
   **type-3 placements** (layer `type == 3`; ruling S1: M2 was measured against
   type 3, and allowing type 1 or 2 caused geometry conflicts that M3 rules
   out) whose anchor `A(c, r)` lies within 24 units of `e.translate.xy`
   on both axes. The tolerance is half the measured cell. On a tie, the nearest
   by Euclidean distance wins, then the lower placement index. A placement
   already claimed is not a candidate.
   - Lookup goes through a hash of `(c, r)` over the placements, checking the
     3 × 3 neighbourhood of `round((x − x0)/48) + 1` and `round((y − y0)/48)`.
   - A match sets `placement.collider` and gives the entry its model:
     `layer.models[(f >> 1) & 7].fileId`.
3. **Geometry vote.** Each `geometryIndex` takes the fileId its matched entries
   agree on.
   - If they disagree, the majority is used, and one warning names the
     geometry and the counts. None occur on the test maps (M3).
   - An unmatched entry takes its geometry's fileId (`collidersLinkedByGeometry`:
     3 on Queensdale).
   - An entry whose geometry has no matched entry is **unlinked**
     (`collidersUnlinked`, expected 0). It is counted and warned: "zone: N
     colliders have no tree model (anchor and geometry both failed)". It keeps
     its hull and draws no tree.
4. **Prop cross-check.** For each geometry also used by a `propModels` entry
   whose `token` is a `prp2` prop's `guid`, the prop's fileId must equal the
   voted fileId. Each disagreement is counted in one warning (expected 0; M3).
5. **Instances.** Each linked entry gives one `ZoneInstance` with
   `source = Collider`, `collider = e`'s index, and the matched placement and
   layer (or `UINT32_MAX` for a geometry-only link).

What this proves and what it does not:

- **Proven by the data** (M2, M3, re-proven by the dat test):
  - the 48-unit grid, its south-west origin and the anchor column (the old
    walk's `c − 1`, the true cell `k − 1`; see M2's correction);
  - the model slot bits;
  - the geometry ↔ model link.
- **Not proven:** that the client uses this rule. The game could reach the same
  result another way. The rule is the data's, labelled `measured` with this
  spec's section as its source.

### D4. The visual transform of a zone instance

- **Collider instances.** `ZoneInstance::world = client_world_matrix(translate,
  rotate, scale)`. This is the client's prop rule (frame §5.1) at scale
  `scale`, without the ×32 and the z flip that turn a hull into map space
  (frame §7.2-7.3).
  - So `CollisionInstance::world = ZoneInstance::world · diag(32, 32, −32)` for
    the same entry, and a pure test pins it.
  - That the visual model takes the prop rule is UNPROVEN: it is inferred from
    props, where it is proven. The M3 containment measurement supports it.
    Setting `zoneVisualTransform`.
- **Scatter instances** (phase 2) use the same rule, from the decoded
  translate, rotate and scale.

### D5. Phase 2: the scatter rule

**Inputs.**

- Per placement: zone index, zone `seed`, `range`, `zPos`, `zoneFlags`,
  `(c, r)`, the walk position `k`, the layer's fields, the slot and bit 0.
- Per zone: `collideData` and `offsetData`.

**Outputs.**

- Per instance: `(u, v)` offset from the anchor, yaw, tilt (x and y rotation),
  scale and z.
- How many instances one placement makes. For type 3 this is 1, measured on
  the colliders: each collided placement has exactly one entry and no entry
  claims two placements. For types 1-2 it is UNPROVEN (U2).

**Work, in order.**

1. **Find the rule: static code first, then a data fit.**
   - **Static disassembly.** Look in the `Gw2-64.exe` IDB / Ghidra project
     the repo already uses (frame §5.1) for the function that expands a zone
     layer into instances, and transcribe its RNG and placement maths.
     - Ways in: the code that reads `PackMapZoneV23.flags` or `encodeData`,
       the layer's `scaleRange`, `rotRange` or `instanceScaleJitter`, or the
       `fadeRange` use.
     - Record the addresses.
     - The game is never launched (see "Static client only").
   - **Data fit**, if the static search fails or as a cross-check. Use the
     4750 anchor-matched collider transforms as labelled samples, against the zone's `seed`, the placement's index and cell, and the
   layer's `scaleRange`, `rotRange`, `instanceScaleJitter`, `encodeData` and
   `offsetData`.
   - Candidate generators: common small RNGs (LCG variants, xorshift, MT19937,
     splitmix and hash-of-index mixes) seeded from the zone seed, placement
     index or cell.
   - Draw orders to try: offset, yaw and scale, in each order.
   - Data fields to test as precomputed placements: `offsetData`, `encodeData`.
   - Stop at the first rule that passes step 2. Otherwise record what was
     tried.
   - Either way, the findings are recorded as a dated amendment to
     `docs/research/gw2-terrain-to-unity.md` §1.3.
2. **Validate against the colliders.** On all three maps, the rule must
   reproduce every anchor-matched collider entry (4378 + 88 + 284 = 4750):
   - x and y within 0.01 units;
   - yaw within 1e-4 rad (mod 2π);
   - tilt components within 1e-4 rad;
   - scale within 1e-5;
   - z within 0.5 units.

   Queensdale's three hand-placed entries (M2) are left out and listed. The
   colliders are game-computed placements of the same scatter (M4: collided
   placements are an ordinary subset by model), so a rule that matches all
   4750 is taken as proven.
3. **Hypotheses to test alongside the fit.** Each is labelled
   `measured` if it holds on all 4750 entries within the tolerances above, and
   UNPROVEN otherwise.
   - **Tilt** = the terrain normal at the instance (from `terrain_height_at`'s
     triangle), clamped to `rotRange` x/y.
   - **z** = `terrain_height_at(x, y)`.
   - **Scale** = `scaleRange[0] + Δ · k` for a per-layer step `Δ` (M5).
   - **`collideData`** is the per-placement ground (normal, z) of the five
     zones that have it.
4. **Outcome.** The manifest setting `zoneScatter` is one of:
   - `colliders`: phase 1 only. The remaining placements are not placed.
   - `exact`: step 2 passed. Every placement becomes instances by the proven
     rule; `source = Scatter`.
   - `densityMatched`: step 2 failed. Each placement without a collider becomes
     one instance, `source = Density`, by castlemist's own rule:
     - xy = anchor + (u, v), with u and v uniform in [−24, 24) (the measured
       range for type 3);
     - yaw uniform in `rotRange.z` (degrees, read as stored);
     - scale uniform in `scaleRange`;
     - tilt from step 3 if it was measured, else upright;
     - z = `terrain_height_at(x, y)`;
     - the numbers come from `splitmix64(mapFileId << 32 ^ placementIndex)`,
       drawn in the order u, v, yaw, scale.

     The setting is UNPROVEN, and each part of it is listed (Settings).
5. **Never doubled.** A placement with `collider != UINT32_MAX` makes no
   Scatter or Density instance. Its Collider instance is the exact one.

**Not applied in any outcome:**

- `probability` (layer and model): the flag already names the model, so the
  draw is taken as done offline (UNPROVEN, `zoneProbability`);
- `hslRanges` and `hslOffset` tint (`zoneTint`, castlemist's choice);
- `noise`, `sortGroup`, `subModel` and `zOffsets`, unless step 1 shows how
  they enter.

### D6. The Map Window

- **Layer.** `WorldLayer` gains `Foliage`, after `Water` and before `Count`.
  - On after `create()`.
  - A toolbar toggle **Foliage**, with a new control id added to the existing
    uniqueness test.
  - A Foliage sub-mode in the same toggle's menu: **Colliders / All**. Phase 1
    has only Colliders.
- **CPU data, off the UI thread.**
  - The map window's world worker runs `load_world`, then builds a
    `FoliageGrid` from `scene.zones.instances`. This is a pure, D3D-free header
    in `src/render/` (`world_foliage_grid.h`).
  - **Tiles** are 1536 units square (half a chunk; castlemist's choice,
    `foliageTileUnits`).
  - Per tile it holds the instance indices sorted by model, an AABB from the
    instances' world spheres (filled in as models arrive; until then the tile
    is not drawn), and the tile's largest `fadeRange` far value.
  - The grid is posted with the world (`WM_APP_MAPWIN_WORLD`).
  - `set_world` takes the scene and the grid. Its UI-thread time with foliage
    stays under 50 ms on Queensdale at full density (Performance).
- **Models.** Zone models stream through the existing `ModelQueue`, ranked by
  nearest prop or zone instance (D2). `add_models` places the zone instances
  of each arriving model, as it does for props, and updates the touched tiles'
  AABBs.
- **Fade.** Per instance, with `d` = the distance from the camera to the
  instance's world-sphere centre, and `[n, f] = layer.fadeRange` times
  `foliageDistanceScale` (a view setting, default 1):
  - `fade = 1` for `d ≤ n`;
  - `0` for `d ≥ f`;
  - linear in between.

  A Collider instance whose layer is unresolved uses the largest type-3
  `fadeRange` of the map. That `fadeRange` is a camera distance in map units
  is UNPROVEN (`zoneFade`).
- **Cull, per frame.**
  1. For each tile, skip it when the camera is farther from its AABB than its
     largest far value × scale, or when the AABB is outside the frustum.
  2. For each instance of a kept tile, skip it when `fade = 0` or its sphere
     is outside the frustum.
  3. Choose the LOD by screen size (the props' rule).
  4. Bucket by (mesh, LOD).

  Visible instances are capped at `foliageMaxVisible` = 20 000, nearest first
  by tile distance. That is about 4× M6's max; castlemist's choice. Instances
  past the cap are counted.
- **Draw.**
  - A `FoliageBatch` uses the `InstancedBatch` pattern with its own instance
    stream: a row-vector 4×3 world plus one float fade, 64 bytes. The prop
    stream and its pipelines are unchanged.
  - Two pipeline variants: AO look (Geometry) and textured (Materials).
    Neither is a game shader.
  - Fade is a **screen-door dither** in the pixel shader: discard when
    `fade < bayer4x4(pixel)`. Depth writes stay on, and no sorting is needed.
  - Cutout follows the material's alpha mode as for props.
  - Under **Shader** shading, foliage draws in the Materials look and is
    counted (`shaderFoliage`).
  - No wind (D10).
- **Collision and walk.** Unchanged: zone hulls are already in
  `scene.collision`.
- **Picking.** `pick()` still returns props only. Picking zone instances is
  stage B.
- **Stats.** `WorldViewStats` and the headless stats line gain:
  - `foliageInstances` (placed and total), `foliageVisible`, `foliageDraws`,
    `foliageKtris`, `foliageTiles`, `foliageCapped`;
  - `foliageCullMs` (CPU, culling and bucketing);
  - `zoneTrees` (Collider instances) and `zoneUnlinked`.
- **Headless.** Two new entries in the env table in `include/castlemist/core/env.h`:
  - `GW2_MAPWIN_FOLIAGE=off|colliders|all`;
  - `GW2_MAPWIN_FOCUS=zonetree` frames the Collider instance nearest the
    camera's start point from 4× its radius, with the Collision layer on, and
    reports its screen rectangle as `treeRect`.

### D7. Export

- **Records, not JSON instances.** Queensdale's 217 114 placements as manifest
  JSON (`m` plus `unity`, about 400 bytes each) would add about 85 MB to
  `manifest.json`. So foliage goes into one binary file per cell,
  `foliage/<cellId>.bin`, written only for cells with at least one record.
- **Record layout (`castlemist-foliage-1`):** 100 bytes, little-endian, no
  padding.

  | offset | type | field |
  |---|---|---|
  | 0 | u32 | `model`: index into the manifest's `models[]` |
  | 4 | u16 | `layer`: index into `foliage.layers[]`, 0xFFFF for a collider linked by geometry only |
  | 6 | u16 | `flags`: bit 0 has a collider; bits 1-2 source (0 collider, 1 scatter, 2 density); bit 3 Terrain tree (D8); bit 4 outside the terrain (bucketed by clamping, as props) |
  | 8 | u32 | `placement`: index into the map's zone placements, 0xFFFFFFFF for a geometry-only collider (stable for a given dat; the thinning key, D9) |
  | 12 | f32[12] | `m`: map-space 3 × 4, column-major, absolute (columns 0-2 the 3 × 3, column 3 the translation) |
  | 60 | f32[3] | `unity.t`: local to the cell node, before `unityScale` (the props' convention, export spec amendment W7) |
  | 72 | f32[4] | `unity.r`: quaternion x, y, z, w |
  | 88 | f32 | `unity.s`: uniform scale |
  | 92 | f32 | `unityYaw`: rotation about Unity +Y in radians, for `TreeInstance.rotation` (sign UNPROVEN, `terrainTreeYawSign`) |
  | 96 | f32 | `tiltDeg`: angle between the instance's up and map up, degrees |

  Records are sorted by (model, placement), so a file is deterministic and an
  importer can group by model in one pass. Queensdale at full density is about
  21.7 MB.
- **Bucketing.** By the translation's (x, y), with `chunk_of_point`, as props.
  Foliage triangles do **not** count toward `cellMaxTriangles`, because foliage
  is distance-culled (castlemist's choice). Each cell reports them as
  `foliage.triangles`.
- **Models.** Every model with a record is written as a VRChat model folder
  `models/<fileId>/`, as props are. Zone-only models are added to `models[]`.
  - Each foliage model gets `wind`: `true` when a material's traced VS uniform
    list (the `vsUniforms` the Shader mode reads) contains `VegPara`, `false`
    when traced without it, and `null` when not traced.
  - It is recorded and not acted on (D10).
- **`unityTerrain` block** (phase 3; on by default, `--no-unity-terrain` skips
  it). It follows the research note §5 and §8.2 exactly, baked-layer mode only:
  - **Tiles.** One tile per 4 × 4-chunk block (`cell_block_chunks`), not per
    cell.
    - `heights`: `terrain/<tile>.r16`, 129 × 129 uint16 LE, row 0 = south and
      column 0 = west, normalised by the map-wide `hMin` and `hMax` (§5.2).
    - Edge blocks are padded by clamping, and the padded quads are holes in
      `terrain/<tile>_holes.png` (128 × 128, white = surface, row 0 north).
  - **Baked layer.** `terrain/<tile>_albedo.png` is assembled pixel-exact from
    the block's cell albedos: same `texelsPerChunk`, and cells never cross a
    block.
  - **Detail layers.** Records whose layer type is 1 or 2, whose model has one
    mesh and one material, and which lie inside the tile go here.
    - One 256 × 256 8-bit PNG per (tile, detail prototype),
      `terrain/<tile>_d<k>.png`, row 0 north.
    - Value = the number of such records in that 48-unit cell, clamped to 255
      (with a warning). The cell is
      `(⌊(x − tileX0)/48⌋, ⌊(tileY1 − y)/48⌋)`.
  - **Trees.** Records whose layer type is 3, lie inside the tile, and have
    `tiltDeg ≤ terrainTreeMaxTiltDeg` (5, castlemist's choice) get flag bit 3.
    - They are written to `terrain/<tile>_trees.bin`, 24 bytes each:
      u32 prototype; f32 u, v (0..1 along Unity +X and +Z from the tile's
      south-west corner); f32 h (0..1 of the tile's height range); f32 yaw;
      f32 scale.
    - Type-3 records above the tilt limit stay GameObject instances in Terrain
      mode, because `TreeInstance` is yaw-only.
- **Manifest additions** are optional, so the version stays 1 (export spec,
  Versioning). See Data formats.
- **CLI.** `world-export` gains `--foliage off|colliders|all` and
  `--no-unity-terrain`. The default is `all`; phase 1 has `colliders` only, and
  `all` means whatever `zoneScatter` the build has.
- **Single-file `--out`.** Unchanged; it has no foliage (Out of scope).

### D8. Importers

**Unity** (`tools/unity/Castlemist/Editor/`). The importer window gains:

- **Terrain:** Mesh (default) / Unity Terrain (baked layer).
- **Foliage:** Off / Collider trees / Trees / All. The default is All.
- **Density** 0..1 (default 1) and **Distance** ×0.1..2 (default 1), per D9.
- A **Quest preset** button (D9).

Builds by mode:

- **Mesh terrain: instances and patches.**
  - **Trees** (layer type 3, and geometry-only colliders): one prefab instance
    per record under `Cell_<id>/Foliage/Trees`, with `unity.t/r/s` as given.
    - The prefab is `Prefabs/Foliage/<fileId>.prefab`: the model prefab plus a
      `LODGroup` with LOD0 only, culled at `cullHeight` (below).
    - `enableInstancing` on, `BatchingStatic` off (instancing needs it),
      occludee static on.
  - **Types 1-2: patches.** Records of one model within a 1536-unit square
    (`foliagePatchUnits`) of a cell are combined into one mesh asset
    `Foliage/<cellId>/<fileId>_<k>.asset`.
    - `Mesh.CombineMeshes`, sub-meshes kept, each record's `unity` TRS
      relative to the patch origin, at most 65 535 vertices per patch (split
      further in record order).
    - One `MeshRenderer` with the model's materials and a `LODGroup` culled at
      `cullHeight` for the patch size.
    - Why patches: one GameObject per placement would be 207 k GameObjects on
      Queensdale.
  - **`cullHeight`** `= clamp(size / (2 · fadeFar · D · tan(fov/2)), 1e-4, 1)`,
    where:
    - `size` is the prefab's or the patch's bounds diagonal;
    - `D` is the Distance knob;
    - `fov` is `lodCullFovDeg` (60, castlemist's choice; VRChat's FOV
      UNPROVEN);
    - `fadeFar` is the largest far value among the model's layers.
- **Unity Terrain (baked layer).**
  - **Per tile.**
    - A `TerrainData` asset: `heightmapResolution` 129, `size` =
      (12288, hMax − hMin, 12288) × `unityScale`, `SetHeights` from the
      `.r16`, `SetHoles`.
    - One `TerrainLayer` whose diffuse is the tile albedo, with `tileSize` =
      the tile's size.
    - `SetDetailResolution(256, 32)`, detail prototypes, `SetDetailLayer` from
      the PNGs.
    - Tree prototypes and `treeInstances` from the `.bin`.
    - A `Terrain` with a `TerrainCollider`, and `materialTemplate`
      Nature/Terrain/Standard (research §3).
    - `SetNeighbors` on every tile.
  - **Layout.** Tiles sit under an unscaled sibling root `GW2_<id>_Terrain`,
    with sizes and positions already × `unityScale` (research §8.3). In this
    mode the cells' mesh terrain and its `MeshCollider` are not instantiated.
    The `.glb` keeps them, so switching back is a re-import.
  - **Detail prototypes.**
    - `usePrototypeMesh` and `useInstancing`; `InstanceCountMode`.
    - Width and height range = the records' scale range.
    - `alignToGround` 0, or 1 if phase 2 measured ground-aligned tilt.
    - `noiseSeed` 0.
    - Within a 48-unit cell, Unity places the instance itself. So details
      match GW2's count per cell, not its positions (`detailPlacement:
      perCell`).
  - **Tree prototypes.** The foliage prefab (with its `LODGroup`); no capsule
    collider, because the `havk` hulls already collide.
  - **Distances.**
    - `treeDistance` = the largest type-3 `fadeFar` × `unityScale` × D.
    - `treeBillboardDistance` = `treeDistance` (GW2 has no billboards; how
      Unity treats a mesh prefab with a `LODGroup` at that distance is
      UNPROVEN).
    - `detailObjectDistance` = the largest type 1-2 `fadeFar` × `unityScale`
      × D, clamped by Unity to 250.
  - **Not Terrain trees.** Type-3 records without flag bit 3 are placed as
    GameObject instances, as in mesh mode.
  - **Terrain frame check.** `Terrain.SampleHeight` at 32 points that the
    manifest lists (`unityTerrain.check`: Unity-space xz and the expected
    height). A mismatch over 0.5 units × `unityScale` is an error naming the
    axis, as the frame probe does.
- **Summary line** gains `foliageTrees= foliagePatches= terrains= detailPrototypes=
  detailCells= treeInstances=`, in the format of `validate_manifest.py --plan`.

**Blender** (`tools/blender/castlemist_world.py --import`).

- **One geometry-nodes object per (cell, model)** under `Cells/<id>/Foliage`:
  - a mesh with one vertex per kept record, at the Blender position;
  - point attributes `cm_rot` (Euler XYZ) and `cm_scale`, from
    `C_b M C_b⁻¹`, computed by the script as it is for props;
  - a modifier using one shared node group, `CM_Foliage` (Instance on Points
    → Collection Info of the model's collection, Rotation and Scale from the
    attributes, not realised).

  This is used for trees as well, because the import of 2354 prop empties
  already took 413-450 s (export spec amendments).
- **Options.** `--foliage off|colliders|trees|all` (default all) and
  `--foliage-density f` (D9).
- **`--verify`** compares `depsgraph.object_instances` matrices for the
  foliage objects with `C_b M C_b⁻¹`:
  - 3 × 3 within 1e-4;
  - translation within 1e-4 · max(1, |t|).

  It also checks that instance counts equal the plan's.

### D9. Quest and performance knobs

All are castlemist's choice and listed in `settings`. They are importer-side,
so one export serves PC and Quest.

- **Foliage.**
  - Off.
  - Collider trees: only records with bit 0 of `flags`.
  - Trees: type-3 records and geometry-only colliders.
  - All.
- **Density.** A record is kept when
  `fmix32(placement) < density · 2³²`, where `fmix32` is MurmurHash3's 32-bit
  finaliser.
  - Collider trees are always kept when trees are shown, because thinning them
    would leave invisible walls.
  - Geometry-only colliders (`placement` 0xFFFFFFFF) are always kept.
  - `validate_manifest.py --plan --foliage-density d` prints the kept counts,
    and both importers must equal them.
- **Distance.** It multiplies every `fadeFar` the importers use.
- **Quest preset.** Terrain Mesh, Foliage Collider trees, Distance 0.5. Foliage
  is otherwise left to the user (research §4.3: Queensdale's terrain alone is
  1.09 M triangles against VRChat's ~250 k Android world budget).

### D10. Wind: none in this sub-project

GW2 foliage sways in its vertex shader: the foliage and tree AMATs declare
`Wind`, `WindGust`, `VegPara` and `Time`, and take a vertex `COLOR` (research
§1.3). The formula is not transcribed.

- Unity Wind Zones move only Tree Editor trees and grass-mode details; mesh
  prefabs and instanced mesh details do not sway (research §2, §6.3).
- Poiyomi's vertex options are unverified for this.

So:

- the Map Window draws foliage static;
- the export marks wind-capable models (`foliage.models[].wind`, D7) and
  leaves vertex colours as the model export writes them;
- the importers add no sway.

Setting `foliageWind: none` (castlemist's choice). Stage B: transcribe one
foliage VS, then add a sway option per target.

### D11. Settings that travel

Each row is written to `settings` with value, status and source, and each
UNPROVEN row is also a line in `warnings` (export spec D3).

| setting | value | status, source |
|---|---|---|
| `zoneGrid` | 48 units, origin parm (x0, y0), rows north, anchor at the cell's vertex (the old walk's c − 1) | measured, this spec M2 (corrected, S4) |
| `zoneModelSlot` | `(flag >> 1) & 7` into `modelArray` | measured, M3 |
| `zoneFlagBit0` | as stored, not used | UNPROVEN meaning, M4 |
| `zoneTreeLink` | anchor ±24 to a type-3 placement, then geometry vote | measured, D3 (S1) |
| `zoneQueryFlags` | 0 (an unfiltered query: every instance emitted) | UNPROVEN, transcribed from the client (Task 10; added in Task 16) |
| `zoneVisualTransform` | `client_world_matrix(translate, rotate, scale)` | UNPROVEN, D4 |
| `zoneScatter` | `colliders` / `exact` / `densityMatched` | measured if exact; UNPROVEN if densityMatched, D5 |
| `zonePlacementMultiplicity` | 1 instance per placement | measured for type 3 (M2); UNPROVEN for types 1-2 |
| `zoneJitter`, `zoneYaw`, `zoneScale`, `zoneTilt`, `zoneZ` | the D5 rule in force | measured or UNPROVEN, each per D5 step 3-4 |
| `zoneProbability` | applied offline (not re-applied) | UNPROVEN, D5 |
| `zoneTint` | `hslRanges` / `hslOffset` not applied | castlemist's choice |
| `zoneFade` | `fadeRange` = camera distance, map units | UNPROVEN, D6 |
| `zoneLayerTypeTarget` | 1-2 detail, 3 tree | castlemist's choice from measured sizes (research §1.3) |
| `foliageWind` | none | castlemist's choice, D10 |
| `terrainTreeMaxTiltDeg` | 5 | castlemist's choice, D7 |
| `terrainTreeYawSign` | `unityYaw` = yaw of `unity.r` about +Y | UNPROVEN until the checklist compares a Terrain tree with its GameObject |
| `detailPlacement` | count per 48-unit cell, Unity positions | castlemist's choice, D8 |
| `foliagePatchUnits` | 1536 | castlemist's choice |
| `foliageTileUnits` (Map Window) | 1536 | castlemist's choice |
| `lodCullFovDeg` | 60 | castlemist's choice; VRChat FOV UNPROVEN |
| `terrainHeightRowOrder` | row 0 south | UNPROVEN until the terrain frame check passes (research §5.2) |
| `terrainColliderDiagonal` | Unity's | UNPROVEN against `CollisionIndex`'s NE-SW (research §5.5; NW-SE until the 2026-10-18 fix wave) |
| `foliageThinHash` | `fmix32(placement)` | castlemist's choice, D9 |

## Architecture

```
extract     gw2model.hpp      + Extractor::parseZon2() -> MapZon2 (raw, by template name);
                                parseMapZones reimplemented over it (anchor x, slot bits)
world       zones.h/.cpp      ZoneScatter types; build_zones(MapZon2, MapHavok, props, scene)
                              (decode, anchor link, geometry vote, prop cross-check, D4 matrices)
            zone_scatter.h    phase 2: scatter_instances(scene, ZoneScatterRule) (exact or density)
            load_world.cpp    calls build_zones; zone models join scene.models
            stream.cpp        rank by prop or zone instance
            terrain_height.h  + terrain_heightfield(terrain, block) -> 129² grid + padding mask (phase 3)
render      world_foliage_grid.h  FoliageGrid (D3D-free, built on the load worker), fade(d, n, f)
            world_view.cpp    WorldLayer::Foliage, FoliageBatch, dither variants, stats
exportgltf  world_foliage.cpp foliage/<cell>.bin, foliage block, wind flags
            world_unity_terrain.cpp  terrain/<tile>.r16, holes, albedo assembly, detail PNGs, trees.bin, check points
            world_manifest.cpp + foliage / unityTerrain / per-cell foliage / counts.foliage
ui          map_window*.cpp   Foliage toggle and sub-mode; worker builds FoliageGrid
tools       gw2dat_cli        world-export --foliage, --no-unity-terrain; map prints corrected zones
            world/            schema + validate_manifest.py (bins, tiles, --foliage-density)
            blender/          castlemist_world.py geometry-nodes foliage
            unity/            importer modes, patches, Terrain build, terrain frame check
```

**Layer rules hold:**

- `world` depends on `extract` only;
- `exportgltf` depends on `extract` and `world`, never on `render`;
- `render` may use `exportgltf`;
- `FoliageGrid` is pure, and nothing below `render` touches Direct3D.

The wind flag reads the model's traced VS uniform names from the `extract`
model data, not from `render`.

## Data formats

### Output folder additions

```
<out>/
  foliage/<cellId>.bin                castlemist-foliage-1 records (D7)
  terrain/<tile>.r16                  129×129 uint16 LE, row 0 south
  terrain/<tile>_holes.png            128×128, white = surface, row 0 north (edge tiles only)
  terrain/<tile>_albedo.png           the block's albedo, assembled
  terrain/<tile>_d<k>.png             256×256 8-bit detail counts, row 0 north
  terrain/<tile>_trees.bin            24-byte Terrain tree records
```

`<tile>` is the block id `t<cx0>_<cy0>` in chunk coordinates.

### manifest.json additions (version 1, all optional)

```jsonc
"foliage": {
  "scatter": "colliders" | "exact" | "densityMatched",
  "recordLayout": "castlemist-foliage-1", "recordBytes": 100,
  "layers": [ { "index": 0, "zoneDef": 17382, "layer": 0, "type": 3, "width": 4, "height": 6,
                "radiusGround": 2, "fadeRange": [7400, 7500], "scaleRange": [0.5, 1.5],
                "rotRange": [[0, 90], [0, 90], [0, 360]], "layerFlags": 142,
                "material": 184994 | null, "target": "tree" | "detail",
                "models": [12, 40, 41] } ],                  // manifest models[] indices by slot; null for an unused slot
  "models": [ { "model": 12, "zoneTypes": [3], "fadeFar": 7500, "wind": true | false | null,
                "terrainDetail": false } ],                  // terrainDetail: eligible as a Unity detail (one mesh, one material, types 1-2)
  "colliders": { "entries": 4381, "linkedByAnchor": 4378, "linkedByGeometry": 3, "unlinked": 0,
                 "geometries": 45, "propCrossChecks": 37, "propDisagreements": 0 }
},
"cells": [ { ...,
  "foliage": { "file": "foliage/c4_8_4x4.bin", "records": 5123, "colliderTrees": 120,
               "triangles": 401234 } | null } ],
"unityTerrain": {
  "tileChunks": 4, "heightmapResolution": 129, "detailResolution": 256, "detailCellUnits": 48,
  "heightRange": { "min": hMin, "max": hMax },                // Unity up, map units
  "tiles": [ { "id": "t4_8", "chunks": [4, 8, 7, 11], "position": [x, y, z], "size": [12288, h, 12288],
               "heights": "terrain/t4_8.r16", "holes": "terrain/t4_8_holes.png" | null,
               "albedo": "terrain/t4_8_albedo.png", "albedoSize": [1024, 1024],
               "details": [ { "prototype": 0, "counts": "terrain/t4_8_d0.png", "records": 3121 } ],
               "trees": { "file": "terrain/t4_8_trees.bin", "records": 210 } | null } ],
  "detailPrototypes": [ { "model": 17, "minScale": 0.5, "maxScale": 1.5, "fadeFar": 3000 } ],
  "treePrototypes":   [ { "model": 12, "fadeFar": 7500 } ],
  "check": [ { "unity": [x, z], "height": y } ]              // 32 points, Unity space before unityScale
},
"counts": { ..., "foliage": { "records": 217114, "colliderTrees": 4381, "scatter": 0, "density": 212733,
                               "modelsFoliageOnly": 117, "terrainTrees": 9800, "detailRecords": 206818 } }
```

- The numbers in the example are illustrative, apart from the Queensdale
  collider figures (M1-M3) and the counts derived from M1.
- `tiles[].position` and `size` are in Unity axes before `unityScale`, as the
  research note writes them.
- The importer multiplies them by `unityScale` itself. This is scaling, not a
  change of frame.
- The schema adds these properties as optional, keeping
  `additionalProperties: false` at every level, and defines the record layouts
  in its description text. `validate_manifest.py` reads the binary files.

### Unity mapping additions (the table the C# and `--plan` share)

| manifest | Unity object |
|---|---|
| `foliage.models[i]` used by a tree record | `Prefabs/Foliage/<fileId>.prefab` (model prefab + LODGroup) |
| tree records (mesh mode; Terrain mode without bit 3) | prefab instances `Cell_<id>/Foliage/Trees/<fileId>_<placement>` |
| type 1-2 records (mesh mode; Terrain mode when the model is not `terrainDetail`) | `Cell_<id>/Foliage/Patches/<fileId>_<k>` + `Foliage/<cellId>/<fileId>_<k>.asset` |
| `unityTerrain.tiles[j]` (Terrain mode) | `GW2_<id>_Terrain/Tile_<id>`: `Terrain` + `TerrainCollider`, `TerrainData/<id>.asset`, `TerrainLayers/<id>.terrainlayer` |
| `detailPrototypes`, `treePrototypes` | per-tile prototypes from the same prefabs |

## Front end

- **CLI.** See D7. `gw2dat_cli world` prints the zone summary:
  - placements by type;
  - colliders linked (anchor, geometry), unlinked;
  - prop cross-checks;
  - `zoneScatter`.
- **UI.**
  - The Map Window's **Foliage** toggle and its Colliders / All sub-mode.
  - The status line gains `fol V/N` (visible / placed).
  - File ▸ Export World (Map)… uses the default `--foliage all`.

## Performance and budgets

**Measured inputs** (Queensdale):

- 217 114 placements;
- within their fade range from a placement-like point: p50 2 610, p90 3 658,
  max 4 955 (M6, one instance per placement);
- triangles by type (research §1.3): type-1 median 26 (max 280), type-2 45,
  type-3 248 (max 3 181);
- 154 distinct zone models (M3).

**Map Window targets, Queensdale, phase 2 at full density.** These are measured
by the headless stats line. Each figure is recorded in the amendments with the
build type it was taken on.

| item | target |
|---|---|
| UI-thread `set_world`, with the FoliageGrid handed over | < 50 ms |
| every `add_models` batch, zone placement included | < 50 ms (the existing rule) |
| `foliageCullMs`, median frame at a p90 view | ≤ 2 ms |
| whole-frame CPU increase with Foliage on against off | ≤ 3 ms median |
| foliage GPU time (WorldView::bench timestamps) | ≤ 4 ms median |
| visible instances | ≤ `foliageMaxVisible` 20 000; `foliageCapped` reported |
| CPU memory for instances and grid | about 17 MB (217 k × (64-byte matrix + 16 bytes)) |

If a target is missed, the levers are, in order: the tile size; then the cap;
then a per-tile static instance buffer instead of the per-frame stream. The
choice is recorded in the amendments.

**Export.**

- Up to 154 more model folders on Queensdale (fewer where props already wrote
  them), plus about 22 MB of records.
- Terrain tiles: 35 × (33 KB RAW, the albedo copy, and small, mostly empty
  count PNGs).
- The model stage dominates, as in sub-project 4. Measured times and sizes go
  into the amendments.

**Unity.** On Queensdale, patches are on the order of a few thousand mesh
assets. The import time and the Frame Debugger draw counts per mode are
checklist items.

## Error handling

`build_zones` never throws for bad data:

- An unresolved def or layer, a slot past `modelArray`, or a model that fails
  to load is counted in one warning each. The placement or instance is kept
  where it can be drawn, and skipped otherwise.
- An unlinked collider keeps its hull, draws no tree, and is warned (D3).
- No `zon2` gives `zones.present = false` and no warning: some maps have none.
- In the export, a foliage model that fails is `status: failed`, its records
  are skipped and counted in `counts.foliage`.
  - A failed record write stops the export and leaves no `manifest.json`, as
    any failed write does.
- Importers:
  - a missing bin or tile file is an error naming it;
  - a record whose `model` is failed is skipped and counted;
  - a failed terrain frame check is an error naming the axis.

## Testing

**Pure, world (`cm_test_world`).**

- **Decode** (synthetic `MapZon2`): the run-length walk (skip and flag pairs,
  row wrap, zero flags skipped), `(c, r)` and the anchor formula, the layer and
  slot bits, and unresolved defs, layers and slots counted.
- **Link.**
  - An entry at anchor + (23, −23) links, and one at +25 does not.
  - A tie takes the nearest, and a claimed placement is not reused.
  - The geometry vote fills an unmatched entry; a conflicting geometry warns
    with its counts.
  - A prop cross-check disagreement warns.
  - An entry with no anchor and no geometry is unlinked and warned.
- **Matrices.** `CollisionInstance::world == ZoneInstance::world ·
  diag(32, 32, −32)` within 1e-6 for the same entry.
- **Models.** Zone-only models are appended after prop models, shared ones
  keep their index, `PropInstance::model` is unchanged, and
  `model_load_order` ranks by zone instances too.
- **Phase 2.**
  - The density rule is deterministic: same map and placement give the same
    result.
  - u and v are in [−24, 24), yaw in `rotRange.z`, scale in `scaleRange`.
  - No placement with a collider makes a second instance.
  - The exact rule, if found, has its own unit vectors, from the static code
    or the collider samples.
- **Thinning.** `fmix32` known vectors. Kept counts at density 0, 0.5 and 1.
  Colliders are always kept.

**Render (`cm_test_render`).**

- `fade(d, n, f)` at and between the ends.
- `FoliageGrid`:
  - instances in the right tile;
  - AABB and largest far value;
  - a tile beyond its far value is culled;
  - a camera inside a tile;
  - the cap keeps the nearest tiles.
- The Bayer threshold matrix.
- Existing render tests are unchanged, since the prop stream is untouched.

**exportgltf (`cm_test_exportgltf`).** A synthetic scene: 2 × 2 chunks, one
collider tree, one geometry-only collider, two grass placements, one model
shared with a prop.

- Records parse at 100 bytes, sorted. `m` round-trips to `unity` TRS by
  `map_matrix_to_unity`. The flags are right.
- Per-cell counts add up to `counts.foliage`.
- The `unityTerrain` tile:
  - the `.r16` row 0 is south and normalised by the map range;
  - an edge tile is padded with holes;
  - the assembled albedo equals the cell albedo pixels;
  - detail PNG counts sum to the detail records;
  - tree u, v and h reproduce the record's position.
- The manifest is still written last.

**Schema and dry run (ctest, Python 3).**

- `validate_manifest.py` on the synthetic export passes. It checks:
  - every bin size = records × 100;
  - indices in range;
  - every record's `unity` TRS reproduces `m` relative to the cell origin,
    within 1e-4;
  - tile files and sizes;
  - `check` points inside their tiles.
- Corrupted copies each fail with their message: a truncated bin, a model out
  of range, an extra foliage field.
- `--plan --foliage-density 0.5` prints the expected counts.

**Blender (ctest label `blender`).**

- `--import` on the synthetic export: one geometry-nodes object per (cell,
  model), with instance counts and matrices as D8.
- At `--foliage-density 0.5`, the counts equal the plan's.
- `--foliage colliders` keeps only bit-0 records.

**Unity (optional local batchmode, 2022.3.22f1; not CI).**

- It compiles.
- `PLAN` equals the dry run in both modes.
- The terrain frame check passes on the synthetic export and on Spirit Vale.

**Dat (`cm_test_world_dat`, skipped without `GW2_TEST_DAT`).**

- **Phase 1 (the acceptance).** On each test map:
  - Placements decode to 217 114 / 32 448 / 7 642, with the M1 type split.
  - `zoneModels` entries 4381 / 88 / 284. Linked by anchor 4378 / 88 / 284,
    by geometry 3 / 0 / 0. **Unlinked 0.**
  - Geometries 45 / 7 / 1, each with one fileId. Prop cross-checks 37 / 4 / 0,
    disagreements 0.
  - Every anchor offset is within ±24.01.
  - **Every zone `CollisionInstance` has a `ZoneInstance` with the same
    `zoneModels` index, whose model loads with drawable meshes.** That is
    "every zone collider has a visible tree".
  - Hull-in-model containment in model-local space, over every linked
    geometry: median ≥ 0.95 and min ≥ 0.8 (measured on 12: median 1.0, min
    0.93).
  - The `zon2 not read` warning is gone.
- **Phase 2.**
  - With `exact`: the 4750 reproductions to D5's tolerances.
  - With `densityMatched`: every placement without a collider has exactly one
    Density instance, and 99 % lie inside the terrain rects (the props'
    figure).
  - In both cases the per-step hypotheses' results are printed for the
    amendments.
- **Export (Spirit Vale, then all three measured):**
  - `validate_manifest.py` passes;
  - `counts.foliage.colliderTrees` = 284;
  - records = `scene.zones.instances.size()`;
  - every collider-tree record's `m` equals its `ZoneInstance::world`;
  - the terrain `check` heights equal `terrain_height_at`.

**Headless Map Window.**

- `GW2_MAPWIN=192711 GW2_MAPWIN_FOLIAGE=colliders GW2_MAPWIN_FOCUS=zonetree`
  writes a screenshot where `treeRect` is non-empty, and the stats line shows
  `zoneTrees=4381 zoneUnlinked=0`.
- With `GW2_MAPWIN_FOLIAGE=all` (phase 2): the budget figures above.

**Manual checklists (`docs/testing.md`, "World foliage").**

- **Map Window, Queensdale.** Walk through a wooded area: every tree you bump
  into is drawn, and no collider stands alone. Also check:
  - Foliage on and off;
  - fade at distance without popping;
  - the Collision layer over trees;
  - frame time on the status line.
- **Unity, VRChat 2022.3.**
  - Both modes on Spirit Vale and Queensdale: compile, PLAN equality, the
    terrain frame check.
  - Trees on colliders, with a CharacterController walk.
  - A Terrain tree beside a GameObject copy of the same record, for
    `terrainTreeYawSign`.
  - Detail density against the Map Window.
  - The Quest preset's counts; Frame Debugger draws; import time.
- **Blender.** The scene viewed, the foliage instances in place, a Cycles
  render at the spawn.
- **Results.** Recorded with dates in this spec's amendments. Anything not
  tried is listed as not tried.

## Done means

- **Phase 1.**
  - The dat acceptance passes on all three maps.
  - The headless zone-tree capture works.
  - The Map Window checklist's walk has been run on Queensdale.
  - Collider trees are exported as records and placed by both importers:
    Blender headless, Unity dry run, and batchmode where available.
- **Phase 2.**
  - `zoneScatter` is `exact` with the reproduction passing, or
    `densityMatched` with every assumption labelled.
  - The static-code and data-fit research is recorded in the research note,
    as found or as not found.
  - The Map Window budgets hold on Queensdale, or are recorded as missed with
    the lever taken.
- **Phase 3.**
  - Schema and dry run pass on all three maps.
  - Blender headless passes with the foliage options.
  - The Unity importer builds both modes with the terrain frame check
    passing, and the checklist is recorded. If no VRChat 2022.3 project was
    available, done is "dry run + schema + batchmode", stated as such.
- **Always.**
  - Every UNPROVEN setting is in `settings`, `warnings` and its code comment,
    with its source.
  - All pure, render, exportgltf, schema, Blender and dat tests pass.
  - One whole-branch review at the end.

## Plan shape (at most 15 tasks)

1. `Extractor::parseZon2`, and `parseMapZones` over it with the anchor and
   slot fixes (`gw2dat_cli map` output re-checked).
2. `world/zones`: types, decode and `build_zones` linking, with pure tests.
   `load_world` wiring, the shared model table and the stream ranking.
3. Phase-1 dat test (the acceptance numbers, containment).
4. `FoliageGrid`, fade, `FoliageBatch` and the dither variants in WorldView,
   the Foliage layer and stats, with render tests.
5. Map Window: the worker builds the grid, toggle and sub-mode, headless
   `FOLIAGE` / `FOCUS=zonetree`. Capture and Queensdale walk checklist.
6. Export: foliage records and the per-cell and foliage manifest blocks, the
   wind flag, CLI `--foliage`, schema and validator for records.
7. Importers, phase-1 level: Unity tree prefab instances, Blender geometry
   nodes, with the headless Blender test.
8. Phase 2 research: find the scatter rule in the static disassembly, else fit
   it from the data against the colliders, and write the research note
   amendment.
9. Phase 2 implementation: `zone_scatter` (exact, or density with the step-3
   hypotheses tested), pure tests and the dat reproduction test.
10. Map Window at full density: measure the budgets, take levers if needed,
    record the amendments.
11. `terrain_heightfield` and the `unityTerrain` writer (RAW, holes, albedo
    assembly, detail PNGs, trees.bin, check points), with tests.
12. Unity importer: Terrain mode, patches, knobs, Quest preset, terrain frame
    check, summary.
13. Blender knobs and `--verify`; validator `--foliage-density`.
14. Dat export test and measurements on all three maps, into the amendments.
15. Docs: `docs/testing.md` checklists, research note cross-links,
    `docs/using-castlemist.md` (Foliage toggle, CLI flags). The whole-branch
    review.

## Risks and UNPROVEN

| item | status | what would settle it |
|---|---|---|
| the client uses the anchor and slot rule | data-measured | the 4750-collider reproduction (phase 2, step 2) |
| U1. visual transform = prop rule at `scale` | UNPROVEN, supported by M3 containment | the collider-to-model containment over all 4750 entries |
| U2. one instance per type 1-2 placement | UNPROVEN (type 3 measured) | the static code or the data fit; else density fallback |
| U3. tilt is ground alignment | hypothesis (M5) | D5 step 3 on 4750 colliders |
| U4. walk position `k − 1` against column `c − 1` at row starts | settled by the client decode (true cell `k − 1`, S4) | — |
| exact scatter (offset, yaw, scale, z, RNG, seed) | UNPROVEN | D5 steps 1-2 |
| flag bit 0, `layerFlags`, `zoneFlags`, `noise`, `zOffsets`, `encodeData`, `collideData`, `offsetData` meaning | UNPROVEN | the static code or the data fit (D5 step 1); else stays a setting |
| `fadeRange` as camera distance in map units | UNPROVEN | a visual check against the export at known distances |
| 107 Queensdale collider scales outside `scaleRange` | unexplained | D5 |
| 3 hand-placed Queensdale entries | linked by geometry; not scatter | none needed |
| Unity: heightmap row order, texel centre, tree yaw sign, billboard distance with LODGroup prefabs, scaled-parent Terrain | UNPROVEN | terrain frame check, checklist |
| Unity: TerrainCollider diagonal | UNPROVEN | the 32-ray check against `Terrain.SampleHeight` |
| VRChat: Terrain avatar-load stutter, lightmaps on terrain and details; instanced details unlit by lightmaps and probes | reported or documented (research §3, §2) | the user's VRChat test |
| Unity details with multi-mesh or multi-material models | UNPROVEN; such models are kept out of details | — |
| Quest viability at any foliage density | not viable in full (research §8.5) | the Quest preset is collider trees only |
| Map Window budgets at 217 k | estimated from M6 | task 10 measurement |

## Out of scope

- Foliage sway (wind), D10.
- HSL tint (`hslRanges`, `hslOffset`).
- Unity Terrain splat mode (real ground layers, control maps).
- `dcal` decals.
- Terrain holes from `tileFlagArray` / `chunkFlags` (only padding holes).
- Height sentinels.
- Picking zone instances in the Map Window.
- Game shaders for foliage under Shader shading.
- Foliage in the single-file `world-export --out`.
- `prp2` trees as Terrain trees (research §6.2 heuristics).
- Model LODs beyond LOD0 (the export's stage B).
- Billboards and impostors.
- Colliders for zone trees that have none (none exist in the data).
- Quest build verification beyond the preset's counts.
- Udon (sub-project 6).

## Open questions for the user (each has a default the plan follows)

1. **Unity Terrain in this sub-project.** Details and Terrain trees need a
   Unity Terrain, so phase 3 includes the baked-layer Terrain mode (research
   §8.4 step 1). Default: yes.
2. **Wind.** None in this sub-project (D10). Default: confirm. Sway would be a
   follow-up after one VS is transcribed.
3. **PC density.** Default: All (every placement), with the Density knob.
4. **Quest.** Default preset: collider trees only, Distance 0.5. Alternative:
   all trees.
5. **Phase 2 fallback.** If neither the static code nor a data fit gives a
   rule that reproduces the colliders, ship `densityMatched`, labelled UNPROVEN. Default:
   yes, rather than stop at phase 1.

## Amendments (after implementation)

### 2026-10-16, Task 11: Map Window budgets at full density (Queensdale)

**Inputs.** Queensdale 192711 holds 369 184 zone instances (4 381 Collider,
364 803 exact Scatter; 369 061 placed, the rest are of the one model without a
mesh), not the ~217 k / ~250 k estimated above: the tiling path at graphics
quality 2 makes about 1.5 instances per type-1/2 cell (Task 10).

**What changed.**

- **set_world.** The per-instance work it still did (each instance's
  row-vector matrix, its model and the counting sort by model) moved into
  `build_foliage_grid` on the world worker: `FoliageGrid` gains `world`,
  `mesh`, `models`, `modelStart` / `byModel` and `colliders`. `set_world` only
  takes the grid over (a grid that does not match the scene is still rebuilt
  there). Measured in release, the foliage part of `set_world` is now under
  0.05 ms; the rest is the terrain chunk upload (about 30 ms), the props
  (about 1 ms), the collision copy (about 1.5 ms) and the water height grid
  (about 2 ms).
- **Fade at graphics quality 2.** The client multiplies every layer's
  `fadeRange` by {0.85, 1, 1.5, 2}[quality] (research §1.3). The Map Window
  now applies the factor for the quality in force (2, as `zoneGraphicsQuality`):
  the cull's distance scale is `foliageDistanceScale` × 1.5
  (`castlemist::world::zone_fade_quality_scale`,
  `WorldView::foliage_fade_quality_scale`, `foliageFadeQuality` on the
  headless stats line). The manifest gains the setting `zoneFadeQualityScale`
  (1.5, UNPROVEN as `zoneFade` is). The export's `fadeRange` and `fadeFar`
  values stay the stored ones; applying the factor in the importers is left
  open.
- **Headless.** `GW2_MAPWIN_FOCUS=foliage` stands 150 units above the ground
  in the tile at the 90th percentile of placed zone instances, looking 8°
  down along the densest of eight headings (`foliage_focus_view`);
  `foliagemax` takes the densest tile and `forest` the densest tile of
  Collider instances. `GW2_MAPWIN_FOLIAGE_SCALE` sets `foliageDistanceScale`.

**Measured** (release build `build/release`, `GW2_MAPWIN_SHADING=materials`,
time 0, 30 bench frames, two runs each, NVIDIA GeForce RTX 4090; all-against-off
is the same run's `bench_nofoliage_*` re-bench):

| item | target | measured |
|---|---|---|
| `set_world`, UI thread | < 50 ms | 32.5-41.4 ms with Foliage All, 33.6-48.2 ms with it off (14 runs; base 8a7c177: 38.6-55.8 ms) |
| `add_ms_max` | < 50 ms | 7.9-34.1 ms |
| `foliageCullMs` at the p90 view (`foliage`) | ≤ 2 ms | 0.22-0.44 ms (densest: 0.29-0.34; forest: 0.22-0.38) |
| whole-frame CPU, All against off, bench median | ≤ +3 ms | p90 view +0.11 / +0.44 ms; densest +0.21 / +0.15; forest +0.11 / +0.71; props +0.10 / +0.13 |
| foliage GPU, bench median | ≤ 4 ms | p90 view +0.02 / +0.05 ms; densest +0.03; forest +0.02 / +0.07 |
| `foliageVisible` | ≤ 20 000 | p90 view 4 019 (90 tiles); densest 3 999; forest 3 036; `foliageCapped` 0 in every run |
| peak private memory (sampled every 250 ms) | (about 17 MB for instances and grid) | 1 975-2 026 MB for the whole process, unchanged from Task 10's ~2.0 GB; the grid's draw data is the matrices the view held before (23.6 MB) |

The whole-map views draw no foliage (everything is past its fade range); their
all-against-off figures are noise. **Budgets held; no lever taken** (tile size
1536, cap 20 000, per-frame stream).

**Look** (shots `foliage`, `forest` with fade ×1 and ×1.5): grass, flowers and
plants stand on the terrain, none floating or buried; tree trunks reach the
ground; with ×1.5 more grass shows on the far slopes and more trees on the
horizon.

### 2026-10-17, Task 15b: model LODs to Unity LODGroups

Added at the user's request. Research: `docs/research/gw2-model-lods.md`
amendment 2026-10-17 (the client's rule, from static disassembly).

- **Export.** World model glbs (`GltfExportOptions::exportLods`, world export
  only) carry each mesh's own LOD1/LOD2 index sets as extra meshes sharing the
  LOD0 vertex accessors and material, on nodes `<LOD0 node>_LOD<k>`; a level a
  mesh has no set for gets no node and repeats its coarser one
  (`settings.lodMissingLevel`, UNPROVEN). At most 3 levels. The VRChat,
  character and model exports are byte-identical (tested).
- **Manifest.** `models[].lods` {levels, distances, distanceSource,
  lodOverride, triangles}; `instances[].lod` for OVERRIDE_LOD placements;
  settings `lodDistances` (measured from the client), `lodDistanceScale` and
  `lodMissingLevel` (UNPROVEN), `lodPropCullHeight` 0 (castlemist's choice).
- **Unity.** A model with levels > 1 gets a LODGroup on its prefab; heights
  `h(d) = size / (2 d tan(lodCullFovDeg / 2))` with the LODGroup's own size and
  the model-unit distance (the instance scale cancels: the client's distance
  scales with it); props never cull; tree prefabs end at the fadeFar cull and
  drop levels it hides; OVERRIDE_LOD instances get their own heights. Grass
  patches and terrain details keep LOD0. PLAN gains `lodPrefabs`,
  `lodTreePrefabs`, `lodOverrides`; batchmode logs `LODS ... OK`.
- **Blender.** LOD meshes go to hidden, non-instanced `<fileId> LOD<k>`
  collections; `--verify` checks them.
- **Map Window.** Not changed: it keeps its own angular-size thresholds.
- **Measured** (Unity 2022.3.22f1 batchmode): Spirit Vale `lodPrefabs=72
  lodTreePrefabs=0 lodOverrides=32`, Queensdale region 8,4,15,11
  `lodPrefabs=38 lodTreePrefabs=3 lodOverrides=6`, PLAN equal to the dry run in
  mesh and Terrain mode. Coverage on the three maps: 8–19% of prop models,
  6–24% of their placements; LOD1 cuts about 30% and LOD2 about 38% of the
  triangles of the models that have LODs.

### 2026-10-17, Task 15d: shared materials across the world export

Added at the user's request ("I don't want a bunch of objects that use
separate materials that are the same"). World export only; the VRChat, model,
character and single-file map exports are byte-identical (tested in
`world_materials.other_exports_unchanged`, and on the dat: 4 models' `--glb`
and `--vrchat` folders, 50 files, and Spirit Vale's `world-export --out` glb).

- **Content key.** Each material's key is the SHA-1 (40 lower-case hex) of a
  canonical text: its glTF material less its name, every texture as its image
  URI (the dat pool's `textures/<fileId>.png` or the content pool's), and its
  materials.json entry less `name`, `fileName`, `usedByMeshes` and `warnings`
  (JSON keys sorted), plus the model's dark-decal flag and uvScroll entry for
  that material. The name is display only. It is written as materials.json
  `key`, glTF `extras.castlemistMaterialKey` and `models[].materialKeys`.
- **Generated-map pool.** Every Poiyomi map and generated glb image (emissive
  bake, metal-rough) is written once, as `textures/gen/<sha1 of the PNG
  bytes>_<Role>.png` (glb images without a role; extra layers
  `_x-<lower-case role>`, so no two names differ only by case, which Spirit
  Vale hit with an extra `projector` beside a `Projector` map), with W8's
  claim, wait and temp-rename. The PNG-bytes hash was chosen over a pixel hash
  because stb writes the same bytes for the same pixels and the validator can
  re-check it with hashlib. `models/<fileId>/Textures/` and `Images/` are no
  longer written.
- **Manifest.** Optional `sharedMaterials` {generatedMaps {dir, count, bytes,
  bytesRequested}, entries [{key, name, model, material, slots, models,
  usedByMeshes}]}: one entry per key in first-use order; name the most common
  slot name (the smallest on a tie); model/material the first used slot, which
  Unity builds from. The validator checks unique keys, that every
  materialKeys key is listed and equals materials.json's, the counts, that each
  pool file is named by its SHA-1 (case-sensitively), and that every map exists.
- **Unity.** One `Materials/Shared/<key>.mat` per key, every FBX material name
  remapped to it (LOD renderers included); a slot the manifest gives another
  dark decal or uvScroll gets `<key>_<8 hex>.mat`. GUIDs stay stable on
  re-import (the file name is the key). PLAN `materials=` is one per distinct
  (key, dark decal, uvScroll), equal to the dry run. Batchmode logs
  `MATERIALS ... OK`. The postprocessor reads a pooled map's role from its name.
- **Blender.** One material per key and variant, shared by every model and LOD
  level, named from the table; `--verify` prints `MATERIALS keys= materials=
  slots=`.
- **Measured** (release build, `--no-fbx`; Unity 2022.3.22f1 batchmode without
  Poiyomi, the before import with the before importer; each in a fresh project):

  | | Spirit Vale before | after | Queensdale 8,4,15,11 before | after |
  |---|---|---|---|---|
  | materials.json entries (used) | 1076 (997) | 1076 (997), 737 keys (688 used) | 906 (838) | 906 (838), 501 keys (474 used) |
  | PLAN `materials=` | 1063 | 754 | 846 | 482 |
  | generated maps, files / MB | 5278 / 1093 | 2098 / 320 | 4069 / 473 | 1239 / 119 |
  | folder without FBX (with FBX), MB | 1683 (1986) | 911 (1214) | 766 (1003) | 412 (650) |
  | export seconds (models stage) | 15.5 (7.9) | 15.5 (7.5) | 11.7 (4.4) | 12.4 (4.3) |
  | Unity importer, first import (wall) | 85 s (242 s) | 60 s (162 s) | 103 s (233 s) | 78 s (158 s) |
  | Unity re-import | 40 s | 34 s | 58 s | 56 s |

  Look unchanged: every prefab renderer's resolved material (shader, queue,
  keywords, every property, each texture by the SHA-1 of its file and its
  import settings) is identical before and after: 1294 of 1294 slots on Spirit
  Vale, 1199 of 1199 on Queensdale. Blender 4.5 (`--import`, Queensdale region): 838 → 474 model
  materials, import 478 → 428 s, and every model slot's resolved material (node types, unlinked inputs, links,
  drivers, images by file SHA-1 and colour space, blend settings) identical, 1382 of 1382 slots. No render was
  compared; the property checks stand in for it.

### 2026-10-17, Task 15c: Poiyomi Pro 10 and 11

The user has Poiyomi Pro 11.0.22 (and 10.x); the importer had looked only for
`.poiyomi/Poiyomi Toon` and assumed 10.0.24's names.

- **Detection.** `PoiyomiShaders.Names` lists `.poiyomi/Poiyomi Pro`,
  `.poiyomi/Poiyomi Toon`, then the `Old Versions/10.0/` and
  `Old Versions/9.3/` Pro and Toon names. Poiyomi Pro 11 registers 11 at the
  plain names and 10.0.23 under `Old Versions/10.0/`; Poiyomi 10 registers 10
  at the plain names. So the version comes from `shader_master_label`, and
  `PoiyomiShaders.Choose` takes the highest major, then Pro over Toon, and
  skips a shader with compile errors. Standard stays the fallback. Still no
  compile-time reference to Poiyomi or ThryEditor.
- **Properties.** Every property the importer sets has the same name in
  10.0.9, 10.0.23 and 11.0.22, so one map serves both. The importer warns
  about any the chosen shader lacks. The `_Mode` presets are read from the
  chosen shader's own description (`PoiyomiShaders.ParsePresets`), with the
  hard-coded table only as the fallback. That table now also sets
  `_OutlineBlendOp`, `_OutlineBlendOpAlpha` and Opaque's
  `_OutlineDstBlendAlpha = 0`, as Poiyomi does.
- **Locking.** Materials stay unlocked. ThryEditor's VRChat world-build hook
  locks every material in the scene.
- **Batchmode.** There is a `SHADER` line. `POIYOMI_MATERIALS` reads back each
  shared material and checks for shader errors. `-castlemistExpectShader`
  pins the expected shader, `-castlemistScreenshot` takes a Camera.Render
  screenshot, and `BatchEntry.LockProbe` runs ThryEditor's lock and unlock.
  PLAN is unchanged.

### 2026-10-18, Task 16: corrections, decisions and measurements from the build

Everything below comes from the task reports and the ledger
(`.superpowers/sdd/2026-10-16-world-foliage/`). The text above (M1, M2, D3,
D11 and the risk row U4) is corrected in place; this section lists what the
build decided and measured.

**Phase 1 and the decode (Tasks 2-4, 10).**

- Task 2 changed `gw2dat_cli map`: its `zones` block now uses the measured
  anchor column and model slot `(flag >> 1) & 7`, and prints `byType` (visits
  per layer type) and `layerUnresolved` (output changed 2026-10-16). Defs and
  layers are read by template name; `missingFields` is empty on all three maps.
  All three are `zon2` v22.
- Ruling S1: anchor candidates are type-3 placements only (D3 above). Cost if
  wrong: a collider tree on a type-1/2 model would go unlinked; none do.
- Ruling S4: the client decode replaces the first walk. Placements are
  QD 247 804, LA 40 252, SV 8 670 (M1); **4381 of 4381** Queensdale colliders
  link by anchor (Task 10), and 4750 of 4750 over the three maps.
- Task 3 (on the old walk): Queensdale 4378 anchor + 3 geometry links, Lion's
  Arch 88/88, Spirit Vale 284/284, 0 unlinked, 0 prop disagreements. Task 4:
  every collider has a tree; hull-in-model share median 1.000, minimum 0.936.

**Phase 2: the scatter rule (Tasks 9-10).** Decision: `exact` (ruling S3). The
rule was found by static disassembly of Gw2-64.exe (the game was never run)
and reproduces all 4750 colliders within D5's tolerances (worst error: xy
0.002, yaw 5e-9, tilt 2.4e-7, scale 4.8e-9, z 0.0092). It is Park-Miller 48271,
re-seeded per cell; the full rule and the addresses are in
`docs/research/gw2-terrain-to-unity.md` §1.3 "zone scatter rule". Types 1-2
(tiling, graphics quality 2, groundcover offset +(24, 24)) are transcribed
only: no collider checks them, so they are UNPROVEN (settings `zoneTiling`,
`zoneGraphicsQuality`, `zoneGroundcoverOffset`). Also transcribed but not
exercised by any collider: the clamp rect, the zone matrix (identity), the
water test, random z, yaw from normal, the 90 degree snap and the sub-model
second instance (`zoneUnprovenBranches`).

- **Query flags.** The client's query flags (the `noGroundcover` /
  `onlyGroundcover` assert, and `param_9 & 1` in 0x140cc1f50) are taken as 0, an
  unfiltered query: every instance is emitted. This is transcribed, not
  measured. Task 16 added it as the setting `zoneQueryFlags` (UNPROVEN; a row
  in `world_manifest.cpp` and a key in the schema).
- Resulting instance counts: Queensdale 369 184 (4 381 Collider, 364 803
  Scatter; 369 061 placed), Lion's Arch about 73 k, Spirit Vale about 21 k.
- **Model 57017 has no mesh.** One Queensdale scatter model (fileId 57017, 123
  instances) has no drawable mesh. The Map Window counts it as failed, and the
  export skips those 123 placements (123 skipped on Queensdale, 0 on the other
  two maps).
- Ruling S2: models used by zone instances count as used in the world folder;
  Task 7 narrowed this to the kept cells.

**Map Window (Tasks 5, 6, 11).** The Foliage split button toggles on click and
opens a Colliders / All menu on its arrow; it is on, with Colliders, after the
window opens. Task 6 captures (debug): Queensdale `zoneTrees` 4381,
`zoneUnlinked` 0; Spirit Vale 284 and 0. With foliage off the bare pink trunk
colliders show; with it on each sits inside a tree, and no collider stands
alone. The zonetree shot's framing deviates from D6's wording in direction
only (it looks 15 degrees below the horizon from the first clear of eight
headings, because the steep whole-map view put the camera behind the map-edge
wall). The user's Queensdale walk (Task 6's handoff) has **no result
recorded**; treat it as not run. Release `set_world` was 34 ms on Queensdale at
Task 6 (colliders only); Task 11's budgets are in its own amendment above.

**Unity Terrain export and importer (Tasks 12-13).**

- Task 12 put `TerrainHeightfield`, `terrain_heightfield` and `terrain_up_range`
  in `world/terrain.h` beside `terrain_height_at`, and the writer in
  `exportgltf/world_unity_terrain.h`. The foliage grid's public header is
  `include/castlemist/render/world_foliage_grid.h` (pure, D3D-free). Spirit
  Vale has 0 Terrain trees: all 284 of its type-3 records lean 27-99 degrees,
  and none is within the 5 degree limit. Queensdale: 992 of 1 090 type-3
  records pass (255 of the 318 collider trees).
- Ruling S5: the export carries `zoneFadeQualityScale` (1.5) and
  `unityTerrain.fadeQualityScale`; the importers' Distance knob multiplies the
  stored `fadeRange` by it by default.
- Ruling S6: the 32 `unityTerrain.check` points sit on heightmap sample
  vertices (exact for any triangle split), compared at 0.5 units.
- Task 13 fixed an importer bug found on a region export: prefabs and
  materials were keyed by `models[].index` (the scene index) and not by array
  position, so 1010 of the Queensdale region's 1385 props had no prefab. Patch
  assets live in `FoliagePatches/<cellId>/`, not `Foliage/<cellId>/` (on Windows
  the second is the same folder as the export's `foliage/`). The fixture's mesh
  plan has `foliagePatches=2`, because its two grass records are 3000 units
  apart.
- Batchmode (Unity 2022.3.22f1; Tasks 8 and 13): PLAN equal to the dry run on
  the fixture, Spirit Vale and the Queensdale region 8,4,15,11, in Mesh and
  Terrain mode, at density 0.5, and with the Quest preset. `TERRAIN_CHECK`
  `maxErr` is 0.00371 (Queensdale) and 0.01025 (Spirit Vale) against a
  tolerance of 0.0127. Blender (Tasks 8 and 14): `--verify` OK on every run;
  the kept instances equal the validator's `kept` in 7 runs (Spirit Vale at
  density 1: 21 344, matrix error 2.9e-5; Queensdale at density 1: 36 001 for
  the region).

**Late additions the user asked for.** Task 15b (model LODs), Task 15d (shared
materials) and Task 15c (Poiyomi Pro 10 and 11) have their own amendments
above. The user-facing consequences: LOD levels share their LOD0 material; the
world export writes one material per content key; the importer chooses Poiyomi
Pro 11, Toon 11, Pro 10 or Toon 10 by the version in the shader's label.

**Side fixes made during the work.**

- **Labyrinth terrain pages** (Mad King's Labyrinth 2499169, untextured): 6x6
  chunks on 2x2 pages fit n = 3, 4 and 5, so no blend pages were found. A tie
  now takes the measured 4 (UNPROVEN warning), with a dat test.
- **32-bit packfile pointers**: the Labyrinth's "no light rig" was caused by
  pointers that were not sign-extended; they now are.
- **Approximate sky**: haze and per-zone parsing, and an approximate bake that
  adds the clouds, haze and material cards the proven bake leaves out. It is
  the Map Window default (the **Approx sky** toggle) and opt-in in the export
  (`--sky-approx`; `gw2dat_cli skybox --approx`). Its inputs are labelled
  UNPROVEN. Spirit Vale looks very different under it, and **its look has not
  been checked against the game**.

**Measurements (Task 15; debug CLI, default options: foliage all, unityTerrain
on, sky on, `--no-fbx`; release was not rebuilt).**
`world_export_dat.spirit_vale_foliage` passes: 284 collider trees, 21 344
records equal to the instances, all 284 collider records equal a collider
instance to 1e-4, the 32 check points exact, validator OK.

| | Spirit Vale | Lion's Arch | Queensdale |
|---|---|---|---|
| wall seconds | 78.6 | 155.9 | 158.5 |
| peak working set / virtual (MB) | 1 094 / 5 568 | 1 928 / 6 405 | 1 316 / 5 803 |
| cells / models ok / failed | 66 / 383 / 12 | 96 / 1 706 / 48 | 44 / 2 244 / 36 |
| foliage records | 21 344 | 73 467 | 369 061 |
| collider / Terrain trees | 284 / 0 | 88 / 401 | 4 381 / 8 590 |
| detail records | 21 060 | 72 451 | 357 718 |
| foliage-only models | 3 | 55 | 98 |
| foliage bins (files, bytes) | 40, 2.13 MB | 86, 7.35 MB | 44, 36.9 MB |
| unityTerrain tiles, terrain dir | 21, 16.8 MB | 15, 27.2 MB | 35, 78.4 MB |
| stage s: models / cells / unity-terrain / sky / manifest | 32.2 / 2.6 / 4.5 / 23.0 / 2.0 | 91.2 / 4.3 / 6.3 / 23.9 / 5.4 | 87.8 / 6.2 / 17.2 / 23.5 / 2.7 |
| models with LOD levels > 1 | 72 of 383 | 225 of 1 706 | 177 of 2 244 |
| shared material keys / slots | 737 / 1 076 | 2 130 / 3 659 | 1 912 / 4 276 |
| generated maps written / requested (MB) | 320 / 1 093 | 751 / 3 079 | 486 / 2 815 |
| height range (units) | -4999.9995 .. 14 465.69 | -10 000 .. 6 165.79 | -1 701.46 .. 5 768.94 |

Foliage records are built inside the cell stage (there is no separate timer).
Every model failure is "This model has no drawable meshes to export". The
validator's schema check and `--plan --mode terrain --foliage all` pass on all
three, with `FOLIAGE records` equal to kept. Spirit Vale's and Lion's Arch's
height ranges contain sentinel values (-4999.9995 and -10 000), which stretch
the `.r16` normalisation; this was not fixed, and the margin of Spirit Vale's
terrain check is thin (0.40 of 0.5 units at Task 13).

**Settings.** Every UNPROVEN setting was checked against `settings`,
`warnings` and its code comment. New in the build: `zoneTiling`,
`zoneGraphicsQuality`, `zoneGroundcoverOffset`, `zoneUnprovenBranches` and
`zoneFadeQualityScale` (all UNPROVEN), `zoneQueryFlags` (UNPROVEN, added in
Task 16), `terrainTreeMaxTiltDeg` (castlemist's choice), `terrainTreeYawSign`,
`detailPlacement`, `terrainHeightRowOrder` and `terrainColliderDiagonal`
(UNPROVEN where the spec says), and Task 15b's `lodDistances`,
`lodDistanceScale`, `lodMissingLevel` and `lodPropCullHeight`.

**Checklist results** (dated rows in `tools/unity/CHECKLIST.md`): Unity items
1-3, 9 (batchmode part) and 12-13 pass in batchmode on 2026-10-15 to 17. The
Blender checks are the `--verify` runs above.

**Not tried.**

- Unity checklist items 14-18 (walk into trees, Terrain tree yaw against a
  GameObject, detail density against the Map Window, Frame Debugger draws and
  import time, no sway) and items 4-8 and 10-11: they need the editor in a
  VRChat 2022.3 project. No VRChat SDK was in any test project, so the
  world-build lock hook (ThryEditor `LockMaterialsOnWorldUpload`) did not run
  and locking all 754 Spirit Vale materials was not timed (6 were locked, in 3
  to 4 s).
- The Blender visual checklist: viewing the Spirit Vale scene and one Cycles
  frame at the spawn. Only `--import --verify` was run.
- The user's Queensdale Map Window walk (Task 6): no result recorded.
- The Spirit Vale approximate-sky look against the game.
- Task 15's release-build timings (debug only), and a full-Queensdale Unity
  import (only the 4-cell region was imported).
- Types 1-2 tiling and the graphics-quality input, and the transcribed
  branches in `zoneUnprovenBranches`: no collider checks them.
- Poiyomi's "World" shader variants were not researched.

**Open items carried to the whole-branch review.** The scatter z uses the
NE-SW terrain triangle while the export and the Map Window walk use NW-SE
(`CollisionIndex`); `parseMapEnv` reads light colours with R and B swapped
against the game (it affects all world lighting and was left alone for its own
fix); one `forest` shot may show a gap at a trunk base; the
`json::parse(std::ifstream&)` crash seen in `cm_test_dat` at 022e2b9 after
`nlohmann-json` was re-fetched as v3.12.0 (the test now reads the file into a
string; stream-parse sites remain at `tests/test_exportgltf_world.cpp` lines
785 and 1384); baked textures named by flagged fileIds could collide in the dat
pool (Task 15d concern 2); and models with "no drawable meshes" count as failed
rather than empty.
