# Map Window Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A separate top-level window that flies over or walks on any map from one `WorldScene`, with props streamed nearest-first, instanced and culled, and click-to-preview.

**Architecture:** Pure collision/walk/stream/normal code in the `world` layer (unit-tested, no GPU); a self-contained `WorldView` in `render` that owns its own D3D11 device and draws terrain chunks, instanced props, collision and water; a `map_window.cpp` top-level window in `ui` that loads on worker threads and drives the camera and walker.

**Tech Stack:** C++20, MinGW-w64 GCC, CMake + Ninja presets, Direct3D 11, Win32, the repo's own test framework (`tests/framework/test_framework.h`).

**Spec:** `docs/superpowers/specs/2026-10-14-map-window-design.md`

## Global Constraints

- Build: `cmake --build --preset debug` with `C:\msys64\ucrt64\bin` on `PATH`; tests: `ctest --preset debug` or `build/debug/bin/cm_test_<suite>.exe <filter>`.
- Configure (once per worktree): `cmake --preset debug -DCASTLEMIST_EXTERNAL_DIR=C:/Users/vital/Downloads/castlemist/castlemist/external`.
- Map space only: left-handed, +X east, +Y north, up = −Z (`include/castlemist/world/frame.h`). "Above" means smaller Z.
- Layer rule: `world` depends on nothing above `extract`; `render` may now depend on `world`; nothing below `ui` includes a ui header; nothing below `render` mentions Direct3D.
- Do NOT touch the old path's globals (`src/render/detail/state.h`, `detail/map_fly.h` state) from `WorldView`; it owns its own device.
- T3D is GPL-3, castlemist MIT: re-derive behaviour, cite `explorer/src/scene/physics-controller.ts` (commit b3428b2) in comments, never copy code.
- `WalkParams` defaults (T3D's choices, map units): gravity 2800, jumpVelocity 900, eyeHeight 120, padding 18, groundSnap 6, maxFall 8000, probe offsets below the eye {0, 60, 108}.
- Never read `Gw2.dat` directly; dat tests go through `load_world` and skip without `GW2_TEST_DAT`.
- Doc comments in the repo's style (`/// @brief`, `/// @file`, cite research-note sections).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

- A map with no collision triangles: Walk must be disabled (button greyed), never a walker that falls forever. → Task 2 test `walker_no_geometry_never_grounded_and_reports`, Task 8 UI gating.
- A walker spawned below the terrain (camera under the map): snap-to-ground must use the highest surface under the XY, not the first hit from the eye. → Task 2 test `snap_from_below_finds_top_surface`.
- Window closed while models are still loading: late worker posts must be dropped (generation check), no use-after-free. → Task 8 step on cancel + headless close test.
- Models whose `ModelPreview` has no meshes or NaN bounds: skipped and counted, never a crash or a frame-wide NaN. → Task 6 test `world_view_skips_empty_and_nan_models` (CPU-side prep function).
- A huge `dt` after a stall (window dragged): physics must clamp dt (≤ 0.1 s) so the walker does not tunnel through floors. → Task 2 test `large_dt_is_clamped_no_tunnel`.

---

### Task 1: CollisionIndex and raycast

**Files:**
- Create: `include/castlemist/world/walk.h`, `src/world/walk.cpp`
- Test: `tests/test_world_walk.cpp` (add to the `world` suite's SOURCES in `tests/CMakeLists.txt`)

**Interfaces:**
- Produces:
  ```cpp
  namespace castlemist::world {
  enum class HitSource : uint8_t { Collision = 1, Terrain = 2 };
  enum HitMask : uint8_t { kHitCollision = 1, kHitTerrain = 2, kHitAll = 3 };
  struct RayHit { float t = 0; float point[3]{}; float normal[3]{}; HitSource source = HitSource::Collision; };
  class CollisionIndex {
  public:
      static CollisionIndex build(const WorldScene& scene, bool includeTerrain);
      /// For tests and tools: build from raw map-space triangles (9 floats each).
      static CollisionIndex from_triangles(const std::vector<float>& collisionTris,
                                           const std::vector<float>& terrainTris);
      std::optional<RayHit> raycast(const float origin[3], const float dir[3], float maxT,
                                    uint8_t mask = kHitAll) const;   // dir need not be unit; t in units of |dir|=1 after normalising
      size_t triangle_count(uint8_t mask = kHitAll) const;
      bool empty() const;
      void bounds(float lo[3], float hi[3]) const;
  };
  }
  ```
- Triangles stored in map space. `build` transforms each `CollisionInstance` mesh by `instance.world` (column-major, `p' = M p`) and, if `includeTerrain`, emits two triangles per sample quad from `TerrainChunk::heights` using the sample-position rule in `world_scene.h`.
- Grid: uniform XY cells; cell size = max(extent_x, extent_y) / clamp(sqrt(triCount/4), 1, 1024). A triangle is put in every cell its XY AABB overlaps. Ray walks cells with a 2D DDA (Amanatides–Woo) over XY, testing triangles (Möller–Trumbore, both faces) and stopping at the first cell whose entry t exceeds the best hit. A ray with zero XY direction tests just its cell. Normal returned is the geometric normal flipped to face the ray origin.

- [ ] **Step 1: Write failing tests** in `tests/test_world_walk.cpp`:
  - `walk.raycast_hits_floor`: one floor quad (two tris) at z=0 spanning x,y∈[-100,100]; ray from (0,0,-50) dir (0,0,1) maxT 1000 → hit, `t≈50`, point z≈0, normal z<0 (faces origin above).
  - `walk.raycast_misses_outside`: same floor, ray at (500,0,-50) down → no hit.
  - `walk.raycast_nearest_of_two`: floors at z=0 and z=-20, ray from z=-50 down → t≈30.
  - `walk.raycast_crosses_cells`: 400 small floor tris on a 20×20 grid over [-1000,1000]² at z=0 plus one wall quad at x=900 (vertical, y∈[-1000,1000], z∈[-200,0]); horizontal ray from (-900,0,-100) dir (1,0,0) maxT 5000 → t≈1800.
  - `walk.raycast_mask`: a collision floor at z=0 and a terrain floor at z=-10; mask `kHitCollision` from z=-50 down → t≈50; `kHitTerrain` → t≈40.
  - `walk.build_from_scene_transforms_instances`: a `WorldScene` with one `CollisionMesh` (unit tri) and one instance whose `world` translates by (1000,0,0) → `triangle_count(kHitCollision)==1` and a ray down at x=1000.2,y=0.2 hits.
  - `walk.build_includes_terrain`: a scene with one 3×3-sample chunk, rect (0,0,200,200), heights all 5 → `triangle_count(kHitTerrain)==8`, ray down from (50,50,-100) hits at z≈5.
- [ ] **Step 2: Run** `build/debug/bin/cm_test_world.exe walk.` → FAIL (does not compile: walk.h missing).
- [ ] **Step 3: Implement** `walk.h` / `walk.cpp` (CollisionIndex only) as specified.
- [ ] **Step 4: Run** `cm_test_world.exe walk.` → all PASS; `ctest --preset debug -R world$` passes.
- [ ] **Step 5: Commit** `feat(world): CollisionIndex: map-space triangles in an XY grid, raycast`.

### Task 2: Walker (T3D-style character controller)

**Files:**
- Modify: `include/castlemist/world/walk.h`, `src/world/walk.cpp`
- Test: `tests/test_world_walk.cpp`

**Interfaces:**
- Consumes: `CollisionIndex::raycast` (Task 1).
- Produces:
  ```cpp
  struct WalkParams {   // defaults: T3D's PhysicsController choices (see Global Constraints)
      float gravity = 2800, jumpVelocity = 900, eyeHeight = 120, padding = 18,
            groundSnap = 6, maxFall = 8000;
      float probeBelowEye[3] = {0, 60, 108};
      float maxDt = 0.1f;
  };
  struct StepResult { bool grounded = false; bool recovered = false; bool gaveUp = false; };
  class Walker {
  public:
      explicit Walker(const CollisionIndex* index, WalkParams p = {});
      void reset(const float pos[3]);                 // fallback = pos; clears motion
      bool snap_to_ground(float pos[3]);              // highest surface under XY; eye = ground - eyeHeight (z)
      void jump();                                    // request; honoured only when grounded
      StepResult step(float pos[3], const float horizontalDelta[2], float dt);
      bool grounded() const;
      float vertical_velocity() const;                // positive = moving up (toward -Z)
  };
  ```
- Behaviour (re-derived from T3D, adapted to up = −Z): horizontal move tries the full delta, else X then Y separately; a move is blocked if any of the three probe rays (from eye, eye−60, eye−108 *below the eye*, i.e. z + offset) along the move direction hits collision (mask `kHitCollision`) within `distance + padding`. Ground: ray from `eyeHeight` above the eye straight down, maxT `3*eyeHeight`, mask `kHitAll`; grounded when the gap between eye and `ground − eyeHeight` is ≤ `groundSnap` (snap up if below; zero downward velocity; remember safe position). Jump sets vertical velocity to `jumpVelocity` when grounded. Gravity integrates with dt clamped to `maxDt`. Fall safety: when not grounded and the eye is more than `maxFall` below the last safe (or fallback) position, return to it; if still not grounded there, `gaveUp = true`. `snap_to_ground` casts from z = `bounds lo z − 1` (above everything) down through the whole index and takes the first hit — that is the highest surface.

- [ ] **Step 1: Write failing tests:**
  - `walk.walker_settles_on_flat_ground`: floor z=0; reset at (0,0,-300); step 120 frames of dt 1/60 with no move → `grounded`, eye z ≈ −120 (±0.5).
  - `walk.walker_blocked_by_wall_and_slides`: floor + wall plane x=100 (y∈[-1000,1000], z∈[-400,0]); grounded at (0,0,-120); repeatedly step with delta (10,10) → final x < 100−18+0.5 and y increased by > 200 (slid along Y).
  - `walk.walker_climbs_low_step`: floor z=0; a step of height 10 from x=50 onward (top quad at z=−10, vertical face at x=50 spanning z∈[−10,0]); grounded at (0,0,−120); delta (5,0) for 60 frames → x > 100 and eye z ≈ −130. (The lowest probe sits at eye+108 = z −12, above the face, so the move is not blocked and the ground snap lifts the walker.)
  - `walk.walker_blocked_by_high_step`: same with height 40 (face z∈[−40,0], which the z −12 probe hits) → x stays < 50.
  - `walk.walker_falls_off_ledge_and_lands`: high floor z=−500 for x<0, low floor z=0 for x≥0; start grounded at (−10,0,−620); move +x → eventually grounded with eye z ≈ −120.
  - `walk.jump_rises_then_lands_no_double_jump`: grounded; `jump()`, step → vertical_velocity > 0 and eye z decreasing (rising); call `jump()` mid-air for several frames → peak height above ground ≤ jumpVelocity²/(2·gravity) + 1 (≈144.6); lands grounded again.
  - `walk.fall_safety_returns_to_safe_ground`: floor only for x∈[-100,100]; grounded at origin; teleport pos to x=500 (no ground), step until result.recovered → pos back at the last safe position, grounded.
  - `walk.walker_no_geometry_never_grounded_and_reports`: empty index; `snap_to_ground` → false; step for 2 s → never grounded; `gaveUp` true once the fall exceeds maxFall from the fallback.
  - `walk.snap_from_below_finds_top_surface`: floors at z=0 and z=−300 (both span the XY); pos (0,0,+500) (below both) → `snap_to_ground` true, eye z ≈ −420.
  - `walk.large_dt_is_clamped_no_tunnel`: floor z=0 thin quad; eye at −120 grounded with vertical velocity forced by stepping off nothing… simpler: reset at (0,0,−2000), one step with dt = 5.0 → eye z ≤ −120+0.5 is still above the floor (z < 0) and `grounded` only if reached; then normal steps → grounded at −120 (never below the floor, z never > 0).
- [ ] **Step 2: Run** `cm_test_world.exe walk.walker` etc. → FAIL.
- [ ] **Step 3: Implement** `Walker` in `walk.cpp`.
- [ ] **Step 4: Run** `cm_test_world.exe walk.` → all PASS.
- [ ] **Step 5: Commit** `feat(world): Walker: gravity, jump, slide, step-up, fall safety (T3D behaviour)`.

### Task 3: Stream order and seam-aware terrain normals

**Files:**
- Create: `include/castlemist/world/stream.h`, `src/world/stream.cpp`, `include/castlemist/world/terrain_mesh.h`, `src/world/terrain_mesh.cpp`
- Test: `tests/test_world_stream.cpp` (add to the `world` suite)

**Interfaces:**
- Produces:
  ```cpp
  std::vector<uint32_t> model_load_order(const WorldScene& s, const float cam[3]);
  /// One unit normal per sample (3 floats each, samples*samples*3), map space, up-dominant (n.z < 0 on flat ground).
  std::vector<float> chunk_normals(const Terrain& t, size_t chunkIndex);
  /// The chunk at grid (cx, cy), or nullptr.
  const TerrainChunk* find_chunk(const Terrain& t, int cx, int cy);
  ```
- `model_load_order`: instance translation is `world[12..14]`; key = min squared distance per model; ties → lower index; models with no instance appended in index order.
- `chunk_normals`: central differences over the sample grid in map space (dx along +X between columns, dy along −Y between rows since row 0 is north); at an edge, use the neighbour chunk's sample one step in (neighbour shares the edge sample, §3 overlap — so step two samples in on the neighbour is wrong; the neighbour's edge sample equals ours, take its next one); without a neighbour, one-sided difference. Normal = normalize(cross(tangentX, tangentY)) oriented so z < 0 on flat ground.

- [ ] **Step 1: Write failing tests:**
  - `stream.order_by_nearest_instance`: 3 models; model 0 instance at x=1000, model 1 at x=10 and x=5000, model 2 none; cam origin → `{1, 0, 2}`.
  - `stream.ties_by_index`: two models at equal distance → lower index first.
  - `stream.empty_scene`: → empty vector.
  - `terrain.normals_flat_point_up`: one 4×4 chunk, heights 0 → every normal ≈ (0,0,−1).
  - `terrain.normals_slope_tilts`: heights z = 0.5·x (rising toward −Z = up as x decreases… z grows with x means the ground goes down eastward) → normal x component < 0? Compute: surface z = 0.5x, up = −Z; normal ∝ (0.5, 0, −1) normalised → expect n.x ≈ 0.447, n.z ≈ −0.894.
  - `terrain.normals_seam_continuous`: two chunks side by side (cx 0,1) sampling one continuous function z = 0.01·x²; the normal at chunk 0's east edge equals chunk 1's west edge normal within 1e-4.
- [ ] **Step 2: Run** `cm_test_world.exe stream.` / `terrain.normals` → FAIL.
- [ ] **Step 3: Implement** both files.
- [ ] **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(world): model_load_order and seam-aware chunk_normals`.

### Task 4: Dat test — walk on Queensdale

**Files:**
- Modify: `tests/test_world_dat.cpp`

**Interfaces:**
- Consumes: `load_world`, `CollisionIndex::build(scene, true)`, `Walker`.

- [ ] **Step 1: Add test** `world_dat.queensdale_walker_lands` (reuse the file's existing dat/template loading helpers and SKIP pattern): load 192711; build index with terrain; assert `triangle_count(kHitCollision) > 0`; place at the bounds centre with z = bounds lo z − 1000 (or terrain min − 1000); `snap_to_ground` true; then 300 steps of 1/60 with no move → grounded; eye XY inside `bounds`.
- [ ] **Step 2: Run** `GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" build/debug/bin/cm_test_world_dat.exe queensdale_walker` → PASS (and SKIP without the env var).
- [ ] **Step 3: Commit** `test(world): a walker lands on Queensdale's collision`.

### Task 5: WorldView — device, camera, terrain

**Files:**
- Create: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`, `src/render/world_view_shaders.h` (HLSL strings), `src/render/detail/mesh_bake.h` + `src/render/mesh_bake.cpp` (the AO baker + flattener moved out of `map_fly.cpp`, which then includes it)
- Modify: `CMakeLists.txt` (render DEPS add `castlemist::world`), `src/render/map_fly.cpp` (use mesh_bake)
- Test: `tests/test_render_world_view.cpp` (add to the `render` suite)

**Interfaces:**
- Consumes: `chunk_normals` (Task 3), `WorldScene`.
- Produces:
  ```cpp
  namespace castlemist::render {
  enum class WorldLayer { Terrain = 0, Props, Collision, Water, Count };
  struct WorldCamera { float pos[3]{}; float fwd[3]{1,0,0}; float up[3]{0,0,-1}; float fovY = 1.15f; float farDist = 40000; };
  struct WorldViewStats { int visible = 0, draws = 0, ktris = 0; float ms = 0; int modelsLoaded = 0, modelsFailed = 0, chunks = 0; };
  /// Row-vector view*proj for a camera (near = clamp(far*5e-5, 0.05, 20)); pure, tested.
  Mat4 world_camera_viewproj(const WorldCamera& c, float aspect);   // Mat4 from detail/math.h — expose via a small public `world_math.h` or keep in a header the test can include via PRIVATE_HEADERS
  /// Ray through pixel (sx, sy) of a w×h surface: origin = camera pos, unit dir.
  void world_camera_ray(const WorldCamera& c, int w, int h, int sx, int sy, float origin[3], float dir[3]);
  class WorldView {
  public:
      WorldView(); ~WorldView();
      bool create(HWND surface);            // own D3D11 device + swap chain (hardware, WARP fallback)
      void resize(int w, int h);
      void set_world(const castlemist::world::WorldScene& s);   // terrain now; collision/water in Task 6
      void render(const WorldCamera& cam);
      void set_layer(WorldLayer l, bool on); bool layer(WorldLayer l) const;
      WorldViewStats stats() const;
      bool save_screenshot(const char* bmpPath);   // 24-bit BMP of the last frame
      /// Camera that frames the whole map from above (pos, fwd, speed hint).
      WorldCamera frame_all(float* speedHint) const;
  private: struct Impl; std::unique_ptr<Impl> d;
  };
  }
  ```
- Terrain: per chunk one immutable VB of `{pos float3, normal R8G8B8A8 (n*0.5+0.5), ao=255}` (the fly `MapVtx` layout) and a shared index buffer per sample count; frustum-culled per chunk (bounding sphere from rect + height range). Shading: the fly shader's hemisphere + sun + fog, sun/fill from `scene.environment.light` (fall back to the fly defaults when `!present`), terrain tint `{0.52,0.56,0.46}`.

- [ ] **Step 1: Write failing tests** (`render` suite, pure maths only):
  - `world_view.viewproj_centre_projects_to_middle`: camera at origin looking +X, up −Z; point (100,0,0) → NDC x≈0, y≈0, 0<z<1.
  - `world_view.up_is_screen_up`: point (100,0,−10) (above) → NDC y > 0.
  - `world_view.pixel_ray_round_trip`: ray through pixel (w/2, h/2) equals fwd; ray through (w−1, h/2) has positive dot with camera right (= cross(up, fwd)).
- [ ] **Step 2: Run** `cm_test_render.exe world_view.` → FAIL.
- [ ] **Step 3: Implement** the maths, `mesh_bake` move (map_fly behaviour unchanged), and `WorldView` device + terrain draw + screenshot.
- [ ] **Step 4: Run** `cm_test_render.exe` (all) → PASS; full build passes.
- [ ] **Step 5: Commit** `feat(render): WorldView: own device, map camera, terrain chunks`.

### Task 6: WorldView — streamed instanced props, collision, water

**Files:**
- Modify: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`
- Create: `src/render/world_props.h`, `src/render/world_props.cpp` (CPU-side prep, testable)
- Test: `tests/test_render_world_view.cpp`

**Interfaces:**
- Consumes: `mesh_bake` flatten/AO (Task 5).
- Produces:
  ```cpp
  // world_view.h
  void WorldView::add_models(const std::vector<std::pair<uint32_t, std::shared_ptr<const ModelPreview>>>& batch);
  // world_props.h (internal)
  struct PreparedModel { std::vector<MapVtx> verts; std::vector<uint32_t> indices;
                         std::vector<std::pair<uint32_t,uint32_t>> lodRanges; float center[3]; float radius; bool ok; };
  PreparedModel prepare_world_model(const ModelPreview& m);   // flatten + AO; ok=false for no meshes / non-finite bounds
  ```
- `set_world` records every `PropInstance` (world matrix → row-vector `Mat4`, world-space sphere from the model's centre/radius × max column length once its model arrives); instances of not-yet-loaded models are skipped at draw time. `add_models` runs `prepare_world_model` across cores (the `parallel_for` in mesh_bake) then creates buffers serially. Draw: distance cull, frustum cull, LOD by screen size, bucket by (model, lod), one dynamic instance buffer, one `DrawIndexedInstanced` per bucket (as `render_fly`). Collision layer: each `CollisionMesh` uploaded once, instances drawn with tint `{0.90,0.42,0.36}`, off by default. Water: one quad over `bounds` at `planeZ` when `water.hasPlane`, drawn last with alpha 0.45, tint `{0.30,0.55,0.85}`. Stats filled every frame.

- [ ] **Step 1: Write failing tests:**
  - `world_view.prepare_cube_ok`: a 1-mesh cube ModelPreview → `ok`, 1+ LOD ranges, every AO byte in [76,255] (0.30 floor).
  - `world_view_skips_empty_and_nan_models`: no meshes → `ok=false`; radius NaN → `ok=false`.
- [ ] **Step 2: Run** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_render.exe` → PASS.
- [ ] **Step 5: Commit** `feat(render): WorldView streams instanced props; collision and water layers`.

### Task 7: WorldView — picking

**Files:**
- Modify: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`, `src/render/world_props.*`
- Test: `tests/test_render_world_view.cpp`

**Interfaces:**
- Produces: `int WorldView::pick(int sx, int sy, const WorldCamera& cam) const;` → `PropInstance` index or −1; and the pure helper `float ray_model_hit(const ModelPreview& m, const float worldColMajor[16], const float o[3], const float d[3])` (returns t or −1; tests LOD0 triangles in model space after inverting the world matrix).
- Visible-instance sphere test first, nearest exact hit wins; terrain does not block picks (props under terrain are rare and picking them is useful).

- [ ] **Step 1: Write failing tests:** `world_view.ray_model_hit_translated_cube` (cube at x=100 → ray from origin +X hits t≈99.5 for a unit cube); `world_view.ray_model_miss` → −1.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** → PASS.
- [ ] **Step 5: Commit** `feat(render): WorldView picks the prop under a pixel`.

### Task 8: Map window — window, loading, fly, walk, pick

**Files:**
- Create: `src/ui/map_window.cpp`, `src/ui/detail/map_window.h` (`void open_map_window(HWND owner, uint32_t mapFileId);` and the pure `MapWinInput` key mapping)
- Modify: `src/ui/detail/app_state.h` (`ID_TOOLS_MAP_WINDOW = 1022`, `WM_APP_MAPWIN_WORLD = WM_APP + 20`, `WM_APP_MAPWIN_MODELS = WM_APP + 21`, control ids for the toolbar in a free range checked by the id test), `src/ui/window_proc.cpp` (Tools menu "&Map Window..." → prompt for fileId, default = selected map entry's fileId; handler), `src/ui/application.cpp` (headless `GW2_MAPWIN` hook), `tests/test_ui_layout.cpp` or whichever file holds the id-uniqueness test (add the new ids)
- Test: `tests/test_ui_map_window.cpp` (add to the `ui` suite)

**Interfaces:**
- Consumes: `load_world`, `model_load_order`, `CollisionIndex`, `Walker`, `WorldView` (all above), `load_model_by_fileid(dat, id, /*want_game=*/false)`, `navigate_to_file_id` (`src/ui/chat_link_dialog.cpp`), the template via `castlemist::tpl::get_or_auto_load()` (as preview.cpp uses).
- Produces: `open_map_window(HWND owner, uint32_t mapFileId)`.
- Pure helper for tests: `struct FlyState { float pos[3], fwd[3], up[3]; float speed; };` and `void fly_step(FlyState&, const bool keys[8], float dt)` / `void fly_mouse(FlyState&, float dx, float dy)` — the fly camera maths (same rules as `src/render/flycam.cpp`: rotate about own axes, Q/E on world up −Z, dt clamp 0.1, Shift ×6, Ctrl ×0.15), so the window does not touch the old fly globals.

Behaviour (spec §ui/map_window.cpp): toolbar buttons Walk/Fly (push-like; Walk disabled when the collision index is empty), Terrain, Props, Collision, Water (check), Reset (frame_all), Warnings (toggles a read-only edit listing `scene.warnings`); status line updated ~4×/s. One loader thread: opens its own `Gw2Dat` on the main app's dat path, `load_world`, posts the scene (heap `shared_ptr<WorldScene>` + generation); main thread calls `set_world`, builds `CollisionIndex` on a worker then enables Walk; then a pool (hardware_concurrency − 1, min 1) loads models in `model_load_order(scene, camPos)`, each worker with its own `Gw2Dat`, posting batches of up to 32 or every 250 ms; each post carries the window generation; `WM_DESTROY` bumps the generation and late posts are freed and dropped. Render on a 16 ms timer only while the window is visible and something moved or models arrived. Input per spec (WASD, Q/E, Space, Shift, Ctrl, right-drag look, wheel speed, Tab walk/fly, left-click pick → `navigate_to_file_id(scene.models[props[i].model].fileId)`, F frame). In Walk the horizontal delta is the fly forward/right projected on XY, scaled by speed·dt; `Walker::step`; `gaveUp` drops back to Fly with a status message.

Headless: `GW2_MAPWIN=<fileId>` opens the window on start (after the dat auto-loads, as other hooks do), waits until all models are loaded or failed (max 600 s), renders one frame at 1280×720, writes `GW2_MAPWIN_SHOT` (default `dump_path("shot_mapwin.bmp")`) and one stats line to `dump_path("mapwin.txt")`, then closes the app.

- [ ] **Step 1: Write failing tests** (`ui` suite): `map_window.fly_forward_moves_along_fwd` (keys[FWD], dt 0.5, speed 100 → pos += fwd·50); `map_window.fly_up_is_world_up` (rolled camera, Q/E up key → pos.z decreases by speed·dt); `map_window.fly_dt_clamped` (dt 5 → moves as dt 0.1); `map_window.fly_mouse_pitch_past_vertical_no_flip` (pitch +200° in steps; fwd stays unit, up ⟂ fwd); new ids unique (extend the existing id test's table).
- [ ] **Step 2: Run** `cm_test_ui.exe map_window.` → FAIL.
- [ ] **Step 3: Implement** `map_window.cpp`, menu, ids, headless hook.
- [ ] **Step 4: Run** `cm_test_ui.exe` → PASS; full `ctest --preset debug` → PASS.
- [ ] **Step 5: Headless check:** `GW2_MAPWIN=192711 build/debug/bin/castlemist.exe` (dat auto-load as the app normally does) → `mapwin.txt` reports modelsLoaded > 0 and the BMP is not uniformly one colour (check with a tiny script: sample pixel variance > 0). Repeat for 191000 and 1151420.
- [ ] **Step 6: Commit** `feat(ui): Map Window: fly and walk a map from WorldScene, streamed props, pick to preview`.

### Task 9: Docs

**Files:**
- Modify: `docs/architecture.md` (render: world_view; ui: map_window; world: walk/stream/terrain_mesh; `render` → `world` edge), `docs/using-castlemist.md` (a "Map Window" section: opening, controls, walk vs fly, layers, picking), `docs/testing.md` (`GW2_MAPWIN`, `GW2_MAPWIN_SHOT`, the new suites), `docs/superpowers/specs/2026-10-14-map-window-design.md` (an "Amendments" section listing what shipped differently, if anything), and the world-data-core spec's roadmap line for 2 ("see 2026-10-14-map-window-design.md").
- Also `src/core` env-hook table if `GW2_*` hooks are registered there (`castlemist::core::env` — check `include/castlemist/core/env.h`; add `GW2_MAPWIN`, `GW2_MAPWIN_SHOT` there if hooks must be listed).

- [ ] **Step 1:** Write the docs. **Step 2:** `ctest --preset debug` → PASS (env-table test, if any). **Step 3: Commit** `docs: map window`.
