---
name: gw2-sky
description: "How GW2 draws its sky: the EnvHemicubeSkybox mesh behind skyModeTex NE/SW/T (a hemicube, not a panorama), the sky pixel shader's brightness/haze maths, which sky mode is day/night, the GW2 -> Unity axis map, and the Unity face mapping of skyModeCubeTex. Source of truth for the skybox baker; anything unproven is marked UNPROVEN."
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
The caller that sets the size was not traced, so the exact `e` is
**UNPROVEN**. Measured instead: the seam error is lowest when each texture is
read **1 texel** in from its edge.

| inset (texels) | 0 | 1 | 2 | 3 | 5 | 10 | 20 |
|---|---|---|---|---|---|---|---|
| mean seam diff, 6 seams | 3.97 | **2.07** | 2.82 | 3.58 | 5.04 | 8.26 | 13.57 |

Use `e = 1 / W` (W = texture width) and mark it as measured, not derived.

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
  t = 0 is night**.
- `k` (6th argument, `[rbp+0x68]`). Its meaning is **UNPROVEN**.

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
- Sky cards (`day` / `night` attributes): **UNPROVEN**. EnvSkyCards.cpp was
  not traced. The obvious guess (same t) is not proven.
- Cloud layers: each layer has 3 `attributes[]` (187611). The 3 lighting
  presets are day/dusk/night but not always in that order
  (`gw2-map-lighting.md`: map 181140 has its brightest rig at index 1). Which
  `attributes[]` index a sky mode uses is **UNPROVEN**. It needs the cloud
  draw (PS 1644 / VS 1645, uniforms `cldPara`, `cldParb`, `uvTrans`) traced to
  the code that fills them.

**Table:** recommended output names: `mode0` → `day`, `mode1` → `night`;
`mode2` and `mode3` keep their index names. Mode merging (spec) may treat two
modes as the same attribute set only for the sky-param set (0/2 day, 1/3
night).

---

## 4. Layer order and blending

The hemicube is one draw per face of program A (or C), so the "base" layer and
its haze are a single pass. Every other layer is a separate system whose
shaders are known but whose CPU-side parameter mapping was not traced.

| Layer | Status | What is known | What would prove it |
|---|---|---|---|
| base hemicube (NE/SW/T) | **PROVEN** | §2 projection, §5 colour maths | — |
| horizon haze | formula **PROVEN**, input `FogColorFar` **UNPROVEN** | §5 | where `FogColorFar` is filled (probably from the `haze` struct via `EnvContext_SetHazeColors`; not traced in this build) |
| sun glow inside the base pass | formula **PROVEN**, inputs **UNPROVEN** | §5 (`sunClr.w` = LightIntensity) | the 4th/7th arguments of the draw (`rdi` = sun dir, `[rbp+0x50]` = colour) traced to the env light rig; the engine SH globals `shRed/Green/Blue` |
| stars (`starFile`, `*StarDensity`) | **UNPROVEN** | `starFile` 187544 is not an ATEX (`gw2dat_cli texture`: "ATEX: bad magic"). `*StarDensity` (`+0x18`/`+0x30`) is not read by the hemicube draw. | the star renderer and the format of 187544 |
| cloud layers | **UNPROVEN** | PS 1644 (`FogColorFar, ScreenDims, sh*, TexelOffset, uvTrans, cldPara, sunDir, sunClr`, samplers `ss0` + `ssNoiseDepth`) and VS 1645 (`uvTrans, cldPara, cldParb`, World/WVP) draw them as world-placed geometry | the code that turns `PackMapEnvDataLayer` fields into `cldPara`/`cldParb`/`uvTrans` and the mesh |
| sky cards | **UNPROVEN** | PS 1670–1680 (`skypara, skyparb, sunDir, sunClr`, some with `TimeOfDay`, `fxclr`, `StencilId`), VS 1673/1677/1681 (`World, WorldViewProjection`). EnvSkyCards.cpp code at `0x140ca8b6b`, `0x140caa97f` | that code: azimuth/latitude → World, `scale` → size, `textureUV` convention, blend state |

Data notes for whoever traces the cards (from parseMapSky on character-fetch):

- `textureUV` is stored as four floats. Most cards on 187611 hold
  `(0, 1, 1, 0)` and one holds `(0, 0.279, 0.732, 0.459)`. That reads like
  `(u0, v0, u1, v1)` with V running bottom-up (a V-flip), but that is a reading
  of the data, not a proof. **UNPROVEN.**
- 3264516: one card has a day texture (186341, 64² DXT5). Four have texture 0
  and material fileId 3135800, so a material draws them, not a texture. A
  material-driven card can't be baked without the AMAT pipeline for that
  material. **UNPROVEN, leave out.**
- 3264516 `sky.verticalOffset = −100000`. In the draw that becomes a world
  translation of `−verticalOffset` along z (§1 item 4). It moves the hemicube
  origin, which changes the view only through the VS horizon clamp. A sky at
  infinity is unaffected. **For a bake, ignore verticalOffset.**

**Table:** bake order = base hemicube only (§2 + §5). Stars, clouds and cards
go into `warnings` as UNPROVEN, and haze and sun glow too (missing inputs).
For a static frame, scroll offset = 0 is moot because no scrolling layer is
baked.

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
instead of t; at t ∈ {0, 1} that is the same endpoint. Which hash is which
name: the register `.w` uses match the shader (`skypara.w` is the cross-fade in
PS 1619, `sunDir.w` scales the texture). The hash function itself was not run.

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

## Open items (what would close each UNPROVEN)

1. Mode `k` factor (modes 2/3): trace the caller of vtable slot 5
   (`0x140c6de80`) back to where its 6th argument comes from.
2. Exact UV inset `e = 10 / R`: trace the call to vtable slot 2
   (`0x140c6d430`).
3. `FogColorFar`, `shRed/Green/Blue` and sun colour for the sky draw: trace the
   draw's arguments and the engine globals. That would let the bake add haze
   and sun glow.
4. Program C activation flag (`0x140d65440`).
5. Stars, clouds, sky cards: trace EnvSkyCards.cpp (`0x140ca8b6b`,
   `0x140caa97f`), the cloud draw (PS 1644 / VS 1645), and the star renderer
   and the format of `starFile`.
6. Render target format and the sky's path through exposure/tonemap.
