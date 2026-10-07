---
name: gw2-material-channels
description: What each texture channel holds, per AMAT shader family, read off the game's own pixel shaders; the shader-profile table for the VRChat model export, and where the spec's default rules are wrong
metadata:
  node_type: memory
  type: project
  modified: 2026-10-07T00:00:00.000Z
---

Research step for `docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md`
("Shader profiles", "Map building"). Survey of 24 models (weapons from
core through Janthir, 3 back items, map props, foliage, glass, fire, additive
and blended effects) plus a 545-material random sample of MODL files.

**Method.** Texture statistics alone could not settle what a channel means: a
gloss map and a coverage mask look the same in a histogram (see 57634 below).
So every claim here comes from the **game's own pixel shader**: the material's
AMAT, dumped with `gw2dat_cli amat --file-id <AMAT> --out-dir` (selected
colour-pass PS) or `gw2dat_cli extract` (every FSH blob in the AMAT), with the
DXBC disassembled by `d3dcompiler_47!D3DDisassemble`. The bgfx uniform table was
parsed so `cb0[n]` reads as `speccp`, `envcr`, and so on. Texture slot `tN` =
the MODL material's `textures[N]` (checked on the Forged Dagger: t3
`glowperturb` offsets the UV for t2 `glow`, and t4 `glowmask` is read on the
second UV pair, the uv2 the MODL gives it). Stats were taken with the
`texture` CLI + PIL, and again over **only the texels the material's own
triangles sample** (UV0 rasterised from a `--glb` export), because most atlases
have empty padding that reads as "alpha 0". Blend words come from the
`GW2_MATDBG=1` stderr line of `gw2dat_cli model` (`rs=`). The scratch scripts
are not committed.

## 1. The diffuse alpha encoding is the same everywhere, but not what the spec says

Every lit GW2 shader that reads the diffuse alpha uses this curve (AMAT 561567
colour PS, identical in 510615, 511755, 2348484, 14149, 13843, 27352, 19911, ...):

```
sample r0, uv0, t0
add_sat r1.x, r0.w, r0.w ; add r1.x, r1.x, -0.5 ; lt ; discard_nz   // clip when a < 0.25
add r0.w, r0.w, -0.5 ; add_sat r0.w, r0.w, r0.w                     // shine = saturate(2a - 1)
```

| alpha (0-255) | meaning |
|---|---|
| 0 - 63 | **hole**, only in shaders that discard (sec. 2) |
| 64 - 127 | opaque, **no shine** |
| 128 - 255 | shine 0 -> 1 (`(a-128)*2`) |

The shine value then scales the sun specular (`speccp.rgb`, exponent `speccp.w`)
and the cube-map reflection (`envcr`/`envcp`/`envcol` RGB, strength `.w`), and
in most shaders also picks the reflection mip (`4 * (1 - fade * shine)`): high
shine = sharp reflection. So shine is **specular intensity and gloss together**.

This contradicts the spec's default rule ("below 16 = hole, above = shine"):

- the cutoff is **0.25 (64)**, not 16. `gw2-frame-pass-order.md` already
  measured 0.25 for the opaque path; this confirms it is in the shader, for
  every alpha-tested family surveyed;
- texels at 64-127 are **matte**, not shine. A lot of real content sits there:
  Nevermore mat 9 (AMAT 14213) has 84% of its sampled texels in 64-127, Wings of
  Ascension 30%, Eureka 46%, Bioluminescent sword 62%. Rescaling from 16 makes
  all of them glossy;
- `glTF alphaCutoff` must be **0.25**, not 0.5 (glTF default).

Map-building consequence: `Packed G/A (shine)` = `clamp((a - 128) * 2, 0, 255)`;
holes = `a < 64`, and only for a profile whose shader actually clips.

## 2. Which materials actually clip: the shader decides, not `prepassCutout`, not the histogram

Classifying every `discard` in every PS of an AMAT gives three kinds: texture
alpha (`saturate(2a) < 0.5`), stipple fade (`round_pi ... StippleDensity`), and
fade-vs-AlphaRef (`fxclr.w < AlphaRef`). The last two are LOD/fade dithering and
cut nothing out of the texture.

- `GameMaterial.prepassCutout` is **true for stipple-only AMATs** (13822 Twilight
  and Sunrise, 54632 Frostfang, 15999 Wings of Dwayna, 57634, 2472137 jade...).
  None of these ever discards on the texture. **prepassCutout cannot be used as
  alphaTest**, as the spec's blend decode proposes.
- `ModelTextureCPU::hasCutout` (histogram) fails the other way. AMAT 57634
  (forest backdrop, model 2893914 mat 0): 59% of sampled texels have alpha < 16,
  yet no PS in the AMAT reads alpha for coverage. The texture is a painted
  tree-wall whose alpha is a dark shine map. Exported as Cutout it becomes
  confetti.
- Conversely Twilight (13822): 5.4% of sampled texels < 16; no clip, those
  texels are just non-reflective.

Texture-alpha discard present (alpha-tested): 561567, 511755, 510615, 2348484,
14149, 14213, 14165, 1891783, 2083141, 2140066 (alpha x eataway), 1465623,
2449347, 2234037, 13843, 13856, 13864, 14003, 27352, 31327, 32657, 34181,
19911, 47468, 47469, 72583, 44707, 1171332.
No texture discard (alpha = shine only, never holes): 13822, 13831, 54632,
15999, 27353, 54592, 57634, 57715, 2472137.

**Recommendation:** the alpha-test answer belongs in the profile table, keyed
by AMAT. In code it can be computed exactly as above: "a PS of the AMAT
discards on a value derived from a t0 sample." That is a small change in
`extractAmat`'s discard scan, which today ORs stipple discards in. It is
listed here, not done (read-only task).

## 3. Blend words seen, and two the spec does not map

From 545 random materials (`rs` field, bgfx layout: blend at bit 12, equation
at bit 28, ALPHA_REF at bit 40):

| rs (blend bits) | src/dst | count | where |
|---|---|---|---|
| 0 | opaque | 460 | everything lit |
| 0x6565000 + ALPHA_REF 1 | SrcA / InvSrcA | 208 | glass-ish, legacy untagged props, effects |
| **0x4242000** | **One / InvSrcColor** | 118 | the commonest weapon/prop effect (19092, 23497, 57224 glass, 21471, 55650 ...) |
| 0x6262000 | One / InvSrcA (premultiplied) | 64 | 20760, 23408, 965703/740364 fire |
| 0x2525000 + ALPHA_REF 1 | SrcA / One (additive) | 23 | 1761213, 2323863 |
| 0x3939000 | DstColor / SrcColor (2x mult) | 5 | 48765, 1009931 |
| 0x1919000 | DstColor / Zero (multiply) | 3 | 15206 |
| **0x2222000 + equation 0x12** | **One / One, REVSUB** | 3 | **904523** (model 1765737 mat 5) |
| 0x2222000 | One / One | 2 | 957604 |

- **One/InvSrcColor is not in the spec's table** and is the most common
  blended mode. It is a screen-style soft additive (`src + dst*(1-src)`), so
  it should map to **Soft Additive** directly, not fall to Custom/"nearest by
  dst".
- **904523 is One/One with BLEND_EQUATION REVSUB** (`0x12` at bit 28): the
  result is `dst - src`, a darkening effect, not additive. `decode_blend` must
  read the equation field. A REVSUB material should be flagged Custom, never
  Additive.
- Every blended effect PS surveyed **premultiplies by its own alpha**:
  19092 `o.rgb = 2*rgb*vc.rgb * a * depthfade`; 23497/57224 `o.rgb = rgb * a *
  ramp(|N.V|) * diffade, o.a = same`; 904523 `o.rgb = 2*rgb*vc.rgb*a*vc.a`;
  20760 `o = 2*rgba*vc*depthfade`. So for Additive/Soft Additive/Transparent the
  diffuse **alpha is an intensity mask**. Export BaseColor RGB x A, or keep A and
  use a Poiyomi mode that multiplies by it; using RGB alone over-brightens the
  transparent parts (the RGB of 23497-family textures is not black where A=0).
- Most blended weapon materials have **0 triangles**: they belong to the MODL's
  particle clouds (`effects.clouds[].materialIndex`), not to a mesh. The
  exporter should skip them for meshes and hand them to the particle sidecar.

## 4. Proposed shader profiles

`AlphaUse` needs one more value than the spec has: `HolesAndShine` with the
0.25 cutoff, plus `Shine` for the non-clipping lit shaders, `Intensity` for
premultiplied effects, `Interior` for jade.

| profile | AMATs (fileId) | diffuse alpha | layers and channels | constants |
|---|---|---|---|---|
| **weapon-glow** (default lit) | 561567, 511755, 510615; lit core also in 2083141, 2140066 (Pharus, Exordium) | holes < 64, shine `saturate(2a-1)` | normal RG (B ignored, Z rebuilt); glow RGB on UV0 at UV offset `glowperturb.RG*2-1 * gloptrb` (UV1); glowmask RGB (UV2) multiplies glow; emission = glow x glowmask x glofade x 2. 510615 has no glow. 2083141/2140066 replace the glow with an animated legendary effect (glowfringe, ramp, noise, flow, eataway dissolve; glowmask x glowcol) | speccp rgb=spec tint, w=exponent; envcr rgb=reflection tint, w=strength; `conduct` = metalness-like: spec/reflection tinted by `0.5 + conduct*(0.6*albedo - 0.3)` |
| **weapon-spec** | 2348484 (SotO/Janthir: Sacred Crystal Rifle, Skyforged Hammer, Onyx Spider's Torch) | holes < 64, shine `saturate(2a-1)` | **specular**: RGB = spec and reflection colour (`saturate(2*spec)`), **A = gloss** (reflection mip `4*(1-fade*A)`); glow on UV2 (perturbed by glowperturb UV1); glowmask on UV0 | gloptrb, glofade only (no speccp/envcr) |
| **legacy-spec** (core era) | 13822, 13831 (no clip); 14149, 14213, 14165, 1891783 (clip). Same layer set, not disassembled: 1749692, 2069382 | shine `saturate(2a-1)` = **reflection only** (x envcp.w); holes only in the clipping ones | **specular: RGB = sun-spec colour (x2), A = exponent/128** (`pow(N.H, A*128)`); backlight (4x4 white); on 14149/14165/14213 the **mask is R = glow gate, G = glow-perturb gate** (`saturate(mask + gloover)`), not metal/gloss; perturb (UV1) offsets glow | envcp rgb/w = reflection; blint = backlight intensity |
| **prop-lit** | 13843, 13856, 13864, 14003, 31327, 32657, 34181, 44707/44708, 72583, 27352 (unlit-ish, diffuse only), 19911/47468/47469 (foliage cards) | holes < 64, shine `saturate(2a-1)`; 13843-family also derives the exponent from shine (`(0.25+0.75*min(2s,1))*128`) and reflection from `max(2s-1,0)` | normal RG; `mod` (UV1) multiplies albedo x2 (grey 128 = neutral); `height` = parallax height | speccp |
| **prop-lit-noclip** | 15999 (Wings of Dwayna), 54592, 57634, 57715, 27353 | **shine only, never holes** (even when the histogram is full of zeros) | as prop-lit; 27353 adds `mod` x2 | speccp |
| **subsurface-decal** | 54632 (Frostfang, Wings of Ascension) | shine only (x envptrb, x speccp), no clip | **decal (UV1)**: final albedo = `lerp(decal.rgb, diffuse.rgb, decal.a)`, so decal **alpha 1 = show diffuse**; decal UV parallax-offset by `pardist`; subsurface RGB = scatter colour x albedo | pardist, speccp, envcol, envptrb |
| **armor-mask** | 2449347, 2234037, 2777930 (and character pieces with `glowcm`/`rimcp`) | holes < 64, shine `saturate(2a-1)` | **mask R = metal** (spec tint `0.5+R*(0.2*albedo-0.2)`), **G = gloss** (exponent `G*speccp.w`, reflection mip), **B = sheen/silk** (`saturate(1-B)` lerps the dyed colour into the sheen term, `saturate(1-2B)` cuts specular), **A = glow** (x glowcm; 2777930: x8); dyemask RGBA = 4 dye-channel weights (hsmnt* matrices) | glowcm, rimcp, speccp, envcr |
| **armor-mask-noglowA** | 1171332, 1699091, 1674959 | holes < 64 | mask R metal, G gloss, B sheen as armor-mask, **A not read**; glow from a separate glow layer | |
| **armor-silk** | 2507831, 3252849 | holes < 64 | mask R, G, B feed a different silk BRDF (`exp`/`pow` chains, `1-G` and `1-B` used); **not** the armor-mask layout. Needs its own pass before mapping. | silkcp, conmin, conmax, hueshif, saturtn, value |
| **jade-interior** | 2472137 (EoD Jade Tech) | **interior blend weight**: `lerp(diffuse, interior parallax, a)`; no holes, no shine | `spec` 4x4 placeholder, `envmask` RGB multiplies reflection, parallaxheight/colour/flow = animated interior | mtlness, specstr, specpwr, envstr, glowstr, int* (newer PBR-ish names) |
| **legacy-untagged** | disassembled: 185120, 187842. Same trait, not disassembled: 188779, 835943, 835971, 1255675, 2333606, ... (material tokens all 0, matId 0, flags 0, SrcA/InvSrcA + ALPHA_REF, forward-lit) | 185120: **unused**; 187842: opacity factor `saturate(2a)` and shine `saturate(2a-1)` | **tex0 = diffuse, tex1 = normal (185120: normal.A = spec, exponent A*64, strength A*0.85), tex2.R = opacity** (x fadedif), clip vs AlphaRef | fadedif |
| **fx-soft-additive** | disassembled: 19092, 23497, 57224 (glass). Same blend word, not disassembled: 23496, 21471, 46460, 25489, 43029, 45993, 49624, 55650, 55903, 882285, 217286 (One/InvSrcColor) | **intensity** (premultiplied) | 23497/57224: `ramp` (1D, indexed by abs N.V) = fresnel opacity; 57224: `mask`.R = opacity, `normal` layer is a **UV-perturb map** (x difptrb), diffuse read on the perturbed second UV; `mod` (UV1) multiplies | diffade (strength), difptrb |
| **fx-premultiplied** | disassembled: 20760. Same blend word: 23408, 54721, 14196, 19116, 53260, 217998 (One/InvSrcA) | intensity / coverage (premultiplied) | vertex colour x2; soft-particle depth fade | depfade, diffade, step |
| **fx-fire** | 740364, 965703 (by layer set and constants) | n/a (procedural) | fireshape/firedetail/flow/flowb/ramp + colorl/colorh: an animated flame, not representable as maps; export emission colour from colorh/colorl, flag it | fireduv, flowuv, rampflw, premult |
| **fx-distort** | by layer set (`distort` + `mask`), not disassembled: 709206, 47396, 27304, 339341 (opaque word), 630598 | n/a | distort RG = screen-space refraction offset; `mask`.R = distortion strength; no colour of its own | distort |

Shared facts, all families:

- **Normal maps:** only `.xy` are sampled; Z is rebuilt (`sqrt(1-x^2-y^2)`). The B
  channel and alpha are ignored (except legacy-untagged, where normal.A is spec).
  Whether G needs flipping for Unity cannot be read off the PS (it depends on the
  VS tangent frame); keep `bake_model_atlas`'s existing flip.
- **glowperturb / perturb / distort / flow** textures are 2-channel offset maps
  (RG, `*2-1`), with blue = 1 just because they are authored as normal maps. They
  are UV offsets, never lighting normals.
- **glowmask is RGB, not greyscale**: Sacred Crystal Rifle and Skyforged Hammer
  point glowmask and glow at the **same file**, so emission = glow^2.
- **4x4 placeholders** (13368 white, 958179 white, 529633 flat normal, 27338
  black, 44203 flat normal, 1459277 red) stand for "constant". Skip them as maps.
- UV sets: diffuse is usually UV0 but Eureka mat 3 (1377202) reads diffuse on
  **UV1**; modern weapons put glow on **UV2** (2348484, 1891783), older ones on
  UV0; glowperturb is UV1; Astralaria mat 3 uses UV3. Anything past UV1 needs the
  warning the spec already plans.

## 5. Exceptions to "has a `mask` layer => R metal, G gloss, A glow"

The trait "has a mask layer" is **not reliable**. Same role name, different
meaning:

| AMAT | mask channels |
|---|---|
| 2449347, 2234037, 2777930 (armor-mask) | R metal, G gloss, B sheen, A glow (the spec rule; holds) |
| 1171332, 1699091, 1674959 | R metal, G gloss, B sheen; A unused |
| 2507831, 3252849 (silk) | different BRDF; do not apply |
| 14149, 14165, 14213 (legacy weapons) | R = glow gate, G = glow-perturb gate (Eureka 1493247 greyscale, model 1765737 1750696: R=255, G 0-109) |
| 1465623 (Astral Ribbons) | R = scrolling-glow gate (`voffset`) |
| 2329259 | G = layer/dye lerp |
| 57224, 21471, 47396, 709206, 339340, 1761213 (effects) | R = opacity / distortion / pulse mask |

Use the AMAT key. A trait fallback that holds in this sample: `mask` **plus**
any of `glowcm`, `rimcp`, `hsmnt*` => armor-mask. `mask` with
`gloover`/`gloptrb` => legacy weapon glow gate.

## 6. Per-model survey

Sampled = stats over the texels the material's own UV0 triangles cover.

| model (fileId) | skin / what | mat -> AMAT | blend | diffuse alpha (sampled) | notes |
|---|---|---|---|---|---|
| Forged Dagger 1766522 | weapon | 0 -> 561567 | opaque, clips | 70-234, no holes | glow UV0, glowperturb UV1, glowmask UV2; conduct 1.5 |
| Twilight 543947 | core GS 4680 | 5 -> 13822 | opaque, no clip | 0-244, 5% < 16 (= no reflection, not holes) | specular RGB colour, A 72-169 (exp 36-85) |
| Sunrise 543946 | core GS 4679 | 8 -> 13822 | opaque, no clip | 48% in 16-64 | same as Twilight |
| Incinerator 218013 | core dagger 4682 | 8 -> 14149 | opaque, clips | 64-204, 0.4% holes | spec A 36-102; mask greyscale (glow gate); glow + perturb UV1 |
| Frostfang 217979 | core axe 4674 | 1 -> 54632 | opaque, no clip | 93-189 | decal UV1, subsurface (alpha 0/255 binary: not read as coverage) |
| Wings of Dwayna 61974 | core back 4766 | 1 -> 15999 | opaque, no clip | 120-230 | diffuse+normal only |
| Storm Wizard's Sword 631433 | gem store 5342 | 3 -> 511755 | opaque, clips | 170-255 | glowperturb file = the 21471 effect diffuse |
| Astralaria 1200313 | HoT axe 6506 | 5 -> 510615 | opaque, clips | 61-255, no holes | 3 -> 842652 parallax/mask, SrcA/InvSrcA, UV3 |
| Nevermore 1206542 | HoT staff 6466 | 0 -> 511755, 1/9 -> 14213 | opaque, clip | 14213 mat 9: 7% holes, 84% matte band | spec A = 255 const |
| Chuka and Champawat 1423754 | HoT bow 6717 | 4 -> 510615, 5 -> 511755 | opaque, clips | 44-255 / 100-255 | 3 -> 965703 fire (0 tris, particle) |
| Wings of Ascension 1313053 | HoT back 6556 | 1 -> 510615, 0 -> 54632 | opaque | 119-255, 30% matte | same texture drives both |
| Eureka 1493249 | PoF 6966 | 5 -> 14165, 3 -> 1377202 | opaque, clip / none | 107-255 | 14165 mask R/G; 1377202 diffuse on UV1, subsurface + perturbmask |
| Pharus 2083162 | PoF 8576 | 1/7 -> 2083141, 2 -> 561567 | opaque, clips | 124-255 | glowfringe 1-texel colour, flow/noise/ramp UV1 animated glow |
| Bioluminescent Sword 2163220 | LW 8827 | 4 -> 511755 | opaque, clips | 97-215 (62% matte) | |
| Jade Tech Scepter 2596230 | EoD 10236 | 0 -> 2472137, 1 -> 511755 | opaque, no clip / clip | 142-250 / 104-255 | the same diffuse means "interior weight" in 2472137 and "shine" in 511755 |
| Exordium 2140487 | legendary 8748 | 12-18 -> 2140066, 2083141 | opaque, clips (x eataway) | 117-230 | 5 near-identical models (2140487-90) |
| Sacred Crystal Rifle 3321621 | 12277 | 1 -> 2348484, 0 -> 57131 | opaque, clip / none | 126-255 | glow = glowmask file; decal UV1 |
| Skyforged Hammer 3123167 | 11754 | 3 -> 2348484, 1 -> 511663 | opaque, clips | 0-255, 2% holes | 511663 `cutout` layer UV1 |
| Onyx Spider's Torch 3375545 | 12450 | 0 -> 2348484 | opaque, clips | 118-247 | spec A 0-255 = gloss |
| Astral Ribbons 3121936 | back 11804 | 3 -> 2449347, 1 -> 2329259, 0 -> 1465623 | opaque, clips | 86-255 | armor-mask layout on a back item; dyemask RGBA |
| foliage 2879992 | map | 0 -> 19911, 2 -> 47469, 1 -> 57715 | opaque | 61% / 49% holes (true cut-outs); trunk 135-255 no clip | 19911's PS reads the diffuse from the interpolator's second pair (`v0.zw`; what the VS puts there was not checked); `grvegp` is a vegetation constant (likely wind) |
| forest backdrop 2893914 | map | 0 -> 57634 | opaque, **no clip** | 59% < 16 yet no holes | the histogram trap |
| foliage 2601739 | map | 0 -> 47468 | opaque, clips | 44% holes, rest 128-255 | leaf cards |
| prop 2898468 | map | 0/2 -> 44707/44708, 3 -> 27353, 12 -> 72583 | opaque | 56-255, no holes | mod UV1 grey 65 (darkens x0.5), height map |
| untagged 1934416 / 3556270 | map | 185120 / 187842 | SrcA/InvSrcA + AlphaRef | 185120 alpha unused | opacity in tex2.R |
| glass 2872543 | map | 2 -> 57224 | One/InvSrcColor | 255 (alpha unused) | opacity = mask.R x ramp(N.V) x diffade |
| additive 1765737 | map | 5 -> 904523 | **One/One REVSUB** | intensity | subtractive |
| fire 2873657 | map | 3 -> 740364 | One/InvSrcA | n/a | procedural flame |

## 7. Changes to the spec this implies

1. Default alpha rule: cutoff **64 (0.25)**, matte band 64-127, shine
   `(a-128)*2`. glTF `alphaCutoff` 0.25.
2. `alphaTest` must not come from `prepassCutout` (stipple discards set it) or
   from `hasCutout` (shine maps trip it). It comes from the profile, or from a
   texture-only discard scan of the AMAT.
3. Add **One/InvSrcColor -> Soft Additive** to the blend table; read the
   **equation bits** and treat REVSUB as Custom/subtractive.
4. Blended presets: BaseColor alpha is an **intensity** that the game multiplies
   into RGB. Premultiply, or tell Poiyomi.
5. "Has a mask layer" must not select armor-mask on its own (sec. 5).
6. Specular-map families: smoothness comes from **specular.A** (exponent/128 on
   legacy, gloss on 2348484), specular tint from specular.RGB. The diffuse alpha
   there is reflection strength.
7. Skip 0-triangle materials for meshes; they are particle materials.

Open: normal-map G orientation (needs the VS); 2507831 silk channel meanings;
whether the stray 0x12 equation also appears with blend fields other than
One/One (only 3 hits in 545).

See [[gw2-frame-pass-order]] (0.25 cutout, stencil-id alpha output),
[[gw2-amat-shader-roles]], [[gw2-amat-effect-selection]],
[[gw2-amat-draw-state]] (REVSUB term), [[gw2-shaders-dxbc]].

## 8. Second survey: unmapped shaders (2026-10)

Goal: find which AMATs fall to the `default` profile most often, read their
colour-pass PS, and propose rows for `shader_profiles.cpp`. Same method as the
top of this note (sec. 1-2): `gw2dat_cli amat` (selected colour PS +
`transparent` PS when the AMAT has one) and `extract` (every FSH blob, for the
discard scan), DXBC through `d3dcompiler_47!D3DDisassemble`, `cb0[n]` named from
the bgfx uniform table (`reg` is a byte offset, `cb0[reg/16]`), blend words from
`GW2_MATDBG=1`, alpha/channel statistics over the texels the material's own
triangles cover (triangles rasterised in UV space from a `--glb` export, raw
texture from `gw2dat_cli texture`). Scratch scripts and dumps in
`%TEMP%\cm_matsurvey2` (not committed).

A "texture discard" below is a `discard` whose condition depends on a texture
sample; `round_pi`/`StippleDensity` stipple fades and `fxclr.w < AlphaRef` fades
are excluded, `x < AlphaRef` on a texture-derived opacity counts as "AlphaRef fade"
(it only drops ~zero-opacity pixels; not a cut-out). Each AMAT's verdict looks at
**every** FSH in the AMAT, not only the colour pass.

### 8.1 Census

**Sample:** every 20th MODL by baseId (10 001 of the 200 002 MODL entries in
`dumps/index/gw2_index.db`), each run through `gw2dat_cli model` (one file id per
base id; `--base-id`/`--data` fail with `map::at` in that command). 0 errors.
18 044 materials, 21.9 M triangles, 1 222 distinct AMATs. `matcensus` was not
usable: it prints only aggregate counts, no per-AMAT or triangle data.
22% of all materials have **0 triangles** (particle-cloud materials, sec. 3).

Coverage by the **current** table (AMAT id in the table, or the legacy-untagged
trait of `profile_for`: matId 0, flags 0, SrcA/InvSrcA, no extra roles):

| | materials | triangles |
|---|---|---|
| in the table by id or trait | 9 854 (54.6%) | 11.17 M (51.0%) |
| ... of which legacy-untagged by trait only | 1 566 (8.7%) | 1.20 M (5.5%) |
| default profile | 8 190 (45.4%) | 10.75 M (49.0%) |
| ... of which the **diffuse slot has fileId 0** (runtime-composited character/creature skin, dye-palette effects) | 1 293 (7.2%) | 2.28 M (10.4%) |

Top 30 AMATs **not** in the table, ranked by models using them, then triangles
(counts are for the 1/20 sample; multiply by ~20 for the archive). "Verdict" is
the proposed row (8.4); "(sig)" = assigned by signature only, not hand-read.

| # | AMAT | models | mats | tris | blend (rs) | extra roles | verdict |
|---|---|---|---|---|---|---|---|
| 1 | 19255 | 151 | 174 | 0 | SrcA/InvSrcA +AR | | fx-alpha (particle, 0 tris) |
| 2 | 14084 | 140 | 172 | 121 823 | opaque | | prop-lit-noclip |
| 3 | 20041 | 118 | 160 | 92 870 | opaque | mod | prop-lit |
| 4 | 157432 | 117 | 117 | 114 064 | DstColor/SrcColor (2x mult.) | | fx-multiply (unsupported) |
| 5 | 40323 | 112 | 113 | 54 314 | opaque | | character (diffuse fileId 0) |
| 6 | 1053007 | 103 | 113 | 0 | One/InvSrcColor | | fx-soft-additive (particle, dye palette) |
| 7 | 27305 | 94 | 152 | 203 878 | opaque | mod | prop-lit-noclip |
| 8 | 977200 | 93 | 132 | 0 | One/InvSrcA | mask | fx-fire (unsupported) |
| 9 | 1729747 | 78 | 102 | 438 337 | opaque | specular | prop-spec (new row) |
| 10 | 1171222 | 74 | 92 | 417 044 | opaque | glow, mask | character (diffuse fileId 0) |
| 11 | 87345 | 65 | 65 | 193 710 | One/One | | fx-soft-additive |
| 12 | 60027 | 63 | 100 | 91 965 | opaque | decal, mod | prop-decal (new field) |
| 13 | 2242223 | 61 | 64 | 315 993 | opaque | glownoise, mask | character (diffuse fileId 0) |
| 14 | 1171330 | 59 | 93 | 0 | One/InvSrcA | | fx-premultiplied (particle) |
| 15 | 27303 | 58 | 77 | 78 180 | opaque | mod | prop-lit |
| 16 | 19910 | 52 | 68 | 120 222 | opaque | decal, decalmask, decalnormal, mod | prop-decal (new field) |
| 17 | 16128 | 52 | 52 | 61 618 | opaque | | character (diffuse fileId 0) |
| 18 | 573675 | 51 | 51 | 70 085 | opaque | glow, glowperturb | character (diffuse fileId 0) |
| 19 | 23672 | 48 | 55 | 69 405 | opaque | | prop-lit-noclip |
| 20 | 3718974 | 43 | 43 | 42 685 | DstColor/SrcColor | | fx-multiply (unsupported) |
| 21 | 882183 | 41 | 42 | 0 | SrcA/InvSrcA +AR | | not read (procedural `startcl/endcl/flow/stepcol` effect, 0 tris) |
| 22 | 31576 | 40 | 40 | 60 237 | opaque | | character (diffuse fileId 0) |
| 23 | 1171331 | 40 | 53 | 0 | One/InvSrcA | fire, mask | fx-fire (unsupported) |
| 24 | 23508 | 39 | 47 | 73 528 | opaque | addnormal, mod | prop-lit-noclip |
| 25 | 16104 | 39 | 48 | 63 909 | opaque | | prop-lit |
| 26 | 56533 | 39 | 44 | 33 934 | opaque | decal, decalnormal, mod | prop-decal (new field) |
| 27 | 15206 | 38 | 40 | 2 056 | DstColor/Zero | | fx-multiply (unsupported) |
| 28 | 709501 | 38 | 39 | 0 | One/InvSrcColor | | fx-soft-additive (particle) |
| 29 | 16081 | 37 | 41 | 8 560 | opaque | | character eyes (`hseye*`, diffuse fileId 0) |
| 30 | 519245 | 32 | 42 | 64 541 | opaque | addnormal, decal, decalnormal, mask, mod, subsurface | not read (181 instr.; decal + subsurface + rim) |

Coverage **if the rows in 8.4 are added** (rows marked unsupported still count:
they get a named profile and a warning instead of `default`):

| | materials | triangles |
|---|---|---|
| current table | 54.6% | 51.0% |
| + hand-read rows (8.4) | +15.5% -> **70.1%** | +14.1% -> **65.1%** |
| + rows assigned by signature only | +4.0% -> 74.1% | +1.5% -> 66.6% |
| of the new coverage, unsupported rows (multiply, refraction, cubemap, fire) | 3.0% | 1.0% |
| diffuse fileId 0 (character path, out of scope) | 7.2% | 10.4% |
| still `default` | 18.7% | 23.0% |

The remaining default is a long tail: 1 222 AMATs in the sample, and past rank
~60 no AMAT has more than 8 models. Most of it is map-prop variants of the
families below (decal + subsurface + rim, window glow, metal masks) and
procedural effect shaders.

### 8.2 The nine known unmapped AMATs (from real exports)

Slots are `tN` = the MODL material's `textures[N]`; roles are castlemist's
(`materialRoles`), UVs are the MODL `uvIndex`. "Clip" = a `discard` on a value
derived from a texture sample, scanned over **every** FSH in the AMAT
(stipple `round_pi`/`fxclr` and `fxclr.w < AlphaRef` fades excluded).

**57131** (Sacred Crystal Rifle 3321621 mat 0; also 915773, 1451520). Opaque, rs 0.
Slots t0 diffuse UV0, t1 normal, t2 `decal` UV1, t3 `decalperturb` UV2 (same file as the normal).
Colour PS 55:
```
sample r2, v1.xy, t0 ; add r1.w, r2.w, -0.5 ; add_sat r1.w, r1.w, r1.w      // shine = saturate(2a-1)
mul r2.w, r1.w, envcp.w ; mul r3.xyz, r1.w, speccp.xyz                      // reflection + sun spec
mad r1.xy, -viewTS.xy, pardist.x, v0.zw                                     // decal UV parallax
sample r1.zw, v0.xy, t3.zwxy ; mad r1.zw, ..,2,-1 ; mad r1.xy, r1.zw, decptrb.x, r1.xy
sample r1.xyz, r1.xy, t2            // decal rgb at perturbed UV
sample r1.w, v0.zw, t2              // decal alpha at unperturbed UV
mad r2.xyz, r1.w, (diffuse - decal), decal          // albedo = lerp(decal, diffuse, decal.a)
mul r1.xyz, r1.xyz, glowcol.xyz ; add x2 ; add_sat r1.w, -r1.w, 1
mad r0.xyz, r1.xyz, r1.w, lit                      // emission = decal.rgb*glowcol*2*(1-decal.a)
```
No texture discard in any of the 89 shaders (stipple 8, AlphaRef 3). Diffuse
alpha = **shine only**. The decal uses the 54632 order (decal **alpha 1 = show
the diffuse**), and the decal also **glows where its alpha is low**. Stats (used
texels): diffuse A 126-255 (96% >= 128); decal A 0-255, 61% < 16 (decal shown
and glowing), 36% >= 128. Needs a new field: emission from the decal layer.

**23507** (props 85938, 2563852, 469921 ...; 7 mats / 3 models in the sample; 0x10006565000 =
SrcA/InvSrcA + ALPHA_REF 1, both the "opaque" and the transparent effect). t0 diffuse, t1 `ramp`
(token 830674056242, castlemist leaves it unnamed). PS 30:
```
dp3 r0.x, v2.xyz, v1.xyz ; mov r0.x, |r0.x| ; sample r0.x, r0.xx, t1   // ramp(|N.V|)
sample r1, v0.xy, t0 ; mul r0.x, r0.x, r1.w                            // a * ramp
mad r0.y, r0.x, diffade.x, -AlphaRef.x ; mul o0.w, r0.x, diffade.x     // o.a = a*ramp*diffade
lt/discard_nz r0.y                                                      // AlphaRef fade only
mad o0.xyz, r1.xyz, (1-fog), fog                                        // rgb NOT premultiplied
```
Diffuse alpha = **straight opacity** (not intensity: the RGB is not multiplied,
the blend is SrcA). The only discard is `< AlphaRef` (opacity ~0), so no clip.
Stats (85938 mat 2): A 0-159, 61% < 16. The fresnel ramp cannot be a map (warn).
-> new row `fx-alpha` with existing fields.

**44709** (44730 mat 3, 1713241, 633441; SrcA/InvSrcA + ALPHA_REF). Same layer
set as 23507 (t1 = the same ramp file 23493) but forward SH-lit (shRed/Green/Blue/shSun):
```
add_sat r0.y, r2.w, r2.w ; mul r0.x, ramp, r0.y ; mul o0.w, r0.x, diffade   // opacity = saturate(2a)*ramp*diffade
add r0.w, r2.w, -0.5 ; add_sat r0.w, r0.w, r0.w ; mul r1.xyz, r0.w, rgb ; add x2
mad r0.xyz, rgb, SHlight, r1.xyz                                             // + unlit rgb*saturate(2a-1)*2
```
Diffuse alpha is **dual**: 0-127 = opacity ramp 0->1, 128-255 = fully opaque
plus **self-illumination** 0->1. Stats (44730 mat 3): A 2-255, 69% >= 128, 9% < 16.
-> needs `AlphaUse::OpacityAndGlow`.

**1465623** (Astral Ribbons 3121936 mat 0). Opaque, clips. t0 diffuse, t1 normal, t2 `ramp`
(80599), t3 `mask` (1459277 = 4x4 **red placeholder**, so R = 1). PS 76 is the
561567 lit core (clip `saturate(2a) < 0.5`, shine, `conduct`, `speccp`, `envcr`, reflection mip) plus:
```
dp3 r1.x, V, N                                   // N.V
sample r0.w, v0.xy, t3.yzwx  (= mask.R) ; add r1.y, r0.w, voffset.x
sample r1.xyz, r1.xy, t2                         // ramp(N.V, mask.R + voffset)
mul r1.xyz, r0.w, r1.xyz ; mad r0.xyz, r1.xyz, shSunColor.xyz, r0.xyz
```
The "glow" is a **fresnel rim ramp**, gated by mask.R and scrolled by `voffset`;
the ramp texture is a lookup table, not UV-mapped art. Stats: diffuse A 103-188
(52% matte, 48% shine, no holes). With today's builder the ramp is not found as a
glow (role "ramp"), so nothing glows. Needs a field (rim colour from the ramp layer).

**2329259** (Astral Ribbons mat 1). Opaque, rs 0. t0 diffuse, t1 normal, t2 `specular`
(not read by the colour PS), t3 `glow` UV1, t4 `decal` UV1, t5 `detailnormal` (529633 flat),
t6 `mask` UV1, t7 `prism` UV1, t8 `diffademask` (13368 white), t9 `dyemask`. PS 127 (248 instr.):
- only texture discard (50 of them, every pass) is on **t8 diffademask**:
  `diffade*(0.5x+0.5)+0.5x < 0.5` = a dissolve-in, never fires with the white
  placeholder. **The diffuse alpha never clips**; it is shine (`saturate(2a-1)` x envcp.w, reflection mip);
- albedo = `lerp(diffuse, decal(parallax paradis/dectile), decal.a * mask.G)`;
- two parallax flake layers (apardis/ascale, bpardis/bscale, flkscal) from t5/t7,
  a `prism` lookup rotated by `Time*prsmspd`, x `layera`/`layerb`, x `viewpow`, x mask.G;
- emission = `glow.rgb*glwstr + prism`, then `c + mask.R*(mask.G*dye4(c) - c)` (dye channel 4, hsmntl/m/n);
- armor dye (hsmnt* 4x4, dyemask RGBA) and `rimcp` rim, as armor-mask.
So **mask G = decal/prism layer weight, mask R = dye weight of the emission**, not
metal/gloss. Stats: diffuse A 100-255 (98% >= 128); mask (UV1) R 0-115, G 142-255, B 0, A 255.
-> supported core (diffuse+normal+shine+dye+glow), animated flake/prism layers raw:
`animatedGlowLayers` fits.

**511663** (Skyforged Hammer 3123167 mat 1; 1823422 mat 0). Opaque, rs 0. t0 diffuse, t1 normal,
t2 `cutout` UV1, t3 `glow` UV1, t4 `perturb` UV2 (3123167: t3 = the diffuse file, t4 = the normal file).
```
sample r0.xy, v1.xy, t4 ; mad r0.xy, cutptrb.x, r0.xy, v0.zw        // perturbed UV1
sample r0.zw, r0.xy, t2.yzxw  (= cutout.R, cutout.A)
sample r1.xyz, r0.xy, t3 ; mul r1.xyz, r1.xyz, glofade.x            // glow at the same UV
mul r0.x, r0.w, r0.z ; mad r0.x, r0.x, cutfade.x, -0.5 ; lt ; discard_nz   // clip: cutout.R*A*cutfade < 0.5
add r0.xy, Time.zw, 1 ; ... mad_sat r0.x, -r0.x, 0.4, 1 ; mul glow, r0.x    // flicker
sample r3, v0.xy, t0 ; shine = saturate(2a-1) -> envptrb, speccp, envcol
```
The clip is on the **cutout layer**, never on the diffuse alpha (39 texture
discards, all on t2/t3/t4). Stats: cutout R*A >= 0.5 on 100% of used texels on
both models (cutfade const 1.0): at rest nothing is cut; it is a dissolve.
Diffuse A: 163-165 (3123167), 224-255 (1823422) = shine. -> works today as
`clips=false, Shine` with the existing glow/perturb handling; a faithful clip
needs a `cutoutLayer` field.

**842652** (Astralaria 1200313 mat 3/4; SrcA/InvSrcA + ALPHA_REF, unlit).
Slots: t0 57890 UV3 (castlemist calls it **diffuse**: token ...144), t1 `mask` UV1,
t2 `mskptrb` UV2 (same file 57890), t3 `parallax` 842653 UV0.
```
sample r0.xy, v0.zw, t0 ; mad r0.xy, paraper.x, r0.xy, v0.xy        // t0 is a UV-offset map
mad r0.xy, -viewTS.xy, pardist.x, r0.xy
sample r0, r0.xy, t3                                                  // COLOUR comes from 'parallax'
sample r1.xy, v1.zw, t2 ; mad r1.xy, cutptrb.x, r1.xy, v1.xy ; sample r1.x, r1.xy, t1
mul r0.w, r0.w, r1.x ; mul o0.w, r0.w, diffade.x                      // opacity = col.a*mask.R*diffade
o0.rgb = col.rgb (fog only, no lighting)
```
castlemist's "diffuse" here is a perturb map and the real colour is the
`parallax` layer. Stats: parallax A = 255 everywhere; mask R 0-255 (29% < 16,
42% >= 128). Needs `baseColorRole` + an opacity-layer field.

**221571** (Twilight 543947 mat 6, 1713241). Opaque, rs 0, 9 instructions:
```
sample r0.xyz, v0.xy, t0 ; mad r0.xyz, -r0.xyz, 0.03, -V        // t0 (415018) perturbs the direction
sample r0, r0.xyz, t1 (texturecube 221582) ; div/square           // material CUBEMAP
o0.rgb = cube^2 (fog only)
```
An unlit "starfield inside the blade": colour = a material cubemap looked up by
view direction. Nothing UV-mapped. -> unsupported (export the cubemap raw; a
Poiyomi cubemap/matcap slot would be the place).

**49659** (Skyforged Hammer 3123167 mat 0; 1215551). Opaque word but a refraction shader:
t0 (3123153, token "distort", castlemist: normal) UV0, t1 `decal` 3123155 UV1, t12 depth,
t14 = **scene colour** (refraction source), t15.
```
sample r0.zw, v0.xy, t0.zwxy ; RG*2-1 ; mad r0.zw, ., distort.x, distort.x   // screen offset
... depth test against t12 ; sample r0.xyz, screenUV+offset, t14 ; mul modxcol x2   // refracted scene
normal = t0.RG (Z rebuilt)                                 // the same map is the lighting normal
sample r4, v0.zw, t1 ; add_sat r1.w, r4.w, r4.w           // decal coverage saturate(2a)
lerp(refraction, decal.rgb*light, saturate(2a)) ; reflection x fresnel^1.5 x min(1,(1-sat(2a))+sat(2a-1))
sun spec pow(N.H,256)
```
Taking t0 as the normal is right (it is used as one), but the material has no
colour of its own: decal alpha 0-127 = how much decal covers the **refracted
background**, 128-255 = decal + more reflection. No clip. Stats (3123167 mat 0):
decal A 0-96, 70% < 16 (mostly see-through glass); t0 RG 122-133 (near flat).
Same family by uniforms (`distort`, `modxcol`, t12/t14 scene samples): **63923**
(SrcA/InvSrcA, o.a = mask.G UV1), **57026** (SrcA/InvSrcA, o.a = mask.R). -> unsupported
`glass-refract` (needs a grab-pass refraction; decal-as-BaseColor with alpha
`saturate(2a)` is the nearest static approximation).

### 8.3 Census families (hand-read)

**prop-lit, clipping** (the existing `prop-lit` row): diffuse t0, `saturate(2a) < 0.5`
discard, shine `saturate(2a-1)` x speccp x2 x sun; colour PS often has no normal
(the deferred prepass reads it). Read: **20041** (+ `mod` UV1 x2; 62080 identical opcode stream),
**56795** (x vertex colour x2; 69668 identical), **57606** (vertex-colour tint gated by
`tintermask`.A: `lerp(0.5, vc, tinter.A)*2`; 75035 identical), **57714** (mod x vc),
**57752** (clip `saturate(2a)*fadedif`, second mod lerped by `fademod`),
**53237** (`height`.A - 0.5 x `bumpoff` UV offset), **16104** (env reflection
`envcol`, fresnel `envptrb`), **27303** (env + mod), **57685**/**60319**/58654
(foliage: + vertex colour, `subsurface` RGB x albedo x back-light term; the subsurface layer
goes out raw). Stats: 20041 (469921 mat 13) diffuse A 0-255, 36.5% < 16 = real holes.

**prop-lit, no clip** (the existing `prop-lit-noclip` row): **14084** (diffuse only;
A 127-152 = mostly matte), **23508** (+ mod; 73205, 73655, 84923 identical),
**27305** (env + mod; 76858 identical; A 10-255, 9.6% < 64 = matte, not holes),
**23672** (env), **54889** (two mods, second x `fademod`; 231183 identical),
**57701** (x `ModelColor` x mod), **75778** (+ `glow` UV0 x glofade x2 with the
Time flicker of 511663).

**13361** (13559 mat 0): t2 `specular` (RGB x2 sun spec, A x128 exponent), t3 `backlight`,
`blint`, shine -> envcp.w only, clips on the diffuse alpha. Same as the clipping
`legacy-spec` row (1891783).

**prop-unlit**: **77876** = `diffuse x vc x 2 x light`, discard `saturate(2a)<0.5`,
**no shine** (holes only). **77598** (189570 identical) = `diffuse x vc x 2 x light`,
alpha never read (A = 255 on 77601). Both fit existing fields with `diffuseAlpha=Unused`
(the builder already cuts holes from `clips` alone).

**prop-decal** (new convention, 14 AMATs read): 56533 (60027, 62212 identical), 44479
(60145 identical), 19910 (525886 identical), 69887, 69913, 79884, 60530, 69713, 57806, 2597095.
```
sample r0, v0.zw, t_decal ; add_sat r0.w, r0.w, r0.w           // coverage = saturate(2*decal.a)
[ sample t_decalmask/mask/blend ; mul r0.w, r0.w, mask.ch ]       // 19910 decalmask.R, 69887 mask.R, 60530 blend.G
lerp(diffuse.rgb, decal.rgb, coverage)
shine = lerp(saturate(2*diffA-1), saturate(2*decalA-1), coverage) // decal alpha upper half = decal shine
```
This is the **opposite** of 54632/57131 (`lerp(decal, diffuse, decal.a)`), so the
existing extra hint "lerp-by-decal-alpha" is ambiguous. No texture discard in any pass
of any of them. 57806 also adds `decal.rgb * saturate(2*decal.a - 1)` **unlit** (decal
upper half = self-illumination); 69887 also multiplies the specular by mask.B.
Stats: 60027 (469921 mat 11) decal A 0-160, 74% < 16 (no decal), 12% >= 128.
**19910 mask UV (checked 2026-10-07, PS 43 + VS 0):** t2 `decal` is sampled at `v0.zw`, t4
`decalmask` at `v1.xy`, t5 `mod` at `v1.zw`; the VS writes `o0.zw` from TEXCOORD1 and `o1.xy`
from TEXCOORD2 (`TexTransform2`), so the decalmask really is on **UV2** (the MODL uvIndex is
right). On 469921 Metal7 the decal A is 252-255 everywhere: the UV2 decalmask alone decides
coverage, so the VRChat export ships it as its own map (`maps.decalMask`).

**prop-projector** (77238, 835499 identical; 69856, 69792, 512093, 512112 identical):
```
f = saturate(N.z-ish * -0.5 + 0.5)^2 ; w = smoothstep(prjfall.x, prjfall.y, f)   // faces pointing up
w *= saturate(2*projector.a)
albedo = lerp(diffuse*mod*2, projector.rgb, w) ; shine = lerp(shineD, shineP, w)
```
A world-normal-driven overlay (snow/moss). Clips on the diffuse alpha only in
512093/512112. Stats: 77238 (2141433 mat 7) projector A 128-255 (full coverage +
shine). Not bakeable as a UV map without the geometry.

**prop-metalmask** (3121953; 3576632, 3576633 by uniforms/roles): the 561567 lit core
(clip, shine, envcr, reflection mip), spec/reflection tint
`lerp(envcr.x, 0.6*albedo+0.2, metalmask.G)` (metalmask t2.G), mod x2 UV1.

**cutout + mod** (53858, 72k triangles in the sample): discard
`cutout.R * saturate(2*diffA) < 0.5` (cutout on UV2), shine, mod. Same need as 511663.

**fx**: 19255 (0 tris, particle: rgb x vc x2, o.a = a x vc.a x soft depth, SrcA) -> `fx-alpha`;
48767 (0 tris, One/InvSrcA, `step` erosion of a x luminance) -> `fx-premultiplied`;
15206 (DstColor/Zero) and 157432 (DstColor/SrcColor, normal-map-driven shading, 5k tris) ->
multiply decals, unsupported.

**Character shaders** (31686, 41268, 40323 `sylglow`, 16128, 16081 `hseye*`, 16027):
`CharLightCurve`/Backlight uniforms and **every texture fileId is 0** in the MODL:
the textures are composited at runtime (body, face, eyes, hair). The model export
cannot reach them; they belong to the character path. Note 31686 cuts at
`min(4a,1) < 0.5`, i.e. **a < 32**, not 64.

**prop-spec** (**1729747**, 438k triangles in the sample, rank 9; 2789316 mat 0):
t0 diffuse, t1 normal, t2 `specular`. Clip `saturate(2a) < 0.5`, shine `saturate(2a-1)`;
`add_sat r2.xyz, spec, spec` = spec colour x shine x sun; reflection `cube * spec.rgb * 2`,
reflection mip `4*saturate(1 - fade*spec.A)`, so **spec.A is gloss**. No material
constants at all. This is the 2348484 layout without the glow: `weapon-spec`
fields with `glowOnUv2MaskOnUv0 = false`.

**Weapon families seen again:** **3423592** (t2 specular, t3 glow UV2 perturbed by t5
glowperturb UV1 x gloptrb, t4 glowmask UV0, glofade) = the 2348484 layout, so `weapon-spec`.
**1203843** = the 561567 lit core (clip, shine, `conduct`, speccp), but the reflection reads a
**material cubemap** t4 x `cubecr` instead of the global t13: `weapon-glow` (cubemap raw).
**2212806** = the 561567 core with a **second glow** (`glowb` x `globfad`, perturbed by
`globptb`, masked by `glowmaskb` on UV3): `weapon-glow`, second layer raw.

**fx:** **87345** (One/One, 194k triangles, real meshes): `o.rgb = diffuse.rgb *
t1(UV + (t2.RG*2-1)*0.0597) * shSunColor * diffuse.a`, `o.a = a`, so intensity,
premultiplied: `fx-soft-additive`. Its texture tokens are 0, so castlemist's role
names are empty. **977200** / **1171331**: two noise samples scrolled by `Time*speed`,
UV perturbed by a flow map x `perturb`, colour `lerp(colorl, colorh, ...)`, soft depth fade.
This is the procedural flame, same as `fx-fire`. **1053007**/**1171330**: colour =
a palette texture sampled at the constant `dyedot` (runtime dye palette, fileId 0) x t1 x
vertex colour, premultiplied by alpha (0 tris). **3718974**: DstColor/SrcColor,
`0.5 + dot(N, normalmap) * t1 * opacity` (a lighting-multiply decal), unsupported
like 157432.

**Creature/character shaders with an empty diffuse slot** (1171222, 2242223, 573675,
31576, 40323, 16128, 31686, 41268, 16081, 16027 ...): Backlight/`CharLightCurve`
uniforms and `textures[0].fileId == 0`. 1171222 has a real `mask` (G x speccp.w =
exponent) and a `glow` x glowcm; 2242223 has a day/night `glownt`/`glowday` x `pulse` glow;
573675 has a glow scrolled by `gpscscr`. Without the composited diffuse, the model
export has nothing to map. These belong to the character pipeline and stay
on `default`.

**Read, no row proposed:** **46230** (43k tris; the `amat` selection finds no
colour PS, so FSH 11 was used): no textures; colour = the `dcolor` constant, lit by
the light buffer, with an `fxclr` fresnel rim. It would need a "constant base colour"
field. **1465535** (131k tris, same problem, no selected PS): an `fxclr`/`opacity`
fresnel overlay over diffuse x normal. Its purpose is unclear. **882183** and the other
`startcl/endcl/flow/stepcol` SrcA shaders: procedural, 0 triangles.

### 8.4 Proposed profile rows

Values are `ShaderProfile` fields. Unlisted fields keep their defaults
(`supported=true`, `clips=false`, all mask channels `None`, `specLayer=None`,
`opacityTexture=-1`, the bools false). "(sig)" ids were assigned by an identical
opcode stream or by uniform/role signature. They were not read line by line.

| AMATs | profile | fields | evidence |
|---|---|---|---|
| 20041, 62080 (same opcodes), 56795, 69668 (same), 57606, 75035 (same), 57714, 57752, 53237, 16104, 27303, 57685, 60319, 58654 (same); (sig) 44627, 44558, 50686, 72532, 44200 | **existing `prop-lit`** | HolesAndShine, clips | t0 `saturate(2a)<0.5` discard in the colour PS and the prepasses; 20041: 36.5% of used texels < 16 |
| 14084, 23508, 73205, 73655, 84923 (same as 23508), 27305, 76858 (same), 23672, 54889, 231183 (same), 57701, 75778; (sig) 75623, 48835 | **existing `prop-lit-noclip`** | Shine | no texture discard in any pass; 14084 A 127-152; 27305 9.6% < 64 = matte |
| 13361 | **existing `legacy-spec` clip row** (with 1891783) | ReflectionOnly, clips, ExponentInAlpha | spec A x128 exponent, shine x envcp.w only, t0 discard |
| 3423592 | **existing `weapon-spec`** | HolesAndShine, clips, GlossInAlpha, glowOnUv2MaskOnUv0 | glow UV2 / glowmask UV0 / spec A = reflection mip |
| 1203843, 2212806 | **existing `weapon-glow`** | HolesAndShine, clips | 561567 core; the 1203843 material cubemap and the 2212806 second glow go out as raw extras |
| 87345, 1053007, 709501 | **existing `fx-soft-additive`** | Intensity, premultiplyRgbByAlpha | `o.rgb = ... * a` (87345 is One/One, not One/InvSrcColor) |
| 48767, 1171330 | **existing `fx-premultiplied`** | Intensity | One/InvSrcA, rgba x vc / step erosion |
| 977200, 1171331 | **existing `fx-fire`** (unsupported) | | scrolling noise x colorl/colorh |
| 1729747 | **`prop-spec`** (new row) | HolesAndShine, clips, specLayer=GlossInAlpha | spec.A = mip, spec.RGB = spec/reflection colour |
| 77876 | **`prop-unlit-holes`** (new row) | diffuseAlpha=Unused, clips | discard on a, no shine term (the builder already cuts holes from `clips`) |
| 77598, 189570 (same) | **`prop-diffuse-only`** (new row) | diffuseAlpha=Unused | alpha never read (A = 255) |
| 23507, 19255 | **`fx-alpha`** (new row) | diffuseAlpha=Opacity | SrcA/InvSrcA, `o.a = a*ramp*diffade`, RGB not premultiplied; only an AlphaRef fade |
| 2329259 | **`armor-prism`** (new row) | Shine, animatedGlowLayers | no diffuse discard (only the diffademask dissolve); flakes/prism animated; mask G = layer weight, R = emission dye weight (not metal/gloss) |
| 511663 | **`weapon-cutout-glow`** (new row, interim) | Shine | the clip is on the `cutout` layer, which never cuts at rest (R*A >= 0.5 on 100% of used texels); glow and perturb are already handled by role |
| 157432, 3718974, 15206; (sig) 54485 | **`fx-multiply`** (new, unsupported) | supported=false | DstColor/SrcColor or DstColor/Zero blend |
| 221571 | **`fx-cubemap`** (new, unsupported) | supported=false | colour = material cubemap by view direction |
| 49659, 63923, 57026; (sig) 729279, 592660, 797215, 55669, 52624, 959502, 74113 | **`glass-refract`** (new, unsupported) | supported=false | samples the scene colour t14 offset by `distort`; decal alpha = coverage over the refraction |
| 56533, 60027, 62212 (same), 44479, 60145 (same), 19910, 525886 (same), 69887, 69913, 79884, 60530, 69713, 2597095; (sig) 76643, 81309, 62170 | **`prop-decal`** | Shine + **new `decalMode=DecalOverDiffuse`** (+ `decalMask` role/channel: 19910 decalmask.R, 69887 mask.R, 60530 blend.G) | `lerp(diffuse, decal, saturate(2*decal.a)[*mask])`, shine lerped to `saturate(2*decal.a-1)`; no texture discard |
| 69623 (sig) | `prop-decal` with clips | HolesAndShine, clips, decalMode | t0 discard (signature) |
| 57806 | `prop-decal` + **new `decalGlow=AboveHalf`** | Shine, decalMode=DecalOverDiffuse | `+ decal.rgb * saturate(2*decal.a-1)` unlit |
| 57131 | **`decal-glow`** | Shine + **`decalMode=DiffuseOverDecal`, `decalGlow=BelowHalf`** (x `glowcol` x2) | `lerp(decal, diffuse, decal.a)`; emission `decal*glowcol*2*(1-decal.a)`; decal A 61% < 16 |
| 44709 | **`fx-alpha-glow`** | **new `AlphaUse::OpacityAndGlow`** | opacity `saturate(2a)*ramp*diffade`, unlit `rgb*saturate(2a-1)*2`; 69% of texels >= 128 |
| 1465623 | **`weapon-rim-ramp`** | HolesAndShine, clips, maskGlowGate=R + **new `rimRampRole="ramp"`** | glow = `ramp(N.V, mask.R + voffset)`; the ramp is a lookup, not UV art |
| 511663, 53858 | (faithful version) | + **new `cutoutLayer`** {role "cutout", value R*A (511663) or R x `saturate(2a)` (53858), threshold 0.5, own UV} | `discard` on the t2 cutout; 53858's cutout is on UV2 |
| 842652 | **`fx-parallax-layer`** | Opacity + **new `baseColorRole="parallax"`, `opacityLayer`={role "mask", channel R}** | colour comes from t3 `parallax`; castlemist's "diffuse" t0 is a UV-offset map |
| 77238, 835499 (same), 69856, 69792; (sig) 69634 | **`prop-projector`** | Shine + **new `projectorRole="projector"`** (weight `smoothstep(prjfall.x, prjfall.y, f(N))*saturate(2*proj.a)`) | world-normal overlay; not a UV bake |
| 512093, 512112 (same) | `prop-projector` with clips | HolesAndShine, clips | t0 discard |
| 3121953; 3576632, 3576633, 3121869 (uniforms/roles only) | **`prop-metalmask`** | HolesAndShine, clips, maskMetal=G + **new `maskRole="metalmask"`** | tint `lerp(envcr.x, 0.6*albedo+0.2, metalmask.G)`; the builder only reads role `mask` today |

**New fields, in order of payoff:**

1. `decalMode` {None, DiffuseOverDecal, DecalOverDiffuse} (+ optional
   `decalMask` role/channel, `decalGlow` {None, BelowHalf, AboveHalf}). The
   commonest map-prop family uses decal alpha the opposite way from 54632, but
   today both get the same "lerp-by-decal-alpha" hint. About 15 AMATs, 3% of triangles.
2. `cutoutLayer`: the clip of 511663 and 53858 is not on the diffuse alpha, and
   the layer has its own UV. So `opacityTexture` (which samples at the diffuse
   texel) cannot fold it into the BaseColor alpha. The target is Poiyomi's alpha
   mask with its own UV.
3. `projectorRole`: the weight depends on the world normal. Export the projector
   raw with a warning, or bake it against the mesh normals.
4. `maskRole`: the mask-channel fields only look at role `mask`.
5. `AlphaUse::OpacityAndGlow` (44709; same curve as 187842's opacity/shine).
6. `baseColorRole` + `opacityLayer` (842652). `glass-refract` would also need these
   to become supported, with `baseColorRole="decal"`.
7. `rimRampRole` (1465623).

### 8.5 Other findings

- **Clip cutoffs differ:** 31686 (character) discards at `min(4a,1) < 0.5`, which
  is **a < 32**. 57752 multiplies by `fadedif` before the 0.5 test. The 64
  cutoff holds for every prop and weapon shader read here.
- **Wrong castlemist roles:** in 842652 the "diffuse" is a perturb map. In 49659
  the "normal" is the `distort` map, though it is also used as the normal there.
  23507/44709 leave the `ramp` (token 830674056242) unnamed. 87345 has all tokens 0.
- **Same file in two slots:** on 3123167, 511663 uses the diffuse file as the glow
  and the normal file as the perturb map. 57131 uses the normal file as the decal
  perturb map. The glb/VRChat export must not dedupe layers by fileId alone.
- `amat` picks no colour PS for 46230 and 1465535 (`psIndex -1`). Among unmapped
  shaders reachable by the model export, they hold 43k and 131k triangles in the sample.
- In the sampled weapons, UV1 equals UV0 (3123167, 1823422 and 3321621 show the
  same ranges in `model`), so the UV1 stats above are the UV0 texels.

Open: 519245/106586/1601802 (decal + subsurface + rim, 181-185 instructions, not
read). The window-glow family (69633, 79747: `windowglow`, `TimeOfDay`). 2571412
(jade-interior-like `int*` uniforms; it probably joins 2472137). Whether
`decalMode` can be read off the PS automatically: the `add_sat` of the decal
alpha before the lerp gives it away.
