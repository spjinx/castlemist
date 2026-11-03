# Particle Browser Window — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** View > Particle Browser…: find any effect model or effect object, watch it in the main preview with the reference sim, inspect and solo emitters, scrub time, read stats, export.

**Architecture:** Testable logic in `fxsim` (feature summary, seek) and `src/ui/detail/particle_browser.h` + `src/ui/particle_browser_model.cpp` (rows, fx index file, filter, describer); the Win32 window in `src/ui/particle_browser.cpp`, built like the Texture Gallery; the renderer exposes its sim and a solo mask.

**Tech Stack:** C++20, Win32 (common controls, owner-drawn ListView), castlemist test framework.

**Spec:** docs/superpowers/specs/2026-10-29-particle-browser-design.md

## Global Constraints

- Only castlemist code opens Gw2.dat; scans run on worker threads, never on the UI thread.
- Cache files live in `dumps/index/`, keyed on the dat fingerprint (as `gw2_texture_headers.bin`).
- Match the gallery's style (naming, comment density); no new third-party code.
- Dat-test baseline: 13 known failures.

## Review Focus

1. A cache file from another dat (fingerprint mismatch) or a truncated one: ignored, rescanned. Test in Task 1.
2. Search text that is not an id, range or list: treated as a codename substring, not an error. Test in Task 1.
3. Seeking backwards or past the end: exact (restart + step), clamped at 0. Test in Task 2.
4. Soloing every emitter off: nothing drawn, no crash. Test in Task 2 (mask size mismatch → all drawn).
5. Closing the window mid-scan: the worker stops and the partial index is saved. Covered in Task 4 (cancel flag test).

---

### Task 1: Rows, fx index file, filter, describer

**Files:** Create `src/ui/detail/particle_browser.h`, `src/ui/particle_browser_model.cpp`, `tests/test_ui_particle_browser.cpp`; modify `tests/CMakeLists.txt` (add the source to `cm_test_ui`).

**Interfaces — Produces** (namespace `castlemist::ui::particles`):
- `struct ModelRow { uint32_t fileId = 0; uint16_t clouds = 0, emitters = 0, streaks = 0, lightning = 0, lights = 0, meshEmitters = 0, bursts = 0, clipTracks = 0; uint8_t alignMask = 0; };` (`alignMask` bit k = some emitter takes sprite case k)
- `bool save_index(const std::filesystem::path&, const std::string& fingerprint, uint32_t scannedUpTo, const std::vector<ModelRow>&);` `bool load_index(const std::filesystem::path&, const std::string& fingerprint, uint32_t& scannedUpTo, std::vector<ModelRow>&);` (binary: magic "CMFX", version 1, fingerprint string, scannedUpTo, count, rows; mismatch/truncation → false)
- `struct ModelFilter { bool streak = false, lightning = false, mesh = false, burst = false, tracks = false; int alignCase = -1; std::string search; };` `std::vector<size_t> filter_rows(const std::vector<ModelRow>&, const ModelFilter&);` (search via `gallery::parse_id_query`; text that does not parse matches nothing on the Models tab)
- `struct FieldLine { std::string group, label, value, meaning; };` `std::vector<FieldLine> describe_emitter(const ParticleEmitterCPU&);` — meanings: alignment → "camera-facing" / "axis billboard along motion" / "along on-screen motion" / "faces alignmentDir" / "faces motion" / "faces away from motion"; emitterFlags 0x8 → "burst: one group when the bone turns visible"; flags 0x10 → "Y size = X", 0x400 → "scale preset reversed", 0x200 → "scale preset inverted", 0x80 → "opacity preset inverted", 0x4 → "no emitter transform at spawn", 0x1000 → "random spin direction"; presets named by `fx::curve_preset` family ("t²", "bump", "ease", "tent", …); pairs shown "min..max".

- [ ] Failing tests: `ui.fx_index_round_trip`, `ui.fx_index_rejects_other_dat_and_truncation`, `ui.fx_filter_features_and_search` (id, range, list; non-numeric text → no rows), `ui.describe_emitter_alignment_burst_flags`.
- [ ] Implement; PASS; commit `feat(ui): particle browser model: rows, fx index, filter, describer`.

### Task 2: Feature summary, seek, renderer hooks

**Files:** `include/castlemist/fxsim/features.h`, `src/fxsim/features.cpp` (deps: native model types already via extract), `include/castlemist/fxsim/sim.h` (`seek`), `src/render/particles.cpp`, `include/castlemist/render/model_renderer.h`, `tools/gw2dat_cli/main.cpp` (census uses the shared summary), `tests/test_fxsim.cpp`.

**Interfaces — Produces:**
- `struct ModelFeatures { uint16_t clouds, emitters, streaks, lightning, lights, meshEmitters, bursts, clipTracks; uint8_t alignMask; };` `ModelFeatures summarize(const castlemist::model::Model&);`
- `void seek(Sim& sim, const SimInput& in, uint32_t seed, float dt, float t);` (rebuild + step `round(t/dt)` times; t < 0 → 0)
- render: `const fxsim::Sim* effect_sim();` `void restart_effects(uint32_t seed);` `void set_effect_solo(const std::vector<bool>& mask);` (mask indexed by `SimInput::emitters`; size mismatch → draw all) `void set_effect_time(float t);` `void set_effect_speed(float k);` `void set_effects_paused(bool);`

- [ ] Failing tests: `fxsim.summarize_counts_features` (hand-built `model::Model`), `fxsim.seek_equals_stepping`, `fxsim.seek_backwards_restarts`. Implement; census switched to `summarize`; PASS; commit `feat(fxsim): feature summary, seek; renderer timeline and solo hooks`.

### Task 3: Effects source

**Files:** `src/ui/detail/particle_browser.h`, `src/ui/particle_browser_model.cpp`, tests.

**Interfaces — Produces:** `struct EffectRow { uint32_t uid = 0, kind = 0, model = 0, sound = 0, children = 0; std::string codename; };` `std::vector<EffectRow> effect_rows(const cntc::PackSet&, const skillfx::ContainerOf&);` `std::vector<size_t> filter_effects(const std::vector<EffectRow>&, int cls /* -1 any */, const std::string& search);` (search: id query on uid, else codename substring)

- [ ] Failing tests (synthetic `PackBuilder` packs as in test_skillfx.cpp): `ui.effect_rows_read_kind_model_children`, `ui.filter_effects_by_class_and_codename`. Implement; PASS; commit `feat(ui): particle browser effect rows`.

### Task 4: Background model scan

**Files:** `src/ui/particle_browser_model.cpp` (`class ModelScan { start(datPath, mftEntries, fileIdOf, cachePath, fingerprint); stop(); progress(); snapshot(); }`), tests.

- [ ] Failing test `ui.model_scan_cancel_saves_partial` (a scan over a fake entry source stops on `stop()`, the saved file loads with `scannedUpTo` < total). Implement (worker thread, entries decoded with `Extractor::extractEffects`, rows appended under a mutex, periodic save every 2,000 entries and on stop); PASS; commit `feat(ui): particle browser background model scan`.

### Task 5: The window

**Files:** `src/ui/particle_browser.cpp`, `src/ui/detail/app_state.h`, `src/ui/window_proc.cpp`, `src/ui/application.cpp` (menu + `GW2_PARTICLES` / `_SHOT` / `_QUIT`).

- [ ] Window: tabs (Models / Effects), filter bar, virtual ListView, progress line; inspector (TreeView: clouds → emitters → field lines; checkboxes = solo), approximations box, stats box (refreshed 4×/s), timeline (play/pause/restart, slider 0–10 s, seed, speed), buttons (Export for Unity…, Copy IDs, Save stats…). Selecting loads the model into the main preview (as the gallery's click does).
- [ ] Headless: `GW2_PARTICLES=1450958 GW2_PARTICLES_SOLO=2 GW2_PARTICLES_SHOT=<path>` writes a screenshot of window + preview; check by eye; `ctest` PASS; commit `feat(ui): Particle Browser window`.

### Task 6: Close-out

- [ ] `docs/using-castlemist.md`: the window. Full `ctest`; dat suite at baseline. Whole-branch review (one fresh reviewer), one fix pass.
