# World Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export a GW2 map from one `WorldScene` as a folder that Unity/VRChat and Blender both build a scene from:
- a versioned `manifest.json` with a committed JSON Schema;
- one glTF per cell: welded terrain with a baked albedo, water, and collider parts;
- one VRChat model folder per prop model, placed as instances;
- sky, sun, a spawn hint and a frame probe.

Two importers ship with it, a Unity editor script and a Blender script. The old `MapScene` / `map_fly` path is then removed.

**Architecture:**
- `world` gains the pure parts: cell partition and bucketing, the terrain albedo and water bake, and the collision-triangle and surface rules. The surface rules move down from `render` so the export can share them.
- `exportgltf` gains the probe, cell glTF writer, manifest writer and folder orchestration, plus a `ModelSource` with a dat implementation.
- `ripper` runs Blender for the FBX step.
- `tools/` holds the schema, the dependency-free validator, the Blender script and the Unity importer.
- The CLI and **File ▸ Export World (Map)…** are the front ends.

**Tech Stack:**
- C++20, MinGW-w64 GCC, CMake + Ninja presets, Win32.
- The repo's test framework (`tests/framework/test_framework.h`: `CM_TEST`, `CHECK*`, `SKIP`).
- nlohmann::json and stb (vendored).
- Python 3.12 (standard library only).
- Blender 4.5 (headless).
- C# 9 for Unity 2022.3.

**Spec:** docs/superpowers/specs/2026-10-15-world-export-design.md

## Global Constraints

**Build and test**
- Build: `export PATH="/c/msys64/ucrt64/bin:$PATH"; cmake --build --preset debug`.
- Tests: `build/debug/bin/cm_test_<suite>.exe <filter>`, where the filter is a `group.name` prefix, and `ctest --preset debug`.
- Paths are relative to the worktree root `C:/Users/vital/Downloads/castlemist/castlemist/.claude/worktrees/world-export-cells`.

**The dat**
- Never read Gw2.dat directly. Only castlemist code opens it.
- The dat is `E:/Games/gw2/Guild Wars 2/Gw2.dat`: `GW2_TEST_DAT` for tests, `GW2_DAT` for the app.
- The template is `C:/Users/vital/Downloads/castlemist/castlemist/dumps/packfile/gw2_packfile.json` (`GW2_TEST_TEMPLATE` for tests).
- Dat tests `SKIP` when the dat is absent.

**Frame and layers**
- Map space only: left-handed, +X east, +Y north, up = −Z. "Above" means smaller Z, and a ray straight down has direction (0, 0, +1).
- Files keep map units.
- Conversions are only `world/frame.h` and `exportgltf::map_to_gltf_root`.
- Layer rules: `world` depends on nothing above `extract`. Nothing below `render` mentions Direct3D. `exportgltf` must not depend on `render` or `ripper`, because `render` already depends on `exportgltf`, so that edge would be a cycle.

**UNPROVEN values**
- Every UNPROVEN value is a labelled setting carried in the manifest's `settings` (value, status, source) and in `warnings`. This is the spec's D3 table, plus `unityScale = 0.0254` (UNPROVEN, `gltf_export.h:57`).
- No metres anywhere in code.

**Pinned values from the spec**

| Setting | Value |
|---|---|
| `texelsPerChunk` | 256 (allowed: 64 / 128 / 256 / 512) |
| `waterTexelsPerChunk` | 32 |
| `maxTextureSize` | 512 |
| `cellMaxChunks` | 4 |
| `cellMaxTriangles` | 1 000 000 |
| `cellMaxTextureMB` | 256 |
| Collider part | at most 65 535 vertices |
| Water alpha | 0.45 |
| Grey fallback | sRGB byte 128 |
| Spirit Vale | 250 chunks; 106 resolved animated placements |

Unity pan = `20 · scroll`. The cell id is `c<cx0>_<cy0>_<w>x<h>`, and the no-terrain cell is `c_all`.

**Tools**
- **T3D is GPL-3; castlemist is MIT.** Re-derive behaviour and never copy code.
- **Blender.** Use `C:/Program Files/Blender Foundation/Blender 4.5/blender.exe`. That is what `ripper::find_blender` (src/ripper/vrchat.cpp:293) picks, since "Blender 4.5" sorts highest. `tests/blender/check_fbx.py` already handles 4.4+ slotted actions. Blender ctests find it with `find_program(CASTLEMIST_BLENDER blender.exe PATHS "C:/Program Files/Blender Foundation/Blender 4.5" "C:/Program Files/Blender Foundation/Blender 4.3" "C:/Program Files/Blender Foundation/Blender 3.6" NO_DEFAULT_PATH)` and are not added when it is not found.
- **Python.** `jsonschema` is not installed. **Decision:** `tools/world/validate_manifest.py` is dependency-free.
  - It carries its own minimal checker for exactly the keywords the schema uses.
  - It errors on any other keyword, so the schema cannot silently outgrow it.
  - When `import jsonschema` succeeds, it also runs that as a cross-check.
  - Python ctests use `find_package(Python3 COMPONENTS Interpreter)` and are not added without it.
- **Unity cannot run in CI.** The importer is verified three ways:
  1. the validator's `--plan` dry run, whose `PLAN` line the importer prints in the same format;
  2. an optional local batchmode run with `C:/Program Files/Unity 6000.0.42f1/Editor/Unity.exe`. That is Unity 6, while VRChat uses 2022.3.22f1 (`poiyomi-10-importer.md` §5), so a pass there is not proof for 2022.3;
  3. the manual checklist `tools/unity/CHECKLIST.md`.

**Style and commits**
- Doc comments follow the repo style (`/// @file`, `/// @brief`) and cite spec and note sections.
- Every commit message ends with a blank line and then `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Use `git commit -m "<subject>" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"`.

## Review Focus

- **The frame chain glb → Blender → FBX → Unity.** The model files carry a reflected root (det −1). Imported, the probe's bars must point east, north and up in each app with lengths in the ratio 3 : 2 : 1, with no negative-scale object left behind.
  → Task 1: `world_probe.bars_in_map_space` and the ctests `blender_probe_glb`, `blender_probe_fbx`.
- **Cell partition edges.**
  - The cells tile the region exactly and never cross a page.
  - A translation on a shared chunk edge lands in exactly one cell, and an outside point is clamped.
  - Edge blocks are smaller, and a 1 × 1 cell over budget stays and warns.
  → Task 2: `cells.tiles_region_exactly`, `cells.shared_edge_one_cell`, `cells.over_budget_1x1_warns`.
- **Instance transforms into Unity.** `unity.t/r/s` must reproduce `map_matrix_to_unity(m)` relative to the cell origin. A placement that is non-uniform, has a non-positive determinant or is not finite is skipped and counted. It is never written as a broken quaternion.
  → Task 7: `world_manifest.unity_trs_roundtrip`, `world_manifest.unity_trs_rejects_bad`.
- **The parallel model stage.**
  - Workers write model folders concurrently against one shared, read-only dat.
  - A failed model is counted and its placements skipped.
  - Output is independent of the thread count.
  - A fatal error leaves no `manifest.json`.
  → Task 8: `world_folder.parallel_matches_serial`, `world_folder.failed_model_counted_and_skipped`, `world_folder.manifest_written_last`.
- **Index ranges.** A 4 × 4 cell's welded terrain has exactly 16 641 vertices and fits 16-bit indices. Collider parts never exceed 65 535 vertices, and the parts add up to every triangle.
  → Task 6: `world_cell.terrain_welded_vertex_count`, `world_cell.collider_parts_under_65535`.

---

### Task 1: Frame probe and the FBX axis settings `[review]`

**Files:**
- Create:
  - `include/castlemist/exportgltf/world_probe.h`
  - `src/exportgltf/world_probe.cpp` (added to the exportgltf sources in `CMakeLists.txt` near :145)
  - `tools/blender/castlemist_world.py` (`--check-probe` and a first `--to-fbx`)
  - `tools/unity/Castlemist/Editor/Castlemist.Editor.asmdef`
  - `tools/unity/Castlemist/Editor/FrameProbe.cs`
  - `tools/unity/Castlemist/Editor/BatchEntry.cs`
  - `tests/tools/world_export_fixture.cpp` (target `cm_world_export_fixture`; writes `probe.glb` into `argv[1]`)
- Test: `tests/test_exportgltf_world.cpp` (new; added to the `exportgltf` SOURCES at `tests/CMakeLists.txt:114`), plus ctests in `tests/CMakeLists.txt`

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
inline constexpr float kProbeLengths[3] = {300.0f, 200.0f, 100.0f};  // east, north, up bars, map units
inline constexpr float kProbeThickness = 20.0f;
/// Three boxes from the origin under map_to_gltf_root(1): "Probe_East" x in [0,300], "Probe_North"
/// y in [0,200], "Probe_Up" z in [-100,0] (up = -Z); each other axis in [-10,10].
bool write_frame_probe(const std::string& glbPathUtf8, std::string* error = nullptr);
/// {"unity":{"east":[1,0,0],"north":[0,0,1],"up":[0,1,0]},
///  "blender":{"east":[1,0,0],"north":[0,1,0],"up":[0,0,1]},"lengths":[3,2,1]}
nlohmann::json frame_probe_expect();
}
```
Check rule, the same in Python and C#:
1. Take each bar's world-space bounds centre minus the probe root's world position, and normalise it.
2. A bar passes when its dot product with the expected direction is > 0.99.
3. The extents along the three directions must be in the ratio 3 : 2 : 1 within 1 %.

`castlemist_world.py --check-probe <file.glb|file.fbx>` imports the file into an empty scene. It prints `PROBE east=(x,y,z) north=(...) up=(...) negScale=<0|1> OK` or `... FAIL <axis>`. `negScale` is 1 when any object's world matrix has det < 0.

`BatchEntry.CheckProbe` is run as `-executeMethod Castlemist.BatchEntry.CheckProbe -castlemistFile <probe.fbx>`. It imports the file into a scratch scene, prints the same `PROBE` line through `Debug.Log` and exits with code 0 or 1.

- [ ] **Step 1: Write failing tests**
  - `world_probe.bars_in_map_space`: `write_frame_probe(tmp/"probe.glb")`. Then `glb_json` (copy the helper at `tests/test_exportgltf_vrchat.cpp:1248`) gives:
    - a scene root `GW2_MapToGltf` whose `matrix` equals `map_to_gltf_root(1)` element-wise;
    - the `Probe_East` POSITION accessor `min[0] == 0`, `max[0] == 300`, `min[1] == -10`, `max[1] == 10`;
    - `Probe_North` `max[1] == 200`;
    - `Probe_Up` `min[2] == -100`, `max[2] == 0`.
  - `world_probe.expect_matches_frame`: for each bar direction d in map space ((1,0,0), (0,1,0), (0,0,−1)), `world::map_to_unity(d)` equals `frame_probe_expect()["unity"]` and `map_to_blender(d)` equals `["blender"]`.
  - ctest `blender_probe_glb`, with FIXTURES_REQUIRED `world_probe`. The fixture runs `cm_world_export_fixture ${CMAKE_BINARY_DIR}/world_fixture --probe-only`, and the test runs `${CASTLEMIST_BLENDER} -b --factory-startup -P tools/blender/castlemist_world.py -- --check-probe <fixture>/probe.glb` with `PASS_REGULAR_EXPRESSION "PROBE .* negScale=0 OK"`.
  - ctest `blender_probe_fbx`: `--to-fbx <fixture>` and then `--check-probe <fixture>/probe.fbx`, with the same regex.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_probe.` and `ctest --preset debug -R blender_probe`. Both FAIL: the code does not compile, and the script does not exist yet.
- [ ] **Step 3: Implement**
  - The probe writer uses `GltfWriter` (`src/exportgltf/internal.h:80`) with one unlit material per bar.
  - `castlemist_world.py --to-fbx <dir>` imports each `.glb` it finds with `bpy.ops.import_scene.gltf`. A mesh-only file gets `transform_apply(location=True, rotation=True, scale=True)` on every object, so the reflected root ends up in the vertex data. This is the shared-data copy step of `castlemist_vrchat.py:41-49`, which also flips winding. A file with an armature keeps its transforms, as `castlemist_vrchat.py` does. The FBX export uses the settings of `castlemist_vrchat.py:181-195` (`axis_forward='-Z'`, `axis_up='Y'`, `apply_scale_options='FBX_SCALE_ALL'`), with `path_mode='RELATIVE'` and `embed_textures=False`.
  - The C# files: an editor-only asmdef `Castlemist.Editor`, and `FrameProbe.Check(GameObject probeRoot, out string line)`, which compares against the fixed expected `Vector3`s of `frame_probe_expect()`.
- [ ] **Step 4: Run** the two commands again → PASS.

  **Optional local Unity run.** Create a scratch project in the scratchpad, copy `tools/unity/Castlemist` into its `Assets/`, copy `probe.fbx` in, and run `"C:/Program Files/Unity 6000.0.42f1/Editor/Unity.exe" -batchmode -nographics -quit -projectPath <scratch> -executeMethod Castlemist.BatchEntry.CheckProbe -castlemistFile Assets/probe.fbx -logFile -`.

  **Deciding rule:**
  - If the line says OK, keep the settings.
  - If east comes out as −X, or the run shows any FAIL, try `axis_forward='Z', axis_up='Y'`, then `ModelImporter.bakeAxisConversion = true` in the importer. Keep the first combination that passes.
  - If Unity cannot run here (licence or editor problem), keep the `castlemist_vrchat.py` settings and record "Unity probe not run". The importer's own probe check (Task 12) then decides at manual-check time.
  - Record the outcome for Task 14's amendments, with the date and the Unity version.
- [ ] **Step 5: Commit** `feat(world-export): frame probe, Blender probe check and FBX conversion settings`.

### Task 2: Cell partition and bucketing `[light]`

**Files:**
- Create: `include/castlemist/world/cells.h`, `src/world/cells.cpp`
- Test: `tests/test_world_cells.cpp` (added to the `world` SOURCES at `tests/CMakeLists.txt:97`)

**Interfaces:**
```cpp
namespace castlemist::world {
struct CellBudget { int maxChunks = 4; uint64_t maxTriangles = 1'000'000; double maxTextureMB = 256.0; };  // castlemist's choice
struct CellStats {
    std::vector<uint64_t> modelTriangles;               // parallel to scene.models (LOD0)
    std::vector<std::vector<uint32_t>> modelTextures;   // distinct texture fileIds per model
    std::unordered_map<uint32_t, uint64_t> textureBytes;// RGBA8 + mips at the cap: w*h*4*4/3
    uint64_t bakedBytesPerChunk = 0;                    // albedo + water textures per chunk
};
struct ChunkRegion { int cx0 = 0, cy0 = 0, cx1 = -1, cy1 = -1; bool whole() const { return cx1 < cx0; } };  // inclusive
struct Cell {
    std::string id;                  // c<cx0>_<cy0>_<w>x<h> | c_all
    int cx0 = 0, cy0 = 0, w = 0, h = 0; bool all = false;
    float rect[4] = {0, 0, 0, 0};    // map space x0,y0,x1,y1
    float origin[3] = {0, 0, 0};     // rect centre, z 0
    std::vector<uint32_t> props, collision;   // indices into scene.props / scene.collision.instances
    uint64_t triangles = 0; double textureMB = 0; bool split = false, overBudget = false;
};
struct CellPlan { std::vector<Cell> cells; int blockChunks = 4; size_t propsOutsideRegion = 0; std::vector<std::string> warnings; };
/// 1/pickerScale when every resolved chunk agrees on it and it is an integer in [1, maxChunks]; else maxChunks (warned by partition_cells).
int cell_block_chunks(const Terrain& t, int maxChunks);
/// Chunk (cx, cy) containing (x, y): cx = floor((x - x0)/cdx), cy = floor((y1 - y)/cdy), each clamped to the grid.
void chunk_of_point(const Terrain& t, float x, float y, int& cx, int& cy);
CellPlan partition_cells(const WorldScene& s, const CellStats& st, const CellBudget& b, const ChunkRegion& r = {});
}
```
**Algorithm** (the signature does not fix it):
1. Blocks are `blockChunks` squares tiled from chunk (0, 0), clipped at the grid edge, visited in row-major (cy, cx) order.
2. Cost of a rectangle: `w·h·segments²·2` terrain triangles + Σ `modelTriangles[props[k].model]`. Texture: Σ over the distinct fileIds of its instances' models of `textureBytes`, plus `bakedBytesPerChunk·w·h`, in MB = bytes / 2²⁰.
3. A rectangle over either budget splits into quadrants with `wHalf = (w+1)/2` and `hHalf = (h+1)/2`; empty quadrants are dropped. Recurse. A 1 × 1 cell never splits: it gets `overBudget = true` and the warning `cells: c<..> over budget (<tris> triangles, <MB> MB) at 1x1`.
4. Bucket each prop and collision instance by `chunk_of_point` of its translation (`world[12]`, `world[13]`), then by cell.
5. A cell outside the region is dropped, and its instances are counted in `propsOutsideRegion`.
6. Cells are sorted by (cy0, cx0).
7. No terrain: one cell `c_all` with `all = true`. Its rect is the prop extent, or `bounds` when `hasBounds`. Warning: `cells: no terrain; one cell c_all holds every placement`.

- [ ] **Step 1: Write failing tests.** A helper `grid(cx, cy)` builds chunks with `samples = 33`, `rect` from x0 = −15360, y1 = 39936 at 3072 per chunk (Spirit Vale's rect, frame §2) and `pickerScale = 0.25`.
  - `cells.block_size_from_pages`: `cell_block_chunks(grid(10,25), 4) == 4`. With `pickerScale = 0.5`, it is 2. With `pickerScale = 0`, it is 4.
  - `cells.edge_blocks`: `grid(10,25)`, no props. `cells.size() == 21`. There are ids `c8_0_2x4`, `c0_24_4x1` and `c8_24_2x1`, and `cells[0].id == "c0_0_4x4"`.
  - `cells.tiles_region_exactly`: Σ w·h == 250, and every chunk is covered exactly once (a 10 × 25 bool grid). No cell crosses a 4-chunk page line: `cx0/4 == (cx0+w-1)/4`, and the same for y.
  - `cells.heavy_block_splits_to_quadrants`: `modelTriangles = {600000}`. Props in chunks (0, 0) and (3, 3) of `grid(4,4)` give 4·4·32·32·2 + 1 200 000 > 1 000 000. The cells are `c0_0_2x2`, `c2_0_2x2`, `c0_2_2x2`, `c2_2_2x2`, all with `split == true`. `c0_0_2x2.triangles == 4*2048 + 600000`.
  - `cells.over_budget_1x1_warns`: three 600 000-triangle props in chunk (0, 0) give `c0_0_1x1` with `overBudget == true`, and one warning containing `"c0_0_1x1"`.
  - `cells.shared_edge_one_cell`: a prop at x = −15360 + 4·3072 = −3072 exactly, y inside row 0, lands in `c4_0_4x4` only (total appearances 1). A prop at x = −20000 lands in a cell with `cx0 == 0`.
  - `cells.texture_budget_counts_distinct`: two props of one model sharing fileId 7 (`textureBytes[7] = 2^20`) give `textureMB == 1.0` plus the baked term.
  - `cells.region_filters`: region {4, 8, 7, 11} on `grid(10,25)` gives one cell `c4_8_4x4`, and `propsOutsideRegion` counts the rest.
  - `cells.no_terrain_one_cell`: `c_all` with `all == true`, holding every prop, and the warning text above.
- [ ] **Step 2: Run** `cm_test_world.exe cells.` → FAIL (does not compile).
- [ ] **Step 3: Implement** as above.
- [ ] **Step 4: Run** `cm_test_world.exe cells.` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(world): cell partition on page-aligned chunk blocks, split by a triangle and texture budget`.

### Task 3: Shared pure rules move into `world` `[light]`

**Files:**
- Create: `include/castlemist/world/surface_rules.h`, `src/world/surface_rules.cpp`
- Modify:
  - `include/castlemist/world/collision.h` and `src/world/collision.cpp`
  - `src/world/walk.cpp` (`CollisionIndex::build` :60-79 calls the new function)
  - `include/castlemist/world/terrain.h` and `src/world/terrain.cpp`
  - `src/render/world_materials.h` (:48, :82, :129, :154): drop the four declarations and add `using castlemist::world::dark_coverage;` and the other three, so render call sites are unchanged
  - `src/render/world_materials.cpp`: move the bodies out (`water_depth_mix` :285 and the other three)
  - `src/render/world_prep.cpp` (`world_texture_ids` calls `terrain_texture_ids` and then appends the water bump)
- Test: `tests/test_world_materials.cpp` (append)

**Interfaces:**
```cpp
namespace castlemist::world {
/// Appends 9 floats per valid triangle of collision instance i, transformed by instance.world (map space);
/// out-of-range indices skipped (walk.cpp:66-67's rule).
void collision_instance_triangles(const WorldScene& s, size_t instance, std::vector<float>& out);
/// Distinct non-zero fileIds, first-seen, per chunk in index order: textureFileIds[0..3], normalFileIds[0..3],
/// pickerFileId, picker2FileId. `chunks` (indices) limits it to those chunks; null = all.
std::vector<uint32_t> terrain_texture_ids(const Terrain& t, const std::vector<size_t>* chunks = nullptr);
// surface_rules.h -- bodies moved verbatim from src/render/world_materials.cpp
bool dark_coverage(const std::vector<uint8_t>& rgba);
bool tex_transform_animates(const ModelTexTransformCPU& t);
void water_colour(const uint8_t stored[4], float rgb[3]);
float water_depth_mix(float terrainZ, float planeZ, float depthAttenuation);
}
```
- [ ] **Step 1: Write failing tests**
  - `surface.collision_triangles_match_index`: one hull `{0,0,0, 1,0,0, 0,1,0}` and two instances (identity, and a translate by (5, 0, 0)), concatenated with `collision_instance_triangles`. `CollisionIndex::from_triangles(tris, {})` and `CollisionIndex::build(s, false)` must agree: equal `triangle_count()`, and the downward rays from (0.2, 0.2, −10) and (5.2, 0.2, −10), direction (0, 0, 1), give equal `t`.
  - `surface.bad_index_skipped`: indices `{0, 1, 9}` append nothing.
  - `surface.terrain_texture_ids_order`: chunk 0 has textures {11, 12, 0, 0}, normals {21, 0, 0, 0}, picker 31, picker2 32. Chunk 1 has textures {11, 13, 0, 0}. The result is `{11, 12, 21, 31, 32, 13}`, and with `chunks = {1}` it is `{11, 13}`.
  - The existing render tests `world_materials.water_colour_stored_order`, `world_materials.water_depth_mix`, `world_materials.dark_coverage_is_black_where_alpha_passes`, `world_materials.only_scroll_only_entries_animate` and `world_materials.texture_ids_cover_terrain_and_water` must keep passing unchanged.
- [ ] **Step 2: Run** `cm_test_world.exe surface.` → FAIL.
- [ ] **Step 3: Implement.** This is a pure move plus the factoring. `CollisionIndex::build` loops `collision_instance_triangles` into `tris_`.
- [ ] **Step 4: Run** `cm_test_world.exe` and `cm_test_render.exe world_materials.` → PASS.
- [ ] **Step 5: Commit** `refactor(world): collision triangles, terrain texture ids and surface rules shared by render and export`.

### Task 4: Terrain albedo and water bake `[light]`

**Files:**
- Create: `include/castlemist/world/terrain_bake.h`, `src/world/terrain_bake.cpp`
- Test: `tests/test_world_bake.cpp` (`world` suite)

**Interfaces:**
```cpp
namespace castlemist::world {
using TextureLookup = std::function<const CappedTexture*(uint32_t fileId)>;   // null = not available
struct BakeSettings { int texelsPerChunk = 256; float groundRepeats = kGroundRepeatsPerChunk; bool srgbColour = true; };
/// clamp(lround(log2(repeats * groundWidth / texelsPerChunk)), 0, mipCount - 1).
int ground_mip_level(int groundWidth, float repeats, int texelsPerChunk, int mipCount);
/// Bilinear on mip `mip` at (u, v) (texel centres at (i+0.5)/w); wrap or clamp; sRGB -> linear on RGB when `srgb`.
BlendTexel sample_bilinear(const CappedTexture& t, int mip, float u, float v, bool wrap, bool srgb);
float linear_to_srgb(float c);   // 12.92 / 1.055 pow(1/2.4) - 0.055, inverse of render's srgb_to_linear
/// T*T*3 bytes RGB sRGB, row 0 north. Texel (i,j): s=(i+0.5)/T, t=(j+0.5)/T; picker/modx clamp, not sRGB (data);
/// ground wrap, sRGB per settings, mip ground_mip_level; terrain_blend with present[k] = fileId != 0 && lookup non-null.
/// Solid page: picker = pickerSolid bytes / 255 (stored order, UNPROVEN). Returns false and writes byte 128
/// everywhere when no colour slot is available.
bool bake_chunk_albedo(const TerrainChunk& c, const TextureLookup& tex, const BakeSettings& s, std::vector<uint8_t>& rgbOut);
/// (cw*T) x (ch*T) RGBA of a cell rect: lerp(shallow, deep, water_depth_mix(terrainZ, planeZ, depthAttenuation)),
/// colours via water_colour (sRGB decode/encode when srgbColour), alpha byte lround(0.45*255) = 115.
/// Returns false (nothing written) when no texel's terrain is below the plane (z > planeZ).
bool bake_cell_water(const WorldScene& s, const float rect[4], int texelsW, int texelsH, bool srgbColour, std::vector<uint8_t>& rgbaOut);
}
```
- [ ] **Step 1: Write failing tests.** Each test uses synthetic 8 × 8 `CappedTexture`s with two mips (8 and 4), since `decode_texture_capped` stops at 4 × 4.
  - `bake.mip_level`: `(512, 8, 256, 8) == 4`, `(512, 8, 512, 8) == 3`, `(4, 1, 256, 3) == 0`, `(4096, 8, 64, 3) == 2` (the clamp).
  - `bake.texel_equals_blend`: with T = 4 and repeats 1, every output byte is within 1 of `lround(linear_to_srgb(albedo)·255)`. Here albedo is `terrain_blend` of `sample_bilinear` at that texel's `page_uv` / `ground_uv`, as the header defines.
  - `bake.solid_page_constant`: `pickerFileId = 0` and `pickerSolid = {255, 0, 0, 0}` put all the weight on `colord`. Every texel equals the bake of `colord` alone.
  - `bake.absent_slot_zero_weight`: slot 2's fileId is 0 and the picker is pure G. The `color` fallback applies, as `terrain_blend.h:24-28` describes.
  - `bake.no_colour_bakes_grey`: the result is `false` and every byte is 128.
  - `bake.water_depth`: plane 0, `depthAttenuation = 10`, flat terrain z = 5, shallow {0, 0, 255}, deep {255, 0, 0}, `srgbColour = false`. Every texel is (128 ± 1, 0, 128 ± 1, 115).
  - `bake.water_above_plane_none`: flat z = −5 returns false.
- [ ] **Step 2: Run** `cm_test_world.exe bake.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(world): CPU terrain albedo bake on terrain_blend and per-cell water colour`.

### Task 5: glTF options for world model files `[review]`

**Files:**
- Modify:
  - `include/castlemist/exportgltf/gltf_export.h` (`GltfExportOptions` :40-57)
  - `src/exportgltf/model_export.cpp` (root at :65-75)
  - `src/exportgltf/internal.h` (`GltfWriter` :80, `write_model_textures` :138)
  - `src/exportgltf/gltf_writer.cpp` (:74)
  - `src/exportgltf/texture_export.cpp`
  - `include/castlemist/exportgltf/vrchat_export.h` and `src/exportgltf/vrchat_export.cpp` (:144, :302)
- Test: `tests/test_exportgltf_world.cpp`

**Interfaces:**
```cpp
struct GltfExportOptions {           // existing fields unchanged
    std::optional<std::array<float, 16>> rootMatrix;   // set: root node "GW2_MapToGltf" with "matrix", rotation/scale ignored
    std::string imageDirUtf8;        // non-empty: images written as <imageDir>/<fileId>.png, referenced by URI
    std::string imageUriPrefix;      // e.g. "Images/"
};
int GltfWriter::add_texture_uri(uint32_t fileId, const std::string& uri, const std::string& name = {});   // dedup by fileId as add_or_reuse_texture
bool write_model_textures(GltfWriter&, const std::vector<ModelTextureCPU>&, std::vector<int>& texIndices,
                          const std::string& imageDirUtf8 = {}, const std::string& uriPrefix = {});
struct VrchatFolderOptions { GltfExportOptions gltf; };
VrchatFolderResult write_vrchat_folder(const ModelPreview&, const std::string& folderUtf8, const std::string& name,
                                       uint32_t modelFileId, const VrchatFolderOptions& opts);   // the 4-arg form forwards {}
```
**Settled.** The spec's "point to the folder's `Textures/` PNGs" means the glb's own raw images, which the Poiyomi maps in `Textures/` are built from. They go to `models/<fileId>/Images/<fileId>.png`. Task 14 records this as an amendment.

- [ ] **Step 1: Write failing tests**
  - `world_gltf.default_vrchat_folder_unchanged`: `glow_quad()` (copy `tests/test_exportgltf_vrchat.cpp:1279`) through the 4-arg and the 5-arg `{}` forms gives byte-equal `.glb` and `materials.json`. The glb root is named `GW2_ZupToYup` and has a `rotation`.
  - `world_gltf.root_matrix_replaces_rotation`: with `rootMatrix = map_to_gltf_root(1)`, the root is `GW2_MapToGltf` with a `matrix` and no `rotation`.
  - `world_gltf.images_by_uri`: with `imageDirUtf8 = dir/"Images"` and prefix `"Images/"`, every `images[k]` has `uri == "Images/<fileId>.png"` and no `bufferView`, and the files exist.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_gltf.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_exportgltf.exe` → PASS (the existing vrchat and writer tests included).
- [ ] **Step 5: Commit** `feat(exportgltf): root matrix and images by URI for world model files`.

### Task 6: Cell glTF writer `[review]`

**Files:**
- Create: `include/castlemist/exportgltf/world_cell.h`, `src/exportgltf/world_cell.cpp`
- Test: `tests/test_exportgltf_world.cpp`

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
struct CellMesh { std::vector<float> pos, nrm, uv; std::vector<uint32_t> idx; size_t vertices() const { return pos.size() / 3; } };
/// Welded: one vertex per distinct sample of the cell's chunks ((w*seg+1) x (h*seg+1)), positions minus cell.origin,
/// normals from world::chunk_normals of the owning chunk, uv (0..1, v = 0 north). Winding as world_export.cpp:107-116
/// (cross product along -Z = up). A missing chunk leaves its quads out.
CellMesh build_cell_terrain(const world::Terrain& t, const world::Cell& c);
/// Same grid at z = planeZ, uv as terrain; empty when bake_cell_water would be false.
CellMesh build_cell_water(const world::WorldScene& s, const world::Cell& c);
/// The cell's collision instances' triangles (collision_instance_triangles), cell-local, unwelded
/// (3 vertices per triangle), split into parts of at most floor(maxVertices/3) triangles.
std::vector<CellMesh> build_cell_colliders(const world::WorldScene& s, const world::Cell& c, size_t maxVertices = 65535);
struct CellImages { std::string albedoUri, waterUri; };   // relative to the .glb
/// Root "GW2_MapToGltf" (map_to_gltf_root(1)) -> "Cell" (translation = origin) -> "Terrain", "Water", "Collider_k".
/// Materials "Terrain" (baseColor albedo), "Water" (baseColor water, BLEND), "Collision" (no texture).
bool write_cell_gltf(const std::string& glbPathUtf8, const world::Cell& c, const CellMesh& terrain, const CellMesh* water,
                     const std::vector<CellMesh>& colliders, const CellImages& images, std::string* error = nullptr);
}
```
- [ ] **Step 1: Write failing tests**
  - `world_cell.terrain_welded_vertex_count`: `grid(4,4)` (Task 2's helper, copied) with cell `c0_0_4x4` gives `vertices() == 16641` and `idx.size() == 32768*3`. Every index is < 65536. Vertex 0 is `(-15360 - origin[0], 39936 - origin[1], h00)`. `uv[0..1] == (0, 0)` and the last uv is `(1, 1)`. The first triangle's cross product has z < 0.
  - `world_cell.shared_edge_welded`: two chunks whose shared edge heights are equal give no duplicated positions.
  - `world_cell.collider_parts_under_65535`: one hull of 50 000 triangles gives 3 parts with triangle counts 21845, 21845 and 6310. Each part has `vertices() <= 65535`.
  - `world_cell.water_only_where_below`: terrain all at z = −5 with plane 0 gives an empty mesh.
  - `world_cell.gltf_layout`: the node path is `GW2_MapToGltf` → `Cell` (`translation == origin`) → `Terrain`, `Water`, `Collider_0`. `images[0].uri == "c0_0_4x4_albedo.png"` and the Water material is `BLEND`.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_cell.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(exportgltf): cell glTF with welded terrain, water and collider parts`.

### Task 7: Manifest writer and JSON Schema `[review]`

**Files:**
- Create:
  - `include/castlemist/exportgltf/world_manifest.h`, `src/exportgltf/world_manifest.cpp`
  - `tools/world/castlemist-world.schema.json`
- Test: `tests/test_exportgltf_world.cpp`. Add `CM_SOURCE_DIR="${PROJECT_SOURCE_DIR}"` to `cm_test_exportgltf` with `target_compile_definitions`.

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
inline constexpr int kWorldManifestVersion = 1;
struct UnityTrs { float t[3]; float r[4]; float s; };   // r = xyzw
/// U = map_matrix_to_unity(M with translation minus origin). s = |U col0|; false when the column lengths differ
/// by > 1e-4 relative, det(U3) <= 0, s <= 0 or anything is non-finite. r from U3/s (Shepperd's method, w >= 0).
bool unity_trs(const float mapWorld[16], const float cellOrigin[3], UnityTrs& out);
struct WorldFolderOptions {
    int texelsPerChunk = 256, waterTexelsPerChunk = 32, maxTextureSize = 512, threads = 0;   // 0 = hardware_concurrency
    world::CellBudget budget; world::ChunkRegion region;
    float groundRepeats = world::kGroundRepeatsPerChunk; bool srgbColour = true;
    float unityScale = 0.0254f;          // UNPROVEN, gltf_export.h:57
    bool sky = true, collision = true, overwrite = false;
};
struct ManifestSetting { std::string name; nlohmann::json value; std::string status, source; };
/// The spec D3 table + texelsPerChunk, waterTexelsPerChunk, maxTextureSize, the three budgets, unityScale, ambient.
std::vector<ManifestSetting> world_export_settings(const WorldFolderOptions& o);
struct ManifestModel { uint32_t index = 0, fileId = 0; bool ok = false; std::string error, folder, glb, materials;
                       uint64_t triangles = 0, textureBytes = 0; std::vector<std::string> clips, darkDecalMaterials;
                       nlohmann::json uvScroll = nlohmann::json::array(); };
struct ManifestCell { const world::Cell* cell = nullptr; std::string glb, albedo, water; int albedoW = 0, albedoH = 0;
                      size_t colliderParts = 0, colliderTriangles = 0; };
struct WorldManifestInput {
    const world::WorldScene* scene = nullptr; const WorldFolderOptions* opts = nullptr;
    std::vector<ManifestModel> models; std::vector<ManifestCell> cells;
    nlohmann::json sky, light, spawn, counts; std::vector<std::string> warnings;
    std::function<std::string(uint32_t propIndex)> clipOf;   // "" = null
};
/// The manifest of spec "manifest.json (version 1)". Instances whose unity_trs fails are omitted and counted in
/// counts.propsSkipped with a warning "props: <n> placements with a non-uniform or degenerate transform skipped".
nlohmann::json world_manifest_json(const WorldManifestInput& in);
}
```
**Settled.** `light` uses `light.sun.toSun` = `MapEnvLight::sunDir` as stored (`map_types.h:37`: toward the sun), `unityForward = −map_to_unity(sunDir)` and `blenderDirection = −map_to_blender(sunDir)`. Ambient is `fillColor · min(fillIntensity, 1)`, named in `settings.ambient` as castlemist's choice.

**Schema.** JSON Schema 2020-12, `$id` `castlemist-world/1`. It is limited to the keywords `$schema $id $defs $ref title description type properties required additionalProperties items enum const minimum maximum minItems maxItems`. Nullable values use `"type": ["object", "null"]`. Every object the importers read has `additionalProperties: false`.

- [ ] **Step 1: Write failing tests**
  - `world_manifest.unity_trs_roundtrip`: the spec's prop (`pos {10, 20, 30}`, `rot {0.3, −0.2, 1.1}`, `scale 2`, built as `tests/test_world.cpp`'s `prop_transform_matches_client`) with origin (5, 5, 0). Rebuild the matrix from t / r / s. It equals `map_matrix_to_unity` of M with its translation minus the origin, within 1e-4. `s ≈ 2`, `r[3] >= 0`.
  - `world_manifest.unity_trs_rejects_bad`: false for scale (1, 2, 1), for a mirrored matrix (det < 0) and for a NaN.
  - `world_manifest.keys_match_schema`: read the schema from `CM_SOURCE_DIR "/tools/world/castlemist-world.schema.json"`. The keys of `world_manifest_json(synthetic)` are a superset of `required` and a subset of `properties`, at the top level and for `cells[0]`, `cells[0].instances[0]` and `models[0]`. `version == 1` and `format == "castlemist-world"`.
  - `world_manifest.settings_labelled`: every entry of `settings` has `status` in {`UNPROVEN`, `castlemist's choice`, `measured`}. `groundRepeatsPerChunk` is `{value: 8, status: "UNPROVEN"}`, and every UNPROVEN setting has a line in `warnings` containing its name and `UNPROVEN`.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_manifest.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(exportgltf): world manifest v1 and its JSON Schema`.

### Task 8: `export_world_folder` orchestration `[review]`

**Files:**
- Create:
  - `include/castlemist/exportgltf/world_folder.h`, `src/exportgltf/world_folder.cpp`
  - `include/castlemist/exportgltf/world_dat_source.h`, `src/exportgltf/world_dat_source.cpp`
  - `tests/world_export_synthetic.h`: inline `synthetic_world()`, `static_cube()` and `skinned_cube()`, adapted from `tests/test_exportgltf_writer.cpp:134,169,593`. It is a 2 × 2-chunk grid, models 101 (static, 1 material) and 102 (skinned, clip `TestClip`, `animClipTokens = {0x1234}`). There are 3 props of 101 and 1 of 102 (`propAnimArray`, `animSequence = 0x1234`), one hull instance, and a water plane at 0 with one preset.
- Modify: `tests/tools/world_export_fixture.cpp` (writes the full synthetic export unless `--probe-only`)
- Test: `tests/test_exportgltf_world.cpp`; dat: `tests/test_exportgltf_world_dat.cpp` (added to the `dat` suite, with `castlemist::world` added to its DEPS at `tests/CMakeLists.txt:94`)

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
class ModelSource {
public:
    virtual ~ModelSource() = default;
    /// Model `scene.models[index]`, textures header-only or decoded; null = failed. Thread-safe.
    virtual std::shared_ptr<ModelPreview> load(uint32_t index, uint32_t fileId) = 0;
    /// decode_texture_capped result; null or empty mips = failed. Thread-safe.
    virtual std::shared_ptr<const CappedTexture> texture(uint32_t fileId, int maxSize) = 0;
};
/// Shared, read-only parsed dat (sub-project 2, R13). Models: load_model_by_fileid(dat, id,
/// ModelLoadOptions{wantGame, /*decodeTextures=*/false}); textures: decode_texture_capped, cached by fileId under a mutex.
std::unique_ptr<ModelSource> make_dat_model_source(Gw2Dat& dat, bool wantGame = false);
struct WorldFolderReport {
    bool ok = false; std::string error, dir, manifest;
    size_t cells = 0, modelsWritten = 0, modelsFailed = 0, propsWritten = 0, propsSkipped = 0,
           chunksUnbaked = 0, animated = 0, animatedResolved = 0, clipsFailedDecode = 0;
    std::map<std::string, double> seconds;    // world-cells, models, terrain-textures, cells, sky, manifest
    std::map<std::string, uint64_t> bytes;    // prop-textures, terrain, geometry, sky
    std::vector<std::string> warnings;
};
/// `sky` may be null (no sky). Order: spec "Order of work" 1-8 (Blender is the caller's step 9).
WorldFolderReport export_world_folder(const world::WorldScene& scene, ModelSource& src, const sky::SkyInputs* sky,
                                      const std::string& dirUtf8, const WorldFolderOptions& opts);
nlohmann::json report_json(const WorldFolderReport& r);
}
```
**Rules:**
- **Output folder.** Absent or empty: go ahead. Holds `manifest.json` and `overwrite` is set: remove its contents first. Anything else: `error = "output folder not empty: <dir>"`.
- **Model stage.** `threads` workers pop the needed model indices (those with an instance in a kept cell) from an atomic counter. Each worker:
  - fills `ModelPreview::textures[i].rgba` from `texture(fileId, maxTextureSize)` mip 0, with `width` and `height`;
  - records `triangles = Σ mesh.indices.size()/3`, the distinct texture fileIds and `clips = animClips[k].name`;
  - records `darkDecalMaterials`: material `k` where `!mat.hasRenderState && capped(diffuse).hasCutout && world::dark_coverage(mip0)`, as `world_alpha_mode` does (`src/render/world_materials.h:66`);
  - records `uvScroll`: the first `texTransforms` entry with `uvIndex == diffuseUv` and `tex_transform_animates` (the `prop_materials` rule, `world_materials.h:70-94`);
  - calls `write_vrchat_folder(models/<fileId>, name = std::to_string(fileId), {gltf: {rootMatrix = map_to_gltf_root(1), imageDir = Images}})`;
  - drops the preview.

  Results are stored by model index, so output does not depend on the thread count.
- **Clip per placement.** `resolve_prop_clip(model.animClipTokens, animSequence)`. A token that is in `animationTokens` but not in `animClipTokens` counts as `clipsFailedDecode`, which is sub-project 3's split.
- **Cells.** Run `partition_cells` with `bakedBytesPerChunk = T²·3 + waterT²·4`. The terrain textures are `terrain_texture_ids` of the kept cells' chunks, fetched through `src.texture(…, maxTextureSize)` on the pool. Cells are baked and written in parallel, with the albedo PNG written by `encode_png` (`internal.h:144`).
- **Spawn.** `CollisionIndex::build(scene, true)` raycast from (region centre x, y, lowest z − 1000) along (0, 0, 1). The probe is placed at the spawn, or at the region centre with z 0.
- **Manifest last.** Write `manifest.json.tmp`, then rename it. Any fatal error before that leaves no `manifest.json`. `DatIoError` (`world/dat_read.h`) is fatal; a model's own failure is not.
- **Sky.** `write_skybox(*sky, dir, "sky")`; its `ok == false` with `error == "no sky"` gives `sky: null` with a warning.

- [ ] **Step 1: Write failing tests**
  - `world_folder.synthetic_export_complete`: these files exist:
    - `manifest.json`, `probe.glb`
    - `cells/c0_0_2x2.glb`, `cells/c0_0_2x2_albedo.png`, `cells/c0_0_2x2_water.png`
    - `models/101/101.glb`, `models/101/materials.json`, `models/102/102.glb`

    In the manifest:
    - `counts.props == 4`, `propsWritten == 4`, `models == 2`, `cells == 1`;
    - `animated == 1`, `animatedResolved == 1`;
    - the 102 instance has `clip == "TestClip"`;
    - `cells[0].colliderParts == 1`, and `water.plane == true`.
  - `world_folder.failed_model_counted_and_skipped`: the source returns null for index 0. The export is still `ok`, `models[0].status == "failed"` and `propsSkipped == 3`.
  - `world_folder.manifest_written_last`: a source whose `texture()` throws `castlemist::world::DatIoError` gives `!ok` and no `manifest.json` (a `.tmp` may remain).
  - `world_folder.refuses_nonempty_dir`: a stray file gives `!ok` and an error mentioning "not empty". With `manifest.json` present and `overwrite = true`, the export is `ok`.
  - `world_folder.parallel_matches_serial`: `threads = 1` against `threads = 4` gives manifests equal after dropping `castlemist` and the report's `seconds`.
  - Dat `world_export_dat.want_game_same_materials`: model 1766522, `wantGame` false against true. `materials.json` must be equal apart from the `castlemist` key.

    **Deciding rule:** if it is not equal, change `make_dat_model_source`'s default to `wantGame = true`, put the measured extra seconds on Spirit Vale in the report, and note it for Task 14.
- [ ] **Step 2: Run** `cm_test_exportgltf.exe world_folder.` → FAIL; `GW2_TEST_DAT=... cm_test_dat.exe world_export_dat.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** both → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(exportgltf): export_world_folder with a parallel model stage, cells, sky, spawn and manifest`.

### Task 9: Dependency-free manifest validator and dry run `[light]`

**Files:**
- Create: `tools/world/validate_manifest.py`
- Modify: `tests/CMakeLists.txt` (Python ctests on the `world_fixture` fixture)

**Interfaces:** `python -I tools/world/validate_manifest.py <exportDir> [--schema <path>] [--plan] [--self-test]`. The default schema is `castlemist-world.schema.json`, next to the script.
- **Exit codes.** 0 when valid. 1 with one `ERROR <path>: <message>` line per problem.
- **Schema check.** `check(instance, schema, path)` handles exactly the Task 7 keyword list, and `ERROR schema: unsupported keyword <k>` for anything else. When `jsonschema` imports, it also runs `jsonschema.validate`.
- **Semantic checks:**
  - every non-null file field exists (`missing file: <rel>`);
  - `model` indices are in range (`index out of range: cells[i].instances[k].model`);
  - every matrix is finite;
  - `unity` reproduces `C_u (m − origin) C_u⁻¹` within 1e-4 (`unity transform mismatch`);
  - cell chunk rects tile the region without overlap;
  - counts add up (`propsWritten + propsSkipped == props`, Σ instances == propsWritten).
- **`--plan`** prints one line, matching the spec's "Unity mapping" table:
  `PLAN materials=<n> prefabs=<n> controllers=<n> cells=<n> instances=<n> colliders=<n> terrainColliders=<n> water=<n> lights=1 skybox=<0|1>`. Here `materials` = Σ over ok models of `len(materials.json["materials"])` with `usedByMeshes != false`, plus one terrain material per cell, plus one water material per water cell. `controllers` = the distinct (model, clip) pairs.
- **`--self-test`** copies the folder to a temp folder three times and corrupts each copy: deletes `models/<first>/materials.json`, sets `cells[0].instances[0].model = 9999`, and adds `map.extra = 1`. Each copy must fail with `missing file`, `index out of range` and `unexpected property: map.extra` respectively. Then it prints `SELFTEST OK`.

- [ ] **Step 1: Write failing tests.** These are ctests with FIXTURES_REQUIRED `world_fixture`, whose setup runs `cm_world_export_fixture ${CMAKE_BINARY_DIR}/world_fixture`.
  - `world_manifest_validate` expects `^OK`.
  - `world_manifest_plan` expects `PLAN materials=4 prefabs=2 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0`.
  - `world_manifest_selftest` expects `SELFTEST OK`.
- [ ] **Step 2: Run** `ctest --preset debug -R world_manifest` → FAIL.
- [ ] **Step 3: Implement.** Standard library only. Run it with `-I` (isolated mode), as the ctests do.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(tools): dependency-free world manifest validator with a Unity dry-run plan`.

### Task 10: CLI `world-export --out-dir` `[light]`

**Files:**
- Modify: `tools/gw2dat_cli/main.cpp` (`cmd_world_export` :2424-2500; the usage line at :2505)
- Test: a CLI smoke check in Step 4

**Interfaces:** `gw2dat_cli world-export --dat <dat> --file-id <map> --template <json> --out-dir <dir> [--region cx0,cy0,cx1,cy1] [--texels-per-chunk 256] [--max-texture 512] [--cell-max-chunks 4] [--cell-max-tris 1000000] [--cell-max-texture-mb 256] [--no-sky] [--no-fbx] [--no-collision] [--overwrite] [--threads n]`.
- Exactly one of `--out` and `--out-dir` is allowed; anything else is `fail("give exactly one of --out <file.glb> or --out-dir <dir>")`. `--out` keeps today's path untouched.
- **Flag validation:**
  - `--texels-per-chunk` must be in {64, 128, 256, 512};
  - `--max-texture` must be a power of two from 64 to 4096;
  - `--region` is four integers.
- **Flow:**
  1. `load_dat_file`, then `load_world`.
  2. `sky::load_sky_inputs(dat, world::read_file_bytes(dat, fileId), tpl, fileId)` unless `--no-sky`, on the main thread, as `cmd_skybox` does (:1467).
  3. `export_world_folder(scene, *make_dat_model_source(dat), skyPtr, dir, opts)`.
  4. Unless `--no-fbx` is given or Blender is missing, `ripper::convert_world_to_fbx`. Task 11 adds this step; until then the CLI stops after step 3.
  5. Print `report_json` and exit 1 when not ok.

- [ ] **Step 1: Write the failing check.** `gw2dat_cli world-export ... --out-dir x --out y.glb` must exit 1 with the message above. Run it by hand.
- [ ] **Step 2: Run** it → it currently succeeds or fails differently.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `build/debug/bin/gw2dat_cli.exe world-export --dat "$GW2_TEST_DAT" --file-id 1151420 --template <tpl> --out-dir "$TMP/sv" --no-fbx`. Expect `"ok": true`. Then run `python -I tools/world/validate_manifest.py "$TMP/sv"` → `OK`.
- [ ] **Step 5: Commit** `feat(cli): world-export --out-dir writes the cell folder and manifest`.

### Task 11: Blender importer and FBX step `[review]`

**Files:**
- Modify: `tools/blender/castlemist_world.py` (adds `--import` and the full `--to-fbx`)
- Modify: `include/castlemist/ripper/vrchat.h` (:75) and `src/ripper/vrchat.cpp` (`run_blender` :312 gains an argument-list overload; `default_script` :371 gains a script-name parameter)
- Modify: `src/exportgltf/world_manifest.cpp` gains `set_manifest_fbx`
- Modify: `tools/gw2dat_cli/main.cpp` (wire Step 4 of Task 10)
- Test: ctests `blender_world_import` and `blender_world_fbx_clip`

**Interfaces:**
```cpp
namespace castlemist::exportgltf {
/// Sets cells[].fbx, models[].fbx and frameProbe.fbx to the relative path of each .fbx that exists next to its .glb;
/// rewrites manifest.json atomically (tmp + rename). Returns the count set.
size_t set_manifest_fbx(const std::string& exportDirUtf8);
}
namespace castlemist::ripper {
/// find_blender(); runs tools/blender/castlemist_world.py --to-fbx <dir> in one session (log in <dir>/blender.log),
/// then set_manifest_fbx. false + log on failure or when Blender is missing ("Blender not found").
bool convert_world_to_fbx(const std::string& exportDirUtf8, std::string& log);
}
```
**Script.** `castlemist_world.py --import <dir> [--no-colliders] [--no-dark-decal] [--scale 1.0] [--verify]`:
- **Collections.**
  - `GW2 <fileId>`.
  - `Models`, excluded from the view layer: one collection per model from its `.glb`, plus `<fileId>_<clip>` copies whose armature gets that action and loops.
  - `Cells/<id>`: `Terrain`, `Water`, `Props` (empties with `instance_type='COLLECTION'` and `matrix_world = C_b M C_b⁻¹ · scale`, `C_b = diag(1, 1, −1)`) and `Colliders` (hidden).
- **Scene.** A Sun lamp from `light.sun.blenderDirection` and the World environment texture from `sky.equirect`.
- **Materials.** Dark-decal materials get `blend_method = 'BLEND'`.
- **UV scroll.** A driver `frame / fps * scroll` on a Mapping node.
- **`--verify` prints:**
  - `INSTANCES <n>`
  - `MATRIX_MAX_ERR <e>`
  - `PROBE ... OK|FAIL`
  - `TERRAIN_IMAGES <n>`
  - `COLLIDERS_HIDDEN <0|1>`
  - `VERIFY OK`, when the instance count equals the manifest's, the error is < 1e-4, the probe passes, every cell's terrain has an image and the colliders are hidden.

- [ ] **Step 1: Write failing tests.** Both ctests require the `world_fixture` fixture.
  - `blender_world_import`: `blender -b --factory-startup -P tools/blender/castlemist_world.py -- --import <fixture> --verify`, with PASS_REGULAR_EXPRESSION `INSTANCES 4[\s\S]*VERIFY OK`.
  - `blender_world_fbx_clip`: `--to-fbx <fixture>`, then `blender ... -P tests/blender/check_fbx.py -- <fixture>/models/102/102.fbx`, with PASS_REGULAR_EXPRESSION `ACTIONS .*TestClip`.
  - `world_folder.set_manifest_fbx` (C++): create an empty `models/101/101.fbx` in a fixture copy. The call returns 1, and `models[0].fbx == "models/101/101.fbx"`.
- [ ] **Step 2: Run** `ctest --preset debug -R "blender_world|set_manifest"` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → PASS. Then run the CLI on Spirit Vale without `--no-fbx`, record the `--to-fbx` seconds, and validate the folder.

  **Deciding rule:** if the FBX step takes more than 30 minutes on Spirit Vale, write one `--to-fbx` list per batch of 200 files, but keep a single session per batch. Record the time for Task 14.
- [ ] **Step 5: Commit** `feat(blender): world importer (collections, instanced props, colliders, sky, sun) and one-session FBX conversion`.

### Task 12: Unity importer `[review]`

**Files** (create, under `tools/unity/Castlemist/Editor/`):
- `MiniJson.cs`: a reader to `Dictionary<string, object>`, `List<object>`, `double`, `string`, `bool` and `null`. Unity's `JsonUtility` cannot read nested arrays.
- `WorldManifest.cs`: typed accessors and a `version` check.
- `PoiyomiMaterials.cs`: `materials.json` → `Material`, following `poiyomi-10-importer.md` §2-§5. `_Mode` alone does nothing (§2), so the preset table sets render queue, `RenderType`, blend factors and `_ZWrite`. Packed map: `_MochieMetallicMaps` + `MOCHIE_PBR`. Emission slot 0. Pan `_MainTexPan = 20 · scroll`. Without Poiyomi, the Standard shader and one warning.
- `WorldImporter.cs`: menu **Castlemist ▸ Import World…**, then spec D6 steps 1-10. The root `GW2_<fileId>` has `localScale = unityScale`, from the window field whose default comes from the manifest. Each instance gets `localPosition = t`, `localRotation = new Quaternion(r[0], r[1], r[2], r[3])` and `localScale = s`. Static flags are `BatchingStatic | OccluderStatic | OccludeeStatic`, and prop materials get `enableInstancing = true`.
- `CastlemistPostprocessor.cs`: `OnPreprocessTexture` by file role (research note §5, Textures).
- `BatchEntry.cs`: gains `ImportWorld` (`-castlemistWorld <Assets/…>`).
- `tools/unity/CHECKLIST.md`: the manual checklist (spec Testing, Unity), with a results table.

**Interfaces** (C#):
```csharp
namespace Castlemist {
public static class WorldImporter { [MenuItem("Castlemist/Import World...")] static void Menu(); public static ImportSummary Import(string assetFolder, float unityScale); }
public sealed class ImportSummary { public int materials, prefabs, controllers, cells, instances, colliders, terrainColliders, water, lights, skybox; public List<string> warnings; public string PlanLine(); } // same format as --plan
public static class FrameProbe { public static bool Check(GameObject probeRoot, out string line); }
public static class BatchEntry { public static void CheckProbe(); public static void ImportWorld(); }
}
```
- [ ] **Step 1: Write the failing check.** This can't run in CI. The check is `ImportSummary.PlanLine()` for the fixture, which must equal Task 9's expected `PLAN` line. CHECKLIST item 1 states this.
- [ ] **Step 2: Optional local run.**
  1. Copy `tools/unity/Castlemist` and `${CMAKE_BINARY_DIR}/world_fixture` into a scratch Unity 6 project's `Assets/`.
  2. Run `Unity.exe -batchmode -nographics -quit -projectPath <scratch> -executeMethod Castlemist.BatchEntry.ImportWorld -castlemistWorld Assets/world_fixture -logFile -`.
  3. Before the importer is written, this FAILS to compile or prints nothing.
- [ ] **Step 3: Implement.** It must compile as C# 9 for 2022.3: no C# 10 features (no file-scoped namespaces, no global usings).
- [ ] **Step 4: Run** the local batchmode run. Expect the `PLAN` line equal to Task 9's and `PROBE ... OK`. If Unity cannot run here, write "batchmode not run" in CHECKLIST's results.
- [ ] **Step 5: Commit** `feat(unity): world importer: Poiyomi materials, prefabs, instances, colliders, sky, sun, frame probe`.

### Task 13: Dat test on Spirit Vale and three-map measurements `[light]`

**Files:**
- Modify: `tests/test_exportgltf_world_dat.cpp`
- Test: same

**Interfaces:** consumes `export_world_folder`, `make_dat_model_source`, `CollisionIndex`, and the `cm_test_world_dat` helpers' pattern for `GW2_TEST_DAT` / `GW2_TEST_TEMPLATE`.

- [ ] **Step 1: Write failing tests**
  - `world_export_dat.spirit_vale` (1151420), exported to a temp folder with `--no-fbx` defaults:
    - `ok`;
    - `counts.props == scene.props.size()` and `chunks == 250`;
    - `chunksUnbaked == 0` and `animatedResolved == 106`;
    - every `scene.models` fileId appears exactly once in `models`;
    - Σ `colliderTriangles` == Σ over instances of `collision_instance_triangles` / 9;
    - 32 rays (a fixed LCG seed, x and y uniform in `bounds`, from z = −20000 along (0, 0, 1)) hit within 0.01 of the same `t`, comparing a `CollisionIndex::from_triangles` built from the exported cell meshes (re-read through `glb_json` plus the BIN chunk, then de-localised by the cell origin) against `CollisionIndex::build(scene, true)`;
    - when `python` is on PATH, `validate_manifest.py` returns 0.
  - `world_export_dat.measure` (prints only, never fails) on 192711, 191000 and 1151420 prints per map:
    - the per-4 × 4-block triangle and texture-MB distribution (min, median, p90, max);
    - the number of blocks split;
    - the folder bytes by kind;
    - the stage seconds.
- [ ] **Step 2: Run** `GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" GW2_TEST_TEMPLATE=<tpl> build/debug/bin/cm_test_dat.exe world_export_dat.` → `spirit_vale` FAILS until every count holds.
- [ ] **Step 3: Fix what fails.** Fix the code, never the expected values. A disagreement with the 106 figure is resolved against sub-project 3's measurement (spec amendments, Task 15 of that plan).
- [ ] **Step 4: Run** → PASS, and keep the `measure` output.

  **Deciding rule for the budgets:** keep the defaults unless more than 90 % of blocks, or none of them, split on Queensdale. Then set `cellMaxTriangles` to the p90 block value rounded up to a multiple of 250 000, and say so in the amendments.
- [ ] **Step 5: Commit** `test(world-export): Spirit Vale export against WorldScene; budget and size measurements on three maps`.

### Task 14: Docs, checklists and spec amendments `[light]`

**Files:**
- Modify:
  - `docs/testing.md`: a "World export" section with the commands, the ctests (blender, python), the dat test and links to `tools/unity/CHECKLIST.md`
  - `tools/unity/CHECKLIST.md`: results filled
  - `docs/superpowers/specs/2026-10-15-world-export-design.md`: `## Amendments (after implementation)`
  - `docs/research/gw2-world-frame.md`: §1.4 links to the probe; the `Images/` note
  - `docs/research/README.md`: an index line if a note changed
- Test: none (docs).

The amendments record:
- Task 1's FBX axis outcome and the Unity probe result, or "not run";
- the `Images/` folder decision (Task 5);
- `wantGame` equality (Task 8);
- the FBX time (Task 11);
- the batchmode result (Task 12);
- the measurements and any budget change (Task 13);
- the Unity and Blender manual checklist results, with dates. Anything not tried is listed as not tried.

- [ ] **Step 1: Run the Blender manual checklist** (spec Testing, Blender) on the Spirit Vale export: open it in Blender 4.5, render one Cycles frame at the spawn and play the timeline. Record the results.
- [ ] **Step 2: Run the Unity checklist** if a VRChat 2022.3 project with Poiyomi 10 is available. Otherwise record "not run: no VRChat project". Done is then "dry run + schema", as the spec says.
- [ ] **Step 3: Write** the amendments and the docs.
- [ ] **Step 4: Run** `ctest --preset debug` → PASS (no code change, a sanity check).
- [ ] **Step 5: Commit** `docs: world export testing, checklists and spec amendments`.

### Task 15: File ▸ Export World (Map)… and removal of the old map path `[review]`

**Files:**
- Modify:
  - `src/ui/window_proc.cpp` (:36 menu text, :45, :1303)
  - `src/ui/view_controls.cpp` (:24-31: `mapReady = kind == Map && !decompressed.empty()`)
  - `src/ui/file_ops.cpp`: replace `do_export_gltf_map` (:498) with `do_export_world_map`. It uses the folder prompt of `do_export_skybox_map` (:620). Its thread does `load_dat_file` on its own `Gw2Dat`, as `src/ui/map_window_loader.cpp:107-111` does, then `load_world`, `load_sky_inputs`, `export_world_folder` and `ripper::convert_world_to_fbx`, and posts `WM_APP_GLTF_EXPORT_DONE` with a message box showing the report.
  - `src/ui/preview.cpp` (:396-420: a map entry shows `world_summary` text and "Open it with Tools > Map Window...")
  - `src/extract/entry_extractor.cpp` (:164, :441: `build_map_scene` calls removed)
  - `include/castlemist/extract/map_types.h`
  - `include/castlemist/exportgltf/gltf_export.h`
  - `src/render/geometry.cpp`, `src/render/model_renderer.cpp`, `src/render/device.cpp`
  - `CMakeLists.txt`
  - the tests that reference removed symbols
- Delete: `src/extract/map_scene.cpp`, `src/render/map_fly.cpp`, `src/render/detail/map_fly.h`, `src/exportgltf/map_export.cpp`
- Test: `tests/test_ui_map_window.cpp` (control-id uniqueness stays), the existing suites

**Removal rule.** Grep each symbol before removing it:
- `MapScene`, `MapInstance`, `build_map_scene`, `ExtractedEntry::map`, `export_map_gltf`, `RenderMode::MapFly`, `fly_*`, `render_fly`, `ID_FILE_EXPORT_GLTF_MAP`.
- **Removed:** what only the old path uses.
- **Kept, because others use them:** `set_scene` / `SceneInstance` (the headless scene shot, `src/ui/application.cpp:216`), `MapEnvRig` and `set_environment_rig` (`src/render/detail/state.h:319`, `game_material.cpp:355`), `mesh_bake`, and the fly camera if `WorldView` uses it.
- The menu id is renamed to `ID_FILE_EXPORT_WORLD_MAP` with the same value.

- [ ] **Step 1: Write failing tests**
  - `ui.export_world_menu_id_unique`, added to the existing control-id test: the new id appears exactly once.
  - Removal check (not a `CM_TEST`): delete the `export_map_gltf` tests in `tests/test_exportgltf_writer.cpp`. In Step 4, `grep -rn "export_map_gltf\|build_map_scene\|MapFly" src include tests tools` must return nothing.
- [ ] **Step 2: Run** `cm_test_ui.exe` → FAIL (the id does not exist yet).
- [ ] **Step 3: Implement** the menu item, then the removal.
- [ ] **Step 4: Run** a full build, `ctest --preset debug` → PASS, and the grep → empty. Then the app with `GW2_DAT` set: select Spirit Vale's map entry, use **File ▸ Export World (Map)…**, and run `validate_manifest.py` on the folder → `OK`. Add a line to the spec amendments that the removal is done.
- [ ] **Step 5: Commit** `feat(ui): Export World (Map) replaces Export glTF (Map); remove MapScene, map_fly and export_map_gltf`.
