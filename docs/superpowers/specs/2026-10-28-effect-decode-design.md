# Effect Decode, Complete — Design (Particle Browser sub-project A)

Date: 2026-10-28
Status: approved direction ("Go, and then keep going", 2026-10-28); written as the record.

## The programme this belongs to

The user wants an exhaustive particle-system browser in castlemist, GW2's particle maths
decompiled well enough that Unity ports come out right the first time, and then Mad King
Thorn, Prince Edrick, Palawa Joko and the Mad Spirits ripped into their open Unity map.
Five sub-projects, in this order, one spec → plan → build each:

| | sub-project | output |
|---|---|---|
| **A** | **effect decode, complete** (this spec) | every byte GW2 uses to draw an effect, read; the client's maths written down and as tested functions |
| B | reference sim | a standalone, deterministic sim built on A's functions, with numeric statistics (the oracle) |
| C | particle browser | castlemist window: every effect, previewed and inspected, scrubbable |
| D | measured Unity translation | analytic GPU particles (avatar-safe) + Shuriken where needed, checked against B by numbers |
| E | NPC effects + the bosses | NPC → effect links; Thorn, Edrick, Joko, Mad Spirits into the open map, in place |

**Premise (settled):** GW2 renders every particle on the client. The server sends an effect
id (and agent, time); the client looks up a type-183 content object and its files in the
dat. So all the visual data is static and readable; only the *which skill/NPC state plays
which effect* link travels over the network (docs/research/gw2-skill-effects.md).

## Goal of A

castlemist reads everything the client uses to draw an effect, and the research note states
the client's own maths for it, so B can simulate it and D can translate it. Nothing is
guessed: what the exe or the data does not settle is recorded as a number, and listed.

## Ground rules

- Gw2.dat only through castlemist's tools; Gw2-64.exe read statically only (Ghidra
  headless, `-readOnly`); never launch or attach to the client.
- No unsure names: fields keep the template's names; a value whose meaning is unproven is
  exported raw and its meaning marked unknown in the note.
- Every semantic claim cites the function it came from (address in the 2026-10 build) and,
  where possible, a real-data case that shows it.

## What is missing today (measured 2026-10-28)

MODL side (`ModelFileDataV70`, template `dumps/packfile/gw2_packfile.json`):

| data | state |
|---|---|
| `cloudData` clouds/emitters | parsed; **not read:** emitter `accelerationDistRange/Speed`, `velocityDistRange/Speed`, `velocityInherit`, `affinity`, per-emitter `drag`; mesh emitter `alignment`, `bankThreshold/Scale`, `obstacleRadius`, `rotation[3]`, `rotationDrag`, `scale*[3]`, `spin[3]`; cloud `obstacleIndices` |
| `streakData` (streaks + anchors) | **not read** |
| `lightningData` | parsed; no semantics, not exported |
| `obstacles` (`ModelObstacleDataV66`) | **not read** |
| `windData` (`ModelEffectWind`) | not read (the sim fakes a wind) |
| clip tracks `cloudAnim`, `lightAnimData`, `streakAnim`, `lightningAnim`, `windAnimData` (`ModelTrackTypeData`: type, granny track group/vector track, initialValue) | **not read** — clips animate effect parameters over time; today only `visTrackData` is used |
| clip `triggerTimes`/`triggerTokens` | read for sound only |

Semantics not yet known: alignment types; opacity/scale curve presets; `curveType`;
`cloud.flags` (blend / sort / space); mesh-emitter orientation, bank and spin;
`velocityInherit`; the dist range/speed terms; obstacles; the whole streak and lightning
sims; what each `ModelTrackTypeData.type` drives.

Content side: type-183 effect objects. Known: GUID @+0, type @+16, uid @+20, dataId
(`kind`) @+40, kind-1 groups' child refs @+112/+120, an optional MODL and AMSP. The client
has one class per effect kind (`Gw2\Game\Effect\Cli\EfCliEffect{Compound,Model,Audio,
Decal,Material,PostProc,ShakeSpring}.cpp`); their content layouts are unread.

## Design

### A1. MODL decode, every field (`native/gw2model.hpp`, template-driven as today)

- `ParticleEmitter` gains the missing V70 fields; `MeshEmitterSettings` read whole.
- New `Streak`, `StreakAnchor`, `Obstacle`, `EffectWind` structs in `CloudSystem`;
  `ParticleCloud.obstacleIndices`.
- `AnimClip` gains `effectTracks`: one list of `{target kind (cloud/light/streak/lightning/
  wind), bone, type, trackGroup, vectorTrack, initial[4]}`, sampled the way `uvAnims` /
  `matConstAnims` already are (granny vector tracks).
- Old versions: each struct's versions are walked through the template, as `parseEmitter`
  already does for V≤66 vs V≥67; a field absent in a version stays at its default and is
  flagged `present=false`, never invented.
- CPU mirrors (`extract/model_types.h`) and the `_particles.json` sidecar carry all of it.
  The sidecar's existing keys keep their meaning; new keys are added beside them, so the
  Unity importer keeps working unchanged.

### A2. The client's maths (`docs/research/gw2-particle-system.md` + `fx` library)

A new layer **`fx`** (`include/castlemist/fx/`, `src/fx/`), depending only on `core`. It
holds the client's per-particle maths as pure functions over plain structs — no globals,
no renderer:

- `rng` (the client's U[0,1) and U[-1,1) helpers as read), `sample(range)` = base + u·span;
- `spawn_position(shape, radius, u…)` for shapes 0–6, `spawn_velocity`, dist terms,
  `velocityInherit`;
- `integrate(state, dt)`: velocity, acceleration, drag (per emitter and per cloud),
  cloud velocity, wind;
- `colour_at(life, begin, end, period, falloff)` (the ping-pong, as in the 10-23 note),
  `curve_eval(curve, t)`, `curve_preset(id, t)`;
- `sprite_basis(alignmentType, alignmentDir, velocity, camera)`; `size_at`,
  `rotation_at`, flipbook frame;
- `streak_*`, `lightning_*` for those systems, once read.

Each function's comment names the exe function it transcribes. Topics, each read in
Ghidra and written into the note before its function is written:

1. alignment types (`PlaneEmitterSettings.alignmentType`, mesh `alignment`);
2. curve presets and `curveType` (the vertex builder `FUN_140ad11b0` and its callees,
   and the baked-curve builder `FUN_140acbe30`);
3. `cloud.flags`, `fvf` (blend, sort, local/world space);
4. dist range/speed, `velocityInherit`, per-emitter vs cloud drag;
5. mesh emitters: orientation, bank, spin, scale over life;
6. obstacles (type, response, drag/gravity coefficients);
7. `ModelTrackTypeData.type` for each target kind: which parameter a clip animates;
8. streaks (`Gr\Streak\GrStreak*.cpp`) and lightning (`Gr\Lightning\GrLightning*.cpp`).

### A3. Effect objects (type 183) — `skillfx` gains a typed decode

- Per EfCli kind, read its content reader in the exe (the functions that take the
  type-183 object, `EfCliEffect*.cpp` asserts) to get field offsets; cross-check with a
  **field census**: for every type-183 object in the dat, the value distribution at each
  offset, split by `kind`.
- `skillfx::EffectDef`: `{guid, uid, kind, codename, children[] (with whatever timing /
  transform the compound reader shows), model, sound, decal{…}, material{…}, postproc{…},
  shake{…}, unknown fields raw}`. Fields are named only once the reader proves them.
- The kind-number → class mapping is settled from the factory that switches on it.
- `skillfx` keeps `find_effects`; it now returns typed defs.

### CLI (research + test surface)

- `gw2dat_cli fx-census --dat … [--what modl|effects]`: counts per field and value (which
  models use streaks, cloudAnim, presets, each alignment…) and lists example fileIds.
  This chooses the curated test cases.
- `gw2dat_cli effect-show --uid N | --guid G`: one effect tree, typed.
- `gw2dat_cli model` keeps emitting the `effects` block, now complete.

## Errors

- A struct version the template lacks, or a field outside the file: the model still
  loads; the effect is marked `incomplete` with the reason; nothing else is dropped.
- An effect kind whose reader is not yet transcribed: exported with its raw fields and
  `kind`, flagged `undecoded`.

## Testing

- `fx` functions: unit tests (`cm_test_fx`) with expectations worked out by hand from the
  decompiled code (each test cites the function).
- MODL decode: curated real-dat cases in `cm_test_dat` chosen from `fx-census` — one model
  per new feature (a streak, a lightning, an obstacle, a cloudAnim clip, each alignment
  type seen, a mesh emitter, an old V≤66 emitter). Expected values are read once with the
  CLI and pinned.
- Effect objects: the census numbers and one curated object per decoded kind.
- Existing suites still pass (11 pre-existing `cm_test_dat` curated-id failures stay as
  they are).

## Out of scope for A

The sim engine and its statistics (B), any UI (C), Unity changes (D), NPC links (E).
`render/particles.cpp` is left as it is until B replaces it.
