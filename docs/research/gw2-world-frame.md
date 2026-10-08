---
name: gw2-world-frame
description: "The frame WorldScene keeps a GW2 map in (map space as stored): axes, handedness, Z sign, units, map bounds, the terrain chunk layout, and the exact map -> Unity / map -> Blender conversions. Source of truth for castlemist::world and every later world export; anything unproven is marked UNPROVEN."
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
