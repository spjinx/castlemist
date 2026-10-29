# Reference Sim — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A deterministic `fxsim::Sim` over a `ModelPreview`'s effects, with statistics, a CLI, and the renderer drawing from it.

**Architecture:** New layer `fxsim` (deps `extract`, `fx`). `make_input(ModelPreview)` → `SimInput`; `Sim` steps particles with A's `fx` functions and keeps `Stats`. Unread client behaviour uses the 2026-07 approximation and is named in `approximations()`.

**Tech Stack:** C++20, castlemist test framework, nlohmann::json.

**Spec:** docs/superpowers/specs/2026-10-28-reference-sim-design.md

## Global Constraints

- Same `SimInput` + seed + dt sequence → bit-identical `Frame`s.
- No globals in `fxsim`; `fx` stays dependency-free (core only).
- Exe reads are static only (Ghidra `-readOnly`); every newly read behaviour goes into docs/research/gw2-particle-system.md with addresses (2026-09 build).
- Known dat-test baseline: 13 failures (memory: project-patch-2026-10-14).

## Review Focus

1. Emitters with spawnPeriod 0 or probability 0: no infinite loop, no spawn. Test in Task 2.
2. Emitter indices in clouds out of range: skipped. Test in Task 1.
3. Huge spawn rates: a live cap (65,536 per sim) with a counted `dropped`. Test in Task 2.
4. dt 0 or negative: no state change. Test in Task 2.
5. A model with effects but no skeleton: emitters at the model origin. Test in Task 1.

---

### Task 1: `fxsim` layer, `SimInput`, `make_input`

**Files:** Create `include/castlemist/fxsim/sim.h`, `src/fxsim/input.cpp`, `tests/test_fxsim.cpp`; modify `CMakeLists.txt` (`castlemist_add_layer(fxsim DEPS castlemist::extract castlemist::fx)` after `extract`), `tests/CMakeLists.txt`.

**Interfaces — Produces:** `struct EmitterIn { ParticleEmitterCPU e; int cloud = -1; fx::Vec3 origin; };` `struct SimInput { std::vector<ParticleCloudCPU> clouds; std::vector<EmitterIn> emitters; std::vector<std::string> warnings; };` `SimInput make_input(const ModelPreview&);` (origin = the emitter's bone, else its cloud's bone, else 0; bad cloud→emitter indices warn and skip).

- [ ] Failing tests `fxsim.make_input_attaches_by_bone`, `fxsim.make_input_skips_bad_emitter_index`, `fxsim.make_input_without_skeleton_uses_origin`. Implement; PASS; commit `feat(fxsim): sim input from a model preview`.

### Task 2: Spawning and stepping

**Files:** `src/fxsim/sim.cpp`, `sim.h`, `tests/test_fxsim.cpp`.

**Interfaces — Produces:** `struct Particle { uint16_t emitter; fx::Vec3 pos, vel; float age, life, rotation, spin; float size[2], growth[2]; float colour[4]; };` `struct Frame { float time = 0; std::vector<Particle> particles; };` `class Sim { public: Sim(SimInput in, uint32_t seed); void step(float dt); const Frame& frame() const; const Stats& stats() const; std::vector<std::string> approximations() const; };`

- Spawn per emitter: accumulator += dt; while acc ≥ period: if u ≤ probability×0.01 spawn round(sample(group)) particles; acc −= period (period ≤ 0 or probability ≤ 0: none). Shapes per the 2026-10-22 amendment; velocity `fx::spawn_velocity`; life `fx::sample(lifetime)`.
- Step: age += dt; expire at age ≥ life; vel += (accel + cloud accel)·dt; drag as the 2026-07 sim (`v *= max(0, 1 − drag·dt)`) — listed in `approximations()` until read; pos += vel·dt.
- [ ] Failing tests: `fxsim.constant_rate_reaches_steady_count` (period 0.1, group {1,1}, life {1,1}, probability 100 → ~10 live after 2 s), `fxsim.zero_period_spawns_nothing`, `fxsim.same_seed_same_frames`, `fxsim.dt_zero_changes_nothing`, `fxsim.live_cap_counts_dropped`. Implement; PASS; commit `feat(fxsim): spawn and step`.

### Task 3: Appearance over life

**Files:** `sim.cpp`, tests.

- Colour: `fx::colour_weight` between begin/end samples; alpha × opacity (`fx::curve_eval` keys, else `fx::opacity_preset_value`). Size: scaleInitial sample + scaleChange·age, × scale curve/preset (`fx::scale_preset_value`). Rotation: rotationInitial + rotationChange·age (radians). Flipbook frame and texCoordRect sub-rect.
- [ ] Failing tests: `fxsim.colour_follows_ping_pong`, `fxsim.size_follows_scale_preset`, `fxsim.flipbook_frame_at_fps`. Implement; PASS; commit `feat(fxsim): colour, size, rotation, flipbook`.

### Task 4: Stats, JSON, CLI

**Files:** `include/castlemist/fxsim/stats.h`, `src/fxsim/stats.cpp`, `tools/gw2dat_cli/main.cpp` (`fx-sim`), `tools/CMakeLists.txt` (link fxsim), tests.

**Interfaces — Produces:** `struct EmitterStats { size_t spawned, expired, dropped; std::vector<uint32_t> live; float spread[3]; float meanSpeed; float alphaAt[5], sizeAt[5]; };` `struct Stats { std::vector<EmitterStats> emitters; std::vector<uint32_t> live; };` `nlohmann::json stats_json(const Stats&);`

- [ ] Failing tests `fxsim.stats_count_spawn_and_expiry`, `fxsim.stats_spread_percentiles`. Implement; CLI `gw2dat_cli fx-sim --dat D --file-id N [--seconds 4 --dt 0.0166667 --seed 1 --frames K]`. Real-dat test `dat.fx_sim_200170_steady_state` (pins live count range after 4 s). Commit `feat(fxsim): stats and fx-sim`.

### Task 5: Renderer on the sim

**Files:** `src/render/particles.cpp`, `src/render/detail/state.h`, `src/render/CMakeLists` (layer deps: render gains `castlemist::fxsim`), `CMakeLists.txt`.

- [ ] Replace `g_fx*` sim state with one `std::unique_ptr<fxsim::Sim>`; `capture_effects` builds it, `update_particles(dt)` steps it, `render_particles` reads `frame()`. Keep the draw code and env switches. `cm_test_render` PASS; headless `GW2_FXTEST` still writes shots. Commit `refactor(render): particles from fxsim`.

### Task 6: Close remaining reads (one commit each, note + fx function + test)

Order by what the user sees most: (a) the integrator / drag (search the job system: callers of the cloud update `FUN_140ac9e90` and the jobs it queues); (b) sprite cases 0, 2–5 → `fx::sprite_axes(case, …)`; (c) mesh-particle transform → `fx::mesh_particle_xform`; (d) cloud `flags`; (e) clip effect-track meanings (the model-instance animation update); (f) streaks (`0x14155fc80`, `0x141562eb0`). Each removes its entry from `approximations()`.
