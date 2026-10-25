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

### 1.3 Foliage and ground clutter: `zon2` — decoded (client decode) and exported as foliage records

**GW2 has a dedicated scatter layer.** `zon2` (`PackMapZonesV23`, read as v22
on Queensdale) holds `zoneDefArray` (each def: `token`, `layerDefArray`) and
`zoneArray` (each zone: `defToken`, `vertRect` (a cell rectangle), `seed`,
`range`, `zPos`, `flags[]`, `encodeData`, `collideData`, `vertices`). A layer
def has `type, width, height, radiusGround, sortGroup, tiling, scaleRange,
probability, fadeRange, rotRange[3], hslRanges[4], instanceScaleJitter,
noise, layerFlags, materialname` and `modelArray[]` (`filename, probability,
flags, hslOffset, zOffsets, permutation`).

> **Corrected 2026-10-18 (world-foliage Task 16).** This section's first
> version described castlemist's original walk of `flags[]`, which was wrong.
> The client stores `flags` as run-length **(count, value)** pairs (`count`
> cells of `value`), and as raw bytes, one per vertex, in a zone with no
> `encodeData` (34 zones). castlemist now uses that client decode
> (`walkZoneFlags`, ruling S4): layer = `(flag >> 4) - 1`, model =
> `layer.modelArray[(flag >> 1) & 7]`, the cell is the walk's position minus
> one. `WorldScene::zones` holds the result, the Map Window draws it, and the
> world export writes it as foliage records. The spec is
> `docs/superpowers/specs/2026-10-16-world-foliage-design.md`, with the
> corrections in M1-M2 and the amendment of 2026-10-18. The rule that places each
> instance is the dated amendment below.

What castlemist did before (kept for the record, **superseded**): it walked
`flags[]` as `(skip, flag)` byte pairs over the zone's cells on a 48-unit grid
from the `parm` rect corner, planted one instance per non-zero flag with the
layer's `modelArray[0]` only, unrotated at scale 1 (ported from Tyria3D's
ZoneRenderer; see §9). `load_world` did not attach it.

The 48-unit cell is consistent with the data: Queensdale is 896 terrain tiles
× 96 = 86 016 units = **1792 × 48**, and the largest `vertRect` x seen is
1620. (48 = half a height sample; the constant itself is T3D's.)

Measured placements (the client decode, counted per layer `type`; the old
walk gave 217 114 / 32 448 / 7 642 in total):

| map | zone defs / layers | zones | placements | type 1 | type 2 | type 3 |
|---|---|---|---|---|---|---|
| Queensdale | 42 / 142 | 248 | **247 804** | 193 125 | 44 380 | 10 299 |
| Lion's Arch | 26 / 60 | 447 | 40 252 | 36 939 | 2 300 | 1 013 |
| Spirit Vale | 2 / 3 | 26 | 8 670 | 8 386 | 0 | 284 |

The low nibble of each flag holds the model slot in bits 1-3, `(flag >> 1) &
7` (measured, spec M3, and read from the client); bit 0 is stored and not used
(**UNPROVEN** meaning, spec M4).

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

Superseded: the per-instance offset, rotation, scale, RNG and seeding, ground
alignment and `zOffsets`, which this section first listed as unreproduced, are
the scatter rule below (type 3 reproduced exactly on all 4750 colliders;
types 1-2 transcribed and UNPROVEN). `noise` and the `hslRanges` tint are not
applied (spec D11).

#### 2026-10-16: zone scatter rule (static client and data fit)

(Written against the old walk's counts; the decode it asks for is the one
described above. The query flags are taken as 0, an unfiltered query: every
instance emitted, transcribed and not measured; spec setting `zoneQueryFlags`.)

**Decision for the World Foliage spec D5: `exact`.** The rule below, read
from `Gw2-64.exe` (PE timestamp 1792004482, the disable-ASLR copy, image base
`0x140000000`) and run against castlemist's own terrain, reproduces **all 4750**
anchor-matched `havk.zoneModels` colliders (Queensdale 4378, Lion's Arch 88,
Spirit Vale 284) within D5 step 2's tolerances:

| component | pass | worst error | tolerance |
|---|---|---|---|
| x, y | 4750 / 4750 | 0.00195 | 0.01 |
| yaw | 4750 / 4750 | 5e-9 rad | 1e-4 |
| tilt (x, y) | 4750 / 4750 | 2.4e-7 rad | 1e-4 |
| scale | 4750 / 4750 | 4.8e-9 | 1e-5 |
| z | 4750 / 4750 | 0.0092 | 0.5 |

Queensdale's three "hand-placed" entries (3350-3352, scale 0.84, spec M2) are
excluded from the count, as D5 says. They are **not** hand-placed: they are the
three type-3 cells of zone 135, a zone stored without `encodeData`, which the
castlemist walk misreads (see the decode below). The rule gives their x, y, yaw
and scale exactly (tilt and z not checked: no terrain stencil was dumped for
them):

| entry | x, y | yaw | scale | model |
|---|---|---|---|---|
| 3350 | 17171.012, 3269.344 | 0.80742 | 0.84 | 85090 |
| 3351 | 17184.000, 3648.000 | 5.68292 | 0.84 | 85933 |
| 3352 | 17171.713, 4041.689 | 1.97905 | 0.84 | 85090 |

What the fit does **not** cover: the type-1/2 path (no type-1/2 placement has
a collider), the graphics-quality input, the zone transform, the clamp, the
water rule and the random-z rule (none of them is exercised by the 4750). Those
parts are transcribed from the code and are UNPROVEN by data. The spec's
`zoneScatter = exact` holds for type 3. For types 1-2 it is the transcribed
rule, labelled as such.

**How it was found.** No IDB or analysed Ghidra project existed for this build,
and `~/gw2re.gpr` holds the September build. A scratch PyGhidra project
(Ghidra 12.1.3, JDK 21, imported with no auto-analysis) was used, and only the
functions below were decompiled. The ways in:
- the source-path strings `...\Engine\Map\Zones\ZnPlacement.cpp`, `ZnZone.cpp`,
  `ZnChunk.cpp`, `ZnData.cpp`, `ZnCompress.cpp` and the asserts beside them
  (`fillCount < MAX_TILING`, `queryFlags == 0 || (noGroundcover != onlyGroundcover)`,
  `collideData`);
- their RIP-relative xrefs;
- `.pdata` to find the function roots.

The game was never launched, attached to or hooked.

| address | file | what it does |
|---|---|---|
| `0x140c8a440` | ZnContext | query path: for each zone and each 32 × 32-vertex chunk in a rect, calls `0x140cc23c0`; clamp rect = map rect inset by 1 unit, matrix from the map |
| `0x140cc23c0` | ZnPlacement | per chunk: decodes the rows, seeds the RNG per cell, picks the collideData entry, dispatches on flag bit 0 |
| `0x140cc1f50` | ZnPlacement | **single-instance path** (flag bit 0 clear: type 3) |
| `0x140cc1900` | ZnPlacement | **tiling path** (flag bit 0 set: types 1-2), up to 4 instances per cell |
| `0x140cc2880` | ZnPlacement | xy jitter, clamp, ground height, z rules |
| `0x140cc2af0` | ZnPlacement | yaw, random tilt or ground-normal tilt |
| `0x140cbc110` | (by position, ZnModel) | per-slot z offset from the `zOffsets` bytes |
| `0x140cbc210` | (by position, ZnModel) | optional `subModel` second instance |
| `0x140cbc0d0` / `0x140cbc0b0` / `0x140cbc090` | (by position, ZnModel) | tiling count by quality; fadeRange × {0.85, 1, 1.5, 2}[quality] |
| `0x140cd0660` / `0x140cd0410` | ZnCompress | row decode of `flags` (raw, or RLE through `encodeData`) |
| `0x140cc80d0`, `0x140cc8150` | ZnZone | vertex index `(x1−x0)(row−y0) + col − x0`; vertex position `(48 col + ox, 48 row + oy, 0)` |
| `0x140c8b9b0`, `0x140c8bb60` | ZnData | pack → runtime layer and model; model flag bits remapped (table `0x141d6e110` → `0x141d6e170`) |
| `0x140e23f90` / `0x140e24cf0` | | RNG seed |
| `0x140e24b10` / `0x140e243d0` / `0x140e24280` | | RNG int range / float ×2⁻³¹ / float ×4.6566126e-10 |
| `0x1409bf1f0` | | round to nearest even (magic 1.75·2²³) |
| `0x142592280` | .data | 256-entry uint32 seed table (count `0x100` at `0x142592680`) |
| `0x141e44210` / `0x141e443a0` / `0x141e44600` | .rdata | tiling offset patterns: 25 × {2, 3, 4} float2 |
| `0x141e449c4` | .rdata | scale step 0.07734375 (= 99/1280) |

Tried and not it: the vtable `0x141d62630` (TrnQueryBasic). Its slots +0x58 and
+0x68 are not the zone ground query. The ground query's implementation was
**not located**. Its behaviour is reproduced by the measured terrain rule below.

**The `flags` decode was wrong in castlemist (phase 1).** `0x140cd0410` reads
`flags` as **(count, value) run-length pairs: `count` cells of `value`**. It is
not "skip `count`, then one flag". A zone with no `encodeData` stores `flags`
raw, one byte per vertex (`0x140cd0660`; `flags.size() == w·h` on all 34 such
zones). Two checks hold on all zones:
- `encodeData[r] = {index, offset}` is the pair holding row r's first cell and
  the count already used from it: `Σ count[<index] + offset = r·w` on 687 of
  687 encoded zones.
- `offsetData[i]` is the first `collideData` index of the zone's i-th 32 × 32
  chunk: 53 of 53 zones.

With the client decode, the true placement counts are:

| map | placements | type 1 / 2 / 3 | old walk |
|---|---|---|---|
| Queensdale | 247 804 | 193 125 / 44 380 / 10 299 | 217 114 |
| Lion's Arch | 40 252 | 36 939 / 2 300 / 1 013 | 32 448 |
| Spirit Vale | 8 670 | 8 386 / 0 / 284 | 7 642 |

The old walk's `k` is the cell *after* a run. So a run of one cell (all 4750
collided cells) sits at `k − 1`, which is spec M2's `c − 1` (U4 answered: the
walk position is `k − 1`, the run's last cell). Runs longer than one and raw
zones lost cells. On Queensdale, the type-3 cells of the 45 collided models are
4381 under the true decode, exactly the 4381 colliders (spec M4 holds).

**Cell and position.** Cell `p` of a zone is vertex `(col, row) = (vr0 + p mod
w, vr1 + ⌊p / w⌋)` and sits at `P = (x0 + 48·col, y0 + 48·row)`, with `(x0, y0)`
the parm rect's south-west corner. The context matrix is identity on the test
maps (not exercised otherwise).

**The flag byte.** Layer `(f >> 4) − 1`, model slot `(f >> 1) & 7` (spec M3,
now read in the code). Bit 0 selects the path: 1 = tiling (`0x140cc1900`),
0 = single (`0x140cc1f50`). Under the true decode bit 0 is set on all type-1/2
cells and clear on all type-3 cells.

**RNG** (`0x140e24b10` family; Park-Miller "minimal standard" 48271 by Schrage,
with the client's own fix-up):
```
seed(s):  state = s ? s : 0x075BD924
next():   state = state*48271 − (state/44488)*0x7FFFFFFF      (uint32 wrap)
          if state > 0x7FFFFFFF: state += 0x80000000           (uint32 wrap)
          if state == 0: state = 0x075BD924
fA() = float(next()) * 2^-31            (0x140e243d0, constant 0x30000000)
fB() = float(next()) * 4.6566126e-10    (0x140e24280, constant 0x2FFFFFFF)
irange(a, b) = a + next() % (b − a + 1) (0x140e24b10)
```
All float arithmetic is single precision, one rounding per operation, with no
FMA.

**Seed per cell** (`0x140cc23c0`): with `lx = col − vr0`, `ly = row − vr1`
and `w = vr2 − vr0`,
`seed = (zone.seed << 8) ^ T[(lx + w·ly + lx·ly) mod 256] ^ (((4·ly) ^ lx) << 14)`,
where T is the table at `0x142592280`. The generator is re-seeded for every
non-empty cell.

**Model flags** are remapped at load (`0x140c8b9b0`). The pack bits that matter
here, with their runtime bit in brackets:
- `0x1` (r0x1): no xy jitter.
- `0x2` (r0x2), `0x200000` (r0x4), `0x4` (r0x8): align to the ground. `0x200000`
  also snaps by 90°. `0x4` also scales the tilt by 0.35.
- `0x8` (r0x10), `0x20000` (r0x20): random tilt from `rotRange`. `0x20000` also
  turns off the random sign.
- `0x10` with `0x800` (r0x4040): random z.
- `0x20` (r0x80): rot.x = π.
- `0x200` (r0x100): yaw from the normal.
- `0x1000` (r0x8000): water test.
- `0x800000` (r0x100000): skipped at quality 0 in the tiling path.

The collided models on the test maps carry these pack flags: `0x100000`,
`0x100002`, `0x100004`, `0x100005`, `0x100008`, `0x100084`, `0x120000`, `0x2000`,
`0x2004`, `0x2082` and `0x2084`.

**Single-instance path, draw order** (`0x140cc1f50`; this is what the 4750
validate):
1. z offset (`0x140cbc110`): `r = fA()`;
   `zoff = 36 − ((z0 + r·(z1 − z0)) / 255)·72`, where z0 and z1 are the slot's
   `zOffsets` bytes. 128 / 128 gives −0.14.
2. Jitter, unless pack flag `0x1`: `x = P.x + (24 − fB()·48)`, then
   `y = P.y + (24 − fB()·48)`. So u and v lie in (−24, 24].
3. Clamp x, y to the context rect. On the `0x140c8a440` path this is the map
   rect inset by 1 unit.
4. Ground height h:
   - zoneFlags bit 1 (`collideData`): `h = collideData[i].zPos`.
   - Otherwise, the terrain under (x, y) on the triangle split NE-SW: plane
     (NW, NE, SW) when `fx + fy ≤ 1`, else (NE, SW, SE). Here fx counts east
     and fy south within the quad of 96-unit samples.
   - Pack flag `0x1000` (water test): when the ground value exceeds the water
     query's value (as stored, Z down), the water value is used, and the two
     cases below are skipped.
   - The r0x4040 case: `h = max(fB()·h, 120)`.

   Then `z = h + zoff`.
5. Rotation (`0x140cc2af0`):
   - Start with rot = (π if pack `0x20`, 0, 0).
   - **Yaw** `rot.z = (rotRange.z0 + fB()·(z1 − z0))·(π/180)`.
   - If random tilt: `rot.x = (x0 + fB()·(x1 − x0) + rot.x)·π/180`, negated if
     `fB() > 0.5` (that sign draw only without `0x20000`). Then `rot.y` the same
     way from rotRange.y.
   - Else, if ground-aligned: take the ground normal n and set
     `rot.x = −atan2(ny, √(nz² + nx²))` and `rot.y = atan2(nx, √(nz² + ny²))`.
     - n = `(nx, ny, √(1 − nx² − ny²))` from `collideData[i]` under zoneFlags
       bit 1.
     - Otherwise n is the terrain plane chosen by **fx ≤ fy: (NW, NE, SW), else
       (NE, SW, SE)**. This is not the triangle the height uses. It is measured
       (3537 / 3537 at ≤ 6.5e-8 rad); the query function was not found.
     - Pack `0x200` sets `rot.z = −atan2(ny, nx) − π/2`.
     - Pack `0x200000` adds ±π/2 to rot.x (if |nx| ≤ |ny|) or to rot.y.
     - Pack `0x4` multiplies rot.x and rot.y by 0.35.
6. **Scale:** `s = s0 + fA()·(s1 − s0)`, then
   `s = s0 + roundEven((s − s0)/Δ)·Δ` with `Δ = 0.07734375`. A single global
   step, not per layer. This explains spec M5's out-of-range scales: 122 of the
   4750 (107 on Queensdale) round past s1, and none falls below s0.
7. `subModel` (`0x140cbc210`): if the layer has one, `r = fA()`. When `r ≤`
   the subModel's probability, a second instance of the subModel's model is
   made with the same transform.

**collideData index** (zoneFlags bit 1): `i = offsetData[chunk] + n`. Here
`chunk = (col>>5 − vr0>>5) + (row>>5 − vr1>>5)·(((vr2−1)>>5) + 1 − vr0>>5)`,
and n is the cell's order among the chunk's non-empty cells, rows then columns.
215 / 215 samples give z to 6.5e-5 and 53 / 53 normals to 1.4e-8 rad.

**Tiling path** (`0x140cc1900`, types 1-2). Transcribed only; no collider
validates it:
1. `zoff` as in step 1, once per cell.
2. Skip the cell if pack `0x800000` and quality 0.
3. `pattern = irange(0, 24)`.
4. `count = irange(1, T)`. T comes from `tiling` (`0x140cbc0d0`): quality 2
   gives `tiling`; quality 1 gives `tiling`, or 3 for 4; quality 0 gives
   `tiling − 1`, or 2 for 4. A `tiling` of 0 or 1 gives `tiling` at any quality.
5. For each instance k < count, the base point is `P + (24, 24) + offset`:
   - offset = (0, 0) for count 1;
   - otherwise entry k of pattern `pattern` in the table for count 2, 3 or 4,
     and then there is no jitter.

   Then steps 2-6 above run per instance. There is no subModel on this path.

The quality input comes from a getter (`0x140c58c10`, not read); castlemist
should take 2 (full tiling). Note the extra (24, 24): groundcover is centred on
the cell between four vertices, and trees on the vertex.

**Not used by placement** (no read in the two expansion paths): the layer and
model `probability` (`zoneProbability`: the flag already names the model, so
the draw is offline), `instanceScaleJitter`, `noise`, `hslRanges` / `hslOffset`
and `radiusGround`. `fadeRange` is scaled by the quality factor at chunk build.
Their render-time meaning stays UNPROVEN.

**D5 step 3 hypotheses** (`world_dat.zones_hypotheses`, all 4750 unless noted):

| hypothesis | result | status |
|---|---|---|
| tilt = terrain normal, clamped to `rotRange` x/y | 234 / 4750, worst 2.26 rad | UNPROVEN as phrased (tilt is per model flag: none 743, random 417 + 125, ground 3537 + 53) |
| z = `terrain_height_at` | 2286 / 4750 within 0.5 | UNPROVEN as phrased (z = NE-SW triangle + zOffset: 4535 / 4535, worst 0.0093) |

**Amendment 2026-10-18 (world-foliage fix wave, review I2).** castlemist's own
terrain now splits NE-SW everywhere: `CollisionIndex` (walk.cpp), the mesh
export (world_cell.cpp `push_quad`) and the Map Window (world_view.cpp
`grid_indices`). It used NW-SE, chosen only to agree with itself (world-export
spec, Task 13). The NE-SW split is the client's ground query as measured: the
z of the client's own collider placements, which `ZnPlacement` (`0x140cc2880`,
Gw2-64.exe 2026-10-14) takes from that query, sits on the NE-SW triangle plus
the slot's zOffset for 4535 / 4535. The query's code is still not located, and
the renderer's own terrain index buffer was not read, so the split is
*derived*, not read. `world_dat.zones_collision_terrain_is_client_ground` checks
that `CollisionIndex`'s terrain equals the NE-SW triangle at every zone instance
(Queensdale: 4381 colliders, previously 479 more than 1 unit off, 243 of
them floating; now 0, worst 0.001 units on all three test maps).
| scale = `scaleRange[0] + Δ·k` | 4750 / 4750, Δ = 99/1280 global | **measured** (and read in the code) |
| `collideData` = the ground (normal, z) | z 215 / 215, normal 53 / 53 | **measured** |

**Proven now.** The client uses the anchor and slot rule. The anchor is the
decoded cell's vertex, the slot is `(f >> 1) & 7`, and the layer is
`(f >> 4) − 1`. The rule is read in the code and reproduces all 4750
colliders. U3 (tilt is ground alignment) is answered: only for ground-aligned
models.

Scratch scripts and dumps for this amendment are not committed (the spec's
rule): `sim.py`, `validate.py`, `fitn*.py`, `zones.py`, and the dumps from
`CM_ZONE_SAMPLES` (`*.csv`, `*.terrain.csv`, `*.zones.csv`, `*.layers.csv`,
`*.unmatched.csv`).

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
| foliage wind | foliage AMAT VS (`Wind`, `WindGust`, `VegPara`, vertex `COLOR`) | read 2026-10-23: two variants, grass (77876, 189570: xy sway by COLOR.a + an engine noise texture) and tree (56795, 69668, 47468, 57606, 78966: COLOR R/G/B/A branch, phase, edge, bend; VegPara = the material's grvegp); transcribed in GW2Core.cginc (_VegMode) | trace the client's Wind / WindGust from PackMapEnvDataWind (azimuth, elevation, noise, speed, gust, gustFreq, gustSpeed; exported raw as environment.wind) and the noise texture |
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
| collider | MeshCollider on the same mesh: proven equal to `CollisionIndex` (32/32 rays) | TerrainCollider; its quad diagonal versus castlemist's NE-SW is UNPROVEN |
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
  versus `CollisionIndex`'s NE-SW split is UNPROVEN; the dat test's 32-ray
  check should be repeated against `Terrain.SampleHeight` and a downward
  raycast.
- Water, sky, light and props: unchanged.

---

## 6. Props: Terrain trees, details, or instances

> World-foliage status (2026-10-18): the Terrain-tree / detail / instance split
> in this section is built (spec D7-D8; amendment 2026-10-18 in
> `docs/superpowers/specs/2026-10-16-world-foliage-design.md`).

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

> Built as the world-foliage work (sub-project 5); see the spec and its
> amendments (`docs/superpowers/specs/2026-10-16-world-foliage-design.md`,
> 2026-10-18 Task 16) for what changed from this proposal.

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
