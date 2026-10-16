# World Foliage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Draw, export and import a GW2 map's zone-scatter layer (`zon2`: grass, plants, shrubs and trees) from the `WorldScene`:
- **phase 1** places every zone tree that has a `havk` collider, exactly, and draws it in the Map Window;
- **phase 2** places the remaining zone instances, exactly where the rule is proven and otherwise density-matched and labelled UNPROVEN;
- **phase 3** carries the foliage through the world export into Unity (Unity Terrain details and trees, or instances on the mesh terrain) and Blender (geometry nodes), with density and distance controls for Quest.

**Architecture:**
- `extract` gains `Extractor::parseZon2()`, a raw reader by template name. `parseMapZones` is reimplemented over it with the measured anchor and slot fixes.
- `world` gains `zones.h/.cpp`. `build_zones` decodes the placements, links each `havk.zoneModels` entry to its model (anchor match, then geometry vote, then a prop cross-check) and builds `ZoneInstance`s. Phase 2 adds `zone_scatter.h/.cpp`.
- `render` gains a D3D-free `FoliageGrid` that the map window's world worker builds, plus a `FoliageBatch` in `WorldView` with a screen-door fade.
- `exportgltf` writes `foliage/<cell>.bin` records (`castlemist-foliage-1`), the manifest's `foliage` block and, in phase 3, the `unityTerrain` block and its files.
- The Python validator, the Blender script and the Unity importer read the records.

**Tech Stack:**
- C++20, MinGW-w64 GCC, CMake + Ninja presets, Win32 / Direct3D 11.
- The repo's test framework (`tests/framework/test_framework.h`: `CM_TEST`, `CHECK*`, `SKIP`).
- nlohmann::json and stb (vendored).
- Python 3.12 (standard library only).
- Blender 4.5 (headless).
- C# 9 for Unity 2022.3.22f1.

**Spec:** docs/superpowers/specs/2026-10-16-world-foliage-design.md. It is the binding authority. Read it with this plan: section names here (M1-M6, D1-D11, Testing, Performance) are the spec's.

**Execution model:**
- Subagent-driven.
- There are **no per-task reviews**, only **one whole-branch review at the end** (Task 16). This is the user's rule of 2026-10-15.
- The tags only say how much weight a task carries:
  - `[review]` marks a task whose interfaces or numbers later tasks depend on. The final review reads these first.
  - `[light]` marks a mechanical task.
  - Neither tag triggers a review of its own.
- Order: phase 1 (Tasks 1-8), then phase 2 (Tasks 9-11), then phase 3 (Tasks 12-16).
- Phase 1 reaches something the user can see early. The user's bug was found by walking Queensdale in the Map Window, where colliders stood with no trees. By the end of Task 6, zone collider trees draw in the Map Window. The controller then hands the build to the user for the Queensdale walk and carries on with Task 7 while they walk.

**Answers to the spec's open questions** (the user, 2026-10-16, all defaults):
1. Unity Terrain baked-layer mode is in phase 3.
2. No wind sway. The export marks wind-shader models (`foliage.models[].wind`).
3. PC density is All, every placement, by default.
4. The Quest preset is collider trees only, at Distance 0.5.
5. If the scatter rule cannot be reproduced, ship the labelled `densityMatched` placement.
6. Foliage is sub-project 5 and Udon moves to 6.

## Global Constraints

**Build and test**
- Worktree root: `C:/Users/vital/Downloads/castlemist/castlemist/.claude/worktrees/world-foliage`. Paths are relative to it.
- **Build environment.** Put `C:/msys64/ucrt64/bin` first on PATH, and set `TMP` and `TEMP` to a scratchpad directory, or g++ fails silently:
  `export PATH="/c/msys64/ucrt64/bin:$PATH"; export TMP=<scratchpad>; export TEMP=<scratchpad>`.
- **Configure.** `external/` in the worktree holds only the tracked glue, so configure with
  `cmake --preset debug -DCASTLEMIST_EXTERNAL_DIR=C:/Users/vital/Downloads/castlemist/castlemist/external`. Build with `cmake --build --preset debug`.
- **Tests.** Run a suite as `build/debug/bin/cm_test_<suite>.exe <filter>`, where the filter is a `group.name` prefix. Run everything with `ctest --preset debug`.
- **Baseline at base (cd71032).**
  - `ctest` passes 33/33.
  - `cm_test_dat` has **11 pre-existing failures**, from curated ids. They are not this branch's to fix, and a task passes when no new failure appears.
- **Measurements and budgets** are taken on `relwithdebinfo`, with the build type recorded.

**The dat and the client**
- **Gw2.dat is opened only by castlemist code** (tools and tests). Never read, grep or hex-dump it directly.
  - The dat is `E:/Games/gw2/Guild Wars 2/Gw2.dat`: `GW2_TEST_DAT` for tests, `GW2_DAT` for the app.
  - The template is `C:/Users/vital/Downloads/castlemist/castlemist/dumps/packfile/gw2_packfile.json` (`GW2_TEST_TEMPLATE`).
  - Dat tests `SKIP` without the dat. A running Gw2-64.exe locks the dat.
- **Static client only.** Static disassembly of `Gw2-64.exe` is allowed: the repo's IDB (`gw2decomp/Gw2-64.exe.i64`, `docs/research/gw2-ida-naming-coverage.md`), the Ghidra project (`~/gw2re.gpr`, `docs/research/emu-ghidra-migration.md`), or `objdump` on `Gw2-64-disable-aslr.exe` as `gw2-world-frame.md` §0 did. **Never launch, attach to, hook or inject into the game.** `tools/hook/` and `tools/host/` are off limits for this sub-project.

**Frame and layers**
- Map space only: left-handed, +X east, +Y north, up = −Z. Files keep map units. The only conversions are `world/frame.h` and `exportgltf::map_to_gltf_root`.
- Layer rules:
  - `world` depends on `extract` (and `native`) only;
  - `exportgltf` depends on `extract` and `world`, never on `render`;
  - `render` may use `exportgltf`;
  - nothing below `render` mentions Direct3D, and `FoliageGrid` is pure.

**No invented conventions**
- Every value the data does not settle is a labelled setting. It is written to the manifest's `settings` (value, status, source) and, when UNPROVEN, to `warnings` and to its code comment. Use the spec's D11 table verbatim.
- castlemist's choices and their values:

| Setting | Value |
|---|---|
| zone grid | 48 units, origin parm (x0, y0), rows north, anchor `A(c, r) = (x0 + 48·(c − 1), y0 + 48·r)` |
| model slot | `layerDefArray[(f >> 4) − 1].modelArray[(f >> 1) & 7]` |
| anchor tolerance | ±24 units per axis (Chebyshev); ties to the nearest Euclidean, then the lower placement index |
| `foliageTileUnits` / `foliagePatchUnits` | 1536 / 1536 |
| `foliageMaxVisible` | 20 000 |
| `terrainTreeMaxTiltDeg` | 5 |
| `lodCullFovDeg` | 60 |
| record layout | `castlemist-foliage-1`, 100 bytes (D7) |
| Terrain tile | 4 × 4 chunks, heightmap 129, detail 256 at 48 units, holes 128 |
| thinning hash | `fmix32(placement) < density · 2³²` |
| Quest preset | Terrain Mesh, Foliage Collider trees, Distance 0.5 |

**Licence**
- Not a concern (the user's ruling). Building on `parseMapZones`, which is Tyria3D-derived, is fine.
- The spec measured that its first-model-only rule (`modelArray[0]`) and its cell column (`x0 + 48·c`) are **wrong** (M2, M3). Rewrite its doc comment (`gw2model.hpp:970-989`) to say what is now proven.

**Style and commits**
- Doc comments follow the repo style (`/// @file`, `/// @brief`) and cite the spec and research sections.
- Every commit message ends with a blank line, then `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Use `git commit -m "<subject>" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"`.
- Don't rename existing labels or fields unless you are certain. Ask first (user memory).

## Review Focus

- **A map with `zon2` but no usable `havk` (or none at all).** Expected: no zone trees, no throw, and no warning when `zon2` is absent. When `zon2` is present but `havk` is absent, the placements still decode and no instance is made.
  → Task 3: `zones.absent_zon2_silent`, `zones.no_havk_decodes_no_instances`.
- **A zone model that fails to load, or never arrives.** Expected: its tile stays undrawn, other tiles in range still draw, and the failure is counted (`modelsFailed`). It must not be drawn at the origin or with a zero sphere.
  → Task 5: `foliage_grid.tile_without_spheres_skipped`, `foliage_grid.unloaded_instance_not_visible`.
- **A collider entry whose transform is degenerate** (scale 0, NaN rotate, mirrored). Expected: the export skips the record and counts it in `counts.foliage.skipped` with one warning. It is never written with a broken quaternion. The hull stays in the cell.
  → Task 7: `world_foliage.bad_transform_skipped`.
- **Region exports, and placements off the terrain.** Expected: records go only to kept cells. A placement outside every chunk is bucketed by clamping, as props are, and gets flag bit 4.
  → Task 7: `world_foliage.region_filters_records`, `world_foliage.outside_terrain_clamped_bit4`.
- **A Unity detail cell holding more than 255 records.** Expected: the count clamps to 255, with one warning naming the tile, and the rest of the tile is unaffected.
  → Task 12: `unity_terrain.detail_count_clamps_255`.

---

## Phase 1: collider trees, exact, drawn

### Task 1: Stop the stray `dumps/index/gw2_index.db` `[light]`

**Files:**
- Modify: whatever writes the file. The suspects are below; confirm by reproducing.
  - `src/db/index_db.cpp:59` `db::open`: `sqlite3_open16` creates a missing file.
  - `src/ui/application.cpp:329`: the headless hook opens the relative `L"dumps/index/gw2_index.db"` without checking that it exists.
  - `tests/test_db_index.cpp:248-255`: the `open_real_index` candidates are relative paths.
- Test: `tests/test_db_index.cpp`

**Interfaces:** `bool db::open(const std::wstring& db_path, std::string& err)` keeps its signature. A path that does not exist returns false with `err = "no such file: <path>"` and creates nothing. Every successful open is read-only.

- [ ] **Step 1: Reproduce.**
  1. Delete `dumps/index/gw2_index.db` if it is there.
  2. Run `build/debug/bin/cm_test_db.exe`, then one headless `GW2_MAPWIN=1151420` run from the worktree root (docs/testing.md, "The Map window").
  3. After each, check whether the file appears. Note which one creates it.
  4. Then run `ctest --preset debug -R "^db$"` → FAIL.
- [ ] **Step 2: Write the failing test.** `index_db.opening_a_missing_file_creates_nothing`: `db::open(L"dumps/index/__cm_missing_12345.db", err)` and `db::open(L"no_such_index_file_12345.db", err)` are false, and `std::filesystem::exists` is false for both paths afterwards. Create `dumps/index/` in the test's temp cwd first, so SQLite could create the file.
- [ ] **Step 3: Run** `cm_test_db.exe index_db.` → FAIL (the file is created).
- [ ] **Step 4: Implement.**
  - `db::open` returns false before SQLite runs when `GetFileAttributesW` says the path is missing.
  - It opens with `sqlite3_open_v2(utf8Path, &g_db, SQLITE_OPEN_READONLY, nullptr)`, converting the path with `WideCharToMultiByte(CP_UTF8, …)`.
  - The headless hook at `application.cpp:329` uses the exe-relative candidates of `try_autoload_index` (`src/ui/index_ui.cpp:172`), factored into `std::vector<std::wstring> index_db_candidates()` in the same file, instead of the cwd-relative path.
- [ ] **Step 5: Run** `cm_test_db.exe` → PASS. Repeat Step 1 → no file appears. `ctest --preset debug` → 33/33.
- [ ] **Step 6: Commit** `fix(db): open the index read-only and never create a missing file`.

### Task 2: `Extractor::parseZon2`, and `parseMapZones` over it `[review]`

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp`
  - `parseMapZones` and its comment (:959-1091);
  - `HavokPlacement` (:1160) gains `token`, and `parseHavok` reads it (:1237-1259);
  - `MapProp` (:1657) gains `guid`, and `parseMapProps` reads it.
- Modify: `tools/gw2dat_cli/main.cpp`. The `map` command's zone block (:1433-1440) prints `byType` and `layerUnresolved`. Its help text notes "zones: anchor column c − 1 and model slot (flag >> 1) & 7 (world foliage spec M2-M3); output changed 2026-10-16".
- Test: `tests/test_world.cpp` (the pure walk) and `tests/test_world_dat.cpp` (the reader on three maps)

**Interfaces** (members of `castlemist::model::Extractor`):
```cpp
struct Zon2LayerModel { uint32_t fileId = 0; float probability = 0; uint32_t flags = 0;
                        float hslOffset[3]{}; float zOffsets[2]{}; uint64_t permutation = 0; };
struct Zon2Layer {      // one layerDefArray entry; fields named as in the template (spec D2)
    uint8_t type = 0, width = 0, height = 0, radiusGround = 0, sortGroup = 0, tiling = 0, noise = 0;
    float scaleRange[2]{}, probability = 0, fadeRange[2]{}, rotRange[3][2]{}, hslRanges[4][2]{};
    float instanceScaleJitter = 0; uint32_t layerFlags = 0; uint32_t materialFileId = 0;
    std::vector<Zon2LayerModel> models;  };
struct Zon2Def { uint32_t token = 0; std::vector<Zon2Layer> layers; };
struct Zon2EncodeRow { uint16_t index = 0; uint8_t offset = 0; };
struct Zon2Zone {
    uint32_t defToken = 0; int32_t vertRect[4]{}; uint8_t seed = 0; float range[2]{}, zPos = 0, waterHeight = 0;
    uint32_t zoneFlags = 0; uint16_t broadId = 0;
    std::vector<uint8_t> flags;                 // the run-length (skip, flag) bytes, as stored
    std::vector<Zon2EncodeRow> encodeData; std::vector<float> collideData; std::vector<uint16_t> offsetData;  };
struct MapZon2 {
    bool present = false; uint16_t version = 0;
    float rect[4]{}; bool hasRect = false;      // parm rect, as parseMapZones reads it
    std::vector<Zon2Def> defs; std::vector<Zon2Zone> zones;
    std::vector<std::string> missingFields;     // "<struct>.<field>" the template version lacks (kept at defaults)
};
MapZon2 parseZon2();
/// The run-length walk (spec Measurements, notation): calls fn(k, c, r, f) for every non-zero flag byte,
/// with k the position after adding the skip byte, (c, r) = (vr[0] + k mod w, vr[1] + k / w). Nothing when w <= 0.
template <class F> static void walkZoneFlags(const Zon2Zone& z, F&& fn);
// HavokPlacement gains: uint64_t token = 0;  (propModels `token`, as stored; 0 elsewhere)
// MapProp gains:        uint64_t guid = 0;   (prp2 `guid`, as stored)
```
`parseMapZones()` keeps its signature and `MapZoneInst`. It is now `parseZon2` plus `walkZoneFlags`, with:
- `pos[0] = x0 + 48·(c − 1)` and `pos[1] = y0 + 48·r`;
- `fileId = layers[(f >> 4) − 1].models[(f >> 1) & 7].fileId`, skipping a placement whose layer or slot does not resolve.

`kMaxPlacements` is removed from `parseMapZones` (Queensdale has 217 114). Arrays that would run past the file read as empty, as `parseHavok`'s `fieldArray` does.

- [ ] **Step 1: Write failing tests**
  - `zones.walk_skip_and_flag_pairs` (pure, `cm_test_world`): `vertRect = {10, 20, 14, 23}` (w = 4) and `flags = {0,0x31, 3,0, 2,0x23, 7,0x10}`. The walk visits (k, c, r, f) = (0, 10, 20, 0x31), (5, 11, 21, 0x23) and (12, 10, 23, 0x10): the zero flag is skipped, and the row wraps at k = 4 and k = 8. A trailing odd byte is ignored. `vertRect` with w = 0 visits nothing.
  - `world_dat.zon2_reads_three_maps` (dat):
    - for 192711, 191000 and 1151420, `present`, `missingFields` empty, and Σ walk visits = 217 114 / 32 448 / 7 642 (M1);
    - for every visit, `(f >> 4) − 1` < the def's layer count and `(f >> 1) & 7` < that layer's model count;
    - `parseMapZones().size()` equals the visit count, and its first entry's `pos[0]` equals `rect[0] + 48·(c − 1)` of the first visit.
  - `world_dat.havk_prop_tokens_read`: on 192711, at least one `propModels` placement has a non-zero `token` that equals some `MapProp::guid`.
- [ ] **Step 2: Run** `cm_test_world.exe zones.walk` and `GW2_TEST_DAT=… GW2_TEST_TEMPLATE=… cm_test_world_dat.exe world_dat.zon2` → FAIL (does not compile).
- [ ] **Step 3: Implement.**
  - Read every field by its template name through the field's own `element` struct, as `parseHavok` does. Check the exact names against `PackMapZoneV*`, `PackMapZoneDefV*`, `PackMapZoneLayerDefV*` and `PackMapZoneModelV*` in the template.
  - A field the version lacks is pushed to `missingFields` and keeps its default.
  - `fileId` comes from `decodeFilename` on the model's `filename`. `materialFileId` comes from the layer's material filename field.
- [ ] **Step 4: Run** both → PASS.
  - Then `build/debug/bin/gw2dat_cli.exe map --dat "$GW2_TEST_DAT" --template <tpl> --file-id 192711`: `zones.placements == 217114`, and `uniqueModels` is no longer 54 (M3 gives 154 distinct models over the collided and uncollided placements together).
  - `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(extract): parseZon2 by template name; parseMapZones with the measured anchor column and model slot`.

### Task 3: `world/zones`: decode, link and instances; `load_world` wiring `[review]`

**Files:**
- Create: `include/castlemist/world/zones.h`, `src/world/zones.cpp` (added to the world sources in `CMakeLists.txt`)
- Modify:
  - `include/castlemist/world/world_scene.h`: `WorldScene` gains `ZoneScatter zones;` after `motion`. Include `zones.h`.
  - `src/world/load_world.cpp`: keep `parseHavok` and `parseMapProps` results for a new `zones` section after `collision`; add `"zon2"` to `kRead` (:197); `world_summary` gains `zones`.
  - `src/world/stream.cpp` `nearest_keys` (:13): zone instances count too.
  - `include/castlemist/world/load_world.h`: doc comments.
- Test: `tests/test_world_zones.cpp` (new; added to the `world` SOURCES at `tests/CMakeLists.txt:102`)

**Interfaces:**
```cpp
namespace castlemist::world {
// Spec D2's ZoneLayerModel, ZoneLayer, Zone, ZonePlacement, ZoneSource, ZoneInstance and ZoneScatter, verbatim.
// ZoneScatter also gains these (the manifest's foliage.colliders block and the scatter setting need them):
enum class ZoneScatterMode : uint8_t { Colliders, Exact, DensityMatched };
//   uint32_t colliderEntries = 0, colliderGeometries = 0, propCrossChecks = 0, propDisagreements = 0,
//            placementsUnresolved = 0;
//   ZoneScatterMode mode = ZoneScatterMode::Colliders;
// (D2's zonePlacementsLinked is the "linked by anchor" count: 4378 / 88 / 284.)
/// The anchor of grid point (c, r) for parm origin (x0, y0): (x0 + 48(c-1), y0 + 48 r).
void zone_anchor(float x0, float y0, int32_t c, int32_t r, float out[2]);
/// Spec D3 steps 1-5. Anchors use scene.bounds[0..1] (the parm rect); !hasBounds: warn "zone: no parm rect;
/// zones not placed" and stop. Appends zone-only models to scene.models (after the prop models, in the
/// order instances are made). Fills scene.zones; replaces anything in it. Never throws for bad data.
void build_zones(const castlemist::model::Extractor::MapZon2& z, const castlemist::model::Extractor::MapHavok& h,
                 const std::vector<castlemist::model::Extractor::MapProp>& props, WorldScene& scene);
/// MurmurHash3's 32-bit finaliser (spec D9's thinning hash).
uint32_t fmix32(uint32_t h);
/// One splitmix64 step: advances `state` and returns the output.
uint64_t splitmix64_next(uint64_t& state);
}
```
**Rules that the signature does not fix:**
- **Placement order.** Placements are in `zoneArray` order, then walk order. `Zone::firstPlacement` and `placementCount` index into them. A layer resolves by `defToken`, then `(f >> 4) − 1`. `ZoneScatter::layers` holds every layer of every def, in def order, with `defToken` and `indexInDef`.
- **The zoneModels index.** It is the position of the entry among the `group == "zone"` placements of `MapHavok::placements`. This is the same number as `CollisionInstance::placement` for group `"zone"`.
- **The hash lookup.** Use spec D3 step 2 exactly: the key `(c, r)`, and the 3 × 3 neighbourhood of `(round((x − x0)/48) + 1, round((y − y0)/48))`.
- **Instance matrices.** `ZoneInstance::world = client_world_matrix(translate, rotate, scale)` (`props.h`), as D4 says.
- **Warnings.** One line per non-zero count, each starting `zone:`:
  - unresolved placements;
  - geometry conflicts, naming the geometry and the counts;
  - unlinked colliders, with the spec's text;
  - prop disagreements;
  - an unloadable slot.
- **The UNPROVEN `zoneVisualTransform` warning.** It is written whenever at least one instance exists.
- **`world_summary` gains** `"zones": {"present", "placementsByType": [t1, t2, t3], "colliders": {"entries", "anchor", "geometry", "unlinked"}, "propCrossChecks", "propDisagreements", "scatter": "colliders"}`. `gw2dat_cli world` prints it unchanged.

- [ ] **Step 1: Write failing tests** (`cm_test_world`). They use a synthetic `MapZon2` with parm origin (−1000, −2000), one def of two layers (type 3 with models {A=501, B=502}, type 1 with {C=503}), and one zone with `vertRect {0, 0, 10, 10}`. Add a `MapHavok` with geometries and `zoneModels` entries, and a `scene` with `hasBounds` and one prop of fileId 501.
  - `zones.decode_layer_and_slot`: the flag `0x13` (layer 1, slot 1) resolves to layer 0 and model B. The flag `0x31` (layer 3) is unresolved and counted in `placementsUnresolved`, with one `zone:` warning.
  - `zones.anchor_formula`: `zone_anchor(-1000, -2000, 5, 7, a)` gives a = (−808, −1664).
  - `zones.anchor_link_tolerance`: an entry at anchor + (23, −23) links (`placement.collider` set, model B). One at anchor + (25, 0) does not, by anchor.
  - `zones.tie_nearest_then_claimed`: two placements in neighbouring cells, and entries at offsets that tie. The nearer placement wins. A second entry does not reuse a claimed placement.
  - `zones.geometry_vote_fills_unmatched`: geometry 4 has two anchor-matched entries (model B) and one far entry. The far entry gets B with `layer = placement = UINT32_MAX`, and `collidersLinkedByGeometry == 1`.
  - `zones.geometry_conflict_warns`: matched entries of one geometry give models {A, A, B}. The majority A is used, and one warning contains the geometry index and "2" and "1".
  - `zones.prop_cross_check_disagreement`: a `propModels` entry of geometry 4 whose `token` equals the prop's `guid` (fileId 501), while geometry 4 voted 502. `propCrossChecks == 1`, `propDisagreements == 1`, and one warning.
  - `zones.unlinked_warned`: an entry with no anchor and a geometry no one else uses. `collidersUnlinked == 1`, the warning text is spec D3 step 3's, and no instance is made.
  - `zones.collision_matrix_relation`: run `build_collision(h, scene)` and `build_zones(…)`. For the same zoneModels index, every element of `CollisionInstance::world` is within 1e-6·max(1, |x|) of `ZoneInstance::world · diag(32, 32, −32)`. Use non-trivial rotate (0.1, −0.2, 2.5) and scale 0.84.
  - `zones.models_appended_after_props`: `scene.models[0].fileId == 501` stays at index 0, so the prop's `PropInstance::model` is unchanged. Model B is appended at index 1. `layers[0].models[0].model == 0` (shared), `layers[0].models[1].model == 1`, and the unused C keeps `UINT32_MAX`.
  - `zones.load_order_ranks_zone_instances`: a zone-only model whose instance is nearest the camera comes first in `model_load_order`.
  - `zones.absent_zon2_silent`: `MapZon2{}` (not present) gives `zones.present == false` and adds no warning.
  - `zones.no_havk_decodes_no_instances`: `zon2` present and `MapHavok{}`. The placements decode, `instances` is empty, nothing throws, and no `zone: … unlinked` warning appears.
  - `zones.hashes_known_vectors`:
    - `fmix32(0) == 0` and `fmix32(1) == 0x514E28B7`;
    - from `state = 0`, `splitmix64_next` returns `0xE220A8397B1DCDAF`, then `0x6E789E6AA1B965F4`.
  - `world.unread_chunks_include_zon2`: `unread_chunk_warnings({{"zon2", 22}})` is empty.
- [ ] **Step 2: Run** `cm_test_world.exe zones.` → FAIL.
- [ ] **Step 3: Implement** `zones.cpp` and the wiring. `load_world`'s new section is named `"zones"`. It calls `ex->parseZon2()` and then `build_zones` with the `MapHavok` and `MapProp` lists the props and collision sections already read; keep them in locals.
- [ ] **Step 4: Run** `cm_test_world.exe` → PASS. `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(world): zone scatter in WorldScene; zoneModels linked to their tree models by anchor, geometry and prop`.

### Task 4: Phase-1 dat acceptance `[light]`

**Files:**
- Modify: `tests/test_world_dat.cpp` (reuses `box_iou`'s neighbours: add `containment(hullBox, modelBox)`)
- Test: same

**Interfaces:**
- Consumes `load_world`, `build_zones`'s counts, and `load_model_by_fileid(dat, fileId, ModelLoadOptions{false, false})` (`entry_extractor.h:246`).
- Produces nothing.

- [ ] **Step 1: Write failing tests**
  - `world_dat.zones_phase1_acceptance`, for each of 192711 / 191000 / 1151420:
    - placements by type are 167 169 / 39 649 / 10 296, then 29 768 / 1 669 / 1 011, then 7 358 / 0 / 284 (M1);
    - `colliderEntries` is 4381 / 88 / 284;
    - linked by anchor 4378 / 88 / 284, and by geometry 3 / 0 / 0;
    - **`collidersUnlinked == 0`**;
    - `colliderGeometries` is 45 / 7 / 1, and every geometry maps to exactly one fileId;
    - `propCrossChecks` is 37 / 4 / 0, and `propDisagreements == 0`;
    - every anchor-linked entry is within ±24.01 of its anchor on both axes;
    - no warning starts with `chunk zon2`.
  - `world_dat.zones_every_collider_has_a_tree`. For each map, every `CollisionInstance` with `group == "zone"` has a `ZoneInstance` with `collider == placement`. Each distinct model of those instances loads with at least one mesh that has indices and positions. This is spec D1's acceptance, "every zone collider has a visible tree".
  - `world_dat.zones_hull_in_model`. Over every linked geometry, take the last animation's hulls in model-local space (vertices × diag(32, 32, −32)) and the model's LOD0 vertex box. The share of the hull box inside the model box has median ≥ 0.95 and min ≥ 0.8. Print every geometry's value.
- [ ] **Step 2: Run** `cm_test_world_dat.exe world_dat.zones_` → FAIL until Task 3's code holds every number.
- [ ] **Step 3: Fix what fails.** Fix the code, never the expected values (they are M1-M4). If a number cannot be reached, stop and report it with the measured value. That is a spec question, not an implementer's choice.
- [ ] **Step 4: Run** → PASS, with no new `cm_test_dat` failures.
- [ ] **Step 5: Commit** `test(world): phase-1 zone acceptance on three maps: every zone collider has a tree model`.

### Task 5: `FoliageGrid`, fade and `FoliageBatch` in `WorldView` `[review]`

**Files:**
- Create: `include/castlemist/render/world_foliage_grid.h` and `src/render/world_foliage_grid.cpp`. Both are pure and D3D-free.
  - The spec places the header in `src/render/`. It is public instead, as `world_prep.h` is, because the ui worker builds the grid and `WorldView::set_world` takes it.
- Modify:
  - `include/castlemist/render/world_view.h`:
    - `WorldLayer::Foliage` goes between `Water` and `Count`;
    - `WorldViewStats` gains the D6 stats fields;
    - `set_world` gains the grid;
    - add `set_foliage_show`, `set_foliage_distance_scale` and `focus_zone_tree`.
  - `src/render/world_draw.h/.cpp`: `FoliageBatch` follows the `InstancedBatch` pattern, with its own 64-byte stream.
  - `src/render/world_view_shaders.h`: the foliage VS, plus PS variants (AO, textured) with the dither discard.
  - `src/render/world_view.cpp`:
    - zone instances recorded in `set_world`;
    - placed in `add_models` (sphere into the grid);
    - culled and drawn per frame;
    - stats.
- Test: `tests/test_render_world_view.cpp` (append, `cm_test_render`)

**Interfaces:**
```cpp
namespace castlemist::render {
inline constexpr float kFoliageTileUnits = 1536.0f;     // castlemist's choice (foliageTileUnits)
inline constexpr uint32_t kFoliageMaxVisible = 20000;   // castlemist's choice (foliageMaxVisible)
float foliage_fade(float d, float n, float f);          // 1 for d <= n, 0 for d >= f, linear between (n >= f: step at f)
float bayer4x4(int px, int py);                         // (M[py&3][px&3] + 0.5) / 16, M = {0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}
struct FoliageTile { std::vector<uint32_t> instances;   // sorted by (model, index)
                     float aabbMin[3]{}, aabbMax[3]{}; bool hasAabb = false; float farMax = 0; };
struct FoliageGrid {
    float x0 = 0, y0 = 0, tileUnits = kFoliageTileUnits; int tilesX = 0, tilesY = 0;   // tiles over scene.bounds, ty north from y0
    std::vector<FoliageTile> tiles;                      // ty * tilesX + tx
    std::vector<uint32_t> tileOf;                        // per zone instance
    std::vector<float> fadeNear, fadeFar;                // per instance, map units (unresolved layer: the map's largest type-3 fadeRange)
    std::vector<float> sphere;                           // 4 per instance (cx, cy, cz, r); r < 0 until its model arrives
    std::vector<uint8_t> collider;                       // per instance: source == Collider
};
FoliageGrid build_foliage_grid(const castlemist::world::WorldScene& s, float tileUnits = kFoliageTileUnits);
/// Sets instance i's world sphere and grows its tile's AABB; out-of-range i is ignored.
void foliage_set_sphere(FoliageGrid& g, uint32_t i, const float centre[3], float radius);
struct FoliageVisible { uint32_t instance = 0; float fade = 0; };
struct FoliageCullStats { int tiles = 0; int capped = 0; };
/// Spec D6 "Cull" steps 1-2 and the cap: tiles by AABB distance > farMax*scale or outside `planes`
/// (inside: dot(n, p) + d >= -r), then instances by fade == 0 or sphere outside; tiles visited nearest first,
/// and instances past maxVisible are counted in st.capped. collidersOnly keeps collider == 1 only.
void foliage_cull(const FoliageGrid& g, const float cam[3], const float planes[6][4], float distanceScale,
                  bool collidersOnly, uint32_t maxVisible, std::vector<FoliageVisible>& out, FoliageCullStats& st);
enum class FoliageShow : uint8_t { Colliders, All };
// WorldView:
//   void set_world(const castlemist::world::WorldScene& s, std::unique_ptr<FoliageGrid> grid = nullptr);
//   void set_foliage_show(FoliageShow);              // Colliders after create()
//   void set_foliage_distance_scale(float);          // foliageDistanceScale, 1 after create()
//   bool focus_zone_tree(WorldCamera& cam, int rect[4]);   // spec D6 Headless: the Collider instance nearest cam.pos,
//                                                    // from 4x its radius; Collision layer on; rect = its screen rect
// WorldViewStats gains: int foliageInstances = 0, foliagePlaced = 0, foliageVisible = 0, foliageDraws = 0,
//   foliageKtris = 0, foliageTiles = 0, foliageCapped = 0, zoneTrees = 0, zoneUnlinked = 0, shaderFoliage = 0;
//   float foliageCullMs = 0;
}
```
**Draw rules (spec D6):**
- The instance stream is a row-vector 4 × 3 world plus one float fade, 64 bytes. LOD comes from screen size by the props' rule. Draws are bucketed by (mesh, LOD).
- Depth writes stay on. The pixel shader discards when `fade < bayer4x4(SV_Position.xy)`.
- Cutout follows the material's alpha mode, as props do.
- Under Shader shading, foliage draws in the Materials look and is counted in `shaderFoliage`.
- The prop stream, its pipelines and `pick()` are unchanged.
- A model shared by a prop and a zone uses one `MeshGPU`.

- [ ] **Step 1: Write failing tests** (`cm_test_render`; a synthetic scene with bounds (0, 0, 6144, 6144) and zone instances on a known layer with `fadeRange` [100, 200])
  - `foliage_grid.fade_ends`: `foliage_fade(50, 100, 200) == 1`, `(150, …) == 0.5`, `(200, …) == 0` and `(250, …) == 0`.
  - `foliage_grid.bayer_matrix`: `bayer4x4(0, 0) == 0.5f/16` and `bayer4x4(1, 0) == 8.5f/16`. All 16 values are distinct and lie in (0, 1). `(4, 4)` equals `(0, 0)`.
  - `foliage_grid.instances_in_tiles`: instances at (10, 10) and (1600, 10) are in tiles 0 and 1, and `tilesX == 4`. Each tile's `farMax` is 200 × `distanceScale` 1.
  - `foliage_grid.aabb_from_spheres`: after `foliage_set_sphere` with centre (10, 10, −5) and r 3, tile 0's AABB is (7, 7, −8)…(13, 13, −2) and `hasAabb`.
  - `foliage_grid.far_tile_culled`: a camera 5000 units from tile 0 sees nothing from it. A camera inside tile 0 sees its instance with fade 1 (the camera-inside case).
  - `foliage_grid.cap_keeps_nearest`: 3 tiles of 10 instances each at increasing distance with `maxVisible = 15`. All of the nearest tile, 5 of the next, and `capped == 15`.
  - `foliage_grid.colliders_only`: with `collidersOnly`, Density instances are excluded.
  - `foliage_grid.tile_without_spheres_skipped`: a tile whose instances never got a sphere is not counted in `st.tiles` and yields nothing. Its neighbour with spheres still yields.
  - `foliage_grid.unloaded_instance_not_visible`: one instance with r < 0 in a tile whose other instance has a sphere. Only the loaded one is visible.
  - `foliage_grid.set_sphere_out_of_range_ignored`: `foliage_set_sphere(g, 999, …)` does not crash or change anything.
  - The existing `world_view.*` and `world_materials.*` render tests pass unchanged.
- [ ] **Step 2: Run** `cm_test_render.exe foliage_grid.` → FAIL.
- [ ] **Step 3: Implement** the grid, then the `WorldView` side. `set_world`'s UI-thread work for foliage is limited to taking ownership of the grid and sizing the CPU instance array.
- [ ] **Step 4: Run** `cm_test_render.exe` → PASS. Build the app.
- [ ] **Step 5: Commit** `feat(render): foliage grid, screen-door fade and instanced foliage batch in WorldView`.

### Task 6: Map Window foliage: worker grid, toggle, headless zone-tree capture `[review]`

**Files:**
- Modify:
  - `src/ui/detail/map_window_loader.h`: `MapWinWorldMsg` (:112) gains `std::unique_ptr<castlemist::render::FoliageGrid> foliage;`.
  - `src/ui/map_window_loader.cpp`: after `load_world` (:114-116), build the grid on the worker.
  - `src/ui/detail/app_state.h`: `ID_MW_FOLIAGE = 2225`, `ID_MW_FOLIAGE_COLLIDERS = 2226` and `ID_MW_FOLIAGE_ALL = 2227`.
  - `src/ui/map_window.cpp`:
    - the toolbar toggle **Foliage** and its menu **Colliders / All** (All is greyed while `scene.zones.mode == Colliders`);
    - `set_world(scene, std::move(foliage))`;
    - the status line `fol V/N`;
    - the stats line (:582-600) gains `foliageInstances=%d foliagePlaced=%d foliageVisible=%d foliageDraws=%d foliageKtris=%d foliageTiles=%d foliageCapped=%d foliageCullMs=%.2f zoneTrees=%d zoneUnlinked=%d shaderFoliage=%d treeRect=%d,%d,%d,%d`.
  - `src/ui/detail/map_window.h` `MapWinHeadless`: gains `int foliage = -1` (−1 unset, 0 off, 1 colliders, 2 all) and `bool focusZoneTree`.
  - `src/ui/application.cpp` (:797): read `GW2_MAPWIN_FOLIAGE` and `GW2_MAPWIN_FOCUS=zonetree`.
  - `include/castlemist/core/env.h`: two table rows, worded as spec D6 "Headless".
  - `tests/test_ui_layout.cpp`: the three ids join the `control_ids.are_unique` table (:134-146).
  - `docs/testing.md`: the two hooks and the new stats fields under "The Map window".
- Test: `tests/test_ui_layout.cpp`, plus the headless run

**Interfaces:**
- Consumes `build_foliage_grid`, `WorldView::set_world(scene, grid)`, `set_layer(WorldLayer::Foliage, …)`, `set_foliage_show` and `focus_zone_tree`.
- Produces the stats fields above. Task 11 reads them.

- [ ] **Step 1: Write the failing test.** `control_ids.are_unique` lists `ID_MW_FOLIAGE`, `ID_MW_FOLIAGE_COLLIDERS` and `ID_MW_FOLIAGE_ALL`.
- [ ] **Step 2: Run** `cm_test_ui.exe control_ids.` → FAIL (the ids are undefined).
- [ ] **Step 3: Implement.** Foliage is on after `create()`, and the sub-mode starts at Colliders.
- [ ] **Step 4: Run** `cm_test_ui.exe` → PASS. Then run the headless capture:
  ```bash
  GW2_DAT="E:\Games\gw2\Guild Wars 2\Gw2.dat" GW2_DUMP_DIR=dumps/mapwin GW2_MAPWIN=192711 \
  GW2_MAPWIN_FOLIAGE=colliders GW2_MAPWIN_FOCUS=zonetree GW2_MAPWIN_SHOT=dumps/mapwin/queensdale_zonetree.bmp \
    ./build/debug/bin/castlemist.exe
  ```
  - Expect `dumps/mapwin/mapwin.txt` to hold `ok=1`, `zoneTrees=4381`, `zoneUnlinked=0`, a non-empty `treeRect`, `foliageVisible>0` and `timeout=0`.
  - Open the bmp. A tree is drawn around its collider. Keep the bmp out of git (`dumps/` is ignored).
  - Repeat with `GW2_MAPWIN_FOLIAGE=off`: `foliageVisible=0`.
  - Check that no `dumps/index/gw2_index.db` appeared (Task 1).
- [ ] **Step 5: Commit** `feat(ui): Map Window Foliage layer (zone collider trees), headless zone-tree capture`.
- [ ] **Step 6: Hand phase 1's visible result to the user.** The controller reports the capture's numbers and the bmp path, and asks the user to walk Queensdale (spec Testing, "Map Window, Queensdale") with the debug build. Do not wait: continue with Task 7. Record the user's result in Task 16's amendments; if they have not answered by then, record it as "not run".

### Task 7: Export: foliage records, manifest blocks, wind flag, `--foliage` `[review]`

**Files:**
- Create: `include/castlemist/exportgltf/world_foliage.h`, `src/exportgltf/world_foliage.cpp`
- Modify:
  - `include/castlemist/world/cells.h` and `src/world/cells.cpp`: `Cell` gains `std::vector<uint32_t> foliage` (indices into `scene.zones.instances`), bucketed as props are. Foliage does not count toward the triangle budget.
  - `include/castlemist/exportgltf/world_manifest.h` and `src/exportgltf/world_manifest.cpp`:
    - `WorldFolderOptions` gains `FoliageExport foliage = FoliageExport::All`. This default is also what File ▸ Export World (Map)… uses;
    - `ManifestModel` gains `int meshes = 0, materialCount = 0` and `nlohmann::json wind`;
    - `ManifestCell` gains `foliageFile`, `foliageRecords`, `colliderTrees` and `foliageTriangles`;
    - `WorldManifestInput` gains `nlohmann::json foliage`;
    - `world_export_settings` gains the spec D11 rows that apply (every `zone*` and `foliage*` row, plus `lodCullFovDeg`). The terrain rows come in Task 12.
  - `src/exportgltf/world_folder.cpp`: zone-only models are needed models; the records are written per kept cell before the manifest.
  - `tools/world/castlemist-world.schema.json`: `foliage`, `cells[].foliage` and `counts.foliage`, all optional, with `additionalProperties: false`. The record layout goes in the description text.
  - `tools/world/validate_manifest.py` (the semantic checks and `--self-test`) and `tools/world/test_validate_manifest.py`.
  - `tests/world_export_synthetic.h`: zone layers, placements and instances, and a model 103 (static grass, one mesh, one material).
  - `tools/gw2dat_cli/main.cpp` `cmd_world_export`: `--foliage off|colliders|all` (default all). Also the usage line.
- Test: `tests/test_exportgltf_world.cpp`, plus the `world_manifest_*` ctests

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
inline constexpr size_t kFoliageRecordBytes = 100;
enum class FoliageExport : uint8_t { Off, Colliders, All };   // All = whatever scene.zones.mode built
struct FoliageRecord {
    uint32_t model = 0; uint16_t layer = 0xFFFF; uint16_t flags = 0; uint32_t placement = 0xFFFFFFFF;
    float m[12]{};          // map-space 3x4, column-major: world[0..2], [4..6], [8..10], [12..14]
    UnityTrs unity{};       // unity_trs(world, cell.origin)
    float unityYaw = 0;     // f = R_unity * (0,0,1); atan2(f.x, f.z)  (sign UNPROVEN, terrainTreeYawSign)
    float tiltDeg = 0;      // degrees between normalize(M3 * (0,0,-1)) and (0,0,-1)
};
// flags: bit 0 collider; bits 1-2 source (0 collider, 1 scatter, 2 density); bit 3 Terrain tree (Task 12);
//        bit 4 outside the terrain (chunk_of_point clamped)
void encode_foliage_record(const FoliageRecord& r, uint8_t out[kFoliageRecordBytes]);   // little-endian, spec D7 offsets
FoliageRecord decode_foliage_record(const uint8_t* in);
/// One cell's records, sorted by (model, placement, collider). Instances whose unity_trs fails are skipped
/// and counted in `skipped`.
std::vector<FoliageRecord> cell_foliage_records(const castlemist::world::WorldScene& s, const castlemist::world::Cell& c,
                                                FoliageExport mode, size_t& skipped);
/// true / false / null per spec D7: true when any ok game material's vsUniforms names "VegPara",
/// false when there are ok game materials and none does, null when there is no ok game material.
nlohmann::json model_wind(const ModelPreview& p);
/// The manifest's "foliage" block (spec Data formats), from the scene and the models' manifest entries.
nlohmann::json foliage_manifest_json(const castlemist::world::WorldScene& s, const std::vector<ManifestModel>& models);
}
```
**The synthetic fixture** (`tests/world_export_synthetic.h`) gains:
- one type-3 layer whose models are {101};
- one type-1 layer whose models are {103};
- a collider tree of model 101 (Collider, placement 0, collider 0);
- a geometry-only collider of model 101 (`layer = placement = UINT32_MAX`, collider 1);
- two grass instances of model 103 (Density, placements 1 and 3; `fmix32(1) < 2³¹` and `fmix32(3) ≥ 2³¹`, so density 0.5 keeps exactly one).

Model 101 is shared with the props, and 103 is zone-only.

- [ ] **Step 1: Write failing tests**
  - `world_foliage.record_roundtrip`: encoding gives 100 bytes and decoding returns every field. The fields sit at the D7 offsets (`model` at 0, `m` at 12, `unity.t` at 60, `tiltDeg` at 96).
  - `world_foliage.synthetic_records`. `export_world_folder` on the fixture writes `foliage/c0_0_2x2.bin` of 400 bytes, holding 4 records sorted by (model, placement):
    - both model-101 records come first, the geometry-only one last, with `layer == 0xFFFF` and `placement == 0xFFFFFFFF`;
    - the collider records have bit 0 set and source 0, and the grass records have source 2 and bit 0 clear;
    - for each record, the `unity` TRS rebuilt with `map_matrix_to_unity` reproduces `m` relative to the cell origin within 1e-4.
  - `world_foliage.manifest_blocks`:
    - `foliage.scatter == "colliders"`, `recordBytes == 100` and `layers.size() == 2`;
    - `foliage.models` covers 101 and 103;
    - `cells[0].foliage.records == 4` and `colliderTrees == 2`;
    - `counts.foliage.records == 4`, `colliderTrees == 2` and `modelsFoliageOnly == 1`;
    - `models` has a folder for 103;
    - `world_manifest.keys_match_schema` still passes.
  - `world_foliage.mode_filters`: `FoliageExport::Colliders` writes 2 records, and `Off` writes no `foliage/` folder, with `cells[0].foliage == null`.
  - `world_foliage.wind_flag`: a preview with one ok game material whose `vsUniforms` names `VegPara` gives true, one without gives false, and no game materials gives null.
  - `world_foliage.bad_transform_skipped`: a collider instance with scale 0 and one with a NaN are skipped. `counts.foliage.skipped == 2`, with one warning `foliage: 2 placements with a degenerate transform skipped`. The cell's collider part still holds its hulls.
  - `world_foliage.region_filters_records`: the 2 × 2 region {0, 0, 0, 0} keeps only the records inside chunk (0, 0).
  - `world_foliage.outside_terrain_clamped_bit4`: an instance at x = −20000 lands in a cell with `cx0 == 0`, with bit 4 set.
  - `world_manifest.settings_labelled` (existing) passes with the new rows. `zoneVisualTransform` and `zoneFade` are UNPROVEN and in `warnings`.
  - The ctests `world_manifest_validate` (`^OK`) and `world_manifest_selftest` (`SELFTEST OK`). The self-test gains three corruptions, each failing with its message:
    - a bin truncated by 1 byte: `foliage size: foliage/c0_0_2x2.bin`;
    - a record's `model = 9999`: `index out of range: foliage/c0_0_2x2.bin[0].model`;
    - `foliage.extra = 1`: `unexpected property: foliage.extra`.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_foliage.` and `ctest --preset debug -R world_manifest` → FAIL.
- [ ] **Step 3: Implement.**
  - Records are written, then the manifest (still last).
  - A failed write is fatal and leaves no `manifest.json`.
  - A failed foliage model is `status: failed`, and its records are skipped and counted in `counts.foliage.failedModel`.
- [ ] **Step 4: Run** → PASS. Then run `gw2dat_cli world-export --dat "$GW2_TEST_DAT" --file-id 1151420 --template <tpl> --out-dir "$TMP/sv" --no-fbx`:
  - `"ok": true`;
  - `counts.foliage.colliderTrees == 284`;
  - `python -I tools/world/validate_manifest.py "$TMP/sv"` → `OK`.
- [ ] **Step 5: Commit** `feat(world-export): foliage records per cell, manifest foliage block, wind flags, --foliage`.

### Task 8: Importers, phase-1 level: Unity tree prefabs, Blender geometry nodes `[review]`

**Files:**
- Modify:
  - `tools/world/validate_manifest.py` `plan()` (:310): the `PLAN` line gains ` foliageTrees=<n>` at its end. A record is a tree when its layer's `type == 3` or `layer == 0xFFFF`.
  - `tools/blender/castlemist_world.py --import`: one geometry-nodes object per (cell, model) under `Cells/<id>/Foliage`, and the shared node group `CM_Foliage` (spec D8, Blender). `--verify` gains `FOLIAGE_OBJECTS <n>` and `FOLIAGE_INSTANCES <n>`.
  - `tools/unity/Castlemist/Editor/`:
    - a new `FoliageRecords.cs` (the 100-byte reader);
    - a new `FoliageImporter.cs` (tree prefabs `Prefabs/Foliage/<fileId>.prefab` with a LOD0-only `LODGroup`, and instances under `Cell_<id>/Foliage/Trees/<fileId>_<placement>`);
    - `WorldImporter.cs` calls it;
    - `ImportSummary.cs` gains `foliageTrees`, and `PlanLine()` matches the validator.
  - `tests/CMakeLists.txt`: `world_manifest_plan`'s expected line, and the `blender_world_import` regex.
- Test: the ctests `world_manifest_plan` and `blender_world_import`

**Interfaces:**
- `cullHeight = clamp(size / (2 · fadeFar · D · tan(fov/2)), 1e-4, 1)`, with `fov = 60°` and D = 1 in this task (spec D8).
- Prefabs have `enableInstancing` on and `BatchingStatic` off.
- Records whose type is 1-2 are not placed by Unity in this task. The summary counts them as `foliageDeferred` in a warning, and Task 13 places them.
- Blender places every record kept by the `--foliage` mode. This task's mode is the default, all.
- Both importers: a missing bin is an error naming it. A record whose model is `status: failed` is skipped and counted (spec Error handling).

- [ ] **Step 1: Write failing tests**
  - `world_manifest_plan` expects `PLAN materials=… skybox=0 foliageTrees=2`: today's expected line (`tests/CMakeLists.txt:272`), with the new field and the fixture's model-103 material counted. Update the `materials=` count to what the validator computes for the new fixture, and check it by hand against model 103's `materials.json`.
  - `blender_world_import`: PASS_REGULAR_EXPRESSION `INSTANCES 4[\s\S]*FOLIAGE_OBJECTS 2[\s\S]*FOLIAGE_INSTANCES 4[\s\S]*VERIFY OK`.
- [ ] **Step 2: Run** `ctest --preset debug -R "world_manifest_plan|blender_world_import$"` → FAIL.
- [ ] **Step 3: Implement.** The C# compiles as C# 9 for 2022.3: no file-scoped namespaces and no global usings.
- [ ] **Step 4: Run** → PASS.
  - Optional local batchmode, with the 2022.3.22f1 editor `tools/unity/CHECKLIST.md` used on 2026-10-15: `-executeMethod Castlemist.BatchEntry.ImportWorld` on the fixture prints the `PLAN` line equal to the validator's.
  - Write the result, or "batchmode not run", into CHECKLIST's results table.
- [ ] **Step 5: Commit** `feat(importers): foliage records as Unity tree prefab instances and Blender geometry-node instances`.

**Phase 1 is done** when Tasks 4, 6 and 8 pass, and the user's walk is recorded or listed as not run (spec "Done means").

## Phase 2: the scatter

### Task 9: Phase-2 research: the scatter rule from the static client, else from the data `[review]`

**Files:**
- Modify: `docs/research/gw2-terrain-to-unity.md` §1.3, with a dated amendment "2026-10-xx: zone scatter rule".
- Modify: `tests/test_world_dat.cpp`. Two measurement tests that print only and never fail:
  - `world_dat.zones_scatter_samples` writes the 4750 anchor-matched samples as CSV to `$CM_ZONE_SAMPLES` when it is set;
  - `world_dat.zones_hypotheses` prints D5 step 3's results.
- Fitting scripts stay in the scratchpad and are not committed (as for the spec's own measurements).

**Interfaces:** the sample CSV columns are:
- `map,entry,zone,seed,placementIndex,k,c,r,flag,layerType,scale0,scale1,rot0..rot5,jitter,`
- `range0,range1,zPos,zoneFlags,encodeIndex,encodeOffset,`
- `tx,ty,tz,rx,ry,rz,scale,u,v`

Here (u, v) = translate − anchor.

The task produces one decision for Task 10, written at the top of the amendment:
- `exact`, with the transcribed rule: generator, seed derivation, draw order and formulas for u, v, yaw, tilt, scale and z, with the addresses or the fit;
- or `densityMatched`, with what was tried.

Either way, the amendment also records the D5 step-3 hypotheses' outcomes (`measured` / UNPROVEN).

- [ ] **Step 1: Static search.**
  1. Open the IDB or Ghidra project (Global Constraints). Find the code that consumes `PackMapZoneV*` data. Ways in (D5 step 1): readers of `flags` / `encodeData`, and uses of `scaleRange`, `rotRange`, `instanceScaleJitter` or `fadeRange`. Start from the struct's field offsets in the template and from the map loader's zone names in `tools/gw2_annotations.json`.
  2. Record every address you visit and what it does.
  3. **Never launch the game.**
  4. Time-box this step to one session's worth of work. If no expansion function is found, record "not found" with the addresses tried.
- [ ] **Step 2: Data fit** (always run, as a cross-check if Step 1 found the rule).
  - Dump the samples: `CM_ZONE_SAMPLES=<scratch>/zone_samples.csv cm_test_world_dat.exe world_dat.zones_scatter_samples`.
  - Try the D5 step-1 generators (LCG variants, xorshift, MT19937, splitmix, hash-of-index) seeded from the zone seed, the placement index or the cell. Try each draw order of offset, yaw and scale. Test `offsetData` and `encodeData` as precomputed values.
- [ ] **Step 3: Validate.** A candidate rule passes when it reproduces all 4750 entries within D5 step 2's tolerances: xy 0.01, yaw 1e-4 rad (mod 2π), tilt 1e-4 rad, scale 1e-5, z 0.5. Queensdale's 3 hand-placed entries are excluded and listed.
- [ ] **Step 4: Hypotheses.** Run `world_dat.zones_hypotheses`. It tests, per D5 step 3:
  - tilt = the terrain normal at (x, y), clamped to `rotRange` x/y;
  - z = `terrain_height_at`;
  - scale = `scaleRange[0] + Δ·k`;
  - `collideData` = the ground (normal, z).

  Each is `measured` only if it holds on all 4750 entries. Print the pass counts and the worst errors.
- [ ] **Step 5: Write the amendment** with the decision, addresses, generators tried, hypotheses and numbers. Mark "the client uses the anchor and slot rule" as proven only if the outcome is `exact`.
- [ ] **Step 6: Commit** `docs(research): zone scatter rule search (static client and data fit) and the phase-2 decision`.

**Deciding rule:** `exact` only if Step 3 passes on all 4750 entries. Otherwise `densityMatched` (the user's answer 5). Don't stop the sub-project.

### Task 10: Phase-2 implementation: `zone_scatter` `[review]`

**Files:**
- Create: `include/castlemist/world/zone_scatter.h`, `src/world/zone_scatter.cpp`
- Modify:
  - `src/world/load_world.cpp`: after `build_zones`, call `scatter_instances(w, zone_scatter_rule_in_force())`.
  - `src/world/zones.cpp`: models for every placement's slot are now resolved and appended.
  - `src/exportgltf/world_manifest.cpp`: `zoneScatter`, `zoneJitter`, `zoneYaw`, `zoneScale`, `zoneTilt` and `zoneZ` take the rule's statuses.
  - `src/ui/map_window.cpp`: the All sub-mode is enabled when the mode is not Colliders.
- Test: `tests/test_world_zones.cpp`, `tests/test_world_dat.cpp`

**Interfaces:**
```cpp
namespace castlemist::world {
struct ZoneScatterRule {
    ZoneScatterMode mode = ZoneScatterMode::Colliders;
    bool tiltFromGround = false;   // D5 step 3 tilt hypothesis measured (Task 9)
    bool zFromTerrain = true;      // z = terrain_height_at; measured or UNPROVEN per Task 9
    // exact mode only: the fields Task 9's transcription needs (generator, seed source, draw order), named after it
};
/// The rule Task 9 settled (a constant; its comment cites the research amendment).
ZoneScatterRule zone_scatter_rule_in_force();
/// Adds one Scatter (exact) or Density instance per placement with collider == UINT32_MAX and a resolved model;
/// none in Colliders mode. Sets scene.zones.mode. Returns the number added. Never doubles a collided placement.
size_t scatter_instances(WorldScene& s, const ZoneScatterRule& r);
/// densityMatched draws: state = (uint64_t(mapFileId) << 32) ^ placementIndex; each draw is
/// (splitmix64_next(state) >> 11) * 2^-53, in [0, 1); order u, v, yaw, scale (castlemist's choice, labelled).
double zone_unit_draw(uint64_t& state);
}
```
**The density rule** (when Task 9 says `densityMatched`) is spec D5 step 4 exactly:
- `u, v = −24 + 48·draw`;
- yaw = `rotRange[2]` in degrees, read as stored, converted to radians;
- scale is uniform in `scaleRange`;
- tilt comes from the ground when `tiltFromGround`, and is upright otherwise;
- `z = terrain_height_at(x, y)`.

The matrix is `client_world_matrix(pos, rot, scale)`. The `world_export_settings` source for each part is "this plan, Task 10; spec D5 step 4".

- [ ] **Step 1: Write failing tests**
  - `scatter.density_deterministic`: two runs on the same synthetic scene give byte-equal `instances`.
  - `scatter.density_ranges`: over 1000 placements, every `u, v` is in [−24, 24), every yaw is within `rotRange.z` (as radians) and every scale is within `scaleRange`.
  - `scatter.never_doubled`: a placement with a collider gets no second instance. The count added equals the placements without a collider that have a model.
  - `scatter.colliders_mode_adds_nothing`.
  - If Task 9 says `exact`: `scatter.exact_unit_vectors`, with 5 samples from the transcription or from `zone_samples.csv` (3 Queensdale, 1 Lion's Arch, 1 Spirit Vale) reproduced to D5's tolerances.
  - Dat `world_dat.zones_phase2`:
    - with `exact`, the 4750 reproductions within the D5 tolerances;
    - with `densityMatched`, every placement without a collider has exactly one Density instance, and ≥ 99 % of them lie inside the terrain rects (the props' figure);
    - either way, it prints the hypotheses (from Task 9) for the amendments.
- [ ] **Step 2: Run** `cm_test_world.exe scatter.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_world.exe`, `cm_test_world_dat.exe world_dat.zones_` and `cm_test_exportgltf.exe` → PASS. Queensdale now has 217 114 zone instances when `densityMatched`, or as the exact rule gives them.
- [ ] **Step 5: Commit** `feat(world): zone scatter (<exact|densityMatched>) for the placements without colliders`.

### Task 11: Map Window at full density: budgets `[light]`

**Files:**
- Modify: `src/render/world_view.cpp` / `world_foliage_grid.cpp` only if a lever is needed.
- Modify: `docs/superpowers/specs/2026-10-16-world-foliage-design.md`, `## Amendments (after implementation)`, the budget figures.
- Test: the headless runs

**Interfaces:** consumes Task 6's stats fields and the existing `bench_*` fields.

- [ ] **Step 1: Measure** on `relwithdebinfo`, Queensdale, `GW2_MAPWIN_SHADING=materials`, with three runs:
  1. `GW2_MAPWIN_FOLIAGE=off`;
  2. `=all`;
  3. `=all` with `GW2_MAPWIN_FOCUS=props` (a low, dense view).
  Read the results from `mapwin.txt`.
- [ ] **Step 2: Compare** with the spec's Performance table:
  - `set_world` < 50 ms (add a `set_world_ms` field to the stats line if it is not already timed);
  - `add_ms_max` < 50;
  - `foliageCullMs` median ≤ 2;
  - `bench_cpu_ms` median increase all-against-off ≤ 3;
  - `bench_gpu_ms` foliage share ≤ 4;
  - `foliageVisible` ≤ 20 000, and report `foliageCapped`.
- [ ] **Step 3: If a target is missed,** take the levers in order: the tile size (try 768), then the cap, then a per-tile static instance buffer. Re-measure after each, and stop at the first that passes. If none passes, record the target as missed with the lever taken (spec "Done means").
- [ ] **Step 4: Write** the figures, the build type and any lever into the spec's amendments. Run `cm_test_render.exe` → PASS.
- [ ] **Step 5: Commit** `perf(render): foliage at full density on Queensdale: <budgets held | lever taken>`.

## Phase 3: Unity Terrain, importers and knobs

### Task 12: `terrain_heightfield` and the `unityTerrain` writer `[review]`

**Files:**
- Modify: `include/castlemist/world/terrain.h`, `src/world/terrain.cpp`. These are beside `terrain_height_at`; the spec's `terrain_height.h` does not exist.
- Create: `include/castlemist/exportgltf/world_unity_terrain.h`, `src/exportgltf/world_unity_terrain.cpp`
- Modify:
  - `src/exportgltf/world_folder.cpp`: run after the cells are baked and before the records are written, because it sets flag bit 3.
  - `world_manifest.cpp`: the `unityTerrain` block, plus the settings rows `terrainTreeMaxTiltDeg`, `terrainTreeYawSign`, `detailPlacement`, `terrainHeightRowOrder` and `terrainColliderDiagonal`.
  - the schema, the validator (tile files and sizes, and `check` points inside their tiles) and the CLI (`--no-unity-terrain`).
- Test: `tests/test_world_cells.cpp` (the heightfield), `tests/test_exportgltf_world.cpp`

**Interfaces:**
```cpp
namespace castlemist::world {
struct TerrainHeightfield {
    int cx0 = 0, cy0 = 0, blockChunks = 4, n = 129;   // n = blockChunks * segments + 1
    float rect[4]{};                                    // x0, y0, x1, y1 of the full (padded) block
    std::vector<float> up;                              // n*n, Unity up = -z, row 0 SOUTH, column 0 west
    std::vector<uint8_t> real;                          // (n-1)^2 quads, row 0 SOUTH: 1 = inside the grid, 0 = padding
};
/// Edge blocks are padded by clamping to the nearest real sample (spec D7).
TerrainHeightfield terrain_heightfield(const Terrain& t, int cx0, int cy0, int blockChunks = 4);
/// Map-wide min and max of -z over every sample (Unity up, map units).
void terrain_up_range(const Terrain& t, float& hMin, float& hMax);
}
namespace castlemist::exportgltf {
struct UnityTerrainInput {
    const castlemist::world::WorldScene* scene = nullptr; const castlemist::world::CellPlan* plan = nullptr;
    const std::vector<std::vector<uint8_t>>* cellAlbedoRgb = nullptr;   // parallel to plan->cells, T*T*3 per chunk area
    int texelsPerChunk = 256;
    std::vector<std::vector<FoliageRecord>>* records = nullptr;          // per cell; bit 3 set here
    const std::vector<ManifestModel>* models = nullptr;                  // meshes / materialCount for terrainDetail
};
/// Writes terrain/<tile>.r16, _holes.png (edge tiles only), _albedo.png, _d<k>.png and _trees.bin (spec D7,
/// Data formats) and returns the manifest's "unityTerrain" block; json(nullptr) and no files when no terrain.
nlohmann::json write_unity_terrain(const UnityTerrainInput& in, const std::string& dirUtf8,
                                   std::vector<std::string>& warnings, std::string* error = nullptr);
}
```
**Rules (spec D7):**
- Tile ids are `t<cx0>_<cy0>`.
- `.r16` is `round((up − hMin) / (hMax − hMin) · 65535)` as uint16 LE.
- Holes are 128 × 128, white = surface, row 0 north.
- Detail cell `(⌊(x − tileX0)/48⌋, ⌊(tileY1 − y)/48⌋)`, row 0 north. A record is a detail when its type is 1-2 and its model has `meshes == 1 && materialCount == 1`.
- Trees are type 3 with `tiltDeg ≤ 5`. Each is 24 bytes `u32 prototype, f32 u, v, h, yaw, scale`, where:
  - `u = (x − tileX0)/12288`;
  - `v = (y − tileY0)/12288`;
  - `h = (−z − hMin)/(hMax − hMin)`;
  - `yaw = unityYaw`;
  - `scale = unity.s`.
- `check` is 32 points from an LCG with a fixed seed (`state = state·6364136223846793005 + 1442695040888963407`, seeded with the map fileId), xz uniform in the tiles. The height is `−terrain_height_at(x, y)`, in Unity space before `unityScale`.

- [ ] **Step 1: Write failing tests**
  - `cells.heightfield_row0_south`: on `grid(4,4)` (Task 2 of sub-project 4's helper), `up[0]` equals `−` the height of the chunk's south-west sample, and `up[128·129]` equals `−` its north-west sample.
  - `cells.heightfield_edge_padded`: on `grid(10,25)`, the block at cx0 = 8 has `real` 0 for quads with x ≥ 64 and padded heights equal to the last real column.
  - `unity_terrain.synthetic_tile`. On the fixture:
    - `t0_0.r16` is 129·129·2 bytes;
    - the row-0 sample decodes to the south edge, normalised by the map range;
    - `_holes.png` exists and is 128 × 128, with quads beyond the 2 × 2 chunks black;
    - `_albedo.png` pixels equal the cell albedo bytes at the same texels;
    - the detail PNG values sum to 2, the grass records;
    - `_trees.bin` holds the 1-record collider tree, with u, v and h reproducing its translation within 1e-4 (that record gets bit 3).
  - `unity_terrain.detail_count_clamps_255`: 300 grass records in one 48-unit cell give value 255. There is one warning naming `t0_0`, and the neighbouring cell's value is unaffected.
  - `unity_terrain.steep_tree_not_terrain`: a type-3 record with `tiltDeg = 10` gets no bit 3 and is not in `_trees.bin`.
  - The ctest `world_manifest_validate` passes, and the self-test adds a missing `.r16` (`missing file: terrain/t0_0.r16`).
- [ ] **Step 2: Run** `cm_test_world.exe cells.heightfield` and `cm_test_exportgltf.exe unity_terrain.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS, then `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(world-export): unityTerrain tiles: heights, holes, assembled albedo, detail counts, Terrain trees, check points`.

### Task 13: Unity importer: Terrain mode, patches, knobs, Quest preset, frame check `[review]`

**Files** (under `tools/unity/Castlemist/Editor/`):
- Create:
  - `TerrainBuilder.cs`: spec D8 "Unity Terrain (baked layer)", per tile, `SetNeighbors`, prototypes, distances, the terrain frame check.
  - `FoliagePatches.cs`: types 1-2, a 1536-unit square per model per cell, `Mesh.CombineMeshes` with at most 65 535 vertices per patch, and a `LODGroup` at `cullHeight`.
- Modify:
  - `FoliageImporter.cs`: modes, Density (`fmix32` in C#), Distance.
  - `WorldImporter.cs`: window fields **Terrain** (Mesh / Unity Terrain), **Foliage** (Off / Collider trees / Trees / All), **Density** 0..1 and **Distance** 0.1..2, and the **Quest preset** button. In Terrain mode, the cells' mesh terrain and `MeshCollider` are not instantiated.
  - `ImportSummary.cs`, and `BatchEntry.cs` (`-castlemistTerrain mesh|terrain -castlemistFoliage off|colliders|trees|all -castlemistDensity f -castlemistDistance f`).
  - `tools/unity/CHECKLIST.md`: the spec's Unity items.
- Modify:
  - `tools/world/validate_manifest.py --plan [--mode mesh|terrain] [--foliage …] [--foliage-density d] [--foliage-distance D]`: the PLAN line gains ` foliagePatches=<n> terrains=<n> detailPrototypes=<n> detailCells=<n> treeInstances=<n>` after `foliageTrees`.
  - `tests/CMakeLists.txt`: the expected line, and a new `world_manifest_plan_terrain`.
- Test: the ctests `world_manifest_plan` and `world_manifest_plan_terrain`, and the optional batchmode run

**Interfaces** (C#):
```csharp
namespace Castlemist {
public enum TerrainMode { Mesh, UnityTerrain }
public enum FoliageMode { Off, ColliderTrees, Trees, All }
public sealed class ImportOptions { public TerrainMode terrain = TerrainMode.Mesh; public FoliageMode foliage = FoliageMode.All;
                                    public float density = 1f, distance = 1f;
                                    public static ImportOptions Quest() { /* Mesh, ColliderTrees, distance 0.5 */ } }
public static class FoliageRecords { public static uint Fmix32(uint h); public static bool Keep(uint placement, ushort flags, float density); }
public static class TerrainBuilder { public static bool CheckFrame(Terrain[] tiles, List<object> check, float unityScale, out string line); }
// WorldImporter.Import(string assetFolder, float unityScale, ImportOptions opts)
}
```
**Keep rule** (spec D9, the same in Python and C#):
- a record with bit 0 is kept whenever its kind is shown;
- a record with `placement == 0xFFFFFFFF` is always kept;
- otherwise `fmix32(placement) < density·2³²`.

**Mode rules:**
- `Collider trees` = bit 0 only.
- `Trees` = type 3 or `layer == 0xFFFF`.
- In Terrain mode:
  - bit-3 records become `treeInstances`;
  - `terrainDetail` type 1-2 records become detail layers, and the rest become patches;
  - the remaining trees stay prefab instances.

- [ ] **Step 1: Write failing tests**
  - `world_manifest_plan` (mesh, all, density 1) expects `… foliageTrees=2 foliagePatches=1 terrains=0 detailPrototypes=0 detailCells=0 treeInstances=0`.
  - `world_manifest_plan_terrain` (`--mode terrain`) expects `… foliageTrees=1 foliagePatches=0 terrains=1 detailPrototypes=1 detailCells=<n> treeInstances=1`. Here n is the number of non-zero detail texels, computed from the fixture by hand in the test comment.
  - `test_validate_manifest.py` gains `test_fmix32_vectors` (0 → 0, 1 → 0x514E28B7) and `test_density_half_keeps_colliders`.
- [ ] **Step 2: Run** `ctest --preset debug -R world_manifest` → FAIL.
- [ ] **Step 3: Implement.** The C# is C# 9. The terrain frame check compares `Terrain.SampleHeight` at the 32 `check` points; a mismatch over 0.5 × `unityScale` is an error naming the axis.
- [ ] **Step 4: Run** → PASS.
  - Optional batchmode (2022.3.22f1) on the fixture in both modes: `PLAN` equals the dry run, and the terrain check prints OK. Repeat on a Spirit Vale export.
  - Record the results, or "not run", in CHECKLIST's results table with the date.
- [ ] **Step 5: Commit** `feat(unity): Unity Terrain mode, foliage patches, density and distance knobs, Quest preset, terrain frame check`.

### Task 14: Blender knobs and `--verify`; validator `--foliage-density` `[light]`

**Files:**
- Modify:
  - `tools/blender/castlemist_world.py`: `--foliage off|colliders|trees|all` and `--foliage-density f`. `--verify` compares `depsgraph.object_instances` matrices with `C_b M C_b⁻¹`: the 3 × 3 within 1e-4, and the translation within 1e-4 · max(1, |t|). It prints `FOLIAGE_MATRIX_MAX_ERR <e>` and `FOLIAGE_KEPT <n>`.
  - `tools/world/validate_manifest.py --plan`: also prints `FOLIAGE records=<n> kept=<n> colliders=<n>` under the given `--foliage` and `--foliage-density`.
  - `tests/CMakeLists.txt`: two Blender ctests and one Python ctest.
- Test: those ctests

**Interfaces:** consumes the keep rule of Task 13. Python's `fmix32` is the same function the validator uses; import it from `validate_manifest.py`.

- [ ] **Step 1: Write failing tests**
  - `world_manifest_plan_density`: `--plan --foliage-density 0.5` prints `FOLIAGE records=4 kept=3 colliders=2`: both colliders, and grass placement 1 but not 3.
  - `blender_world_import_density`: `--import <fixture> --foliage-density 0.5 --verify` matches `FOLIAGE_KEPT 3[\s\S]*VERIFY OK`.
  - `blender_world_import_colliders`: `--foliage colliders --verify` matches `FOLIAGE_INSTANCES 2[\s\S]*VERIFY OK`.
  - `blender_world_import` also requires `FOLIAGE_MATRIX_MAX_ERR` below 1e-4 for `VERIFY OK`.
- [ ] **Step 2: Run** `ctest --preset debug -R "world_manifest_plan_density|blender_world_import"` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(blender): foliage mode and density options, foliage matrix verify; validator kept counts`.

### Task 15: Dat export test and three-map measurements `[light]`

**Files:**
- Modify: `tests/test_exportgltf_world_dat.cpp`
- Test: same

**Interfaces:** consumes `export_world_folder` and the Spirit Vale test's pattern (`world_export_dat.spirit_vale`).

- [ ] **Step 1: Write failing tests**
  - `world_export_dat.spirit_vale_foliage` (1151420, `--no-fbx` defaults):
    - `ok`;
    - `validate_manifest.py` returns 0 when Python is on PATH;
    - `counts.foliage.colliderTrees == 284`;
    - Σ records over the cells == `scene.zones.instances.size()` − `counts.foliage.skipped`;
    - every collider-tree record's `m` equals its `ZoneInstance::world`, element-wise within 1e-4;
    - every `unityTerrain.check` height equals `−terrain_height_at` within 1e-3.
  - `world_export_dat.measure` (gated by the existing `CM_WORLD_EXPORT_MEASURE=1`) also prints per map:
    - foliage records;
    - bin bytes;
    - foliage-only model folders;
    - terrain tiles and bytes;
    - the stage seconds.
- [ ] **Step 2: Run** `GW2_TEST_DAT=… GW2_TEST_TEMPLATE=… cm_test_dat.exe world_export_dat.spirit_vale_foliage` → FAIL until it holds.
- [ ] **Step 3: Fix the code, never the expected values.**
- [ ] **Step 4: Run** → PASS (the 11 pre-existing `cm_test_dat` failures stay the only ones). Run `measure` and keep its output for Task 16. Then run the schema and the dry run on Queensdale and Lion's Arch exports too (spec "Done means", phase 3).
- [ ] **Step 5: Commit** `test(world-export): Spirit Vale foliage export against WorldScene; three-map foliage measurements`.

### Task 16: Docs, checklists, amendments, and the whole-branch review `[light]`

**Files:**
- Modify:
  - `docs/testing.md`: a "World foliage" section with the spec's manual checklists, the new ctests and the hooks.
  - `docs/using-castlemist.md`: the Foliage toggle and sub-mode, `world-export --foliage` / `--no-unity-terrain`, and the importer options.
  - `docs/research/gw2-terrain-to-unity.md`: cross-links from §1.3, §6 and §8 to the spec and the amendment.
  - `docs/research/README.md`: an index line if a note changed.
  - `docs/superpowers/specs/2026-10-16-world-foliage-design.md`, `## Amendments (after implementation)`:
    - Task 2's `gw2dat_cli map` change;
    - Task 6's capture numbers and the user's Queensdale walk, or "not run";
    - Task 9's decision;
    - Task 11's budgets;
    - Task 12's choices (heightfield in `terrain.h`, the public grid header);
    - Tasks 8 and 13's batchmode results;
    - Task 15's measurements;
    - the Blender and Unity checklist results with dates; anything not tried is listed as not tried.
  - `docs/superpowers/specs/2026-10-15-world-export-design.md`: one line under its "Out of scope", saying that zone models are now sub-project 5 and Udon is 6.
- Test: none (docs)

- [ ] **Step 1: Run the Blender manual checklist** on the Spirit Vale export: view the scene, check that the foliage instances are in place, and render one Cycles frame at the spawn.
- [ ] **Step 2: Run the Unity checklist** if a VRChat 2022.3 project is available. Otherwise "done" is "dry run + schema + batchmode", stated as such.
- [ ] **Step 3: Write** the docs and the amendments. Check every UNPROVEN setting against `settings`, `warnings` and its code comment (spec "Done means", Always).
- [ ] **Step 4: Run** `ctest --preset debug` → PASS. `cm_test_dat` shows only the 11 pre-existing failures. Check that no stray `dumps/index/gw2_index.db` exists.
- [ ] **Step 5: Commit** `docs: world foliage testing, usage, research cross-links and spec amendments`.
- [ ] **Step 6: Whole-branch review.**
  - Dispatch one reviewer on the most capable model over `master...world-foliage`, with the spec, this plan and its Review Focus.
  - Fix what it finds in follow-up commits, then use superpowers:finishing-a-development-branch.
