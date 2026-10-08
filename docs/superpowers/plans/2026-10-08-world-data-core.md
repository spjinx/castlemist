# World Data Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `world` layer whose `load_world()` turns a GW2 map into one `WorldScene` (terrain + materials, props, collision, water, environment, motion) in a single proven frame, checked against T3D on three maps.

**Architecture:** `src/world/` sits on `extract`; it calls the native `Extractor` readers (adding the two it lacks: terrain materials and Havok placements) and owns every conversion into the frame. A Node script runs T3D's parser on the same map bytes and writes reference numbers that dat tests compare against. A research note records every convention with evidence.

**Tech Stack:** C++20, CMake layers (`cmake/Layers.cmake`), nlohmann::json, the repo's `CM_TEST` framework; Node 24 + the user's T3D fork (`github.com/spjinx/t3d`, `parser/` package) for references only.

**Spec:** `docs/superpowers/specs/2026-10-08-world-data-core-design.md`

## Global Constraints

- No invented conventions: every axis, unit, rotation order, chunk layout and UV rule cites a template field, the game's code, or agreement with T3D on real data; otherwise it is UNPROVEN, left out or flagged, and listed in `warnings`. No fudge constants.
- Template-driven parsing by field name; no hard-coded offsets or versions.
- T3D is GPL-3, castlemist MIT: port understanding, credit it in comments (`// after spjinx/t3d TerrainRenderer.ts`), copy no code. `tools/world/` runs T3D from a checkout *outside* this repo.
- `world` depends on `castlemist::extract` and `ext::json` only.
- `load_world` never throws for bad map data and never returns a silently empty scene; dat I/O failure and a missing template are errors.
- Committed reference files hold numbers only, no asset data.
- Tolerances: integers exact; positions/heights within 0.01 world units; quaternion components within 1e-4 (sign-normalised).
- Test maps: Queensdale 192711, Lion's Arch 191000, Spirit Vale 1151420.
- No Udon/gameplay data in `WorldScene`.

## Review Focus

1. A map with no `parm` rect — expect a warning and no terrain placement from an invented rect (the old ±3072 fallback must not return). Test in Task 4.
2. A terrain whose sample count per chunk is not `(verticesPerChunkSide+3)^2` — expect the chunk layout reported UNPROVEN and terrain left empty with a warning, not a reshaped grid. Test in Task 4.
3. A collision geometry index or collision index out of range — expect that placement skipped with one aggregated warning, not a crash. Test in Task 7.
4. A terrain chunk whose material texture index is past `texFileArray` — expect that chunk's material unresolved and named in warnings; the "every chunk resolves" dat test still passes on the three maps. Test in Task 5.
5. A map file that is not a `mapc`/area packfile (e.g. a model fileId given to `world`) — expect a clear error from the CLI, not an empty summary with `ok: true`. Test in Task 9.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `include/castlemist/world/world_scene.h` | `WorldScene` and its part structs (plain data) |
| `include/castlemist/world/frame.h`, `src/world/frame.cpp` | the frame: GW2 map space → Unity / Blender conversions |
| `include/castlemist/world/terrain.h`, `src/world/terrain.cpp` | chunk grid, de-tiling, chunk placement, terrain materials |
| `include/castlemist/world/props.h`, `src/world/props.cpp` | model table + instance transforms |
| `include/castlemist/world/collision.h`, `src/world/collision.cpp` | Havok hulls placed by obs/prop/zone models |
| `include/castlemist/world/load_world.h`, `src/world/load_world.cpp` | `load_world`, water/env/motion attach, unread-chunk warnings |
| `include/castlemist/native/gw2model.hpp` | add `parseTerrainMaterials()`, `parseHavokPlacements()` |
| `tools/world/t3d_reference.mjs`, `tools/world/README.md` | T3D reference dumper |
| `tests/world_ref/*.json` | reference numbers per test map |
| `tests/test_world.cpp` (pure), `tests/test_world_dat.cpp` (dat) | tests |
| `docs/research/gw2-world-frame.md` | conventions with evidence |
| `tools/gw2dat_cli/main.cpp` | `cmd_world` |

Existing facts the tasks rely on (read these first):
- Prop transform is already proven against the client and T3D: `sceneWorld` in `src/render/detail/math.h:160-189` (row-vector `rotZ(-r2)*rotX(-r0)*rotY(-r1)`, scale, translate); the exportgltf copy is `scene_world` in `src/exportgltf/transform_math.cpp:66`.
- Prop parsing already reads `propArray`, `propAnimArray`, `propMetaArray`, `propInstanceArray` + `transforms[]` (`gw2model.hpp:1827`).
- Collision is known incomplete: hulls are local, placements unapplied (`gw2model.hpp:1123-1131`).
- T3D terrain (`library/src/dataRenderer/TerrainRenderer.ts`): chunk grid `numChunksD_1 = sqrt(dims[0]*chunkCount/dims[1])`, `numChunksD_2 = chunkCount/numChunksD_1`; each chunk stores `(segments+3)^2` samples and T3D drops the outer ring, using the inner `(segments+1)^2`; heights negated; chunk size `rect extent / chunk count`; an odd/even offset on Y (`rect[1] ± cdy/2`) that T3D does not explain; materials `materials.materials[chunk].loResMaterial.texIndexArray` (first half indexes `materials.texFileArray`), picker pages from the `pimg` file's `pgtb.strippedPages` (layer 0 and 1, 4x4 chunks per page, page offset `(cx%4)/4`, `0.75-(cy%4)/4`); `uvScale` hard-coded 8 (T3D TODO).
- T3D collision (`HavokRenderer.ts`): `geometries[geometryIndex].animations[last]` → `animations[...].collisionIndices[]` → `collisions[i]`; transform pos `(x,-z,-y)`, Euler `(r0,-r2,-r1)` order ZXY, scale `32*model.scale`, obs models scale 1.
- castlemist current de-tiling (`src/extract/map_scene.cpp:90`) treats the 3 extra samples as overlap starting at index 0 — disagrees with T3D's border ring; Task 4 settles it.

---

### Task 1: `world` layer, `WorldScene` types, test targets

**Files:**
- Create: `include/castlemist/world/world_scene.h`, `src/world/world_scene.cpp` (layer needs one source)
- Modify: `CMakeLists.txt` (after the `exportgltf` layer line 140: `castlemist_add_layer(world DEPS castlemist::extract ext::json)`)
- Modify: `tests/CMakeLists.txt` (`castlemist_add_test(world SOURCES test_world.cpp DEPS castlemist::world)` and `castlemist_add_test(world_dat SOURCES test_world_dat.cpp DEPS castlemist::world castlemist::extract)` with `TIMEOUT 900`)
- Test: `tests/test_world.cpp`, `tests/test_world_dat.cpp` (the latter starts with the dat/template helpers copied from `tests/test_extract_dat.cpp:40-86`: `shared_dat`, `ensure_template`, `packfile_by_file_id`)

**Interfaces — Produces** (`namespace castlemist::world`):

```cpp
struct TerrainMaterial {
    bool resolved = false;
    std::vector<uint32_t> textureFileIds;   // ground textures, dat order
    uint32_t materialFileId = 0;            // the chunk's terrain material (shader) file
    uint32_t pickerFileId = 0, picker2FileId = 0;  // blend pages (layer 0 / 1); 0 = none
    float pickerOffset[2] = {0, 0};         // this chunk's sub-rect in its page
    float pickerScale = 0.25f;              // page covers 4x4 chunks
};
struct TerrainChunk {
    int cx = 0, cy = 0;
    float rect[4] = {0, 0, 0, 0};           // x0, y0, x1, y1 in map space
    int samples = 0;                        // per side (segments + 1)
    std::vector<float> heights;             // samples*samples, row-major, map-space Z
    TerrainMaterial material;
};
struct Terrain { bool present = false; int chunksX = 0, chunksY = 0; std::vector<TerrainChunk> chunks; };
struct PropModel { uint32_t fileId = 0; };
struct PropInstance {
    uint32_t model = 0;                     // index into WorldScene::models
    float world[16] = {};                   // column-major, map space
    float pos[3] = {}, rot[3] = {}; float scale = 1;  // as stored, for references
    std::string group;                      // "propArray" | "propAnimArray" | "propMetaArray" | "propInstanceArray"
};
struct CollisionMesh { std::vector<float> verts; std::vector<uint32_t> indices; };   // local
struct CollisionInstance { uint32_t mesh = 0; float world[16] = {}; std::string group; };   // "obs" | "prop" | "zone"
struct Collision { std::vector<CollisionMesh> meshes; std::vector<CollisionInstance> instances; };
struct Water { std::vector<castlemist::model::Extractor::MapWaterSurface> surfaces;
               castlemist::model::Extractor::MapShore shore; };
struct Environment { castlemist::model::Extractor::MapSky sky; castlemist::model::Extractor::MapEnvLight light; };
struct Motion { std::vector<uint32_t> animatedProps; };   // indices into props
struct WorldScene {
    uint32_t mapFileId = 0;
    float bounds[4] = {0, 0, 0, 0}; bool hasBounds = false;   // parm rect, map space
    Terrain terrain; std::vector<PropModel> models; std::vector<PropInstance> props;
    Collision collision; Water water; Environment environment; Motion motion;
    std::vector<std::string> warnings;
};
```

- [ ] **Step 1: Write the failing test** `world.default_scene_is_empty`: a default `WorldScene` has `!terrain.present`, empty `props`, `models`, `collision.instances`, `warnings`, `!hasBounds`.
- [ ] **Step 2: Run** `cmake --build --preset debug --target cm_test_world` — expected: FAIL (no target / header).
- [ ] **Step 3: Implement** the header, the layer line, the two test targets (`test_world_dat.cpp` holds only the helpers plus `CM_TEST(world_dat, dat_opens)` that calls `shared_dat()`).
- [ ] **Step 4: Run** `cmake --build --preset debug --target cm_test_world cm_test_world_dat && ./build/debug/bin/cm_test_world.exe` — expected: PASS.
- [ ] **Step 5: Commit** `feat(world): world layer and WorldScene types`

---

### Task 2: T3D reference dumper and reference files

**Files:**
- Create: `tools/world/t3d_reference.mjs`, `tools/world/package.json` (`"type": "module"`), `tools/world/README.md`
- Create: `tests/world_ref/192711.json`, `tests/world_ref/191000.json`, `tests/world_ref/1151420.json`

**Interfaces — Produces** reference JSON (all map-space numbers *as T3D computes them, converted back out of three.js Y-up by the inverse of T3D's own `(x,-z,-y)` mapping*):

```json
{ "map": 192711, "t3dCommit": "<sha>",
  "rect": [x0, y0, x1, y1],
  "chunks": [chunksX, chunksY], "segments": 32, "samplesPerChunkStored": 35,
  "heights": [[x, y, z], ...],            // 16x16 grid of positions across rect, z = T3D terrain height there
  "props": [{"fileId": n, "group": "propArray", "pos": [..], "rot": [..], "scale": s, "world": [16 floats]}, ...],   // first 50 OF EACH group, file order within the group (propInstanceArray: base then transforms[])
  "collision": {"instances": n, "sample": [{"geometryIndex": g, "collisionIndex": c, "world": [16]}, ...]},          // first 20
  "water": {"surfaces": n, "z": [..]},
  "terrainMaterials": [{"chunk": i, "textures": [fileIds], "pickerPage": [px, py]}, ...]   // first 16 chunks
}
```

Usage: `node tools/world/t3d_reference.mjs --t3d <path to t3d checkout> --map-bytes <decompressed map file> --file-id <id> --out tests/world_ref/<id>.json`; map bytes come from `gw2dat_cli extract --dat <dat> --file-id <id> --out <tmp>`. The script imports T3D's `parser` package and `definitions` (built with `npm ci && npm run build` in `<t3d>/parser`), parses `parm`, `trn`, `prp2`/props, `havk`, `watr` chunks the way `parser/test/mapc2.test.ts` does, and reproduces T3D's placement math from `TerrainRenderer.ts` / `PropertiesRenderer.ts` / `HavokRenderer.ts` (cite line numbers in comments). Terrain heights at a position: bilinear over T3D's inner `(segments+1)^2` grid of the containing chunk, as T3D's `sampleTerrainHeightChunk` does. Picker page fileIds need the `pimg` file: the script records `pickerPage` coordinates only (fileIds are compared in Task 5 from castlemist's side).

- [ ] **Step 1: Confirm the three fileIds** are map packfiles: `gw2dat_cli extract` each, first 4 bytes `PF` and the chunk list contains `trn` and `parm` (`gw2dat_cli parse --file-id <id>` lists chunks). If one is not, pick the same map from `<t3d>/library/src/MapFileList.ts` with the dat's id and update the spec's table in the same commit.
- [ ] **Step 2: Write the script**; run it on all three maps.
- [ ] **Step 3: Check the output by eye**: `rect` non-degenerate; `chunks[0]*chunks[1]` equals the `trn` chunk count; up to 50 props per non-empty group; heights finite. Note the T3D commit sha in each file.
- [ ] **Step 4: Commit** `tools(world): T3D reference dumper and references for three maps` (references are numbers only — confirm no texture or mesh bytes).

---

### Task 3: The frame — research note §1–2 and conversions

**Files:**
- Create: `docs/research/gw2-world-frame.md` (front matter like `docs/research/gw2-sky.md`), `include/castlemist/world/frame.h`, `src/world/frame.cpp`
- Test: `tests/test_world.cpp`

**Interfaces — Produces:**

```cpp
// Map space = GW2 map space as stored (WorldScene's only frame).
void map_to_unity(const float in[3], float out[3]);       // position / direction
void map_to_blender(const float in[3], float out[3]);
void map_matrix_to_unity(const float in[16], float out[16]);   // column-major
void map_matrix_to_blender(const float in[16], float out[16]);
constexpr float kMapUnitsPerMetre = /* §1 value */;
```

Evidence to gather for §1 (axes, handedness, units, Z sign): `docs/research/gw2-sky.md` §1 (sky space: left-handed, X east, Y north, Z down) — state whether map space is the same and why; T3D's `(x,-z,-y)` mapping into three.js (right-handed, Y-up); T3D terrain negating heights; `MapCollision`'s "Z-up" comment in `gw2model.hpp:1081` — resolve the Z-up/Z-down contradiction from data: on Queensdale, the `havk` `waterSurfaceZ` vs terrain heights near the coast, and a ground prop's Z vs the terrain height at its XY (whichever sign makes props sit on terrain is the convention). Units: cite where "inches" (≈39.37/m) comes from or mark UNPROVEN. §2: `parm.rect`; a map without it → no bounds, warning, no invented rect.

- [ ] **Step 1: Write the note §1–2** with evidence lines and the Queensdale numbers you measured.
- [ ] **Step 2: Write failing tests** `world.frame_to_unity_axes` and `world.frame_to_blender_axes`: map-space east, north and up unit vectors map to the Unity/Blender vectors the note states (exact), and `map_matrix_to_unity(identity)` is a proper rotation (determinant +1 after the handedness change is accounted for as the note states).
- [ ] **Step 3: Run** `./build/debug/bin/cm_test_world.exe` — expected FAIL.
- [ ] **Step 4: Implement** `frame.cpp` exactly per the note.
- [ ] **Step 5: Run** — expected PASS.
- [ ] **Step 6: Commit** `feat(world): map frame and conversions, with research note`

---

### Task 4: Terrain geometry — note §3

**Files:**
- Create: `include/castlemist/world/terrain.h`, `src/world/terrain.cpp`
- Test: `tests/test_world.cpp`, `tests/test_world_dat.cpp`

**Interfaces:**
- Consumes: `Extractor::parseTerrain()` → `MapTerrain{dims, vertsPerChunkSide, heights, rect, hasRect}`.
- Produces:

```cpp
struct TerrainLayout { int chunksX = 0, chunksY = 0, segments = 0, stored = 0; bool ok = false; std::string why; };
TerrainLayout terrain_layout(uint32_t dimX, uint32_t dimY, uint32_t vertsPerChunkSide, size_t heightCount);
Terrain build_terrain(const castlemist::model::Extractor::MapTerrain& t, std::vector<std::string>& warnings);
float terrain_height_at(const Terrain& t, float x, float y, bool* inside = nullptr);
```

Decide in the note (§3), from data, with T3D and castlemist's current code as the two hypotheses: (a) which `stored`-per-side samples are the chunk's own (T3D: drop the outer ring; castlemist: 3-sample overlap from index 0) — test both against Queensdale: the correct one makes adjacent chunks' shared edge heights identical *and* matches T3D's sampled heights; (b) the chunk grid (T3D's `sqrt(dims[0]*count/dims[1])`); (c) chunk placement incl. T3D's odd/even `rect[1] ± cdy/2` offset — prove it or report it UNPROVEN with the maps it affects; (d) height sign (from Task 3).

- [ ] **Step 1: Write failing pure tests:**
  - `world.terrain_layout_from_dims_and_count`: `terrain_layout(128, 192, 32, 24*35*35)` → `chunksX 4, chunksY 6, segments 32, stored 35, ok`.
  - `world.terrain_layout_rejects_odd_sample_count`: `heightCount` not divisible into `(segments+3)^2` per chunk → `!ok`, `why` non-empty.
  - `world.terrain_detile_hand_grid`: a hand-built 2x1-chunk `MapTerrain` (stored 5, segments 2) with distinct values per sample → chunk heights are exactly the samples the note's rule selects, and the shared edge of chunk 0 and chunk 1 is equal.
  - `world.terrain_no_rect_warns`: `hasRect = false` → `terrain.present == false`, a warning containing `"no parm rect"`.
- [ ] **Step 2: Write failing dat tests** for each test map: `chunksX/chunksY` equal the reference; `terrain_height_at` at each of the 256 reference positions within 0.01; every pair of adjacent chunks shares edge heights exactly.
- [ ] **Step 3: Run** both test exes — expected FAIL.
- [ ] **Step 4: Write note §3, then implement** `terrain.cpp` per the note. Credit T3D in a comment where its rule is used.
- [ ] **Step 5: Run** — expected PASS (dat tests SKIP without `GW2_TEST_DAT`).
- [ ] **Step 6: Commit** `feat(world): terrain chunk layout and heights, proven against T3D`

---

### Task 5: Terrain materials — note §4

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp` (new reader next to `parseTerrain`, line ~1491)
- Modify: `src/world/terrain.cpp`; Test: `tests/test_world.cpp`, `tests/test_world_dat.cpp`

**Interfaces — Produces:**

```cpp
// in Extractor:
struct MapTerrainMaterials {
    bool present = false;
    std::vector<uint32_t> texFileIds;                       // materials.texFileArray[].filename
    struct Chunk { uint32_t materialFileId = 0; std::vector<uint32_t> texIndices; };  // lo-res material
    std::vector<Chunk> chunks;                              // per terrain chunk, chunk order
    uint32_t pimgFileId = 0;                                // the trn's picker image file, if referenced
};
MapTerrainMaterials parseTerrainMaterials();
// in world (terrain.h):
void resolve_terrain_materials(Terrain& t, const castlemist::model::Extractor::MapTerrainMaterials& m,
                               Gw2Dat& dat, const nlohmann::json& tpl, std::vector<std::string>& warnings);
```

Note §4 records: which `trn` fields hold materials (lo- vs hi-res: T3D uses `loResMaterial`; say which the game's hi-detail path uses or mark UNPROVEN and store lo-res), what the second half of `texIndexArray` is, where the `pimg` file is referenced and how `pgtb.strippedPages` (`layer`, `coord`, filename) map to chunks (4x4 per page; V offset `0.75-(cy%4)/4`), and the UV scale (T3D's 8 is a TODO constant — find the real field or mark UNPROVEN; store `0` meaning "unknown" rather than 8).

- [ ] **Step 1: Failing pure test** `world.terrain_material_index_out_of_range`: a chunk whose `texIndices` includes `texFileIds.size()` → `material.resolved == false`, a warning naming the chunk.
- [ ] **Step 2: Failing dat tests** per map: every chunk `material.resolved`; the first 16 chunks' `textureFileIds` equal the reference's `terrainMaterials[].textures`; every chunk has `pickerFileId != 0` and its page equals the reference `pickerPage`.
- [ ] **Step 3: Run** — expected FAIL.
- [ ] **Step 4: Write note §4; implement** the reader (template field names only) and `resolve_terrain_materials` (the `pimg` packfile is read with the same Extractor pattern).
- [ ] **Step 5: Run** — expected PASS.
- [ ] **Step 6: Commit** `feat(world): terrain materials and picker pages`

---

### Task 6: Props — note §5

**Files:**
- Create: `include/castlemist/world/props.h`, `src/world/props.cpp`; Test: both test files

**Interfaces:**
- Consumes: `Extractor::parseMapProps()`; extend `MapProp` with `std::string group` set by the array it came from (`gw2model.hpp:1472`, set in the loop at line ~1851 and the `propInstanceArray` branch).
- Produces: `void build_props(const std::vector<castlemist::model::Extractor::MapProp>& in, WorldScene& out);` — dedupes `models` by fileId, fills `props[].world` by the proven client transform (port `sceneWorld`'s math into `props.cpp` as column-major; cite `math.h:160` — do not depend on `render`), fills `motion.animatedProps` with indices of `group == "propAnimArray"`.

Note §5 copies the transform proof summary from `math.h:160-185` and adds the T3D agreement measured here.

- [ ] **Step 1: Failing pure tests:** `world.prop_transform_matches_client` (for `pos {10,20,30}`, `rot {0.3,-0.2,1.1}`, `scale 2`, `world` equals the client matrix written out from the formula in `math.h:168-172`, within 1e-6); `world.props_dedupe_models` (three props, two fileIds → 2 models, instance model indices correct); `world.anim_props_recorded`.
- [ ] **Step 2: Failing dat tests:** per group, the first 50 props of that group equal the reference's (fileId, pos within 0.01, world within 1e-4 after mapping T3D back) — compare per group because castlemist and T3D concatenate the groups in different orders; ≥99% of prop XY positions inside the terrain's union of chunk rects.
- [ ] **Step 3: Run** — FAIL. **Step 4: Implement.** **Step 5: Run** — PASS.
- [ ] **Step 6: Commit** `feat(world): prop models and instances in the map frame`

---

### Task 7: Collision placements — note §7

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp` (new reader beside `parseMapCollision`, line ~1092; leave the old one for the old path)
- Create: `include/castlemist/world/collision.h`, `src/world/collision.cpp`; Test: both test files

**Interfaces — Produces:**

```cpp
// in Extractor:
struct HavokPlacement { uint32_t geometryIndex = 0; float translate[3] = {}, rotate[3] = {}; float scale = 1; std::string group; };
struct MapHavok {
    bool present = false;
    std::vector<MapCollision> hulls;                       // collisions[i], local, one per entry (verts/indices)
    std::vector<std::vector<uint32_t>> geometryAnimations; // geometries[i].animations[]
    std::vector<std::vector<uint32_t>> animationCollisions;// animations[i].collisionIndices[]
    std::vector<HavokPlacement> placements;                // obsModels, propModels, zoneModels in that order
};
MapHavok parseHavok();
// in world:
void build_collision(const castlemist::model::Extractor::MapHavok& h, WorldScene& out);
```

Rule to prove in note §7 (T3D's, `HavokRenderer.ts:188-260`): placement → `geometries[geometryIndex].animations[last]` → `collisionIndices[]` → hulls; transform pos/rot as props; scale `32 * scale` with obs `scale = 1`. Prove the ×32 (hull units vs map units) from data: placed hull bounds must match the visual prop's bounds for the same placement on Queensdale — or mark UNPROVEN. Why `animations[last]` (T3D comments "for now") — record what the other entries are or mark UNPROVEN.

- [ ] **Step 1: Failing pure test** `world.collision_bad_indices_skipped`: a `MapHavok` with one placement whose `geometryIndex` is out of range and one whose collision index is out of range → no instances, exactly one warning mentioning `"2 collision placements"`.
- [ ] **Step 2: Failing dat tests:** instance count and the first 20 instance matrices equal the reference; ≥99% of instance origins inside the terrain rect union (the old blob-at-origin fails this).
- [ ] **Step 3: Run** — FAIL. **Step 4: Write note §7; implement.** **Step 5: Run** — PASS.
- [ ] **Step 6: Commit** `feat(world): Havok collision placed by obs/prop/zone models`

---

### Task 8: Water, environment — note §6

**Files:**
- Modify: `src/world/load_world.cpp` (created here with the attach helpers), `include/castlemist/world/load_world.h`; Test: both test files

**Interfaces — Produces:** `void attach_water(castlemist::model::Extractor& ex, WorldScene& out);` and `void attach_environment(castlemist::model::Extractor& ex, WorldScene& out);` (`parseWater`, `parseShore`, `parseMapSky`, `parseMapEnv()`). A V0 `watr` (no `waterSurfaces` field) → warning `"watr V0 not read"`, not silence. The old flood-fill/flat-plane water is not used.

Note §6: where surfaces live, the meaning of `waterSurfaceFlags` if known, the relation between `havk.waterSurfaceZ` and the surfaces.

- [ ] **Step 1: Failing dat tests:** surface count and Z values equal the reference water block on all three maps; `environment.sky.present` on all three.
- [ ] **Step 2: Run** — FAIL. **Step 3: Write note §6; implement.** **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `feat(world): water surfaces and environment attached`

---

### Task 9: `load_world`, unread-chunk warnings, CLI, docs

**Files:**
- Modify: `src/world/load_world.cpp`, `include/castlemist/world/load_world.h`, `tools/gw2dat_cli/main.cpp` (`cmd_world`, dispatch next to `cmd_map` ~line 2382, add `world` to the usage string), `tools/gw2dat_cli/CMakeLists.txt` or the root target if `gw2dat_cli` doesn't link `castlemist::world`, `docs/using-castlemist.md` (CLI example next to the `map` one)
- Test: `tests/test_world_dat.cpp`

**Interfaces — Produces:**

```cpp
WorldScene load_world(Gw2Dat& dat, uint32_t mapFileId, const nlohmann::json& tpl);   // throws only on dat I/O / not-a-map
nlohmann::json world_summary(const WorldScene& w);   // {map, bounds, terrain:{chunks:[x,y], resolvedMaterials}, models, props, animatedProps, collision:{meshes, instances}, water:{surfaces}, sky, warnings}
```

`load_world` runs each section (terrain, materials, props, collision, water, environment) in its own `try`, so one failure leaves its part empty with a named warning. After parsing, it lists every chunk in the packfile not consumed by a section as `"chunk <fourcc> v<ver> not read"`. A file without `trn`, `parm`, props and `havk` chunks all missing is "not a map" → throw `std::runtime_error("file <id> is not a map packfile")`.

CLI: `gw2dat_cli world --dat <dat> --file-id <map> --template <json> [--out summary.json]` prints `world_summary` (with `"ok": true`); errors go through `fail()`.

- [ ] **Step 1: Failing dat tests:** `world_dat.load_world_three_maps` (each loads; terrain present; `props` non-empty; every material resolved; no warning starts with `"exception"`); `world_dat.not_a_map_throws` (a model fileId, e.g. the first `MODL` fileId used in `tests/test_extract_dat.cpp`, throws).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement** `load_world`, `world_summary`, `cmd_world`, docs.
- [ ] **Step 4: Verify the CLI** on all three maps:

```bash
D="E:/Games/gw2/Guild Wars 2/Gw2.dat"; T=dumps/packfile/gw2_packfile.json
for m in 192711 191000 1151420; do ./build/debug/bin/gw2dat_cli.exe world --dat "$D" --file-id $m --template $T; done
./build/debug/bin/gw2dat_cli.exe world --dat "$D" --file-id <model fileId> --template $T   # expect non-zero exit, "is not a map packfile"
```

Expected: three `"ok": true` summaries; every unread chunk listed in `warnings`.
- [ ] **Step 5: Run the full suite** `ctest --preset debug` with `GW2_TEST_DAT` set — expected: no failures.
- [ ] **Step 6: Self-check the note**: every Formula/Table cites evidence; every UNPROVEN says what would prove it.
- [ ] **Step 7: Commit** `feat(world): load_world, unread-chunk warnings and gw2dat_cli world`

---

## Note on the spec's `motion` part

The spec lists UV-scroll values and placed-effect references under `motion`. Neither is a map-chunk field this plan reads: UV scroll lives in each prop model's materials (read when models load, sub-project 3), and placed effects are owned by the effects project. `Motion` v1 therefore records animated props only; the other two arrive with sub-project 3. This is a deliberate narrowing, flagged for the user at plan review.
