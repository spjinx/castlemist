# GW2 map particle fields (the ambient motes)

2026-10-23. Static reading of Gw2-64.exe; nothing was run against the live client.

A map's environment (`env` chunk, `PackMapEnvDataGlobalV78`) holds `particleFields`
(`PackMapEnvDataPFieldV76`, a ptr_array_ptr) and `particleFieldCutouts`
(`PackMapEnvDataPFieldCutoutV47`). The zones (`dataLocalArray[]`) carry their own lists. A
field is not placed in the world: it is one tile of particles that the vertex shader wraps around
the camera, so the camera always sits in a box of motes (stars, dust, snow) wherever it flies.
Client source: `Engine\Map\Environment\EnvParticleField.cpp`.

## Code

| Address | What it does |
|---|---|
| `0x140ca5b50` | Builds one instance per field with `particleCount != 0`; keeps `fieldDirection` normalised, `texColRow` as floats, `1 / texFPS` |
| `0x140ca47a0` | Builds the field mesh (below) and its material: shader blob by `type` (0..3, assert `< MAP_ENV_PARTICLE_FIELD_TYPES`) from the table at `0x14258af20`, or `0x14258af60` with flag `0x10`; blend state by flags `0x20` / `0x80` / `0x40` (assert `MAP_ENV_PFIELD_BLEND`) |
| `0x140ca5de0` | Per frame: visibility and the `fldprta..g` constants |

The runtime record is 0xa0 bytes and not in the file's (alphabetical) order. Offsets read in code:
`+0x0` altitude, `+0x4` angle, `+0xc` clusterCount, `+0x10` clustering, `+0x18` depth,
`+0x1c` deviation, `+0x20` deviationSpeed, `+0x28` fade, `+0x2c` extent (float), `+0x30` fieldDirection,
`+0x3c` flags, `+0x40` period, `+0x48` lifetime, `+0x50` opacity, `+0x58` particleCount,
`+0x5c` rotation, `+0x64` scaleX, `+0x6c` scaleY, `+0x74` seed, `+0x78` speed, `+0x80` texColRow,
`+0x88` texFPS, `+0x90` texPath, `+0x98` type. Three of these are read from use, not from a loader:
- `+0x2c` is the tile size: `extent`, because `fade` (50..250) would pack 15 000 sprites into a 6 m cube.
- `+0x40` / `+0x48`: the shader keeps a particle visible for the `+0x48` range and hidden for the `+0x40` range. ShootingStar (`lifetime` 93..100, `period` 0..1.07) is visible ~1 s in 100 s only with `+0x40` = lifetime. But the always-on stars (`period` 0) show only with `+0x48` = lifetime. Read here as visible = `lifetime`, hidden = `period`. UNPROVEN.
- `+0x28`: `fade`, which goes to `fldprtd.w`; no type-2 shader reads it.

## The mesh (0x140ca47a0)

The game seeds its RNG with `seed`. Each particle has 4 vertices sharing one position, with
6 indices per quad.
- **Position:** uniform in the unit cube. With a nonzero `clustering` (lerp of the range by a random
  t), the particle moves toward one of `clusterCount` random centres by t times the wrapped
  (shortest, toroidal) delta, then wraps back into [0, 1). Flag `0x100`: no vertical move.
- **COLOR:** x, y = the corner (0/1); z = a random byte (the particle's seed); w = `opacity` (a lerp).
- **TEXCOORD0:** size = (scaleX, scaleY) lerps. Flag `0x1`: square (scaleX for both).
- **TEXCOORD1:** (`angle` lerp, `rotation` lerp): the starting angle and the spin in rad/s.
- **TEXCOORD2:** (random frame < cols × rows, corner tag 0.25 / 0.75 / −0.25 / −0.75).

## Constants (0x140ca5de0; the VS uniform table names them, `cb0[8..13]`)

| Constant | Value |
|---|---|
| `fldprta` | ((camera − T/2 + (T/2 + 65.05) · viewDir) / T, time) |
| `fldprtb` | (T, visibility, deviation / T, −altitude) |
| `fldprtc` | (fieldDirection, depth) |
| `fldprtd` | (animated frame, cols, rows, fade) |
| `fldprte` | (speed.min, speed.max, deviationSpeed.min, deviationSpeed.max) |
| `fldprtf` | (period.min, period.max, lifetime.min, lifetime.max) |
| `fldprtg` | (..., frame blend) for the PS |

Visibility: flag `0x2` takes a time-of-day factor, flag `0x4` its complement, and their sum
is scaled by the camera's height (flag `0x10` off). Every star field in map 3811773 sets both
flags, so its visibility is 1. Particles larger than a client threshold are dropped at a low
graphics quality.

## Vertex shader, type 2 (blob `0x141d74130`; type 1 `0x141d7c230` differs only in the wobble)

With s = the seed byte and p = the tile position:
- **Drift:** p += fieldDirection · lerp(speed, s) · time · 0.025 (in tiles).
- **Wobble:** p += deviation / T · smooth(frac(hash)) per axis, at lerp(deviationSpeed) · time · 100 / T.
  Type 1 wobbles horizontally only.
- **Wrap:** p' = frac(p − cam) + cam; the world position is p' · T.
- **Billboard:** the view right/up vectors are rotated by angle + rotation · time, and scaled by
  size × the corner (±1).
- **Alpha:** opacity × visibility × twinkle (a smooth wave^4) × edge fade, where the edge fade is
  min over the axes of 1 − (2p' − 1)^4. It is capped at |world z| · 0.01, gated by the
  lifetime / period cycle, and zero inside a cutout box (up to 6, `clipAA..clipFC`).

## Pixel shader

colour = 2 × lerp(frame, next frame, blend); alpha = texture alpha × vertex alpha × a soft depth
fade (smoothstep over 120 units, squared).

## Map 3811773 (Wizard's Vault)

The global list has 15 fields, all type 2 except TwinkleStar (type 1):

| Field | Particles | Extent | Size |
|---|---|---|---|
| DistantStars | 24 464 | 54 464 | ~17 |
| CloseStars | 15 000 | 54 464 | ~10 |
| DistantStars (dupe) | 10 500–16 928 | 34–39 k | 13–22 |
| DistantStarsColor / blue | 30–400 | 10–28 k | — |
| TwinkleStar | 342 | 60 000 | ~95 |
| ShootingStar | 1–11 | 60 000 | — |

Textures: 3560858 (4×4 frames), 188545 (8×8), 191224, 52952.
