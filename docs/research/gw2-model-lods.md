---
name: gw2-model-lods
description: How GW2 stores model LODs (index-buffer LODs in one MODL), where the switch distances live (prp2 lod1/lod2 + OVERRIDE_LOD flag, MODL lodOverride), what castlemist parses/renders/exports today, and how to export LODs for a Unity LODGroup
metadata:
  node_type: memory
  type: project
  modified: 2026-10-07
---

# GW2 model LODs

Research for the future "map props as Unity prefabs with a LODGroup" export
(sub-project 5 in `docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md`).
Sources: castlemist code, the packfile struct template
`dumps/packfile/gw2_packfile.json`, the IDA annotations in
`tools/gw2_annotations.json`, and measurements made with `gw2dat_cli`
(`map`, `parse`) on real map and model files. Nothing here comes from reading
Gw2.dat directly.

## TL;DR

- **One model, several index buffers.** A GW2 MODL mesh has one vertex buffer
  and up to three index sets: `ModelMeshGeometryV1.indices` (LOD0) plus
  `ModelMeshGeometryV1.lods[]` (LOD1, LOD2). There are no separate LOD model
  files, and no impostor or billboard LODs.
- **Switch distances exist in two places.**
  - Per placement: `PackMapPropObj*.lod1` / `.lod2` (float), used only when
    prop flag bit 16 `OVERRIDE_LOD` is set.
  - Per model: `ModelFileDataV16+.lodOverride` (float[2]).
  - castlemist reads neither of them today.
- **LOD coverage is partial.** About 40% of the prop models I sampled have LODs,
  and the most I saw was 3 levels (LOD0..LOD2). Inside a multi-mesh model,
  often only one or two meshes have `lods`.
- **Export today is LOD0 only.** Both the glTF mesh export and the map export
  write `mesh.indices`. The other levels (`ModelMeshCPU::lodIndices`) reach
  the CPU model, but only the renderer uses them.

## 1. Storage: struct chain and parser

```
MODL file
  GEOM chunk (v1)  ModelFileGeometryV1
    meshes : ptr_array_ptr<ModelMeshDataV66>
      visBone, flags, meshName, minBound/maxBound, bounds[] (GrBoundData),
      materialIndex, materialName, boneBindings,
      geometry : ptr<ModelMeshGeometryV1>
        verts   : ModelMeshVertexDataV1          (one shared vertex buffer)
        indices : ModelMeshIndexDataV1           LOD0  { indices: array_ptr<word> }
        lods    : array_ptr<ModelMeshIndexDataV1> LOD1..N, same vertices
        transforms : array_ptr<dword>
```

Older formats (`ModelMeshGeometryV0`, and pre-GEOM `ModelMeshData.lods`) keep
the same idea. The engine reads the data per LOD:
`ModelFile_GetLodGeosets` (0x140CFBBD0, `Arena\Engine\Model\ModelFile.cpp`)
asserts `lodIndex < arrsize(m_geosets)`. See `gw2-render-asset-pipeline.md`.

Where castlemist parses it:

- `include/castlemist/native/gw2model.hpp`
  - About line 2542, `Extractor` mesh decode: `ModelMeshGeometryV1.lods` goes
    to `Mesh::lods` (`vector<vector<uint32_t>>`). Each level's
    `ModelMeshIndexDataV1.indices` is read as u16. Empty levels are kept so
    the numbering does not shift. There is a `lcount <= 16` sanity cap.
  - About line 2250: the raw path fills `GeosetRaw::lodIndices` (u16,
    verbatim).
- `src/extract/model_preview.cpp:299`: `mesh.lodIndices = src.lods;` copies
  the levels into `ModelMeshCPU::lodIndices`
  (`include/castlemist/extract/model_types.h:268`, "LOD1..N, indexing the
  same vertices").
- `src/ripper/parts.cpp:275` flips winding on every LOD index set together with
  LOD0. That means the ripper/export path already has them.

## 2. Switch distances

I searched every `Model*`/`Prop*` struct reachable from `ModelFileDataV70`,
`ModelFileGeometryV1` and `PackMapPropV21` for field names matching
`lod|dist|fade|visib|cull|screen|bias|impost|billb`. The search found only the
fields below as geometry-LOD data. The cloth `lod0/1Constraints`, the light and
wind `near/farDistance`, and the animation `animLod` are not geometry LOD.

### Per placement: `prp2` → `PackMapPropObj*V3..V21`

Fields in every version from V3 to V21 of `PackMapPropObj`, `...AnimSeq`,
`...Instance` and `...Meta`, after `scale`:

| field | type | meaning |
|---|---|---|
| `lod1` | float | LOD0→LOD1 distance (GW2 units, inches) |
| `lod2` | float | LOD1→LOD2 distance (inches) |
| `flags` | dword | `MapPropFlags`. Bit 16 is `OVERRIDE_LOD`. |

These come from the IDA string table `g_MapPropFlagNames[28]`, indexed by bit:
4 `DROP_LOD_2`, 7 `DISABLE_LOD`, 13 `NO_CULL`, 16 `OVERRIDE_LOD`,
19 `DETAIL`, 24 `CUSTOM_CULL`, 27 `NO_FADE`. The engine also has
`MAP_PROP_FLAG_STIPPLE_FADE`.

`PackMapPropTransformV21`, the per-instance transforms of `propInstanceArray`,
has only position, rotation and scale. Every transform therefore shares the
base record's `lod1`/`lod2`.

From the IDA annotations:

- `PrDataImportImportPropData`: "Per-prop culling/LOD behaviour comes from
  `PackMapPropObj.flags` … plus lod1/lod2 fade distances, which
  `PrContext_LoadPropModel` later turns into the `lodDistances[4]` array it
  hands to `ModelMgr_LoadModel`."
- `PrContext_LoadPropModel` "picks one of TWO LOD-distance sets based on prop
  flag 0x20 at +220 (high-detail vs normal)". That is a quality setting.
- `ZnModel_RequestModelLoad`, for zone/static scenery, also builds
  `lodDist0..3`.
- `MfOldProp::ConvertPropFadeDistances`: old records stored these as u16
  16.16 fixed point. The sentinel `0xBAAD` means "derive as
  clamp(2·second, min 0.75)".

The exact mapping from `lod1`/`lod2` to the 4 entries of `lodDistances[4]`
has not been decompiled. The other two entries are probably the fade-out
start and end, but that is not verified.

**Measured** with `gw2dat_cli parse` on 3 maps, all props except tool and
volume records:

| map fileId | props | `lod1 > 0` | bit 16 set | notes |
|---|---|---|---|---|
| 197249 | 3352 | 2663 | 2663 | 2604 of 2663 have **`lod1·scale = 2500`, `lod2·scale = 4250`** |
| 3560860 | 230 | 79 | 79 | `lod1 == lod2` is common (61); values up to 36 882 |
| 2257438 | 14 | 10 | 10 | `lod2 = 2·lod1` on most |

- `lod1 > 0` matches bit 16 (`OVERRIDE_LOD`) exactly in all three maps.
  When the bit is clear, both fields are 0 and the engine uses its default,
  most likely the model's `lodOverride`.
- In map 197249 the stored value is the world distance divided by the
  instance scale. For example, scale 0.65 stores 3846 = 2500/0.65. The
  effective world-space switch distance therefore looks like `lod·scale`.
  Inference: confirm in `PrContext_LoadPropModel` before relying on it.
- 2500 / 4250 inches ≈ 63.5 m / 108 m.
- Flag bits 23 and 30 are set on most props. Bit 30 lies outside the
  28-name table. Treat flag names other than bit 16 as unverified until they
  are checked against a decompile.

### Per model: `MODL` → `ModelFileDataV16..V70.lodOverride`

`lodOverride : float[2]` exists from ModelFileData V16 onward. Values measured
on 20 prop models:

| value | models |
|---|---|
| `3500, 7500` | 13 |
| `500, 4000` | 3 |
| one each | `3500, 5000`; `2000, 4000`; `1500, 3000`; `3500, 5553`; `10039.9, 11000`; `800, 1400` |

`3500, 7500` (≈ 89 m / 190 m) is clearly the authoring default. Whether
`lodOverride` is in model space or world space is not verified.

### Mesh-level

`ModelMeshDataV66` has no distance, screen-size or bias field. Its `flags` and
`visBone` control visibility through a bone, not LOD.

## 3. Separate LOD models, impostors, billboards

- A prop record has exactly one model reference (`filename`), plus
  `blitTextures`, `constants` and `permutation`. MODL has no field that
  references another model as a LOD. **All LODs are index-buffer LODs inside
  the same file and share vertices and materials.**
- There is no impostor or billboard LOD. No field or struct name matches
  `impost`, `billb` or `sprite` in the Model/Prop structs. The only `*Card*`
  types are the sky cards in `env`. Distant props fade out (`NO_FADE` and
  `STIPPLE_FADE` exist) instead of being swapped for a card. Some models are
  card geometry at LOD0 (for example 291982, a stack of coplanar masked
  planes; see `gw2-shaders-dxbc.md`), but those are ordinary meshes.

## 4. Measured LOD and triangle counts (prop models)

I ran `gw2dat_cli parse --file-id N --max-depth 9` on each model and walked
`GEOM → meshes[i] → geometry* → indices / lods[k].indices`. Triangles are
index count / 3. **The `gw2dat_cli model` JSON does not report LODs**: its
`indexCount`/`triangles` are LOD0 only, so `parse` is the tool for this.

| model fileId (map) | meshes | meshes with lods | tris LOD0 / LOD1 / LOD2 (meshes that have that level) | lodOverride |
|---|---|---|---|---|
| 1713091 (197249) | 1 | 1 | 1781 / 1034 / 500 | 3500, 7500 |
| 2257422 (2257438) | 1 | 1 | 8211 / 5218 / 3717 | 3500, 7500 |
| 1792088 (3560860) | 1 | 1 | 2208 / 1278 / 566 | 1500, 3000 |
| 3132352 (3560860) | 2 | 2 | 3203 / 1923 / 664 (mesh0 has only LOD1) | 3500, 7500 |
| 88666 (197249) | 2 | ≥1 | 3766 / 869 | 3500, 7500 |
| 89948 (2257438) | 10 | some | 1865 / 726 | 3500, 7500 |
| 77439 (197249) | 6 | 1 | 7824 total; mesh3 1456 → 851 | 3500, 7500 |
| 86177 (2257438) | 10 | 1 | 13534 total; mesh5 4205 → 2546 | 3500, 7500 |
| 500390, 888451, 526614, 71897, 76918, 499278, 3135004, 3132874, 3560847, 79554, 1735021 | 1–7 | 0 | 12 … 20442, LOD0 only | various |

Also known from before: 291977 has 2080 / 1226 / 627
(`docs/research/castlemist-app.md`).

Takeaways:

- 3 levels at most, matching the two distances `lod1`/`lod2`.
- Each level cuts roughly 40–60% of the triangles.
- Many props, including big ones (3135004 at 20k tris, 3132874 at 10k), have
  no LODs.
- In multi-mesh models, the LOD often covers only the one heavy mesh. The
  other meshes have no `lods`. Whether the engine keeps drawing those meshes
  at LOD1/LOD2 (as LOD0) or drops them is not verified.
  `src/render/map_fly.cpp` `flatten()` keeps them, falling back to the
  coarsest level the mesh has.

## 5. How castlemist uses LODs today

- **Single-model viewer** (`src/render/geometry.cpp:85-105`, state in
  `src/render/detail/state.h:67-72`)
  - Packs LOD0 and each non-empty LOD into one index buffer, with per-submesh
    `lodRanges`.
  - `set_submesh_lod` / `max_lod_count` back the LOD combo box. Selection is
    manual, with no distance logic.
- **`GW2_LODTEST=1`** (`src/ui/application.cpp:633`)
  - Prints `LODTEST: submeshes=… maxLODs=…`.
  - Saves wireframe `shot_lod0.bmp` and `shot_lodmax.bmp`, plus
    `shot_texreduced.bmp`.
  - The table entry in `include/castlemist/core/env.h:31` ("each
    LOD/texture-size combination") overstates this a little.
- **Map fly-through** (`src/render/map_fly.cpp`)
  - `flatten()` merges every submesh's LOD k into one range per level.
  - The level is picked by the angular size `radiusW / distance`, with
    thresholds 0.20 / 0.06 / 0.02 chosen by castlemist. These are not GW2
    values, and the code does not read `lod1`/`lod2`/`lodOverride`.
- **Map scene parse**
  - `Extractor::parseMapProps()` (gw2model.hpp about line 1583) reads only
    `filename`, `position`, `rotation`, `scale` and `bounds`.
  - `MapProp` and `MapInstance` (`include/castlemist/extract/map_types.h`)
    have no LOD or flag fields. `lodOverride` is not parsed anywhere.

## 6. Exporter today

- `src/exportgltf/mesh_export.cpp:132` writes `mesh.indices` only, as one
  primitive per mesh. There is no reference to `lodIndices` anywhere in
  `src/exportgltf`.
- `src/exportgltf/map_export.cpp` writes each unique model's meshes once and
  instances them per placement node. That is LOD0 only, and no LOD distances
  are written.

## 7. Recommendation for a Unity LODGroup export

**Geometry.** For each model, emit LOD0..N as separate meshes that share the
same vertex attributes and materials.

- glTF allows several primitives or meshes to reference the same
  POSITION/NORMAL/TEXCOORD/JOINTS accessors. Each LOD primitive then adds only
  a new `indices` accessor and keeps the same `material` index, so there is no
  duplication of vertex data.
- The number of levels is `N = max over meshes of (1 + count of non-empty
  lods)`.
- For a mesh that has fewer levels than N, reuse its coarsest level at the
  higher levels, as `map_fly.cpp flatten()` does. The alternative is to drop
  the mesh, but keeping it is the safe default until the engine's behaviour is
  confirmed. Skip empty index sets, as `geometry.cpp` does.
- Models with no LODs export a single level and get no LODGroup, or a LODGroup
  with only LOD0 + Culled.

**Naming and hierarchy.**

- Put the levels under one model node as children named `<Model>_LOD0`,
  `<Model>_LOD1`, `<Model>_LOD2`. Unity's model importer turns the
  `_LODn` suffix into a LODGroup automatically on FBX import, which the spec
  already routes through Blender.
- The castlemist Unity importer (sub-project 3) can also build the LODGroup
  explicitly from a sidecar JSON. That is better, because it can carry the
  transition values.
- `MSFT_lod` + `MSFT_screencoverage` is the standard glTF way to express this,
  but common Unity glTF importers ignore it. Use it, if at all, only as extra
  metadata.

**Distances, per placement:**

1. Prop flag bit 16 (`OVERRIDE_LOD`) set: `d1 = lod1·scale`, `d2 = lod2·scale`
   in world inches. The `·scale` is the interpretation measured in section 2;
   verify it.
2. Otherwise use the model's `ModelFileData.lodOverride[0..1]`. This needs a
   new parse in `Extractor`, since only the raw parse dump shows it today.
3. Otherwise default to 3500 / 7500 inches (≈ 89 m / 190 m).
4. Bit 7 `DISABLE_LOD`: LOD0 only. Bit 4 `DROP_LOD_2`: drop LOD2. Treat both
   as tentative until the flag table is verified.

Convert to metres with ×0.0254.

**Unity transition values.** A LODGroup switches on screen-relative height,
not distance. For a LODGroup of world size `S` metres (Unity's
`LODGroup.size` × the largest lossy scale axis), a switch at distance `d`
metres with vertical FOV `fov` is

```
h(d) = S / (2 · d · tan(fov/2))      (then Unity scales by QualitySettings.lodBias)
```

- Write `screenRelativeTransitionHeight = h(d1)` for LOD0 and `h(d2)` for
  LOD1.
- Use 60° for `fov` (the VRChat desktop default).
- The last level needs a Culled threshold. GW2's fade-out distance has not
  been extracted (it is probably entries 3–4 of `lodDistances[4]`), so
  default the Culled height to about 1–2%, or 0 for very large or landmark
  props.

**Where to store distances.**

- When distances come from the model (`lodOverride` or the default), they are
  the same for every placement. Store them on the prefab.
- `OVERRIDE_LOD` placements carry their own values. Write them as
  per-instance LODGroup overrides in the scene/world builder.
- Because Unity's height already includes instance scale, a prefab-level value
  computed with scale 1 is correct only where GW2's world distance also scales
  with the instance. Map 197249's constant `lod·scale = 2500` suggests GW2
  keeps the world distance constant. The safest choice is to compute `h`
  per instance from its world `S`.

**Open items to verify, IDA-side, before trusting the numbers:**

- How `PrContext_LoadPropModel` builds `lodDistances[4]`: does it multiply
  `lod1/lod2` by scale, and is it a fallback to `lodOverride`?
- What entries 2–3 of that array hold (the fade or cull distance).
- What the high-detail vs normal distance sets are (prop flag 0x20 at +220).
- Whether meshes without `lods` stay visible at LOD1/LOD2.

## Amendment 2026-10-17: the client's rule, and what the export does (Task 15b)

**Executable.** `Gw2-64-disable-aslr.exe` (md5 `1495f3a25b4a0ecc6cb71cf7fb81f8df`,
imagebase `0x140000000`), disassembled with `objdump -d -M intel`. The
annotations file's addresses are from an older build (shifted by about
`0x6500` here). The game was not run.

**`lodDistances[4]` is not the switch distances.** `PrContext_LoadPropModel`
(here `0x140c7be50`) fills four *bytes* from preference getters
(`0x1409b4ed0` .. or `0x1409b6470` .. by prop flag `0x20` at `+0xdc`) and
passes them to `ModelMgr_LoadModel` (`0x140cf7000`). They are quality
settings, not distances. The open item about "entries 2–3" is moot.

**The model's distances.** The model object (vtable `0x141ee0310`) keeps
`lod1` / `lod2` at `+0x128` / `+0x12c`:

- At construction (`0x140cf8184`) they are **3500 / 7500** (`0x141ae20d0`,
  `0x141ee186c`), or 800 / 1800 when bit 10 of the load flags at `+0x90` is
  set (not a prop load as far as traced). Each is then replaced by `+0x130` /
  `+0x134` when that is > 0, clamped to 100 000 (`0x141b991f4`).
- On load (`0x140cf7b50`) `+0x130` / `+0x134` come from the model file's
  `ModelFileData` at `+0x5c` / `+0x60`, which is `lodOverride[0..1]` (the
  V66/V70 offset with 8-byte pointers), taking the larger of the model's and
  a second file's value. While no explicit distance was set (bit 7 / bit 6 of
  `+0x144`), they become the model's distances, clamped to [0, 100 000].
- So the model's switch distances are `lodOverride[k]` when it is > 0, else
  3500 / 7500. That is the rule `world_lods.h model_lod_distances` applies.

**The placement's distances.** After the model loads (`0x140cb3f05`, and the
same code at `0x140cb5a04`), the prop calls the model's setter (vtable `+0x7e8`,
`0x140cfa0e0`) with `lod1` / `lod2` from the prop (`+0x3c` / `+0x40`) only when
`flags` (`+0xdc`) has bit 16 (`OVERRIDE_LOD`, `0x10000`) **and** they are not
both 0. The setter clamps each to at most 100 000 and makes a negative one 0.

**The switch.** The model's LOD pick (`0x140cf9010`):
`d_k = lod_k × s × G`, where `s` is the instance transform's largest axis scale
(`0x140bac510`; 1 when the transform has no scale) and `G` is a global float
(`.data 0x1425927f8`, initial value 1.0, probably a quality setting; its
runtime value was not read). The distance is the 3D distance from the camera to
the model's bounds. Level 0 below `d_1`, level 1 from `d_1`, level 2 from `d_2`,
with a 75-unit hysteresis (`0x14192d3fc`) when stepping up. This confirms the
`lod · scale` reading of section 2 for the per-placement values, and shows the
model's own distances scale with the instance too.

**Consequence for Unity.** Unity's screen-relative height carries the same
instance scale, so `h = size / (2 · d_k · tan(fov/2))` with the prefab's own
LODGroup size and `d_k` in model units is right for every instance: no
per-instance value is needed except for `OVERRIDE_LOD` placements. The
"compute per instance from its world S" advice of section 7 is not needed.

**Measured on the three test maps** (`world_export_dat.lods`, every prop model
loaded, LOD0 / LOD1 / LOD2 triangles with a mesh without its own level
repeating its coarser one):

| map | models with LODs | of them 3 levels | placements of LOD models | OVERRIDE_LOD placements (on LOD models) | tris LOD0 → LOD1 → LOD2 (models) |
|---|---|---|---|---|---|
| Queensdale 192711 | 177 / 2244 (7.9%) | 54 | 646 / 11492 (5.6%) | 785 (37) | 722 824 → 503 518 (−30.3%) → 451 828 (−37.5%) |
| Lion's Arch 191000 | 225 / 1706 (13.2%) | 98 | 2747 / 18319 (15.0%) | 1327 (194) | 901 029 → 639 207 (−29.1%) → 527 497 (−41.5%) |
| Spirit Vale 1151420 | 72 / 383 (18.8%) | 23 | 564 / 2354 (24.0%) | 42 (32) | 441 734 → 308 178 (−30.2%) → 273 598 (−38.1%) |

- Coverage is well below section 1's "about 40%": that came from a sample of
  20 models. Over whole maps it is 8–19% of the prop models.
- Almost every model stores a non-zero `lodOverride` (2244 / 2244, 1705 / 1706,
  382 / 383); only 528, 516 and 97 of them hold the 3500 / 7500 pair.
- In Queensdale, 6667 placements store a non-zero `lod1` or `lod2` without
  `OVERRIDE_LOD`; the other two maps have none. The client ignores them.
- Models 1713091 and 1792088 give exactly section 4's triangle counts and
  `lodOverride` values.

**Still open.** Whether a mesh without its own `lods` is hidden at LOD1/LOD2
(the export repeats its coarser level, `settings.lodMissingLevel`); the
runtime value of `G`; GW2's prop fade/cull distance (the export never culls a
prop, `settings.lodPropCullHeight` 0); what `DROP_LOD_2` (bit 4) and
`DISABLE_LOD` (bit 7) do (bit 4 is tested near the prop's fade code at
`0x140cb49ec`; not acted on).
