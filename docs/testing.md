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
| `dat` | End-to-end extraction against a real `Gw2.dat` |

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
`add_ms_max`). If the window is closed before the capture, the line says so
with `ok=0`. The window is a normal desktop window: input given to it during the
run (a key press, a drag) moves the camera that the capture uses. `GW2_MAPWIN_SHOT=<bmp>` picks where the frame
goes (default `shot_mapwin.bmp` in the dump directory).

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
rows were measured after. Each prop's dat lookup is a linear MFT scan and its
textures are decoded and then discarded, so props stream in slowly; sub-project
3's texture cache is the natural fix. Interactive input (fly, walk, pick, the
layer toggles, the prompt) has only been exercised by this capture and the unit
tests, not driven by hand.

Lighting and tone-mapping knobs (`GW2_LIGHT`, `GW2_POSTEXP`, `GW2_POSTGAMMA`,
`GW2_AUTOEXP`, ...) are for eyeballing a change, not for assertions.

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
