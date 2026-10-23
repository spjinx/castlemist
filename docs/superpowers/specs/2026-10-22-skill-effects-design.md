# Skill Effects for VRChat Avatars — Design

Date: 2026-10-22
Status: draft for review (revised 2026-10-22 after mantra research)

## Goal

Port GW2 skill and ability visuals — particles, effect lights and the effect's own
models — into Unity so they can be worn on VRChat avatars. Firebrand comes first
because it is the most particle-heavy profession; the pipeline is profession-agnostic.

## What the user decided

- **Deliverable:** a prefab library (one prefab per skill) **plus** a ready-made avatar
  toggle menu. Tome of Justice is the first fully worked skill.
- **Platform:** PC first. The export keeps what a later Quest/Android variant needs
  (each material's simple blend mode), but no Quest build is made now.
- **What an effect contains:** particles, effect lights and the effect's own models
  (e.g. the floating tome), attached to the avatar. **GW2 character animations
  (cast/hold poses) are out of scope**: they belong in a later, separate
  locomotion/action-layer sub-project.
- **Emitter rebuild:** one Unity (Shuriken) ParticleSystem per GW2 emitter (approach A).
  A GPU-baked playback path was considered and rejected for now.
- **Menu tooling:** VRCFury, in a new minimal avatar test project.
- **Shared reader:** the cntc pack reader moves from the npc layer into `format`.

## Ground rules

- Never read Gw2.dat directly; all reads go through castlemist's dat/index code.
- No unsure names. Numbers that are not understood (e.g. the effect object's kind)
  are recorded as numbers, not given guessed labels.
- Missing data is reported, never filled in by guesswork.

## Data model (measured 2026-10-22)

- A skill is content type **64**, keyed by its API id (dataId @+40).
- A skill points (local or external refs, at varying fields: +184, +216, +3560 ...)
  at type **183** objects. A type-183 object carries an optional AMSP sound script and
  an optional MODL — the effect model. Its dataId is a small number (6, 10, 11 seen);
  meaning unknown, recorded as `kind`.
- The effect MODL carries the baked particle clouds/emitters/lights already decoded by
  `gw2model.hpp` (see docs/research/gw2-particle-system.md).

| skill | effect model | clouds / emitters / lights |
|---|---|---|
| Tome of Justice 44364 | 1764329 | 9 / 19 / 1 |
| Symbol of Vengeance 40624 | 1764294 (+ 57192, 57469) | 3 / 13 / 1 |
| Blazing Edge 45402 | 1764496 (+ 57192) | not measured |

- Model 57192 (1 cloud, 1 emitter) is shared by several weapon skills.
- Full measurements: docs/research/gw2-skill-effects.md. The parts the design relies on:
  - **Effects form trees.** A kind-1 effect is a group whose refs (+112, +120) are child
    effects.
  - **Mantras** have no effect of their own. Their charge skills do: +736 = first
    charge (visuals come only from the boons it applies), +744 = final charge (shared
    effect 1764310), plus a group skill carrying shared effect 1764482.
  - **Tomes** are bundle items (type 35) whose type-65 bar lists the chapter skills; the
    tome skill does not point at its chapters. The tome's effect model (1764329)
    contains the floating book mesh.
  - **The public API misses skills** (all tome chapters, some charge skills), so the
    skill set comes from the dat: every type-64 object in the spec's codename namespace
    (`Bp5rI.` for Firebrand: 137 objects).
  - **51 of Firebrand's 68 effect objects are referenced by nothing found** (no
    pointer, tracked reference, GUID, uid or codename anywhere in content, the human
    animation banks or the exe). The chapters' fire is probably among them.

## Architecture

### 1. castlemist (C++)

- **`format` layer:** the cntc pack reader (`src/npc/pack.{h,cpp}`,
  `src/npc/decompress.h`) moves here unchanged in behaviour, still layer-internal.
  The npc layer and the new skillfx layer both use it.
- **New `skillfx` layer** (`include/castlemist/skillfx/`, `src/skillfx/`):
  - `spec_skills(PackSet, api_skill_ids) -> SkillSet`: the skill namespace is the
    codename prefix most of the spec's API skills share. Every type-64 object in it is
    an entry, plus each bundle item (type 35) in it with its bar's slots, which gives
    the tome chapters and their order.
  - `find_effects(PackSet, skill) -> SkillEffects`: the type-183 objects the skill
    points at directly, expanded through kind-1 groups. Each effect is recorded as
    `{field, kind, codename, model fileId, sound fileId}`. Other type-64 refs are
    listed as `related_skills`. Refs to boon/condition skills (ids < 2000 seen) are
    listed as `applies`, not followed: their visuals are generic (e.g. the Aegis
    bubble), not Firebrand's.
  - **Unlinked effects:** the effect namespace is the codename prefix most of the
    reached effects share (`Bmt2N.`). Every effect in it that no skill reached is
    exported under `unlinked/`, flagged as such, so they can still be used by hand.
  - `export_skill(dat, effects, out_dir)`: writes the skill folder (below), exporting
    each effect model through the existing glTF exporter, which already writes the
    `_particles.json` sidecar. Mesh-particle emitters' referenced meshes are exported
    too. Models used by more than one skill go to `shared/` once.
  - **Names:** the API name where it has one. Otherwise a structural label: chapters
    from their bar (`Tome of Justice, slot 1`), charges from their mantra
    (`Mantra of Solace, final charge`), else the codename. No guessed names.
- **CLI:** `gw2dat_cli skillfx-export (--spec N | --skill N) --out DIR` and the
  research command `gw2dat_cli cgraph` (content-graph walk; already written).
- Skill names come from the public API, as the npc catalogue does.

**Export layout:**

```
DIR/
  firebrand.json            spec id, skill list
  skills/44364-tome-of-justice/
    skill.json              id, name, effects[], related_skills[]
    1764329.glb, 1764329_particles.json
  shared/57192.glb, 57192_particles.json
  unlinked/1764300.glb, 1764300_particles.json, unlinked.json   codename, kind per effect
```

`skill.json` effects entries: `{field, kind, codename, model, sound, glb}`. Sounds are
recorded, not exported.

### 2. Unity prefab importer (`tools/unity/Castlemist/Editor/SkillFxImporter.cs`)

Builds one prefab per skill: `skill root → effect child → model + one ParticleSystem
per emitter`. Needs only built-in Unity modules (works in GW2Porting too).

| GW2 emitter | Unity ParticleSystem |
|---|---|
| spawnPeriod × spawnProbability × spawnGroupSize | emission rate; bursts when the period is long |
| spawnShape 0 cylinder / 1 disc / 2 line / 3 box / 4–5 mesh surface / 6 point | shape: cone(0°) / circle / edge / box / mesh / sphere r=0 |
| velocity / acceleration (dir+speed, `{min, max}` each); cloud acceleration, velocity, drag | start speed range; force over lifetime; drag |
| colorBegin / colorEnd, opacityCurve, scaleCurve | start colour range; colour over lifetime; size over lifetime |
| texCoordRect, flipbook | material tiling/offset; texture-sheet animation within the rect |
| rotationInitial / rotationChange, alignmentType | start rotation; rotation over lifetime; billboard / stretched / local |
| wind fields | mild noise + force approximation |
| mesh emitter | renderer Mesh mode with the exported mesh |
| distortion emitter (`isDistortion`) | GW2 Distort shader (GrabPass, PC only) |
| effect light | additive glow sprite (no real light) |

- Stored emitter float pairs are `{min, max}`: the client's loader turns them into its runtime
  `{base, span}` (verified in the exe 2026-10-22, docs/research/gw2-particle-system.md).
- Positions and directions use the same GW2 (Z-up, inches) → Unity (Y-up, metres)
  conversion as the .glb export.
- Materials use the planned `GW2 Particle` shader; each records its simple blend
  (additive / alpha) for a later Quest swap.
- A GW2-bone → Unity Humanoid bone table resolves attach points; unmapped bones fall
  back to the avatar root and are listed in the import summary.

### 3. Avatar menu (`SkillFxAvatarMenu.cs`, compiled only with the Avatars SDK + VRCFury)

- Generates a "Firebrand FX" prefab: the skill prefabs plus one VRCFury **Toggle** per
  skill with effects (menu path `Firebrand/<skill name>`, saved). An option puts all
  toggles under one VRCFury exclusive tag (one effect at a time). VRCFury builds the
  menu, parameters and FX layer at upload; the generator writes no animator.
- Reports the avatar-performance totals (particle systems, max live particles,
  materials) and flags toggles that would push a typical PC avatar past "Poor".
- **Test project:** new minimal Unity 2022.3.22f1 project with the VRChat Avatars SDK,
  VRCFury and Poiyomi; test avatar = a castlemist-exported GW2 character.

### 4. Pairing from arcdps logs (part of stage 1)

The user records arcdps `.evtc` logs while casting each Firebrand skill on a training golem.
Claude only parses the files offline. It never launches, attaches to or hooks the client.

- **Reader** (`skillfx/evtc.{h,cpp}`): the public EVTC layout (header, agents, skills,
  then 64-byte combat items, zip-wrapped when `.zevtc`). It reads three kinds of event:
  - skill activations (cast start/end, with the skill id);
  - effect events. Current arcdps (2026) logs 60 `EFFECTGROUNDCREATE`, 61
    `EFFECTGROUNDREMOVE`, 62 `EFFECTAGENTCREATE` and 63 `EFFECTAGENTREMOVE`: effect
    content id, source agent, time. Builds before 2025-05-26 used 45/51 instead
    (both retired); read those as well for older logs;
  - id-to-GUID events (46 `IDTOGUID`), which map each effect content id to its 16-byte GUID. That GUID
    is the effect object's +0.
- **Pairing:** each effect event whose source is the recording player is assigned to that
  player's most recent cast that started before it and within 3 s. Pairs seen in at least
  two casts are kept, with their counts. Effects seen under several skills (shared
  visuals) keep every skill.
- **Use:** `skillfx-export --pairings FILE` adds paired effects to each skill's entry with
  `"source": "arcdps"` (direct links stay `"source": "content"`). Whatever stays unpaired
  remains in `unlinked/`. Elite Insights' `EffectGUIDs.cs` names (MIT) are recorded too,
  as a cross-check.
- **CLI:** `gw2dat_cli evtc-pairs --log FILE... --out pairs.json`.

## Dependencies

- `GW2 Particle.shader` and `GW2 Distort.shader` (über-shader work) are uncommitted on
  master. They must be committed there and merged into this branch before stage 2.

## Errors

- A skill with no effects found: listed in `firebrand.json` with `effects: []`.
- A model that fails to load or export: recorded in `skill.json` as
  `{model, error}`; other effects still export.
- Unity: an unknown spawn shape or alignment falls back (point / billboard) and is
  listed in the import summary, never silently dropped.

## Testing

- **C++:** synthetic-pack tests (PackBuilder style, as test_format_content_schema.cpp)
  for the walk rules: depth cap, stop at skills / spec objects, local and external
  refs, dedup of shared models. A curated real-dat test: Tome of Justice → model
  1764329 with 19 emitters. The existing npc tests must still pass after the reader move.
- **Unity:** one editor test per mapping row (Tests~), then a visual comparison of Tome
  of Justice in Unity against castlemist's own particle preview of 1764329.

## Stages (one plan each)

1. castlemist: reader move, skillfx layer, `skillfx-export`, the evtc reader and
   `evtc-pairs`, tests.
2. Unity prefab importer (after the shader dependency lands).
3. VRCFury menu generator + avatar test project.

## Open research (not blocking)

- **Settled (static RE, see the research note):** the 51 unlinked effects are played by
  an id the client receives at runtime (server messages in `EfCliMsg.cpp`, and the
  animation-trigger map in `AnmlAnimTriggers.cpp`). Their skill pairing is not in
  client files. They are paired from arcdps logs (section 4).
- The meaning of the type-183 `kind` numbers.
- Skill effects that live outside MODL clouds (decals, streaks, lightning, ribbons).
