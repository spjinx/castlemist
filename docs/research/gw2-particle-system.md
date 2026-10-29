---
name: gw2-particle-system
description: "GW2 baked particle clouds + effect lights live in MODL cloudData/lightData; emitter sim semantics (RE'd) + castlemist CPU billboard renderer"
metadata: 
  node_type: memory
  type: project
  originSessionId: 6a18ea30-a78b-4f11-8166-a57bb98aa152
  modified: 2026-07-25T10:05:30.423Z
---

GW2 bakes visual effects (fire/spark/smoke/infusion shimmer, glow lights, streaks, lightning) **into the model file** — `MODL` chunk → `ModelFileDataV*` fields: `cloudData`(ptr→`ModelCloudDataV*`), `lightData`(ptr→`ModelLightDataV*`), `streakData`, `lightningData`. They run on their own spawn timers, so they animate on an **idle, un-posed** model (answers "efek api senjata tetap berkobar saat diam").

**Data model (template `gw2_packfile.json`, all versions present):**
- `ModelCloudData` = `clouds[]`(`ModelParticleCloud`) + `emitters[]`(`ModelParticleEmitter`).
- `ModelParticleCloud`: `materialIndex`(→ model's own material = real game shader/texture for the particles), `bone`(token64 attach), `acceleration`(float3 gravity), `velocity`(float3), `drag`, `flags`, `fvf`, `emitterIndices`(array<dword> → into emitters[]), `obstacleIndices`.
- `ModelParticleEmitter`: `spawnPeriod`(float sec/spawn), `spawnProbability`, `spawnGroupSize`,`spawnRadius`,`lifetime`,`colorFalloff`(float2), `spawnShape`(byte), `velocity`/`acceleration`(**array[4] float2**), `colorBegin`/`colorEnd`(**array[2] float4**), `colorPeriod`, `opacityCurve`/`scaleCurve`(ptr→`ModelParticleCurve`{curveType byte, keys array<float2>=(t,val)}), `*Preset`(dword), `transform`(ptr→`ModelMatrix43`=3×float4 rows), `bone`, `planeEmitterSettings`(ptr), `meshEmitterSettings`(ptr).
- `PlaneEmitterSettings` (billboard): `alignmentType`(byte),`alignmentDir`(float3), `rotationInitial`/`rotationChange`(float2),`rotationDrag`, `scaleInitial`/`scaleChange`(array[2] float2 = 2 axes), `texCoordRect`(float4), **`flipbook`**(ptr→`ModelParticleFlipbook`{columns,count,rows,start bytes; fps float} = animated sprite atlas).
- `MeshEmitterSettings`: `filename`(mesh-particle model fileId), alignment, rotation/scale arrays, spin.
- `ModelLightData` = `effectLights[]`(`ModelEffectLight`{bone token64, color byte[3] RGB, intensity, nearDistance, farDistance}).

**Emitter SIM semantics — RE'd from `GrCloud.cpp` (IDA, Gw2-64):**
- Spawn loop `sub_140AC5A40` (asserts @0x141c6e/f, "index < m_cloudData->emitterCapacity"): accumulator `+= dt`; while `acc >= spawnPeriod`: if `rand01 <= spawnProbability` spawn a group of `round(sample(spawnGroupSize))`; `acc -= spawnPeriod`.
- **Range helper `sub_140E12DE0(float2 r, rng) = r.x + rand01()*r.y`** → the RUNTIME emitter float2 is **{base, span}** (base + U[0,1)*span).
  **CORRECTED 2026-10-22: the FILE stores {min, max}.** The loader (GrCloud.cpp, `FUN_140ac8c40` in the 2026-09 build) copies each
  stored pair as `base = file[i]`, `span = file[i+1] - file[i]`. Data agrees: equal pairs (a constant) are the commonest form
  (914/1692 scaleInitial, 734/846 spawnGroupSize in the Firebrand export). So read the sidecar/template values as {min, max};
  a reversed pair (spawnRadius [3, 1]) is just a negative span. The rest of this note's {base,span} wording is the runtime form. `sub_140E123C0`=U[0,1), `sub_140E12270`≈U[-1,1).
- Particle init `sub_140ACA4B0`: `spawnShape` switch (**0 cylinder, 1 disc, 2 line, 3 box, 4/5 mesh-surface, 6 point**) sets pos from `spawnRadius{base,span}`; **`velocity[4]` = (dirX,dirY,dirZ,speed) each {base,span}** → normalize(dir)*speed; `acceleration[4]` same layout (dir+magnitude); then transformed by emitter `transform` (4×3) + bone world pos; `+= cloud.acceleration`(gravity). **CORRECTED 2026-10-22:** `colorBegin`/`colorEnd` are `{min RGBA, max RGBA}` like every other range (the loader: base = A, span = B - A, `FUN_140ac8c40`; the spawn samples each channel `base + rand*span`, `FUN_140e1f0c0`). The one-colour layout (V≤66) is min = max; castlemist's reader wrote 0 for its max before (read as "random down to black").
- Pixel-exact match needs runtime globals (same limit as the deferred-lighting work) — the sim is faithful, not bit-exact.

**castlemist integration (DONE, 2026-07-15):**
- `gw2model.hpp`: `CloudSystem`/`ParticleCloud`/`ParticleEmitter`/`ParticleFlipbook`/`EffectLight` structs + `parseEffects`/`parseCloudData`/`parseEmitter`/`parsePlane`/`parseLightData` (template-driven `fOff`/`rdFloat2`/`rdCurve`; resolves versioned struct names from field `target`/`element`). `Model.effects`. CLI: `gw2dat_cli model` now emits an `effects` block; **`gw2dat_cli effscan --dat --template [--offset --count --min-size --limit]`** scans MODL entries for clouds/lights (finds candidates fast).
- `entry_extractor.h`: CPU mirrors `ParticleCloudCPU`/`EmitterCPU`/`EffectLightCPU` + `ModelPreview.clouds/emitters/effectLights` + `hasEffects()`; `build_model_preview` copies them and resolves each `bone` token64→joint index via `tokenizeBoneName`.
- `model_renderer.cpp`: CPU sim `capture_effects`/`update_particles(dt)`/`render_particles(mvp)` — xorshift RNG, per-emitter accumulators, spawn from `spawnShape` (surface-spawn samples model triangles by cumulative area), integrate vel/accel/drag, color/opacity/scale curves over life, flipbook UV (fps>0 → age*fps, else spread over life), **billboard** faces camera (model-space basis = columns 0/1 of `g_rot`), additive blend (`g_blendAdd` ONE/ONE, PS outputs premultiplied), depth-test on / write off, sampled from the cloud material's real texture (`g_mats[matIdx].srv`) or a procedural soft disc. Effect lights = additive glow discs at bone pos. Cap 6000 particles. `render()` ticks the sim every frame via a dedicated QPC anchor (`g_fx_qpc_last`) — independent of anim playback. Public API `set_show_effects/show_effects/has_effects/effect_stats`. Env `GW2_EFFECTS=0` off, `GW2_FXINTENSITY=<f>` scales additive brightness (default 0.55 — additive on the 8-bit backbuffer blows the dense emission core white; no HDR tonemap on the particle pass). Opacity default = bell envelope (fade-in 15%, fade-out) when no `opacityCurve`.
- `main.cpp`: **"Effects" toggle** (ID_EFFECTS_TOGGLE 2065, 2nd model toolbar row) + keeps `TIMER_ANIM` (16ms) running whenever `has_effects()&&show_effects()` (WM_TIMER repaints for idle animation). Headless `GW2_FXTEST` (+GW2_AUTOLOAD) warms the sim over real time → `shot_fx_a/b.bmp` (Full) + `shot_fx_shader.bmp` (GameShader) + `fxtest.txt` counts.

**Test models (Steam Gw2.dat, use PHYSICAL index = baseId−1; note this dat build: fileId lookups for 553679/556048 fail, use --index):** **idx 200170** (base 200171) = 2 clouds/3 plane emitters, **flipbook 2×2 fire embers** spawnShape 4 (surface), dark-red, upward speed 20 — the ideal animated-fire test. **idx 200152** = 17 clouds/52 emitters + 1 light. Effects are pervasive (~most character/weapon MODLs in the 200k range have clouds). See [[castlemist-app]], [[gw2-shaders-dxbc]], [[gw2-skeleton]].

**TWO emitter layouts (2026-07-15, fixing sand-dog fileId 1822155 / idx 557542):** `ModelParticleEmitter` **V≤66** keeps plane/flipbook/scale/rotation/alignment + **wind** fields DIRECTLY on the emitter, and `colorBegin`/`colorEnd` = a **single float4** (no random span). **V≥67** moved them into a `planeEmitterSettings` sub-struct and made colorBegin/End `array[2] float4` ({base,span}). `parseEmitter` now handles both: detects the color field `kind` (array vs float4), and if there's no `planeEmitterSettings` field but the emitter has `scaleInitial`, calls `parsePlane(emitterType, emitterBase)` to read the inline plane fields. Model 200170 = V≥67, most creatures (1822155) = V≤66 — the old parser silently produced `hasPlane=false`/no scale/no flipbook.

**WIND (sand/dust/leaves blown by wind):** emitters carry `spawnWindSpeed`/`spawnWindEmit`(float2 {base,span}) + `windInfluence`(byte, e.g. 25) + the model has `windData`(→`ModelEffectWind`). Also **`cloud.velocity`(float3) = constant drift** (was parsed but never applied!). Sim now: at spawn adds `cloud.velocity` + `windDir*sample(spawnWindSpeed)`; per frame adds a gusting `windDir*g_fxWindGust*g_radius*(0.55+0.45*sin(t*1.9+phase))*windInfluence/255*dt` for `windInfluence>0`. `g_fxWindDir` model-space (GW2 Z-up, default ~(0.94,0.33,0.10)); env `GW2_WIND` (gust scale), `GW2_WINDDIR="x,y,z"`.

**DISTORTION/HEAT-HAZE emitters skipped:** GW2 refraction emitters (Branded shimmer, heat haze) store a **flat surface normal ~(0,0,1)→(1,0,0) as their "color"** (they perturb the framebuffer, not add light — invisible in-game). Additive rendering drew them as giant purple/blue streaks. `fxIsDistortion(e)` (begin b>0.8,r&g<0.2 AND end r>0.8,g<0.2) skips spawning them. Sand-dog 1822155: 20 emitters = 6 visible dust (grey 0.47, flipbook 2×2, scale 0.5-2, windInfluence 25) + 2 teal + **12 distortion** (blue, scale 30-80, wind speed 100) now suppressed.

**"KEMBANG API" (fireworks) LOOK FIXED — texCoordRect UV (2026-07-25).** ROOT CAUSE: `render_particles` computed the billboard UV as the FULL texture `[0,1]` (only subdividing when a flipbook was present), **ignoring the emitter's `texCoordRect`**. GW2 particle textures are usually shared ATLASES and each emitter's `texCoordRect(u0,v0,u1,v1)` picks the ONE sprite it uses — so drawing full [0,1] showed the whole atlas (every flame/spark/glow sprite at once) per billboard, additively, on outward-moving particles = chaotic fireworks. FIX (model_renderer.cpp): base UV = `e.texCoordRect` (degenerate/empty → full [0,1]); flipbook cells now subdivide WITHIN that rect (`fw=(ru1-ru0)/columns`, etc.). **texCoordRect is (u0,v0,u1,v1) not (x,y,w,h)** — proven: Balthazar 556048 emitter0=(0.5,0.5,1,1) (bottom-right quarter; as x,y,w,h it'd overflow u>1), emitter2=(0.45,0,0.5,0.25) thin strip. `texCoordRect` was already parsed (gw2model em.texCoordRect line 1378) + copied to the CPU struct (entry_extractor.cpp:819) — only the draw ignored it. NB most Balthazar emitters have velocity mostly +Y (upward flame) + empty flipbook (static sub-rect sprite), so correct UV alone makes them read as fire, not sparks. Also earlier: particle PS got a soft-edge radial falloff (quad-local `luv` TEXCOORD1) to round hard billboard squares. Verify: parse a MODL via gw2mcp (extract by **`index=`**, its base_id is off-by-one vs mftIndex) and read emitters[].texCoordRect.

**PARTICLE SHADERS, read from the game (2026-10-22, skill-effects worktree).** `gw2dat_cli amat --file-id <AMAT> --out-dir <d> --dump-all` dumps every VS/PS of an AMAT; `AMATDBG=1` lists each technique / pass / effect with its bgfx blend. The particle vertex is **POSITION, NORMAL, COLOR, TEXCOORD0..3**; a flipbook variant (effect 7) reads uv1 as the next frame and uv2.x as the frame blend. A small set of pixel-shader families draws the Firebrand particles, each with `soft = smoothstep01(saturate((sceneZ - z) / D))` (D = `depfade` / `depdist`, else a literal 17) and `x (1 - fog)`:
- soft additive 19092 / 54360 / 25489 / 43029 (One / InvSrcColor): `a = T.a vc.a soft`, `rgb = 2 T vc a`.
- premultiplied 20760 / 53260 (One / InvSrcAlpha): `soft (2 T vc.rgb vc.a, T.a vc.a)`.
- step dissolve 19116 / 14196 / 48767: the vertex alpha dissolves the sprite through `step` (luminance and alpha both stepped), x `diffade` or x vc.r.
- alpha-test fade 1601524 (pass 0) / 19255: `a = T.a vc.a soft`, discard below AlphaRef, rgb = T (1601524 ignores the vertex colour). **1601524 has a pass 1** (SrcAlpha / One): the page's burning edge from its `add` map (`addcolr`, `stepmin` vs vc.r).
- flow 882183 / 1710790: the vertex colour carries the parameters (r: flow strength, g/b: alpha thresholds), `startcl`/`endcl` by a stepped alpha.
- fire 977200 / 1171331: as GW2Core.cginc's fire block (checked term by term against 977200 ps 34).
- flow layer 1764372: a scrolled `opacitytile` layer, distorted by a screen-space flow scaled by the camera distance.
Ported as `Castlemist/GW2 Particle` (tools/unity/Castlemist/Shaders/GW2Particle.cginc).

**texCoordRect's v runs from the image BOTTOM** (2026-10-22): on atlas 1653632 (the Tome book's `Mat_19092_2`), SpawnFlare's rect v 0.37-0.5 is the star-flare strip only when measured from the bottom; Flash (2x2 flipbook) lands on the 2x2 sparkle tile, Glow on the round glow, LightBms on the cone beam. Measured from the top they hit quarter discs and the cone's edge. The Unity import's offset is therefore `(u0, v0)`, not `(u0, 1 - v1)` (the Balthazar rect above is symmetric in v and could not tell).

**Not yet done:** mesh-particle emitters (spawnShape 4/5 with `meshEmitterSettings.filename` — currently rendered as billboards, not the referenced mesh); per-cloud blend modes (alpha vs additive from `cloud.flags`); streaks/lightning; infusion COMPOSITING (overlay another item's cloudData onto a character); dye/skin color-palette recoloring. Alignment types (velocity-aligned/axis) approximated as camera-facing.

## Amendment 2026-10-22: the client's emitter semantics (Ghidra, GrCloud.cpp, 2026-09 build)

Read in Gw2-64.exe with the `~/gw2re` Ghidra project (functions created in memory; the region
0x140ac4000-0x140ace000 had none). The Unity side (tools/unity/Castlemist/Editor/ParticleEffects.cs)
follows this.

- **File record** `ModelParticleEmitterV70` is 0x132 bytes; the loader `FUN_140ceb0b0` copies it into a
  definition, and `FUN_140ac8c40` builds the runtime emitter with `base = a, span = b - a` for every
  pair: **stored pairs are {min, max}**; `FUN_140e1f0c0` samples `base + rand * span`.
  spawnProbability x 0.01 (a percent); rotations x pi/180 (degrees); windInfluence x 100/255.
- **spawnShape** (file value, through the table at 0x141ee8e00): 0 disc (radius min..max), 1 line
  (along X, max long, min thick), 2 mesh surface, 3 flat rectangle (half-sizes max x min), 4 sphere
  (radius min..max), 5 cylinder (ring at min, up to max high), 6 mesh. `FUN_140ace900`.
- **Velocity / acceleration**: normalize(direction sample) x magnitude sample; no velocity at all
  when every direction range is 0. velocityDistRange / accelerationDistRange add a radial term.
- **Sizes** are half-sizes (bounds = position +- size); scaleInitial[0] X, [1] Y; Y = X when the
  runtime flag 0x800 is set, which is **file `flags` bit 0x10** (`FUN_140d52160` permutes the file
  bits: file 0x4 -> 0x10, 0x10 -> 0x800, 0x1000 -> 0x20). scaleChange adds per second.
- **File `flags` 0x4**: no transform at spawn, positions drawn through the cloud matrix, and sizes
  *not* multiplied by its scale; without it spawn position / velocity go to world space and every
  size is multiplied by |transform row|. 0x1000: random sign on the spin.
- **emitterFlags** (file -> runtime, `FUN_140d51e90`): 0x8 burst: one group each time the emitter's
  bone turns visible (`FUN_140acc370`), nothing else; 0x4 / 0x2: the rate scales with the wind,
  remapped from spawnWindSpeed to spawnWindEmit (`FUN_140acf2e0`); 0x40 / 0x80: kept only below /
  above a height; 0x100 / 0x200 / 0x20: the near / mid / far distance tiers it emits in.
- **Active** = the bone's visibility bit (model instance `FUN_140cd7240`). Each clip's
  `visTrackData` holds, per bone, the times it toggles at (seconds, frame-aligned at 30 fps);
  castlemist reads them as visible at the clip's start, each key toggling: fire 1727682's `on04`
  has no keys and `off04` keys 0 alone; the comet 3560854's `Straight` keys 0, 0.33, 4.33 (a 4 s
  flight in a 66.7 s clip; `StraightB` 53.7..57.7); `zeropose` keys 0 on nearly every effect bone.
  The track holds only the bone and the times, no start state. Exported per clip in the
  sidecar's `clips`.
- **Curves** are baked to 32 bytes, values clamped 0..1 (`FUN_140acbe30`); the opacity / scale curve
  *presets* are stored raw and evaluated on the GPU: not transcribed.
- Capacity is max life / period x max group x probability: no budget cap in the client.

## Amendment 2026-10-23: colour period, offset

Read from Gw2-64.exe.
- **The emitter struct in memory** follows the schema's (alphabetical) field order:
  `FUN_140ceb0b0` copies V70's dwords in that order.
- **Runtime block:** `colorFalloff` lands at `+0x10`, `colorPeriod` at `+0x18` and `offset` at
  `+0x20` of the emitter's second block (`plVar8[1]`, filled by `FUN_140ac8c40`).
- **Colour** (`FUN_140ad3a30`, called by the vertex builder `FUN_140ad11b0`). The age `t` is the
  life fraction `1 - remaining / total`, not seconds. With `m = t mod period` and
  `n = floor(t / period)`:
  - `x = m / period` on even `n`, else `1 - m / period` (a ping-pong; `x = 0` with period 0);
  - `w = clamp((x - falloff.x) / (falloff.y - falloff.x))` (`FUN_140acf2e0`);
  - colour = `begin * (1 - w) + end * w` on all four bytes, alpha included, then alpha times
    the opacity curve.

  So `colorPeriod` 1 with falloff (0, 1) is one sweep over the life, 0.167 bounces about six
  times (the embers' flicker), and 2 only gets halfway. The sidecar key is now `colorPeriod`; it
  was `colorPeriodSec`.
- **Offset** (`FUN_140ad11b0`): the quad centre is `position + offset * normalize(position - camera)`
  in world units, unscaled. A negative value pulls the sprite toward the camera, a depth bias.
  ParticleEffects maps it to the renderer's pivot z (in particle sizes; positive is toward the
  camera, measured).
- **Particle vertices are built on the CPU.** The AMAT vertex shader only transforms them and
  passes COLOR through.
- **Velocity without file flag 0x4:** spawn (`FUN_140ace900`) multiplies position and velocity by
  the emitter's full 3x3 world matrix, scale included, with no normalisation. A prop placed at
  x64 (the orrery 3124726) throws its sparks 64 times as far.


## Amendment 2026-10-28: curve presets, flags, sprite alignment, mesh particles

Read in the `gw2re` Ghidra project, which holds the **2026-09 build**: the Gw2-64.exe now on
disk is newer and its addresses differ (the bytes at 0x1409c61c0 there are another function's
epilogue). Every address below is the project's. Coded in `castlemist::fx` (src/fx/fx_math.cpp).

**Curve presets.** `FUN_1409c61c0(id, t)` jumps through an 18-entry table at 0x14257d870
(read with tools/ghidra/ReadMem.java; the on-disk table differs). With the constants read from
0x141b90918..0x141b9096c:

| id | f(t) | function |
|---|---|---|
| 0 | 1 | 0x14029cdc0 |
| 1 | t (a bare `ret`, Ghidra names it `_guard_check_icall`) | 0x140163990 |
| 2 | A = -1.0347t + 21.174t² - 40.278t³ + 20.139t⁴ (a bump, 0 at both ends) | 0x1409c4930 |
| 3, 4, 5 | 0.75A + 0.25, 0.5A + 0.5, 0.25A + 0.75 | 0x1409c4980, 49e0, 4a40 |
| 6 | 8.0606t - 20.059t² + 16.354t³ - 4.3561t⁴ (bump) | 0x1409c4aa0 |
| 7 | 9.3333t - 30.667t² + 42.667t³ - 21.333t⁴ (bump) | 0x1409c4b30 |
| 8 | 6.3333t - 11.4t² + 5.0667t³ (bump) | 0x1409c4af0 |
| 9, 10 | t², t⁴ | 0x1409c4d50, 4be0 |
| 11 | E = -0.6t + 4.8t² - 3.2t³ (ease, ends at 1) | 0x1409c4bf0 |
| 12, 13, 14 | 0.75E + 0.25, 0.5E + 0.5, 0.25E + 0.75 | 0x1409c4c30, 4c80, 4cd0 |
| 15, 16, 17 | (1-\|2t-1\|)², ⁴, ¹ | 0x1409c4b80, 4bb0, 4d20 |

A stored curve wins over the preset: the builders take the preset only when the baked curve's
key count (`+0x42`) is 0 (`FUN_140ad3920` samples the baked curve at `(count - 1) t`).

**Emitter `flags`, file → runtime** (`FUN_140d52160`): runtime bit k = file bit
{19, 0, 1, 6, 2, 12, 8, 7, 11, 10, 9, 4, 18, 16, 17, 13, 5, 3, 20, 21}[k]; bits 22+ are dropped.
It reproduces the three mappings of 2026-10-22 (0x4 → 0x10, 0x10 → 0x800, 0x1000 → 0x20) and adds:
- file 0x80 (runtime 0x80): opacity preset becomes 1 - value (`FUN_140ad3a30`);
- file 0x400 (runtime 0x200): the scale preset runs backwards, t → 1 - t (`FUN_140acf880`);
- file 0x200 (runtime 0x400): the scale preset becomes 1 - value (`FUN_140acf880`, `FUN_140ad11b0`);
- file 0x80000 (runtime 0x1): a facing test in the sprite builder sets a per-quad flag (meaning not read);
- file 0x20 (runtime 0x10000): when **clear**, the sprite axes are transformed by the 3x4 at
  `pfVar20` (with its inverse-scale fix-up); when set they are not (which space that leaves them
  in is not read).

**File → definition copy** (`FUN_140ceb0b0`): `planeEmitterSettings` @+0x122, `meshEmitterSettings`
@+0x12a in the V70 record; a mesh emitter sets runtime flag 0x1000. Plane fields land at
definition dwords 0x45 (alignment case), 0x46..0x48 (alignmentDir, normalised by
`FUN_140ac8c40`), 0x49 rotationDrag, 0x4a/0x4b rotationInitial and 0x4c/0x4d rotationChange
(both × π/180), 0x4e..0x51 scaleInitial, 0x52..0x55 scaleChange, 0x56..0x59 texCoordRect —
the template's field order.

**Sprite alignment.** The plane's `alignmentType` byte is remapped by `FUN_140d52110` (table
0x141ee8df0): **file 0→1, 1→5, 2→4, 3→2, 4→0, 5→3** (past 5 asserts). The sprite builder
`FUN_140ad11b0` then branches on the remapped case (particle record: position +0x84, a vector
+0x90 that the cases treat as the motion direction, sizes +0xa8/+0xac clamped at 0, remaining /
total life +0xb8/+0xbc, runtime flags +0xd4):
- **case 1 (file 0, the common one): camera-facing.** Half-axes in the view plane
  u = (cos r, sin r)·sx, v = (-sin r, cos r)·sy (`FUN_140e56c60` = cos, `FUN_140e57160` = sin:
  the latter returns its argument for tiny inputs). `fx::camera_sprite_axes`.
- **case 0 (file 4): fixed plane.** Built from `alignmentDir` transformed by the matrix at
  +0x20..+0x48 of the cloud instance, with the up axis swapped when the direction is ±Z, then
  rotated by r.
- **case 2 / 3 (file 3 / 5): faces the motion direction**, `+0x90` normalised (falling back to
  the position term when it is near 0; ±Z handled as in case 0); case 3 negates it.
- **case 4 (file 2): along the motion direction on screen** — `+0x90` projected on the camera's
  first two rows, normalised (a zero projection gives the default axes).
- **case 5 (file 1): an axis billboard** along `+0x90`, turned about that axis to face the
  camera position (cross product with the eye vector).
Cases 0 and 2–5 are read as above but not yet coded (B codes each as its sim needs it).

**Mesh particles** (`FUN_140acf880`, record stride 0x80): life fraction t = 1 - remaining/total
(+0x54 / +0x58, clamped); orientation = the particle quaternion (+0x5c) times a **bank** rotation
about X when the angle between the motion direction (+0x24, + the +0x30 term) and +0x30 passes
`bankThreshold` (emitter +0x50): bank = (angle - threshold) × 0.5 × `bankScale` (emitter +0x54),
clamped; then the mesh `alignment` (`FUN_140d52030`: 0, 1, 2 pass through; others assert):
0 none, 1 yaw-then-pitch toward the velocity +0x24, 2 yaw only (velocity's XY). Scale = the
scale curve or preset (flags above) × emitter +0x1c × the particle's xyz size (+0x3c..+0x44);
`FUN_1409c8cf0` composes scale, position (+0x18) and rotation. Colour from `FUN_140ad3a30`.

## Amendment 2026-10-28 (2): the random generator, lightning

**The engine's random numbers** (`FUN_140e1e6a0`, per-thread state at TLS +0x68 or a passed
one): MINSTD — `v = 48271*s - (2^31-1)*(s / 44488)` in 32-bit arithmetic; when v > 0x7fffffff
the client adds **0x80000000** (Schrage's method adds 2^31 - 1, so the client's sequence differs
from textbook MINSTD after every negative step); v = 0 becomes 0x75bd924; u = v * 2^-31 (the
constant at 0x1420b3070). The range sampler `FUN_140e1f110` returns `base` **without drawing**
when `span` is 0, so constants do not advance the sequence. `fx::Rng`, `fx::sample`. (Which
seed each emitter starts from is not read; B seeds its own.)

**Lightning** (GrLightning.cpp, GrLightningJob.cpp):
- **Bolt creation** `FUN_140b13cf0` walks the node tree from the root, depth < 32: every child
  whose node flags bit 0 is clear and whose `probability` (node def +0x68) passes a draw
  (u <= p) is followed, so a bolt forks along every passing path; each root-to-leaf path of
  >= 2 nodes becomes one bolt instance. Its lifetime is sampled from the bolt's `lifetime`
  range, started `dt` early; a texture offset is u × (bolt +0x3c); a second sample from the
  range at +0x1c and one from {+0x54, +0x58} (as a {min, max} pair) are kept per instance
  (which bolt fields these are is not settled by this read).
- **Node points**: by node `shape` (+0x28): **0** a point on a disc, angle 2πu drawn first,
  radius sampled from `radius` (`fx::lightning_disc_offset`); **1** a random point from
  `FUN_140e1f170` with z forced to -|z| (the lower half); **2** the same without the fold
  (`FUN_140e1f170`'s distribution is not read). The point is moved by the node bone's 3x4.
- **Path** `FUN_140c08e70`: the bolt at t over its node points, **mode 0 linear, mode 1
  Catmull-Rom** (constants 0.5, 3, 2, 5) with the end points repeated, returning position and
  unit tangent; t past the last node gives the last node. `fx::bolt_path_point`. Which field
  picks the mode (the bolt's `type` is the candidate) is not proven.
- Not read: the per-segment displacement (`variance`, `noise`, `numSegments`), the strip
  build, thickness and opacity over life.

**Streaks** (GrStreak.cpp 0x14155e170..0x14155fc80, GrStreakJobCommon.cpp 0x141562eb0..):
decompiled, not yet read.

## Amendment 2026-10-28 (3): the integrator, spin drag, the live cap

Found from the call graph (tools/ghidra/FindCallers.java): the cloud update `FUN_140ac9530` calls,
per emitter with live particles, `FUN_140acb950` (sprites) or `FUN_140acac60` (mesh particles,
runtime flag 0x1000), then the spawn loop `FUN_140ac9e90`.

- **Per sprite particle** (`FUN_140acb950`, record stride 0x6c): remaining life (+0x4c) -= dt
  (a particle flag 0x8000 scales dt by a wind-rate term); expired particles are swapped out;
  sizes grow linearly, `size += scaleChange * dt`; **spin drag**: when the emitter's rotation
  drag is set, `spin' = (1 - rotationDrag * dt) * spin`, kept only if it has the same sign
  (else 0: it stops, never reverses); `rotation += spin * dt`.
- **Motion** (`FUN_140acb1d0`), in order:
  1. wind (emitter wind influence x the environment wind): `a += w*k - proj_v(w)` while the
     along-velocity component is under `k` (with no velocity, `a += w*k`); a cloud-instance flag
     8 drops the wind's z;
  2. **`pos += v dt + a dt^2 / 2`, `v += a dt`** (exact constant-acceleration step, 0.5 at
     0x141923c38), a = the particle's own acceleration;
  3. a noise field (emitter +0xdc strength, +0xec scale; `FUN_140ddfa40`) moves the position;
  4. **drag** d = cloud drag + emitter drag: `v *= 1 - d dt`; if the result's squared speed is
     under 1e-4 (0x141b87a94) the velocity keeps its direction at speed 1e-4;
  5. obstacles (`FUN_140ad3020`) when the cloud and particle flags ask; height cut-offs
     (runtime flags 0x100 / 0x200) kill particles above / below.
  `fx::integrate` is steps 2 and 4.
- **Live cap**: 0x4000 particles per cloud instance; past it the update logs "Cloud particle
  count ({} total, {} mesh, {} quad) exceeds maximum allowed ({})" and trims.

## Amendment 2026-10-28 (4): streaks, first read

`FUN_14155fc80(streak, ring, ?, dt, offset, scale)` (GrStreak.cpp), per streak per frame, dt
clamped to the constant at 0x141932f24:
- **Ring**: points of 0x4c bytes in a ring (head +0x3e, count +0x3c, capacity +0x3a, grown to a
  power-of-two byte size); points whose remaining life (+0x34) is <= 0 are retired from the tail
  first. Capacity cap 0x8000 points.
- **Anchors** (0x8c bytes each, `ModelStreakAnchor`): each frame the largest distance any anchor
  moved is measured (anchors with flag 4 kept apart from the rest); with the passed offset it is
  the frame's travel.
- **Emission**: the travel is added to an accumulator (streak +0x50); while it is positive, or a
  forced flag is set, a **row of points, one per anchor**, is emitted at the interpolated
  fraction f of this frame's move (clamped 0..1). Each point: remaining life = the anchor's
  lifetime - dt f; colour from the anchor (start/end bytes; anchor flag 4 clear zeroes two of
  them); texV and the anchor's other two values copied; the texture u = lerp(previous u, current
  u, f) unless ring flag 2. A streak flag 1 leaves the last anchor out of the row.
- Not yet read: the accumulator's decrement (the spacing, `spawnDist` the candidate), how
  `jitter` / `noise` / `wind` / `velocity` / `acceleration` move points, the strip build in
  `FUN_141562eb0`, colour over life.

## Amendment 2026-10-29: streaks, motion and spacing (FUN_14155fc80, second read)

- **Spacing**: after each row of points the travel accumulator drops by `scale x` the ring's
  spacing value (ring +0x10; `spawnDist` is the field that lands there — candidate, not traced
  through the loader); rows keep coming while the accumulator stays above a small epsilon, so a
  row is laid every spacing of anchor travel. A forced row (first frame, or a reset flag) sets
  the accumulator to 0.
- **A new point's velocity** (+0x18..+0x20): with streak flag 0x10, the anchor's own velocity
  (its position delta / dt) is added; when the streak's noise value (streak +0x68) is > 0, a
  random vector (`FUN_140e1ec30`) is low-pass smoothed per anchor (factor the constant at
  0x141932f24) and added times the noise value times a constant. Without either, points start
  still. A point emitted mid-frame copies the previous row's state where the anchor did not move.
- **Each live point, each frame**: `life -= dt` (floored at 0; the streak keeps the largest
  remaining life); `vel += dt * (acceleration + wind x windFactor)` — the streak's `acceleration`
  (+0x20..+0x28) plus the passed wind vector times the streak's wind value (+0x6c); then
  `pos += vel dt` plus the frame's origin offset.
- Points carry a flag 0x40000000 when they close a strip (the last row before a break).
- Still not read: the strip build (`FUN_141562eb0`: width, texture u along the strip, colour
  start -> end by life with `falloff`) and the `jitter` field.
