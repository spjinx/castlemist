# Armor skins, race variants, the character atlas and dyes

Research for the character ripper's per-piece export (sub-project 2), done
2026-10-03 with castlemist's own tools (`gw2dat_cli` on single entries, the
`gw2index` DB) against a live Gw2.dat plus the public GW2 API. Everything
below was measured unless marked otherwise.

## 1. A skin's content entry

Armor/weapon/back skins are cntc content type 66 (`CONTENT_TYPE_SKIN`); the
object is 312 bytes. Fields used:

| Offset | Meaning | Evidence |
|---|---|---|
| +40 | API skin id (dataId) | already known (content_schema.h) |
| +48 | model fileref (file-index fixup) | `cmap::resolve_all()[0]` |
| +88 | icon fileref | `cmap::resolve_all()[1]` |
| +208 | **u64 appearance token** -- the key into the Composite file's `fileData` | identical for two skins that share one armor (517 and 1558); found in every race's fileData (section 2) |
| +216, +224, +232, +240 | external fixups to the four default dye colors (cntc type 9 objects whose dataId is the API color id) | skin 517 -> colors 67, 19, ... |

Tokens seen: Warden Coat 517 `0x00000348C28A32A3`, Warden Leggings 512
`0x00000C48C28A32A3`, Holographic Dragon set `0x??A5089844F38644` /
`0x0125089844F38644`, Benthic Hydrobreather 1993 `0x0021240A1C53E70F`.

(Corrected 2026-10-03: the token is at +208, not +200 -- an off-by-one-word misread of the dump row `+192: 0 0 0 0 C28A32A3 00000348 ...`.)

**The +48 model is only the HumanMale default.** For armor it is not what a
character of another race/gender wears (Holographic greaves: HumanMale
2693820 = the content-map model; SylvariFemale 2693907). Weapons and back
items are not in the Composite file; their +48 model is self-contained (it
carries its own textures).

## 2. The Composite file (`cmpc` / chunk `comp`, PackCompositeV20)

Exactly one entry: **fileId 154681** (baseId 131797, 14.6 MB decompressed).
Find it through the index: the entry carrying chunk fourcc `comp`.

The generic struct-template parser misreads it: in 64-bit (`pfVersion 5`)
packfiles a `fileref` is an **8-byte self-relative pointer** to a 4-byte
compressed filename (decode as `cschema::decode_fileref_pair`), not a 4-byte
value. Structs are packed (no alignment). Layout:

- Chunk data starts at file offset 28 (12-byte PF header + 16-byte chunk header).
- `array_ptr` = `{u32 count, i64 rel}`, rel counted from the i64's own address.
- `wchar_ptr` = `i64 rel` to UTF-16LE, NUL-terminated.

```
PackCompositeV20 (at 28)
  +0  armorColorIds  array_ptr<u32>
  +12 blitRects      array_ptr<BlitRectSet>      (40 B each)
  +24 boneScales     array_ptr<...>
  +36 raceSexData    array_ptr<RaceData>         (224 B each)
  +48 configVersion  u16

BlitRectSet: name wchar_ptr, size u32x2, rectIndex array_ptr<u8>, rectArray array_ptr<u32x4 {x0,y0,x1,y1}>

RaceData (224 B): name w, nameToken q, baseHeadToken q, beard a, bodyBoneScales a,
  bodyBoneScaleFiles a, ears a, eyeColorPalette w, faceBoneScales a, faces a,
  fileData a, flags u32, hairStyles a, hairColorPalette w, skeletonFile fileref(8),
  skinPatterns a, skinColorPalette w, skinPatternPalette w, skinStyles a, type u32,
  variantRefRace q, variants a, animOverrides a        (w=8, q=8, a=12)

FileData (103 B): name u64 (the token), type u8, flags u8, animRoleOverride u64,
  meshBase, meshOverlap, maskDye1, maskDye2, maskDye3, maskDye4, maskCut,
  textureBase, textureNormal (filerefs, 8 B each), dyeFlags u32, hideFlags u32,
  skinFlags u32, blitRectIndex u8
```

29 RaceData entries. The playable ones by name: `AsuraMale`, `AsuraFemale`,
`CharrMale`, `CharrFemale`, `HumanMale`, `HumanFemale`, `NornMale`,
`NornFemale`, `SylvariMale`, `SylvariFemale`. The rest are NPC/creature
variants (`CreatureCM`, `HumanMaleC`, `InvisHM`, `DwarfMale`, ...). The key
is `race + gender` exactly as `/v2/characters/:id/core` reports them.

Per armor piece and race (the FileData whose `name` == the skin's token):
`meshBase` is the model, `textureBase` the dyeable diffuse (painted around the
dye base hue, alpha = cut-out), `maskDye1..4` one grayscale (DXTA) mask per dye
channel (0 = channel unused), `maskCut` an extra cut mask, `textureNormal` the
normal map (3DCX), `hideFlags` which body regions the piece hides (used by
assembly later), `type` the piece kind (8 boots, 9 coat, 10 gloves, 11 helm,
12 leggings, 13 aquatic helm, 14 shoulders), `blitRectIndex` the BlitRectSet.

Example, Warden Coat for SylvariFemale: mesh 40405, base 151455 (512x256),
masks 151449/151451/151453/-, normal 151457.

## 3. The character atlas (how armor UVs work)

The game composites every armor piece's textures into one 1024x1024
character atlas; armor models' UVs address that atlas directly (no flip).

- BlitRectSet `blitRects[fileData.blitRectIndex]` (e.g. 2 = `ArmorHeavy`,
  1024x1024) lists the destination rects.
- A piece's rect is the one containing its armor meshes' UV bounding box
  (measured: aquatic helm rect (384,256,640,384), coat (0,512,384,1024),
  boots (896,256,1024,512), gloves (512,384,640,512), helm (0,0,384,256),
  leggings (512,0,1024,256), shoulders (0,256,256,512) in ArmorHeavy). The
  rectIndex -> type mapping was not decoded; containment is unambiguous.
- **The piece texture is drawn at 2x, anchored at the rect's top-left,
  clipped to the rect:** `tex_px = (atlas_px - rect_origin) / 2`. Checked
  visually on all seven pieces of a SylvariFemale heavy set (UV points land on
  the painted shapes) and numerically (100% of UVs on painted texels for
  aquatic helm, boots, gloves, helm and shoulders).
- Meshes whose UVs fall outside the piece's rect (the `Skin` material,
  rects 10/13) sample the character's body-skin texture, not the armor.

## 4. Dye color math (verified)

`/v2/colors` gives each dye `base_rgb` and, per material
(cloth/leather/metal/fur), `brightness, contrast, hue, saturation, lightness`
and a precomputed `rgb`. The transform (Cliff's reference,
<http://jsfiddle.net/cliff/jQ8ga/>) builds a 4x4 matrix:

1. If brightness != 0 or contrast != 1:
   `b = brightness/128`, `t = 128*(2b + 1 - contrast)`,
   `M = [[c,0,0,t],[0,c,0,t],[0,0,c,t],[0,0,0,1]] * M`.
2. If hue != 0 or saturation != 1 or lightness != 1 (hue in degrees -> radians):
   `M = RGBtoHSL * M`, then
   `M = [[cos*s, sin*s,0,0],[-sin*s, cos*s,0,0],[0,0,l,0],[0,0,0,1]] * M`,
   then `M = HSLtoRGB * M`, with
   `RGBtoHSL = [[.707107,0,-.707107,0],[-.408248,.816497,-.408248,0],[.577350,.577350,.577350,0],[0,0,0,1]]`
   and `HSLtoRGB = [[.707107,-.408248,.577350,0],[0,.816497,.577350,0],[-.707107,-.408248,.577350,0],[0,0,0,1]]`.
3. Apply to the **BGR** vector `[b, g, r, 1]`, swap back to RGB, truncate to
   int and clamp 0..255.

Checked against all 52 color/material pairs in
`tests/data/character/colors.json`: every channel matches the API's `rgb`
exactly.

Baking a dyed piece: for each texel of `textureBase`, for each dye channel i
with mask weight `m_i`, `out = lerp(out, M_i(texel), m_i)`, where `M_i` is
the matrix of the dye in slot i for the slot's material. (The texture is
authored around `base_rgb`'s hue, which is what the matrix is built to shift.)
Not yet verified against an in-game screenshot.
