---
name: gw2-terrain-to-unity
description: "How to turn a GW2 map's ground layer (trn heightfield, terrain blend, zon2 foliage scatter, prp2 trees, dcal decals) into native Unity Terrain for VRChat worlds (Unity 2022.3.22f1, Built-in RP, Poiyomi). What the dat holds and what castlemist decodes, the exact GW2 -> TerrainData mapping, how props split into Terrain trees / details / instances, a decal strategy, a staged pipeline for world / exportgltf / the Unity importer, and the risks. Research only; anything unproven is marked UNPROVEN."
---

# GW2 ground layer to native Unity Terrain

Research for a possible stage after the World Export
(`docs/superpowers/specs/2026-10-15-world-export-design.md`, sub-project 4).
Done 2026-10-15/16. It builds on `gw2-world-frame.md` (§3 terrain layout,
§4 terrain materials, §5 props), `gw2-world-materials.md` (§1 terrain blend)
and `gw2-model-lods.md`, and does not repeat what they prove. Same rule as
those notes: every claim has its evidence, or is marked **UNPROVEN** with
what would prove it. Web facts carry their URL; "inference" marks a
conclusion drawn here rather than read in a source.

---

## Summary: the recommendations

| question | recommendation | confidence |
|---|---|---|
| Mesh terrain or Unity Terrain? | **Keep the mesh terrain as the default and add Unity Terrain as an opt-in importer mode** (`terrainMode: mesh / unityTerrain`), built from the same heightfield. | high for "keep mesh"; Unity Terrain gains are UNPROVEN in VRChat until measured |
| PC VRChat | Unity Terrain is worth it mainly for **foliage** (details and trees with distance culling) and for heightfield LOD on big maps. Start with **one TerrainLayer per tile carrying the existing baked albedo** (one pass, today's look, Poiyomi not needed for the ground), then optionally real splat layers. | medium |
| Quest / Android | **Stay on mesh terrain**, region-limited and decimated; no Unity Terrain, little or no foliage. Queensdale's full terrain mesh alone is 1.09 M triangles against VRChat's ~250 k whole-world budget for Android. | high |
| Plain Unity (not VRChat) | Unity Terrain in **splat mode** (real ground textures, 4 per pass), with details and trees. | medium |
| Tile size | **One Unity Terrain per 4 × 4-chunk page block** (the export's cells): heightmap **129**, alphamap **512** (exactly the GW2 blend page), detail resolution **256** (exactly the GW2 zone grid), holes 128, 12 288 map units a side. | high (all exact data fits) |
| Foliage | GW2 has a real foliage layer, **`zon2`**, that castlemist does not export yet (217 114 placements on Queensdale). Zone layer type 1 and 2 (small cutout cards, ≤ 300 triangles) → **Unity details** (instanced mesh); type 3 (trees, bushes, rocks up to 2119 units tall) → **Unity Terrain trees** via prefabs with a LODGroup. | layer-type meaning UNPROVEN; the size split is measured |
| `prp2` props | **Stay GameObject instances**, as today, including trees placed as props: they carry full 3-axis rotation, collision hulls, LOD distances and animation. | high |
| Decals | **Export `dcal` decals as conforming mesh decals** cut from the heightfield (13 on Queensdale, 3 on Lion's Arch, 0 on Spirit Vale). No Projectors. Decal-like props stay props with the dark-decal rule. | high for "mesh, not Projector" |
| Exporter additions | Per tile: a **16-bit RAW heightmap** in Unity row order, splat RGBA8 PNGs (4 layers each), a holes mask, a layer table (texture fileIds, tile size), detail-count maps and tree-instance lists, in an optional manifest block. The importer only builds; it does no frame maths. | high |

**Top unknowns:** (1) the foliage scatter's exact game placement (per-instance
jitter, rotation, scale and the RNG; castlemist's zone parser plants one
unrotated model per cell and only reads each layer's first model); (2) how
Unity Terrain actually performs and lights in VRChat on this data (known
avatar-load stutter, historical lightmap problems, instanced details get no
lightmaps or light probes); (3) the ground texture repeat (`uvScale`, 8 is
T3D's guess) and the meaning of `trn.tileFlagArray` / `chunkFlags` (possible
holes).

---

## Evidence base

- **Repo, read-only:** the notes and specs named above; `include/castlemist/world/*`
  (`world_scene.h`, `terrain_blend.h`, `terrain_bake.h`, `cells.h`,
  `terrain_mesh.h`, `surface_rules.h`), `include/castlemist/native/gw2model.hpp`
  (`parseMapZones`, lines 960-1090), `tools/unity/Castlemist/Editor/*`,
  `tools/blender/castlemist_world.py`, the export ledger
  `.superpowers/sdd/2026-10-15-world-export/task-13-report.md`.
- **Template:** `dumps/packfile/gw2_packfile.json` (structs `PackMapTerrainV15`,
  `PackMapZonesV23`, `PackMapDecalsV10`, `PackMapPropV21`).
- **castlemist tools only opened the dat** (main checkout build
  `build/debug/bin/gw2dat_cli.exe`, Gw2-64.exe not running), all output to the
  session scratchpad, nothing committed:
  - `gw2dat_cli map --file-id {192711, 191000, 1151420}` (terrain dims, height range, zone placements);
  - `gw2dat_cli world --file-id 192711` (WorldScene warnings: unread chunks);
  - `gw2dat_cli parse --file-id {192711 depth 8, 191000 / 1151420 depth 6}`, walked by small
    Python scripts (not committed) for `trn`, `zon2`, `dcal`, `prp2`, `shex`, `octm`, `surf`, `laye`;
  - `gw2dat_cli model --file-id N` for all 195 distinct Queensdale zone models and a sample of the most-placed `prp2` models;
  - `gw2dat_cli amat --file-id N --out-dir` for six AMATs (foliage, tree, decal, terrain); uniform names read from the written VS blobs.
- **Test maps:** Queensdale 192711, Lion's Arch 191000, Spirit Vale 1151420.
- **Web:** Unity 2022.3 manual and scripting reference, VRChat creator docs and
  release notes, VRChat feedback board, Unity forums, Poiyomi docs. URLs inline.
- **T3D** (GPL-3) is cited as prior art only. Nothing here is copied from it.

---

## 1. What a GW2 map's ground layer contains

### 1.1 Heightfield (`trn`) — decoded

From `gw2-world-frame.md` §2-§3 (PROVEN there) and `gw2dat_cli map`:

- A grid of chunks, each **32 segments = 33 own samples** a side, 3072 map
  units per chunk, so **one sample every 96 units**. Stored as `float32`
  map-space z (up = −Z), chunk-major with a 1-sample apron.
- The de-tiled map-wide grid is `(32·chunksX + 1) × (32·chunksY + 1)` samples:

  | map | chunks | samples (E × N) | extent (units) | stored z min .. max | range |
  |---|---|---|---|---|---|
  | Queensdale 192711 | 28 × 19 | 897 × 609 | 86 016 × 58 368 | −5768.9 .. 1701.5 | 7 470 |
  | Lion's Arch 191000 | 19 × 12 | 609 × 385 | 58 368 × 36 864 | −6165.8 .. **10000.0** | 16 166 |
  | Spirit Vale 1151420 | 10 × 25 | 321 × 801 | 30 720 × 76 800 | −14465.7 .. **4999.9995** | 19 466 |

  Queensdale's extremes are ordinary values (its five largest are 1681-1701).
  Lion's Arch's exact 10000.0 and Spirit Vale's 4999.9995 look like **sentinel
  or clamp values**; what they mark is UNPROVEN (proof: count and map the
  samples at those values). They matter here because they stretch the
  quantisation range (§4.3); in stored z (up = −Z) they are the **lowest**
  points, so they are not peaks.
- Terrain is a single-valued heightfield: no overhangs. Cliffs, arches, caves
  and raised floors are **props** standing on or above it (Spirit Vale: 1274
  of 2423 props sit more than 500 units above the terrain under them,
  `gw2-world-frame.md` §3.3), and walkable through their `havk` hulls.

### 1.2 Ground textures and blend — decoded (render), partly UNPROVEN

From `gw2-world-frame.md` §4 and `gw2-world-materials.md` §1:

- **Per chunk:** four colour textures (`color` … `colord`) and four matching
  normal maps, plus two 512 × 512 **pages** shared by 4 × 4 chunks: `blend`
  (picker, DXT5) and `modx` (DXT1). A chunk owns **128 × 128 page texels**
  (4 per height segment).
- **Weights:** picker channels R → `colord`, G → `colorc`, B → `colorb`,
  A → `color`; each weight is multiplied by its ground texture's
  **alpha + 0.01** (a per-texel height bias at the tiled ground UV) and
  normalised. Then `modx.G` saturation, `modx.R` brightness (×2), `modx.B` a
  fresnel rim. Normal maps use the same weights.
- **Counts, measured on Queensdale (this note):** 45 distinct colour textures
  map-wide, 84 distinct 4-texture sets. 527 chunks bind 4, 5 bind 3.
  Distinct colour textures **per 4 × 4 block: min 6, median 9, max 12** (35
  blocks); **per 2 × 2: min 4, median 5, max 9**.
- **UNPROVEN:** the ground repeat per chunk (`uvScale`; castlemist and the
  export use T3D's 8, `kGroundRepeatsPerChunk`); the `tiling` bytes; the
  normal maps' tangent sign; `pickerSolid` byte order; the 36 Spirit Vale
  chunks with `uvData` animation and a third UV set.

### 1.3 Foliage and ground clutter: `zon2` — native parser only, NOT in WorldScene, NOT exported

**GW2 has a dedicated scatter layer.** `zon2` (`PackMapZonesV23`, read as v22
on Queensdale) holds `zoneDefArray` (each def: `token`, `layerDefArray`) and
`zoneArray` (each zone: `defToken`, `vertRect` (a cell rectangle), `seed`,
`range`, `zPos`, `flags[]`, `encodeData`, `collideData`, `vertices`). A layer
def has `type, width, height, radiusGround, sortGroup, tiling, scaleRange,
probability, fadeRange, rotRange[3], hslRanges[4], instanceScaleJitter,
noise, layerFlags, materialname` and `modelArray[]` (`filename, probability,
flags, hslOffset, zOffsets, permutation`).

What castlemist does today (`gw2model.hpp:960-1090`, `parseMapZones`): walks
`flags[]` as run-length `(skip, flag)` byte pairs over the zone's cells on a
**48-unit grid** from the `parm` rect corner, plants one instance per
non-zero flag, layer = `flag >> 4` (1-based), model = that layer's
**`modelArray[0]` only**, at the zone's `zPos`, unrotated, scale 1. Its
comment says it was "Ported from Tyria3D's ZoneRenderer" (see §9, licence).
`gw2dat_cli map` reports it; `load_world` does not attach it
(`chunk zon2 v22 not read` is in Queensdale's `WorldScene::warnings`), so the
map window and the world export have **no foliage**.

The 48-unit cell is consistent with the data: Queensdale is 896 terrain tiles
× 96 = 86 016 units = **1792 × 48**, and the largest `vertRect` x seen is
1620. (48 = half a height sample; the constant itself is T3D's.)

Measured placements (same RLE walk, counted per layer `type`):

| map | zone defs / layers | zones | placements | type 1 | type 2 | type 3 |
|---|---|---|---|---|---|---|
| Queensdale | 42 / 142 | 248 | **217 114** | 167 169 | 39 649 | 10 296 |
| Lion's Arch | 26 / 60 | 447 | 32 448 | 29 768 | 1 669 | 1 011 |
| Spirit Vale | 2 / 3 | 26 | 7 642 | 7 358 | 0 | 284 |

The low nibble of each flag is 1, 3, 5 or 7 on 94 % of Queensdale's
placements (0, 2, 4, 6 on the rest); its meaning is **UNPROVEN** (a model
index into `modelArray` and an "on" bit is a hypothesis; proof: the client
code that reads the flag, or a frame capture of one zone).

What the layer types hold — all 195 distinct Queensdale zone models through
`gw2dat_cli model`:

| layer type | layers | distinct models | triangles min / median / max | largest extent min / median / max (units) | cutout | fade far (median, units) |
|---|---|---|---|---|---|---|
| 1 | 87 | 72 | 10 / 26 / 280 | 3 / 50 / 149 | 72 / 72 | 3 000 |
| 2 | 11 | 38 | 10 / 45 / 122 | 36 / 86 / 189 | 38 / 38 | 4 025 |
| 3 | 44 | 95 | 2 / 248 / 3 181 | 6 / 234 / 2 119 | 95 / 95 | 6 000 (max 17 000) |

Type 1 is grass and small clutter; type 2 is larger plants (several layers
name a `materialname`, e.g. 184994, the same AMAT their models use); type 3
holds trees and big shrubs (85448: 152 triangles, 2119 units tall; 85520: 2540
triangles, 4 meshes). This reading of `type` is inference from sizes and
fade ranges; the game's meaning is **UNPROVEN**. Type-1 layers are always
2 × 2 cells (`width`, `height`); type 3 ranges 2-14, which looks like a
spacing.

**Wind is in the shader.** The VS of every foliage and tree AMAT checked
(77876, 184994 — zone types 1 and 2; 56795, 57714, 85295 — type 3) declares
`Wind`, `WindGust`, `VegPara` and `Time` uniforms and takes a vertex
`COLOR`. The terrain (187795) and the decal AMAT (58246) do not. So GW2
foliage sways in the vertex shader, presumably weighted by vertex colour; the
formula is **UNPROVEN** (proof: transcribe one VS, as `gw2-world-materials.md`
§1.2 did for the terrain PS). No GW2 model has a billboard or impostor LOD
(`gw2-model-lods.md` §3); trees fade out at `fadeRange` instead.

Not reproduced anywhere in castlemist and **UNPROVEN**: the per-instance
offset inside a cell, rotation (`rotRange`), scale (`scaleRange`,
`instanceScaleJitter`), the `probability` cull, `noise`, `hslRanges` tint,
the RNG and its seeding (`seed` is per zone, 0 on many), the ground
alignment, and `zOffsets`. A Unity build can match the game's **density and
model mix per cell**, not its exact instances, until these are proven.

### 1.4 Trees placed as props: `prp2` — decoded and exported

`prp2` placements are already exported as prefab instances (11 687 on
Queensdale, 18 572 on Lion's Arch, 2425 on Spirit Vale, task-13 report). A
`PackMapPropObjV21` carries `filename, blitTextures, constants, guid,
permutation, bounds, position, rotation (3 Euler), color, scale, lod1, lod2,
flags, broadId, bucketId, sortLayer`. Big landmark trees are props, with full
rotation and often a `havk` hull. `flags` is a bit field whose bits are
unnamed apart from `OVERRIDE_LOD` (bit 16, `gw2-model-lods.md` §2); a
foliage/wind bit is **UNPROVEN**. The best tree signal available today is
the model's AMAT: a VS with `VegPara` (§1.3). How many prop models use such
shaders is in §6.4 (a sample: 11 % of placements).

### 1.5 Decals: `dcal` — NOT decoded

`PackMapDecalsV10.decals[]` = `{position, extents, rotation (float3 each),
textureScaleUV0/1, textureOffsetUV0/1, gridSize, materialFilename,
textureFilenames[], flags, anim* (UV animation), surfaceBias,
constantTokens/Values, vertices[] (position, normal, tangent, bitangent),
indices[], propIds[], projection (byte), surfaceId, id}`. So GW2 has
**box-projected decals** that the engine turns into meshes on the surfaces
under them, optionally including props (`propIds`).

| map | decals | material | extents | `vertices` | `propIds` | `gridSize` | `surfaceBias` |
|---|---|---|---|---|---|---|---|
| Queensdale | 13 | all AMAT 58246 | cubes 46-66 units | 0 (all) | 1-4 per decal | 16, 16 | 1.0 |
| Lion's Arch | 3 | 58246, 189632, 185120 | 60 × 60 × 7 to 424 × 166 × 176 | 0 | — | 16, 16 | 1.0-1.46 |
| Spirit Vale | 0 | — | — | — | — | — | — |

`vertices` is empty on every decal seen, so the mesh is **built at load**,
presumably as a `gridSize`-spaced grid conformed to the terrain and lifted by
`surfaceBias` (inference; UNPROVEN). AMAT 58246 is the darkening decal that
the sub-project 3 dark-decal rule was measured on (spec 2026-10-14,
amendments). Decal-like **props** also exist: materials on the `prop-decal`
and `decal-glow` profiles (`gw2-material-channels.md`) and the dark-decal
props the export flags in `darkDecalMaterials`.

### 1.6 Holes and other terrain flags — NOT decoded, UNPROVEN

- `trn.tileFlagArray` holds `dimX · dimY / 16` dwords (Queensdale 34 048 =
  896 · 608 / 16; Lion's Arch 14 592; Spirit Vale 16 000), so **2 bits per
  terrain tile** is the natural reading. On Queensdale and Lion's Arch the
  commonest values are `0xFFFFFFFF`, `0`, `0xAAAAAAAA`, `0x55555555` (2-bit
  patterns). Spirit Vale's include `0x3F800000` (= 1.0f) and values near
  `0xBF7Fxxxx`, which look like floats, not flags. Whether any bit means "hole"
  is UNPROVEN. **Proof:** render a map's tiles by their 2-bit code against a
  known cave mouth or a known hole, or find the client code that reads the
  array.
- `trn.chunkArray[].chunkFlags`: Queensdale 0 ×520, 1 ×12; Lion's Arch 0 ×215,
  6 ×13; Spirit Vale 7 ×122, 1 ×92, 9 ×36. Meaning UNPROVEN.
- No map so far needs holes to be walkable: castlemist's walker already uses
  terrain ∪ `havk`, and Spirit Vale's raid floors are props.

### 1.7 Other ground-related chunks — NOT decoded

| chunk | what the template / values show | relevance |
|---|---|---|
| `shex` | `filename` (Queensdale 192445, LA 190924, SV 1151180), `shadowDims` (2304 × 2240, 2048 × 1664, 1280 × 2432) | a baked shadow texture for the map is the obvious reading; its mapping onto the map (dims do not divide the chunk grid evenly) is UNPROVEN. Could seed lightmaps or AO. |
| `octm` | `enableTomeQueries`, `tome` (13.1 M bytes on Queensdale), `propIDMap` | an occlusion "tome" (Umbra's term; inference). Unity bakes its own; not needed. |
| `surf` | `attributeData` (20), `terrainArray`, `propArray`; every chunk's `surfaceIndexArray` has 4 entries | surface types (footstep sounds, materials) per terrain texture, presumably. Not visual. |
| `laye` | `props` (10 821 on Queensdale), `lights`, `layers` (33) | prop layers (event or phase visibility, inference). Could decide which props to omit. |
| `trni` | `tableData` / `pageData` null on all three maps | nothing to read. |

### 1.8 Summary of what is missing for this work

| needed | chunk / field | castlemist today | research to do |
|---|---|---|---|
| foliage placements | `zon2` | native RLE walk, first model only, no jitter, not in `WorldScene` | own decoder from the template; `flags` low nibble; jitter, rotation, scale, probability, RNG |
| foliage wind | foliage AMAT VS (`Wind`, `WindGust`, `VegPara`, vertex `COLOR`) | not read | transcribe one VS |
| decals | `dcal` | not read | conform rule (`gridSize`, `surfaceBias`, `projection`), `flags`, UV fields |
| holes | `trn.tileFlagArray`, `chunkFlags` | not read | §1.6 proof |
| ground repeat | `uvScale` / `tiling` | 8, UNPROVEN | `gw2-world-materials.md` §1.6 proof |
| height sentinels | `trn.heightMapArray` values 10000 / 4999.9995 | kept as stored | count and map them |
| tree flag on props | `prp2 flags` | read, unnamed | bit census against VegPara models |

---

## 2. Unity Terrain facts (2022.3, Built-in RP)

- **Heightmap resolution** is clamped to "33, 65, 129, 257, 513, 1025, 2049,
  or 4097" ([TerrainData.heightmapResolution](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TerrainData-heightmapResolution.html)).
  `SetHeights` takes floats "ranging from 0 to 1, array indexed as [y,x]"
  ([TerrainData.SetHeights](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TerrainData.SetHeights.html)).
  The stored precision is not in the docs; `Terrain.heightmapFormat` is a
  native `GraphicsFormat` property with no stated value
  ([Terrain.heightmapFormat](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Terrain-heightmapFormat.html),
  [UnityCsReference Terrain.bindings.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/2022.3/Modules/Terrain/Public/Terrain.bindings.cs)).
  Unity's RAW import/export is 16-bit
  ([Working with Heightmaps](https://docs.unity3d.com/2020.2/Documentation/Manual/terrain-Heightmaps.html)).
  **That the in-memory heightmap is 16-bit is UNPROVEN here** (widely assumed);
  §4.3 budgets for 16 bits and shows that 15 would still be fine.
- **Holes:** `SetHoles(bool[y,x])`, "true for surface and false for hole"
  ([TerrainData.SetHoles](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TerrainData.SetHoles.html));
  `holesResolution = heightmapResolution − 1`
  ([TerrainData.bindings.cs](https://github.com/Unity-Technologies/UnityCsReference/blob/2022.3/Modules/Terrain/Public/TerrainData.bindings.cs)),
  i.e. one hole flag per height quad.
- **Alphamaps / layers:** `SetAlphamaps(float[y,x,layer])` checks the layer
  count against `alphamapLayers` (bindings source above). Built-in and URP
  render "four Terrain Layers per Texture pass, with no limit on the number of
  passes", each extra pass "increases the time spent rendering", and the
  advice is four layers per tile; Mask Maps are documented for URP/HDRP
  TerrainLit only; in Built-in the Normal Map is used "when instancing is
  enabled" ([Terrain Layers](https://docs.unity3d.com/2022.3/Documentation/Manual/class-TerrainLayer.html)).
  The allowed alphamap resolutions are native constants
  (`k_MinimumAlphamapResolution` … `k_MaximumAlphamapResolution`) not printed
  in the docs; 16-4096 powers of two is the editor's range (**UNPROVEN** from
  docs; the importer should read back `alphamapResolution` after setting it).
- **Neighbours:** `SetNeighbors(left, top, right, bottom)` "ensures LOD
  matches up on neighboring Terrain tiles" and must be called on every tile
  ([Terrain.SetNeighbors](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Terrain.SetNeighbors.html)).
- **Trees:** a prototype is a prefab; "A SpeedTree Prefab has an LODGroup
  component, but a Tree Editor Prefab does not"; tree colliders are a Capsule
  Collider on the prefab plus "Enable Tree Colliders"; Tree Editor trees need
  the Nature/Soft Occlusion shaders; Bend Factor and Wind Zones apply to Tree
  Editor trees ([Trees](https://docs.unity3d.com/2022.3/Documentation/Manual/terrain-Trees.html)).
  `TreeInstance` has `position`, `widthScale`, `heightScale`, a read-only
  `rotation` "on X-Z plane (in radians)", `color`, `lightmapColor`,
  `prototypeIndex` ([TreeInstance](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TreeInstance.html)):
  **yaw only**. A user report says trees without a LODGroup ignore rotation
  ([forum.arongranberg.com](https://forum.arongranberg.com/t/terrain-tree-collider-issue-unity-2020/9262), Unity 2020; UNPROVEN for 2022.3).
- **Details:** `DetailPrototype` has `usePrototypeMesh`, `useInstancing`,
  `prototype`, `minWidth/maxWidth/minHeight/maxHeight`, `density`,
  `alignToGround`, `positionJitter`, `noiseSeed`, `targetCoverage`
  ([DetailPrototype](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/DetailPrototype.html));
  `DetailScatterMode` is `CoverageMode` or `InstanceCountMode`
  (bindings source). Instanced mesh details are recommended; the shader
  "must support GPU instancing" and Unity uses "the material and the shader
  specified on the prefab"; batches are "1,023 or fewer"; **"Instanced light
  probe and lightmap lighting aren't supported"**; grass meshes and grass
  textures move in the wind, vertex-lit meshes do not
  ([Grass and other details](https://docs.unity3d.com/2022.3/Documentation/Manual/terrain-Grass.html)).
- **Terrain settings** (heightmap / control texture / detail resolution,
  detail resolution per patch "must be a divisor of the detail resolution",
  Draw Instanced, Pixel Error)
  ([Terrain settings](https://docs.unity3d.com/2022.3/Documentation/Manual/terrain-OtherSettings.html)).
- **Transform:** that a Terrain ignores its Transform's rotation and scale
  is a user claim ([Unity forum](https://forum.unity.com/threads/terrain-rotation-arbitrary-orientation-in-arkit-world.820497/));
  no official page says so. **UNPROVEN**; the safe design builds TerrainData in
  final (scaled) units under an unscaled, unrotated parent (§7).

## 3. VRChat facts

- **Android/Quest worlds** ([Quest content optimization](https://creators.vrchat.com/platforms/android/quest-content-optimization)):
  "budget approximately 250,000 triangles for your world in total"; "aim for
  using 1k (1024x1024) resolution textures at maximum"; "worlds that exceed
  100MB in size after build-time compression" cannot be uploaded; "Baking
  lighting for your world is **essential**"; "Baking occlusion culling is
  exceedingly important"; "You should be enabling GPU Instancing on all of
  your materials"; shaders are not restricted for worlds; avoid transparency.
  The page does not mention Terrain.
- **Unity Terrain in VRChat:** release 2023.4.1 lists as known issues "a
  significant stutter when loading avatars in worlds with Unity Terrain" and
  "Some older worlds have shader issues on Unity Terrain"
  ([VRChat 2023.4.1](https://docs.vrchat.com/docs/vrchat-202341)). Whether
  either still holds in current builds is UNPROVEN.
- **Lightmaps on Terrain:** a 2017-2019 report says terrain lost its
  lightmaps in VRChat; the community workaround was a material with
  Nature/Terrain/Standard as the Base Terrain Material, and grass was still
  reported unlit (black) in 2019; closed for age in 2022
  ([feedback.vrchat.com](https://feedback.vrchat.com/bug-reports/p/terrain-not-affected-by-lightmapping)).
  Current status UNPROVEN.
- **Quest performance of Unity Terrain (not VRChat-specific):** one
  developer measured "SImple 130x130 terrain with 3 textures and no
  vegetations eats -20 fps on Quest 2"
  ([Unity forum](https://discussions.unity.com/t/unity-terrain-performances-on-oculus-quest-2/818501));
  Meta forum advice is to convert terrains to meshes split into tiles
  ([Meta forum](https://communityforums.atmeta.com/discussions/dev-unity/unity-terrain-on-oculus-quest/939691)).
- **PC world size:** no official figure found; user posts cite about
  1.2 GB ([feedback.vrchat.com](https://feedback.vrchat.com/feature-requests/p/world-size-limit-increase-pc-mainly-for-larger-ce-creators)).
  UNPROVEN.
- **Poiyomi:** its decals are four UV-space slots on one material (position,
  scale and rotation "in UV space"; no projection mode)
  ([Poiyomi decals](https://www.poiyomi.com/color-and-normals/decals)). No
  Poiyomi terrain (splatmap) shader was found; a Poiyomi-lit splat terrain
  would be a custom shader (MicroSplat is a commercial replacement terrain
  shader system, [80.lv](https://80.lv/articles/microsplat-for-unity)).
- **Built-in Projector** works only in the Built-in RP, needs the
  Projector/Light or Projector/Multiply shaders from Standard Assets
  ([Projector manual](https://docs.unity3d.com/2022.3/Documentation/Manual/class-Projector.html)),
  and "will render all objects in its view frustum with the provided
  material" ([Projector API](https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Projector.html)),
  i.e. every object in a projector's frustum is drawn again (inference).
- **Occlusion culling:** Umbra, with Occluder/Occludee Static flags, baked
  ([Occlusion culling](https://docs.unity3d.com/2022.3/Documentation/Manual/OcclusionCulling.html));
  Umbra's own engineers called foliage poor occluders
  ([Unity blog, 2013](https://unity.com/blog/engine-platform/occlusion-culling-best-practices-unity-4-3)).
  Whether a Terrain is an Umbra occluder in 2022.3 is UNPROVEN here (proof: bake
  occlusion over a ridge with and without the Terrain marked static and compare
  the culled renderers in the Occlusion window).

---

## 4. Mesh terrain versus Unity Terrain

### 4.1 Costs today (measured, task-13 report and §1)

- Mesh terrain at full resolution: **32 768 triangles per chunk-block of
  4 × 4** (2 · 128²), i.e. 532 · 2048 = **1 089 536** on Queensdale, 466 944 on
  Lion's Arch, 512 000 on Spirit Vale. One 1024² baked albedo per 4 × 4 cell
  (256 texels per chunk), 76 MB of PNG on Queensdale.
- Props: block triangles median 243 k (Queensdale), 930 k (Lion's Arch),
  33 k (Spirit Vale); prop textures 1.3-3.6 GB on disk.

### 4.2 Pros and cons

| | mesh terrain (today) | Unity Terrain |
|---|---|---|
| shading | Poiyomi or Standard, same as props; lightmappable like any static mesh | Built-in terrain shader (Nature/Terrain/Standard) or a custom one; **not Poiyomi** |
| lighting in VRChat | normal static-mesh lightmaps | historical lightmap problems (§3), instanced details unlit by lightmaps/probes |
| LOD | none (stage B could add decimated meshes) | built-in heightfield LOD (Pixel Error), seams handled by neighbours |
| ground detail close up | 256 texels per chunk bake (blurry at walking height) | splat mode: real ground textures at full repeat, normal maps |
| foliage | prefab instances only | details (instanced, distance-culled) and trees |
| collider | MeshCollider on the same mesh: proven equal to `CollisionIndex` (32/32 rays) | TerrainCollider; its quad diagonal versus castlemist's NW-SE is UNPROVEN |
| editing in Unity | mesh | sculpt / paint tools work |
| Quest | fine, if region-limited | measured −20 fps for a tiny terrain on Quest 2 (forum), VRChat known issues |
| proven in this repo | yes (Unity 2022.3 batchmode, probe) | nothing yet |

### 4.3 Recommendation per target

- **PC VRChat:** keep mesh terrain as the default. Add Unity Terrain as an
  importer option and measure it on Queensdale. Its first stage is
  **"baked-layer" mode**: one TerrainLayer per tile whose diffuse is the cell's
  existing albedo PNG, `tileSize` = the tile's size, so one pass and exactly
  the current look, plus details and trees. Real splat layers ("splat mode")
  are a second stage, worth it only if the close-up ground matters (2-3 passes
  per 4 × 4 tile on Queensdale, §1.2).
- **Quest / Android:** mesh terrain, `--region` to a playable area,
  decimated (a 4 × 4 cell at 2 × 2-quad decimation is 8192 triangles), one
  1024² albedo per cell, foliage either off or a few hundred instanced type-3
  props. Unity Terrain only after a measured Quest build shows it beats the
  mesh.
- **Plain Unity:** Unity Terrain in splat mode with details and trees; the
  VRChat lighting caveats do not apply.

No performance numbers specific to VRChat PC terrain were found; the
importer should print triangle and draw counts per mode so the user can
compare.

---

## 5. Exact mapping: GW2 terrain → TerrainData

All quantities below are castlemist map units and map space (left-handed,
+X east, +Y north, up = −Z) unless they say Unity. `S` = `unityScale` (the
manifest setting, 0.0254 by default, UNPROVEN). Unity after the axis bake:
east +X, north +Z, up +Y; `map_to_unity(x, y, z) = (x, −z, y)`.

### 5.1 Tiles

- **One Terrain per 4 × 4-chunk block** (`cell_block_chunks`, `1/pickerScale`).
  128 segments → **`heightmapResolution = 129`** exactly, no resampling. It is
  also the cell the export already writes, and one blend page.
- **Size:** `size = (12288·S, (hMax − hMin)·S, 12288·S)`: 312.1152 m a side at
  `S = 0.0254`.
- **Edge blocks** (e.g. Queensdale's last row is 3 chunks tall, Spirit Vale's
  last column 2 wide): keep the full 129 grid, fill the missing samples by
  clamping the last real row/column, and set those quads to **holes**
  (`holesResolution = 128`, one per 96-unit quad: the padding is exactly
  representable). TerrainData's square resolution cannot express a 4 × 3 tile
  otherwise.
- Tile counts at 4 × 4: Queensdale 7 × 5 = 35, Lion's Arch 5 × 3 = 15,
  Spirit Vale 3 × 7 = 21 (the export's block counts).
- Larger tiles are possible but worse: 8 × 8 (257) gives 4 × the texture
  sets per tile; one 1025 tile covers Queensdale but would hold all 45
  textures (12 passes).

### 5.2 Samples and axes

Global sample `(I, J)`: `I` from the west edge, `J` from the north edge, chunk
`(cx, cy)` own sample `(i, j)` → `I = 32·cx + i`, `J = 32·cy + j`
(`TerrainChunk` row 0 = north, column 0 = west). Its map position is
`(rect[0] + 96·I, rect[3] − 96·J)`.

Tile `(tx, tyN)` (`tyN` counted from the north) holds `I ∈ [128·tx, 128·tx + 128]`,
`J ∈ [128·tyN, 128·tyN + 128]`. Unity's `heights[r, c]` has `c` along +X
(east) and `r` along +Z (north) from the tile's south-west corner (inference
from Unity's [y,x] convention and Terrain's origin at its min corner;
**the probe-style check in §8 must confirm it**). So:

```
c = I − 128·tx                    // 0 west .. 128 east
r = 128 − (J − 128·tyN)           // 0 south .. 128 north
unityHeight(I, J) = −z(I, J)      // up = −Z
heights[r, c] = (−z(I, J) − hMin) / (hMax − hMin)
tile position (Unity, before S) = (rect[0] + 12288·tx,  hMin,  rect[3] − 12288·(tyN + 1))
```

with `hMin = −max(stored z)` and `hMax = −min(stored z)` **over the whole
map**, so every tile shares one quantisation and shared edge samples are
bit-identical (the edges are already bit-identical in the data,
`gw2-world-frame.md` §3.1). Multiply positions and size by `S`.

### 5.3 Height range and precision

At 16 bits, one step is `range / 65535`: Queensdale 0.114 units, Lion's Arch
0.247, Spirit Vale 0.297, i.e. **at most 7.5 mm** at `S = 0.0254` (15 mm if the
store were effectively 15-bit). The horizontal spacing is 96 units (2.44 m),
so quantisation is negligible. If the 10000 / 4999.9995 values are sentinels
(§1.1) they should be excluded from the range (and the quads holed or
clamped) once their meaning is known; with them, the steps above hold.

### 5.4 Splat layers (splat mode)

- **Layers per tile** = the union of the tile's chunks' `textureFileIds`
  (6-12 on Queensdale, median 9 → 2-3 control maps, i.e. passes). One
  `TerrainLayer` asset per colour texture fileId, shared across tiles.
- **Alphamap = 512**, exactly the page's 128 texels per chunk. Each control
  texel takes the chunk's picker `w` (page sampled at its texel), mapped
  R → `colord`, G → `colorc`, B → `colorb`, A → `color`, zero for absent
  slots, renormalised to sum 1, then scattered into the tile's layer order.
  Chunks of one page never disagree at a seam because both halves come from
  the same page (the export's bake samples the page the same way).
  Whether Unity's control-texture texel `k` is centred at `(k + 0.5)/512` of
  the tile is UNPROVEN (a half-texel shift is 12 units).
- **What Built-in cannot express:** the per-texel height bias
  (`alpha + 0.01`, at the 8-repeat ground UV, far finer than 512² can store),
  `modx` brightness/saturation/rim, and the specular mask. Either accept the
  difference (plain linear blend) or write a custom terrain shader (§6.3). The
  baked-layer mode keeps all of it, because the bake does.
- **Diffuse alpha:** GW2's ground alpha is a blend height; Unity's built-in
  Standard terrain reads the diffuse alpha as **smoothness** (inference from
  the Standard terrain shader; UNPROVEN in docs). The exporter should write
  the layer PNGs as RGB with alpha 1, or alpha → the layer's smoothness
  deliberately.
- **Tiling:** `tileSize = 3072 / repeats` units (384 at the UNPROVEN 8),
  `× S` = 9.75 m. GW2's ground UV has `t` from the **north** edge, Unity's
  `v` runs north, so the texture lands mirrored north-south unless the
  exporter **flips the layer image vertically** (the importer does no frame
  maths). `tileOffset = 0` aligns the repeat with the chunk grid when tiles
  start on chunk boundaries (they do), since 3072 is a whole number of
  repeats.
- **Normals:** `normalFileIds[k]` → `normalMapTexture`, with the tangent
  sign UNPROVEN (the export's stage B carries the same flag).

### 5.5 Collider, water, everything else

- `TerrainCollider` per tile (no MeshCollider on the terrain in that mode);
  `havk` colliders unchanged. The triangle diagonal of Unity's collider
  versus `CollisionIndex`'s NW-SE split is UNPROVEN; the dat test's 32-ray
  check should be repeated against `Terrain.SampleHeight` and a downward
  raycast.
- Water, sky, light and props: unchanged.

---

## 6. Props: Terrain trees, details, or instances

### 6.1 Rules

| source | Unity target | why |
|---|---|---|
| `zon2` type 1 (grass, flowers; 10-280 tris, ≤ 149 units) | **detail**, `usePrototypeMesh`, `useInstancing`, `InstanceCountMode` | tiny, very many (167 k on Queensdale), cutout, distance-faded in GW2 at about 3000 units |
| `zon2` type 2 (plants; 10-122 tris, 36-189 units) | **detail**, same; a tree if the user wants colliders or longer draw distance | 40 k on Queensdale, faded at about 4000 |
| `zon2` type 3 (trees, shrubs, rocks; up to 3181 tris and 2119 units) | **Terrain tree**: prefab of the model's FBX **with a LODGroup** (LOD0 only now, GW2 index LODs later) | 10 k on Queensdale, fade up to 17 000; yaw-only rotation is all a scatter needs |
| `prp2` props, all groups | **GameObject instances** (today) | full rotation, bounds, `lod1/lod2`, animation clips, `havk` hulls, picking |
| `prp2` props on a VegPara AMAT, yaw-only, static, no hull | optional later: Terrain trees | only if instance counts prove a cost; see §6.2 |

**Detail grid.** A GW2 zone cell is 48 units, so a 12 288-unit tile is
**256 × 256 zone cells = `detailResolution` 256**, `detailResolutionPerPatch`
32 or 64 (divisors). One layer per (zone layer, model) pair, its count map
= number of placements of that model in each cell (0 or 1 today). Unity
positions instances inside each cell itself, which matches what castlemist
knows (cell, not offset).

**Tree instances.** `position` = the placement's (x, y) normalised to the
tile (inference: TreeInstance positions are tile-normalised, the docs do not
state units), height from the terrain; `rotation` and `widthScale/heightScale`
from `rotRange` / `scaleRange` once proven, else 0 and 1; `prototypeIndex` from
the model.

### 6.2 Heuristics for "is this prop a tree"

- **AMAT:** the model's material VS declares `VegPara` / `Wind` (§1.3). This
  is the only signal with code evidence. In the §6.4 sample, 17 of 118
  models (11 % of placements) use one.
- **Rotation:** `rot[0]`, `rot[1]` ≈ 0 (yaw-only), since `TreeInstance` has
  yaw only.
- **No collision hull and no clip:** a hull or an animation keeps it an
  instance.
- **Bounds:** extents and triangle counts like §1.3's type-3 row.
- **Counts:** only worth converting where a model is placed many times (the
  top 200 Queensdale models cover 3338 of the 6216 non-instance-array
  placements).

### 6.3 Wind and shaders

- Unity Wind Zones move Tree Editor trees and grass-mode details only (§2);
  GW2 meshes imported as prefabs will not sway. Sway needs a custom vertex
  shader using the GW2 vertex colour as the weight, or Poiyomi's vertex
  options if they suffice (UNPROVEN).
- Instanced details render with the prefab's material, so a Poiyomi cutout
  material works if it supports instancing (Poiyomi materials get
  `enableInstancing` today); but instanced details get **no lightmap or light
  probe lighting** (§2), which in a baked VRChat world means foliage lit
  differently from the ground. Accept, or use non-instanced vertex-lit
  details (which drop the Poiyomi material: "no other material properties
  are considered" for Grass mode).

### 6.4 Prop sample (Queensdale)

`gw2dat_cli model` on the 118 most-placed `prp2` models (non-instance-array
groups; 2758 of their 6216 placements), then `gw2dat_cli amat` on the 60
AMATs they use, checking each VS for `VegPara`:

| | models | placements | triangles (median) | largest extent (median, range) |
|---|---|---|---|---|
| uses a VegPara (wind) AMAT | 17 | 303 (11 %) | 1215 | 379 (81 - 1551) |
| other | 101 | 2455 | 284 | 128 |

Wind AMATs found: 19911, 47468, 56795, 57606, 57644, 57714, 57715, 73227,
75220, 75775 (56795, 57714, 73227 are also on zone type-3 models). The most
placed wind prop is 79699 (42 placements, 2413 triangles, 707 units, three
wind materials, a tree by every measure). So on Queensdale **trees placed as
props are a minority** (about 300 in this sample against 10 296 zone type-3
placements): keeping all `prp2` props as instances costs little, and the
foliage gain comes from `zon2`. The sample is the head of the distribution,
not the whole map.

---

## 7. Decal strategy

1. **`dcal` → mesh decals at export (recommended).** For each decal: take its
   oriented box (`position`, `extents`, `rotation`, rotation order as props'
   `rotY·rotX·rotZ`, UNPROVEN for decals), sample the terrain on a
   `gridSize`-spaced grid (16 units) inside the box's footprint with
   `terrain_height_at`, lift by `surfaceBias` along up, UV by box projection
   with `textureScaleUV0/OffsetUV0`, and write `Decal_k` into the cell glTF.
   Material: the decal AMAT's texture through the existing profile path;
   58246 takes the dark-decal rule (Fade, or Multiply where the shader has it).
   `propIds` (decals also on props) is stage 2: clip the props' triangles to
   the box. With Unity Terrain, the same heights feed both, so the decal sits
   on the terrain either way.
2. **Not Projectors:** each one redraws every object in its frustum (§3), needs
   Standard Assets shaders, and is not Poiyomi.
3. **Not Poiyomi decals:** they are UV-space on one material, not world
   projections (§3).
4. **Decal-like props** (flat road and dirt meshes, `prop-decal` /
   `decal-glow` materials, dark-decal props): stay props, as today. Z-fighting
   against a Unity Terrain (whose LOD moves the surface) is a risk the mesh
   terrain does not have; mitigations are a render-queue bump, a small depth
   offset, or a low Pixel Error near roads (UNPROVEN which is enough).

---

## 8. Pipeline proposal

### 8.1 world

1. **`ZoneScatter` in `WorldScene`** (new struct, `world/scatter.h`): layer
   defs (type, sizes, ranges, flags, all `modelArray` entries with their
   probability) and placements `(layer, model, cell, map xy, terrain z)`.
   Written fresh from the template and this note's measurements (§9 licence).
   Distinct models join `WorldScene::models` so the export's model loader
   handles them.
2. **`Decals` in `WorldScene`**: `dcal` as stored.
3. **`terrain_heightfield(terrain, block)`**: the 129² float grid of a block
   in the §5.2 order, with clamp padding and the padding mask.
4. **`terrain_splat(terrain, block, pages)`**: the block's layer list and 512²
   weights (§5.4), pure, testable against `terrain_blend`'s weights.

### 8.2 exportgltf (manifest v1, optional fields, no major bump)

```jsonc
"unityTerrain": {
  "tileChunks": 4, "heightmapResolution": 129, "alphamapResolution": 512,
  "detailResolution": 256, "heightRange": { "min": hMin, "max": hMax },
  "layers": [ { "fileId": 123, "albedo": "terrain/layers/123.png",      // RGB, flipped N-S
                "normal": "terrain/layers/123_n.png" | null, "tileSize": 384 } ],
  "tiles": [ { "cell": "c4_8_4x4", "position": [x, y, z], "size": [12288, h, 12288],   // Unity axes, before unityScale
               "heights": "terrain/c4_8_4x4.r16",            // 129*129 uint16 LE, row 0 = south, column 0 = west
               "holes": "terrain/c4_8_4x4_holes.png" | null,  // 128x128, white = surface
               "layers": [0, 3, 7, ...], "splat": ["terrain/c4_8_4x4_splat0.png", ...],   // RGBA8, 4 layers each
               "bakedLayer": "cells/c4_8_4x4_albedo.png",
               "details": [ { "prototype": 0, "counts": "terrain/c4_8_4x4_d0.png" } ],
               "trees": [ { "prototype": 2, "p": [u, v], "yaw": 0, "scale": [1, 1] } ] } ],
  "detailPrototypes": [ { "model": 17, "zoneType": 1, "fadeFar": 3000 } ],
  "treePrototypes":   [ { "model": 42, "zoneType": 3, "fadeFar": 6000 } ]
}
```

- **RAW, not 16-bit PNG, for heights:** Unity's `Texture2D.LoadImage` is not
  documented to keep 16 bits per channel (UNPROVEN), while a raw file is read
  with `File.ReadAllBytes` and is Unity's own RAW format. Row order and
  normalisation are the exporter's (§5.2), so the importer does no frame
  maths, as D6 requires.
- Splat, holes and count maps are PNG (8 bits are enough), written north-up
  (row 0 = north), which `Texture2D.GetPixels` returns south-first, matching
  `[y,x]` (inference; the frame check below proves it).
- Settings rows (`settings` block) for every UNPROVEN value: `groundRepeats`,
  `zoneCellUnits` 48, `zoneLayerTypeMeaning`, `scatterJitter: none`,
  `terrainDiagonal`, `heightSentinels`.
- `validate_manifest.py` gains the block's schema and file checks.

### 8.3 Unity importer

1. Window option **Terrain: Mesh / Unity Terrain (baked layer) / Unity
   Terrain (splat)**, **Foliage: off / details only / details + trees**.
2. Per tile: `TerrainData` asset (`heightmapResolution`, `size` × `S`,
   `SetHeights` from the RAW, `SetHoles`, `terrainLayers`,
   `alphamapResolution`, `SetAlphamaps`, `SetDetailResolution(256, 64)`,
   `detailPrototypes`, `SetDetailLayer`, `treePrototypes`, `treeInstances`),
   `Terrain` + `TerrainCollider`, `materialTemplate` = Nature/Terrain/Standard
   (the community lightmap workaround, §3), `SetNeighbors` on every tile.
3. Terrain GameObjects under an **unscaled** sibling root (`GW2_<id>_Terrain`)
   with sizes and positions already multiplied by `S`, because Terrain's
   handling of a scaled parent is UNPROVEN (§2). In mesh mode nothing changes.
4. Prototype prefabs: the existing model prefabs, wrapped with a LODGroup for
   trees.
5. **Terrain frame check** (like the frame probe): sample
   `Terrain.SampleHeight` at the 32 points the dat test uses and compare with
   the manifest's expected Unity heights; log an error naming the axis on a
   mismatch (catches a flipped row order).
6. Summary line gains `terrains= layers= detailPrototypes= details= trees=`.

### 8.4 Order of work

1. Heightfield + baked layer + TerrainCollider + frame check (no new GW2
   research needed).
2. `zon2` decoder in `world` and foliage as details / trees, density per cell.
3. `dcal` mesh decals.
4. Splat mode (layers, control maps, flipped layer PNGs).
5. Research items, each a small note amendment: zone jitter/RNG, foliage wind
   VS, `tileFlagArray`, `uvScale`, sentinels.

### 8.5 Risks and unknowns

| risk | status |
|---|---|
| zone scatter exact positions, rotation, scale, probability, RNG | UNPROVEN; density per cell only |
| zone layer `type` meaning (detail vs tree split) | UNPROVEN; split by measured size |
| `flags` low nibble meaning | UNPROVEN |
| foliage wind formula | UNPROVEN (uniforms proven present) |
| ground repeat (8) | UNPROVEN, inherited |
| splat height-bias and `modx` lost in Built-in splat | known loss; baked-layer mode avoids it |
| heightmap row order / alphamap texel centre in Unity | UNPROVEN until the terrain frame check passes |
| Unity heightmap bit depth | UNPROVEN (16 assumed; 15 also fine) |
| Terrain under a scaled parent | UNPROVEN; avoided by design |
| TerrainCollider diagonal vs `CollisionIndex` | UNPROVEN; re-run the 32-ray test |
| VRChat avatar-load stutter with Terrain, lightmaps on terrain and details | reported (2023, 2017-19); current status UNPROVEN |
| instanced details unlit by lightmaps / probes | documented Unity limitation |
| holes in GW2 data (`tileFlagArray`, `chunkFlags`) | UNPROVEN; padding holes only |
| height sentinel values 10000 / 4999.9995 | UNPROVEN |
| Quest viability of foliage at GW2 density (217 k on Queensdale) | not viable in full (inference from 250 k triangle budget) |

---

## 9. Prior art and licences

- **Tyria3D / T3D** (GPL-3): the reference for GW2 terrain, zones and props.
  Cite, never copy. Note: `gw2model.hpp`'s `parseMapZones` comment says it was
  "Ported from Tyria3D's ZoneRenderer". **If that port follows T3D's code
  rather than only its behaviour, castlemist may already contain GPL-derived
  code**; this is for the user to review. A new `world` decoder should be
  written from the template and the data, not from that function or T3D.
- **Mesh → heightmap** (raycast sampling, e.g. the Unity-forum
  "Object2Terrain" style scripts,
  [discussions.unity.com](https://discussions.unity.com/t/how-can-i-turn-multiple-objects-into-a-single-terrain/118635)):
  not needed, because GW2 stores the heightfield itself. Licences of those
  scripts are not stated.
- **Terrain → mesh** for Quest: unitycoder's FastTerrainToMesh ("free to use
  for any purpose, including commercial projects",
  [github.com/unitycoder/FastTerrainToMesh](https://github.com/unitycoder/FastTerrainToMesh));
  the commercial "Terrain to Mesh" asset. castlemist already has the mesh
  path, so neither is needed.
- **Splat shaders:** MicroSplat (commercial); none Poiyomi-based found.
- **VRChat lighting helpers:** VRC-Bakery-Adapter (Bakery directional
  lightmaps in VRChat, [github.com/MerlinVR/VRC-Bakery-Adapter](https://github.com/MerlinVR/VRC-Bakery-Adapter));
  its terrain support is UNPROVEN.

---

## 10. Questions only the user can answer

1. **Quest/Android**: is it a target? It decides mesh-only, region size and
   whether foliage is exported at all.
2. **Fidelity**: is the 256-texel-per-chunk bake enough at walking height,
   or is close-up ground detail (splat mode, 2-3 passes, no Poiyomi on the
   ground) wanted?
3. **Foliage density**: all of GW2's (217 k placements on Queensdale), a
   thinned share, or trees only?
4. **Shader on the ground**: is a non-Poiyomi terrain shader acceptable
   (Nature/Terrain/Standard, or a bought one such as MicroSplat), or must
   everything be Poiyomi (which means staying on mesh terrain)?
5. **Wind**: do trees and grass need to sway (a custom vertex shader)?
6. **Licence**: review `parseMapZones`' Tyria3D port before anything builds on
   it?
7. **Both modes in one scene**: keep the mesh terrain as a fallback alongside
   Unity Terrain (doubles the import, lets the user switch), or one only?
