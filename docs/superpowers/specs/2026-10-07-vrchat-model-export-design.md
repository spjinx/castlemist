# Export for VRChat (Model) — Design

Date: 2026-10-07
Status: designed with the user section by section; awaiting spec review.
Builds on: the glTF model exporter (`src/exportgltf/`), the Character Ripper's
VRChat path (`src/ripper/vrchat.cpp`, `tools/blender/castlemist_vrchat.py`),
and the game-shader material extraction (`src/extract/game_shader.cpp`).
Target shader: Poiyomi Toon 10 (10.0.24 at time of writing). Importer-side
facts (property names, presets, locking): `docs/research/poiyomi-10-importer.md`.

## Goal

Any model castlemist can preview — a weapon, back item, armor piece, prop —
exported as a folder that drops into a VRChat Unity project with every GW2
material map worked out, every material's real blend mode named, and every
animation clip intact. The folder carries a `materials.json` that a later
sub-project (a Unity importer shipped with castlemist) turns into Poiyomi
materials and a prefab with no hand work.

This is sub-projects 1 and 2 of five agreed with the user:

1. **Material maps for any model** (this spec)
2. **Blend modes and effect data** (this spec)
3. Unity importer that ships with castlemist (next)
4. Particles built by the importer (after 3)
5. Map props as LOD prefabs (after 3)

## Out of scope

- The Unity importer and anything that runs inside Unity (sub-project 3).
- Building particle systems (sub-project 4); `_particles.json` is only copied.
- Map props, LODs, `LODGroup` prefabs (sub-project 5).
- Material values that live only in the game shader's own uniforms (matched
  by the uncracked token hash — see `docs/research/gw2-shaders-dxbc.md`,
  "MATERIAL-CONSTANT BINDING"). Never guessed; left `null`.
- Retargeting a weapon's own clips onto the Character Ripper's merged
  skeleton. The Blender fix below helps characters too, but the ripper's
  skeleton merge dropping piece clips is its own follow-up.
- Porting GW2's DXBC shaders to Unity (deferred, engine-bound, hundreds of
  permutations — discussed and rejected in favour of Poiyomi).

## User-facing behaviour

- **File → Export for VRChat (Model)…** — enabled whenever a model is the
  current entry (list click, content browser asset, or texture-panel jump).
  Asks for a destination folder name, then writes the folder below on a
  background thread like the glTF export, reporting the result in the
  status bar and a message box (including "Blender not found: .glb and
  textures only" when that applies).
- **CLI:** `gw2dat_cli model --dat <dat> --file-id <id> --template <json>
  --vrchat <dir>` — the same export, printing the report as JSON.

### Output folder

```
<Name>/
  <Name>.glb             meshes, skeleton, every clip, PBR maps (Blender)
  <Name>.fbx             Blender-converted, every clip kept (Unity)
  <Name>.blend           for hand edits
  <Name>_particles.json  the existing particle sidecar, unchanged, when present
  materials.json         one entry per material (schema below)
  Textures/
    <Mat> - BaseColor.png
    <Mat> - Normal.png
    <Mat> - Packed.png          Poiyomi packed map: R metal, G smooth, B reflection mask, A specular mask
    <Mat> - EmissionMap.png     glow colour texture (Poiyomi Emission Map)
    <Mat> - EmissionMask.png    glowmask (Poiyomi Emission Mask)
    <Mat> - Emission.png        mask × colour, baked (only used by the .glb)
    <Mat> - Distortion.png      glowperturb, when present
    <Mat> - <role>.png          any other layer (mask, decal, detail, ...) raw
```

`<Mat>` is the exported material name (artist name, else `Mat_<amatFileId>`,
as `material_export.cpp` already names them), sanitised for file names. A
texture shared by several materials is written once per material under that
material's name — the Unity importer must not have to resolve sharing.

## Components

Source of truth for every texture convention below:
`docs/research/gw2-material-channels.md` (shader disassembly over 24 models
and 545 materials). Where this spec and that note disagree, the note wins
and the spec is corrected.

### 1. Blend decode — `castlemist/exportgltf/blend_mode.h` (new, pure)

```
enum class BlendPreset { Opaque, Cutout, Fade, TransClipping, Transparent,
                         Additive, SoftAdditive, Multiplicative, Multiplicative2x, Custom };
struct BlendInfo {
    BlendPreset preset;
    bool exact;                               // false: nearest Poiyomi preset, use the raw factors
    int srcRgb, dstRgb, srcA, dstA, opRgb;    // raw bgfx factors and equation
    bool alphaTest;                           // from the shader profile (section 2)
};
BlendInfo decode_blend(uint64_t bgfxState, bool hasRenderState, bool alphaTest);
const char* poiyomi_preset_name(BlendPreset);  // "Opaque", "Soft Additive", ...
```

bgfx factors: 1 Zero, 2 One, 3 SrcColor, 4 InvSrcColor, 5 SrcAlpha,
6 InvSrcAlpha, 7 DstAlpha, 8 InvDstAlpha, 9 DstColor, 10 InvDstColor. RGB
factors at bits 12/16 (as `make_blend_state_from_bgfx` reads them), and the
**equation at bits 28+** (Add, Sub, RevSub, Min, Max), which must be read:

| GW2 blend (count among the survey's 545 materials) | preset | exact |
|---|---|---|
| none (460) | Opaque, or **Cutout** when `alphaTest` | yes |
| SrcAlpha / InvSrcAlpha (208) | **Fade**, or **TransClipping** when `alphaTest` | yes |
| One / InvSrcColor (118, the commonest effect/glass word) | **Soft Additive** | no |
| One / InvSrcAlpha (64) | **Transparent** (premultiplied) | yes |
| SrcAlpha / One (23), One / One (2) | **Additive** | yes |
| DstColor / SrcColor (5) | **2x Multiplicative** | yes |
| DstColor / Zero (3) | **Multiplicative** | yes |
| InvDstColor / One | **Soft Additive** | yes |
| One / One with RevSub (3, AMAT 904523: darkens) | **Custom**, nearest Multiplicative | no |
| anything else | **Custom**, nearest by destination (One → Additive, InvSrcAlpha → Fade, Zero → Opaque) | no |

`exact: false` puts a warning in `materials.json`; the raw factors and
equation are always recorded so the importer can set Poiyomi's Advanced
Blending fields exactly.

`alphaTest` is **not** `GameMaterial.prepassCutout` (true for stipple and
distance-fade discards too) and **not** `ModelTextureCPU::hasCutout` (a
dark shine map reads as holes). It comes from the shader profile; an AMAT
with no profile is treated as not clipping; its single "default profile" warning
says the clip is unknown.
Materials with no game shader (no `hasRenderState`) fall back to today's
`isEffect`: Additive when set, else Opaque.

The glTF export uses the same decode: Opaque/Custom-opaque → `OPAQUE`,
Cutout/TransClipping → `MASK` with `alphaCutoff` 0.25, the rest → `BLEND`.

**Materials with no triangles** (most blended weapon materials: they draw
the model's particle effects) are not mesh materials. They stay in
`materials.json` with `"usedByMeshes": false`, so the particle importer can
use them, but no mesh references them.

### 2. Shader profiles — `castlemist/exportgltf/shader_profiles.h` (new)

GW2's texture conventions vary by shader, and so by expansion. A profile
says how one shader family's layers read. Profiles are data, keyed by AMAT
fileId (`ModelMaterialCPU::materialFile`):

```
enum class AlphaUse { HolesAndShine, Shine, ReflectionOnly, Intensity, Opacity, InteriorWeight, Unused };
enum class Channel : int8_t { None = -1, R, G, B, A };
struct ShaderProfile {
    std::string name;                 // recorded in materials.json
    bool supported;                   // false: maps exported raw + a warning, no interpretation
    bool clips;                       // alphaTest
    AlphaUse diffuseAlpha;
    Channel maskMetal, maskGloss, maskSheen, maskGlow, maskGlowGate;   // in the `mask` layer
    bool specLayer;                   // a `specular` layer: RGB = spec colour, A = gloss or exponent/128
    bool modMultiply2x;               // `mod` layer multiplies albedo x2 (128 = neutral)
    bool premultiplyRgbByAlpha;       // blended effects: colour x alpha
};
const ShaderProfile& profile_for(const ModelMaterialCPU&);
```

Profiles shipped (AMATs and channel details as in the research note, sec. 4):

| profile | AMATs | in this sub-project |
|---|---|---|
| weapon-glow | 561567, 511755, 510615 | supported |
| weapon-glow-legendary | 2083141, 2140066 (Pharus, Exordium) | lit part supported; animated glow layers exported raw + warning |
| weapon-spec (SotO/Janthir) | 2348484 | supported |
| legacy-spec | 13822, 13831 (no clip); 14149, 14213, 14165, 1891783 (clip) | supported; mask = glow gate (R) / glow-perturb gate (G) |
| prop-lit | 13843, 13856, 13864, 14003, 31327, 32657, 34181, 44707, 44708, 72583, 27352, 19911, 47468, 47469 | supported |
| prop-lit-noclip | 15999, 54592, 57634, 57715, 27353 | supported |
| subsurface-decal | 54632 | supported (decal kept as its own layer; decal alpha 1 = show diffuse) |
| armor-mask | 2449347, 2234037, 2777930 | supported: R metal, G gloss, B sheen, A glow |
| armor-mask-noglowA | 1171332, 1699091, 1674959 | supported: as armor-mask, A unused |
| jade-interior | 2472137 | diffuse alpha = interior weight; exported raw + warning |
| legacy-untagged | 185120, 187842 (+ the untagged trait group) | supported: tex2.R = opacity |
| fx-soft-additive, fx-premultiplied | as the note | supported (intensity, premultiplied) |
| armor-silk, fx-fire, fx-distort | as the note | **not supported**: raw maps + warning |

Choice order: AMAT fileId → a trait the note proves reliable (the
legacy-untagged trait: all texture tokens 0, matId 0, flags 0,
SrcA/InvSrcA) → **default**. The default is deliberately conservative: no
holes, shine `saturate(2a-1)`, mask/specular layers exported raw and not
interpreted, plus a "default profile" warning (which also says the clip is
unknown). Ruling R8: a **uniform** diffuse alpha (every texel equal, or a
placeholder) carries no shine data, so on the default profile it is read as
unused — Packed G = `specstr` (else 128, source "default"), A = 255 (source
"default") — with a warning; profiled shaders keep alpha 255 = full shine. On a
blended preset (not Opaque/Cutout) the default profile keeps the diffuse alpha
as opacity in BaseColor (no premultiply) and reads no shine from it, with a
warning. A new AMAT gets a profile by
adding a table row, never by special-casing the map code.

### 3. Map building — `castlemist/exportgltf/vrchat_maps.h` (new, pure)

```
struct MaterialMaps {
    ModelTextureCPU baseColor, normal, packed, emissionMap, emissionMask, emissionBaked, distortion;
    std::vector<std::pair<std::string, ModelTextureCPU>> extras;  // role -> raw layer
    uint8_t baseUv, normalUv, emissionMapUv, emissionMaskUv, distortionUv;
    // ... values recorded alongside (see materials.json)
};
MaterialMaps build_material_maps(const ModelPreview&, const ModelMaterialCPU&,
                                 const BlendInfo&, const ShaderProfile&);
```

The diffuse alpha, wherever a profile reads it as holes and shine (verified
by disassembly: every clipping shader discards on `saturate(2a) < 0.5`):

| alpha | meaning |
|---|---|
| 0–63 | hole (only when the profile clips) |
| 64–127 | opaque, no shine |
| 128–255 | shine 0 → 1 = `saturate(2a - 1)` |

Rules:

| Output | Built from |
|---|---|
| BaseColor RGB | `diffuseTex`, × alpha where `premultiplyRgbByAlpha`. A `mod` layer is not baked in; it is kept as a Detail extra. |
| BaseColor A | clips: < 64 → 0, else 255. `Opacity`: the opacity source (e.g. legacy-untagged tex2.R). `Intensity`: the alpha as-is. Otherwise 255. |
| Packed R (metal) | `maskMetal`; else `mtlness`; else `conduct` (weapon-glow, metalness-like) scaled to 0–1; else 0. |
| Packed G (smooth) | `maskGloss`; else the `specular` layer's A where it is gloss (weapon-spec); else the shine `saturate(2a-1)`; else `specstr`; else 0.5. |
| Packed B (reflection mask) | the shine where `ReflectionOnly` (legacy-spec); else 255 when `envcr`/`envcp`/`envstr` exist; else 0. |
| Packed A (specular mask) | the shine; else 255. |
| Normal | `normalTex` **RG only**; B rebuilt as `sqrt(1 - x² - y²)` (GW2 ignores B and A). Green orientation is an open question (see "Open questions"); the flip is one switch in this function. |
| EmissionMap / EmissionMask | per profile: `glow` / `glowmask` on their own UV sets (weapon-glow; weapon-spec has glow on UV2, glowmask on UV0); `maskGlow` → EmissionMask with the base colour as map (armor-mask); `maskGlowGate` → EmissionMask (legacy-spec). Additive-style presets with no glow layer → base colour as EmissionMap. Some models use one file for both glow and glowmask; that is fine. |
| Emission (baked) | EmissionMask × EmissionMap's average colour — the .glb's `emissiveTexture`, as `material_export.cpp` does today. |
| Distortion | `glowperturb` / `perturb` layer: an **RG UV-offset map** (not a normal), recorded with its strength constant (`gloptrb`). |
| Decal | see "Decal" below. |
| extras | every other layer raw, by role, with a `"use"` hint where the profile knows it (`mod` → `"detail-multiply2x"`, `decal` → `"lerp-by-decal-alpha"` on profiles without a `decalMode`, `height` → `"parallax"`, `specular` → `"specular-color"`). |
| `specularTint` / `reflectionTint` | `speccp` / `envcr` (or `envcp`) RGB. |

4×4 placeholder textures (e.g. 13368 white, 529633 flat normal) stand for
constants: the map is skipped and the value recorded.

**Decal.** GW2 decal shaders blend a `decal` layer (usually on UV1) over the
diffuse in one of two opposite ways, set by the profile's `decalMode`
(research note sections 4 and 8): `DecalOverDiffuse` (the map-prop family,
`lerp(diffuse, decal, saturate(2·decal.a))`, × a `decalMaskRole` channel on
19910/525886 `decalmask.R`, 69887 `mask.R`, 60530 `blend.G`) and
`DiffuseOverDecal` (54632, 57131: `lerp(decal, diffuse, decal.a)`). The Decal
map is the decal on **its own UV**: RGB = decal.rgb, A = the decal's coverage
of the diffuse — `saturate(2a)` [× the mask channel] or `1 − a` — and its
`source` names the formula (`"decal saturate(2a) x decalmask.R"`). A mask on
another UV than the decal (19910: the PS samples it at TEXCOORD2) cannot go
into that alpha: it is written as its own greyscale **DecalMask** map
(`maps.decalMask {file, uv, fileId, channel, source}`) on its own UV, the
decal's source says `(x decalMask on UV2)`, and a warning says the coverage
must be multiplied by it. A 4×4 placeholder mask is a constant and is always
multiplied in. Not mapped, one warning each: the decal parallax (`pardist`,
54632/57131) and 69887's specular × `mask.B`. The decal is
not baked into BaseColor (another UV) and its shine (`saturate(2a−1)`, prop
family) is not folded into Packed: one warning, "decal shine not mapped". The
decal is then no longer an extra; a missing or failed decal layer only warns.
`decalGlow` adds emission from the decal when no glow/glowmask/mask-glow source
owns it (else the warning "decal glow not mapped: emission slot in use"):
`BelowHalf` (57131) EmissionMap = decal.rgb, EmissionMask = `1 − a`, colour
`glowcol × 2` (stored at peak 1, the peak as `emission.strength`; white with a
warning when `glowcol` is absent); `AboveHalf` (57806) EmissionMask =
`saturate(2a − 1)`, colour white. Both sit on the decal's UV; the baked
emission is map × mask × colour. The .glb keeps
the decal as it was (the occlusion slot); materials.json gets
`maps.decal {file, uv, fileId, source, mode: "decal-over-diffuse" |
"diffuse-over-decal"}`, `null` when absent.

Each rule records which source it used, so `materials.json` can say
"smoothness from diffuse alpha" or "metal from conduct".

### 4. materials.json — `castlemist/exportgltf/vrchat_export.h` (new)

```
{
  "castlemist": "<version>", "model": <fileId>, "poiyomi": "10",
  "materials": [ {
    "name": "Mat_561567", "amat": 561567, "profile": "default",
    "renderPreset": "Opaque", "blend": {"srcRgb":..,"dstRgb":..,"srcA":..,"dstA":..,"op":..},
    "alphaCutoff": 0.25, "alphaCutoffIsDefault": true,
    "renderQueueOffset": <sortLayer>, "cull": "Back" | "Off",
    "maps": {
      "baseColor":    {"file": "Textures/... - BaseColor.png", "uv": 0, "fileId": 1766516},
      "normal":       {...}, "packed": {..., "sources": {"metal":"none","smooth":"diffuseAlpha", ...}},
      "emissionMap":  {..., "uv": 0, "panning": null},
      "emissionMask": {..., "uv": 2},
      "distortion":   {...},
      "decal":        {..., "uv": 1, "source": "decal saturate(2a)", "mode": "decal-over-diffuse"} | null,
      "extras": [{"role": "detail", ...}]
    },
    "emission": {"color": [1, 0.27, 0.05], "strength": 1.0},
    "specularTint": [0.78, 0.91, 0.62] | null, "reflectionTint": [..] | null,
    "gw2": {"specstr": .., "glofade": 0.7, "gloptrb": 0.4, ...},   // every named constant, raw
    "warnings": ["emissionMask uses UV2: check Poiyomi's UV choice"]
  } ],
  "animations": ["zeropose", "StowedA", "DrawingA", "DrawnA", "StowingA"],
  "particles": "<Name>_particles.json" | null
}
```

- `panning`: GW2's raw `intscru`/`intscrv` values when the material has them
  (`{"u":..,"v":..,"unit":"gw2-raw"}`), else `null`. Not converted: GW2's
  unit is unknown, and Poiyomi's pan is UV per `_Time.x` (seconds / 20), so
  the conversion is the importer's job once the unit is confirmed.
- `cull`: `"Off"` for two-sided materials (`isEffect` as the glTF export
  does today, or a profile override), else `"Back"`.
- `gw2`: every `namedConstants` entry, untouched.
- `warnings`: anything the importer or user should check — a map on a UV
  set above 3 (Poiyomi textures can only use UV0–UV3), default cutoff, Custom blend, default profile used
  for a material with unexplained layers.

### 5. Export orchestration — `export_vrchat_model()`

In `castlemist::ripper` (beside `export_vrchat`, which already owns
`find_blender`/`run_blender`):

```
struct VrchatModelReport { bool ok; std::string error, folder, glb, fbx, blender;
                           size_t materials, clips; std::vector<std::string> warnings; };
VrchatModelReport export_vrchat_model(const ModelPreview&, const std::string& folder,
                                      const std::string& name, const VrchatOptions& = {});
```

Steps: build each material's `BlendInfo`/profile/maps → write PNGs →
`export_model_gltf` (the glTF export gains the decoded alpha mode and the
packed map as `metallicRoughnessTexture` converted to glTF's G-rough/B-metal)
→ copy the particle sidecar → write `materials.json` → run Blender when
found.

### 6. Blender script — `tools/blender/castlemist_vrchat.py`

Changes, shared by characters and models:

- **Animations kept:** `export_scene.fbx(bake_anim=True,
  bake_anim_use_all_actions=True, bake_anim_use_nla_strips=False,
  bake_anim_force_startend_keying=True, bake_anim_simplify_factor=0)` so
  every imported action becomes its own Unity clip, named as in GW2.
- **No `transform_apply` on an animated armature.** The root's Z-up→Y-up
  rotation and scale move into the FBX export (`axis_forward`/`axis_up`,
  `apply_scale_options`, `bake_space_transform`) instead of being applied to
  the data, which would leave every action's curves in the old space.
- **Joining leaves the armature's actions alone**; meshes still join per piece.
- **Models without an armature** (props) export as plain meshes instead of
  exiting with "no armature".
- A `--mode model|character` argument picks the piece-joining and eye-bone
  steps (character only).

## Research (done)

The channel survey ran before this spec was approved:
`docs/research/gw2-material-channels.md`. Its section 4 is the profile
table above; its section 6 is the per-model evidence. Adding profiles later
uses the same method: disassemble the AMAT's colour pixel shader, name its
constants from the bgfx uniform table, and take alpha statistics over only
the texels the material's triangles use.

## Open questions

- **Normal map green orientation.** The pixel shaders can't show it; it
  needs the vertex shader's tangent frame or a visual check (a known bevel
  lit from above). The first plan task settles it.
- **GW2 scroll units** (`intscru`/`intscrv`): recorded raw (section 4).
- **Silk shaders** (armor-silk) and the procedural fire and distortion
  effects are not mapped: raw maps + warning.

## Error handling

- A missing or undecodable layer drops only that map; the material still
  exports, with a warning in `materials.json`.
- No Blender: `.glb`, textures and `materials.json` are still written; the
  report and message box say the `.fbx` was skipped and why.
- Blender failure: its log is saved as `<Name> blender.log` in the folder and
  the report carries the error; the other files stay.
- Model with no meshes: refused before anything is written (as the glTF
  export does).

## Testing

- **Unit (`tests/test_exportgltf_*.cpp`, existing framework):**
  `decode_blend` for every table row plus Custom and the no-render-state
  fallback; `build_material_maps` on synthetic materials — cutout vs shine
  alpha, packed channel sources, normal G flip, glow/glowmask/mask-alpha
  emission variants; `materials.json` fields for a synthetic model.
- **CLI end to end:** `gw2dat_cli model … --vrchat` on the Forged Dagger
  (file 1766522) and the research sample: folder complete, JSON valid,
  every map present where expected.
- **Blender (headless, scripted):** re-import the `.fbx`; the dagger has its
  5 clips (they are constant bind poses in GW2, so only their presence is
  checked); a model whose clips really move — picked with `gw2dat_cli skel
  --clip N` (`keyframedCurves`/`totalKnots` > 1 per curve) — re-imports with
  the same clips and keyframes that move; meshes and materials intact; a
  prop with no armature exports.
- **Visual:** the dagger's maps in the channel viewer match their
  `materials.json` sources.
