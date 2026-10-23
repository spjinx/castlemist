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
- **Range helper `sub_140E12DE0(float2 r, rng) = r.x + rand01()*r.y`** → EVERY emitter float2 is **{base, span}** (base + U[0,1)*span), NOT {min,max}. `sub_140E123C0`=U[0,1), `sub_140E12270`≈U[-1,1).
- Particle init `sub_140ACA4B0`: `spawnShape` switch (**0 cylinder, 1 disc, 2 line, 3 box, 4/5 mesh-surface, 6 point**) sets pos from `spawnRadius{base,span}`; **`velocity[4]` = (dirX,dirY,dirZ,speed) each {base,span}** → normalize(dir)*speed; `acceleration[4]` same layout (dir+magnitude); then transformed by emitter `transform` (4×3) + bone world pos; `+= cloud.acceleration`(gravity). `colorBegin[0]`=base RGBA, `colorBegin[1]`=random span (color = base + rand*span per channel).
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

