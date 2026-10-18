# Testing

Two complementary layers: a unit suite that runs anywhere in under ten seconds,
and headless `GW2_*` hooks in the application itself for the parts that need a
GPU and a window.

## The unit suite

```bash
ctest --preset debug
```

```bash
./build/debug/bin/cm_test_core.exe            # one suite directly
```

```bash
./build/debug/bin/cm_test_core.exe packfile   # filter by "group.name" substring
```

One executable per layer, so a failure names the layer immediately and a
`core` failure can never be caused by a Direct3D device that would not create.

| Suite | Covers |
|-------|--------|
| `native` | method-0 round trips: single-symbol input, incompressible input, payloads crossing the CRC framing stride |
| `core` | `Bytes` bounds behaviour, packfile traversal, pointer width, text conversion, env hooks |
| `format` | DDS header and pitch, chat-link decoding, strs framing |
| `db` | The index SQL, on a fixture DB **and** on the real index |
| `extract` | Signature sniffing, content classification, summary parsers, `parallel_for` |
| `render` | Row-vector matrix convention, projection, quaternion pose maths; `world_view`: camera matrices, pixel rays, prop upload and pick triangles, and a WARP-device render of terrain, streamed props, collision and water |
| `sim` | Cloth pinning, clamping, and that anchors never drift |
| `media` | Audio/video probe and reject paths |
| `ui` | Control-id uniqueness, timer and message ids, theme brush caching; `map_window`: fly-camera and walk-delta maths, key map, model-pool size |
| `world` | `world_walk`: `CollisionIndex` raycasts and the `Walker` (settle, wall slide, step-up, ledge fall, jump, fall safety, ceiling, swept fall); `world_stream`: `model_load_order`, `rank_models`, `ModelQueue` (re-sorting around a new camera, never repeating a taken model) and `chunk_normals` |
| `dat` | End-to-end extraction against a real `Gw2.dat`, and the Spirit Vale world export (see "World export") |

### The framework

`tests/framework/test_framework.h` -- about 200 lines, no dependencies beyond
the standard library. GoogleTest or Catch2 would mean one more checkout under
`external/` for very little.

```cpp
CM_TEST(bytes, u32_is_little_endian) {
    const uint8_t raw[] = {0x78, 0x56, 0x34, 0x12};
    castlemist::core::Bytes b(raw, sizeof raw);
    CHECK_EQ(b.u32(0), 0x12345678u);
}
```

`CHECK`, `CHECK_FALSE`, `CHECK_EQ`, `CHECK_NE`, `CHECK_NEAR`, and `SKIP(reason)`
for a missing fixture. A skipped test is not a failure.

### Tests that need real data

The `dat` suite and two `db` tests open real files and **skip** when they are
absent, so the suite stays green on a machine without a game install.

| Variable | Default |
|----------|---------|
| `GW2_TEST_DAT` | `C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat` |
| `GW2_TEST_INDEX_DB` | `dumps/index/gw2_index.db` |
| `GW2_TEST_TEMPLATE` | `dumps/packfile/gw2_packfile.json` |

Both defaults are found by climbing from `build/<preset>/bin/` to the
repository root, so a normal `ctest --preset debug` picks them up with no setup.

The curated entry ids they assert against are in
`docs/research/curated-test-ids.txt`. They are
**baseIds** (MFT index + 1), not fileIds — mixing the two up is the most common
reason a "known good" id suddenly resolves to something else.

## Headless hooks

Anything needing a device, a window or a sound card is verified by running the
application with an environment variable set, then reading the dump it writes.
The full table is on `castlemist::core::env` in `include/castlemist/core/env.h`; the ones
used most:

```bash
GW2_AUTOLOAD=291977 ./build/debug/bin/castlemist.exe
```

Add `GW2_GAMESHOT=1` to also write `shot_game11.bmp` from the "Game 1:1" bgfx
view; `GW2_ORBIT=yaw,pitch` (radians) turns both views the same way.

Others, same shape: `GW2_GIZMOTEST=1` exercises gizmo hit-testing,
`GW2_CLOTHTEST=1` dumps rest/draped/wind BMPs, `GW2_SCENE=181140
GW2_SCENEGAME=1` loads a map with the game's own shaders, `GW2_AUDIOTEST=2861`
probes and plays an entry into `audiotest.txt`, and
`GW2_PARSETEST=118599,119671` runs the summary parsers into `parsetest.txt`.

### The Map window

`GW2_MAPWIN=<map fileId>` loads the dat (`GW2_DAT`, as `GW2_AUTOLOAD`), opens
the Map window on that map, waits until every model is loaded or has failed and
the collision index is built (at most 600 s), renders one 1280x720 frame, writes
one stats line to `mapwin.txt` in the dump directory, and quits. The line includes
the UI-thread time of each `add_models` batch (`add_batches`, `add_ms_median`,
`add_ms_max`), and the material fields: `shading` (`materials` or `geometry`),
`time` (the material clock, seconds), `textures`, `textureMB` (GPU size, all
mips), `texturesFailed`, `animated` (placements playing a clip),
`unresolvedClips` (placements with an `animSequence` that names no clip of
their model, or whose model has no skin), `chunksUntextured` and
`materialsUntextured` (draws that fell back to the plain look), `waterPreset`
(1 when the water is drawn from an env preset) and `animRect` (see
`GW2_MAPWIN_FOCUS`; `-1,-1,-1,-1` when none). If the window is closed before the
capture, the line says so with `ok=0`. The window is a normal desktop window: input given to it during the
run (a key press, a drag) moves the camera that the capture uses. `GW2_MAPWIN_SHOT=<bmp>` picks where the frame
goes (default `shot_mapwin.bmp` in the dump directory).

Three more hooks steer the capture:

- `GW2_MAPWIN_SHADING=geometry` draws in the plain look (sub-project 2's). The
  default is `materials`.
- `GW2_MAPWIN_TIME=<seconds>` freezes the material clock at that time (0 is
  honoured). Without it the clock runs and the capture is not repeatable.
- `GW2_MAPWIN_FOCUS=animated` frames the nearest playing animated placement
  instead of the whole map, and `animRect` is that placement's screen rectangle
  `x0,y0,x1,y1` (conservative when a corner is behind the camera). Two runs
  with different `GW2_MAPWIN_TIME` and the same focus can be compared inside
  that rectangle to prove that something moved.
- `GW2_MAPWIN_FOLIAGE=off|colliders|all` sets the Foliage layer (spec
  `2026-10-16-world-foliage-design.md` D6): off, or on with that sub-mode. The
  default, as in the window, is on with `colliders`.
- `GW2_MAPWIN_FOCUS=zonetree` frames the Collider zone instance nearest the
  start camera (the whole-map view) from 4x its world-sphere radius, turns the
  Collision layer on, and reports the sphere's screen rectangle as `treeRect`
  (`-1,-1,-1,-1` without this focus; `0,0,0,0` when it is off the surface).
  The camera looks 15 degrees below the horizon from the first of eight
  headings, starting at the start view's, whose sight line to the tree hits no
  collision. Stepping back along the steep whole-map view lands behind
  hills and the map-edge walls, whose collision then fills the shot.
  Compare a shot with `GW2_MAPWIN_FOLIAGE=off` against one with `colliders`
  at this focus: off shows the bare trunk colliders, on shows each inside its
  tree.
- `GW2_MAPWIN_FOCUS=foliage` stands 150 units above the ground in the
  foliage tile at the 90th percentile of placed zone instances (a p90 view for
  the budgets), looking 8 degrees down along the densest of eight headings
  (`foliage_focus_view`). `foliagemax` takes the densest tile, and `forest`
  the densest tile of Collider instances (a ground-level view into the trees).
- `GW2_MAPWIN_FOLIAGE_SCALE=<x>` sets `foliageDistanceScale`. The cull also
  multiplies every fade range by the client's factor at graphics quality 2
  (1.5, `foliageFadeQuality`), so 0.6667 reproduces the stored ranges.

The foliage fields of the stats line (spec D6 "Stats"): `foliageInstances`
(zone instances in the scene), `foliagePlaced` (those whose model has
arrived), `foliageVisible`, `foliageDraws`, `foliageKtris`, `foliageTiles`
(grid tiles kept by the cull), `foliageCapped` (instances past
`foliageMaxVisible` 20 000), `foliageCullMs` (CPU ms of the cull and
bucketing, last frame), `zoneTrees` (Collider instances), `zoneUnlinked`
(havk zoneModels entries without a model), `shaderFoliage` (foliage drawn in
the Materials look under Shader), `treeRect` (above), and `foliage`
(`off`/`colliders`/`all`), `foliageFadeQuality` (the fade factor above). Also: `set_world_ms` (the UI thread's
`WorldView::set_world`, which takes over the worker-built FoliageGrid and
its draw data),
`foliage_grid_s` (the grid build on the world worker), and
`bench_nofoliage_cpu_ms` / `bench_nofoliage_gpu_ms` (median/max of the same
`GW2_MAPWIN_BENCH` frames with Foliage off; `bench_*_ms` minus these is the
foliage's cost; 0 when Foliage is off).

The dump directory is not created for you; `dumps/mapwin` must already exist.

```bash
GW2_DAT="E:\Games\gw2\Guild Wars 2\Gw2.dat" GW2_DUMP_DIR=dumps/mapwin \
GW2_MAPWIN=192711 GW2_MAPWIN_SHOT=dumps/mapwin/queensdale.bmp \
  ./build/debug/bin/castlemist.exe
```

The struct template is found by climbing from the exe's directory (up to the
repository's `dumps/packfile/`) and from the working directory, so run it from
the repository root or leave the build tree in place.

Loading dominates the run. Measured 2026-10-14, debug build, against the
2026-10-14 client (the stats line in `mapwin.txt`; "total" is world +
collision + all models):

| map | fileId | models loaded/failed | models s | total s | collision tris (incl. terrain) |
| --- | ------ | -------------------- | -------- | ------- | ------------------------------ |
| Queensdale | 192711 | 2146/35 | 155.7 | 162.6 | 2,078,522 |
| Lion's Arch | 191000 | 1651/48 | 174.8 | 182.6 | 4,939,935 |
| Spirit Vale | 1151420 | 380/12 | 60.0 | 65.0 | 2,529,506 |

Lion's Arch was measured before the loader shared one dat (R13); the other two
rows were measured after. Each prop's dat lookup was a linear MFT scan and its
textures were decoded and then discarded, which is why props streamed in slowly.

Sub-project 3 fixed both (fileId lookup is a binary search over the sorted MFT
lists; models are prepared on the workers and textures decoded once, capped at
512 px). Measured 2026-10-15 at the end of sub-project 3, debug build,
Materials shading, one map at a time (Task 16):

| map | fileId | models s | total s | add_models ms median / max | textures | texture MB | chunksUntextured | animated | unresolvedClips |
| --- | ------ | -------- | ------- | -------------------------- | -------- | ---------- | ---------------- | -------- | --------------- |
| Queensdale | 192711 | 8.08 | 28.05 | 0.74 / 13.08 | 1124 | 624 | 0 | 267 | 2 |
| Queensdale, `GW2_MAPWIN_SHADING=geometry` | 192711 | 9.64 | 27.13 | 0.81 / 14.86 | 1124 | 624 | 423 | 267 | 2 |
| Lion's Arch | 191000 | 8.03 | 21.37 | 0.81 / 48.30 | 1301 | 761 | 0 | 294 | 134 |
| Spirit Vale | 1151420 | 3.65 | 16.92 | 0.81 / 9.68 | 547 | 361 | 0 | 106 | 10 |
| Spirit Vale, `GW2_MAPWIN_TIME=2.5` | 1151420 | 4.29 | 17.57 | 0.83 / 33.12 | 547 | 361 | 0 | 106 | 10 |

`texturesFailed` is 0 on every run. Against the targets: Queensdale models in
8.08 s against the sub-project 2 figure of 155.7 s (target: well under), and
its largest `add_models` batch 13.08 ms against 50 ms (pass; Lion's Arch's
48.30 ms and Spirit Vale's 33.12 spike show batch time is noisy, and nothing
was changed to chase it). Spirit Vale's `animRect` differs between time 0 and
time 2.5 in 10,431 pixels (by more than 8/255), so the props really move.

Two results to read with care. The plan's luma-variance check (Materials
against Geometry on Queensdale, ratio above 1.5) **failed as a metric**: the
ratio was 0.296 (272.5 against 920.4), because the Geometry look's bright white
props on pink-grey terrain against a dark sky have more luma variance than dark
textured terrain. It says nothing about texturing. "Textured" is shown by
`chunksUntextured=0`, the texture counts and the viewed screenshots. And
Lion's Arch has `unresolvedClips=134`, against 2 and 10 on the other maps; the
cause is not found (an open finding).

GPU texture memory is 620 to 760 MB at the 512 px cap on the two big maps
(361 MB on Spirit Vale). Interactive input (fly, walk, pick, the layer
toggles, the Materials and Motion toggles, the prompt) has only been exercised
by this capture and the unit tests, not driven by hand.

Lighting and tone-mapping knobs (`GW2_LIGHT`, `GW2_POSTEXP`, `GW2_POSTGAMMA`,
`GW2_AUTOEXP`, ...) are for eyeballing a change, not for assertions.

## World export

`gw2dat_cli world-export --out-dir <dir>` writes a map as a cell folder
(`manifest.json`, `cells/*.glb`, `models/`, `textures/`, `sky/`), then, when
Blender is found, one `.fbx` per `.glb` (`--no-fbx` skips that step; `world-fbx
--dir <dir>` runs it alone). Spec:
`docs/superpowers/specs/2026-10-15-world-export-design.md`, with its
amendments at the end.

```bash
ctest --preset debug -L blender                 # the Blender ctests (4.1 or newer)
ctest --preset debug -R "world_manifest|world_fixture"   # the Python ones, and the fixture
python tools/world/test_validate_manifest.py    # the validator's own tests
python tools/world/validate_manifest.py <dir> --plan   # schema, files, and the Unity dry-run PLAN line
```

- **Blender ctests** (`blender_probe_*`, `blender_skinned_probe_*`,
  `blender_world_import*`, `blender_world_to_fbx`, `blender_world_fbx_*`) run
  `tools/blender/castlemist_world.py` headless on the synthetic fixture that
  `cm_world_export_fixture` writes. They are skipped when CMake finds no Blender
  4.1 or newer (`$BLENDER`, PATH, then `C:/Program Files/Blender Foundation`).
- **Python ctests** (`world_manifest_validate`, `world_manifest_plan`,
  `world_manifest_selftest`, `world_manifest_unit`) need Python 3. The C++
  fixture test is `cm_test_exportgltf` (`world_*`).
- **Dat test** (`cm_test_dat world_export_dat.`): exports Spirit Vale (1151420)
  and checks it against the `WorldScene` (about 65 s, 1.7 GB, removed after).
  It needs `GW2_TEST_DAT` and `GW2_TEST_TEMPLATE`, and the game must be closed:
  a running Gw2-64.exe locks the dat. `world_export_dat.measure` prints the
  three-map measurements and runs only with `CM_WORLD_EXPORT_MEASURE=1` (about
  10 GB, 5 minutes).
- **Manual checklists.** Unity: `tools/unity/CHECKLIST.md` (items 1 to 3 run in
  batchmode; items 4 to 11 need the editor in a VRChat 2022.3 project with
  Poiyomi Pro 10 or 11). Blender: `castlemist_world.py --import <dir> --verify
  [--save x.blend]`, then a Cycles render at the spawn and the timeline. The
  results, with dates, are in the spec's amendments.
- Keep the output folder path short: Windows image paths over 260 characters
  fail to write.

## World foliage

Zone foliage (spec `docs/superpowers/specs/2026-10-16-world-foliage-design.md`
and its amendments): the `zon2` decode, the scatter rule, the Map Window's
Foliage layer, the export's `foliage/` records and `terrain/` tiles, and the
importers' foliage options. The Map Window hooks (`GW2_MAPWIN_FOLIAGE`,
`GW2_MAPWIN_FOCUS=zonetree|foliage|foliagemax|forest`, `GW2_MAPWIN_FOLIAGE_SCALE`)
and the stats fields are under [The Map window](#the-map-window).

```bash
ctest --preset debug -j1                       # everything; run it serially
ctest --preset debug -R "world_manifest"       # validator, --plan lines, terrain and density plans
ctest --preset debug -R "blender_world_import" # foliage instances, density, colliders
python tools/world/test_validate_manifest.py   # includes fmix32 vectors and the density rules
dotnet run --project tools/unity/Tests~ -- <export dir>   # Unity-free planner and readers
```

- **Unit tests (no dat).** `cm_test_world` (`zones.*`, `scatter.*`: the flag
  walk, anchor link, scatter rule and tiling tables), `cm_test_render`
  (`foliage_grid.*`: grid, cull, fade, batch), `cm_test_ui` (`map_window.*`
  foliage and control-id tests), `cm_test_exportgltf` (`unity_terrain.*`,
  `world_foliage.*`, `world_lods.*`, `world_materials.*`), and the Python and
  Blender ctests above. The synthetic fixture's PLAN lines are in
  `tools/unity/CHECKLIST.md` item 1.
- **Dat tests** need `GW2_TEST_DAT` and `GW2_TEST_TEMPLATE` and a closed game.
  Beside the existing `world_dat.*`: `world_dat.zon2_reads_three_maps`,
  `zones_phase1_acceptance`, `zones_every_collider_has_a_tree`,
  `zones_hull_in_model`, `zones_hypotheses` (prints), `zones_scatter_samples`
  (writes a CSV to `$CM_ZONE_SAMPLES`, skipped without it), and
  `world_export_dat.spirit_vale_foliage` (284 collider trees, records equal
  instances, 32 check points exact, validator OK). `world_export_dat.measure`
  (`CM_WORLD_EXPORT_MEASURE=1`) also prints the foliage, unityTerrain, LOD and
  shared-material lines; the three maps' numbers are in the spec's 2026-10-18
  amendment.
- **Known failures.** `cm_test_dat` has 11 failures from curated ids that
  predate this work. The ripper ctests need `gw2_packfile.json` next to the
  exes in `build/debug/bin`. Check that no stray `dumps/index/gw2_index.db`
  appears (only `.gitkeep` belongs there).
- **Export flags.** `gw2dat_cli world-export --out-dir <dir> [--foliage
  off|colliders|all] [--no-unity-terrain] [--sky-approx] [--no-fbx]`. Records
  and tiles are checked by `python tools/world/validate_manifest.py <dir>
  [--plan --mode mesh|terrain --foliage off|colliders|trees|all
  --foliage-density 0..1 --foliage-distance 0.1..2]`.
- **Unity batchmode** (2022.3.22f1; see `tools/unity/CHECKLIST.md`):
  `-executeMethod Castlemist.BatchEntry.ImportWorld -castlemistWorld
  Assets/<export> [-castlemistTerrain mesh|terrain] [-castlemistFoliage ...]
  [-castlemistDensity d] [-castlemistDistance D] [-castlemistQuest]
  [-castlemistExpectShader "<name>"] [-castlemistScreenshot <png>]`. The log
  carries `PLAN`, `FOLIAGE_TREES`, `FOLIAGE_PATCHES`, `TERRAIN_CHECK`,
  `TERRAIN_FILES`, `TERRAIN_TREES`, `LODS`, `MATERIALS`, `SHADER` and
  `POIYOMI_MATERIALS` lines; `BatchEntry.LockProbe` locks and unlocks a few
  materials with ThryEditor. Items 14 to 18 and the VRChat upload need the
  editor and have not been run.
- **Blender.** `castlemist_world.py --import <dir> --verify [--foliage
  off|colliders|trees|all] [--foliage-density f]` prints `FOLIAGE_OBJECTS`,
  `FOLIAGE_INSTANCES`, `FOLIAGE_KEPT` and `FOLIAGE_MATRIX_MAX_ERR`. Parallel
  Blender runs on one export no longer collide: the temporary files carry the
  process id (`*.castlemist_tmp<pid>.glb`). The visual check (view the scene, one Cycles frame
  at the spawn) has not been run.
- **Scratch paths and disk.** A Spirit Vale export is about 1.7 GB and a full
  Queensdale one more; a Unity project with Poiyomi is 3 to 4 GB. Keep exports
  under a short path and delete them after.

## Adding a test

1. Put it in the suite for the layer that owns the behaviour.
2. If it needs a layer's internal header (`internal.h`, `detail/state.h`,
   `detail/app_state.h`), add the layer to that test's `PRIVATE_HEADERS` in
   `tests/CMakeLists.txt` -- it is already set up for `extract`, `render` and `ui`.
3. Prefer a synthetic fixture built in the test to a captured blob: the
   interesting cases (a 64-bit-pointer packfile, a truncated array, a chunk
   whose declared size runs past the buffer) are hard to find in real data and
   trivial to write by hand.
4. Say **why** in a comment when the assertion encodes a fact about GW2 rather
   than about the code. "Reading a 64-bit blob as 32-bit yields zero-length
   arrays instead of an error" is the reason that test exists.
