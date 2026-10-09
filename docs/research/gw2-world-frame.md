---
name: gw2-world-frame
description: "The frame WorldScene keeps a GW2 map in (map space as stored): axes, handedness, Z sign, units, map bounds, the terrain chunk layout, terrain materials and blend pages, prop and collision placements, and the exact map -> Unity / map -> Blender conversions. Source of truth for castlemist::world and every later world export; anything unproven is marked UNPROVEN."
---

# GW2 world frame: axes, units, bounds

Research for the World Engine data core
(spec `docs/superpowers/specs/2026-10-08-world-data-core-design.md`,
plan `docs/superpowers/plans/2026-10-08-world-data-core.md`, Task 3). Done
2026-10-08. `WorldScene` keeps everything in **map space as stored**; this
note proves what that space is and fixes the conversions in
`include/castlemist/world/frame.h` / `src/world/frame.cpp`. Later sections
(terrain, props, collision, water) are added by later tasks.

## Evidence base

- **Test maps:** Queensdale 192711, Lion's Arch 191000, Spirit Vale 1151420.
- **castlemist's own parser:** `gw2dat_cli map --file-id 192711` (template
  `dumps/packfile/gw2_packfile.json`).
- **T3D references:** `tests/world_ref/<id>.json`, made by
  `tools/world/t3d_reference.mjs` from T3D commit `b3428b2`
  (conventions in `tools/world/README.md`).
- **Signed prop/terrain statistics:** a throwaway copy of
  `t3d_reference.mjs` (scratchpad, not committed) that additionally prints, per
  map, the signed gap `prop z - terrain h` over every `propArray` prop (h is
  T3D's terrain height **as stored** under the prop's x, y), the same gap with
  h negated, and the sign split of every stored `trn.heightMapArray` value.
- **Executable:** `Gw2-64-disable-aslr.exe` (the build `gw2-sky.md` used,
  imagebase `0x140000000`), `objdump -d -M intel`, RIP-relative xrefs found
  by scanning `.text` for displacements.

---

## 1. Axes, handedness, Z sign, units

**Map space is the same frame as the sky space of `gw2-sky.md` §1:
left-handed, +X = east, +Y = north, +Z = down (up = −Z). Units per metre are
UNPROVEN.**

### 1.1 Up is −Z

Four independent lines of evidence, two of them from data alone:

1. *Props stand in open air on the −Z side of the terrain (data, castlemist's
   parser).* `gw2dat_cli map --file-id 192711` reports the stored terrain
   heights in `[−5768.9, +1701.5]` and the prop positions' z in
   `[−11748.5, +1807.8]`. Props reach 5980 units past the terrain on the −Z
   side but only 106 units past it on the +Z side. Things built on a
   landscape tower above it (trees, buildings, vista props) and are not buried
   deep beneath its lowest point, so −Z is up. With +Z up, props would reach
   about 6000 units below the deepest point of the terrain, and only 106 units
   above its highest.
2. *Signed prop-to-terrain gap (data).* Over every `propArray` prop with
   terrain under it:

   | map | props | median abs(z − h) | median abs(z + h) | (z − h) p10 | p25 | median | p75 | p90 |
   |---|---|---|---|---|---|---|---|---|
   | Queensdale 192711 | 5918 | **32.8** | 2482.9 | −430.6 | −71.7 | 0.2 | 20.3 | 117.6 |
   | Lion's Arch 191000 | 7190 | **88.6** | 1745.0 | −1306.0 | −411.3 | −39.2 | 1.6 | 83.4 |
   | Spirit Vale 1151420 | 1026 | 787.1 | 3808.0 | −3552.8 | −2304.3 | −458.4 | 8.0 | 590.9 |

   - The stored heights and the stored prop z are in **the same sign**:
     `|z − h|` is 33 / 89 against 2483 / 1745 for `|z + h|`. No negation is
     needed between `trn.heightMapArray` and prop positions. (Spirit Vale's
     T3D terrain placement is suspect, Task 2. It still favours `z − h` by 5x.)
   - The gap's tail is one-sided: props that are far from the ground sit at
     **smaller z** than the terrain under them (p10 −431 and −1306, p90 +118
     and +83). Props on bridges, roofs, upper floors and ledges are above the
     ground they overhang. So smaller z is higher: up = −Z. (Lion's Arch, a
     city of raised walkways and buildings, has the longest tail.)
3. *Water level against the terrain (data).* The `havk` water surface is
   `waterSurfaceZ = 0` on Queensdale and Lion's Arch (`gw2dat_cli map`
   `collision.waterZ`; `water.havkWaterSurfaceZ` in both references). Sign
   split of every stored height sample:

   | map | samples | h > 0 | h < 0 |
   |---|---|---|---|
   | Queensdale 192711 | 651 700 | 9.8 % | 90.2 % |
   | Lion's Arch 191000 | 279 300 | 25.2 % | 74.8 % |
   | Spirit Vale 1151420 (no water) | 306 250 | 49.1 % | 50.9 % |

   With up = −Z, 90 % of Queensdale (a land zone with lakes) is above its water, and 25 % of Lion's Arch (a city around a bay
   that opens to the sea) is sea floor. With +Z up, 90 % of Queensdale would
   be under water.
4. *Game code (sky).* `gw2-sky.md` §1 proves −Z up for the sky geometry and
   shaders. It also shows the sky is drawn with World =
   translate `(p.x, p.y, −verticalOffset)` × scale 100, **with no rotation**
   (`0x140c6e19c..0x140c6e1d4`), where `p` is a world position. The sky's
   axes are therefore the world's axes, so the world also has up = −Z.
   `MapEnvRig.sunDir` ("−Z is up", `include/castlemist/extract/map_types.h:37`)
   agrees.
5. *T3D.* T3D draws a map-space point `(x, y, z)` at three.js
   `(x, −z, −y)` (`PropertiesRenderer.ts:202`, `HavokRenderer.ts:249`;
   `tools/world/README.md`). three.js is +Y up, so T3D's up is map −Z. T3D
   "negates terrain heights" (`Y = −h`, `TerrainRenderer.ts:456`) only
   because it applies the same `Y = −z` to heights: heights are map-space z,
   as item 2 measures.

**Resolving the "Z-up" comments.** `MapCollision::verts` "(Z-up, world
space)" (`include/castlemist/native/gw2model.hpp:1081`) and the glTF map
export's root node `GW2_ZupToYup` (`src/exportgltf/map_export.cpp:92-99`,
−90° about X, which sends **+Z** to glTF +Y) both claim +Z up. Neither cites
evidence. Items 1–5 contradict them: Z is the vertical axis, but up is −Z.
Consequence, **not fixed here**: the existing glTF map export puts map up
(−Z) at glTF −Y, so it is upside down. A proper rotation from a left-handed
frame into right-handed glTF also mirrors it. `src/exportgltf` is outside the
world layer and is left alone. A later export task should use §1.4 instead.

### 1.2 North is +Y, east is +X; handedness

- `gw2-sky.md` §1 item 2 proves N = +Y, E = +X for sky space from the game's
  code (NE/SW texture binding to the ±X/±Y faces, and the stored cube sky's
  N → E → S → W order). The sky is drawn with no rotation in world axes (item
  4 above), so the same holds for map space.
- T3D agrees. Its map `(x, y, z) → (x, −z, −y)` has determinant −1. It takes
  map +Y to three.js −Z, which is "north" in a right-handed, Y-up, X-east
  frame (X × Y = +Z = south). T3D shows maps unmirrored, so it treats map
  space as left-handed with X east and Y north.
- **Left-handed.** With X = east, Y = north and up = −Z, the physical
  right-hand rule gives east × north = up = −Z, i.e. X × Y = −Z: the frame is
  left-handed. This rests, like the sky note's, on the game showing the world
  unmirrored.

### 1.3 Units: UNPROVEN

Several places in castlemist say "GW2 inches" (`assemble.h:33`,
`gltf_export.h:57`, `gw2-model-lods.md`). None of them cites evidence, and
nothing in the dat template names a unit. What was tried:

- `0.0254` is not in the executable as a float or a double (exact bit
  patterns searched). The nearest float, `0.025400052` (`0x3cd013c5`, 28 ulp
  off), sits at file offsets `0x1edbf64` and `0x257dc38`.
- `39.37f` (`0x421d7ae1`) is at VA `0x141eded2c`. It is read through
  RIP-relative displacements at 7 code sites (displacement addresses
  `0x140d2f4d3`, `0x1411a3305`, `0x1411a3668`, `0x141329e96`, `0x141349c91`,
  `0x14134a3cd`, `0x14134aafc`). The one inspected (`movss xmm3` at
  `0x140d2f4cf`) passes it as a plain call argument next to two other
  constants, with nothing tying it to a unit conversion.
- The MumbleLink shared memory (which by its protocol reports positions in
  metres) is opened at `0x140a14340` (`OpenFileMappingW` on
  L"MumbleLink" `0x141b9d038`, size `0x1554`). The view pointer lives at
  `0x1426801d0` and is returned by the getter `0x140a13fe0`. Its writer was
  not found by direct call xrefs (only a tail `jmp` from `0x1409fbb18`).

**What would prove it:** the code that writes `fAvatarPosition` into the
MumbleLink view, with the factor between the world position and the written
value; or a measured in-game distance in a known metric unit. Until then,
**`kMapUnitsPerMetre` is not defined**. The conversions in §1.4 change axes
only and keep map units. An export that needs metres must flag the scale as
UNPROVEN.

### 1.4 Conversions (`src/world/frame.cpp`)

Positions and directions (no scale; §1.3):

```
map_to_unity  (x, y, z) = ( x, -z,  y )   // Unity: LH, +Y up, +Z forward = north
map_to_blender(x, y, z) = ( x,  y, -z )   // Blender: RH, +Z up, +Y = north
```

| map | meaning | Unity | Blender |
|---|---|---|---|
| +X | east | +X | +X |
| +Y | north | +Z | +Y |
| −Z | up | +Y | +Z |

- **Unity** is left-handed like map space. `C_u` (rows `[1 0 0; 0 0 −1; 0 1 0]`)
  is a **proper rotation, det = +1** (+90° about X). This is the same formula
  as `gw2_to_unity` in `src/exportgltf/sky_bake.cpp` / `gw2-sky.md` §1, since
  the frames are the same. It is written separately in the world layer
  because `world` must not depend on `exportgltf`.
- **Blender** is right-handed. `C_b = diag(1, 1, −1)` has **det = −1**. That
  reflection *is* the handedness change: it keeps east, north and up
  physically where they are. It cannot be a rotation, because a rotation
  would mirror the map.
- **Matrices.** A column-major map-space matrix `M` (taking local
  coordinates in map space to map space, as `PropInstance::world` and
  `CollisionInstance::world` do, applied as `p' = M p`) becomes
  `C M C⁻¹` with `C` padded to 4×4. The converted matrix then works on
  converted vertices: `map_matrix_to_X(M) · map_to_X(p) = map_to_X(M · p)`.
  Conjugation keeps the determinant, so a map-space proper rotation (det +1),
  identity included, stays a proper rotation (det +1) in both Unity and
  Blender. In Blender, the det −1 handedness change is carried by `C_b` on
  the vertices and does not appear in the matrices. Translations convert like
  positions.

---

## 2. Map bounds: `parm.rect`

**`WorldScene::bounds` = `parm.rect` as stored, `[x0, y0, x1, y1]` in map
space, x0 < x1, y0 < y1. A map without a `parm` chunk, or whose `parm` has no
`rect` field, has no bounds: `hasBounds = false`, a warning in
`WorldScene::warnings`, and no invented rect.**

Evidence:

- Template: chunk `parm` → struct `MapParam` = `{ rect: float4, flags: dword,
  guid: byte16 }` (`dumps/packfile/gw2_packfile.json`). castlemist reads it
  by field name (`gw2model.hpp:1496-1500`).
- The rect is the terrain's extent. T3D's terrain bounds, taken back to map
  space, equal `rect` exactly on all three maps (`terrainBounds == rect` in
  every reference), and the rect divides into the `trn` chunk grid at exactly
  3072 units per chunk on both axes:

  | map | rect | chunks | width / cx | height / cy |
  |---|---|---|---|---|
  | 192711 | [−43008, −27648, 43008, 30720] | 28 × 19 | 3072 | 3072 |
  | 191000 | [−27648, −18432, 30720, 18432] | 19 × 12 | 3072 | 3072 |
  | 1151420 | [−15360, −36864, 15360, 39936] | 10 × 25 | 3072 | 3072 |

  `gw2dat_cli map --file-id 192711` reads the same rect
  (`terrain.rect = [−43008, −27648, 43008, 30720]`).
- Props can lie outside the rect (Queensdale prop bounds x from −44589.6 to
  43008, y from −31485.3 to 30719). `bounds` is the terrain/map rect, not a
  prop bounding box.
- No fallback: the spec lists "a hard-coded ±3072 rect when the map has none"
  as a fudge to remove. Nothing in the dat says what a rect-less map's extent
  is, so none is made up.

---

## 3. Terrain: chunk layout, sampling, placement

**`trn.heightMapArray` is `chunksX × chunksY` chunks of `(segments + 3)²`
samples, chunk-major (chunk index `cy · chunksX + cx`), row-major inside a
chunk. A chunk's own samples are the inner `(segments + 1)²` (stored rows and
columns `1 … segments + 1`); the outer ring is an apron that repeats the
neighbours' samples. Chunk `cx` runs east from `rect[0]`, chunk `cy` runs
south from `rect[3]`; inside a chunk column 0 is the west edge and row 0 the
north edge. Heights are map-space Z as stored (§1.1). This is T3D's layout;
on all three test maps T3D's odd/even placement rule gives exactly this, so
the hypothesis that it misplaces Spirit Vale is refuted.** Implemented in
`include/castlemist/world/terrain.h` / `src/world/terrain.cpp`.

### Evidence base and how to reproduce it

- **In the repo:** `cm_test_world_dat` (`GW2_TEST_DAT` set) runs
  `terrain_chunk_grid_matches_reference`, `terrain_heights_match_reference`,
  `terrain_adjacent_chunks_share_edges` and `terrain_props_sit_on_terrain`;
  the last prints the §3.3 props-on-terrain counts.
- **Hypothesis tables (§3.1, §3.3):** a throwaway C++ program (scratchpad,
  not committed) that includes `castlemist/native/gw2model.hpp`, opens the
  decompressed map bytes (`gw2dat_cli extract --file-id <id> --out <id>.bin`,
  as in `tools/world/README.md`) with the template
  `dumps/packfile/gw2_packfile.json`, and calls `Extractor::parseTerrain()` and
  `Extractor::parseMapProps()`. Built with
  `g++ -std=c++20 -O2 -Iinclude -Iexternal/nlohmann-json/single_include measure.cpp`.
  It reports, per map:
  1. for horizontal neighbours `A = (cx, cy)`, `B = (cx+1, cy)`: how many of
     `A[row][32+k] == B[row][k]` hold, `k = 0, 1, 2`, all 35 rows; the same
     for vertical neighbours `A = (cx, cy)`, `B = (cx, cy+1)` with
     `A[32+k][col] == B[k][col]`, and reversed (`A[k][col] == B[32+k][col]`);
  2. for each placement hypothesis, over **every** prop `parseMapProps()`
     returns (all four groups, `transforms[]` included; props outside the rect
     skipped): `|z − h|` with `h` bilinear over the hypothesis's chunk grid,
     as median, p25, and the share under 16 and under 64 units;
  3. for the T3D layout, the share of props under 16 units when the props are
     shifted by `s` samples (96 units) along x or y, `s = −64 … 64`.

### 3.1 Which samples are a chunk's own — hypothesis (a)

Two candidates: **T3D** keeps the inner `(segments + 1)²` and drops the outer
ring (`TerrainRenderer.ts:450-462`); **castlemist's old code**
(`src/extract/map_scene.cpp:90-160`) places each 35-sample chunk from stored
index 0, advancing 32, so a chunk's own samples start at index 0.

*Shared edges.* Neighbouring chunks overlap by exactly 3 samples:

| map | x-neighbours, `A[r][32+k] == B[r][k]` (k = 0, 1, 2) | y-neighbours, `A[32+k][c] == B[k][c]` | reversed y |
|---|---|---|---|
| Queensdale 192711 | 17954 / 17955 each | 17640 / 17640 each | 0 / 17640 |
| Lion's Arch 191000 | 7558 / 7560 each | 7315 / 7315 each | ≤ 50 / 7315 |
| Spirit Vale 1151420 | 7875 / 7875 each | 8400 / 8400 each | ≤ 3275 / 8400 |

Every mismatch is in **stored row 0** (the apron): Queensdale chunks
(14,0)|(15,0), Lion's Arch (5,0)|(6,0) and (12,0)|(13,0). With T3D's rule the
chunk's edges are stored rows/columns 1 and 33, so every shared edge of every
chunk pair is bit-identical on all three maps (`terrain_adjacent_chunks_share_edges`).
With the old rule a chunk's north edge is stored row 0, and those three pairs
disagree by up to 99 (Queensdale) and 882 (Lion's Arch) units.

*Props.* Same placement (§3.3), own samples starting at stored index 0, 1 or 2:

| map | own = 0…32 (old) median / <16 | **own = 1…33 (T3D)** | own = 2…34 |
|---|---|---|---|
| Queensdale | 44.8 / 32.0 % | **36.1 / 37.6 %** | 48.2 / 31.2 % |
| Lion's Arch | 153.7 / 19.3 % | **142.9 / 20.9 %** | 153.5 / 19.3 % |
| Spirit Vale | 876.7 / 8.0 % | **847.7 / 9.6 %** | 854.3 / 9.2 % |

**Verdict: T3D's inner-ring rule, PROVEN** (exact shared edges on all maps;
best prop fit on all maps, by a clear margin on Queensdale and Lion's Arch,
narrowly on Spirit Vale). What the apron is for (normals across seams, at a
guess) is UNPROVEN and does not matter: castlemist does not use it.

### 3.2 Chunk grid and samples per chunk — hypothesis (b)

| map | `trn.dims` | `verticesPerChunkSide` | samples | chunks (T3D `sqrt(dims[0]·count/dims[1])`) | `dims / chunks` |
|---|---|---|---|---|---|
| 192711 | 896 × 608 | 32 | 651 700 = 532 · 35² | 28 × 19 | 32 × 32 |
| 191000 | 608 × 384 | 32 | 279 300 = 228 · 35² | 19 × 12 | 32 × 32 |
| 1151420 | 320 × 800 | *absent* | 306 250 = 250 · 35² | 10 × 25 | 32 × 32 |

- Stored per side = `verticesPerChunkSide + 3` (the sample count divides
  exactly, both maps with the field).
- The grid is T3D's `chunksX = sqrt(dims[0] · count / dims[1])`,
  `chunksY = count / chunksX` (`TerrainRenderer.ts:192-195`), required to be
  whole; it equals the reference `chunks` on all three maps, and
  `dims = chunks × segments` holds on all three.
- **No `verticesPerChunkSide` (Spirit Vale).** T3D assumes 32. castlemist
  instead solves `samples = (dimX/s) · (dimY/s) · (s + 3)²` for an integer
  `s` dividing both dims, using the relation `dims = chunks × segments` that
  holds on the maps that have the field; Spirit Vale has exactly one solution,
  `s = 32` (stored 35). No solution, or more than one, is an error
  (`TerrainLayout::why`, warning, no terrain), never a guess.

**Verdict: PROVEN** on the three maps (grid equals T3D's and the reference;
the solved `segments` for Spirit Vale equals T3D's fallback, now derived).

### 3.3 Chunk placement — hypothesis (c)

*Direction from data alone.* Vertical neighbours continue each other only
with `A = cy`, `B = cy + 1` and B's rows after A's (table in §3.1, "reversed"
fails), and column-major chunk order fails as a control (0 / 17955, 0 / 7560,
1908 / 7875 equal). So chunk rows and sample rows run the same way, as do
chunk columns and sample columns. What the edges cannot say is the whole
grid's orientation; the props say it:

| placement (inner samples) | Queensdale median / <16 | Lion's Arch | Spirit Vale |
|---|---|---|---|
| **cy and rows north → south, cx and columns west → east** | **36.1 / 37.6 %** | **142.9 / 20.9 %** | **847.7 / 9.6 %** |
| both south → north | 904.3 / 3.3 % | 1424.1 / 1.2 % | 3366.0 / 1.2 % |
| chunks north → south, rows south → north | 218.3 / 11.5 % | 441.0 / 7.5 % | 977.4 / 3.3 % |
| chunks south → north, rows north → south | 912.9 / 4.2 % | 1456.0 / 0.9 % | 3354.8 / 1.8 % |
| mirrored east-west | 773.8 / 1.4 % | 957.2 / 1.9 % | 2570.2 / 0.5 % |
| old `map_scene.cpp` (south → north, own from index 0) | 903.3 / 3.3 % | 1426.0 / 1.2 % | 3367.0 / 1.1 % |

The last row is why the old viewer's terrain "doesn't match where objects are
placed": it is flipped north-south.

*Shift scan* (share of props under 16 units, props moved by `s` samples):

| map | x: s = −32, −16, −1, **0**, +1, +16, +32 | y: same |
|---|---|---|
| Queensdale | 2.7, 7.5, 33.4, **37.6**, 34.8, 8.5, 2.5 % | 4.5, 9.7, 33.2, **37.6**, 34.7, 8.0, 3.2 % |
| Lion's Arch | 2.6, 6.8, 19.8, **20.9**, 20.2, 7.9, 3.0 % | 2.6, 6.4, 20.0, **20.9**, 20.1, 6.2, 2.9 % |
| Spirit Vale | 1.3, 3.2, 9.8, **9.6**, 8.7, 2.4, 1.1 % | 1.0, 1.4, 8.3, **9.6**, 9.0, 2.0, 0.7 % |

The peak is at 0 everywhere except Spirit Vale x, where `s = −1` is 0.2
points higher (9.8 vs 9.6 %, about 5 props of 2423, within noise). A half-chunk
(`s = ±16`) shift loses 62–85 % of the props on the terrain, a whole-chunk
(`s = ±32`) shift 86–93 %.

*T3D's odd/even rule.* T3D puts chunk `cy`'s centre at three.js
`Z = rect[1] ± cdy/2 + cy · cdy` (+ for an even `chunksY`, − for odd,
`TerrainRenderer.ts:490-502`), and its row 0 at the chunk's smallest Z.
Taken back to map space (`y = −Z`), chunk `cy`'s north edge is at
`−rect[1] − cy·cdy` (even) or `−rect[1] + cdy − cy·cdy` (odd). The rule above
puts it at `rect[3] − cy·cdy`. The two agree exactly when
`rect[1] + rect[3] = 0` (even) or `= cdy` (odd):

| map | chunksY | `rect[1] + rect[3]` | needed | agrees |
|---|---|---|---|---|
| Queensdale | 19 (odd) | 3072 | cdy = 3072 | yes |
| Lion's Arch | 12 (even) | 0 | 0 | yes |
| Spirit Vale | 25 (odd) | 3072 | cdy = 3072 | yes |

Spirit Vale is no different from Queensdale (both odd), and T3D's sampled
heights equal castlemist's at all 256 reference positions on all three maps
(`terrain_heights_match_reference`). Spirit Vale's 787-unit median
prop–terrain gap (Task 2/§1.1) is therefore not a placement error: the rule is
the best placement there too (tables above; the in-repo test finds 233 props
on the terrain against at most 28 for any alternative), and 1274 of its 2423
props are more than 500 units **above** the terrain under them, against 214
more than 500 below. The raid is built on prop floors above its terrain.

**Verdict: placement PROVEN** as stated (chunk and row order from shared
edges on all maps; orientation from props on all maps). T3D's odd/even
*formula* is only proven where it coincides with this rule, which is all three
test maps. For a map where `rect[1] + rect[3]` is neither, the formula would
put the terrain off `rect` (contradicting §2); castlemist does not use it and
places from `rect[3]`.

### 3.4 Sampling: `terrain_height_at`

`terrain_height_at(t, x, y, &inside)` finds the chunk containing `(x, y)`
(`cx = ⌊(x − x0)/cdx⌋`, `cy = ⌊(y1 − y)/cdy⌋`, clamped so the terrain's far
edges belong to the last chunk) and interpolates **bilinearly** over that
chunk's `(segments + 1)²` samples, sample `(i, j)` at
`(rect[0] + i·cdx/segments, rect[3] − j·cdy/segments)`. That is T3D's sampler
(`TerrainRenderer.ts:85-148`) in map space. A point on a shared edge gets the
same height from either chunk (§3.1). `inside = false` (and NaN returned)
means no chunk covers `(x, y)`: it is outside the terrain's extent, or the
map has no terrain. Points on the outer edge are inside. Agreement with T3D:
256 / 256 reference positions within 0.01 on each test map.

### 3.5 Height sign — hypothesis (d)

Heights are kept **as stored**: map-space Z, up = −Z (§1.1, item 2: stored
heights and prop z share a sign, median `|z − h|` 33 / 89 against 2483 /
1745 negated). No negation.

### 3.6 What changed from the old code

`build_terrain_model` (`src/extract/map_scene.cpp:90-160`) is not used by the
world layer and none of its rules returns:

- own samples from index 0 (a one-sample shift, apron mismatches) → inner ring (§3.1);
- `TILES_PER_CHUNK = 32` fixed → `verticesPerChunkSide`, or solved from dims
  and the sample count (§3.2);
- rows placed south → north from `rect[1]` (terrain flipped north-south) →
  north → south from `rect[3]` (§3.3);
- `±3072` rect when there is none → no terrain and a `"no parm rect"`
  warning (§2);
- heights more than 4000 from the median clamped to the median → no clamp;
  heights as stored.

---

## 4. Terrain materials and blend pages

**Each terrain chunk `i` (`i = cy · chunksX + cx`, the index of §3 and of
`Terrain::chunks`) has `trn.materials.materials[i]`. Its
`loResMaterial.texIndexArray` indexes `trn.materials.texFileArray`, and each
indexed entry's `tokenName` says what it is: four colour textures
(`color`, `colorb`, `colorc`, `colord`), four normal maps (`normal` …
`normald`), and two page references, `blend` and `modx`, into layers 0 and 1
of the paged image `trn.materials.pagedImage`. A page covers 4 × 4 chunks;
page-image row 0 is the page's north edge and column 0 its west edge. The
ground-texture UV scale is UNPROVEN.** Implemented in
`Extractor::parseTerrainMaterials` / `Extractor::parsePagedImage`
(`include/castlemist/native/gw2model.hpp`) and `resolve_terrain_materials`
(`src/world/terrain.cpp`).

### Evidence base and how to reproduce it

- **Template** (`dumps/packfile/gw2_packfile.json`): `trn` v14/v15 root
  `PackMapTerrainV14`/`V15` → `materials` (`ptr` to
  `PackMapTerrainMaterialsV14` = `{pagedImage: filename, constArray,
  texFileArray: PackMapTerrainTexV14[], materials:
  PackMapTerrrainChunkMaterialV14[], midFade, farFade}`). A chunk material is
  `{tiling: byte[3], hiResMaterial, loResMaterial, faderMaterial:
  PackMapTerrainMaterialV14, uvData: ptr PackMapTerrainChunkUVDataV14}`; a
  material is `{materialFile, fvf, constIndexArray, texIndexArray}`; a tex
  entry is `{tokenName: dword, flags: dword, filename, flags: dword2, layer:
  dword}` (the template names two fields `flags`; the reader takes each by
  name **and** kind). The PIMG file's chunk `PGTB` (v3,
  `PagedImageTableDataV3`) has `layers[]` (`strippedDims`, `strippedFormat`)
  and `strippedPages[]` (`layer`, `coord: dword2`, `filename`, `flags`,
  `solidColor: byte4`).
- **Inspection:** `gw2dat_cli parse --file-id 191000 --max-depth 8` (the map)
  and `--file-id 190582` (its PIMG) show the fields above with real values.
- **Tokens** decode with GW2's base-23 Token rule (`decode_token`,
  `src/extract/game_shader.cpp:111-122`).
- **Measurement program** (scratchpad, not committed): includes
  `castlemist/native/gw2model.hpp`, `gw2_atex.hpp`, `castlemist/world/terrain.h`;
  built with `g++ -std=c++20 -O2 -Iinclude
  -Iexternal/nlohmann-json/single_include measure.cpp -Lbuild/debug/lib
  -lcastlemist_world -lcastlemist_extract -lcastlemist_native -lcastlemist_core`,
  run as `measure <Gw2.dat> <template> 192711 191000 1151420`. Per map it
  calls `parseTerrain` + `build_terrain` (chunk grid), `parseTerrainMaterials`
  and, on the PIMG file, `parsePagedImage`, and reports:
  1. the decoded token sequence of every chunk's `loResMaterial.texIndexArray`,
     what kind of entry each token names (filename or page reference), how many
     indices are out of range;
  2. for every page reference, whether `coord == (i % chunksX, i / chunksX)`
     and whether `layer` is 0 for `blend`, 1 for `modx`;
  3. whether `hiResMaterial.texIndexArray == loResMaterial.texIndexArray`, the
     (hi, lo) `materialFile` pairs, the `tiling` bytes, non-null `uvData`;
  4. the PIMG layers, the layer-0/1 page grid, every `n` with
     `ceil(chunks / n) = page grid` on both axes, pages with no filename;
  5. a seam test: every page of a layer decoded (`atex::decode`, mip 0, RGBA8);
     for each pair of pages adjacent in `coord` (`(px, py)` and `(px, py+1)`,
     `(px, py)` and `(px+1, py)`), the mean absolute RGBA difference between
     page A's **last** row (column) and page B's **first**, and between A's
     **first** and B's **last**; and, as a baseline, between adjacent rows
     inside a page (every 7th row).
- **In the repo:** `cm_test_world_dat terrain_materials_match_reference`
  (T3D reference and PIMG pages) and `cm_test_world
  terrain_material_index_out_of_range`.

### 4.1 Which fields hold the materials; what `texIndexArray` holds

Measured on every chunk of the three maps:

| map | chunks | `texIndexArray` length | token sequence (`loResMaterial`) | out of range |
|---|---|---|---|---|
| Queensdale 192711 | 532 | 10 (all) | `color, colorb, colorc, colord, blend, modx, normal, normalb, normalc, normald` (532) | 0 |
| Lion's Arch 191000 | 228 | 10 (all) | same (228) | 0 |
| Spirit Vale 1151420 | 250 | 10 (all) | same (225); `color … colord, blend, modx, normal, normalc, normald, ramp` (25) | 0 |

- `color`…`colord`, `normal`…`normald`, `ramp` are entries with a filename
  (`flags = 1`, `layer = 0xFFFFFFFF`); `blend` and `modx` have **no filename**
  (`flags = 4`) and are page references (§4.2).
- **First half** (the five entries T3D reads, `TerrainRenderer.ts:362-363`):
  the four colour textures and the `blend` page reference. T3D looks up the
  fifth entry's filename and gets 0; the reference's `textures[4] = 0` is that.
  **Second half:** the `modx` page reference and the normal maps (on 25
  Spirit Vale chunks, `normal, normalc, normald, ramp`: no `normalb`, plus a
  `ramp` texture). So the split is not "half and half" and the positions are
  not fixed: castlemist binds by token, not position.
  `TerrainMaterial::textureFileIds[k]` is the `color` / `colorb` / `colorc` /
  `colord` texture (slot k, 0 if the chunk binds none), `normalFileIds[k]` the
  matching normal map; `ramp` is not kept (one warning per map names it). On
  all three maps slots 0-3 are also T3D's order, which the test checks
  against the reference.
- **Hi- vs lo-res.** `hiResMaterial.texIndexArray` equals
  `loResMaterial.texIndexArray` on 532/532, 228/228 and 250/250 chunks. They
  differ only in `materialFile`, a pair of consecutive AMAT files (hi = lo − 1):
  (187794, 187795) on Queensdale and Lion's Arch; (184988, 184989) ×208,
  (197590, 197591) ×11, (421455, 421456) ×25, (1151159, 1151160) ×6 on Spirit
  Vale. So the textures and pages are the same at both levels; only the
  shader differs. Which one the game draws when (presumably by distance) is
  **UNPROVEN** and does not change the textures. `materialFileId` stores the
  lo-res file, as T3D uses; `faderMaterial` (empty on Lion's Arch chunk 0) is
  not read.

### 4.2 The paged image and its pages

- The `pimg` file is named by **`trn.materials.pagedImage`** (a `filename`):
  Queensdale 191359, Lion's Arch 190582, Spirit Vale 1151141. T3D loads the
  same field (`TerrainRenderer.ts:602-604`). (The map's own `trni` chunk,
  `MapTerrainImg`, has null `tableData` / `pageData` on Lion's Arch.)
- **Page references carry the chunk coordinate.** Every `blend` / `modx`
  entry's second `flags` field (dword2) equals `(i % chunksX, i / chunksX)`
  for the chunk `i` that binds it, and its `layer` is 0 for `blend` and 1 for
  `modx`: 1064/1064, 456/456 and 500/500 references. This independently
  confirms §3's chunk index (`cx = i % chunksX`, `cy = i / chunksX`) from
  the material data, and that `materials[i]` belongs to `Terrain::chunks[i]`.
- **Pages.** Two layers of 512 × 512 pages: layer 0 `DXT5` (`blend`), layer 1
  `DXT1` (`modx`). `strippedPages[].coord` is in **pages**:

  | map | chunks | layer 0 / 1 page grid | `n` with `ceil(chunks/n)` = grid | pages without a filename |
  |---|---|---|---|---|
  | 192711 | 28 × 19 | 7 × 5 / 7 × 5 | **4 only** | 0 of 70 |
  | 191000 | 19 × 12 | 5 × 3 / 5 × 3 | **4 only** | 0 of 30 |
  | 1151420 | 10 × 25 | 3 × 7 / 3 × 7 | **4 only** | 8 of 42 (all with non-zero `solidColor`) |

  So a page holds 4 × 4 chunks (128 × 128 px each), and chunk `(cx, cy)`'s
  page is `(⌊cx/4⌋, ⌊cy/4⌋)`, which is T3D's `pickerPage`
  (`TerrainRenderer.ts:340-341`). castlemist **derives** `n` as the one value
  that fits the page grid and warns (no pages) when none or several fit.
  `pickerScale = 1/n`. A page with no filename is a solid-colour page (T3D
  makes a 1 × 1 texture from `solidColor`, `TerrainRenderer.ts:283-290`):
  `pickerFileId` is 0 and `pickerSolid` holds the stored 4 bytes (channel
  order UNPROVEN). On Spirit Vale 128 chunk-layer pairs use such pages.
- Layers above 1 (none on the test maps) and `rawPages` (empty) are not read.

### 4.3 The chunk's sub-rect in its page: orientation

T3D's V offset `0.75 − (cy % 4)/4` is in its own frame: three.js texture
`v` with `flipY = true` (`MaterialUtils.ts:600`), so `v = 1` is the image's
first stored row, and the chunk's PlaneGeometry has `v = 1` at its north edge
(the edge §3 proves is row 0). The chunk then spans `v ∈ [0.75 − k/4, 1 − k/4]`,
`k = cy % 4`: image rows `k/4 … (k+1)/4` from the top, north edge at `k/4`.
That is a claim about T3D; the data says the same thing:

*Seam test.* If page-image rows run north → south (the same way as `cy`, and
page `coord.y`), the last row of page `(px, py)` continues into the first row
of `(px, py + 1)`; if they ran south → north it would be the first row into
the last. Likewise for columns west → east:

| map / layer | adjacent rows inside a page | vertical seams: last→first / first→last | horizontal seams: last→first / first→last |
|---|---|---|---|
| 192711 / 0 | 6.90 | **2.76** / 77.61 (28) | **2.06** / 78.20 (30) |
| 192711 / 1 | 2.48 | **2.16** / 54.38 | **1.90** / 17.51 |
| 191000 / 0 | 6.09 | **3.51** / 82.78 (10) | **4.26** / 79.19 (12) |
| 191000 / 1 | 1.04 | **0.47** / 9.11 | **0.62** / 46.44 |
| 1151420 / 0 | 2.03 | **1.36** / 73.21 (14) | **1.78** / 82.62 (10) |
| 1151420 / 1 | 0.37 | **0.51** / 26.18 | **0.80** / 41.17 |

(mean absolute RGBA difference, 0-255; seam counts in brackets.) Across
every seam, the "last → first" pairing is as smooth as neighbouring rows
inside a page and the other is 4-100× rougher. Page-image rows run with
`coord.y`, i.e. with `cy`, north → south (§3.3), and columns with `cx`,
west → east.

**Verdict, PROVEN:** `pickerOffset = ((cx % n)/n, (cy % n)/n)` in page-image
UV with `u` from the first column (west) and `v` from the first stored row
(north); the chunk spans `pickerOffset … pickerOffset + pickerScale`, its
sample `(i, j)` (§3) at `pickerOffset + (i, j)/segments · pickerScale`. A
renderer that puts the first stored row at GL `v = 0` uses this as is; one
that flips (T3D) uses `1 − v`, which gives T3D's `0.75 − (cy % 4)/4` for the
sub-rect's lower bound. How texels are inset at page edges (T3D's shader
`edge` term) is not settled here.

### 4.4 Ground-texture UV scale: UNPROVEN

T3D hard-codes `uvScale = 8` with "TODO: READ FROM VO"
(`TerrainRenderer.ts:405-406`). Candidates in the data:

- `tiling` (byte[3], byte in v10): `[0, 8, 8]` on 512 / 532 Queensdale
  chunks (`[0, 2, 2]` 8, `[0, 4, 4]` 6, `[0, 8, 2]` 6), 222 / 228 Lion's Arch
  (`[0, 2, 2]` 3, `[0, 2, 8]` 3), Spirit Vale `[8, 8, 8]` 130, `[0, 8, 8]` 108,
  `[0, 8, 4]` 12. The 8 matches T3D's constant, but there are three bytes for
  four colour textures and nothing ties a byte to a texture or to a unit.
- `uvData` (`translation`, `xScaleRange`, `yScaleRange`, `scaleSpeed`,
  `rotation`): null on every Queensdale and Lion's Arch chunk, non-null on 36
  Spirit Vale chunks; it looks like UV animation and was not decoded.
- `constArray` is empty on Lion's Arch.

**What would prove it:** the terrain AMAT's shader (e.g. 187795: `GRMT`
`texCount 10`, 79 DX11 shaders in `BGFX`) disassembled to show how it scales
the colour-texture UVs, and which constant/`tiling` byte feeds it; or the game
code that reads `PackMapTerrrainChunkMaterial.tiling`. Until then
`TerrainMaterial::uvScale = 0` ("unknown", not 8) and the `tiling` bytes are
kept as stored in `TerrainMaterial::tiling`.

### 4.5 What `resolve_terrain_materials` does

Per chunk `i`: `materialFileId = loResMaterial.materialFile`; each
`texIndexArray` entry by token (§4.1); `blend` / `modx` → the layer 0 / 1 page
at the entry's coord / `n` (§4.2), offset as §4.3; `uvScale = 0`, `tiling`
as stored (§4.4). `resolved = true` when every index is inside
`texFileArray` and at least one colour texture is bound. Warnings (no
exceptions) for: no `materials`; a chunk-count mismatch; no / unreadable
PIMG; a page grid no `n` fits; a missing page; a page reference whose layer
or coord is not the chunk's own; an index past `texFileArray` (names the
chunk); a chunk with no colour texture; tokens not kept (`ramp`). On the three
test maps every chunk resolves; the only warning is Spirit Vale's 25 `ramp`
bindings.

---

## 5. Props: models, instances and the world matrix

`build_props` (`src/world/props.cpp`) turns `Extractor::parseMapProps()` into
`WorldScene::models` (distinct `fileId`s, first-seen order), `props` (one per
placement, input order) and `motion.animatedProps` (indices of
`group == "propAnimArray"`). `MapProp::group` is the `prp2` array the
placement came from: `propArray`, `propAnimArray`, `propMetaArray`, or
`propInstanceArray` (the base placement and each of its `transforms[]`).

### 5.1 The transform (proven against the client)

The rule is the client's, not a convention. It is the summary of
`src/render/detail/math.h:160-189`: `Gw2-64.exe` builds a prop's world
transform in one leaf helper (`sub_1409C8920` in the IDB, called from
`PrContext_LoadPropModel` as `(out, scale, &prop->position, &prop->rotation)`;
`PrProp` holds position at +32 and rotation at +44). It takes cos/sin of
`rotation[0..2]` and writes a float3x4 as three 16-byte rows `[r0 r1 r2 | t]`,
translation in each row's fourth column, i.e. the column-vector layout
`p' = M·p + t`. With `cx = cos(rot[0])`, `sx = sin(rot[0])`, `cy`/`sy` for
`rot[1]`, `cz`/`sz` for `rot[2]`:

```
[ cz*cy - sy*sx*sz   cz*sx*sy + sz*cy   -cx*sy ]
[ -cx*sz             cz*cx               sx    ]
[ cy*sx*sz + cz*sy   sz*sy - cz*cy*sx    cy*cx ]
```

which is `Ry(-r1) · Rx(-r0) · Rz(-r2)` for column vectors. The render layer's
`sceneWorld` builds the same thing in a row-vector basis as
`rotZ(-r2) · rotX(-r0) · rotY(-r1)`, the transpose; the matrix above is what
`PropInstance::world` holds directly, so no transposition is needed.

`PropInstance::world` is **column-major, column vectors, map space** (the same
convention as §1.4): `world[col * 4 + row] = scale * M[row][col]`,
translation `pos` in elements 12-14, bottom row `0 0 0 1`. Scale is uniform
and multiplies the 3×3 only. `pos`, `rot`, `scale` are kept as stored. The
pure test `world.prop_transform_matches_client` writes the matrix out from the
formula above for `pos {10,20,30}`, `rot {0.3,-0.2,1.1}`, `scale 2` and
compares within 1e-6.

### 5.2 Agreement with T3D

`world` in the T3D references is `A · M · L` with `A = (x,y,z) → (x,-z,-y)`
taken back out of three.js space (`tools/world/README.md`, "`world` matrices"),
column-major, so it is directly comparable. Procedure (`tests/test_world_dat.cpp`,
`world_dat.props_match_reference`; reproduce with
`GW2_TEST_DAT=<dat> build/debug/bin/cm_test_world_dat.exe world_dat`):

1. Parse each test map's props, `build_props`, and bucket castlemist's props by
   `group`.
2. Bucket the reference's `props` (the first 50 of each group, file order, a
   prop's `transforms[]` following it) by `group`. The groups are compared
   one at a time because castlemist concatenates them as `propArray`,
   `propAnimArray`, `propMetaArray`, `propInstanceArray` and T3D as
   `propArray`, `propAnimArray`, `propInstanceArray`, `propMetaArray`.
3. For the k-th prop of each group: `fileId` equal, `pos` within 0.01, all 16
   `world` elements within 1e-4.

Result: 150 of 150 props agree on each of 192711, 191000 and 1151420 (50 each
of `propArray`, `propAnimArray`, `propInstanceArray`; no map has any
`propMetaArray`). Largest difference: `world` 5.96e-07 / 7.15e-07 / 9.54e-07
(the reference is rounded to 1e-6), `pos` 0. So castlemist's client transform
and T3D's three.js `Euler(r0, -r2, -r1, "ZXY")` mapped back through `A` are
the same matrix, as `math.h` already claimed to 1e-14 on its own data.

### 5.3 Props sit inside the terrain

`world_dat.props_inside_terrain_rects`: the share of prop `(x, y)` inside the
union of the terrain's chunk rects (`build_terrain`, §3) is 11667 / 11687
(99.83%) on 192711, 18532 / 18571 (99.79%) on 191000 and 2423 / 2425 (99.92%)
on 1151420. The test requires at least 99%. The stragglers are placements
just outside the map rect; they are kept, not clipped.

## 7. Collision: hulls placed by obs, prop and zone models

**A `havk` placement (an `obsModels`, `propModels` or `zoneModels` entry)
names a geometry. The geometry's last animation lists the hull indices. Each
hull is placed by the client's prop transform (§5.1) at scale `32 · scale`
(obs models: `32`), with the hull's z negated first: a hull vertex `v` as
stored lands at `R · 32s · diag(1, 1, −1) · v + t` in map space. The ×32 and
the z flip are proven on Queensdale against the visual props.
`animations[last]` is T3D's rule and is UNPROVEN: the data says the
animations are per-sequence collision states and that a prop placement names
its own.** Implemented in `Extractor::parseHavok`
(`include/castlemist/native/gw2model.hpp`, beside the old
`parseMapCollision`, which is left as it was for the old path) and
`build_collision` (`src/world/collision.cpp`).

### Evidence base and how to reproduce it

- **Template** (`dumps/packfile/gw2_packfile.json`, `havk` v16 root
  `PackMapCollideV16`): `collisions[]` = `{indices: word[], vertices:
  float3[], surfaces: word[], moppCodeData}`; `animations[]` = `{sequence:
  qword, collisionIndices: dword[], blockerIndices: dword[]}`;
  `geometries[]` = `{quantizedExtents: byte, animations: dword[],
  navMeshIndex: word}`; `obsModels[]` = `{translate: float3, geometryIndex}`;
  `propModels[]` = `{token: qword, sequence: qword, scale, translate,
  rotate: float3, geometryIndex}`; `zoneModels[]` = `{scale, translate,
  rotate, geometryIndex}`. The reader takes every field by name from the
  element struct that its array field names, so other versions read the same
  way. A struct without `scale` (obs) reads as 1; one without `rotate` reads
  as 0.
- **T3D**: `HavokRenderer.ts:189-204` (geometry → `animations[last]`,
  commented "for now"), `:137-146` (missing animation or hull skipped),
  `:244-262` (`compose` of translation, `Euler(r0, −r2, −r1, "ZXY")`, scale
  `32 · scale`, or 1 when that is 0), `:288-292` (hull vertex
  `(v0, v1, v2) → (v0, v2, −v1)` in three.js), `:373-375` (obs `scale = 1`).
  Taken back to map space this is `world = A · M · L` of
  `tools/world/README.md`.
- **References**: `tests/world_ref/<id>.json` `collision` (the total and
  per-group counts, and the first 20 rows overall and per group).
- **In the repo** (`GW2_TEST_DAT=<dat> build/debug/bin/cm_test_world_dat.exe
  world_dat`): `collision_hulls_fit_visual_props` (§7.2-7.3),
  `collision_matches_reference` (§7.4) and `collision_inside_terrain_rects`;
  also `cm_test_world collision_bad_indices_skipped` (§7.5).
- **Census of `animations[]`** (§7.1): `tools/world/havk_sequences.mjs`, run as
  `node tools/world/havk_sequences.mjs --t3d <t3d checkout> --map-bytes
  <map bytes>` on bytes extracted as in `tools/world/README.md`. It reads the
  map with T3D's parser package and prints numbers only.

### 7.1 Which hulls a placement uses: `animations[last]` is UNPROVEN

`build_collision` follows T3D: placement → `geometries[geometryIndex]` →
`animations[last]` → that animation's `collisionIndices[]` → hulls. A geometry
with no animation gives no instance and is counted in a warning (no test map
has one). What the other
`animations[]` entries are, from `havk_sequences.mjs`:

| | 192711 | 191000 | 1151420 |
| --- | --- | --- | --- |
| geometries / animations / hulls | 1127 / 1873 / 1318 | 1192 / 1615 / 1292 | 332 / 551 / 388 |
| geometries with 1 animation | 911 | 1068 | 277 |
| geometries with 2+ animations, all `sequence`s distinct within it | 216 of 216 | 124 of 124 | 55 of 55 |
| animations whose `sequence` is `192720390330` | 1127 | 1190 | 326 |
| `propModels` whose `sequence` is `192720390330` | 8174 of 8301 | 12552 of 12669 | 1326 of 1395 |
| `propModels` on a 2+ geometry: the animation its own `sequence` names is first / middle / **last** / missing | 680 / 508 / **311** / 0 | 422 / 230 / **533** / 6 | 45 / 58 / **60** / 5 |
| prop rows from `animations[last]` / from the animation the placement's `sequence` names | 8510 / 8449 | 12758 / 12819 | 1693 / 1680 |

- Every animation carries a `sequence` token, and the tokens within one
  geometry are distinct. So a geometry's `animations[]` holds one collision
  set per sequence. `192720390330` is on (nearly) every geometry and is the
  `sequence` of most prop placements: it is the default.
- `propModels.token` is the `guid` of a `prp2` base prop for 4903 of 8301
  placements on 192711 (6389 / 12669 on 191000, 856 / 1395 on 1151420), and
  every such prop has the same translate and scale. For the ones from
  `propAnimArray`, the prop's `animSequence` equals the placement's `sequence`
  in 211 / 211, 215 / 217 and 89 / 89 cases.
- So a prop placement names the sequence it plays. T3D's rule gives a
  placement another sequence's hulls whenever the named animation is not the
  last one: 1188 placements on 192711, 652 on 191000 and 103 on 1151420. Add
  the placements whose sequence names no animation of their geometry (0, 6, 5)
  and the last animation's `sequence` differs from the placement's in 1188,
  658 and 108 placements: the counts `build_collision`'s warning reports.
- `zoneModels` and `obsModels` carry no `sequence`. Obs geometries have one
  animation, the default. Zone geometries have the default plus
  `566482876922` (last) on 192711 and 191000, a set of six sequences (70
  placements on 192711), or `566482876922` alone (1151420).
- The tokens do not decode with the base-23 Token rule (§4.1), so the
  sequences are unnamed. Which animation the client uses was not traced in the
  executable.

**Status.** The rule is kept as T3D's so the references compare row for row.
For props, the data points to "the animation whose `sequence` equals the
placement's own, else the default". That is a reading of the data, not the
client's rule, and it is not implemented. What would prove it: the client
code that picks a geometry's animation for a placement.

`Extractor::parseHavok` reads `animations[].sequence` and each placement's
`sequence` (by name, kind `qword`; only `propModels` has one).
`build_collision` counts the placements whose geometry has 2+ animations and
whose `sequence` differs from `animations[last].sequence`, and emits one
warning: "`N` collision placements use animations[last] (T3D's rule,
UNPROVEN, see gw2-world-frame.md §7.1) rather than the animation their
sequence names". `world_dat.collision_matches_reference` requires exactly
this warning, with N = 1188 / 658 / 108.

### 7.2 Hull units: ×32 (proven)

Hull vertices are in units of 1/32 map unit. `world_dat.collision_hulls_fit_visual_props`:

1. On Queensdale, link each `propModels` placement to the `prp2` prop with the
   same `pos` (within 0.01), `rot` and `scale` (within 1e-4). 653 placements
   link. The first 300 distinct (model, geometry) pairs are measured, and 299
   of them load.
2. Both are placed with the same rotation and translation. So in the
   placement's frame, the model's vertex box (all meshes, as stored) is
   compared with the box of the placement's hulls (`animations[last]`).
3. Per axis, ratio = model extent / hull extent; the median is taken over the
   pairs.

Result: **x 32.085, y 32.097, z 32.128.** No factor is fitted. The test
asserts each median is within 1 of 32, which excludes 39.37 (inches per
metre), 16 and 64. The models reach a little past their hulls, since
collision shapes are simplified.

This fixes the ratio of hull units to map units only. How long a map unit is
stays UNPROVEN (§1.3). Scale is `32 · scale` for prop and zone placements and
`32` for obs models, which have no `scale` field. T3D's fallback to scale 1
when `32 · scale` is 0 is not carried over: no placement on the test maps has
scale 0, and a zero scale is kept as the data says and counted in a warning.

### 7.3 Hull z points up: `diag(1, 1, −1)` (proven)

For the same pairs, the hull box at ×32 under each of the eight axis sign maps
`diag(±1, ±1, ±1)` is scored by its box IoU with the model box (median):

| x y z | `+ + +` | `− + +` | `+ − +` | `− − +` | **`+ + −`** | `− + −` | `+ − −` | `− − −` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| IoU | 0.046 | 0.039 | 0.031 | 0.027 | **0.860** | 0.710 | 0.703 | 0.537 |

Negating z fits best, and it beats keeping z in 294 of 299 pairs. So hulls are
stored with up = +Z, the opposite of map space (§1.1), and the hull-local →
map matrix is the prop transform times `diag(1, 1, −1)`. That is a
reflection: **`CollisionInstance::world` has a negative determinant, so a
hull triangle's winding reverses in map space.** T3D reaches the same matrix
through its hull vertex mapping (`tools/world/README.md`:
`A · L_havok = diag(1, 1, −1)`).

### 7.4 Agreement with T3D

`world_dat.collision_matches_reference` checks, per map:

- The instance count and the per-group counts equal the reference.
- Every reference row (`sample`, 20 rows, and `sampleByGroup`, 20 per group)
  has an instance with the same (group, placement index in its group,
  collision index).
- That instance's `world` equals the reference's within 1e-4. The
  reference's z flip is the one §7.3 proves, so the two are compared as they
  are.

Rows are matched by key, not by position, because T3D draws prop, zone, obs.

| map | instances (obs / prop / zone) | rows compared | disagree | max \|Δworld\| |
| --- | --- | --- | --- | --- |
| 192711 | 13010 (109 / 8510 / 4391) | 80 | 0 | 3.08e-06 |
| 191000 | 13075 (229 / 12758 / 88) | 80 | 0 | 1.95e-06 |
| 1151420 | 2327 (66 / 1693 / 568) | 80 | 0 | 1.7e-05 |

The only collision warning on each test map is §7.1's.
`world_dat.collision_inside_terrain_rects`
counts instance origins inside the union of chunk rects: 12998 / 13010
(192711), 13063 / 13075 (191000), 2326 / 2327 (1151420). The test requires
99%. The old reader left every hull at the origin.

### 7.5 What `build_collision` does with bad data

- It makes one `CollisionMesh` per hull, at the same index
  (`CollisionInstance::mesh` is the havk collision index), with vertices as
  stored. A triangle that indexes past its hull's vertices is dropped, as T3D
  does (`HavokRenderer.ts:298-316`), and the drops are counted in one warning.
- A placement whose `geometryIndex`, last animation index or any collision
  index is out of range keeps whatever does resolve. It is counted once, in
  one warning: "`N` collision placements reference a geometry, animation or
  collision index out of range" (pure test `collision_bad_indices_skipped`).
  T3D would throw on the bad geometry and silently skip the rest.
- Placements whose geometry has no animations (no hulls placed), and
  placements with scale 0 (kept as stored, so the matrix is degenerate), are
  each counted in one warning (pure test `collision_suspect_placements_warned`).
