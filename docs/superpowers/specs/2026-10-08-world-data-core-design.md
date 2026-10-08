# World Data Core — Design (sub-project 1 of the World Engine)

Date: 2026-10-08
Status: draft for review

## Goal

Read a GW2 map from the dat into one trustworthy, renderer-free `WorldScene`:
terrain, terrain materials, props, water, collision and environment, all in a
single documented coordinate frame, with every convention proven and every
gap named. Everything later in the World Engine (map window, materials,
export, Udon) consumes this model and nothing else.

## Why a rebuild

The current map path (`extract/map_scene.cpp` → `MapScene`, drawn by
`render/map_fly.cpp`, exported by `exportgltf/map_export.cpp`) is not usable
for whole-map export:

- Terrain and props disagree on orientation; terrain is often rotated
  against the prop layout.
- Terrain is one untextured mesh; no terrain material is read.
- Water is guessed (flood fill / a flat plane), not read.
- Fudge values stand in for data: a hard-coded ±3072 rect when the map has
  none, and a fixed 32 tiles per chunk where the dat carries
  `verticesPerChunkSide`.
- Nothing moves; it is fly-through only; export is one undivided `.glb`.

Likely cause of the orientation mismatch (unproven until this sub-project
checks it): terrain and props are put into world space by two separate
code paths with no shared, documented frame.

## Decisions

- **Approach A:** a new `world` layer producing `WorldScene`, consumed by the
  map window, the exporter and tests. The old `MapScene` path stays until
  the new one replaces it, then is removed.
- **T3D is the reference, not the base.** The user's fork
  (`github.com/spjinx/t3d`) renders and walks maps correctly in the browser.
  It is GPL-3 and castlemist is MIT: its *understanding* is ported and
  credited in comments; no code is copied.
- **No invented conventions.** Axes, handedness, units, rotation order, chunk
  layout and UV orientation each come from a packfile template field, the
  game's code, or agreement with T3D on real data. Anything none of these
  settle is marked UNPROVEN, left out or flagged, and listed in `warnings`.
  A value that is right for some maps and wrong for others is a wrong value.
- **Template-driven parsing.** Map chunks are read through the packfile
  template by field name (as `parseMapEnv` / `parseMapSky` do); no
  hard-coded offsets or versions.
- **Unknown data is named, never dropped.** Unread chunks, unknown versions
  and failed decodes go into `WorldScene::warnings`.
- **Udon and gameplay data are out of this model.** No triggers, doors,
  waypoints or scripts in `WorldScene` v1; unread chunks are merely named in
  warnings, which later Udon research can start from.

## Architecture

```
format / native  (dat, packfile, Extractor parse* readers)
      │
   extract       existing: models, materials, game shaders, textures
      │
    world        NEW: map bytes + dat → WorldScene
    ╱    ╲
 render   exportgltf   (later sub-projects)
   │
  ui
```

New layer: `castlemist_add_layer(world DEPS castlemist::extract ext::json)`,
sources in `src/world/`, headers in `include/castlemist/world/`. It does not
depend on `render`, `exportgltf` or `ui`.

## WorldScene v1

Plain data; no GPU handles.

| Part | Contents |
|------|----------|
| `frame` | the world convention (axes, handedness, units, origin) as documented in `gw2-world-frame.md`; map bounds in that frame |
| `terrain` | chunk grid size; per chunk: heights (de-tiled), normals, world rect; material: blend/picker maps, ground textures (up to the count the dat gives), UV scale/offset, plus the terrain shader/material ids for a later game-shader path |
| `models` | table of distinct prop models by fileId |
| `props` | instances: model index, world transform (composed once, in the frame), flags/variant, source group (which props chunk/array it came from) |
| `water` | surfaces as the dat describes them (shape, height, material ids); none guessed |
| `collision` | Havok geometry as triangle meshes, terrain and props, in the frame, separate from visual geometry |
| `environment` | the existing `MapSky` and `MapEnvLight` parses, attached |
| `motion` | recorded, not played: animated-prop clip references, material UV-scroll values, placed-effect references (by id only) |
| `warnings` | every unread chunk, unknown version, failed decode, UNPROVEN convention |

Entry point:

```cpp
namespace castlemist::world {
WorldScene load_world(Gw2Dat& dat, uint32_t mapFileId, const nlohmann::json& tpl);
}
```

Parsing stays in the native `Extractor` where a `parseMap*` reader already
exists (`parseMapProps`, `parseMapZones`, `parseMapCollision`, `parseMapEnv`,
`parseMapSky`); the `world` layer adds missing readers (terrain material,
water surfaces) and owns all conversion into the frame. Conversion happens
in exactly one place per data kind.

## Research note: `docs/research/gw2-world-frame.md`

Same style as `gw2-sky.md`: every convention with its evidence or UNPROVEN
plus what would prove it. Must cover at least:

1. World axes, handedness and units; how they map to Unity (Y-up, left-handed,
   metres) and Blender (Z-up, right-handed).
2. Map bounds: the `parm` rect, and what applies when a map has none.
3. Terrain layout: chunk grid from `dims`, chunk count and
   `verticesPerChunkSide`; the overlap between chunks; grid index → world
   position.
4. Terrain material: which chunk/fields hold the blend maps and ground
   textures, how a chunk picks them, UV scale.
5. Prop transforms: position, rotation (order, units, sign), scale; how
   grouped/instanced props compose.
6. Water: where surfaces live and their height.
7. Collision: Havok geometry's frame relative to the visual world.

## Verification against T3D

A small script in `tools/world/` runs T3D's library (a local checkout of the
user's fork, outside this repo) on a map and writes a JSON of reference
numbers: map rect, chunk grid, terrain heights on a fixed 16x16 sample of
world positions, the first 50 prop transforms in file order, water height.
Committed reference files hold numbers only, no asset data. Dat tests compare
`WorldScene` against them: integers (grid sizes, counts) exactly; positions
and heights within 0.01 world units; rotations within 1e-4 per quaternion
component (sign-normalised). A disagreement is resolved from the dat and recorded in the
research note, never by tuning.

## Test maps

| Map | fileId | Why |
|-----|--------|-----|
| Queensdale | 192711 | open world, water, varied terrain |
| Lion's Arch | 191000 | city, dense props, water |
| Spirit Vale | 1151420 | small instance |

(fileIds from T3D's `MapFileList.ts`; confirmed against the dat in the plan's
first task.)

## Front end

CLI: `gw2dat_cli world --dat <dat> --file-id <map> --template <json>
[--out summary.json]` prints a summary (frame, bounds, chunk grid, counts of
models/props/water/collision, material resolution per chunk, warnings). No UI
in this sub-project.

## Error handling

`load_world` never throws for bad map data and never returns a silently empty
scene. Each failed section leaves its part empty and adds a named warning;
the rest of the scene still loads. A map with no terrain or no props is valid
(warned), not an error. Dat I/O failures and a missing template are errors.

## Testing

- **Pure** (no dat): frame conversions (GW2 → Unity, GW2 → Blender) on known
  vectors; terrain de-tiling on a hand-built chunked grid with overlap;
  prop transform composition on known inputs.
- **Dat** (skip without `GW2_TEST_DAT`), on the three test maps:
  - terrain and props agree: at least 99% of prop positions fall inside the
    terrain's world rect (catches a rotated or offset terrain), and the
    T3D comparison below passes for both;
  - every terrain chunk resolves a material;
  - chunk grid, rect, sampled heights, prop transforms and water height
    match the T3D reference files.

## Done means

- The three test maps load with terrain and props in one frame, agreeing
  with each other and with T3D's numbers.
- Every terrain chunk has a resolved material.
- `gw2-world-frame.md` documents every convention used, with evidence.
- The CLI summary runs on all three maps.

## Out of scope (this sub-project)

Rendering, the map window, walking, GPU materials, export, segmenting,
effects, Udon.

## Roadmap (later sub-projects, each its own spec)

2. **Map window** — a separate top-level window: walk mode (gravity, jump,
   slide and step-up against `WorldScene` collision, fall-through recovery;
   T3D's `PhysicsController` as the behavioural reference) and fly mode; layer
   toggles; picking a prop pours it into the main window's preview (Full /
   Shader / Game 1:1, dyes, VRChat export). Drawn through the Game 1:1 path
   with chunk streaming, culling and instancing.
3. **World materials and motion** — terrain blend material (game shader and a
   reconstruction), props through the existing game-shader/profile work,
   water, UV scroll, animated props played; placed effects once the effects
   viewer exists.
4. **Export** — Unity/VRChat first, Blender as a reader for renders and
   animation. A JSON manifest (with a schema) as the contract plus glTF per
   segment; terrain cut on chunk boundaries into cells sized for VRChat,
   props bucketed into the same cells as instances; Poiyomi-ready materials
   reusing the VRChat model export plus baked terrain albedo per cell;
   collider meshes per cell; sky and lights. A Unity importer (scene,
   prefabs, colliders, optional LOD/occlusion) and a Blender importer
   (collections, instanced props, materials; colliders optional).
5. **Udon** — research what gameplay data the dat holds, then decide what
   translates. Fully separate; starts after 2–4 ship.
6. **Effects viewer** (separate project) — particles and effects attached to
   models, skills and character movement; its own brainstorm.
