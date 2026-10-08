# Export Skybox (Map) — Design

Date: 2026-10-08
Status: designed with the user; awaiting spec review.
Builds on: the map `env` chunk walk (`Extractor::parseMapEnv`,
`docs/research/gw2-map-lighting.md`), texture decoding (`gw2dat_cli texture`,
`src/extract/texture_source.cpp`), and the engine-shader survey
(`docs/research/gw2-exe-shaders.md` — sky/atmosphere PS candidates, e.g. #1611).

## Goal

Any GW2 map's sky, out as files that drop into a VRChat world project as a
Unity skybox. Where the game stores a real cube sky, those faces go out as-is.
Every other sky is baked from its layers — panoramas, stars, clouds and sky
cards (sun, moons, planets) — into a static image as close to the game's sky
as the reverse-engineered shader maths allows.

This is sub-project 1 of three agreed with the user, in this order:

1. **World skyboxes** (this spec)
2. World reflection probes — the map `cube` chunk (`PackMapCubeMapV4`
   samples: position + day/night DDS cubemaps)
3. Avatar material cubemaps — material DDS cubes (e.g. 221582, 1203843) into
   the VRChat model export, replacing the `fx-cubemap` "unsupported" profile

## Out of scope

- Reflection probes and material cubemaps (sub-projects 2 and 3).
- Animated skies: cloud scrolling, sky-card motion, flipbooks, day/night
  transitions. One static frame per sky mode is baked.
- A Unity importer or `.mat`/`.cubemap` assets — output is images plus a JSON
  sidecar; the Unity side is standard skybox materials set up by hand.
- HDR output. GW2 sky textures are 8-bit; brightness above 1 is tonemapped
  into PNG (decided in the research step, recorded in `gw2-sky.md`).
- Per-zone sky overrides. A scan of 176 of 287 maps found no
  `dataLocalArray` entry with its own sky textures; only the global sky is
  exported.

## What the data holds

From `PackMapEnvDataGlobalV75..V78` (the `env` chunk's `dataGlobal`):

| Field | Holds |
|---|---|
| `skyModeTex[]` | per sky mode: `texPathNE`, `texPathSW` (half-panoramas, horizon mid-image), `texPathT` (zenith cap) |
| `skyModeCubeTex[]` | per sky mode: `texPathE/W/N/S/B/T` — a real 6-face cube (env v77+) |
| `starFile` | star texture |
| `clouds.layers[]` | cloud layer textures and parameters |
| `skyCards.cards[]` | `day`/`night` attributes: `texture`, `azimuth`, `latitude`, `scale`, `textureUV`, `brightness`, `density`, haze; plus `material` |
| `sky` | day/night brightness, haze bottom/density/falloff, star density, light intensity, vertical offset |

Scan of 176 maps (throwaway, 2026-10-08): 171 have panoramas, 3 also have a
cube sky (3134778, 3194054, 3264516), 152 have textured sky cards, 2 have no
sky textures. Both sampled panorama maps repeat mode 0 as mode 3; 3264516
fills cube modes 0–1 and leaves 2–3 null.

## User-facing behaviour

- **File → Export Skybox (Map)…** — next to "Export glTF (Map)…", enabled when
  the current entry is a map. Asks for a destination folder name, runs on a
  background thread like the glTF map export, reports in the status bar and a
  message box (modes written, whether a raw skybox was found, warnings).
- **CLI:** `gw2dat_cli skybox --dat <dat> --file-id <map> --template <json>
  --out <dir> [--size <face px>]` — the same export, printing the report as
  JSON.

### Output folder

```
<Name>/
  sky.json                    modes, source fileIds, sun per mode, warnings
  <mode>/
    baked/
      equirect.png            4096x2048, for Skybox/Panoramic
      px.png nx.png py.png ny.png pz.png nz.png   1024² each, for Skybox/6 Sided
    skybox/                   only when the map stores a cube for this mode
      px.png nx.png py.png ny.png pz.png nz.png   faces as stored, reoriented
```

`<mode>` names come from the research step; until proven they are
`mode0`..`mode3`. Modes whose inputs are identical to an earlier mode are not
written again; `sky.json` lists them as aliases of the first.

`sky.json` per mode: the source fileIds used, sun direction (Unity axes) and
colour/intensity from the env light rig for that preset, and which layers made
it into the bake.

## Architecture

Three units, each testable alone:

1. **`Extractor::parseMapSky()`** (`gw2model.hpp`, beside `parseMapEnv`).
   Template-navigated, version-resolved with `resolveVariant`. Returns a plain
   `MapSky` struct: per mode the panorama fileIds and cube-face fileIds, the
   star fileId, cloud layers, sky cards, sky parameters. No decoding, no
   maths. Fields missing from older versions stay empty.

2. **Sky sampler** (`src/exportgltf/sky_bake.cpp`, header in
   `include/castlemist/exportgltf/`). Given decoded RGBA images and a `MapSky`
   mode, `radiance(direction) -> rgb`. Layers in the order the research
   proves (expected: base panorama → stars → clouds → sky cards → haze).
   Pure function of its inputs; no dat, no GPU. Cube/equirect writers take
   any `radiance` callback, so projection and layering are tested apart.

3. **Skybox export** (`src/exportgltf/sky_export.cpp`). Loads the map, calls
   `parseMapSky`, decodes textures through the existing texture source, writes
   raw faces (axis remap + rotation only, no resampling), runs the bake, merges
   duplicate modes, writes PNGs and `sky.json`. Called by the CLI and the UI.

Axis conversion reuses the conventions in `transform_math.cpp` (the glTF map
export already maps GW2 space to glTF/Unity).

## Research step (before sampler code)

`docs/research/gw2-sky.md`, from the sky/atmosphere pixel shaders (#1611 and
the other 15 candidates in `stage_candidates.json`), `EnvContext_LoadSkyTextures`
and `EnvContext_SetOverrideSkyTextures`. It must establish:

- The NE/SW/T projection: which direction each texel covers, where the
  horizon sits, how the Top cap meets the panoramas, what the mirrored lower
  half is for.
- Which sky mode index is which time of day.
- How stars, clouds and sky cards blend and what each parameter
  (`density`, `hazeDensity`, `minHaze`, `brightness`, `latitude`, `azimuth`,
  `scale`, `textureUV`) does.
- Haze and brightness maths, and how values above 1 are handled.
- The orientation of the six cube faces.

The sampler implements only what the note proves. Anything unproven is left
out of the bake and named in `sky.json` warnings, never guessed.

## Errors and edge cases

- No `env` chunk, or no sky textures (2 maps in the scan): the export reports
  "no sky" and writes nothing.
- A texture that fails to decode: that layer is skipped and warned; the base
  panorama failing skips the whole mode's bake.
- Null cube faces for a mode (3264516 modes 2–3): no `skybox/` for that mode.
- Unknown env version above the template: `resolveVariant` falls back to the
  nearest lower variant, as `parseMapEnv` does.

## Testing

Unit tests (no dat):
- Cube writer: a radiance callback that is red only along +X lands on `px`,
  and likewise for every face; face edges agree with their neighbours.
- Equirect writer: known directions map to the expected pixels.
- Panorama projection: synthetic NE/SW/T images with marked edges show no
  break at the NE↔SW seams or the panorama↔Top boundary.
- Sky cards: a card at a given azimuth/latitude appears at that direction.
- Mode merging: identical inputs merge; differing ones do not.

Dat-backed checks (skipped when the dat is absent, like existing ones):
- `parseMapSky` on 187611 matches the parse-tree values (modes 0–3 fileIds,
  `starFile` 187544); on 3264516 it returns cube faces for modes 0–1.
- End-to-end CLI export of 187611 and 3264516 writes the expected files.

Visual check with the user: in-game sky screenshots (e.g. Lion's Arch day and
night) beside the bake. The user supplies the screenshots.
