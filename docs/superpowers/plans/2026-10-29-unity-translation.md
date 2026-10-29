# Measured Unity Translation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unity imports of GW2 effects use the client's proven maths, and a Measure tool compares each port against castlemist's reference statistics.

**Architecture:** C++: the model export writes `<fileId>_fxstats.json` (fxsim, seed 1, 4 s at 1/60). C#: `ParticleEffects.cs` swaps its stand-ins for the client's curves/flags/colour/alignment/motion; a new `ParticleMeasure.cs` simulates the built systems in edit mode and compares with `_fxstats.json`. C# maths that must match C++ lives in one static class (`GW2ParticleMath.cs`) with EditMode tests.

**Tech Stack:** C++20 (exportgltf, fxsim); Unity 2022.3 C# (Shuriken), Unity Test Framework EditMode tests under `Tests~`.

**Spec:** docs/superpowers/specs/2026-10-29-unity-translation-design.md

## Global Constraints

- Sidecar `_particles.json` keys unchanged; `_fxstats.json` is a new file.
- GW2 → Unity units/axes as the importer already converts (`Conv`, inches → metres by the map scale).
- No changes in the user's live Unity project without their go-ahead (deployment is its own step).

## Review Focus

1. A model with no effects: no `_fxstats.json`, no crash. Test in Task 1.
2. Preset ids past 17: constant 1 in C# as in C++. Test in Task 2.
3. Colour period 0 / falloff lo == hi: no NaN in the gradient. Test in Task 2.
4. Emitters Shuriken cannot align (case 0/2/3 without a mesh quad): warned, billboard. Task 3.
5. Measure on a selection with no GW2 FX systems: a message, no exception. Task 4.

---

### Task 1: `_fxstats.json` beside every model export

**Files:** `src/exportgltf/particle_export.cpp` (or model_export.cpp after the sidecar), `src/exportgltf/CMakeLists` deps (exportgltf gains `castlemist::fxsim` — check for a cycle: fxsim depends on extract only), `tests/test_exportgltf_writer.cpp`.

- [ ] Failing test `exportgltf.fxstats_written_beside_the_sidecar` (make_quad_with_effects: file exists, `live` has 240 entries, emitter count matches); `no_particle_sidecar_without_baked_effects` also asserts no `_fxstats.json`.
- [ ] Implement `write_fxstats(model, glbPath)`: `fxsim::Sim(make_input(model), 1, true)`, 240 steps, `stats_json` + `{seed, dt, seconds, approximations}`. PASS; commit `feat(exportgltf): reference particle statistics beside every model export`.

### Task 2: `GW2ParticleMath.cs` — the client's curves and colour

**Files:** `tools/unity/Castlemist/Editor/GW2ParticleMath.cs`, `tools/unity/Castlemist/Tests~/GW2ParticleMathTests.cs`.

**Produces:** `static float CurvePreset(int id, float t)`, `static float ScalePreset(int id, float t, uint fileFlags)`, `static float OpacityPreset(int id, float t, uint fileFlags)`, `static uint RuntimeFlags(uint fileFlags)`, `static float ColourWeight(float life, float period, Vector2 falloff)` (moved from ParticleEffects), `static int AlignmentCase(int fileType)`, `static AnimationCurve CurveOf(int preset, uint flags, bool scale, int samples = 33)`.

- [ ] EditMode tests with the same hand values as tests/test_fx.cpp (presets at t 0.5 and 1, flags, id 99 → 1, colour weight degenerate cases). Implement; commit `feat(unity): GW2ParticleMath, the client's curves, flags and colour weight`.

### Task 3: Importer upgrade

**Files:** `tools/unity/Castlemist/Editor/ParticleEffects.cs`.

- [ ] Presets: `ColourAndSize` uses `GW2ParticleMath.CurveOf` instead of the 15 % / 60 % envelope (stored curves unchanged). Colour over life: gradient keys at `ColourWeight` samples (8). Alignment: cases 1 Billboard; 4, 5 Stretch (length scale = sizeX / sizeY, velocity scale 0); 0, 2, 3 Billboard + a "not aligned" note in the import summary. Motion: emitter acceleration (dir x magnitude) + cloud acceleration into Force over Lifetime; drag into Limit Velocity `drag`, `multiplyDragByParticleSize = false`, `multiplyDragByParticleVelocity = false`; rotation drag into Rotation over Lifetime (an exponential-to-stop curve). `MaxAlive` 0x4000.
- [ ] Update the file's header comment (the "Not transcribed" list). Commit `feat(unity): particle import follows the client's curves, colour, alignment and motion`.

### Task 4: Measure tool

**Files:** `tools/unity/Castlemist/Editor/ParticleMeasure.cs`.

- [ ] `Castlemist > Particles > Measure Selection`: for each `GW2 FX` system under the selection, `ps.Simulate(t, true, true)` with `useAutoRandomSeed = false, randomSeed = 1`, stepping 1/60 to 4 s, collecting live count, distance percentiles from the system's origin (in GW2 inches: divide by the map scale), mean speed, alpha/size at life 0..1 bins (from `GetParticles`); load `<fileId>_fxstats.json` next to the sidecar; window table per emitter with Unity/GW2 ratios, red past ±15 %, "Copy report". No FX → a message.
- [ ] Commit `feat(unity): Measure — compare imported particle systems with castlemist's reference statistics`.

### Task 5: Deployment (ask first)

- [ ] Ask the user before copying the importer into GW2Porting / reimporting into the open map; then re-fit in place (memory: apply changes in place), measure 1450958 and 1450976, report the table.
