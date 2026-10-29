# Measured Unity Translation — Design (Particle Browser sub-project D)

Date: 2026-10-29
Status: written as the record (autonomous run; programme: 2026-10-28-effect-decode-design.md).

## Goal

GW2 effects imported into Unity behave like the client's — spread, rate, size and fade over
life, alignment — and the importer proves it with numbers instead of eyeballing: each port is
sampled in the editor and compared with castlemist's reference sim (`fx-sim` stats).

## Where the Unity side stands (tools/unity/Castlemist/Editor/ParticleEffects.cs)

Already right: {min, max} ranges, percent probability, degrees, spawn shapes, velocity as
normalize(dir) x magnitude, half-sizes and flag 0x10, flag 0x4 / 0x1000, bursts and bone
visibility per clip, texCoordRect / flipbook, distortion skipped, alignment 1 as a stretched
streak. Stand-ins the file itself marks UNPROVEN, now settled by A/B:

| stand-in | the client (research note) |
|---|---|
| opacity / scale presets = a 15 % / 60 % envelope | 18 exact preset curves; flags 0x80 (opacity inverted), 0x400 (scale reversed), 0x200 (scale inverted) |
| colour begin -> end over the life | `colour_weight`: ping-pong every colorPeriod, falloff window |
| alignments other than 1 camera-facing | 0 fixed plane (alignmentDir), 2 faces motion, 3 faces away, 4 along on-screen motion (Stretch), 5 axis billboard (Stretch) |
| motion | `pos += v dt + a dt²/2`, drag `v *= 1 - d dt` (cloud + emitter), the emitter's own acceleration |
| spin | rotationDrag slows the spin and stops it |
| MaxAlive 2000 | the client allows 0x4000 per cloud |

## Design

1. **Importer upgrade** (ParticleEffects.cs): presets as exact AnimationCurves (sampled at 33
   points, flags applied); colour over life from `colour_weight` (gradient keys at the ping-pong
   turns, 8 keys max); alignment cases → Shuriken: 1 Billboard, 4 and 5 Stretch (velocity scale
   from the size ratio), 0 / 2 / 3 Mesh-quad with `alignment = Local` / velocity-aligned via
   `renderMode = Mesh` + `alignment = Velocity` where Shuriken can, else Billboard with a listed
   warning; emitter acceleration into Force over Lifetime (plus the cloud's); drag into Limit
   Velocity `drag` (dampen 0, multiply-by-size off); rotation drag as a rotation-over-lifetime
   curve; MaxAlive 0x4000 per cloud.
2. **Measurement** (`Castlemist > Particles > Measure`): for each GW2 FX system under the
   selection, `ParticleSystem.Simulate` in edit mode at 1/60 s for 4 s with a fixed random seed,
   gathering the same statistics as `fxsim::Stats` (live per step, spread 5/50/95 %, mean speed,
   alpha and size at life 0 / .25 / .5 / .75 / 1), converted to GW2 units (inches, Z up).
3. **Comparison**: the measure window loads the model's `fx_stats.json` (the browser's "Save
   stats…" or `gw2dat_cli fx-sim`, exported beside the sidecar by the model export) and shows per
   emitter the ratio Unity / GW2 for each statistic, red past ±15 %; "Copy report" gives the
   table as text.
4. **Sidecar**: the model export writes `<fileId>_fxstats.json` (seed 1, 4 s, 1/60) beside
   `_particles.json`, so every exported model carries its reference numbers.

## Out of scope (later stage)

Analytic GPU particles for avatars (a shader that evaluates the closed-form motion per quad);
streaks and lightning in Unity; clip effect tracks.

## Testing

- C++: the export writes `_fxstats.json` (exportgltf test on a synthetic emitter: present, live
  count matches `fxsim` for the same input).
- Unity (EditMode tests in `Tests~`): each preset curve matches the C++ table at 5 points; flag
  combinations; colour gradient turns; a synthetic emitter's measured spread and live count within
  ±10 % of numbers worked by hand.
- Live: import 1450958 (the pillar) and 1450976 (the orb) into the open GW2Porting project via
  kitwright, measure, and report the comparison table.
