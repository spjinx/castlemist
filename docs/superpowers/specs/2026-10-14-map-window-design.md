# Map Window — Design (sub-project 2 of the World Engine)

Date: 2026-10-14
Status: written as a record (the user asked for sub-projects 2–4 to be built in
order without sign-off gates; see the World Data Core spec's roadmap)

## Goal

A separate top-level window that opens any map through `load_world` and lets
you fly over it or walk on it: terrain, props, collision and water drawn from
one `WorldScene`, props streamed in nearest-first, instanced and culled, with
gravity, jumping, sliding and step-up against the map's own collision. Clicking
a prop opens that model in the main window's preview.

## Decisions

- **Its own Direct3D device.** The existing renderer is one set of
  `inline` globals bound to the main window's model surface
  (`src/render/detail/state.h`). The map window does not touch them: a
  `WorldView` object owns its device, swap chain, targets and pipelines. Two
  devices cost a little memory and share nothing, which is what is wanted —
  the map window can be opened, closed and resized without disturbing the
  preview, and the old `MapScene` path keeps working beside it.
- **Geometry look in this sub-project; materials in sub-project 3.** The
  roadmap said "drawn through the Game 1:1 path". Running every prop's game
  shader at map scale *is* sub-project 3's work (terrain blend material, prop
  game shaders, water), so sub-project 2 draws the fly view's look: compact
  16-byte vertices, AO baked per model, hemisphere + sun from the map's own
  `environment.light`. The draw path is built so a material pass can replace
  the shading without changing culling, instancing or streaming.
- **Walk physics is pure and lives in `world`.** Collision queries and the
  character controller take only `WorldScene` data and plain vectors, so they
  are unit-tested without a GPU and can be reused by an exporter or a Unity
  importer test.
- **T3D's `PhysicsController` is the behavioural reference** (user's fork,
  `explorer/src/scene/physics-controller.ts`, commit b3428b2). Its behaviour
  is re-derived, not copied (GPL-3 vs MIT): eye-height ground snap, three
  horizontal probe rays at eye, mid and knee height, per-axis slide when the
  full move is blocked, jump only when grounded, fall-safety back to the last
  safe ground. Its tuning numbers (gravity 2800, jump 900, eye height 120,
  padding 18, snap 6, max fall 8000, probes at 0 / −60 / −108 below the eye,
  all map units) are T3D's *choices*, not dat facts: they are the defaults of
  a `WalkParams` struct, named as T3D-derived in its comment, and adjustable.
  This is a behaviour setting, not a data convention, so it does not break the
  "no invented conventions" rule; the units-per-metre gap stays UNPROVEN.
- **Ground = collision ∪ terrain.** T3D walks on collision meshes only. Here
  the terrain heightfield is also ground (a ray down hits whichever is
  higher), so a map whose `havk` lacks terrain hulls is still walkable.
  Horizontal blocking uses collision only: terrain slopes are climbed by the
  ground snap, as in T3D.
- **Streaming = models nearest-first.** `load_world` is quick; the slow part
  is loading thousands of distinct prop models. The window shows terrain at
  once, then loads models on a worker pool in order of their nearest
  instance's distance to the camera at the time each one is taken, uploading
  batches as they arrive. Terrain is uploaded whole (one small mesh per chunk)
  and culled per chunk.
- **The old path stays.** `MapScene` / `map_fly` remain until sub-project 3
  gives the map window textures; removing them earlier would lose the main
  window's textured map modes.

## Architecture

```
world      walk.h        CollisionIndex (triangles in map space, XY grid),
                         Walker (T3D-style controller), WalkParams
           stream.h      model_load_order(scene, cameraPos)
           terrain_mesh.h  chunk_normals(scene.terrain, chunk) (seam-aware)
render     world_view.h  WorldView: own device/swap chain; set_world,
                         add_models, render(camera), pick, layers, stats
ui         map_window.cpp  top-level window: toolbar, surface, status bar,
                         worker threads, input → Walker / fly camera
```

`render` gains a dependency on `world` (it draws a `WorldScene`). `world`
still depends on nothing above `extract`.

### world/walk.h

- `CollisionIndex::build(const WorldScene&, bool includeTerrain)` — every
  collision instance's triangles transformed to map space (`instance.world`),
  plus two triangles per terrain sample quad when asked, bucketed into a
  uniform XY grid (cell size from the scene extent and triangle count).
- `raycast(origin, dir, maxT, mask) -> optional<Hit{t, point, normal, source}>`
  — nearest hit, walking grid cells along the ray (2D DDA over XY, full 3D
  triangle test). `mask` selects collision and/or terrain.
- `Walker` — state (vertical velocity, grounded, last safe position, fallback
  position) and `step(pos, horizontalDelta, dt) -> StepResult`, `jump()`,
  `snap_to_ground(pos)`, all in map space with up = −Z: "height above" is
  smaller Z. The eye sits `eyeHeight` above the ground point.

### world/stream.h

`std::vector<uint32_t> model_load_order(const WorldScene&, const float cam[3])`
— every model index once, ordered by the squared distance from `cam` to its
nearest instance's translation; ties by index. Models with no instance last.

### world/terrain_mesh.h

`chunk_normals(const Terrain&, size_t chunk) -> std::vector<float>` — one
normal per sample from central differences, reading the neighbouring chunk's
samples across an edge (the deferred item from sub-project 1's amendments), so
adjacent chunks shade without a seam. Normals point up (−Z dominant).

### render/world_view.h

```cpp
class WorldView {
public:
    bool create(HWND surface);              // own device + swap chain
    void resize(int w, int h);
    void set_world(const WorldScene&);      // terrain, collision, water
    void add_models(const std::vector<std::pair<uint32_t,
                    std::shared_ptr<const ModelPreview>>>&);   // streamed
    void render(const WorldCamera&);        // pos, fwd, up, fov, far
    int  pick(int sx, int sy, const WorldCamera&) const;   // prop index or -1
    void set_layer(WorldLayer, bool);
    WorldViewStats stats() const;           // visible, draws, ktris, ms, models loaded
    bool save_screenshot(const char* bmp);
};
```

Internals reuse the fly view's ideas (AO bake, instance stream of world
columns, Gribb–Hartmann frustum, LOD by screen size, distance fog) in its own
translation units; the AO baker and flattener move to a shared internal header
so both paths use one copy. Water: one plane at `water.planeZ` over the map
bounds when `hasPlane`, otherwise none (as stored; UNPROVEN area, §6.2).
Picking: ray from the pixel, sphere test per visible instance, then exact
triangle test on the kept `ModelPreview`.

### ui/map_window.cpp

- Opened from **Tools ▸ Map Window…** (prompts for a map fileId; defaults to
  the selected entry when it is a map) and from a **Map Window** button shown
  for map entries.
- Layout: toolbar (Walk/Fly, Terrain, Props, Collision, Water, Reset), the
  surface, a status line (load stage, models loaded/total, visible/draws/ktris,
  ms, position, mode, speed).
- Threads: one worker runs `load_world` (posts `WM_APP_MAPWIN_WORLD`), then a
  pool loads models in `model_load_order` from the camera position at start,
  posting batches (`WM_APP_MAPWIN_MODELS`); closing the window cancels by a
  generation counter. Each worker opens its own `Gw2Dat` handle as the
  extract path does.
- Input: WASD move, Q/E down/up (fly), Space jump (walk), Shift fast, Ctrl
  slow, right-drag look, wheel speed, Tab walk/fly, left-click pick →
  `navigate_to_file_id(model fileId)` on the main window, F frame map.
- Walk mode is enabled only when the collision index has triangles; entering
  it snaps to the ground under the camera.
- Headless hook: `GW2_MAPWIN=<fileId>` (+ `GW2_MAPWIN_SHOT=<bmp>`) opens the
  window on that map, waits for models, saves a screenshot and a stats line.

## Error handling

A map that fails `load_world` shows its error in the status line and keeps the
window open. Models that fail to load are counted and skipped. Device creation
failure shows a message and closes the window. Warnings from `load_world` are
listed in a read-only panel (toggle).

## Testing

- **Pure (world):** raycast against a hand-built triangle set (hit, miss, grid
  cell crossings, nearest of two); walker on flat ground (settles at eye
  height, stays grounded), against a wall (blocked, slides along it), up a
  step lower than the knee probe (climbs) and higher (blocked), off a ledge
  (falls, lands), jump arc (rises then lands, no double jump), fall-safety
  recovery; `model_load_order` ordering and ties; `chunk_normals` flat → up,
  a slope → the right tilt, seam continuity across two chunks.
- **render:** camera matrices from a basis; ray-from-pixel round trip.
- **ui:** control ids stay unique (existing test, new ids added).
- **dat** (skip without `GW2_TEST_DAT`): on Queensdale, the collision index
  has triangles, and a walker dropped at the map centre lands on ground
  within the map bounds.
- **Headless:** `GW2_MAPWIN=192711` writes a non-black screenshot and a
  stats line with models loaded > 0.

## Done means

- Tools ▸ Map Window opens Queensdale, Lion's Arch and Spirit Vale; terrain
  appears at once, props stream in, frame time stays interactive.
- Walk mode stands on the ground, is stopped by walls, climbs steps, jumps,
  and recovers from a fall-through.
- Layer toggles work; clicking a prop opens its model in the main preview.
- Pure and dat tests pass.

## Out of scope

Textures and game materials (sub-project 3), animated props (3), export (4),
removing the old map path (after 3).

## Amendments (2026-10-14, after implementation)

What shipped differs from the text above in these points. The text above is
left as written; this list wins where they disagree.

- **Ground ray starts at the eye (R6).** `Walker` casts down from the eye, not
  from `eyeHeight` above it, with length `2 * eyeHeight` plus the frame's fall.
  Surfaces above the head are never ground. This deviates from T3D's
  `findGroundY` on purpose: T3D would teleport the walker onto a ceiling.
- **Walker additions.** The ground check is swept (from the higher of the old
  and new eye, so a fast fall cannot skip the floor), runs only while vertical
  velocity is <= 0, and the fall speed is clamped to `maxFall`.
- **Collision upload is lazy (R8).** Collision GPU buffers upload on the first
  Collision-layer enable (or first render with it on), not in `set_world`. The
  CPU copy is released afterwards; `WorldViewStats::collisionMeshes` is 0 until
  then.
- **Water plane without a parm rect (R9).** With `water.hasPlane` but no
  `hasBounds`, the plane covers the scene extent.
- **Model loader (R13).** All model workers share one parsed, read-only
  `Gw2Dat` (the world loader's), instead of one handle each. The pool is
  `max(1, hardware_concurrency - 1)`, uncapped.
- **Framing (R7).** `frame_all`'s extent includes prop translations, so a map
  with no terrain frames its props.
- **Walk speed.** Walk has its own speed (300 units/s to start, wheel-adjusted
  in [20, 20000]), not the fly speed.
- **No toolbar button.** There is no "Map Window" button in the main window.
  Only **Tools > Map Window...** opens it, defaulting to the selected map entry.
- **`WM_APP_MAPWIN_COLLISION`.** An extra message (`WM_APP + 22`) delivers the
  collision index, built off the UI thread after the world arrives, so Walk
  becomes available when it lands.
- **Device recovery.** `WorldView::device_lost()` reports a removed device; the
  window recreates the view and re-adds the loaded models.
- **`pick()` returns the `scene.props` index** (`InstCPU::source`), not a
  per-view instance number.
- **Load-time gap.** Models stream in over 60-175 s per map (total 65-183 s; see docs/testing.md, The Map window). Each lookup is a
  linear MFT scan and textures are decoded then discarded. Recorded as a
  follow-up; sub-project 3's texture cache is the natural fix.
- **Not tried by hand.** Interactive input (fly, walk, pick, layer toggles, the
  prompt) has only been exercised by the headless capture and the unit tests,
  not driven by hand.
