# World Materials and Motion — Design (sub-project 3 of the World Engine)

Date: 2026-10-14
Status: written as a record (the user asked for sub-projects 2–4 to be built in
order without sign-off gates; see the World Data Core spec's roadmap)

## Goal

Give the map window (sub-project 2) the map's own look and movement: terrain
painted with its blend pages and ground textures, props with their textures,
water with its env-preset colours, UV scroll on the materials that store it,
and animated props playing the clip their placement names. It is a material
pass inside `WorldView` that replaces the AO shading and leaves culling,
instancing, LOD and streaming as sub-project 2 built them. Every rule comes
from `docs/research/gw2-world-materials.md`; what that note leaves UNPROVEN is
a labelled setting or is left out, and is listed in the scene's warnings.

## Why

Sub-project 2 draws geometry only. The old `MapScene` path (`map_fly`,
`render_scene`) still holds the only textured map modes, and it pays one
draw and one cbuffer update per instance per submesh. Sub-project 4 (export)
needs the same material decisions offline: a baked terrain albedo per cell and
Poiyomi-ready prop materials. Making the decisions once, in code both use, is
the point of this sub-project.

## Decisions

- **Reconstruction first.** castlemist's own HLSL, written from the game's
  terrain DXBC as transcribed in the note (§1.2), not from T3D's shader (whose
  blend and `modx` rule differ, §1.3). The terrain blend also exists as a pure
  CPU function in `world`, so tests can check the GPU against it and the
  exporter can bake the same albedo.
- **Game DXBC only where castlemist already proves it works.** That is MODL
  props (the main window's `render_scene_game`). It is an optional last stage
  (E), not part of "done". Terrain and water DXBC are not built: the terrain
  TEXCOORD/TexTransform values and the water's scene-colour, depth and
  reflection inputs are unknown or absent (note §2, §6). Picking a prop still
  opens it in the main window's preview, where Game 1:1 already exists.
- **No invented conventions; labelled settings instead.** Where the data says
  nothing, the value is a named setting with its source in the comment,
  adjustable, and listed in `warnings`. This is the `WalkParams` rule of
  sub-project 2:
  - `groundRepeatsPerChunk = 8` (T3D's guess; note §1.6). `uvScale` stays 0
    in `WorldScene`.
  - `uvScrollUnit = per second` and `offset = frac(scroll · t)` on the UV set
    `uvIndex` (castlemist's reading of `texTransforms`; note §4).
  - All animated props start at t = 0 and loop (phase and looping are UNPROVEN;
    note §5).
  - The water plane over the whole map, depth-tested (as sub-project 2; note
    §6 and `gw2-world-frame.md` §6.2).
- **Data stays as stored, in `world`.** New fields are copied from the dat by
  field name and kept raw. Their interpretation lives in one place in
  `render` (and later `exportgltf`), never in `world`.
- **The lo-res terrain shader is the reference.** It is the one fully
  transcribed (note §1.2). The hi-res extras (env reflection, roughness,
  local lights) are out of scope; which one the game draws at what distance
  is UNPROVEN.
- **Clips resolve exactly or not at all.** A placement whose `animSequence`
  names no clip of its model (4 of 41 pairs on Spirit Vale; note §5) stays in
  its bind pose and is counted in a warning. No fallback is invented.
- **Texture memory is bounded.** One texture cache per `WorldView`, keyed by
  fileId, holds the reduced member of each texture, capped at
  `maxTextureSize = 512` and mip-mapped. It never flips the process-wide
  `set_texture_full_res`.

## Architecture

```
native   Extractor: animSequence, per-placement color; env water presets;
         MODL texTransforms (by field name)
extract  ModelMaterialCPU::texTransforms, ModelPreview::animClipTokens,
         ModelLoadOptions{wantGame, decodeTextures}, decode_texture_capped
world    terrain_blend.h   (pure CPU blend, page/ground UV rules)
         anim.h            resolve_prop_clip(...)
         WorldScene additions (below)
render   world_materials.{h,cpp}  (internal to WorldView::Impl)
           TextureCache, TerrainMaterialPass, PropMaterialPass,
           SkinnedPropPass, WaterPass
         world_view.h      set_shading(WorldShading), set_motion(bool),
                           set_time(seconds)
ui       map_window.cpp    toolbar: Materials / Motion; status: textures, MB
```

Layer rules are unchanged: `world` depends on nothing above `extract`;
`render` draws a `WorldScene`; `exportgltf` (sub-project 4) will call
`world/terrain_blend.h` and `profile_for`, not `render`.

### WorldScene additions (`world_scene.h`)

| type | added | from |
|---|---|---|
| `TerrainMaterial` | `texIndexFileIds` (the ten `texIndexArray` fileIds by position; 0 for a page entry) and `hiResMaterialFileId` | note §1.1: needed for a later 1:1 path, cheap now |
| `TerrainMaterial` | `hasUvData`, `uvData{translation[2], xScaleRange[2], yScaleRange[2], scaleSpeed[2], rotation}` as stored | note §1.6 (today only counted in a warning) |
| `Terrain` | `swapDistance` as stored | note §1.6 |
| `PropInstance` | `animSequence` (token64, 0 when not `propAnimArray`), `color[4]` as stored | note §3, §5 |
| `Environment` | `light.presetIndex` (which `lighting` preset the auto-select took); `water`: the `PackMapEnvDataWater` presets of the `dataGlobal` block, as stored (colours, bump, pattern, fresnel, depth attenuation, foam, material and texture fileIds) | note §6 |

`Motion::animatedProps` stays. Clip resolution needs the model file, so it
happens after a model loads (render), through the pure
`world::resolve_prop_clip(clipTokens, animSequence) -> int` (index or −1).

### world/terrain_blend.h

```cpp
struct BlendTexel { float rgba[4]; };          // linear 0..1, as decoded
struct TerrainBlendInput {
    BlendTexel picker, modx;                   // page samples at the chunk's page UV
    BlendTexel ground[4];                      // color, colorb, colorc, colord at the ground UV
};
void terrain_blend(const TerrainBlendInput&, float albedoOut[3], float weightsOut[4]);
void page_uv(const TerrainMaterial&, float s, float t, float uv[2]);    // s,t in [0,1] over the chunk
void ground_uv(float s, float t, float repeats, float uv[2]);
```

`terrain_blend` is note §1.2 to the letter: picker R/G/B/A weight `colord`/
`colorc`/`colorb`/`color`, times ground alpha + 0.01, normalised; all-zero
picker means `color` alone; luminance (0.30, 0.59, 0.11); saturation `2·modx.g`;
brightness `2·modx.r`. The HLSL is a line-for-line copy of it.

### render/world_materials (inside WorldView)

- **TextureCache.** `fileId -> {SRV, bytes}`. Decodes on the model-load worker
  (`decode_texture_capped`: reduced member, longest side ≤
  `maxTextureSize`, CPU box-filtered mips down to 4×4, RGBA8), uploads on the
  UI thread in the same batches as `add_models`. Solid pages become 1×1
  textures from `pickerSolid` in stored byte order (UNPROVEN, warned).
  Reports count and MB to the stats line.
- **TerrainMaterialPass.** Terrain vertices grow from 16 to 20 bytes: the
  sample's chunk coordinate `(s, t)` as two UNORM16 (page and ground UVs are
  computed in the VS from `pickerOffset`, `pickerScale` and
  `groundRepeatsPerChunk`). Per chunk: four colour and four normal SRVs and
  the two page SRVs, bound per draw; draws are already one per chunk. Normal
  maps use the grid's tangent frame (tangent = +X east, bitangent = the
  direction of increasing `t`, normal from `chunk_normals`). The game's
  terrain tangent vectors are not in the data, so the normal-map detail is
  a setting (`terrainNormalMaps`, default on) whose sign convention is
  UNPROVEN and named in its comment. Lighting is forward: `L.rgb` from the
  hemisphere + sun of `environment.light`, `L.a` from the sun specular, so
  §1.2's `alb·L.rgb + nrm.a·0.855·sun·L.a + rim·fres`, then fog.
- **PropMaterialPass.** The flattener records, per LOD range, the sub-range
  and material index of each submesh it appends (it already appends in mesh
  order). Prop vertices become `MatVtx`, 32 bytes: position f32×3, normal+AO
  UNORM8×4 (as `MapVtx`), uv0 f32×2, uv1 f32×2 (enough for every
  `diffuseUv` seen; a material whose diffuse is on UV2+ draws untextured and
  is counted). Per bucket `(model, lod)`: one instance upload (unchanged),
  then one `DrawIndexedInstanced` per submesh range with that material's
  diffuse SRV and a small per-material cbuffer (alpha mode, UV set, scroll).
  The alpha mode comes from `exportgltf::profile_for` (clip below alpha
  0.25 / shine / intensity) and the blend word (`exportgltf::decode_blend`, `blend_mode.h:33`): opaque and
  clip draw in pass 1; blended materials in pass 2 with depth write off,
  buckets ordered back to front by their nearest instance (no per-instance
  sort; an approximation, documented). Shading: albedo × AO × the same
  hemisphere + sun the AO pass used, so Materials off/on compare like for
  like. Diffuse-only: prop normal and specular maps are out of scope.
- **UV scroll.** `ModelMaterialCPU::texTransforms` (uvIndex, scroll, and the
  other fields kept as stored). The per-material cbuffer adds
  `frac(scroll · t)` to the UV set the diffuse is sampled on when its
  `uvIndex` matches; only entries with `columns = rows = count = 1` and zero
  `scrollFreq/scale/scaleFreq/rotate` animate (all 33 seen in the census;
  others are counted, not drawn moving). Terrain `uvData` is recorded, not
  animated (which ground UV it moves is per-AMAT, note §1.5).
- **SkinnedPropPass.** Only for models with a resolved clip. Those models also
  keep `SkinVtx` (bone indices UINT8×4, weights UNORM8×4) next to `MatVtx`.
  One pose per `(model, clip)` per frame (every instance starts at t = 0, so
  instances share it), computed on the CPU with `granny::composePose` /
  `skinMatrix` as `gw2bgfx_view` does, into a structured-buffer palette. Rigid
  single-bone meshes are weight-1 vertices. Instances of the same
  `(model, clip)` are still drawn instanced. Collision does not move with
  them (out of scope).
- **WaterPass.** The sub-project 2 plane, drawn with a reconstruction from
  `environment.water[presetIndex]`, where `presetIndex` is the index of the
  lighting preset `load_world` chose (`MapEnvLight` gains `presetIndex`; that
  water preset k pairs with lighting preset k is UNPROVEN, note §6): colour lerps `surfaceShallowColor -> surfaceDeepColor`
  by depth below the plane over `depthAttenuation` (depth from a map-wide
  R32F terrain height texture built from `WorldScene::terrain`); two scrolled
  bump normals from the preset's bump texture with `bumpTile0/1`,
  `bumpSpeed0/1`, `bumpAngle0/1`; fresnel `surfaceFresnel`; sun specular.
  The units of tile, speed and angle are UNPROVEN (note §6): the mapping is
  one function with that comment. No preset: the sub-project 2 flat colour.
- **Fallback.** `WorldShading::Geometry` keeps the sub-project 2 AO path
  exactly. A model or chunk whose textures fail draws in the AO look and is
  counted.

### extract / native

- `Extractor`: `texTransforms[]` of each MODL material (by field name, every
  version that has it); `animSequence` and `color` on `MapProp`; the env
  `water` presets of `dataGlobal`. Unknown versions leave the field empty and
  name it in a warning.
- `ModelPreview::animClipTokens`, parallel to `animClips` (today
  `animationTokens` is not parallel; note §5).
- `ModelLoadOptions{bool wantGame = true; bool decodeTextures = true;}` on
  `load_model_by_fileid`. The map window passes `{false, false}` and decodes
  through the texture cache.

## Front end

Map window toolbar adds **Materials** (on) and **Motion** (on). The status line
adds textures loaded / MB and animated instances playing / unresolved. The
warnings panel lists the new UNPROVEN settings. Headless:
`GW2_MAPWIN_SHADING=geometry|materials` (default materials) and
`GW2_MAPWIN_TIME=<seconds>` (a fixed time, so screenshots are repeatable).

## Error handling

As sub-project 2: a texture that fails to decode is skipped and counted, its
draw falls back to the AO look; a model whose clip fails to decode stays in
bind pose (counted); a map with no water preset gets the flat water. Nothing
throws for bad map or model data. Dat I/O failure is an error, as in
`load_world`.

## Testing

- **Pure (`cm_test_world`):** `terrain_blend` on hand-made texels (one-hot
  pickers select each ground texture; all-zero picker gives `color`; equal
  weights with alphas 0 and 1 give 0.01 : 1.01; `modx (0.5, 0.5)` is
  neutral; `modx.g = 0` gives grey at the luminance); `page_uv` on a chunk at
  page offset (0.25, 0.75) covers exactly that sub-rect; `resolve_prop_clip`
  exact hit, miss → −1, token 0 → −1.
- **extract/native:** `texTransforms` read from a hand-built packfile;
  `animClipTokens` stays parallel when one clip fails to decode.
- **render (`cm_test_render`):** the HLSL blend matches `terrain_blend`: a
  single chunk rendered orthographically top-down with lighting off, read
  back, compared with the CPU bake of the same chunk within 2/255 per channel;
  the UV-scroll offset function; the flattener's per-submesh ranges cover each
  LOD range exactly once.
- **dat (skip without `GW2_TEST_DAT`):**
  - every terrain chunk of the three test maps decodes its textures and pages
    (or has a solid page);
  - model 1126949 has two `texTransforms` with `scroll (0, 0.2)` on `uvIndex`
    1 and 0;
  - Spirit Vale: the number of `propAnimArray` placements whose clip resolves
    equals the count the plan's first task measures (the note counts 37 of 41
    distinct model/sequence pairs), and the unresolved ones are warned;
  - Queensdale's `environment.water` holds the presets the note lists (AMATs
    187551 and 188555, first preset's colours and tiling).
- **Headless:** `GW2_MAPWIN=192711 GW2_MAPWIN_TIME=0` with materials writes a
  screenshot whose colour variance is well above the geometry screenshot's
  (textures are on), and the stats line shows textures loaded > 0; the same
  with `GW2_MAPWIN_TIME=2.5` on Spirit Vale differs from t = 0 inside the
  screen rect of an animated prop.

## Done means

- Queensdale, Lion's Arch and Spirit Vale open in the map window with
  textured terrain (blend pages and ground textures per note §1.2) and
  textured props, and stay interactive; Materials off returns the
  sub-project 2 look unchanged.
- Spirit Vale's animated props play their named clips; materials with a
  `texTransforms` scroll move; Queensdale's water shows its preset colours.
- Every UNPROVEN setting is named in the warnings panel and in its code
  comment, with the note section.
- Pure, render, dat and headless tests pass.
- After this sub-project the old `MapScene` / `map_fly` path can be removed
  (sub-project 2's condition); the removal is the first task of sub-project 4
  or a follow-up, not part of this one.

## Optional stage E (not in "done")

Near-field game DXBC for props in `WorldView`: instances within a radius drawn
through their AMAT shaders, as `render_scene_game` does in the main window.
Needs, in order: `build_one_game_mat` given a device and a uniform table
instead of `g_dev` / `g_game_vals` (it touches only those two globals); a
`WorldView` light prepass (port of `src/render/lighting.cpp`) for the deferred
families; per-instance cbuffer updates for that small set only. Terrain and
water stay reconstructed.

## Out of scope (this sub-project)

Terrain and water game DXBC; the hi-res terrain look; prop normal, specular,
glow and decal maps (the export profiles keep them for sub-project 4);
`uvAnimData` and `matConstAnim` tracks; flipbooks; terrain `uvData`
animation; per-placement `constants`, `blitTextures` and `color` (recorded,
not applied); rivers and shore surfaces; collision that follows animated
props; shadows and screen-space AO; BC re-encoding of textures; zone models;
placed effects and particles (effects project); export (sub-project 4).

## Amendments before planning (2026-10-14, controller)

Written after sub-project 2 shipped, before the plan. This list wins where it
disagrees with the text above.

- **Names follow what sub-project 2 built.** The instanced draw lives in
  `src/render/world_draw.{h,cpp}` (`InstancedBatch`, `cull_into`,
  `draw_buckets`), CPU prep in `src/render/world_props.{h,cpp}`
  (`prepare_world_model`, `PreparedModel`, `ray_model_hit`), the flattener
  and AO baker in `src/render/detail/mesh_bake.{h,cpp}` (`MapVtx`, `flatten`,
  `bake_ao`), the camera maths in `src/render/world_camera.h`. Loading lives
  in `src/ui/map_window_loader.{h,cpp}` (shared parsed `Gw2Dat`, `ModelQueue`
  re-sorted around the live camera). The material passes extend these files
  rather than duplicating them; `world_materials.{h,cpp}` holds only the new
  texture cache and material state.
- **Sub-project 2's load-time follow-ups are part of this sub-project**,
  because the texture cache changes the same loader path:
  1. `ModelLoadOptions{wantGame, decodeTextures}` as specified; the map window
     loads geometry without texture decode and decodes each texture once,
     capped, through the cache.
  2. **CPU prep moves to the workers.** `prepare_world_model` (flatten, AO
     bake, and now the per-submesh ranges and `MatVtx`/`SkinVtx`) runs on the
     model-load worker; `WorldView::add_models` takes already-prepared models
     and only creates GPU buffers on the UI thread. Measured before: one
     `add_models` batch blocked the UI thread 2.86 s on Queensdale.
  3. **fileId lookup by index.** `load_model_by_fileid` and the texture
     lookups resolve fileId → MFT entry through a hash index built once per
     parsed `Gw2Dat` (today each lookup scans ~800k entries). The index is
     built before the worker pool starts and is read-only afterwards, so the
     shared-dat rule (sub-project 2, R13) still holds.
  Target, measured by the headless stats line: Queensdale models streamed in
  well under the sub-project 2 figure of 156 s, with the UI thread's largest
  `add_models` batch under 50 ms.
- **Headless hook names.** `GW2_MAPWIN_SHADING` and `GW2_MAPWIN_TIME` join the
  env table in `include/castlemist/core/env.h`, like `GW2_MAPWIN`.
- **The headless stats line gains** `textures`, `textureMB`, `animated`,
  `unresolvedClips`, and the per-batch `add_models` max/median it already
  measures.

## Amendments (2026-10-15, after implementation)

What shipped differs from the text above, and from the pre-planning
amendments, in these points. The text above is left as written; this list wins
where they disagree. "P1" and so on are the controller's rulings during the
build.

**Structure**

- **fileId lookup is a binary search, not a hash index (P1).** `fileId` and
  `baseId` lookups binary-search the MFT lists `load_dat_file` already sorts.
  Same speed class, nothing to build or keep in sync, and read-only, so the
  shared-dat rule (sub-project 2, R13) holds. The precondition is that the
  lists stay sorted; a path that left them unsorted would make lookups miss.
- **render now depends on exportgltf.** The prop material pass takes its alpha
  mode from the glTF export's rules (`profile_for`, `alpha_tested`,
  `decode_blend`). `exportgltf` depends on `extract` and `world` only, so there
  is no cycle.
- **`WorldMaterialSettings` lives in `include/castlemist/render/world_prep.h`**,
  not in `world_materials.h`, because the load workers need `maxTextureSize`
  and `world_texture_ids` and `world_prep.h` stays free of Direct3D.
- **`TerrainBlendInput::present[4]` and the absent-slot rule.** A ground slot
  whose fileId is 0 binds no texture and gets weight 0 (castlemist's rule, not
  the note's). If every weight is then 0, the note's 1/255 goes to `color`, or
  to the first present slot. A resolved chunk with no colour texture at all
  takes the AO look and counts in `chunksUntextured` (P5), instead of sampling
  a null view as black.

**Settings the spec did not list**

- `srgbColourTextures` (castlemist's choice: colour textures decode sRGB to
  linear before the blend), `terrainSpecPower` (castlemist's choice, a forward
  stand-in for the light buffer's L.a), `waterBumpTextureIndex` (UNPROVEN beyond
  AMAT 187551), `waterTileUnit` (UNPROVEN units), `waterSpecPower` (castlemist's
  choice).
- **P10:** `waterBumpAngleDegrees` (on; UNPROVEN: the stored 143.3 / 61.7 on
  Queensdale read as degrees, nothing settles the unit) and
  `waterBumpStrengthScale = 0.02` (castlemist's choice; the stored `bumpAmount`
  23.7 is far too strong as a normal gain). The `uvData` warning also says
  UNPROVEN (P4), which keeps sub-project 1's rule that every UNPROVEN line says
  so.
- **The water alpha `lerp(0.45, 1, fresnel)` is castlemist's choice**, and is
  now a line in the Map window's warnings (the flat water uses 0.45).

**Textures**

- **Header-only textures; the pixel-effect guess moved to the view.** A
  header-only model load cannot apply `looksLikeEffect`, so `WorldView` applies
  the rule to the decoded texture. The header peek (P3) reads a 64 KB prefix of
  the entry and inflates only the header, about 37x faster than the full read
  on a 300 KB synthetic. `mipCount` from a prefix is an estimate; the map path
  builds its own mips, so nothing reads it.
- **P9: a plain uncompressed 32-bit DDS fallback in `decode_texture_capped`.**
  Some entries (the water bump 186359 among them) are raw DDS, not ATEX. On
  Queensdale this took `texturesFailed` from 31 to 0.
- **Textures are not restored after a device reset.** `create()` drops the
  cache with the old device, and the decoded textures were not kept, so props
  draw untextured until the window is reopened. The status line says so.
- **The 16 MB batch cap and the terrain texture thread.** A model batch posts
  to the UI thread at 16 MB of decoded texture (`kMapWinBatchTextureBytes`) as
  well as by count, and the terrain's textures are decoded on a thread of
  their own and posted in batches. Each texture fileId is claimed by exactly one
  thread, so the terrain thread and the model workers never decode it twice.
- **GPU texture memory is 620 to 760 MB at the 512 px cap** on Queensdale and
  Lion's Arch (361 MB on Spirit Vale).

**Light and blending**

- **P6: the textured passes use the measured light calibration.** `kPropMat`
  and `kTerrainMat` use `detail/light_calib.h`, the same rig as the main
  window's Shader mode (sun 0.75 over the default sun luminance, ambient from
  the fill), not the AO look's budget, which was tuned for an albedo of 0.78
  and made textured props about 2x too dark. The map's sun and fill
  intensities are kept.
- **P7 and P8: exact blend factors per preset.** Fade and TransClipping blend
  on SrcAlpha / InvSrcAlpha; Additive is SrcAlpha / One and AdditiveOne is
  One / One; Transparent is premultiplied (One / InvSrcAlpha); Multiplicative
  is DstColor / Zero and Multiplicative2x is DstColor / SrcColor. **Custom and
  SoftAdditive are left unmapped** and drawn in the AO look, counted in
  `materialsUntextured`; no factors are guessed.
- **The dark-decal rule is castlemist's own.** Measured on AMAT 58246's
  decals: they are all-black diffuse under a soft alpha, and clipped they drew
  as solid black splats. A Clip material with no render state, whose texels
  with alpha >= 64 are all black (<= 16), blends on its alpha instead. It is not
  in the research note, and the warnings line says so. Known risk: it can
  misfire on black cutout props (fences, grates), and the glTF export still
  clips them.

**Water presets (P12)**

- Queensdale's `dataGlobal` water presets were measured and pinned by a test
  (`materials_queensdale_water_presets`): 3 presets, preset 0 drawn (it pairs
  with lighting preset 0; the pairing is UNPROVEN). The values quoted in note
  section 6 (shallow A5DF5C, deep 364F21, fresnel 3.89, depth 15.66, pattern
  4.58) are in none of them, probably because they belong to a per-zone block
  `load_world` does not read. The note carries a dated correction.

**Headless and UI**

- `GW2_MAPWIN_SHADING`, `GW2_MAPWIN_TIME` and `GW2_MAPWIN_FOCUS` are in
  `core/env.h`. `GW2_MAPWIN_FOCUS=animated` frames the nearest playing animated
  placement and reports its screen rectangle as `animRect`.
- The stats line gained `shading`, `time`, `textures`, `textureMB`,
  `texturesFailed`, `animated`, `unresolvedClips`, `chunksUntextured`,
  `materialsUntextured`, `waterPreset` and `animRect`, besides the
  `add_models` figures the pre-planning list names.
- The Map window has **Materials** and **Motion** toggles and a
  `tex N (X MB)  anim A/U` status field. The material clock step is clamped to
  0.1 s so a stall does not jump the animation (a fix in Task 14; P11: it had
  no separate re-review, the final branch review covers it).

**Measured**

- **Spirit Vale's placement count was measured in Task 15:** 41 (model,
  clip name) pairs tested, 37 resolve, and 106 of 116 animated placements resolve
  (`kSpiritValeResolvedPlacements = 106`, 2026-10-15), which matches the note's
  `propAnimArray` count of 116 as 106 animated plus 10 unresolved. The four
  misses are the clip names `on`, `zeropose`, `noarch` and `zeropose`.
- **Task 16, against the targets** (debug build, Materials shading): Queensdale
  models 8.08 s (sub-project 2: 155.7 s; target well under), total 28.05 s
  (162.6 s), largest `add_models` batch 13.08 ms (target under 50 ms),
  1124 textures, 624 MB, `texturesFailed` 0, `chunksUntextured` 0, 267
  animated, 2 unresolved. Lion's Arch: models 8.03 s, total 21.37 s, largest
  batch 48.30 ms (just under; batch time is noisy), 1301 textures, 761 MB.
  Spirit Vale: models 3.65 s, total 16.92 s, largest batch 9.68 ms, 547
  textures, 361 MB, 106 animated and 10 unresolved. Spirit Vale's animated
  rectangle differs in 10,431 pixels between time 0 and 2.5.
- **P13: the luma-variance check failed as a metric.** Queensdale Materials
  against Geometry gave a ratio of 0.296 against the plan's 1.5 (the Geometry
  look's white props on pink terrain have high variance). Textured rendering
  is shown by `chunksUntextured=0`, the texture counts, `materialsUntextured`
  and the viewed screenshots, not by that number.
- **Open finding: Lion's Arch has `unresolvedClips=134`**, against 2 on
  Queensdale and 10 on Spirit Vale. Not investigated. The Lion's Arch shot also
  shows pale faceted blended cards near the lighthouse (mist or cloud
  geometry), a v1 approximation.
