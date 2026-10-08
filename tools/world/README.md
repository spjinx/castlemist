# tools/world

`t3d_reference.mjs` writes the reference numbers castlemist's `world` layer is
tested against (`tests/world_ref/<fileId>.json`). The numbers are what
[T3D](https://github.com/spjinx/t3d) -- the browser map viewer whose output
looks right in practice -- computes for a map. They are an oracle, not a
proof: where T3D's own rule is unexplained (see "Known T3D quirks" below) the
reference reproduces it as is, and the research note decides whether
castlemist agrees.

T3D is GPL-3 and castlemist is MIT. Nothing of T3D is copied here: the script
loads T3D's `parser` package from a checkout outside this repo at run time,
and re-derives the arithmetic of T3D's three.js renderers, citing the file and
line of each rule it reproduces.

## Running it

Needs Node 18+, a T3D checkout and the decompressed map bytes.

```bash
# once, in the T3D checkout
cd <t3d>/parser && npm ci && npm run build

# per map (write the bytes outside the repo)
./build/debug/bin/gw2dat_cli.exe extract --dat "$GW2_TEST_DAT" --file-id 192711 --out <tmp>/192711.bin
node tools/world/t3d_reference.mjs --t3d <t3d> --map-bytes <tmp>/192711.bin \
     --file-id 192711 --out tests/world_ref/192711.json
```

The script has no npm dependencies of its own. It prints one line on stderr
with a diagnostic (how far `propArray` props' stored z is from T3D's terrain
under them); that number is not written to the JSON.

The committed references were made from T3D commit `b3428b2`
(`t3dCommit` in each file) on maps 192711 (Queensdale), 191000 (Lion's Arch)
and 1151420 (Spirit Vale).

## Spaces

- **Map space**: GW2 coordinates as stored in the map file.
- **three.js space**: where T3D draws. T3D maps a map-space point
  `(x, y, z)` to `(x, -z, -y)` (`PropertiesRenderer.ts:202`,
  `HavokRenderer.ts:249`; model vertices the same way, `RenderUtils.ts:649-656`).
  This map, `A`, is its own inverse, so every computed value below is taken
  back into map space with `A` again.

Stored values are copied unchanged; computed values are rounded to 1e-6.

## Fields

| field | convention |
| ----- | ---------- |
| `map` | the map's fileId |
| `t3dCommit` | `git rev-parse HEAD` of the T3D checkout |
| `rect` | `parm.rect` as stored: `[x0, y0, x1, y1]`, map space |
| `terrainBounds` | T3D's terrain bounds (`TerrainRenderer.ts:504-520`, `mapRect`) taken back to map space: `[X1, -Z2, X2, -Z1]`. A warning is added if it differs from `rect` |
| `chunks` | `[numChunksD_1, numChunksD_2]` = `[sqrt(dims[0]*count/dims[1]), count/that]` (`TerrainRenderer.ts:192-195`); `chunks[0]*chunks[1]` is the `trn` chunk count |
| `segments` | quads per chunk side T3D draws: `verticesPerChunkSide`, or 32 when the field is absent or the sample count disagrees (`TerrainRenderer.ts:15-36`) |
| `samplesPerChunkStored` | samples per chunk side stored in `heightMapArray` (`segments + 3`); T3D drops the outer ring and keeps the inner `(segments+1)^2` (`TerrainRenderer.ts:450-462`) |
| `heights` | 256 rows `[x, y, z]`, map space. `(x, y)` are the cell centres of a 16x16 grid over `rect`: `x = x0 + (i+0.5)(x1-x0)/16`, `y = y0 + (j+0.5)(y1-y0)/16`, rows ordered `j` outer, `i` inner. `z` = T3D's terrain height there: T3D samples three.js `(X = x, Z = -y)` with `createTerrainHeightSampler` / `sampleTerrainHeightChunk` (bilinear over the chunk's inner grid, `TerrainRenderer.ts:85-148`), which returns three.js `Y`; T3D stores `Y = -h` (`TerrainRenderer.ts:456`), so `z = -Y` is the height **as stored in `heightMapArray`** (not negated). `null` if T3D has no terrain there (warned) |
| `props` | the first 50 placements of each prop group that has any, groups in T3D's order `propArray`, `propAnimArray`, `propInstanceArray`, `propMetaArray` (`PropertiesRenderer.ts:63-67`; `propToolArray` is not drawn). Within a group, file order; each prop is followed by the entries of its `transforms[]` (`PropertiesRenderer.ts:164-171`), which carry `"transform": k` and the parent's `fileId` (its `filename`). `pos`, `rot`, `scale` are as stored. `world` is below |
| `propCounts` | total placements per group, transforms included |
| `collision.instances` | number of (placement, collision index) rows T3D draws, over `obsModels`, `propModels`, `zoneModels` |
| `collision.instancesByGroup` | the same, per group |
| `collision.sample` | the first 20 rows in the order **obs, prop, zone** (castlemist's placement order; T3D itself draws prop, zone, obs -- `HavokRenderer.ts:382-389` -- and buckets rows by collision index, so it has no global row order). Each row: `group`, `index` (placement index in its group), `geometryIndex`, `collisionIndex`, `world` |
| `collision.sampleByGroup` | the first 20 rows of each group (obs placements carry no rotation, so `sample` alone does not exercise rotation or scale) |
| `water` | `watr` as T3D's parser reads it, as stored: `surfaces` = `waterSurfaces.length`, `z` = each `waterSurfaceZ`, `planeZ` = `waterPlaneZ`; `havkWaterSurfaceZ` from `havk` when present. T3D's renderers do not use `watr`: they draw one flat plane at three.js `Y = 0` over the terrain bounds (`TerrainRenderer.ts:180-190, 556`) |
| `terrainMaterials` | chunks 0-15: `textures` = `texFileArray[i].filename` (numeric fileId, 0 kept) for the first half of `materials[chunk].loResMaterial.texIndexArray` (`TerrainRenderer.ts:344, 362-363`, loop `gi < length/2`); `pickerPage` = `[floor(cx/4), floor(cy/4)]` with `cx = chunk % chunks[0]`, `cy = floor(chunk / chunks[0])` (`TerrainRenderer.ts:338-341`). The page's texture fileId needs the `pimg` file and is not recorded |
| `warnings` | anything T3D does that could not be reproduced, or that the data lacks |

### `world` matrices

16 floats, **column-major** (three.js `Matrix4.elements`, glTF), translation
in elements 12-14. Each takes a local vertex **as stored in its file** to map
space exactly where T3D draws it:

```
world = A * M * L
```

- `M` is T3D's three.js object matrix `T * R * S`: translation
  `(x, -z, -y)`, rotation three.js `Euler(r0, -r2, -r1, "ZXY")` =
  `Rz(-r1) * Rx(r0) * Ry(-r2)`, uniform scale `s`
  (`PropertiesRenderer.ts:196-205`; `HavokRenderer.ts:244-261`).
- `L` is how T3D brings the file's local vertices into three.js:
  - props: `L = A`, `(a, b, c) -> (a, -c, -b)` (`RenderUtils.ts:649-656`);
  - collision hulls: `L = (v0, v1, v2) -> (v0, v2, -v1)` (`HavokRenderer.ts:288-292`).
- Prop scale `s` = stored `scale`. Collision scale `s = 32 * scale`, with
  obs models given `scale = 1` (`HavokRenderer.ts:247, 373-375`), and `s = 1`
  when that product is 0 (`HavokRenderer.ts:250-253`); placements without
  `rotate` (obs) get no rotation, without `translate` the origin
  (`HavokRenderer.ts:249, 255-259`).

So a prop's `world` translation is its stored `pos` and its linear part is a
proper rotation times `scale`. For a collision row, `A * L_havok` is
`diag(1, 1, -1)`: T3D draws hull vertices with z flipped relative to map
space (visible as `-32` in the third column of an unrotated obs row).

## Known T3D quirks reproduced as is

- Chunk Y placement depends on whether `chunks[1]` is even (`rect[1] + cdy/2`)
  or odd (`rect[1] - cdy/2`), unexplained (`TerrainRenderer.ts:490-502`).
  Taken back to map space (`y = -Z`) this lands the terrain exactly on `rect`
  for all three test maps (odd: 192711, 1151420; even with a y-symmetric
  rect: 191000).
- Terrain row 0 of a chunk is drawn at the chunk's smallest three.js Z, i.e.
  its **largest** map y.
- `animations[last]` of a geometry is the collision source, commented "for now"
  (`HavokRenderer.ts:194`).
- Terrain UV scale 8 is hard-coded (`TerrainRenderer.ts:405-406`); not recorded.
