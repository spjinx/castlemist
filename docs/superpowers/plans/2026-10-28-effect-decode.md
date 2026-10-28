# Effect Decode, Complete — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** castlemist reads every MODL and type-183 field the client uses to draw an effect, and the client's per-particle maths exists as tested pure functions.

**Architecture:** Template-driven decode in `native/gw2model.hpp` (as `parseCloudData` does today), CPU mirrors in a new `extract/effect_types.h`, new sidecar keys beside the old ones. A new `fx` layer (depends on `core` only) holds the client's maths, one function per exe routine. Semantics come from static Ghidra reads of Gw2-64.exe, written into `docs/research/gw2-particle-system.md` before each function is coded. Type-183 effect objects get a typed decode in `skillfx`.

**Tech Stack:** C++20 (gcc 16 UCRT64, Ninja, `debug` preset), nlohmann::json, castlemist test framework (`CM_TEST`, `CHECK`, `CHECK_EQ`, `SKIP`), Ghidra 12.1.3 headless (`tools/ghidra/*.java`).

**Spec:** docs/superpowers/specs/2026-10-28-effect-decode-design.md

## Global Constraints

- Gw2.dat only through castlemist tools (`E:\Games\gw2\Guild Wars 2\Gw2.dat`); never read it directly.
- Gw2-64.exe only statically: `analyzeHeadless ... -noanalysis -readOnly`; never launch/attach.
- No unsure names: template field names as-is; unproven meanings exported raw and marked unknown.
- Every semantic claim cites its exe function address (2026-10 build) and, if possible, a real-data case.
- Sidecar `_particles.json`: existing keys keep their meaning; only add keys.
- `render/particles.cpp` is not changed in A.
- Build recipe: `export PATH="/c/msys64/ucrt64/bin:$PATH"; export TMP=$SCRATCH/tmp TEMP=$SCRATCH/tmp; cmake --build build/debug` (worktree configured with `-DCASTLEMIST_EXTERNAL_DIR=<main checkout>/external`).
- Real-dat tests run with `GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat"`; 11 pre-existing `cm_test_dat` failures are known.

Ghidra command (used by every RE step):
```
JAVA_HOME="C:/Program Files/Eclipse Adoptium/jdk-21.0.1.12-hotspot" \
"$GHIDRA/support/analyzeHeadless.bat" 'C:\Users\vital' gw2re -process Gw2-64.exe -noanalysis -readOnly \
  -scriptPath '<worktree>\tools\ghidra' -postScript DecompStringRefs.java OUT.c 8 <substr>...
```
(`$GHIDRA` = `C:/Users/vital/Downloads/ghidra_12.1.3_PUBLIC_20260817/ghidra_12.1.3_PUBLIC`; a new `DecompAt.java` call takes addresses.)

## Review Focus

1. **Old MODL versions** (emitter V≤66, streak/obstacle structs missing in a version): must load with defaults and `present=false`, never garbage. Test: Task 2 curated V≤66 sand-dog 1822155 still reports its 20 emitters and the new V70-only fields as absent.
2. **Pointer fields that are null** (no transform, no meshEmitterSettings, empty curve): defaults, no crash. Test: Task 2 checks an emitter without `meshEmitterSettings` keeps `mesh.present == false`.
3. **Array counts past the file end** (corrupt/odd entries): caps as `parseCloudData` uses (256 emitters, 128 clouds); new arrays get caps too. Test: Task 3 synthetic check that `eachElement` stops at `n_` — covered by reading a real model with 0 streaks (empty, not garbage).
4. **Degenerate maths inputs** (`colorPeriod` 0, `falloff.x == falloff.y`, zero-length direction): no NaN. Test: Task 1 `colour_at` and `spawn_velocity` cases.
5. **Effect objects of a kind not yet decoded**: still listed with raw fields and `undecoded`. Test: Task 10.

---

### Task 1: `fx` layer with the already-proven maths

**Files:**
- Create: `include/castlemist/fx/fx_math.h`, `src/fx/fx_math.cpp`, `tests/test_fx.cpp`
- Modify: `CMakeLists.txt` (after `core`: `castlemist_add_layer(fx DEPS castlemist::core)`), `tests/CMakeLists.txt` (`castlemist_add_test(fx SOURCES test_fx.cpp DEPS castlemist::fx)`)

**Interfaces — Produces** (namespace `castlemist::fx`):
- `struct Range { float base = 0, span = 0; static Range from_min_max(float lo, float hi); float at(float u) const; };` (`at` = base + u·span; the file stores {min,max}, `FUN_140ac8c40`)
- `struct Vec3 { float x = 0, y = 0, z = 0; };`
- `float colour_weight(float life, float period, Range falloff);` (`FUN_140ad3a30`: m = t mod p, n = floor(t/p), x = m/p on even n else 1 − m/p, x = 0 when p = 0; w = clamp((x − lo)/(hi − lo)); `falloff` given as {lo, hi}; hi == lo → step at lo)
- `Vec3 spawn_velocity(const Range dir[3], Range magnitude, const float u[4]);` (normalize(dir sample)·magnitude sample; all-zero direction ranges → {0,0,0})
- `float curve_eval(const std::vector<std::pair<float,float>>& keys, float t, float dflt);` (linear between keys, clamped 0..1 per `FUN_140acbe30`; empty → `dflt`)

- [ ] **Step 1: Write failing tests** in `tests/test_fx.cpp`:
  - `CM_TEST(fx, range_from_min_max)`: `from_min_max(3,1)` → base 3, span −2; `at(0.5)` = 2.
  - `CM_TEST(fx, colour_weight_single_sweep)`: period 1, falloff {0,1}: life 0.25 → 0.25; life 1.0 → 1.0 (m = 0, n = 1 odd → x = 1 − 0 = 1).
  - `CM_TEST(fx, colour_weight_ping_pong)`: period 0.5, life 0.75 → m 0.25, n 1 → x 0.5 → w 0.5.
  - `CM_TEST(fx, colour_weight_degenerate)`: period 0 → 0; falloff {0.5,0.5}, life 0.6 (period 1) → 1, life 0.4 → 0; never NaN.
  - `CM_TEST(fx, spawn_velocity_normalises_direction)`: dir {2,0,0}, magnitude {5,0} → {5,0,0}; all-zero dir → {0,0,0}.
  - `CM_TEST(fx, curve_eval_clamps_and_interpolates)`: keys {(0,0),(1,2)}: t 0.5 → 1 clamped to 1.0; empty keys → dflt.
- [ ] **Step 2:** Build; run `build/debug/bin/cm_test_fx.exe` → link failure / FAIL.
- [ ] **Step 3:** Implement in `src/fx/fx_math.cpp`; each function's comment names its exe function.
- [ ] **Step 4:** `cm_test_fx.exe` → all PASS; `ctest --test-dir build/debug -R "^fx$"` PASS.
- [ ] **Step 5:** Commit `feat(fx): fx layer with the client's proven particle maths`.

### Task 2: Every V70 emitter field, mesh settings, and the census command

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp` (`ParticleEmitter`, `ParticleCloud`, `parseEmitter`, `parseCloudData`)
- Create: `include/castlemist/extract/effect_types.h` (new CPU mirrors, included from `model_types.h` with one `#include`)
- Modify: `include/castlemist/extract/model_types.h` (`ParticleEmitterCPU` gains `ParticleEmitterExtraCPU extra;`, `ParticleCloudCPU` gains `obstacleIndices`), `src/extract/model_preview.cpp` (copy), `src/exportgltf/particle_export.cpp` (new keys), `tools/gw2dat_cli/main.cpp` (`fx-census`)
- Test: `tests/test_extract_dat.cpp`

**Interfaces — Produces:**
- `gw2::ParticleEmitter` new fields: `float accelerationDistRange[2], accelerationDistSpeed[2], velocityDistRange[2], velocityDistSpeed[2], velocityInherit[2]; uint32_t affinity; bool hasV70Extras;` and `MeshEmitter mesh` = `{bool present; uint32_t fileId, alignment; float bankThreshold, bankScale, obstacleRadius; float rotation[3][2], rotationDrag[3], scaleChange[3][2], scaleInitial[3][2], spin[3][2];}` (`hasMesh`/`meshFileId` stay, filled from `mesh`).
- `gw2::ParticleCloud::obstacleIndices` (`std::vector<uint32_t>`).
- `effect_types.h`: `struct MeshEmitterCPU` (same fields as `gw2::MeshEmitter`), `struct ParticleEmitterExtraCPU { float accelerationDistRange[2], accelerationDistSpeed[2], velocityDistRange[2], velocityDistSpeed[2], velocityInherit[2]; uint32_t affinity; bool present; MeshEmitterCPU mesh; };`
- Sidecar emitter keys: `accelerationDistRange`, `accelerationDistSpeed`, `velocityDistRange`, `velocityDistSpeed`, `velocityInherit`, `affinity`, `mesh` (object, absent when no mesh settings); cloud key `obstacleIndices`. Pairs exported `[min, max]` as the existing keys are.
- CLI: `gw2dat_cli fx-census --dat D [--offset N --count N --limit N]` emits `{models, with_clouds, counts: {feature: n}, examples: {feature: [fileId…≤8]}}`; features: `v70_extras`, `velocity_inherit`, `dist_terms`, `mesh_emitter`, `alignment_<k>`, `opacity_preset_<k>`, `scale_preset_<k>`, `curve_type_<k>`, `cloud_flags_<hex>`, `emitter_v66`. Built on `effscan`'s MODL walk (reuse its entry loop).

- [ ] **Step 1:** Add the census (counting only already-parsed fields + `emitter_v66`); run `fx-census --count 400000` against the dat in the background; save output to scratch `census1.json`. Pick: one model with `velocityInherit ≠ {0,0}`, one with `mesh_emitter`, the V≤66 sand dog 1822155.
- [ ] **Step 2: Write failing tests** `CM_TEST(dat, emitter_v70_extras_<id>)` (values read once with `gw2dat_cli model --file-id <id>` after Step 3 — pin those numbers), `CM_TEST(dat, mesh_emitter_settings_<id>)` (fileId + scaleInitial), `CM_TEST(dat, emitter_v66_has_no_v70_extras)` (1822155: 20 emitters, `extra.present == false`, `mesh.present == false`).
- [ ] **Step 3:** Implement the decode (`rdFloat2` for each pair; mesh struct via `ptrTargetStruct`), copy into CPU mirrors, export keys, census features.
- [ ] **Step 4:** Tests PASS; `cm_test_exportgltf` still PASS (old keys unchanged).
- [ ] **Step 5:** Commit `feat(native): every V70 emitter field, mesh emitter settings, fx-census`.

### Task 3: Streaks, obstacles, effect wind

**Files:** `gw2model.hpp` (`Streak`, `StreakAnchor`, `Obstacle`, `EffectWind` in `CloudSystem`; `parseStreakData`, `parseObstacles`, `parseWindData` called from `parseEffects`), `effect_types.h` (`StreakCPU`, `StreakAnchorCPU`, `ObstacleCPU`, `EffectWindCPU`, bones resolved to `boneJoint`), `model_types.h` (`ModelPreview` gains `streaks`, `streakAnchors`, `obstacles`, `effectWinds`; `hasEffects()` also true for streaks), `model_preview.cpp`, `particle_export.cpp` (top-level keys `streaks`, `streakAnchors`, `obstacles`, `effectWinds`), census features `streak`, `obstacle`, `effect_wind`. Test: `tests/test_extract_dat.cpp`.

**Interfaces — Produces:** fields exactly as the template (`ModelStreakV66`: acceleration, velocity, anchorIndices, bone, flags, jitter, materialIndex, noise, spawnDist, texScale, wind; `ModelStreakAnchorV66`: bone, colorStart, colorEnd (packed dwords, as stored), falloff, lifetime, flags, texV; `ModelObstacleDataV66`: affinity, bone, type, response, flags, dragCoef, gravityCoef, length, width, height, radius, transform[12]; `ModelEffectWindV66`: bone, farDistance, intensity, nearDistance). Caps: 256 streaks, 1024 anchors, 128 obstacles, 64 winds.

- [ ] **Step 1:** Census run 2 (features added) → pick one model per feature.
- [ ] **Step 2: Failing tests** `CM_TEST(dat, streaks_<id>)` (count, first streak's anchorIndices and materialIndex), `CM_TEST(dat, obstacles_<id>)`, `CM_TEST(dat, effect_wind_<id>)`, `CM_TEST(dat, model_without_streaks_has_none)` (200170: streaks empty).
- [ ] **Step 3:** Implement with `eachElement`.
- [ ] **Step 4:** PASS. **Step 5:** Commit `feat(native): streaks, obstacles and effect wind`.

### Task 4: Clip effect tracks

**Files:** `gw2model.hpp` (`AnimClip::EffectTrack {uint8_t target; uint64_t bone; uint8_t type; uint32_t trackGroup, vectorTrack; float initial[4];}`, `std::vector<EffectTrack> effectTracks`; read in the clip loop beside `matConstAnim` for `cloudAnim`/`cloudTrackData`, `lightAnimData`/`lightTrackData`, `streakAnim`/`anchorAnim`→`anchorTrackData` (bone = the anchor's), `lightningAnim`/`lightningTrackData`, `windAnimData`), `effect_types.h` (`EffectTrackCPU` + `enum class EffectTrackTarget : uint8_t { Cloud = 0, Light = 1, Streak = 2, Lightning = 3, Wind = 4 }`), `model_types.h` (`ModelPreview::animClipEffectTracks`, parallel to `animClips`), `model_preview.cpp`, `particle_export.cpp` (each sidecar `clips[i]` gains `effectTracks: [{target, bone, type, trackGroup, vectorTrack, initial}]`), census `track_<target>_type_<k>`. Test: `tests/test_extract_dat.cpp`.

- [ ] **Step 1:** Look up `ModelWindAnimationV25`'s fields in the template first (`python tpl.py ... ModelWindAnimation 3`) and read its track list the same way.
- [ ] **Step 2:** Census run 3 → a model with cloud tracks. **Step 3: Failing test** `CM_TEST(dat, cloud_anim_tracks_<id>)` (clip name, track count, first track's type and initial).
- [ ] **Step 4:** Implement. **Step 5:** PASS; commit `feat(native): clip effect tracks (cloud, light, streak, lightning, wind)`.

### Task 5: RE — spawn and motion (dist terms, velocityInherit, drag, obstacles)

**Files:** `docs/research/gw2-particle-system.md` (new section "Amendment 2026-10-28: motion"), `fx_math.h/.cpp`, `tests/test_fx.cpp`.

**Interfaces — Produces:** `Vec3 dist_velocity(Vec3 offsetFromEmitter, Range distRange, Range distSpeed, float u0, float u1);` `Vec3 inherit_velocity(Vec3 emitterVelocity, Range inherit, float u);` `void integrate(ParticleState& p, const MotionParams& m, float dt);` with `struct ParticleState { Vec3 pos, vel; float age = 0, life = 1; };` `struct MotionParams { Vec3 accel, cloudAccel, cloudVelocity; float drag = 0, cloudDrag = 0; };` `bool obstacle_collide(ParticleState& p, const ObstacleShape& o);` (signature fixed only after the read; shape fields from `ObstacleCPU`).

- [ ] **Step 1:** Ghidra: the integrator called from the cloud update (callers of `FUN_140ace900` / the update loop in `GrCloud.cpp`; DecompStringRefs `GrCloud.cpp` 40). Record: drag form (linear `v *= 1 − d·dt` vs exponential `exp(−d·dt)`), where cloud vs emitter drag apply, dist terms, inherit, obstacle response by `type`/`response`.
- [ ] **Step 2:** Write the amendment with addresses.
- [ ] **Step 3: Failing tests** whose expectations are worked by hand from the decompiled formulas (e.g. `CM_TEST(fx, integrate_drag_matches_client)`: v0 10, drag 0.5, dt 0.1 → the value the read formula gives).
- [ ] **Step 4:** Implement; PASS. **Step 5:** Commit `feat(fx): client particle motion (drag, dist terms, inherit, obstacles)`.

### Task 6: RE — sprites: alignment, curve presets, curveType, cloud flags

**Files:** research note ("Amendment 2026-10-28: sprites"), `fx_math.h/.cpp`, `tests/test_fx.cpp`.

**Interfaces — Produces:** `struct SpriteBasis { Vec3 right, up, centre; };` `SpriteBasis sprite_basis(uint8_t alignmentType, Vec3 alignmentDir, Vec3 pos, Vec3 vel, Vec3 cameraPos, Vec3 cameraUp, float rotation, float offset);` `float curve_preset(uint32_t preset, float t);` `float curve_eval_typed(uint8_t curveType, const std::vector<std::pair<float,float>>& keys, float t, float dflt);` `struct CloudDrawFlags { bool known; uint8_t blend; bool sorted, worldSpace; uint32_t unknownBits; };` `CloudDrawFlags decode_cloud_flags(uint32_t flags);`

- [ ] **Step 1:** Ghidra: the vertex builder `FUN_140ad11b0` and its switch on alignment; the baked-curve builder `FUN_140acbe30` and where presets are consumed (if on the GPU: dump the particle AMAT VS with `gw2dat_cli amat --dump-all` and read the preset table); cloud flag tests in the cloud draw setup.
- [ ] **Step 2:** Amendment. Bits/values not proven go to `unknownBits` and the note's "unknown" list.
- [ ] **Step 3: Failing tests**: per alignment type seen in census (`alignment_<k>`), a basis case worked from the read; `curve_preset` at t 0, 0.5, 1 for each preset id seen; `decode_cloud_flags` for each flag value seen in census.
- [ ] **Step 4:** Implement; PASS. **Step 5:** Commit `feat(fx): sprite alignment, curve presets, cloud draw flags`.

### Task 7: RE — mesh-particle emitters

**Files:** research note ("mesh particles"), `fx_math.h/.cpp`, `tests/test_fx.cpp`.

**Interfaces — Produces:** `struct MeshParticleXform { float m[12]; };` `MeshParticleXform mesh_particle_xform(const MeshSpin& s, uint32_t alignment, Vec3 pos, Vec3 vel, float age, const float u[9]);` (`MeshSpin` = rotation/rotationDrag/spin/scaleInitial/scaleChange/bank from `MeshEmitterCPU`).

- [ ] **Step 1:** Ghidra: the mesh-particle path (spawnShape 6 / meshEmitterSettings consumer in GrCloud.cpp). **Step 2:** Amendment. **Step 3:** Failing tests from the read (spin over 1 s, bank at a velocity past `bankThreshold`). **Step 4:** PASS. **Step 5:** Commit `feat(fx): mesh-particle transforms`.

### Task 8: RE — what clip effect tracks drive

**Files:** research note ("clip effect tracks"), `fx_math.h/.cpp` (`enum`-free: `const char* effect_track_meaning(EffectTrackTarget, uint8_t type)` returns the proven parameter name or `nullptr`), `tests/test_fx.cpp`.

- [ ] **Step 1:** Ghidra: the consumers of cloudAnim / lightAnim / streakAnim / lightningAnim / windAnim track types (switch on `type` in the model-instance animation update). **Step 2:** Amendment with a table target × type → parameter (+ how the sampled value combines: multiply, replace, add). **Step 3:** Failing test per proven pair; unproven types return `nullptr`. **Step 4/5:** PASS; commit `feat(fx): clip effect track meanings`.

### Task 9: RE — streak and lightning sims

**Files:** research note (two sections), `include/castlemist/fx/streak.h`, `src/fx/streak.cpp`, `include/castlemist/fx/lightning.h`, `src/fx/lightning.cpp`, `tests/test_fx.cpp`.

**Interfaces — Produces:** `struct StreakPoint { Vec3 pos; float age; float v; };` `void streak_step(std::vector<StreakPoint>& pts, Vec3 anchorPos, const StreakParams& p, float dt, Rng& rng);` `struct BoltSegment { Vec3 a, b; float thickness, opacity, u0, u1; };` `std::vector<BoltSegment> lightning_bolt(const std::vector<Vec3>& nodePath, const BoltParams& p, float age, Rng& rng);` `struct Rng { uint32_t state; float u01(); float s11(); };` — moved into `fx_math.h` here if it did not exist; `Rng` reproduces the client's generator when the read shows it, else xorshift with a note.

- [ ] **Step 1:** Ghidra `GrStreak.cpp`, `GrStreakJobCommon.cpp`, `GrLightning.cpp`, `GrLightningJob.cpp` (DecompStringRefs, 40 each). **Step 2:** Amendments. **Step 3:** Failing tests from the reads (segment count = numSegments, endpoints on the nodes, thickness in range; streak point spacing = spawnDist). **Step 4/5:** PASS; commit `feat(fx): streak and lightning sims`.

### Task 10: Type-183 effect objects — kind mapping and census

**Files:** `include/castlemist/skillfx/effect_def.h`, `src/skillfx/effect_def.cpp`, `tools/gw2dat_cli/main.cpp` (`fx-census --what effects`, `effect-show`), `docs/research/gw2-skill-effects.md` (amendment), `tests/test_skillfx.cpp`.

**Interfaces — Produces:** `enum class EffectClass : uint8_t { Unknown, Compound, Model, Audio, Decal, Material, PostProc, ShakeSpring };` `struct RawField { uint32_t offset; uint32_t value; };` `struct EffectDef { std::array<uint8_t,16> guid; uint32_t uid = 0, kind = 0; std::string codename; EffectClass cls = EffectClass::Unknown; bool decoded = false; std::vector<uint32_t> childUids; uint32_t model = 0, sound = 0; std::vector<RawField> raw; };` `std::optional<EffectDef> read_effect(const format::ContentPack& pack, uint32_t objectOffset);` `EffectClass effect_class_of_kind(uint32_t kind);`

- [ ] **Step 1:** Ghidra: the factory that builds an `EfCliEffect*` from a type-183 object (callers of the type-0xB7 lookups; `DecompBytes.java ... bab7000000`, plus `DecompStringRefs EfCliEffectCompound EfCliEffectModel EfCliEffectDecal EfCliEffectMaterial EfCliEffectPostProc EfCliEffectShakeSpring EfCliEffectAudio`). Record the switch on `kind` → class.
- [ ] **Step 2:** Census of every type-183 object: per kind, per 4-byte offset up to the object's size, distinct-value count and top values; output to scratch, summary into the amendment.
- [ ] **Step 3: Failing tests** (synthetic `PackBuilder` packs as in test_skillfx.cpp): `CM_TEST(skillfx, effect_def_reads_header_and_kind)`, `CM_TEST(skillfx, effect_def_undecoded_kind_keeps_raw_fields)` (an unmapped kind → `decoded == false`, `raw` non-empty), `CM_TEST(skillfx, effect_class_of_kind_matches_factory)` (each proven kind).
- [ ] **Step 4:** Implement; `effect-show --uid N` prints the def; PASS. **Step 5:** Commit `feat(skillfx): type-183 effect defs, kind mapping, effect census`.

### Task 11: Type-183 per-class fields

**Files:** `effect_def.h/.cpp` (per-class structs `CompoundFields{children with offsets/timing as read}`, `ModelFields{model, bone?, scale?, lifetime?}`, `DecalFields`, `MaterialFields`, `PostProcFields`, `ShakeFields` — only fields the readers prove), `src/skillfx/effects.cpp` (`find_effects` returns `EffectDef`s; existing `SkillEffects` fields unchanged), `effect-show`, research amendment, `tests/test_skillfx.cpp`, `tests/test_extract_dat.cpp` (curated: Tome of Justice's effect 1764329's def; one decal; one material effect from the census).

- [ ] **Step 1:** Ghidra each class's content reader (`EfCliEffect<Class>.cpp` asserts). **Step 2:** Amendment per class with offsets and addresses. **Step 3:** Failing synthetic tests per class + curated dat tests. **Step 4:** Implement; existing skillfx tests still PASS. **Step 5:** Commit `feat(skillfx): typed effect fields per EfCli class`.

### Task 12: Close-out

- [ ] Full `ctest --test-dir build/debug` with `GW2_TEST_DAT` set; compare against the 11 known `cm_test_dat` failures; report any new one.
- [ ] Research note: an "Unknown, as of A" list (every field/bit still unexplained).
- [ ] `docs/using-castlemist.md`: `fx-census`, `effect-show`.
- [ ] Commit `docs: effect decode close-out`.
