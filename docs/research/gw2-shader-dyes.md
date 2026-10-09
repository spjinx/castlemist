---
name: gw2-shader-dyes
description: Mounts dye in the pixel shader via a dyemask texture and twelve hsmnt* affine rows (MODL material constants); how castlemist overrides them, and what that implies for a material/shader editing UI
metadata:
  type: project
  modified: 2026-10-08T00:00:00.000Z
---

Armor is dyed **on the CPU** into the runtime character atlas
([gw2-armor-skins-and-dyes.md](gw2-armor-skins-and-dyes.md) section 4). Mounts are
not: their textures are used as-is, and the dye is applied **in the pixel shader**
from per-material uniforms. Measured 2026-10-08 on the Springer (mount skin 3,
model fileId 1750673, MFT 143863) and the Dark Monarch Skyscale (skin 292, model
2306945, MFT 569032).

## Finding a mount's model

`cmap::resolve_all(302, skinId)` (CLI: `gw2dat_cli users --content-type 302
--content-id N`) gives the skin's icon **and its MODL fileIds directly**
(skin 3 -> 1766899 icon, 1750673 model). `/v2/mounts/skins/{id}` lists the skin's
`dye_slots`: per slot the default `color_id` **and the material**, so unlike armor
the material is not a guess (Springer: 466 Old Penny, cloth; Dark Monarch: 4
slots, all metal).

## The material side

Every dyeable material carries twelve MODL constants. Their token32 names
decode (base-23, `game_shader.cpp decode_token`) to `hsmnt` + a..n **skipping
k** (j/q/z aren't in the alphabet; `k` is simply unused):

| channel | rows (R, G, B) | MODL default |
|---|---|---|
| 1 | `hsmnta hsmntb hsmntc` | (1,0,0,0) (0,1,0,0) (0,0,1,0) |
| 2 | `hsmntd hsmnte hsmntf` | identity |
| 3 | `hsmntg hsmnth hsmnti` | identity |
| 4 | `hsmntl hsmntm hsmntn` | identity |

(`hsmnta` decodes as `hsmnt`: a trailing `a` is digit 0.) Some materials ship
partly zeroed rows (Dark Monarch mat 1/2: `hsmnth` = 0, `hsmntm` = (0.42,0,0,0)):
the engine overwrites all twelve at draw time from the character's dyes, so the
file values only matter for an undyed preview.

The texture with role **`dyemask`** (5-bit token64 27219522307397412; decode with
`detokenizeName64`) is the per-texel channel weight: R/G/B/A = channels 1-4.
Other role tokens seen on these materials: `diffuse normal specular backlight
dyemask dyemaskb decal flow mask ramp glow noise fireshape firedetail flowb bgmask`.

## The shader side

AMAT 1749831 (Springer body), colour effect PS 68, disassembled:

```
r0  = sample t0 (diffuse); discard if saturate(2a) < 0.5
r1  = (r0.rgb, 1)
r2  = (dp4 r1 cb0[9..11])            ; hsmnta/b/c . (rgb,1)
r0  = lerp(r0.rgb, r2, t4.r)         ; t4 = dyemask
r1  = lerp(r0, hsmntd/e/f . base, t4.g)
r0  = lerp(r1, hsmntg/h/i . r1, t4.b)
r0  = lerp(r0, hsmntl/m/n . r1, t4.a)
... then normal, specular (t2), backlight (t3), env cube, LightBuffer (t14)
```

So per channel `c = lerp(c, M_i * (rgb, 1), mask_i)` with `M_i` a 3x4 affine
matrix on **0..1 RGB**. The Dark Monarch body (AMAT 2306900, PS 63) packs the
same twelve uniforms **transposed** in the cbuffer (`hsmnta, hsmntd, hsmntg,
hsmntl, hsmntb, ...`), computes all four channels at once as a mask-weighted
sum, and samples a second mask `dyemaskb` that gates an animated (Time, noise,
`embfade`) glow. Uniform *names* keep their meaning in both layouts, so binding
by name is layout-proof.

## From a dye to the rows

`ripper::dye_matrix()` is the verified 4x4 on **0..255 BGR**. The shader rows are
the same matrix with rows and columns reversed and the offset scaled:

```
row_i[j] = M[2-i][2-j]  (i, j in R,G,B)      row_i[3] = M[2-i][3] / 255
```

`ripper::dye_shader_rows()`; `tests/test_ripper_shader_dye.cpp` checks it
against `apply_dye()` (and hence against the API's swatches) to within 1/255.

## What castlemist does with it

`ripper/shader_dye.{h,cpp}`:

- `has_shader_dyes(model)`: any material declares an `hsmnt*` constant.
- `shader_dye_channels(model)`: channels with any weight in some dyemask.
- `set_shader_dyes(model, pristine, uniforms)`: rewrites the `hsmnt*` entries of
  each `GameMaterial::psConsts` (Shader mode, by the uniform's byte offset), and
  for the reconstruction (Full mode) bakes a dyed copy of each
  (diffuse, dyemask) pair on the CPU with the same lerp.
- Game 1:1 loads its own copy of the model from the dat, so it takes the values
  through `gw2bgfxview::set_uniform_overrides(name -> float4)`, which wins over
  every draw's `matConsts` (also in the texture bake).

The viewer's Dyes window drives it: a model with no armor atlas but with
`hsmnt*` constants gets the window with `g_app->shader_dyes`, whose channels
start at colour 0 = **as authored** (MODL rows). Verified headless
(`GW2_AUTOLOAD=143863 GW2_GAMESHOT=1 GW2_DYES=10/0`): Sky on the Springer turns
its stripes and mane the same blue in Full, Shader and Game 1:1;
`GW2_DYES=10/2,466/2,584/2,11/2` dyes all four Dark Monarch channels.

Open:

- Default dyes from `/v2/mounts/skins` (slot colour + material) are not fetched
  yet; the window starts as authored.
- Before dyeing, the Springer's mane/stripes read **red** in Full mode but
  **dark teal** in Shader and Game 1:1 with identity rows. Both game paths
  agree, so the reconstruction is the odd one out; not chased.
- Outfits: the outfit content object (type 51) references only its icon. In
  game they composite into the character atlas like armor, so their meshes
  should go through `build_armor_preview`, but no outfit model has been opened
  to confirm it.

## Toward swapping materials, shaders and material elements in a UI

What this investigation shows is that a material is fully described by data
castlemist already holds, and all of it is addressable **by name**:

| element | where it lives | how a UI would change it |
|---|---|---|
| scalar/colour constants (`hsmnt*`, `envcp`, `blint`, `speccp`, `glofade`, `colorl`, ...) | MODL `constants[]` (token32 = uniform name) -> `GameMaterial::psConsts` / bgfx `matConsts` | same as dyes: override by name. `set_uniform_overrides` already does this for Game 1:1; Shader mode needs the per-material psConsts rewrite generalised from `set_shader_dyes` |
| textures | MODL `textures[]` (fileId + 5-bit role token) -> sampler slot via the AMAT's `samplers[]` | swap a fileId per role (e.g. another skin's `dyemask`/`diffuse`); Game 1:1 binds by `texByFileId`, Shader mode by `GameSamplerCPU::gameTex` |
| the shader itself | MODL `materialFile` -> AMAT -> effect picked by token64 | point a material at another AMAT. Works only when the replacement's samplers and uniforms are a superset of what the material provides: the uniform table (names, byte offsets) and sampler list are in the AMAT, so compatibility can be checked before the swap |

Constraints found on the way: uniform names are 23-letter base-23 tokens (`q`
spells as `v`, see `canonicalTokenName23`); a material's file constants can be
stripped from the DXBC when unused, so an editor should list the **shader's**
live uniforms (bgfx table) and show the MODL value or engine default beside
each, not the other way round; and the engine overwrites some per-draw values
(`hsmnt*` from dyes, Time, fog), so editing those in the file is not what the
game would show.
