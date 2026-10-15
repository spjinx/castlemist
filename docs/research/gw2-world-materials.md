---
name: gw2-world-materials
description: "What a GW2 map's surfaces are painted and moved with: the terrain shaders' blend of the picker/modx pages and four ground textures (read off the game's DXBC), the UV routing per terrain AMAT, what pickerScale/pickerOffset/uvScale/tiling/uvData mean and what is still unknown, what the existing game-shader path would need to draw terrain, prop textures at map scale, UV scroll in MODL texTransforms, animated props' animSequence -> clip token, and what water the dat describes. Research for World Engine sub-project 3; anything unproven is marked UNPROVEN."
---

# GW2 world materials and motion

Research for World Engine sub-project 3, "World materials and motion"
(roadmap: `docs/superpowers/specs/2026-10-08-world-data-core-design.md`;
design: `docs/superpowers/specs/2026-10-14-world-materials-design.md`). Done
2026-10-14. It builds on `gw2-world-frame.md` (§4 terrain materials, §5
props, §6 water) and does not repeat what that note proves. Same rule: every
claim has its evidence, or is marked **UNPROVEN** with what would prove it.

## Evidence base

- **Test maps:** Queensdale 192711, Lion's Arch 191000, Spirit Vale 1151420
  (as `gw2-world-frame.md`).
- **Only castlemist's own tools opened the dat.** Build used:
  `.claude/worktrees/world-map-window/build/debug/bin/gw2dat_cli.exe`
  (MinGW runtime from `C:\msys64\ucrt64\bin`), template
  `dumps/packfile/gw2_packfile.json`. Every output went to a scratch folder
  under `%TEMP%`; nothing derived from the dat is committed.
- **Shaders.** `gw2dat_cli amat --dat <dat> --template <tpl> --file-id <AMAT>
  --out-dir <tmp>` writes the colour-pass VS and PS that castlemist's
  `extractAmat` selects (no material token passed, so the pass-0 default
  effect; `gw2-amat-draw-state.md`) as bgfx blobs. The DXBC inside each blob was
  disassembled with `d3dcompiler_47!D3DDisassemble` (a 30-line Python ctypes
  script, not committed), and the bgfx uniform table (name, register byte
  offset; layout as `parse_bgfx`, `src/extract/game_shader.cpp:37-64`) read
  from the blob header so `cb0[n]` reads as a name.
- **Generic parses.** `gw2dat_cli parse --file-id <id> --max-depth <d> --out
  <tmp>` for the maps (depth 6-8, 350-450 MB each) and for prop models (depth
  9). Small Python scripts walked the JSON trees (not committed). A census
  script ran `parse` over the 33 distinct `propAnimArray` models of Spirit
  Vale plus a random 60 of its static prop models (29 of those 60 produced no
  output from the generic parser; not investigated) and counted clip tokens,
  `texTransforms`, `uvTransLinks`, non-zero `uvAnimId`, `uvAnimData` and
  `matConstAnim`.
- **Template** (`dumps/packfile/gw2_packfile.json`) for every struct named
  below; field names are quoted as the template spells them.
- **T3D** (`C:\Users\vital\Downloads\t3d\t3d`, commit `b3428b2`, GPL-3) was
  read to compare behaviour only: `library/src/util/TerrainShader.ts`,
  `library/src/dataRenderer/TerrainRenderer.ts`. No code is copied.

---

## 1. The terrain shader: how the four ground textures are blended

**PROVEN from the game's DXBC (AMAT 187795, the lo-res terrain material of
Queensdale and Lion's Arch, PS 48):** the picker page (`blend`) is sampled
on the page UV and gives four weights; each weight is multiplied by its
ground texture's alpha + 0.01 and the four are normalised to sum 1; the
blended colour is desaturated/resaturated by `modx.G`, scaled by `modx.R`
and doubled; the four normal maps are blended with the same weights. The
result is lit by the deferred light buffer, gets a `modx.B` fresnel rim, and
is fogged.

### 1.1 Which texture is in which slot

The selected PS samples slots 0-9 with `textureIndex = slot` (0..9), plus slot
14 (`textureIndex` 34, the light buffer; `gw2-amat-shader-roles.md`). The hi-res
PS (AMAT 187794, PS 100) adds slot 13 (`textureIndex` 33, the env cube).

`textureIndex` indexes the material's texture list (`gw2-uniform-hash.md`,
"TEXTURE->SAMPLER binding"). For a terrain chunk that list is, by position,
the chunk's `loResMaterial.texIndexArray` (`gw2-world-frame.md` §4.1): on
Queensdale and Lion's Arch `color, colorb, colorc, colord, blend, modx,
normal, normalb, normalc, normald`. The shader agrees with that order on every
point it can be checked:

- slots 4 and 5 (`blend`, `modx`) are the only colour-sized textures sampled
  on the second UV pair (the page UV, §1.5); slots 0-3 and 6-9 share the
  other;
- slots 6-9 are decoded `*2 - 1` and rotated by the tangent frame (normal
  maps), and slot 6+k is weighted by exactly the weight of slot k.

That terrain materials bind by **position** in `texIndexArray`, as MODL
materials do by position in `textures[]`, is therefore consistent with the
shader but **UNPROVEN in client code** (proof: the client function that builds a
terrain chunk's material from `PackMapTerrainMaterial.texIndexArray`). It
matters for a 1:1 path only: `TerrainMaterial` (world_scene.h:17-39) stores
textures by token, not position, which is enough for a reconstruction but not
for binding the game's samplers (on 25 Spirit Vale chunks position 7 is
`normalc`, not `normalb`, and position 9 is `ramp`).

### 1.2 The blend (lo-res PS 48, transcribed)

```
w   = blend.rgba (page UV)            ; if w == 0 everywhere: w = (0,0,0,1/255)
h   = (colord.a, colorc.a, colorb.a, color.a) + 0.01        ; ground UV
k   = w * h / dot(w, h)                                      ; weights, sum 1
alb = k.w*color.rgb + k.z*colorb.rgb + k.y*colorc.rgb + k.x*colord.rgb
lum = dot(alb, (0.30, 0.59, 0.11))
alb = lum + (alb - lum) * (2 * modx.g)     ; saturation, 0.5 = unchanged
alb = 2 * modx.r * alb                     ; brightness, 0.5 = unchanged
nrm = k.w*normal + k.z*normalb + k.y*normalc + k.x*normald   ; RGBA, same k
```

So the picker channels map **R -> `colord`, G -> `colorc`, B -> `colorb`,
A -> `color`**, and an all-zero picker texel paints `color` alone. The ground
textures' **alpha is a blend height** (it biases the weight, it is not
opacity). This is a weighted average, not a hard height-blend: `+0.01` keeps a
texture with alpha 0 from vanishing.

Lighting and the rest of PS 48 (`cb0[0]` LightBuffer, `cb0[1]` StencilId,
`cb0[2]` ScreenDims, `cb0[3]` shSunColor, `cb0[4]` TexelOffset):

```
N    = normalize(TBN * normalize(nrm.xyz * 2 - 1))   ; tangent x, bitangent y, normal z
L    = LightBuffer.y * lightbuffer[(SV_Position.xy + TexelOffset.x) * ScreenDims.xy]
spec = nrm.a * 0.854902 * shSunColor.rgb * L.a       ; normal alpha = specular mask
rim  = saturate(2*modx.b - 1) * 0.239216 * shSunColor.rgb * shSunColor.w
fres = (1 - saturate(dot(N, V)))^8
rgb  = alb * L.rgb + spec + rim * fres
out  = rgb * (1 - fog.w) + fog.rgb ;  out.a = StencilId.x
```

`modx` therefore carries three things: **R brightness, G saturation, B a
fresnel rim (sheen) gate above 0.5**. The hi-res PS 100 (AMAT 187794) uses the
same weights, the same `modx.R/G` albedo rule and the same blended normal, and
replaces the rest with a heavier model: a roughness from the blended normal
alpha (`1 - smoothstep(0, 0.9, a)` clamped to 0.3-0.8), an env-cube (slot 13)
reflection at a roughness mip, SH ambient (`shRed/shGreen/shBlue`) plus
per-vertex local lights (`LightCount`, `LightPointAndSpotData` in its VS) and a
sun specular through `shSunData`; it does not read `modx.B`. Those details were
read off the disassembly, not transcribed term by term.

Spirit Vale's main pair (184988 / 184989, 208 chunks) has the same structure
(PS 73 of 184989 samples the same slots, plus slot 13).

### 1.3 Agreement with T3D

T3D's terrain fragment shader (`TerrainShader.ts:23-69`) uses the same channel
mapping (`tp1.a -> texture1 ... tp1.r -> texture4`) and the same
`blend` / `modx` page layout. Its blend function is not the game's: it adds
`1.5 + texture.a` to `4 * weight`, keeps only what lies within 2.0 of the
maximum (a sharpened height blend) and normalises; and it applies `modx` as
`color *= 0.5 + modx.r` (the game: `2 * modx.r`, plus the `modx.g`
saturation). T3D ignores the normal maps, `modx.b` and the specular mask. A
castlemist reconstruction should follow §1.2, not T3D.

### 1.4 What the lighting input is

The terrain colour pass is **deferred**: it multiplies the albedo by the
screen-space light buffer (slot 14) and does not compute sun or ambient itself
(lo-res). castlemist's main window already rebuilds such a buffer for the game
shaders (`gw2-shaders-dxbc.md`, "LIGHT PRE-PASS RECONSTRUCTED";
`src/render/lighting.cpp`). A forward reconstruction gets the same image by
computing `L.rgb = ambient(N) + sunColor * saturate(dot(N, sunDir))` and
`L.a` as the sun specular term in the pixel shader, from
`WorldScene::environment.light` (`gw2-map-lighting.md`).

### 1.5 UV routing: which vertex UV feeds which texture (PROVEN per AMAT)

The terrain VS takes `POSITION, NORMAL, TANGENT, BITANGENT, TEXCOORD0,
TEXCOORD1` (all float) and passes each UV set through its own `TexTransform`
pair: `uv' = (dot(A, (u, v, 1, 1)), dot(B, (u, v, 1, 1)))` with `A =
TexTransformNA`, `B = TexTransformNB` (an affine 2x3 per set; VS of 187795:
`TexTransform0A/B = cb0[21..22]`, `1A/B = cb0[23..24]`). Read off the four
terrain AMATs on the test maps:

| AMAT (lo-res) | chunks | pages (`blend`, `modx`) | `color` + `normal` (slot 0, 6) | other ground textures |
|---|---|---|---|---|
| 187795 | Queensdale, Lion's Arch (all) | TEXCOORD0 · TexTransform0 | TEXCOORD1 · TexTransform1 | TEXCOORD1 · TexTransform1 |
| 184989 | Spirit Vale 208 | TEXCOORD0 · TexTransform0 | TEXCOORD1 · TexTransform1 | TEXCOORD1 · TexTransform1 |
| 197591 | Spirit Vale 11 | TEXCOORD0 · TexTransform0 | **TEXCOORD2 · TexTransform2** | TEXCOORD1 · TexTransform1 |
| 421456 | Spirit Vale 25 | TEXCOORD0 · TexTransform0 | TEXCOORD1 · TexTransform1; slot 9 `ramp` at a computed coordinate | TEXCOORD1 · TexTransform1 (+ a TEXCOORD2 · TexTransform2 output) |
| 1151160 | Spirit Vale 6 | TEXCOORD0 · TexTransform0 | TEXCOORD2 · TexTransform2 | TEXCOORD1 · TexTransform1 |

(The VS of 197591 takes only `POSITION, NORMAL, TEXCOORD0-2`, that of 1151160
only `POSITION, TEXCOORD0-2`; both have `TexTransform2A/B = cb0[25..26]`.) So:

- **TEXCOORD0 is the page UV** and **TEXCOORD1 the tiled ground UV** on
  every terrain shader seen; some shaders give the first ground texture a
  third UV set of its own.
- The **36 Spirit Vale chunks with a non-null `uvData`** are exactly the 11
  chunks on AMAT 197591 and the 25 on AMAT 421456. Both have a
  `TexTransform2`; so does 1151160, whose 6 chunks have no `uvData` (so a
  third UV set alone does not imply animation). That `uvData` drives
  `TexTransform2` on the chunks that have it is the obvious reading and is
  **UNPROVEN** (proof: the client code that fills `TexTransform2` for a
  terrain chunk, or a frame capture of one of those chunks).
- The shader never multiplies a UV by a constant: **how often a ground texture
  repeats per chunk is decided outside the shader**, by the TEXCOORD values
  the client writes into its terrain vertex buffer and by `TexTransform1`.

### 1.6 What the stored parameters mean

| parameter | meaning | status |
|---|---|---|
| `pickerOffset`, `pickerScale` | the chunk's sub-rect of its 512² page, in page UV (`gw2-world-frame.md` §4.2-4.3); in the game this is what TEXCOORD0 · TexTransform0 must produce | sub-rect PROVEN (seam test); which of TEXCOORD0 or TexTransform0 carries it, UNPROVEN (both reach the shader the same way) |
| page edge inset | T3D insets by 1/1024 (`TerrainShader.ts:54-55`) | UNPROVEN; the game's inset, if any, is in the client's TEXCOORD0/TexTransform0 |
| `uvScale` (`TerrainMaterial::uvScale = 0`) | ground repeats per chunk | **UNPROVEN**: not in the shader (§1.5); T3D's 8 is a hard-coded guess (`TerrainRenderer.ts:405-406`). Proof: the client's terrain vertex/TexTransform1 builder, or a capture of `TexTransform1` and a chunk's TEXCOORD1 range |
| `tiling` byte[3] | — | **UNPROVEN**. Not per ground texture (all four share TEXCOORD1, §1.5). Spirit Vale: `[8,8,8]` 130 chunks, `[0,8,8]` 108, `[0,8,4]` 12, with no clean link to the AMAT (184989 has both `[8,8,8]` and `[0,8,8]`). Three bytes for hi/lo/fader materials is a hypothesis the data does not support (`faderMaterial` is null on every Spirit Vale chunk while `tiling[0]` is 8 on 130). Proof: as `uvScale` |
| `uvData` {translation, xScaleRange, yScaleRange, scaleSpeed, rotation} | per-chunk UV animation | values read (Spirit Vale chunks 121-126: translation (0, 0.2), scale ranges (1, 1), scaleSpeed 0, rotation 0; chunks 131-132: (0.028169, 0.150235)); the same `(0, 0.2)` as the commonest prop `scroll` (§4). Units (per second?) and target (`TexTransform2`) UNPROVEN |
| `hiResMaterial` vs `loResMaterial` | same textures, different shader (§1.2) | which is drawn when is UNPROVEN; `trn.swapDistance` (Queensdale 39936, Spirit Vale 24576) is the natural candidate for the switch distance. Proof: the client code that reads `swapDistance` |
| `pickerSolid` | 1×1 page colour | channel order UNPROVEN (`gw2-world-frame.md` §4.2); the shader reads the page as RGBA weights (§1.2), so the order decides which ground texture a solid page selects. Proof: a solid page whose chunk is visibly one ground texture in game |
| `midFade`, `farFade`, `constArray` | — | (0, 0), (0, 0) and empty on Queensdale and Spirit Vale; nothing to read |

---

## 2. Can the existing game-shader path draw terrain chunks?

**Not as it stands.** What exists and what would be missing:

- `extract_game_material` (`src/extract/game_shader.cpp:124-265`) takes a
  MODL `castlemist::model::Material`. Terrain has none; an adapter could make
  one (`materialFile = materialFileId`, `textures[]` = the ten
  `texIndexArray` fileIds **by position**, §1.1, with the page fileIds in
  positions 4-5). The AMAT lookup itself is the same (`fileId -> baseId - 1`;
  `gw2dat_cli amat --file-id 187795` loads it). Solid pages have no fileId:
  the sampler loop (`game_shader.cpp:227-234`) would bind grey, not the
  page's colour.
- **Vertex layout.** Terrain has no MODL fvf. The client builds the vertex
  buffer; castlemist would have to build `POSITION, NORMAL, TANGENT,
  BITANGENT, TEXCOORD0, TEXCOORD1` (+`TEXCOORD2` for 197591/421456/1151160)
  as floats itself, with UV values it does not know (§1.6).
- **Uniforms.** VS: `CameraPosition, FogColorNearMinusFar, FogColorFar,
  FogColorHeight, FogDepthCue, FogParam0, ViewProjection, World, WorldView,
  WorldToShadowA-C, TexTransform0A/B, TexTransform1A/B` (hi-res adds
  `LightCount, LightPointAndSpotData`; 197591/421456 add `TexTransform2A/B`).
  PS lo-res: `LightBuffer, StencilId, ScreenDims, shSunColor, TexelOffset`;
  hi-res adds `shRed, shGreen, shBlue, shSunData`. The existing fill
  (`game_material.cpp:290-353`) sets every TexTransform to **identity**
  (`game_material.cpp:296-297`), which would draw the pages at the wrong scale
  (the whole page over each chunk) and the ground at one repeat per TEXCOORD1
  unit.
- **Textures.** Slots 0-9 from the dat; slot 14 needs a light buffer; slot 13
  (hi-res) an env cube. The main window has a light buffer
  (`src/render/lighting.cpp`) on its own device globals; `WorldView` has none.

So a 1:1 terrain draw needs three things nobody has proven: the TEXCOORD
values, the TexTransform values, and the hi/lo choice. The reconstruction of
§1.2 needs only the first two (as a repeat count) and is honest about it.

For **props**, the game path is proven: `render_scene_game`
(`src/render/scene.cpp:156-340`) draws map props through their own DXBC in the
main window. It is per instance per submesh (one cbuffer update and one draw
each) and its builder `build_one_game_mat` (`game_material.cpp:373-476`)
touches only two globals, `g_dev` and `g_game_vals`, so it could be given a
device parameter; but the deferred PS families also need the light buffer.

---

## 3. Prop textures at map scale

- **Instancing is per model and stays so.** The fly path flattens every
  submesh of a model into one VB/IB per LOD (`map_fly.cpp:186-222`: LOD k is
  each submesh's LOD k in mesh order) and buckets visible instances by
  `(model, lod)` (`map_fly.cpp:423-464`), one `DrawIndexedInstanced` per
  bucket. Because the flattener appends submeshes in order, the index range of
  each submesh inside an LOD range is known at flatten time. With materials,
  one bucket becomes **one instance upload plus one draw per submesh range**,
  each with its material's textures bound. Culling, LOD choice, the instance
  stream and streaming do not change.
- **Draw count.** The census averages 232 materials over 62 models (3.7 per
  model). A view that drew N model buckets now issues about 3.7 N draws.
- **Textures repeat across models.** Decoding per model would load the same
  ground-clutter atlases many times. A cache keyed by texture fileId, shared
  by every model, is the obvious design (measurement of the sharing ratio:
  not done).
- **Memory.** `ModelTextureCPU` is RGBA8 at the decoded size
  (`model_types.h:62-90`), and GW2 ships most textures as a full/reduced pair
  (`entry_extractor.h:183-216`). At map scale the reduced member, capped
  further and mip-mapped, is the cheap choice. `set_texture_full_res` is a
  **process-wide** preference (`entry_extractor.h:192`), so the map path must
  not flip it under the main window.
- **What the reconstruction needs per material**, all already in
  `ModelMaterialCPU` (`model_types.h:97-210`): `diffuseTex`, `diffuseUv`,
  `isEffect`, the blend word, and the export profile
  (`exportgltf/shader_profiles.h:132`, `profile_for`), which says whether the
  diffuse alpha is holes (< 64, `gw2-material-channels.md` §1-2), shine, or an
  intensity. Normal maps need tangents, which the 16-byte `MapVtx` does not
  carry.
- **Per-placement data the map stores and nothing reads yet**
  (`PackMapPropObjV21`): `color` byte4 (Spirit Vale `propArray`: `80 80 80 80`
  928, `00 00 00 00` 39, tinted e.g. `4A 81 64 80` 4); `constants`
  (`PackMapPropConstantV18 {token, constant, submodel}`, 22 placements);
  `blitTextures` (151 placements with a non-null entry). What they do is
  **UNPROVEN** (`color` as a ×2 tint with `0x80` neutral is the obvious
  reading; proof: a capture of a tinted placement, or the client's use of
  `PrProp` colour).

---

## 4. UV scroll: where it is stored

**PROVEN from the data:** a MODL material's `texTransforms[]`
(`ModelMaterialDataV66`; `ModelMaterialTexTransformV66 {flags, uvIndex,
columns, rows, count, fps, scroll: float2, scrollFreq: float2, scale: float2,
scaleFreq: float2, rotate, rotate}`) holds static per-UV-set animation.
Example, model 1126949 (a Spirit Vale `propAnimArray` prop): material 0 has two
entries, `uvIndex 1` and `uvIndex 0`, both `scroll (0, 0.2)`, `columns = rows =
count = 1`, everything else 0.

Census (62 models, 232 materials, §Evidence base):

- 33 `texTransforms` entries in 19 models, **all 19 of them animated props**
  (0 of the 29 static models parsed). Every entry has `flags 0`, `columns =
  rows = count = 1`, `fps 0`, `scrollFreq = scale = scaleFreq = rotate = 0`;
  only `scroll` and `uvIndex` vary: `(0, 0.2)` ×6, `(0.3, 0.3)` ×4,
  `(0.5, 0.5)` ×4, `(0, -0.1)` ×3, `uvIndex 2 (0.5, 0)` ×3, then singles
  from `(0, 0.01)` to `(-0.1, 0.5)`. No flipbook (`columns`/`rows` > 1) was
  seen.
- `uvTransLinks`: 0. Textures with `uvAnimId != 0`: 9. `uvAnimData`
  entries over all clips: 6 (`ModelUVAnimationV25 {uvAnimId, uvTransformData:
  ModelTrackTypeDataV25[]}`, keyed tracks inside a clip). `matConstAnim`
  entries: 495, in 19 models (`ModelMatConstAnimV25
  {materialId, constToken, trackGroupIndex, vectorTrackIndex, initialValue}`,
  animated material constants).

How these reach the shader is **UNPROVEN**. The VS has `TexTransform0A/B ...
TexTransform3A/B`, one affine pair per UV set (§1.5; the same uniforms in prop
VS, `gw2-shaders-dxbc.md`), and castlemist fills them with identity
(`game_material.cpp:296-297`, and `gw2bgfx_view.cpp:154-157`). The reading
"`TexTransform<uvIndex>` translation = `scroll × time`, wrapped" fits the
data (a scroll of 0.2 means one repeat per 5 s if the unit is per second)
but its unit, its wrap and the meaning of `scale`/`scaleFreq`/`rotate`
(all 0 here, so `scale` cannot be a plain multiplier) are not established.
**Proof:** the client code that writes `TexTransform*` per draw, or two frame
captures of a scrolling prop a known time apart. castlemist does not parse
`texTransforms`, `uvTransLinks` or `uvAnimId` today (`MatTexture` has only
`uvIndex`, `gw2model.hpp:144`; `Material`, `gw2model.hpp:150-168`).

---

## 5. Animated props: how a placement names its clip

**PROVEN on Spirit Vale:** a `propAnimArray` placement
(`PackMapPropObjAnimSeqV21`, which adds `animSequence: qword` to the prop
fields) names its clip by **token64**, and the token is the clip name packed
5 bits per letter (`detokenizeName64`, `gw2model.hpp:130`):

| `animSequence` | decodes to | placements (of 116) |
|---|---|---|
| 192720390330 | `zeropose` | 36 |
| 6351 | `off` | 19 |
| 463 | `on` | 16 |
| 464399 | `open` | 14 |
| 175158308 | `damage` | 10 |
| 6086006991 | `offblue` | 6 |
| 9233160238, 5881219, ... | `navmesh`, `close`, `ongreen`, `onblue`, `onred`, ... | rest (15 distinct) |

The same token is a clip's `ModelAnimationDataV33.token` in the model's own
`ANIM` bank: model 1126949 holds `zeropose, on, off`; model 1127821 holds
`zeropose, offred, chargered, onred, chargeblue, onblue, offblue, chargegreen,
ongreen, offgreen, demored, demoblue, demogreen`. Over the 33 models, **37 of
the 41 distinct (model, animSequence) pairs name a clip in that model's bank**.
The 4 that do not: `zeropose` on 1127039 (bank: `damage, demolish, off4, off8,
on4, on8`) and on 1151335 (`template, zeropose4, zeropose8`); `on` on 740373
(`off4, off8, on4, on8, zeropose`); `noarch` on 1150399. So the client has a
fallback (the digit-suffixed names look like variants; `ModelAnimationDataV33`
has `variantCount`/`variantIndices`, the bank has `fallbacks`), and **the
fallback rule is UNPROVEN** (proof: `ModelAnimBank_FindAnimation`
@`0x140D327A0`, `gw2-animation-banks.md`, read for the miss path). The two banks
read in full (1126949, 1127821) have no `imports`, so their clips are local; in general the lookup walks
the import chain (`gw2-animation-banks.md`).

The same names appear in collision: `zeropose` is the default `havk`
sequence and `zoneprop`/`navmesh` are sequence names too
(`gw2-world-frame.md` §7.1 lists the numbers `192720390330` and
`566482876922` = `zoneprop`). The note there says the tokens "do not decode
with the base-23 Token rule"; they decode with the 5-bit name rule.

castlemist today:

- `MapProp` (`gw2model.hpp:1635-1644`) and `PropInstance`
  (`world_scene.h:64-69`) do not keep `animSequence`; `Motion`
  (`world_scene.h:112`) is the list of `propAnimArray` indices only
  (`src/world/props.cpp:53`).
- `ModelPreview` decodes every clip with Granny (`model_preview.cpp:418-427`)
  into `animClips` and keeps `animationTokens` for **every** clip, but pushes
  to `animClips` only the ones that decode, so the two vectors are not
  parallel and a token cannot be mapped to a decoded clip by index.
- Posing exists: `granny::composePose` and `granny::skinMatrix`, used by
  `gw2bgfx_view.cpp:1254-1350` (including rigid single-bone attachment,
  `d.rigidBone`).
- What phase each placement plays at, and whether an `on`/`off` placement
  loops, is **UNPROVEN** (the clip's own flags were not read; proof: the
  client's prop-animation start, or a capture of two identical props).

---

## 6. Water: what the dat describes

`gw2-world-frame.md` §6 covers geometry: a plane height only (`watr.waterPlaneZ`,
`havk.waterSurfaceZ`, 0 on Queensdale and Lion's Arch, absent on Spirit
Vale), no `watr` surfaces, no shore chunk, river centrelines on Spirit Vale,
and the coverage UNPROVEN. New here is the **material**:

- Every `env` data block has `water: PackMapEnvDataWaterV76[]` with
  `waterFlags, animAmplitude, animChoppiness, animWind: float2, bumpAmount,
  bumpAngle0/1, bumpScale0/1, bumpSpeed0/1, bumpTile0/1, patternAngle,
  patternTile, patternSpeed, patternEdge, surfaceShallowColor,
  surfaceDeepColor, patternColor: byte4, surfaceFresnel, distortAmount,
  depthAttenuation, materialFilename, textureFilenames[], constantTokens[],
  constantValues[], foamSpawn, foamDissolve, foamDepthAttenuation,
  foamColor0/1: byte4`.
- **Queensdale** has 39 presets (30 non-empty arrays). They name two AMATs:
  **187551** on 26 presets (textures `(null), 186359, 187574/192654/188570,
  187574/187580, 187576/190963/190961`, 14 constants) and **188555** on 13
  (textures `(null), 186359, 186361`, 7-8 constants). The first preset reads
  `bumpTile 5.01 / 13.66, bumpSpeed 1.31 / 0.64, patternTile 4.58,
  surfaceShallowColor A5 DF 5C 00, surfaceDeepColor 36 4F 21 00,
  surfaceFresnel 3.89, depthAttenuation 15.66, distortAmount 0.01`.
  Constants decode (base-23, `game_shader.cpp:116-122`) to `spclr, shrclr,
  wtrshr, shrcl, shrclb, shrch, shrchb` (shore colour and churn) and
  `wtrprmv, wtrprmd, wtrprme, wtrprmp, wtrclrs, wtrclrd, wtrclrp`, the last
  seven stored as **all zero**.
- **AMAT 187551** (10 PS). Selected PS 3 samples `ssPattern0` (slot 0,
  textureIndex 2), `ssChurnLow0` (1, idx 3), `ssChurnHigh0` (2, idx 4),
  `ssReflect0` (3, idx 0 = the `(null)` entry: a runtime reflection target),
  `ssBump0` (4, idx 1 = 186359), `ssDepth0` (12, role 35 scene depth),
  `ssEnvMap0` (13, role 33), `ssScreen0` (14, **role 36**, a scene-colour copy
  not seen in the model census), `ssShadowMap0` (15, role 37). Its uniforms
  include `BlurryWater, SpecularCp, ShoreColor, ShoreChurnLowA/B,
  ShoreChurnHighA/B, wtrprmp, wtrclrs, wtrclrd, wtrclrp`. Its VS takes `Time`
  and computes the bump UVs from the vertex position:
  `position.xy * wtrprmv.xxyy + wtrprmd` and `position.xy * wtrprme.x +
  wtrprmv.zw`, plus a `Time`-driven shore churn. Since the preset stores
  `wtrprm*` as zero, the client fills them per frame, presumably from
  `bumpTile`/`bumpSpeed`/`bumpAngle`; that mapping is **UNPROVEN** (proof: the
  client code that sets `wtrprmv/d/e`, or a capture).
- **AMAT 188555**: castlemist's selector finds no colour effect (`psIndex
  -1`, 6 PS). What it draws (underwater? a depth pass?) is **UNPROVEN**.
- Which preset applies where: one per lighting preset (three per block,
  `gw2-world-frame.md` §6.1) and per zone block; the pairing is UNPROVEN.

> **Correction (2026-10-15, sub-project 3).** The first-preset values quoted
> above (`surfaceShallowColor A5 DF 5C`, `surfaceDeepColor 36 4F 21`,
> `surfaceFresnel 3.89`, `depthAttenuation 15.66`, `patternTile 4.58`) are **not
> in Queensdale's `dataGlobal` block**. `dataGlobal` has 3 water presets there,
> and preset 0, the one the Map window draws (it pairs with lighting preset 0),
> measures: `bumpTile 5.013 / 13.661`, `bumpSpeed 1.31 / 0.643`, `patternTile
> 14.283`, `surfaceShallowColor BD CF 5F`, `surfaceDeepColor 3B 64 85`,
> `surfaceFresnel 3.325`, `depthAttenuation 28.47`, `distortAmount 0.01`.
> Presets 0 and 1 share `bumpTile`, `bumpSpeed` and `distortAmount` with the
> values above; their colours, fresnel, depth attenuation and pattern differ.
> Every one of the three names AMAT 187551 or 188555 (preset 2 names 188555).
> The earlier values are probably from a per-zone environment block that
> `load_world` does not read; that is a guess, not a finding, and the 39
> presets and 26/13 split above were not re-counted. A test pins the measured
> `dataGlobal` values (`materials_queensdale_water_presets`).

So the dat describes **a plane height, a per-zone surface material (colours,
bump tiling and speed, fresnel, depth attenuation, foam) and its textures, and
nothing about the outline**. The game's water shader needs scene depth, a scene
colour copy and a reflection target, none of which `WorldView` has; a 1:1 water
draw is far away. A reconstruction from the preset fields (deep/shallow colour
by depth below the plane, scrolling bump normals, fresnel, sun specular) uses
only stored values plus UNPROVEN units.

---

## 7. UNPROVEN, collected

| item | what would prove it |
|---|---|
| terrain binds `texIndexArray` by position (§1.1) | client code building the chunk material |
| ground repeats per chunk (`uvScale`), TEXCOORD1 / TexTransform1 values (§1.5-1.6) | client terrain vertex/TexTransform builder, or a capture |
| `tiling` bytes (§1.6) | same |
| `uvData` -> `TexTransform2`, its units (§1.5-1.6) | client code, or a capture of a Spirit Vale soul-river chunk |
| hi- vs lo-res switch; `swapDistance` as the distance (§1.6) | client code reading `swapDistance` |
| `pickerSolid` channel order (§1.6) | a solid-page chunk compared with the game |
| page edge inset (§1.6) | TEXCOORD0/TexTransform0 capture |
| `texTransforms` -> `TexTransform*`: unit, wrap, `scale`/`rotate` (§4) | client code, or two captures |
| `uvAnimData` / `matConstAnim` track decoding (§4) | `ModelTrackTypeDataV25` decoded |
| clip fallback when `animSequence` names no clip (§5, 4 of 41) | `ModelAnimBank_FindAnimation` miss path |
| per-placement animation phase and looping (§5) | client prop-animation start |
| per-placement `color`, `constants`, `blitTextures` (§3) | capture of a tinted/overridden placement |
| water area, `waterPlaneZ` as the drawn height (`gw2-world-frame.md` §6.2) | as there |
| `wtrprm*` from the preset fields; AMAT 188555's role; preset per zone/time (§6) | client water setup |
