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
