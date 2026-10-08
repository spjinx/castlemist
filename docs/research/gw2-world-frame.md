---
name: gw2-world-frame
description: "The frame WorldScene keeps a GW2 map in (map space as stored): axes, handedness, Z sign, units, map bounds, and the exact map -> Unity / map -> Blender conversions. Source of truth for castlemist::world and every later world export; anything unproven is marked UNPROVEN."
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
