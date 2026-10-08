---
name: gw2-sky
description: "How GW2 draws its sky: the EnvHemicubeSkybox mesh behind skyModeTex NE/SW/T (a hemicube, not a panorama), the sky pixel shader's brightness/haze maths, which sky mode is day/night, the GW2 -> Unity axis map, the Unity face mapping of skyModeCubeTex, and (round 2) the star field, sky cards and cloud layers with their sampler formulas and the frame inputs they share. Source of truth for the skybox baker; anything unproven is marked UNPROVEN."
---

# GW2 sky: projection, layers and axes

Research for Task 2 of `docs/superpowers/plans/2026-10-08-skybox-export.md`
(spec `docs/superpowers/specs/2026-10-08-skybox-export-design.md`). Done
2026-10-08. The sky baker (`sky_bake`, Task 4) implements only what this note
proves; every layer marked **UNPROVEN** is left out of the bake and named in
`sky.json` `warnings`.

## Evidence base

- **Executable:** `E:/Games/gw2/Guild Wars 2/Gw2-64-disable-aslr.exe`
  (md5 `6a2b0557ac47da4c23e5c2d790dc491c`, imagebase `0x140000000`). Every
  code address below is in this build. `tools/gw2_annotations.json` was exported
  from a different build (md5 `c8dd1daa…`); its addresses are close but shifted
  (for example the sky factory call sits at `0x140c4128f`, which the annotations
  file places inside "EnvContext_LoadSkyTextures" at `0x140c412a0`). Do not mix
  the two sets of addresses.
- **Code:** disassembled with `objdump -d -M intel` (msys64 ucrt64 binutils).
  String anchors: `"…\Map\Environment\EnvHemicubeSkybox.cpp"` (`0x141d66d60`),
  `"Invalid skybox face"` (`0x141d66d40`), `"Invalid skybox texture"`
  (`0x141d66db8`), `"…\Map\Environment\EnvSkyCards.cpp"` (`0x141d8aab0`).
  Code references to them were found by scanning `.text` for RIP-relative
  displacements.
- **Shaders:** `tools/shaders/extract_exe_shaders.py` re-run on that exe
  (2209 blobs, so the indices differ from the 2244-blob run in
  `gw2-exe-shaders.md`). Disassembled with `D3DDisassemble`. "PS 1617" below
  means `cso/1617_F_*.cso` from that run.
- **Textures:** decoded with `gw2dat_cli texture` (map 187611 mode 0: NE 187554,
  SW 187556, T 187558, 512² each; map 3264516 cube faces 3263205..3263215;
  map 3134778 cube faces 3265586..3265596). Seams were measured numerically:
  mean absolute RGB difference (0..255) between the edge texel strips that the
  derived geometry says meet. The control is the same pair with one strip
  reversed.
- **Round 2 (stars, cards, clouds; same exe, same shader run).** Extra code
  disassembled: `0x140aa0000..0x140ab0000` (graphics helpers) and
  `0x140c00000..0x140e00000` (all of `Map\Environment`). Embedded program blobs
  (`PF`/`AMAT`/`GRMT` packfiles in `.rdata`) were cut out of the exe at the
  address and size each creation call passes (`0x140aab1f0(size, blob, …)`) and
  read with `gw2dat_cli amat --data`, which reports each effect's bgfx render
  state. Uniform ids set by the draws are GW2 base-23 Tokens
  (`gw2-uniform-hash.md`), decoded by hand: `0x36262728` → `skypar[a]`,
  `0x3ef90059` → `skyparb`, `0x3045e8da` → `sun[D]ir`, `0x304647e8` →
  `sun[C]lr`, `0x36b8f56f` → `uv[T]rans`, `0x3044592b` → `cldPar[a]`,
  `0x30a68f22` → `cldParb` (capitals are not in the alphabet; a trailing `a` is
  a leading zero digit). The draws also run the token encoder `0x14029ab40` on
  the literal names (`"uvTrans"` `0x141d74108`, `"cldPara"` `0x141d74110`,
  `"cldParb"` `0x141d74118`, `"skypara"` `0x141d66dd0`, `"sunDir "`,
  `"sunClr "`), so the name ↔ id pairs are proven both ways. Dat files were
  read only through `gw2dat_cli` (`sniff`, `extract`, `parse`, `amat`); the
  extracted star packfile and DDS were then parsed as plain files.

### The sky renderer object (EnvHemicubeSkybox)

| What | Where | Finding |
|---|---|---|
| factory | `0x140c6cfb0`, called from `0x140c4128f` (EnvContext setup) | allocates 0xdd0 bytes, vtable `0x141d66cf0` |
| vtable | `0x141d66cf0` | [0] dtor `0x140c6c050`, [2] set size `0x140c6d430`, [4] `0x140c6de20`, [5] draw `0x140c6de80`, [6] `0x140c6d320` |
| set size | `0x140c6d430` | `this+0xd8 = arg * 0.4` (`0x1419357dc` = 0.4) |
| build meshes | `0x140c6d0c0` | for face 0..4: `BuildFace(this, R = this+0xd8, face, e = 10.0 / R)` (`0x141923c40` = 10.0) |
| build one face | `0x140c6c0f0` | `switch(face)` over 0..4, else asserts "Invalid skybox face" (line 0x1d1) |
| set textures (one mode) | `0x140c6d4d0` | reads the mode's entry `{+8 NE, +0x10 SW, +0x18 T}` (template field order `texPathNE, texPathSW, texPathT`); makes three 1-texture materials at `this+0xdb8` (NE), `+0xdc0` (SW), `+0xdc8` (T) |
| set textures (two modes) | `0x140c6d850` | same, but two modes per material and the 2-sampler program: cross-fade |
| bind materials to meshes | `0x140c6e846..0x140c6e8e4` | face 0 and face 1 meshes (`this+0xf8`, `+0x100`) get NE; face 2 and 3 (`+0x108`, `+0x110`) get SW; face 4 (`+0x118`) gets T |
| draw | `0x140c6de80` | sets the per-face uniforms (§4, §5) and World = translate + scale 100 |

Programs (`this+0xd88/0xd98/0xda8` = byte size, `+0xd90/0xda0/0xdb0` = blob):

| Program | Blob VA, size | Shaders | Used |
|---|---|---|---|
| A | `0x141d63b50`, 0xd9c | PS 1617 + VS 1618 (identical to PS 1611 / VS 1612) | one sky mode |
| B | `0x141d648f0`, 0xe34 | PS 1619 + VS 1620 | cross-fade of two modes (`0x140c6d850`) |
| C | `0x141d65730`, 0x15bc | PS 1621 + VS 1622 | instead of A when the flag from `0x140d65440` is set (`cmove` at `0x140c6d635`); adds a scattering term and a sun disc. When the flag is set is **UNPROVEN**. |

---

## 1. Axes

**GW2 sky space is left-handed with X = east, Y = north, Z = down (up = −Z).**

Evidence:

1. *Up is −Z.* `BuildFace` (`0x140c6c0f0`) puts the top cap (face 4) at
   `z = −R`, the horizon edge of every side face at `z = 0`, and a
   below-horizon skirt at `z = +1.0` (`0x141923c3c`). VS 1618 treats
   `v0.z > 0` as "below the horizon" (`lt r0.x, 0, v0.z` → clamps world z to
   `max(z, 0)` and pushes those vertices to the far plane). PS 1617 applies
   horizon haze from `max(n.z, 0)` where `n = normalize(−view)`, so up is
   positive `n.z`, i.e. negative view z. This matches `MapEnvRig.sunDir`
   ("-Z is up", `include/castlemist/extract/map_types.h:37`) and
   `src/ripper/face_morphs.cpp:67`.
2. *North is +Y, east is +X.* The NE texture is bound to the +X and +Y side
   faces and SW to −X and −Y (binding table above). Panning right across the
   upright faces runs +X → −Y → −X → +Y (§2 geometry; seams measured in §2).
   The stored cube sky (§6) pans right N → E → S → W on both cube maps
   checked. The only assignment where both textures' names hold and the ring
   order matches is **N = +Y, E = +X, S = −Y, W = −X**.
3. *Left-handed.* Facing +Y (north) with up −Z, east (+X) is on the viewer's
   right. In a right-handed frame, right = forward × up = Y × (−Z) = −X, so a
   right-handed reading would mirror every face. Left-handed:
   right = up × forward = (−Z) × Y = +X. This holds as long as the game
   shows its sky unmirrored (east to the right of north).
4. *No yaw on the sky.* The draw (`0x140c6e19c..0x140c6e1d4`) sets the World
   transform with `0x140aa5eb0(2, {p.x, p.y, −verticalOffset})` (translation)
   and `0x140aa5060(2, 100.0)` (scale, `0x141923c48` = 100.0), with no rotation
   call between them. Sky directions are therefore world directions.

**Disagreement with the map glTF export:** `src/exportgltf/map_export.cpp:93`
and `gltf_export.h:55` turn GW2 → glTF with a fixed −90° about X, which sends
**+Z** to +Y (it assumes +Z up). The sky code above proves −Z up for the sky.
For the skybox only the sky frame matters, so the skybox export must **not**
reuse that root rotation. Whether the map glTF export is upside down is outside
this note. Check it before relying on a shared convention.

**Formula** (Unity: left-handed, +Y up, +Z forward; GW2 north → Unity +Z):

```
gw2_to_unity(x, y, z) = ( x, -z, y )          // proper rotation, LH -> LH, det = +1
unity_to_gw2(X, Y, Z) = ( X,  Z, -Y )
```

| GW2 | meaning | Unity |
|---|---|---|
| +X | east | +X |
| −X | west | −X |
| +Y | north | +Z (forward) |
| −Y | south | −Z |
| −Z | up | +Y |
| +Z | down | −Y |

Evidence: `0x140c6c0f0` geometry, the `0x140c6e846` texture binding, the §2/§6
seams, and `map_types.h:37`. Test: `gw2_to_unity(0,0,-1) == (0,1,0)`,
`gw2_to_unity(0,1,0) == (0,0,1)` (north), `gw2_to_unity(1,0,0) == (1,0,0)`
(east).

---

## 2. Panorama projection (skyModeTex NE / SW / T)

**NE, SW and T are not panoramas. They are the five faces of a hemicube.**
Each 512² side texture holds two cube faces. The upper half (v ∈ [0, 0.5]) is
one face, upright, with the zenith edge at v ≈ 0 and the horizon at v = 0.5. The
lower half (v ∈ [0.5, 1]) is another face **rotated 180°**: horizon at v = 0.5,
zenith edge at v ≈ 1, u reversed. That rotated half is the "reflection-like
lower half". T is the top face.

From `BuildFace` (`0x140c6c0f0`, vertices `{float3 pos, half2 uv}` written to
`0x1427f0fb0..`). The UV corner constants are function statics built from
`a = e`, `b = 1 − e`, `0.5`
(`0x140c6cd22..0x140c6cf99`: `half2(a,a)`, `(a,.5)`, `(a,b)`, `(b,a)`, `(b,.5)`,
`(b,b)`). The face quads, with R = cube half-size:

| face | plane | texture | vertices (pos → uv) |
|---|---|---|---|
| 0 | y = +R (north) | NE lower half | (−R,R,−R)→(b,b) · (−R,R,0)→(b,.5) · (R,R,0)→(a,.5) · (R,R,−R)→(a,b) · skirt (R,R,1)→(a,.5), (−R,R,1)→(b,.5) |
| 1 | x = +R (east) | NE upper half | (R,R,−R)→(a,a) · (R,R,0)→(a,.5) · (R,−R,0)→(b,.5) · (R,−R,−R)→(b,a) · skirt (R,−R,1)→(b,.5), (R,R,1)→(a,.5) |
| 2 | y = −R (south) | SW lower half | (R,−R,−R)→(b,b) · (R,−R,0)→(b,.5) · (−R,−R,0)→(a,.5) · (−R,−R,−R)→(a,b) · skirt z=1 at v=.5 |
| 3 | x = −R (west) | SW upper half | (−R,−R,−R)→(a,a) · (−R,−R,0)→(a,.5) · (−R,R,0)→(b,.5) · (−R,R,−R)→(b,a) · skirt z=1 at v=.5 |
| 4 | z = −R (top) | T | (−R,−R)→(a,a) · (−R,R)→(a,b) · (R,R)→(b,b) · (R,−R)→(b,a) |

UVs are affine in the plane coordinates (each quad is a parallelogram in uv),
so the mapping is linear across each face. The skirt runs from the horizon
down to z = +1 at constant v = 0.5, so below the horizon the sky is the
horizon row stretched down. The texture's lower half is not the lower part of
the cube.

**Inset `e`:** `e = 10 / R`, `R = this+0xd8 = 0.4 × (vtable slot 2 argument)`.
Round 2 traced the size call: the EnvContext constructor `0x140c40d40` calls
slot 2 with its own float argument `F0` (`0x140c413a9..0x140c413af`; `xmm6`
holds `F0` from `0x140c40d6f` to the call), so `R = 0.4·F0` and
`e = 25 / F0`. Round 3 derived `F0` from the map's terrain data (§7.1):
`F0 = 3072 · max(3, round(swapDistance / 3072))`, which is 36864 for 187611,
so **`e = 25/36864 = 6.8e-4` = 0.35 texel of 512 (PROVEN from code + data)**.
The seam measurement below prefers a larger inset. A finer re-run (scratch
`inset.py`: edge strips read with D3D bilinear, pixel = uv·512 − 0.5, clamp)
gives mean seam error 4.30 for every inset ≤ 0.5 texel (they all read the
outermost texel), 2.68 at 1.0, a minimum of **2.10 at 1.4 texels**, 2.27 at
2.0. **The two do not agree.** The derived 0.35 texel sits in the flat region,
the same as inset 0. The textures appear to be authored with about 1.4 texels
of edge overlap that the game does not remove, so the game itself shows the
seams that inset 0 shows. The table below is the round-1 measurement.

| inset (texels) | 0 | 1 | 2 | 3 | 5 | 10 | 20 |
|---|---|---|---|---|---|---|---|
| mean seam diff, 6 seams | 3.97 | **2.07** | 2.82 | 3.58 | 5.04 | 8.26 | 13.57 |

Round 1 recommended `e = 1 / W` (measured, not derived). To match the game, use
the derived `e = 25 / F0` (§7.1). Use `1.4 / W` only to hide the authored seams
on purpose, and say so in `warnings`.

**Seam check** (187611 mode 0, inset 0; columns = seam, control):

| seam (3D edge) | diff | control |
|---|---|---|
| (+R,−R) vertical: NE upper right col ↔ SW lower right col, rows mirrored | 5.34 | 19.84 |
| (+R,+R): NE upper left col ↔ NE lower left col, rows mirrored | 4.77 | 41.32 |
| (−R,+R): SW upper right col ↔ NE lower right col, rows mirrored | 5.52 | 51.23 |
| (−R,−R): SW upper left col ↔ SW lower left col, rows mirrored | 5.04 | 47.64 |
| top y=+R: T bottom row ↔ NE bottom row reversed | 2.83 | 41.17 |
| top x=+R: T right col ↔ NE top row reversed | 4.05 | 24.98 |
| top y=−R: T top row ↔ SW bottom row | 4.22 | 12.45 |
| top x=−R: T left col ↔ SW top row | 2.62 | 22.92 |

All eight seams are continuous. NE rows 255 and 256 differ by 31.9, about as
much as two random rows (34.6). The two halves of one texture are not
continuous across the middle row; they only meet at the corner columns. This is
expected: they are two different faces.

**Formula** (inverse, as the sampler needs it). GW2 sky direction `d = (x, y, z)`
(§1 frame), `up = −z`, texture `v = 0` is the top row (D3D; the decoded PNG row
0 is v = 0, which the seam table relies on). `L(t, u, v)` is a bilinear lookup
with clamp, and `a = e`, `b = 1 − e`.

```
ax = |x|; ay = |y|; m = max(ax, ay)
if (up > 0 && up >= m) {                       // top cap, face 4, texture T
    X = x / up;  Y = y / up;                   // in [-1, 1]
    return L(T,  a + (b-a)*(1+X)/2,  a + (b-a)*(1+Y)/2);
}
// side face; below the horizon the skirt repeats the horizon row
U = (up > 0) ? up / m : 0;                     // 0 at horizon .. 1 at cube edge
if (ax >= ay) {                                // east / west
    Y = y / ax;
    if (x > 0) return L(NE, a + (b-a)*(1-Y)/2,  a + (0.5-a)*(1-U));  // face 1, east, upper half
    else       return L(SW, a + (b-a)*(1+Y)/2,  a + (0.5-a)*(1-U));  // face 3, west, upper half
} else {                                       // north / south
    X = x / ay;
    if (y > 0) return L(NE, a + (b-a)*(1-X)/2,  0.5 + (b-0.5)*U);    // face 0, north, lower half
    else       return L(SW, a + (b-a)*(1+X)/2,  0.5 + (b-0.5)*U);    // face 2, south, lower half
}
```

Within one half, clamp `v` to that half ([a, 0.5] or [0.5, b]) before the
bilinear fetch so the two faces don't bleed into each other at v = 0.5.

Evidence: `0x140c6c0f0` (cases at `0x140c6c22e` face 4, `0x140c6c335` face 3,
`0x140c6c51e` face 2, `0x140c6c70b` face 1, `0x140c6c8f6` face 0), the bindings
at `0x140c6e846..0x140c6e8e4`, VS 1618 (uv passed through: `mov o0.xy, v1.xy`),
and the seam table.

---

## 3. Sky modes

The draw (`0x140c6de80`) receives two factors, both clamped to [0, 1]:

- `t` (5th argument, `[rbp+0x60]`). It is the day/night lerp weight for every
  sky parameter: `value = night + (day − night)·t` (§5). So **t = 1 is day,
  t = 0 is night**. Round 2: the caller passes `EnvContext+0x1050`
  (`0x140c45424`, `0x140c45474`; it also passes the sun direction
  `[rbp+0x48]` in `r9` and a pointer to the sun colour `[rbp+0x58]` on the
  stack, which the callee reads as `[rbp+0x50]`; §7), and that is the same
  field the stars,
  clouds and cards read through EnvContext vtable slot `0xc0`
  (`0x140c431f0: movss xmm0,[rcx+0x1050]`; vtable `0x141d5e590`). **One `t`
  drives every sky layer.**
- `k` (6th argument, `[rbp+0x68]`). Round 2: the caller passes
  `EnvContext+0x105c` (`0x140c4543c`, `0x140c45481`), which the frame function
  gets back as an out-parameter of `0x140c58540` (`0x140c44e55`). What it means
  is still **UNPROVEN**.

In steady state (no transition), `0x140c6e7a0` picks the mode
(`0x140c6e9d9..0x140c6ea47`, calls into `0x140c6d4d0`):

| t | k | mode index | parameters used | name |
|---|---|---|---|---|
| 1 | ≠1 | **0** | `day*` | **day**, PROVEN |
| ≠1 | ≠1 | **1** | `night*` | **night**, PROVEN |
| 1 | 1 | 2 | `day*` | day variant of the unknown `k` state, UNPROVEN |
| ≠1 | 1 | 3 | `night*` | night variant of the unknown `k` state, UNPROVEN |

While t or k is changing, the renderer cross-fades between two modes with
program B (pairs (0,2), (1,3), (2,3), (0,1), set up at `0x140c6eb35`,
`0x140c6ed1b`, `0x140c6ed26`). The blend weight goes into `skypara.w` (PS 1619:
`tex = lerp(tex1, tex0, skypara.w)`). A static bake uses no cross-fade.

Data: 187611 stores mode 3 == mode 0. 3264516 fills modes 0–1 only. Both fit
the table. Neither says what `k` is.

**Attribute sets per mode:**

- Sky params (`PackMapEnvDataSky`): modes 0 and 2 use `day*`, modes 1 and 3
  use `night*` (t = 1 / t = 0). PROVEN by the lerps in §5.
- Sky cards (`day` / `night` attributes): **PROVEN**, same `t`. The card draw
  `0x140caa330` lerps every per-card value as `night + (day − night)·t`
  (`0x140caa6fc..0x140caa7cf`; brightness, density, hazeDensity,
  lightIntensity, minHaze, scale), and the direction helper `0x140ca6ec0`
  does the same for azimuth and latitude (`0x140ca70f3..0x140ca7139`). The
  texture switch `0x140ca9d00` binds the day texture and day program at
  `t = 1`, the night ones at `t = 0`, and a two-texture cross-fade program in
  between (§9).
- Cloud layers: **PROVEN.** The import `0x140c87d20` asserts
  `srcLayer->attributeCount == MAP_ENV_GROUP_TYPES` (3, `0x140c87e1a`) and
  keeps all three sets. The draw `0x140ca4050` uses
  `lerp(attributes[1], attributes[0], t)` (`0x140ca4300..0x140ca437b`), so
  **`attributes[0]` is day and `attributes[1]` is night**. `attributes[2]`
  replaces both, unblended, only when `camera.z > level`
  (`0x140ca42cb..0x140ca42fb`), where `level` is a float the frame function
  receives (`[rbp+0x248]`, §7). What that level is (water surface?) is
  **UNPROVEN**. In 187611 every layer's `attributes[2]` is brightness 1,
  density 1, lightIntensity 1, which fits an "alternate" set rather than a
  third time of day. These 3 sets are not the 3 lighting presets of
  `gw2-map-lighting.md`.

**Table:** recommended output names: `mode0` → `day`, `mode1` → `night`;
`mode2` and `mode3` keep their index names. Day (`t = 1`) takes every `day*` /
`day.*` / `attributes[0]` value; night (`t = 0`) takes every `night*` /
`night.*` / `attributes[1]` value, for the hemicube, stars, cards and clouds
alike. Modes 2/3 differ from 0/1 only by `k`, which none of the round-2 layers
read.

---

## 4. Layer order and blending

The hemicube is one draw per face of program A (or C), so the "base" layer and
its haze are a single pass. Stars, sky cards and cloud layers are separate
systems, traced in round 2 (§7–§11).

| Layer | Status | What is known | What is missing |
|---|---|---|---|
| base hemicube (NE/SW/T) | **PROVEN** | §2 projection, §5 colour maths | — |
| horizon haze | formula **PROVEN**, input `FogColorFar` **UNPROVEN** | §5 | `FogColorFar` is an engine global; none of the sky draws sets it (§11) |
| sun glow inside the base pass | formula **PROVEN**, inputs **PARTIAL** | §5; round 2: sun direction = `EnvContext+0x4f0`, sun colour = `EnvContext+0x4e4`, copied once per frame and handed to every sky layer (§7) | who writes `EnvContext+0x4e4..0x4f8`; the SH globals `shRed/Green/Blue` |
| stars (`starFile`, `*StarDensity`) | **PROVEN** except the twinkle phase (size from `F0`, §7.1) | §8 | the per-vertex RNG and the `Time` uniform for twinkle |
| sky cards (texture cards) | **PARTIAL**: direction, UV, size (`F` = `F0` at load, §7.1), day/night, colour, blend **PROVEN**; haze/sun inputs as for the base | §9 | `FogColorFar`, SH, sun colour (§11) |
| sky cards (material cards) | **UNPROVEN** for baking | §9.6: AMAT, constants and textures are all resolvable statically | evaluating that AMAT's pixel shader (needs `Time`, `TimeOfDay`) |
| cloud layers | **PARTIAL**: plane geometry, size (`F`, §7.1), UV, attribute choice, colour maths, blend **PROVEN**; look needs camera height and the engine fog uniforms | §10 | camera height, `FogParam0`/`FogColor*`, the cloud fade factor |
| layer order on the GPU | **UNPROVEN** | submission order and per-program state words (§7) | the bgfx sort key, or one frame capture |

Data notes (187611 parsed with `gw2dat_cli parse`; 3264516 from round 1):

- `textureUV` is `(uLeft, uRight, 1 − vTop, 1 − vBottom)`, **PROVEN** from the
  card mesh builder (§9.3). `(0, 1, 1, 0)` is the whole texture upright;
  `(0, 0.279, 0.732, 0.459)` (187611 card 1, drawn at night only: day density 0) is the atlas rectangle
  u 0..0.279, v 0.268..0.541.
- 3264516: one card has a day texture (186341, 64² DXT5). Four have texture 0
  and material fileId 3135800, so a material draws them, not a texture (§9.6).
  **Leave material cards out of the bake.**
- 187611 card 0 (day latitude 0.41, scale 3.2, texture 186341) has
  `flags = 2`, the flag that hides a card while program C (the scattering
  program with its own sun disc) is active (§9.4). That fits a sun card.
- 3264516 `sky.verticalOffset = −100000`. In the draw that becomes a world
  translation of `−verticalOffset` along z (§1 item 4). It moves the hemicube
  origin, which changes the view only through the VS horizon clamp. A sky at
  infinity is unaffected. **For a bake, ignore verticalOffset.**

**Table:** what the baker may add now.

| Layer | Bake? | Condition |
|---|---|---|
| base hemicube | yes | §2 + §5 (unchanged) |
| stars | yes | §8 formula with `F0` from §7.1; twinkle `tw` goes into `warnings` as an assumed value |
| texture sky cards | yes, `F = F0` (§7.1) | §9 formula with `FogColorFar`-dependent haze and sun light dropped (see the reduced formula); warn when `hazeDensity·sky.HazeDensity`, `minHaze` or `lightIntensity·sky.LightIntensity` is non-zero |
| material sky cards | no | warn |
| clouds | only with a stated camera height, `cloudFade` and fog = 0 (`F = F0`, §7.1) | §10 formula; those three go into `warnings` |

Static frame: cloud scroll offsets and the card rotation accumulator both
start at 0 when the map loads (§9.2, §10.2), so "time 0" = offset 0 is the
game's own initial state, not a guess. Twinkle has no such zero (§8.4).

---

## 5. Brightness and range

Program A's pixel shader (PS 1617 = PS 1611, 39 instructions), with uniform
registers from its bgfx table: `FogColorFar` cb0[0], `shRed/Green/Blue`
cb0[1..3], `skypara` cb0[5], `sunDir` cb0[6], `sunClr` cb0[7].

What the draw writes into them (`0x140c6e05e..0x140c6e28b`). The sky params
struct is the runtime copy of `PackMapEnvDataSkyV76`: flags, then day ×6 at
+4..+0x18, night ×6 at +0x1c..+0x30, verticalOffset at +0x34. +0x34 is
confirmed by its use as −z translation.

| uniform (hash) | .x | .y | .z | .w |
|---|---|---|---|---|
| skypara (`0x36262728`) | HazeBottom | HazeFalloff ¹ | HazeDensity | mode cross-fade weight |
| sunDir (`0x3045e8da`) | normalize(sun).x | .y | .z | **Brightness** |
| sunClr (`0x304647e8`) | colour.r | .g | .b | LightIntensity |

Each value is `lerp(night, day, t)`. ¹ HazeFalloff uses weight `1 − sqrt(1 − t)`
instead of t; at t ∈ {0, 1} that is the same endpoint. Which id is which
name: the register `.w` uses match the shader (`skypara.w` is the cross-fade in
PS 1619, `sunDir.w` scales the texture), and round 2 decoded the ids: they are
base-23 Tokens, not hashes (`0x36262728` → `skypar[a]`, `0x3045e8da` →
`sun[D]ir`, `0x304647e8` → `sun[C]lr`; Evidence base).

**Formula** (PS 1617, line by line; `tex` = `t0` at the mesh uv):

```
n    = normalize(v1)                  // v1 = CameraPosition - 100*localPos (VS 1618)
                                      // = -viewDir for a sky centred on the camera
T    = tex.rgb * tex.a * sunDir.w     // premultiplied by alpha, times Brightness
// sun glow (needs sunClr, SH, sunDir)
g    = (1 - n.z) + sunDir.z * n.z     // = mad(sunDir.z, 1-(1-n.z), 1-n.z)
g   *= sq(dot(sunDir.xyz, n) * 0.25 + 0.75)
amb  = float3(dot(float4(-n,1), shRed), dot(..., shGreen), dot(..., shBlue))
C    = lerp(amb, sunClr.rgb, g)
P    = C*T
X    = P + sq(dot(T, (0.3,0.59,0.11))) * (T + C - 2*C*T)
M    = 0.5*X + 0.5*T
G    = lerp(M, X, g)
col  = T + sunClr.w * (G - T)         // LightIntensity = 0  ->  col = T
// horizon haze
f    = saturate((max(n.z, 0) - skypara.x) / skypara.y)   // HazeBottom, HazeFalloff
f    = (1 - f*f*(3 - 2*f)) * skypara.z                   // HazeDensity
out.rgb = lerp(col, FogColorFar.rgb, f)
out.a   = 0
```

So with `LightIntensity = 0` and `HazeDensity = 0` the sky pixel is exactly
`tex.rgb · tex.a · Brightness`. Program C adds a scattering factor and a sun
disc `pow(max(dot(view, sunDir),0), 256) · sunClr · 128` on top
(PS 1621, last 12 instructions). It is not used in the bake (when C is active
is UNPROVEN).

**Range:** the PS writes unclamped float colour (render target format not
traced). Brightness > 1 can push values over 1. The engine's later
exposure/tonemap (compute shaders `ExposureControl`, `HistParams`;
`gw2-exe-shaders.md`) is **UNPROVEN** for the sky. The closest honest 8-bit
mapping is the plan's `round(clamp(c, 0, 1) · 255)`.

**Formula for the bake:**

```
radiance(dir) = clamp( L(dir).rgb * L(dir).a * Brightness , 0, 1 )
Brightness    = dayBrightness   (modes 0, 2)
              = nightBrightness (modes 1, 3)
```

Leave out sun glow (needs sun colour + SH) and haze (needs FogColorFar), and
name both in `warnings`.

Evidence: PS 1617/1611 and PS 1619 disassembly; the draw `0x140c6de80`
(lerps at `0x140c6e05e..0x140c6e0f3`, uniform sets at
`0x140c6e1e7..0x140c6e28b`); template `PackMapEnvDataSkyV76` field order.

---

## 6. Cube sky faces (skyModeCubeTex E / W / N / S / B / T)

The renderer for `skyModeCubeTex` was not found (`BuildFace` only knows faces
0..4). The mapping below comes from the textures themselves, whose names are
compass directions. It is tied to §1's compass frame (N = +Y, E = +X), which
§1 derives from the hemicube.

**Ring:** the side faces as stored, upright. Right column of A vs left column
of B:

| A → B | 3264516 | 3134778 | worst other pairing |
|---|---|---|---|
| N → E | 0.99 | 2.06 | ≥ 11.5 |
| E → S | 1.10 | 1.56 | ≥ 13.4 |
| S → W | 1.76 | 1.93 | ≥ 14.9 |
| W → N | 1.11 | 2.54 | ≥ 16.1 |

Panning right goes N → E → S → W on both maps, with no flip or rotation of
the side faces.

**Top:** tested in all 8 orientations against the side faces' top rows. Best,
on both maps: **as stored (no rotation)**, with the bottom edge against N, the
right edge against E, the top edge against S and the left edge against W
(mean diff 1.04 on 3264516, 0.90 on 3134778). The other three rotations score
the same only because they relabel the ring.

**Bottom:** B is a flat colour on 3264516 (52,64,26, std 0) and nearly flat
with flat edges on 3134778. Its seam score is the same for every orientation,
so B's orientation **cannot be checked on any known map**. The assignment
below uses Unity's NY convention (image top edge toward +Z = north). Since B
is flat, a wrong guess can't show.

Unity `Skybox/6 Sided` face table (`sky_project.h`, Unity axes):

| Face | forward | right | up | slot |
|---|---|---|---|---|
| PZ | +Z | +X | +Y | `_FrontTex` |
| NZ | −Z | −X | +Y | `_BackTex` |
| PX | +X | −Z | +Y | `_LeftTex` |
| NX | −X | +Z | +Y | `_RightTex` |
| PY | +Y | +X | −Z | `_UpTex` |
| NY | −Y | +X | +Z | `_DownTex` |

**Table** (with `gw2_to_unity` from §1: N → +Z, E → +X, up → +Y):

| stored | index in `cube[]` | Unity face | reorientation | check |
|---|---|---|---|---|
| E | 0 | **PX** | none | PX right = −Z = south; E's right neighbour is S ✓ |
| W | 1 | **NX** | none | NX right = +Z = north; W's right neighbour is N ✓ |
| N | 2 | **PZ** | none | PZ right = +X = east; N's right neighbour is E ✓ |
| S | 3 | **NZ** | none | NZ right = −X = west; S's right neighbour is W ✓ |
| B | 4 | **NY** | none (unverifiable, B flat on all known maps) | NY up = +Z = north |
| T | 5 | **PY** | none | PY up = −Z = south, so the image top edge meets S and the bottom edge meets N ✓; right = +X = east ✓ |

All six faces go out unrotated and unflipped; only the file name changes
(`E→px, W→nx, N→pz, S→nz, B→ny, T→py`).

**Cross-check with §2:** the hemicube's east face (face 1, NE upper half) has
image right = −Y = south. Under `gw2_to_unity` that is Unity −Z, the same as
PX's right. The baked faces and the raw cube agree in yaw and handedness.

Evidence: decoded faces 3263205..3263215 (3264516) and 3265586..3265596
(3134778, from `gw2dat_cli parse` of its env chunk), with the seam scores
above. 3194054 points at the same faces as 3264516.

---

## 7. Shared frame inputs (EnvContext)

All four sky systems hang off one EnvContext object (constructor `0x140c40d40`,
vtable `0x141d5e590`, 0x13a8 bytes allocated at `0x140c5213c`). Its per-frame
function is vtable slot `0x50` = `0x140c43f80`, which hands the same inputs to
every layer.

| Input | Where it lives | Who reads it | Status |
|---|---|---|---|
| `t` (day weight) | `EnvContext+0x1050`, getter slot `0xc0` `0x140c431f0` | hemicube (callee `[rbp+0x60]`), stars `0x140c6fd09`, clouds `0x140ca41e7`, cards `0x140caa3de` | **PROVEN** shared; 1 = day, 0 = night |
| `k` | `EnvContext+0x105c` | hemicube only (callee `[rbp+0x68]`) | value **UNPROVEN** (out-param of `0x140c58540`) |
| `F` (sky distance) | `EnvContext+0x1058`, getter slot `0x80` `0x140c42a00`, setter slot `0x110` `0x140c43a10` | cloud plane size `0x140ca40e5`, card quad distance `0x140ca7320` | **= `F0`** unless the setter runs (§7.1) |
| `F0` | constructor argument `xmm2` | `+0x1058 = F0` (`0x140c40f54`), hemicube size `R = 0.4·F0` (`0x140c413af`), star radius `0.5·F0` (`0x140c413b2..0x140c413d7`) | **PROVEN**: `3072·max(3, round(swapDistance/3072))` from the map's `trn` chunk (§7.1) |
| sun direction | `EnvContext+0x4f0..0x4f8`, copied to the frame at `0x140c451ff`/`0x140c45232` | every layer normalizes it into `sunDir.xyz` | source **UNPROVEN** |
| sun colour | `EnvContext+0x4e4..0x4ec` (+ w = 1), copied at `0x140c45237..0x140c4526a` | `sunClr.rgb` of hemicube, cards, clouds | source **UNPROVEN** |
| camera position | frame argument `[rbp+0x240]` | world translation of stars, cards and clouds; cloud plane centre | runtime |
| `level` | frame argument `[rbp+0x248]` | clouds (`attributes[2]` switch), cards (flags 4 / 0x80) | meaning **UNPROVEN** |
| sky params | per-frame copy of `PackMapEnvDataSkyV76` at `[rbp+0x70]` (flags, day ×6, night ×6, verticalOffset) | hemicube, stars, cards, clouds | **PROVEN** layout (§5) |

`F0` and `F` start equal. The card quad is built once at map load
(`0x140ca7280`), so cards use `F` as it was then. Whether anything calls the
setter later is §7.1.

### 7.1 The sky distance `F0` (PROVEN)

The chain, from the EnvContext back to the map file:

1. The Environment component creator `0x140c520e0` asks its map for component
   `0x18` (`[rcx]->vfunc 0x130(0x18)`, `0x140c520f8..0x140c5210b`), calls its
   slot `0x68` (`0x140c52117`) and passes the float to the EnvContext
   constructor as `xmm2` (`0x140c5216f`). The map-component table in
   `.rdata` (one `{id, wchar* name, …, create, …}` record per component, e.g.
   Environment at `0x141d5f898`) names id `0x18` **TerrainClient**
   (`0x141d5fe48`, create `0x140c55690`) and id `0x17` **Terrain**
   (`0x141d5d200`, create `0x140c36510`).
2. TerrainClient (vtable `0x141d5fea0`, constructor `0x140c55800`) slot `0x68`
   = `0x140ca2e30`: returns `(float)([+0x3f8] · [+0x364] · [+0x358])`.
3. Those three ints belong to the TerrainClient sub-object at `+0x288`
   (`+0xd0`, `+0xdc`, `+0x170`). `0x140c7d580` (called at `0x140c55993` with
   the TerrainClient itself as the source) sets `+0xd0 = TC->vfunc 0x78()`,
   `+0xdc = TC->vfunc 0x98()`, and `+0x170 = round(24576 / (a·b))`
   (`0x141d5d878` = 24576). Then the TerrainClient creator reads the map's
   `trn` chunk (`mov edx,0x6e7274` at `0x140c5571d`) and calls `0x140c7ed50`
   with its float at +8 (`0x140c55734`), which sets
   `+0x170 = max(3, round(value / (a·b)))` (`0x140c7ed50..0x140c7edbb`;
   rounding helper `0x1409c1b10`). The `trn` chunk is `PackMapTerrainV15`
   (`dims` dword2, then **`swapDistance`** float at +8).
4. TC slots `0x78` and `0x98` forward to the Terrain component's interface
   (`[TC+0x18]`, component `0x17`, stored at `0x140c5584f`) slots `0x90` and
   `0xb0`. On the interface at Terrain+0x20 (vtable `0x141d5d258`) those read
   `Terrain+0x1c8 = V` and `Terrain+0x1cc = 0xc00 / V` (written by the Terrain
   constructor, `0x140c36680` and `0x140c36702..0x140c36710`). This is the only
   interface whose slots there are plain integer getters; the primary vtable's
   slot `0x90` returns a struct. So `a·b = (3072 div V)·V`, which is **3072**
   whenever V divides 3072 (V = `verticesPerChunkSide`, 32 in the V15 maps
   read).

```
F0 = 3072 · max(3, round(trn.swapDistance / 3072))     // 24576 if no trn chunk is read
F  = F0 at load;  R_hemicube = 0.4·F0;  R_stars = 0.5·F0;  e = 25 / F0
```

It is a **map value, not a user setting.** No graphics option or camera
far-plane value is on the path.

| map (mapc fileId) | `trn.swapDistance` (`gw2dat_cli parse`) | `F0` | hemicube `e` (texels of 512) |
|---|---|---|---|
| 187611 | 36864 | 36864 | 0.35 |
| 3134778 | 24576 | 24576 | 0.52 |
| 3264516 | 70656 | 70656 | 0.18 |
| 3194054 | 70656 | 70656 | 0.18 |

All four swap distances are exact multiples of 3072, so `F0 = swapDistance`.

**The setter.** `+0x1058` has only two writers in `0x140c00000..0x140e00000`:
the constructor (`0x140c40f54`) and EnvContext slot `0x110` (`0x140c43a10`,
which stores `xmm1` if it differs). A byte scan of all of `.text` for
`call [reg+0x110]` found 308 sites; 10 load a float into `xmm1` first. Four of
them are model-animation controls (`ModelAnimDeferredControl.cpp`,
`ModelAnimSmartDeferredControl.cpp`), and none could be tied to an EnvContext
receiver. Most likely the setter is never called, so `F = F0` for the whole
session, but that is **not proven**. The second sky object (`EnvContext+0x9b8`,
created at `0x140c458ad` as a hemicube or, via `0x140c68aa0`, the
`skyModeCubeTex` renderer) is also sized from `+0x1058`
(`0x140c458f7..0x140c45909`).

**Submission order** inside `0x140c43f80`: for each environment, the cloud
wrapper (`0x140c633c0` → cloud draw `0x140ca4050`) and the card wrapper
(`0x140c635a0` → card draw `0x140caa330`), at `0x140c45296`/`0x140c452d4` and
in the loop `0x140c45370..0x140c453f6`; then the program-C flag is pushed to
the hemicube (`0x140c453fc..0x140c45421`, vtable slot 3); then the two
hemicube objects (`+0x9b0`, `+0x9b8`, `0x140c45465`, `0x140c454ae`); then the
stars (`0x140c454bf`). bgfx sorts draws before submitting them, so this is not
proven to be the GPU order. The program creation calls pass different state
words and sort-like arguments (`0x140aab1f0` 5th/6th arguments: hemicube
`0x4086400`/3, stars `0x86000`/3, cards `0xc086000`/3, clouds `0x86000`/10),
and decoding them was not done. **Layer order: UNPROVEN.** The order that
fits the blend states is hemicube (opaque) → stars (additive) → cards → clouds
(both alpha-blended), but that is an assumption.

---

## 8. Stars

### 8.1 Code

| What | Where | Finding |
|---|---|---|
| star object | `EnvContext+0x9c0`; `+0x8` = EnvContext (`0x1405d0510`, called at `0x140c412ed`), `+0x10` = radius `0.5·F0`, `+0x20` = material | |
| load | `0x140c6fa90` | opens `starFile` as a packfile (`0x140de65b0`), takes chunk `'STAR'` (`mov edx,0x52415453` at `0x140c6fb17`), builds the mesh (`0x140c6ee10`), loads the chunk's texture (`0x140a763f0`), makes the program from blob `0x141d66df0` (size `0xa64`, PS 1624 + VS 1623) |
| mesh | `0x140c6ee10` | 4 vertices + 6 indices per star; vertex = `{float3 pos, u32 colour, half2 uv}` |
| draw | `0x140c6fcd0` | `skypara = (HazeBottom, HazeFalloff, HazeDensity, StarDensity)`, all `lerp(night, day, t)` (HazeFalloff with weight `1 − sqrt(1 − t)`); hides the mesh when StarDensity = 0 (`0x140c6fd3b..0x140c6fd48`); World = translate(camera) only (`0x140c6fe13..0x140c6fe20`) |
| blend | AMAT of blob `0x141d66df0` | render state `0x2222000` = **ONE, ONE** (additive), default and transparent effect alike |

### 8.2 `starFile` format

`starFile` 187544 is a **`PF` packfile with container and chunk `STAR`**
(`gw2dat_cli sniff`: `containerType "STAR"`, 40844 bytes), not a texture,
which is why `gw2dat_cli texture` rejected it with "ATEX: bad magic". The
loader opens it with the generic packfile reader. Chunk `STAR` v0, layout as
the loader reads it (`0x140c6fb32..0x140c6fb67`):

| offset | type | meaning | 187544 |
|---|---|---|---|
| +0 | float | `S`, size scale | 0.125 |
| +4 | u32 | star count | 1699 |
| +8 | ptr | star array, 24 bytes each | |
| +0x10 (file +0xc) | filename | atlas texture | fileId **187543** |

Star = 6 floats `{e0, e1, u0, u1, v0, v1}`: `e0` azimuth and `e1` elevation in
radians (187544: e0 ∈ [−1.557, 4.702], e1 ∈ [0, 1.549], none below the
horizon), and the star's rectangle in the atlas (every star has its own
rectangle; 1699 distinct). The texture reference is the filename words
`(0xdf96, 0x0102)`, i.e. fileId `0xff00·(0x102−0x100) + (0xdf96−0x100) + 1 =
187543` (castlemist's `content_schema.cpp` formula). The atlas 187543 is a
plain **DDS** (`gw2dat_cli sniff`: magic `DDS `), 256², one mip,
uncompressed A8R8G8B8 (masks R `0xff0000`, G `0xff00`, B `0xff`,
A `0xff000000`), 262272 bytes; castlemist's ATEX decoder does not read it, a
DDS reader does. v = 0 is the first row.

### 8.3 Placement (PROVEN)

The mesh builder puts each star in the plane `x = R` (R = star radius), with
corners (`0x140c6f0ac..0x140c6f1ca`, constant 2500 at `0x141b8bf6c`):

```
hu = 2500 · S · (u1 − u0);   hv = 2500 · S · (v1 − v0)
(R, +hu, −hv) → (u0, v0)     (R, +hu, +hv) → (u0, v1)
(R, −hu, −hv) → (u1, v0)     (R, −hu, +hv) → (u1, v1)
```

scales all four corners to length R (`0x140c6f219..0x140c6f2b4`; equal
lengths, so the quad stays planar), then applies `M2 · M1` with
`M1 = rot(axis (0,−1,0), e1)` and `M2 = rot(axis (0,0,1), e0)`
(`0x1409cb0e0` builds both; `0x140c6f2bf..0x140c6f423` applies them as
`p' = M·p` with the rows it builds). Worked out:

```
c(e0,e1)  = ( cos e1·cos e0, −cos e1·sin e0, −sin e1 )   // star centre (local +X)
eL(e0)    = ( sin e0,  cos e0, 0 )                       // local +Y: the u0 side
eD(e0,e1) = ( sin e1·cos e0, −sin e1·sin e0, cos e1 )    // local +Z: the v1 side (down)
```

So `e1` is elevation above the horizon (up = −Z) and `e0 = 0` points east,
growing toward **south** (clockwise seen from above). That is the opposite
turn to the sky cards (§9.2), and both are read straight from their own code.
Check: at `e0 = e1 = 0` the star faces east, `eL` = north = the viewer's left
in the §1 frame, `eD` = down, so the sprite is upright and unmirrored.

### 8.4 Colour (PROVEN except the twinkle phase)

VS 1623: `o1.xyz = normalize(World·pos)` = direction `d`;
`o1.w = tw` (twinkle) from the vertex colour and `Time`:

```
p   = (col.r + col.g + col.b) + Time.xz · col.a     // two phases (col in 0..1)
q   = frac(p)·2 − 1
q   = frac(q · 2.223)·2 − 1
tw  = (|q.x|²·(3 − 2|q.x|)) · (|q.y|²·(3 − 2|q.y|))
```

The colours are pseudo-random bytes from an RNG seeded with 1337
(`0x140c6f303`: `0x140e1e260(rng, 0x539)`), `r,g,b = rand·255`,
`a = rand·128·(1 − min(500·S²·(u1−u0)(v1−v0), 1))` (`0x140c6f0a2`,
`0x140c6f428..0x140c6f49e`, constants 500 `0x14192c3e8`, 255 `0x14192ab7c`,
128 `0x1419357f4`). The RNG (`0x140e1e6a0`) and the `Time` value were not
decoded, so the phase of a given frame is **UNPROVEN**; `tw ∈ [0, 1]`.

PS 1624 (lines in order) with `T = atlas(u, v)`:

```
f    = saturate((|d.z| − skypara.x) / skypara.y)        // HazeBottom, HazeFalloff
att  = 1 − (1 − f²(3 − 2f)) · skypara.z                 // 1 − horizon-haze weight of §5
B    = 2 · T.rgb²
rgb' = B + (2/3) · (B.r + B.g + B.b) · T.a · tw
out  = att · skypara.w · (rgb', T.a)                    // skypara.w = StarDensity
dst += out                                              // ONE, ONE
```

### 8.5 Sampler formula

Inputs: `starFile` STAR chunk (S, stars), atlas DDS, `StarDensity`,
`HazeBottom/HazeFalloff/HazeDensity` for the mode (day: `day*`, night:
`night*`), and `R = 0.5·F0`.

```
add(d) = 0
for each star (e0, e1, u0, u1, v0, v1):
    x = dot(d, c);  if (x <= 0) continue
    a = dot(d, eL) / x;   b = dot(d, eD) / x
    au = hu / R;  av = hv / R                    // tangent half-extents
    if (|a| > au || |b| > av) continue
    u = u0 + (u1 − u0) · (1 − a/au) / 2
    v = v0 + (v1 − v0) · (1 + b/av) / 2
    T = atlas(u, v)
    B = 2·T.rgb²
    add += B + (2/3)·(B.r+B.g+B.b)·T.a·tw
f   = saturate((|d.z| − HazeBottom) / HazeFalloff)
add *= (1 − (1 − f²(3−2f))·HazeDensity) · StarDensity
radiance = base(d) + add          // base = §5 hemicube output, before the 8-bit clamp
```

`tw` is not reproducible; bake `tw = 0` (the steady part) and say so in
`warnings`. `R = 0.5·F0` with `F0` from §7.1, so the star size is **PROVEN**
too (187611: `R` = 18432, the largest star's half-size 41.5 → 0.13°).

---

## 9. Sky cards

### 9.1 Code

| What | Where | Finding |
|---|---|---|
| import | `0x140c899c0` | packfile card (`PackMapEnvDataSkyCardV78`, 0xca bytes packed) → 0x468-byte card, fields copied as is (`0x140c89b30..0x140c89c34`) **when the import transform is identity** (`0x140c8b1d0` tests the 3×4 matrix). Otherwise (`0x140c89f04..0x140c8a0ff`) the day azimuth/latitude are read as degrees (×0.0174533), rotated through that matrix and written back in degrees (×57.2958) into **both** the day and the night slots. Which maps take that path is **UNPROVEN**; the formulas below are for the identity path |
| init | `0x140ca8460` | 0x4e0-byte runtime card per entry; latitude is **multiplied by π/2** (`0x141d8ab20` = 1.5708; day `0x140ca8577`, night `0x140ca876a`), azimuth is copied as is; texture cards get the quad from `0x140ca7280`, material cards (non-empty material path) get `0x140ca7740` |
| per-frame texture/program | `0x140ca9d00` | `t = 1`: day texture + program `0x141d84d30` (PS 1670 + VS 1673); `t = 0`: night texture + program `0x141d86b70` (PS 1674 + VS 1677); `0 < t < 1`: both textures + program `0x141d889b0` (PS 1678–1680 + VS 1681, cross-fade); material cards: `0x140ca9970` |
| direction | `0x140ca6ec0` | `az = lerp(night.azimuth, day.azimuth, t)`, `lat = lerp(night.latitude, day.latitude, t)` (already ×π/2); flag 8 cards aim at `location` instead (`0x140ca6eca..0x140ca70eb`) |
| draw | `0x140caa330` | World and uniforms below |
| blend | AMAT of the three card blobs | render state `0x10006565000` = **SRC_ALPHA, INV_SRC_ALPHA** (bit 40 also set) |

### 9.2 Direction (PROVEN)

World = `translate(camera) · rot(axis (0,0,−1), az + spin) · rot(axis (0,−1,0), lat)
· scale(1, sx, sy)` (`0x140caab38..0x140caabde`; `0x140aa4a50` builds the
rotation with the same row layout as `0x1409cb0e0`; `0x140aa5eb0` shows the
matrix stack multiplies new transforms on the right). The lens-flare code
places the flare at exactly this direction with explicit trig
(`0x140caa8d9..0x140caa963`: `x = cos az·cos lat`, `y = sin az·cos lat`,
`z = −sin lat`, times 25000 `0x141d8ab24`), and flag-8 cards invert it with
`lat = asin(−dz)`, `az = atan2`-style (`0x140ca6ff7..0x140ca70e7`). So:

```
az  = azimuth + spin        // radians; spin = 0 at load (below)
lat = latitude · π/2        // stored 0..1 = horizon..zenith
c   = ( cos lat·cos az,  cos lat·sin az, −sin lat )   // card centre
eL  = ( −sin az,  cos az, 0 )                          // local +Y: uLeft side
eD  = ( sin lat·cos az,  sin lat·sin az,  cos lat )    // local +Z: bottom side
```

`az = 0` is east, growing toward **north** (counter-clockwise seen from
above). `spin` comes from EnvContext slot `0xa8` (`0x140c42eb0`): a table keyed
by `(day.speed, night.speed)` whose entry is created with value 0
(`0x140c430d9`). It is not used for flag-8 cards (`0x140caa86c`). **Static
frame: spin = 0** (the value at map load).

### 9.3 Quad, size and textureUV (PROVEN)

`0x140ca7280` builds 4 vertices (stride 0x54, FVF `0xff0079`) at distance
`F = EnvContext+0x1058` with half-size 1000 (`0x141923c4c`), normalizes them
to length F, and writes two UV sets: the first from the **night**
`textureUV`, the second from the **day** `textureUV` (call site
`0x140ca8b0a..0x140ca8b15`: `rdx = night UV` at card+0x200, `r8 = day UV` at
card+0x118, which the import filled from packfile offsets `+0x7d` and `+0x2c`).
The day program's VS 1673 reads `TEXCOORD1`, the night program's VS 1677 reads
`TEXCOORD0`, which matches.

| vertex | local position | uv from `textureUV = (x, y, z, w)` |
|---|---|---|
| 0 | (F, +1000, −1000) | (x, 1 − z) |
| 1 | (F, +1000, +1000) | (x, 1 − w) |
| 2 | (F, −1000, −1000) | (y, 1 − z) |
| 3 | (F, −1000, +1000) | (y, 1 − w) |

So **`textureUV = (uLeft, uRight, 1 − vTop, 1 − vBottom)`**: `(0,1,1,0)` is the
whole texture upright and unmirrored (left = +Y = `eL`, top = −Z = up). Scale:
`sx = lerp(night.scale.x, day.scale.x, t)`, `sy` likewise (each clamped to
at least 1e-6), applied to local Y and Z (`0x140caa840..0x140caa86c`,
`0x140caab97..0x140caabde`). Flags 8+0x10 together divide the scale by the
distance to `location` and multiply by 25000 (`0x140caa7d5..0x140caa83c`).
Angular half-size: `tan = 1000·s / F`, `F = F0` (§7.1). 187611 card 0 (day
scale 3.22, `F0` = 36864): half-size `atan(0.0874)` = 5.0°.

### 9.4 Colour and blend (PROVEN; inputs as §11)

Uniforms (`0x140caac0b..0x140caacea`; card values are `lerp(night, day, t)`,
sky values as in §5):

| uniform | .x | .y | .z | .w |
|---|---|---|---|---|
| `skypara` | sky HazeBottom | sky HazeFalloff | card.hazeDensity · sky HazeDensity | card.minHaze |
| `skyparb` | card.density · cardFade | card.brightness | 0 | 0 |
| `sunDir` | normalize(sun direction) | | | 0 |
| `sunClr` | sun colour r | g | b | card.lightIntensity · sky LightIntensity |

`cardFade` = `EnvEnvironment+0xa40` (× `1 − blend` while two environments
blend, `0x140c635bf..0x140c63631`), written by EnvEnvironment vtable slot
`0xd0` (`0x140c62160`) as `enabled ? value : 0`. Its steady value is
**UNPROVEN** (expected 1).

PS 1670 (= PS 1674), with `d` = camera→card direction (VS 1673 outputs
`World3x3·pos`, not its negative) and `T = tex(uv)`:

```
g    = ((1 − d.z) + sunDir.z·d.z) · (dot(sunDir, d)·0.25 + 0.75)²
amb  = ( dot((−d,1), shRed), dot((−d,1), shGreen), dot((−d,1), shBlue) )
C    = lerp(amb, sunClr.rgb, g)
X    = C·T + lum(T)²·(C + T − 2·C·T)        // lum = dot(T.rgb, (0.3,0.59,0.11))
M    = 0.5·X + 0.5·T
G    = lerp(M, X, g)
col  = T + sunClr.w·(G − T)
e    = saturate(−d.z)                        // elevation sine
f    = saturate((e − skypara.x) / skypara.y)
h    = saturate((1 − f²(3 − 2f))·skypara.z + skypara.w)
rgb  = lerp(col, FogColorFar.rgb, h) · skyparb.y
a    = T.a · skyparb.x
dst  = rgb·a + dst·(1 − a)                   // SRC_ALPHA, INV_SRC_ALPHA
```

Visibility (`0x140caa623..0x140caa6b9`): nothing is drawn while
`EnvSkyCards+0x60 == 0`; flag 2 hides the card while program C is active
(`0x140d65440`, the same flag that switches the hemicube to its sun-disc
program); flag 4 hides it when `camera.z >= level`, flag 0x80 when
`camera.z < level`.

### 9.5 Sampler formula (texture cards)

For the mode (day: `t = 1`, `day.*` and the day texture; night: `t = 0`,
`night.*` and the night texture), skip cards with a material, `density = 0`,
flag 0x80, or flag 2 if program C is assumed on; flag 8 cards need `location`
and the camera (not baked).

```
x = dot(d, c);  if (x <= 0) miss
a = dot(d, eL) / x;   b = dot(d, eD) / x
tu = 1000·scale.x / F;   tv = 1000·scale.y / F
if (|a| > tu || |b| > tv) miss
s  = (1 − a/tu) / 2;   r = (1 + b/tv) / 2           // 0..1 left→right, top→bottom
u  = UV.x + (UV.y − UV.x)·s
v  = (1 − UV.z) + ((1 − UV.w) − (1 − UV.z))·r
T  = tex(u, v)
(rgb, a) = §9.4
dst = rgb·a + dst·(1 − a)
```

Reduced form the baker can use without the missing globals: when
`card.lightIntensity·sky.LightIntensity = 0`, `col = T`; when also
`card.hazeDensity·sky.HazeDensity = 0` and `minHaze = 0`, `h = 0`, so
`rgb = T.rgb·brightness`, `a = T.a·density` (with `cardFade = 1`). Otherwise
the result needs `FogColorFar` / SH / sun colour (§11) and the card goes into
`warnings`. 187611 card 2 (`minHaze = 0.73`) and card 1 at night
(`lightIntensity 0.88`) are examples that need them.

### 9.6 Material cards (UNPROVEN for baking)

A material card's runtime data is `PackMapEnvDataSkyCardMaterialV47`:
`filename` (an AMAT), `constants[]` (`token`, `float4`), `textures[]`
(`filename`, `textureUV`), `textureAnimation`, `flipbook`. `0x140ca9970` loads
every texture, builds the program from the AMAT through `0x140aab550` with
state `0xc086000`, and sets each constant by its token unless the token is in
a 7-entry reserved table at `0x1427f1550` (`0x140ca9b29..0x140ca9b85`). All of
this is resolvable offline: `gw2dat_cli amat --file-id 3135800` reads the
3264516 card material (19 shaders, chosen PS has samplers 0–2, render state
SRC_ALPHA/INV_SRC_ALPHA), and its PS uniforms are `FogColorFar, Time,
TimeOfDay, AlphaRef, norpan, norscaa, norapow, norscab, norbpow, alphaov,
difclbd, difclad, difcolb, difcola, skypara, skyparb`. Baking one means
evaluating that PS on the CPU with a chosen `Time`/`TimeOfDay`, plus knowing
which effect the sky path selects. Not done: **leave material cards out**.

---

## 10. Cloud layers

### 10.1 Code

| What | Where | Finding |
|---|---|---|
| import | `0x140c87d20` (`0x140c87df0..0x140c87ed2`) | layer → 0x70 bytes: altitude, `attributes[0..2]` (brightness, density, haze, lightIntensity, velocity), cutOut, depth, extent, fadeEnd, fadeWidth (fade values of the **last** attribute set win), scale, texture |
| setup | `0x140ca3830` | one shared mesh `0x140ca3290(0.5, 10)`; per layer texture (or a default) and program from blob `0x141d72f80` (size `0x1184`, PS 1644 + VS 1645); scroll offsets zeroed (`0x140ca3aa1`) |
| draw | `0x140ca4050` | World, attribute choice, uniforms below |
| blend | AMAT of blob `0x141d72f80` | **SRC_ALPHA, INV_SRC_ALPHA**; second sampler is engine texture 35 in slot 12 (`ssNoiseDepth`, the scene depth) |

### 10.2 Geometry and UV (PROVEN)

Mesh `0x140ca3290`: an 11×11 grid on `z = 0`, `x, y ∈ [−0.5, 0.5]` step 0.1,
`uv = (x + 0.5, y + 0.5)`, vertex colour (all 4 bytes)
`255·(1 − min(r/0.5, 1)⁴)` with `r = sqrt(x² + y²)` (`powf` `0x140e58450`,
exponent 4 `0x14192ce20`). It is a **flat plane with a round soft edge**, not
a dome.

World (`0x140ca4244..0x140ca42c6`, set straight on the material with
`0x140a86f20`):

```
m  = 0.5925 + 0.4075 · L.extent              // 0x141d74124, 0x141d74120
Sw = 2 · F · m                                // F = EnvContext+0x1058, ×2 at 0x140ca40fe
World = [ Sw 0 0 cam.x ; 0 Sw 0 cam.y ; 0 0 1 −L.altitude ]
```

So the layer is a horizontal square of half-size `F·m`, centred under the
camera, at height `L.altitude` above z = 0 (world, not camera-relative in z).
`uvTrans = (cam.x/Sw + scroll.x, cam.y/Sw + scroll.y, m·L.scale, brightness)`
and VS 1645 does `uv = (meshUV + uvTrans.xy)·uvTrans.z`, which comes out as

```
u = P.x · L.scale / (2F) + (0.5 + scroll.x) · m · L.scale
v = P.y · L.scale / (2F) + (0.5 + scroll.y) · m · L.scale
```

for a world point `P` on the plane: the texture is anchored to the world and
tiles every `2F / L.scale` units. `scroll += velocity·dt / Sw` each frame
(`0x140ca4380..0x140ca43b1`) and starts at 0.

### 10.3 Attributes and uniforms (PROVEN)

`A = lerp(attributes[1], attributes[0], t)`, or `attributes[2]` when
`camera.z > level` (§3).

| uniform | .x | .y | .z | .w |
|---|---|---|---|---|
| `uvTrans` | as above | | `m · L.scale` | A.brightness |
| `cldPara` | L.depth | A.density · cloudFade | A.haze | L.cutOut |
| `cldParb` | L.fadeEnd | L.fadeEnd + L.fadeWidth | 0 | 0 |
| `sunDir` | normalize(sun direction) | | | 0 |
| `sunClr` | sun colour r | g | b | A.lightIntensity · sky LightIntensity |

`cloudFade` = `EnvEnvironment+0x970` (wrapper `0x140c633df..0x140c63451`,
written by EnvEnvironment slot `0xc8` = `0x140c61640` as
`enabled ? value : 0`); steady value **UNPROVEN** (expected 1).

### 10.4 Shaders

VS 1645: `n = normalize(camera − P)` (= −d); vertical fade
`vf = saturate((|cam.z − P.z| − cldParb.x) / cldParb.y)²`; fog
`fz = saturate(viewZ·FogParam0.x + FogParam0.y)`,
`fog = min((1 − fz)·FogColorNearMinusFar.w + FogColorFar.w, fz) · cldPara.z`.
PS 1644:

```
a0   = tex(uv).a
uv2  = uv + (a0 − 0.5)·cldPara.x·n.xy           // parallax by depth
T    = tex(uv2)
g, amb, C, X, M, G, col: as §9.4 with d replaced by n (= −d), sunClr.w as above
rgb  = lerp(col, FogColorFar.rgb, fog) · uvTrans.w
soft = smoothstep(saturate((sceneDepth − pixelDepth) / cldPara.w))
a    = vertexAlpha · cldPara.y · T.a · soft · vf
dst  = rgb·a + dst·(1 − a)
```

### 10.5 Sampler formula

Needs `F` (= `F0`, §7.1), a camera position `cam` (only `cam.z` changes the
shape; `cam.xy` shifts the texture phase), `fog` and the §11 globals.
**Camera height (round 3, PARTIAL).** The cloud code applies no clamp and no
relative offset. The plane sits at the absolute world height
`z = −L.altitude` (World row 3), the vertical fade and the parallax/lighting
direction come from the engine's `CameraPosition` uniform in VS 1645 (the
render camera), and only the plane centre (`cam.xy`) and the
`attributes[2]` switch (`cam.z > level`) use the frame argument. That argument
is the 5th argument of EnvContext slot `0x50` (`[rbp+0x240]` = entry+0x28 in
`0x140c43f80`). The frame function also takes a second position as `r8` and
tests its `.z > level` (`0x140c44096..0x140c440ae`). Which of the two is the
camera and which the player was not traced. A bake must pick an absolute
camera z in map units (up = −z). The code offers no "sky camera" height to
copy. With no scene geometry
`soft = 1` (assumption: the depth buffer holds the far plane).

```
for each layer L, back to front (order UNPROVEN; farthest first is the safe choice):
    if (d.z == 0) continue
    s = (−L.altitude − cam.z) / d.z;  if (s <= 0) continue
    P = cam + s·d
    xl = (P.x − cam.x) / Sw;  yl = (P.y − cam.y) / Sw
    if (|xl| > 0.5 || |yl| > 0.5) continue
    va = 1 − min(sqrt(xl² + yl²)/0.5, 1)⁴     // game: per-vertex, linear per 0.1 cell
    uv = ((xl + 0.5) + cam.x/Sw, (yl + 0.5) + cam.y/Sw) · m·L.scale   // scroll = 0
    n  = −d
    (rgb, a) = §10.4 with vertexAlpha = va, soft = 1,
               vf = saturate((|cam.z + L.altitude| − fadeEnd)/(fadeEnd + fadeWidth))²
    dst = rgb·a + dst·(1 − a)
```

The plane only covers directions whose hit point is within `F·m` of the camera,
so a cloud layer never reaches the horizon. **PARTIAL**: everything above is
from code (`F` included, §7.1), but `cam.z`, `fog` (engine fog uniforms),
`cloudFade` and the lighting globals are runtime values.

---

## 11. Haze and sun-glow inputs

- `FogColorFar`: no sky draw sets it (hemicube §5; stars, cards and clouds set
  only the uniforms listed in §8–§10), so it is an engine-global uniform set
  by the fog/haze system. Its writer was not found. **UNPROVEN.** The clouds
  also need `FogParam0` and the `.w` of `FogColorNearMinusFar`/`FogColorFar`.
- `shRed/Green/Blue`: engine globals, not set by any sky draw. **UNPROVEN.**
- Sun direction and colour: **PARTIAL.** Every sky layer gets them from
  `EnvContext+0x4f0..0x4f8` and `EnvContext+0x4e4..0x4ec` through the frame
  function (§7). No direct stores to those offsets were found in
  `Map\Environment` (`0x140c00000..0x140e00000`), so they are probably filled
  by a block copy of the interpolated light rig. Not traced.
- `TimeOfDay`, `Time`: engine globals (cross-fade card program, material
  cards, star twinkle). **UNPROVEN.**

---

## Open items (what would close each UNPROVEN)

Static work still possible:

1. `k` (modes 2/3): `0x140c58540` is a weighted accumulator. It adds
   `(1 − weight)·x` of each environment's values (`x` from the recursive
   `0x140c60c00`, out-param `[rbp+0x38]`), then divides by the total weight.
   So `k` is a zone-blended per-environment scalar, but which env field `x`
   is was not traced. Next: the leaf case of `0x140c60c00`.
2. `F`: done (§7.1). Still open: whether anything calls EnvContext slot
   `0x110` (`0x140c43a10`). 10 float-passing `call [reg+0x110]` sites need
   their receivers identified.
3. Layer order: decode the 5th/6th arguments of `0x140aab1f0` (state word and
   the value asserted `< 0x10`) into the bgfx sort key (`0x140aaa2c0`,
   `0x140aa9fd0`).
4. Star twinkle: decompile the RNG `0x140e1e260`/`0x140e1e6a0` (seed 1337) to
   get each vertex's colour; the phase then needs only `Time`.
5. Material cards: run the AMAT's PS on the CPU (castlemist has the AMAT
   reader and token decoder); find which effect `0x140aab550` picks.
6. Sun direction/colour writers: find the block copy into
   `EnvContext+0x4e4..0x4f8`.

Runtime captures that would settle the rest (one map, day and night):

7. (Confirmation only.) `EnvContext+0x1058` should equal the map's
   `trn.swapDistance` rounded to 3072 (§7.1). A mismatch would mean the slot
   `0x110` setter runs.
8. `EnvContext+0x1050` (`t`) and `+0x105c` (`k`) in each sky mode.
9. At the cloud draw `0x140ca4050`: the 8 arguments (sun direction, sun
   colour, camera position (which object?), `level`, `xmm3` = `cloudFade`),
   and the engine uniforms
   `FogColorFar`, `FogColorNearMinusFar`, `FogParam0`, `shRed/Green/Blue`,
   `Time`, `TimeOfDay` as they stand for the sky pass.
10. `0x140d65440` return value (program C, which also hides flag-2 cards).
11. GPU order of the hemicube, star, card and cloud draws (one frame capture).
12. Render target format and the sky's path through exposure/tonemap
    (unchanged from round 1).
