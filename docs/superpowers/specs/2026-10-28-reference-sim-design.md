# Reference Sim — Design (Particle Browser sub-project B)

Date: 2026-10-28
Status: written as the record (autonomous run, user: "Go, and then keep going").
Programme: docs/superpowers/specs/2026-10-28-effect-decode-design.md (A → B → C → D → E).

## Goal

One deterministic simulation of a model's effects, built on A's `fx` functions, that the
browser (C) draws and the Unity translation (D) is measured against. Same input + seed +
time step → the same particles, every time, on any machine.

## What A left for B (ledger rulings)

- The per-frame integrator: drag form, where cloud vs emitter drag/acceleration apply.
- Sprite alignment cases 0, 2, 3, 4, 5 (read, described, not coded); mesh-particle transform
  (read, not coded).
- Streak sim (decompiled, not read); lightning segment displacement (not read).
- Cloud `flags` (not read). Clip effect-track meanings (not read).

Each is read in Ghidra when B's code reaches it, written into the research note, and coded
in `fx`. Until read, the sim uses the 2026-07 approximation for that piece and its output
says so (`approximations: [...]`).

## Design

### Layer `fxsim` (`include/castlemist/fxsim/`, `src/fxsim/`; deps `extract`, `fx`)

- `struct SimInput` — built from a `ModelPreview` (`make_input(const ModelPreview&)`):
  clouds, emitters, lights, streaks, lightning, the bind-pose joint matrices, the clip list
  with visibility and effect tracks.
- `class Sim { Sim(SimInput, uint32_t seed); void set_clip(int); void step(float dt);
  const Frame& frame() const; Stats stats() const; }` — no globals; one `fx::Rng` per
  emitter, seeded `seed ^ emitterIndex * 0x9e3779b9` (the client's per-emitter seeding is
  unread; recorded as an approximation).
- `struct Particle { uint16_t emitter; fx::Vec3 pos, vel; float age, life, rotation;
  float size[2]; uint32_t rgba; float uv[4]; }`; `struct Frame { float time;
  std::vector<Particle> particles; std::vector<BoltStrip> bolts; std::vector<StreakStrip>
  streaks; }`.
- Spawn: accumulator per emitter (`FUN_140ac5a40` as recorded), shapes 0–6 (2026-10-22
  amendment), velocity via `fx::spawn_velocity`, emitter visibility from the clip's vis
  keys, burst emitters (emitterFlags 0x8) on hidden→visible.
- Per particle per step: integrate; colour via `fx::colour_weight` + opacity curve/preset;
  size via scale curve/preset × scaleInitial + scaleChange·age; rotation; flipbook frame;
  texCoordRect.
- `Stats`: per emitter and total — live count over time (sampled each step), spawned,
  expired; position bounds and the 5/50/95th percentile distance from the emitter; size and
  alpha at life fractions 0, 0.25, 0.5, 0.75, 1 (mean); mean speed. JSON via
  `stats_json(const Stats&)`.

### CLI

`gw2dat_cli fx-sim --dat D --file-id N [--clip NAME] [--seconds 4] [--dt 1/60] [--seed 1]
[--frames K]` → `{stats, approximations, frames: [first K frames' particles]}`.

### Renderer

`render/particles.cpp` keeps its draw code but takes particles from `fxsim::Sim` (the
globals `g_fx*` that duplicate the sim go). The look stays the same where the sim matches
the old code; where it differs it is the newly read client behaviour.

## Testing

- `cm_test_fxsim`: hand-built `SimInput`s — a constant-rate emitter reaches
  life/period × group live particles; a burst emitter fires once per visibility change; the
  same seed gives identical frames; a different seed gives different ones; colour and size
  follow the curves at the sampled life fractions.
- Real dat (`cm_test_dat`): model 200170 (2 clouds, flipbook embers) for 4 s — live count,
  bounds pinned once.
- `cm_test_render` still passes.

## Out of scope

UI (C), Unity (D).
