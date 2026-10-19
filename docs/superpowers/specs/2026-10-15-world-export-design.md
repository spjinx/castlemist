# World Export — Design (sub-project 4 of the World Engine)

Date: 2026-10-15
Status: written as a record (the user asked for sub-projects 2–4 to be built in
order without sign-off gates; see the World Data Core spec's roadmap)

## Goal

Export a GW2 map from one `WorldScene` as a folder that a Unity/VRChat world
project and Blender can both build a scene from with no hand work. The folder
holds a versioned JSON manifest (the contract, with a committed JSON Schema),
one glTF per cell, one VRChat model folder per prop model, colliders, water,
sky and light. Terrain is cut on chunk boundaries into cells that fit a stated
budget, and each cell gets a terrain albedo baked from the same blend the map
window draws. Props are placed in the same cells as instances of one shared
model each. Two importers ship in the repo: a Unity editor script that builds
the scene, prefabs, colliders and Poiyomi materials, and a Blender script that
builds collections, instanced props and materials.

Sub-project 1's rule still holds: **no invented conventions**. A value the data
does not settle is a labelled setting with its source, written into the
manifest's `settings` and `warnings`, or it is left out.

## Why

The single-file `world-export` (`src/exportgltf/world_export.cpp`) proves the
frame. It writes terrain, props and collision in map space under one
`map_to_gltf_root` node. It is not something a VRChat world can use:

- one undivided `.glb`, so nothing can be culled, streamed or edited per area;
- untextured terrain (its own header says glTF cannot express the blend);
- prop materials are plain glTF PBR, not the Poiyomi mapping the VRChat model
  export works out (`materials.json`, profiles, blend presets);
- no colliders a game engine can use per area, no water, sky or light;
- Unity has no glTF importer of its own, and the third-party ones vary
  (UNPROVEN which features each supports; nothing in the repo documents them).
  The repo's route to Unity is Blender → FBX
  (`2026-10-07-vrchat-model-export-design.md`, `castlemist_vrchat.py`).

Sub-project 3 already made the material decisions once, in code meant for this
use: `world::terrain_blend` / `page_uv` / `ground_uv`, `profile_for`,
`decode_blend`, `decode_texture_capped`, `resolve_prop_clip`. This sub-project
calls them offline.

## Decisions

### D1. Cells: page-aligned blocks of chunks, split by a measured budget

- **A cell is a rectangle of whole terrain chunks.** Start from 4 × 4-chunk
  blocks aligned to the picker-page grid. A page covers exactly 4 × 4 chunks on
  every test map (`gw2-world-frame.md` §4.2, `pickerScale = 0.25`). The block
  size is `1 / pickerScale`, read from the data and not fixed in code. A map
  whose pages did not resolve uses `cellMaxChunks` (default 4) and gets a
  warning.
- **Why 4 × 4 is the largest block.** A 4 × 4 cell's terrain, welded on shared
  edges, has (4·32 + 1)² = 16 641 vertices. 8 × 8 would have 66 049, which is
  over the 65 535 that 16-bit indices can address. 4 × 4 is the largest
  page-aligned square that fits a 16-bit index buffer. (That Unity meshes
  default to 16-bit indices is UNPROVEN in the repo docs. The export does not
  rely on it, but it never makes a terrain mesh that would need 32 bits.) The
  rule is counted in vertices, never in metres.
- **Budget, all measurable, castlemist's choice and labelled so:**

  | setting | default | measured as |
  |---|---|---|
  | `cellMaxChunks` | 4 (or `1/pickerScale`) | chunks per side |
  | `cellMaxTriangles` | 1 000 000 | terrain triangles + Σ LOD0 triangles of the cell's prop instances |
  | `cellMaxTextureMB` | 256 | Σ over the distinct textures the cell's instances use, plus the cell's albedo and water textures, of RGBA8 + mips at the export's texture cap (w·h·4·4/3) |

  The two budget defaults are not VRChat numbers. VRChat states no per-area
  limit in any repo doc (UNPROVEN). They are the split point for a cell, the
  unit the Blender importer loads and hides and the Unity importer groups,
  culls and marks static. The plan's first task measures every 4 × 4 block on
  the three test maps and records the distribution in this spec's amendments.
  The defaults stay unless that shows them splitting nearly every block or
  none.
- **Split rule (a quadtree on the chunk grid).** A block over either budget
  splits into four quadrants (2 × 2 chunks), and each of those can split into
  1 × 1. A 1 × 1 cell is never split, even when it is still over budget; that
  case is warned with its numbers. Blocks on the grid edge may be smaller than
  4 × 4. Cells never cross a page boundary. The cell id is
  `c<cx0>_<cy0>_<w>x<h>` in chunk coordinates.
- **Bucketing.** A prop instance and a collision instance belong to the cell
  that contains their translation's (x, y). A point outside the terrain is
  clamped to the nearest edge cell, so every placement lands somewhere
  (Queensdale has props just outside `rect`; frame §5.3). Large props that span
  cells stay whole in their translation's cell.
- **No terrain** (no `parm` rect, so no chunk grid, §2). The map gets one cell
  `c_all` that holds every prop and collision instance, with a warning. No grid
  is invented.
- **Region.** `--region cx0,cy0,cx1,cy1` (chunk coordinates, inclusive) exports
  only the cells inside it. Instances are bucketed first and then filtered by
  cell. This is the user's lever for VRChat's upload size (see Performance).

### D2. Props: one model folder each, placed as prefab instances

- **Each distinct model is written once**, as a VRChat model folder
  (`write_vrchat_folder`: `.glb`, `materials.json`, `Textures/`), under
  `models/<fileId>/`. When Blender is found, the folder also gets a `.fbx`. Its
  materials are therefore exactly the VRChat model export's: profile, Poiyomi
  preset, packed map, emission, decal, alpha mask and warnings.
- **Instances, not merged geometry, and not `EXT_mesh_gpu_instancing`.** The
  manifest lists each placement as (model, transform). The Unity importer
  places one prefab instance per placement and the Blender importer places one
  collection instance per placement. Reasons:
  - Merged geometry multiplies the size by the placement count (11 687
    placements on Queensdale, 18 571 on Lion's Arch, 2 425 on Spirit Vale;
    frame §5.3). It loses per-prop LOD, picking and editing, and it bakes the
    prop's material split into every cell.
  - `EXT_mesh_gpu_instancing` would only help a glTF importer inside Unity, and
    this route uses none: Unity reads FBX from Blender. Whether Unity's
    third-party glTF importers honour the extension is UNPROVEN.
  - Placing a prefab many times leaves Unity both options: static batching
    (static flags) or GPU instancing (`Material.enableInstancing`). Which one
    wins for a static Poiyomi material, and what each costs in VRChat, is
    UNPROVEN. The importer sets both (D6), and the manual checklist reads the
    draw-call count off the Frame Debugger.
- **Frame of a model file.** A model `.glb` written for the world uses
  `map_to_gltf_root(1)` as its root, the reflection the single-file world
  export already uses (`world_export.h`). It does not use the model export's
  default −90° about X, which `gltf_export.h` documents as a Z-up → Y-up turn.
  Frame §1.1 shows that map up is −Z, so the default would leave props upside
  down relative to their placements. Model-local data is then in map space, and
  an instance's transform is the map-space `PropInstance::world` converted by
  `frame.h`. `GltfExportOptions` gains an optional `rootMatrix` (a reflection
  is not a quaternion), and `write_vrchat_folder` takes the options through.
- **LODs: LOD0 only in stage A.** The model export writes `mesh.indices` only
  (`gw2-model-lods.md` §6). The LODGroup design in that note's §7 converts
  distances "in inches" with ×0.0254, a unit frame §1.3 leaves UNPROVEN. LODs
  are stage B (below), carried as raw distances plus the UNPROVEN scale.
- **Animated props are in stage A.** The model folder already carries every
  clip: the VRChat model export keeps them through Blender to FBX
  (`2026-10-07-vrchat-model-export-design.md` §6). A placement's clip is
  `resolve_prop_clip(model.animClipTokens, animSequence)`, the sub-project 3
  rule: exact or nothing, start at t = 0, loop. The manifest names the clip per
  instance (`clip`, or `null` when unresolved or when the clip failed to
  decode). Each case is counted, with the warnings of sub-project 3
  (`clipsFailedDecode` apart from "names no clip"). Collision does not follow
  the animation, as in sub-project 3.

### D3. Materials

- **Props:** `materials.json` exactly as the VRChat model export writes it (no
  second mapping). The Unity importer's material builder reads that schema, so
  it is also the Unity importer the model export's spec deferred (its
  sub-project 3).
- **Terrain: a baked albedo per cell, on the CPU, with `terrain_blend`.**
  - **Density.** `texelsPerChunk = 256` (setting; allowed values 64, 128, 256
    and 512). This is twice the blend page's own density of 128 px per chunk
    (512-px pages, 4 × 4 chunks, frame §4.2). It keeps the picker pattern
    sharp. Ground-texture detail does not survive: at 8 repeats per chunk and
    the 512-px cap, a chunk holds 4096 ground texels per side. A 4 × 4 cell is
    one 1024 × 1024 image.
  - **Per texel.** Texel (i, j) of chunk (cx, cy) has s = (i + 0.5)/T and
    t = (j + 0.5)/T, with T = `texelsPerChunk`. Row 0 is north, as for the
    chunk. The page is sampled bilinearly at `page_uv`. Each ground texture is
    sampled bilinearly at `ground_uv(s, t, groundRepeatsPerChunk)` on mip level
    `L = clamp(round(log2(repeats · groundWidth / T)), 0, last)` of its
    `decode_texture_capped` chain, so one texel averages the ground area it
    covers instead of aliasing. The blend is then `terrain_blend`, with
    `present[]` from the chunk's fileIds. The same colour space as the map
    window: colour textures are linearised when `srgbColourTextures`,
    blended, and written as sRGB 8-bit RGB PNG.
  - **Solid pages.** Spirit Vale's solid-colour pages become a constant texel
    from `pickerSolid` in stored byte order. That order is UNPROVEN, as in
    sub-project 3.
  - **Failures.** A chunk whose colour textures all fail, or that has none,
    bakes mid-grey (0.5). It is counted in `chunksUnbaked` and warned. This is
    the export's version of the map window's AO fallback.
  - **Where it lives.** The bake is pure, in `world`
    (`world/terrain_bake.h`), so the test that compares it with
    `terrain_blend` needs no files. It depends only on `terrain_blend.h` and
    `CappedTexture` (`extract`), and `world` already depends on `extract`.
- **Terrain normals.** Vertex normals from `chunk_normals` (seam-aware), welded
  across the cell. **No baked normal map in stage A.** The ground normal maps'
  tangent sign is UNPROVEN (sub-project 3, `terrainNormalMaps`), and blending
  them is a second bake. Stage B adds one behind the same labelled setting.
- **Water: a plane per cell.**
  - **Shape.** A grid with one vertex per terrain sample, at `water.planeZ`,
    over the cell's rect. It is written only when `water.hasPlane` and at least
    one terrain sample of the cell lies below the plane (z > planeZ, since up is
    −Z). A cell whose terrain is all above the plane would hide the plane under
    the terrain anyway. With `hasPlane` but no rect, the plane covers the prop
    extent of `c_all`, as sub-project 2's R9 does.
  - **Colour.** A water texture per cell, `waterTexelsPerChunk = 32`, RGBA:
    `lerp(shallow, deep, saturate(depth / depthAttenuation))` from
    `environment.water[light.presetIndex]`. The colour uses sub-project 3's
    rule (which water preset pairs with which lighting preset is UNPROVEN).
    Depth is measured below the plane from the terrain under each texel. Alpha
    is 0.45, the map window's flat-water alpha and castlemist's choice.
  - **Where it lives.** The colour function is a new pure
    `world::water_colour(preset, depth)`. It is the CPU form of the WaterPass
    rule, and a render test checks it against the HLSL at three depths.
  - **No preset.** The flat colour of sub-project 2.
  - **Left out.** Bump, fresnel and specular are not exported (stage B: a
    Poiyomi normal map with panning, whose units are UNPROVEN).
- **UNPROVEN settings that travel.** Every value below is written to the
  manifest's `settings` block with its value, its status (`UNPROVEN` or
  `castlemist's choice`) and the section of the research note that covers it.
  Each one is also a line in `warnings`.

  | setting | value | status, source |
  |---|---|---|
  | `groundRepeatsPerChunk` | 8 | UNPROVEN, T3D's guess, materials §1.6 |
  | `srgbColourTextures` | true | castlemist's choice (sub-project 3) |
  | `pickerSolidByteOrder` | as stored | UNPROVEN, frame §4.2 |
  | `waterPresetPairing` | preset k with lighting preset k | UNPROVEN, materials §6 |
  | `waterArea` | whole cell, depth-tested | UNPROVEN, frame §6.2 |
  | `waterAlpha` | 0.45 | castlemist's choice |
  | `uvScrollUnitSeconds` | 1 | UNPROVEN, materials §4 |
  | `darkDecalRule` | on | castlemist's own rule (sub-project 3 amendments) |
  | `blendModes` | the VRChat export's preset table | see below |
  | `collisionHullSelection` | `animations[last]` | UNPROVEN, frame §7.1 |
  | `unityScale` | 0.0254 | UNPROVEN, see D7 |
  | `clipPhase` | all at t = 0, looping | UNPROVEN, materials §5 |

- **Blend modes.** Props use the VRChat export's `renderPreset` and its raw
  factors (`blend_mode.h`), including Soft Additive and Custom-nearest with
  `exact: false`. The map window's exact-factor table (sub-project 3, P7/P8)
  governs only the renderer. The difference is stated in the manifest's
  `settings.blendModes`. The importer sets Poiyomi's preset and, where `exact`
  is false, its Advanced Blending factors from the raw ones
  (`poiyomi-10-importer.md` §2).
- **Dark-decal rule.** The export computes the flag the map window uses (a Clip
  material with no render state whose texels with alpha >= 64 are all <= 16)
  and lists flagged materials per model in the manifest. With `darkDecalRule`
  on (default), the importers switch those materials to the alpha-blended
  preset (Poiyomi Fade, glTF BLEND). `materials.json` itself is left as the
  model export writes it. The known risk travels with the setting's warning:
  the rule can misfire on black cutout props.
- **UV scroll.** Where a material's `texTransforms` scroll applies (sub-project
  3's rule: the diffuse's UV set, `columns = rows = count = 1`, the other fields
  zero), the manifest records
  `{model, material, uvIndex, scroll[2], unit: "per second, UNPROVEN"}`. The
  Unity importer sets Poiyomi's main-texture pan to `20 · scroll`, because pan
  is UV per `_Time.x` and `_Time.x = t/20` (`poiyomi-10-importer.md` §2, UV
  channels and panning). The Blender importer adds a driver on the mapping
  node.

### D4. Colliders

- **What goes in.** Each cell writes its collision as mesh nodes `Collider_k`
  in the cell glTF. The triangles are every collision instance bucketed into
  the cell, transformed to map space by the same code as
  `CollisionIndex::build`, then made cell-local. The terrain collider is the
  cell's terrain mesh itself: ground = collision ∪ terrain, as the map window
  walks (sub-project 2).
- **Shared transform.** The transform code is factored out of `CollisionIndex`
  into `world::collision_instance_triangles(scene, i, out)`, so the walker and
  the export use one copy.
- **Not simplified.** havk hulls are already the game's low-poly physics
  geometry.
- **Parts.** A cell's collision is split into parts of at most 65 535 vertices
  each, so no part needs 32-bit indices.
- **Unity.** The importer adds a non-convex `MeshCollider` per part and one on
  the terrain, on the Default layer, with no renderer. Things the export does
  not settle, all UNPROVEN and all on the manual checklist:
  - the triangle limits Unity or VRChat put on a non-convex static
    `MeshCollider`;
  - which layers VRChat's player collides with;
  - whether hull winding matters to PhysX. The single-file export already marks
    hulls double-sided, because their winding is not proven
    (`world_export.cpp`).
- **Blender.** Colliders are imported into a hidden `Colliders` collection per
  cell (optional, on by default).
- **Spawn hint.** The manifest's `spawn` is the first ground hit straight down
  from above the centre of the exported region, found with a `CollisionIndex`
  raycast, as sub-project 2's dat test drops a walker. With no hit it is
  `null`, with a warning.

### D5. Sky and light

- **Sky.** `exportgltf::sky::write_skybox` writes into `<out>/sky/`, unchanged
  (`2026-10-08-skybox-export-design.md`). The manifest references `sky/sky.json`
  and the day mode's `baked/equirect.png`. The Unity importer makes a
  `Skybox/Panoramic` material from it (the shader the skybox spec names) and
  sets `RenderSettings.skybox`. The Blender importer sets the World to an
  Environment Texture node. A map with "no sky" gets `sky: null`, with a
  warning.
- **Light: one sun and an ambient colour.**
  - **Sun.** `environment.light` gives the direction, colour, intensity and
    `presetIndex`. `sunDir` points toward the sun (`map_types.h:37`). The
    manifest carries it raw in map space and also as `unityForward`
    (`−map_to_unity(sunDir)`) and `blenderDirection`.
  - **Fill.** Colour and intensity, raw.
  - **Unity.** The importer creates one Directional Light (colour and
    intensity raw) and sets `RenderSettings.ambientMode = Flat` with
    `ambientLight = fillColor · min(fillIntensity, 1)`.
  - **Blender.** A Sun lamp and the World colour.
  - **Status.** The ambient formula and the use of raw intensities are
    castlemist's choice, named in `settings`. The map window's light calibration
    (`light_calib.h`) is renderer-specific and is not exported. Baked lighting
    and lightmap UVs are out of scope; Unity's own options (lightmap UV
    generation on FBX import) are the user's.

### D6. Importers

**Unity: `tools/unity/Castlemist/Editor/`**

- **Target.** C# 9, Unity 2022.3, the version VRChat uses (2022.3.22f1,
  `poiyomi-10-importer.md` §5). It is an editor-only asmdef, and Poiyomi is
  optional at compile time (the research note's Version Define).
- **Entry.** A menu item **Castlemist ▸ Import World…** picks an export folder
  that has been copied under `Assets/`. There is no postprocessor auto-trigger
  for worlds: thousands of assets arrive in many import batches. The
  postprocessor half of the research note's design (texture types and sRGB set
  by role in `OnPreprocessTexture`) is used as written.
- **Build steps.**
  1. Parse `manifest.json` with a bundled minimal JSON reader.
     `JsonUtility` cannot read the manifest's nested arrays.
  2. Refuse a major version it does not know.
  3. Materials: the research note's builder, from each model's
     `materials.json`, plus terrain (Poiyomi Opaque, `_MainTex` = albedo) and
     water (Poiyomi Fade, `_MainTex` = water texture). Without Poiyomi, the
     Standard shader is used, with one warning.
  4. Prefabs: one per model, from its `.fbx`, with materials remapped by
     `AddRemap` (research note, "FBX material remap").
  5. One `AnimatorController` per (model, clip) actually used, with a single
     looping state.
  6. Scene: `GW2_<fileId>` root (scale `unityScale`) → `Cell_<id>` at the cell
     origin → `Terrain`, `Water`, `Colliders`, `Props/<model>_<prop>`
     instances, with `unity.t/r/s` taken as given. The importer does no frame
     maths of its own.
  7. Static flags on terrain, props and colliders (batching, occluder,
     occludee); `enableInstancing` on prop materials.
  8. Sky and light (D5).
  9. Frame probe check (below).
  10. A summary line in the format of `validate_manifest.py --plan`.
- **What the importer does not do.** It never locks Poiyomi materials, and it
  does not add VRChat's scene descriptor. Adding the descriptor is a checklist
  step for the user; that VRChat needs it is UNPROVEN in the repo docs.
- **Re-import.** Existing `.mat` and `.prefab` assets are updated in place, so
  GUIDs stay stable (research note, Materials). User edits to materials are
  overwritten. That choice answers the research note's open question 6 for
  worlds.

**Blender: `tools/blender/castlemist_world.py`**

- **Two modes, one script.**
  - `--to-fbx <exportDir>` converts every `cells/*.glb`, `models/*/*.glb` and
    `probe.glb` to `.fbx` in **one** Blender session. Files already up to date
    are skipped. The FBX settings are the VRChat script's model mode, with
    textures not embedded: Unity uses the PNGs that `materials.json` names.
    castlemist runs this itself when `find_blender` finds Blender, as the
    VRChat export does.
  - `--import <exportDir>` builds the scene:
    - collection `GW2 <fileId>`;
    - `Models` (excluded from the view layer): one collection per model,
      imported from its `.glb`, and one more per (model, clip) used, with that
      clip's action assigned;
    - `Cells/<id>`, holding Terrain, Water, `Props` (collection-instance
      empties with `matrix_world = C_b M C_b⁻¹`, computed by the script) and
      the hidden `Colliders`;
    - a Sun lamp and the World sky.
  - Materials are the glTF ones from the `.glb` files, with the dark-decal
    switch applied when the setting is on.
- **Blender is installed on this machine**: Blender 3.6, 4.3 and 4.5 under
  `C:\Program Files\Blender Foundation`. So the importer is tested **headless**
  (`blender -b --factory-startup -P`), following `tests/blender/check_fbx.py`.
  A ctest runs it when `find_blender` finds Blender and skips otherwise.

**Frame probe.** Every export writes `probe.glb`: three boxes from one corner
(east bar 3 units long, north bar 2, up bar 1, in map space), placed in the
manifest at the spawn hint. Its `expect` block gives the probe's three bar
directions in Unity and in Blender, from `frame.h`. Both importers measure the
imported bars and log an error when a direction or length ratio is off. The
Blender headless test asserts the check. In Unity, the check is the one
automated proof that the glb → Blender → FBX → Unity chain is
`map_to_unity` × `unityScale`. That chain's net transform is UNPROVEN until
the probe passes. If it fails, the fix goes into the FBX axis settings of
`castlemist_world.py`, never into the importer.

**Verifying the Unity importer.** CI cannot compile or run it. It is verified
in four ways:

1. **Schema.** `tools/world/castlemist-world.schema.json` (JSON Schema
   2020-12), checked by ctest on every export the tests write.
2. **Dry run.** `tools/world/validate_manifest.py <dir> [--plan]` runs with no
   dependencies. It uses `jsonschema` when installed; it is not installed on
   this machine, so a bundled checker for the keywords the schema uses is the
   fallback. Beyond the schema it checks:
   - every referenced file exists;
   - indices are in range and matrices are finite;
   - every `unity` TRS reproduces `map_matrix_to_unity(m)` relative to the cell
     origin, within 1e-4;
   - cell rects tile the region without overlap;
   - the counts match `counts`.

   `--plan` prints what the Unity importer will create (materials, prefabs,
   controllers, instances, collider parts, lights) by the same mapping table as
   the C#, written in this spec's "Unity mapping" below.
3. **Optional local batchmode run.** Unity 6000.0.42f1 is installed here. A
   `-batchmode -executeMethod` run that compiles the importer and imports the
   Spirit Vale export is an optional local check, not CI. It does not prove
   2022.3.
4. **Manual checklist** in `docs/testing.md` (below), run in a VRChat 2022.3
   project with Poiyomi 10.

### D7. Units and scale

All files keep map units (`frame.h`: axes change, scale does not). Units per
metre stay UNPROVEN (frame §1.3), and the manifest's
`frame.units.perMetre` is `null` with status `UNPROVEN`.

The Unity importer applies `unityScale` on the map's root GameObject. It is a
labelled user setting (importer window and manifest), defaulting to 0.0254.
That default is the "GW2 inches" reading the model export already uses
(`gltf_export.h:57`); nothing proves it, and the setting's tooltip and the
warning say so.

A scale is needed for use, not for correctness. At 1, Queensdale would be
86 016 Unity units across, and a VRChat avatar would be tiny against GW2
props. The Blender importer uses 1 by default with the same optional setting.
The CLI's old `--scale` for the single-file export is unchanged.

### D8. The old `MapScene` / `map_fly` path is removed in this sub-project, as its last task

- **The condition is met.** Sub-project 2 kept the old path until the map
  window had textures, and sub-project 3 gave it textures.
- **What the old path still holds.**
  - The main window's map preview (`PreviewKind::Map`, `build_map_scene`,
    `RenderMode::MapFly`), which loads every prop model when a map entry is
    selected.
  - **File ▸ Export glTF (Map)…** (`export_map_gltf`). This export is upside
    down by frame §1.1: map up (−Z) lands at glTF −Y.
- **Why it is removed here.** This sub-project adds the replacement for the
  second item, **File ▸ Export World (Map)…**. Removing the old export in the
  same branch means the menu never offers a known-wrong exporter next to the
  right one.
- **Why it is the last task and not the first.** The removal is independent of
  the export and can be dropped without touching it. Doing it last keeps the
  main window usable until the new menu item exists.
- **The task.**
  - Removed: `src/extract/map_scene.cpp` (`build_map_scene`), `MapScene` /
    `MapInstance`, `src/render/map_fly.cpp` and `RenderMode::MapFly`,
    `src/exportgltf/map_export.cpp` (`export_map_gltf`), and their tests.
  - Kept: whatever the new path still uses (`mesh_bake`, `MapEnvRig` if the
    model preview uses it, the fly camera if `WorldView` does). The task greps
    each symbol before removing it.
  - A map entry in the main window then shows its `world` summary, plus a line
    pointing to **Tools ▸ Map Window…**.

## Architecture

```
world       cells.h          partition_cells(scene, stats, CellBudget, region) -> CellPlan
                             (pure: quadtree on page-aligned blocks, bucketing)
            terrain_bake.h   bake_chunk_albedo(material, TextureLookup, T, repeats, srgb, rgbOut)
            water_colour.h   water_colour(MapEnvWater, depth, rgbaOut)        (pure)
            collision.h      + collision_instance_triangles(scene, i, out)    (shared with CollisionIndex)
            terrain.h        + terrain_texture_ids(const Terrain&)            (render's world_texture_ids calls it)
exportgltf  world_folder.h   export_world_folder(scene, ModelSource&, dir, WorldFolderOptions) -> WorldFolderReport
            world_manifest.h manifest_json(...), the version constant
            world_cell.cpp   cell glTF: terrain (welded, cell-local), water, collider parts, external PNG images
            gltf_writer      + image by URI; GltfExportOptions::rootMatrix
            vrchat_export    write_vrchat_folder(..., const GltfExportOptions&)
ripper      vrchat.cpp       run_blender reused for castlemist_world.py --to-fbx
tools       gw2dat_cli       world-export --out-dir ...
            world/           castlemist-world.schema.json, validate_manifest.py
            blender/         castlemist_world.py
            unity/           Castlemist/Editor/*.cs (+ .asmdef)
ui          file_ops.cpp     File > Export World (Map)... (replaces Export glTF (Map)...)
```

Layer rules: `exportgltf` depends on `extract` and `world` only. `render`
depends on `exportgltf`, so the export must not call `render`. This is why
`world_texture_ids` keeps its terrain half in `world`, and why
`WorldMaterialSettings` values used by the export (repeats, sRGB, cap) are
passed as plain fields of `WorldFolderOptions`, with defaults taken from the
same `world` constants.

**Model loading.** `ModelSource` is an interface ("give me model i, or null"),
so tests can feed synthetic models. The dat implementation is the CLI's
existing pool, moved into a library function shared by the CLI and the UI. It
loads with `ModelLoadOptions{wantGame = false, decodeTextures = false}`, as the
map window does. Textures are decoded per fileId through
`decode_texture_capped(maxTextureSize)`, and each mip 0 is filled into the
preview's `ModelTextureCPU::rgba` before `write_vrchat_folder`. The map window
draws correct blend modes with `wantGame = false`, which suggests
`renderState` is present without the game trace. The plan's first task proves
it: `materials.json` for model 1766522 must be byte-equal with `wantGame` true
and false. If it is not, the export uses `wantGame = true` and the task records
the cost.

**Order of work.**

1. `load_world`; bucket instances; apply the region.
2. Load and write each needed model folder in parallel, recording LOD0
   triangles, texture bytes at the cap, clip names and dark-decal flags. A
   model is released once written.
3. `partition_cells` from those stats.
4. Decode the terrain textures (the `terrain_texture_ids` of the region's
   chunks) once.
5. Bake and write each cell in parallel.
6. `write_skybox`.
7. Probe.
8. Manifest.
9. Blender `--to-fbx`, when found.

The report gives the time for each stage.

## Data formats

### Output folder

```
<out>/
  manifest.json
  probe.glb  [probe.fbx]
  cells/<id>.glb  [<id>.fbx]          terrain + water + Collider_k nodes, images by URI
  cells/<id>_albedo.png               (cw·T) x (ch·T), RGB sRGB, row 0 north
  cells/<id>_water.png                RGBA, only when the cell has water
  models/<fileId>/<fileId>.glb  [<fileId>.fbx]  materials.json  Textures/*.png
  sky/sky.json  sky/<mode>/...        write_skybox, unchanged
  export.log                          the report as JSON
```

- **Textures are external PNG files**, referenced by URI from the glTF.
  - Unity imports PNG as texture assets and recompresses them on import.
    Unity's default format choice is UNPROVEN here.
  - Blender's glTF importer resolves relative URIs; the headless test checks
    this.
  - Nothing is stored twice. The `.fbx` does not embed textures.
- **Model `.glb` files** keep the model export's materials and point to the
  folder's `Textures/` PNGs by URI instead of embedding them. This is an option
  in `write_vrchat_folder`; the single-model VRChat export keeps embedding.
- **Cell glTF.** One root node `GW2_MapToGltf` (`map_to_gltf_root(1)`), then a
  `Cell` node translated to the cell origin (rect centre, z = 0). Its children
  are `Terrain`, `Water` and `Collider_k`, with vertices relative to that
  origin. Positions near the origin keep float precision on large maps.
  `TEXCOORD_0` covers 0..1 across the cell, with v = 0 at the north edge.
  Materials:
  - `Terrain`: baseColor = the albedo.
  - `Water`: baseColor = the water texture, `BLEND`.
  - `Collision`: unlit, invisible to the importers, which skip its renderer.

### manifest.json (version 1)

```jsonc
{
  "format": "castlemist-world", "version": 1, "castlemist": "<git describe>",
  "map": { "fileId": 1151420, "bounds": [x0, y0, x1, y1] | null },
  "frame": {
    "space": "gw2-map", "handedness": "left", "east": "+X", "north": "+Y", "up": "-Z",
    "toUnity":   [[1,0,0],[0,0,-1],[0,1,0]],          // frame.h map_to_unity
    "toBlender": [[1,0,0],[0,1,0],[0,0,-1]],          // frame.h map_to_blender
    "gltfRoot":  [16 floats],                          // map_to_gltf_root(1)
    "units": { "perMetre": null, "status": "UNPROVEN", "source": "gw2-world-frame.md §1.3",
               "unityScale": { "value": 0.0254, "status": "UNPROVEN", "source": "gltf_export.h:57 'GW2 inches'" } }
  },
  "settings": { "<name>": { "value": ..., "status": "UNPROVEN" | "castlemist's choice" | "measured",
                            "source": "<doc §>" }, ... },     // the D3 table + texelsPerChunk, maxTextureSize, budgets
  "terrain": { "present": true, "chunksX": 10, "chunksY": 25, "chunkSize": [3072, 3072],
               "segments": 32, "region": [cx0, cy0, cx1, cy1] } | { "present": false },
  "models": [ { "index": 0, "fileId": 123, "folder": "models/123", "glb": "...", "fbx": "..." | null,
                "materials": "models/123/materials.json", "triangles": 4210, "textureBytes": 1398101,
                "clips": ["idle", ...], "darkDecalMaterials": ["Mat_58246"],
                "uvScroll": [ { "material": "Mat_...", "uvIndex": 1, "scroll": [0, 0.2] } ],
                "status": "ok" | "failed", "error": "..." } ],
  "cells": [ { "id": "c4_8_4x4", "chunks": [4, 8, 7, 11], "rect": [x0, y0, x1, y1],
               "origin": [x, y, 0], "glb": "cells/c4_8_4x4.glb", "fbx": "..." | null,
               "albedo": "cells/c4_8_4x4_albedo.png", "albedoSize": [1024, 1024],
               "water": { "texture": "cells/..._water.png", "z": 0 } | null,
               "colliderParts": 2, "colliderTriangles": 51234,
               "budget": { "triangles": 812345, "textureMB": 141.2, "split": false, "overBudget": false },
               "instances": [ { "prop": 1234, "model": 0, "group": "propArray",
                                "m": [16 floats, column-major, map space, absolute],
                                "unity": { "t": [3], "r": [x, y, z, w], "s": 1.0 },   // local to the cell node, before unityScale
                                "clip": "idle" | null } ] } ],
  "water": { "plane": true, "z": 0, "preset": 0, "shallow": [r, g, b, a], "deep": [...],
             "depthAttenuation": 15.7 } | { "plane": false },
  "sky": { "json": "sky/sky.json", "default": "day", "equirect": "sky/day/baked/equirect.png" } | null,
  "light": { "presetIndex": 0,
             "sun": { "toSun": [3], "unityForward": [3], "blenderDirection": [3], "color": [3], "intensity": 1.3 },
             "fill": { "color": [3], "intensity": 1.15 } },
  "spawn": { "map": [3], "unity": [3] } | null,
  "frameProbe": { "glb": "probe.glb", "fbx": "..." | null, "map": [3],
                  "expect": { "unity": { "east": [1,0,0], "north": [0,0,1], "up": [0,1,0] },
                              "blender": { "east": [1,0,0], "north": [0,1,0], "up": [0,0,1] },
                              "lengths": [3, 2, 1] } },
  "counts": { "props": 2425, "propsWritten": 2425, "propsSkipped": 0, "models": 0, "modelsFailed": 0,
              "collisionInstances": 0, "cells": 0, "chunks": 250, "chunksUnbaked": 0,
              "animated": 116, "animatedResolved": 106, "clipsFailedDecode": 0 },
  "warnings": [ "<every WorldScene warning, as is>", "export: ...", "settings: ... UNPROVEN" ]
}
```

- **Versioning.** `version` is an integer major. Adding an optional field
  keeps it; renaming, removing or changing a meaning bumps it. Importers refuse
  an unknown major.
- **Schema.** `tools/world/castlemist-world.schema.json` (`$id`
  `castlemist-world/1`) uses `additionalProperties: false` at every level the
  importers read, so a writer change that the schema does not know fails the
  tests.
- **Counts.** `counts.props` is `scene.props.size()` for the region.
  `propsWritten + propsSkipped = props`. Counts across cells must add up.

### Unity mapping (the table the C# and `--plan` share)

| manifest | Unity object |
|---|---|
| `models[i]` with status ok | `Prefabs/<fileId>.prefab` from its `.fbx`; materials `Materials/<fileId>/<Mat>.mat` from `materials.json` |
| distinct (model, clip) in instances | `Controllers/<fileId>_<clip>.controller` (one looping state) |
| `cells[j]` | `Cell_<id>` under `GW2_<fileId>`, position `toUnity · origin` |
| cell terrain | `Terrain` MeshRenderer + MeshCollider; `Materials/terrain/<id>.mat` |
| cell water | `Water` MeshRenderer, `Materials/water/<id>.mat` (Fade) |
| `colliderParts` | `Colliders/Collider_k` MeshCollider, no renderer |
| `instances[k]` | prefab instance `Props/<fileId>_<prop>` with `unity.t/r/s`; Animator + controller when `clip` |
| `light` | `Sun` Directional Light; ambient Flat |
| `sky` | `Materials/Skybox.mat` (`Skybox/Panoramic`), `RenderSettings.skybox` |

## Front end

- **CLI.** `gw2dat_cli world-export --dat <dat> --file-id <map> --template
  <json> --out-dir <dir> [--region cx0,cy0,cx1,cy1] [--texels-per-chunk 256]
  [--max-texture 512] [--cell-max-chunks 4] [--cell-max-tris 1000000]
  [--cell-max-texture-mb 256] [--no-sky] [--no-fbx] [--no-collision]
  [--overwrite] [--threads n]`. It prints the report as JSON.
  - `--out <file.glb>` keeps today's single-file export unchanged. Exactly one
    of `--out` and `--out-dir` must be given.
  - The output folder must be empty or absent. A folder that already holds a
    `manifest.json` is replaced only with `--overwrite`. Anything else is
    refused.
- **UI.** **File ▸ Export World (Map)…** replaces Export glTF (Map)…, with the
  same background-thread pattern and status-bar/message-box report as Export
  Skybox (Map)…, and default options.
- **No "Export…" button in the Map window.** Its models are loaded header-only
  and its texture cache lives on the GPU, so an export from it would need a
  second load anyway: the same cost as the menu item. Stage B, if wanted.

## Performance and size

- **Measured inputs.** At the 512-px cap, sub-project 3 measured GPU texture
  memory (RGBA8 + mips) of 624 MB on Queensdale (1 124 textures), 761 MB on
  Lion's Arch (1 301) and 361 MB on Spirit Vale (547). Mip 0 alone is
  3/4 of that: about 470, 570 and 270 MB of raw RGBA8.
- **Estimates (UNPROVEN; the dat test measures Spirit Vale and the plan
  records all three maps).**
  - **Prop textures.** PNG of game textures at about 40–60 % of raw. The VRChat
    folder also writes derived maps per material (BaseColor, Normal, Packed,
    Emission). A shared texture is written once per material that uses it,
    which is the VRChat export's rule. Expected on disk: about 0.3–0.8 GB for
    Queensdale and about 0.15–0.4 GB for Spirit Vale.
  - **Terrain albedo at 256 texels per chunk.** Queensdale's 532 chunks are
    532 · 256² · 3 B = 105 MB raw, roughly 60–90 MB as PNG. Spirit Vale's 250
    chunks are about 49 MB raw.
  - **Geometry.** LOD0 `.glb` and `.fbx` per model, tens to a few hundred MB
    per map. Cells: about 33 k terrain triangles each at 4 × 4, plus colliders.
  - **Total, Queensdale.** About 0.5–1.5 GB with FBX. Unity's imported library
    is larger again: it recompresses to GPU formats (UNPROVEN size).
- **VRChat.** VRChat limits world upload size. The figure is in no repo doc
  (UNPROVEN), and the export does not enforce it. The levers are `--region`,
  `--max-texture` (256 or 512; default 512, the map window's measured cap) and
  `--texels-per-chunk`. The report prints the folder size by kind (prop
  textures, terrain, geometry, sky) so the user can choose.
- **Time.** The model stage is bounded by sub-project 3's loader (models
  streamed in 8 s on Queensdale, header-only) plus texture decode and PNG
  encode. The Blender `--to-fbx` session for about 1 000–1 500 models is
  UNPROVEN and is measured on Spirit Vale in the plan. If it is far too slow,
  the fix is to batch more per session, not to drop FBX.

## Error handling

`export_world_folder` follows `load_world`'s rule: bad map or model data never
throws and never yields a silently empty export.

- A model that fails to load or write gets `status: "failed"` with its error.
  Its placements are skipped and counted. A model that loads with no drawable
  meshes gets `status: "empty"` (world-foliage fix wave, M1), counted in
  `counts.modelsEmpty` (and still in `modelsFailed`); its placements are
  skipped with their own "empty models" warning.
- A texture that fails drops that map only, with the VRChat export's warning.
- A terrain chunk with no usable colour texture bakes grey and is counted.
- A cell with no terrain, water or collider is still written if it has
  instances, and omitted otherwise.
- No sky: `sky: null`. No Blender: `fbx: null` everywhere, and the report says
  Unity needs `gw2dat_cli world-fbx --dir <folder>` or
  `castlemist_world.py --to-fbx <folder>` to be run (either records the `.fbx`
  files in the manifest; see Amendments, I3).
- A Blender failure keeps its log in `export.log` and leaves the rest of the
  folder valid.
- Errors that stop the export: dat I/O, a missing template, an output folder
  that is not allowed, and a disk-full or failed write. Each leaves a partial
  folder with no `manifest.json`, so no importer mistakes it for a finished
  export. The manifest is written last.
- Importers:
  - an unknown major version, or a missing file the manifest names, is an
    error naming the file;
  - missing Poiyomi falls back to Standard, with a warning;
  - a failed frame probe is an error that names the axis;
  - everything else is a warning in the importer's summary.

## Testing

- **Pure, world (`cm_test_world`).**
  - `partition_cells`: a 10 × 25 grid with 4 × 4 pages gives 3 × 7 blocks, with
    the edge blocks sized 2 × 4 and 4 × 1 and the corner 2 × 1. One heavy block splits into four
    quadrants, and a heavy quadrant into 1 × 1. An over-budget 1 × 1 stays and
    warns. No cell crosses a page boundary. The cells tile the region exactly.
    Bucketing puts a translation on a shared edge in exactly one cell and
    clamps an outside point to the nearest edge cell. With no terrain, one
    `c_all` cell.
  - `bake_chunk_albedo`: with synthetic 4 × 4 textures, each texel equals
    `terrain_blend` fed the bilinear samples at that texel's `page_uv` /
    `ground_uv`, within 1/255. The mip level follows the formula. A solid page
    gives a constant picker. An absent slot carries weight 0. A failed chunk
    bakes grey.
  - `water_colour`: depth 0 gives shallow, depth >= `depthAttenuation` gives
    deep, and halfway gives the midpoint.
  - `collision_instance_triangles`: matches `CollisionIndex::build`'s triangles
    exactly on a hand-built scene, so the refactor changes nothing.
- **Render (`cm_test_render`).** `water_colour` agrees with the WaterPass HLSL
  at three depths. The existing blend readback test already ties
  `terrain_blend` to the GPU, so the bake inherits it.
- **exportgltf (`cm_test_exportgltf`).** `export_world_folder` on a synthetic
  scene (2 × 2 chunks, two cube models, one animated with a clip, one collision
  hull, water) and a fake `ModelSource`:
  - the folder layout is complete;
  - the cell glTF parses and has welded terrain, cell-local positions,
    images by URI and collider parts;
  - each instance's `unity` TRS round-trips to `map_matrix_to_unity(m)`;
  - counts add up;
  - the dark-decal flag and the UV-scroll entry are present;
  - the manifest is written last.
- **Schema and dry run (ctest, needs Python 3).** `validate_manifest.py` on the
  synthetic export passes. Three corrupted copies (a missing file, an index out
  of range, an extra field) each fail with the expected message. `--plan` on
  the synthetic export prints the expected counts. The test is skipped when
  Python is absent.
- **Blender (ctest label `blender`, skipped without Blender).**
  `castlemist_world.py --import` on the synthetic export, headless:
  - the collection tree;
  - one collection-instance empty per placement;
  - matrices equal to `C_b M C_b⁻¹` within 1e-4;
  - the frame probe check passes;
  - terrain and water materials have their images;
  - colliders are hidden.

  `--to-fbx` writes an `.fbx` for every `.glb`, and `tests/blender/check_fbx.py`
  finds the animated model's clip in its FBX.
- **Dat (`cm_test_world_dat`, skipped without `GW2_TEST_DAT`), Spirit Vale
  1151420.** Export to a temp folder, then:
  - `validate_manifest.py` passes (when Python is present);
  - `counts.props` equals `scene.props.size()`;
  - `chunks = 250`, `chunksUnbaked = 0`;
  - `animatedResolved = 106` (sub-project 3's measured figure);
  - every model in `scene.models` appears once;
  - the cells' collider triangles total the scene's collision triangles;
  - 32 random downward rays hit the same height (within 0.01) against a
    `CollisionIndex` built from the exported cell meshes as against one built
    from the scene;
  - folder sizes and stage times are printed for the amendments.
- **Manual checklists (`docs/testing.md`, "World export").**
  - **Unity**, in a VRChat Unity 2022.3 project with Poiyomi 10, importing the
    Spirit Vale export:
    - the compile is clean;
    - the summary line equals `validate_manifest.py --plan`;
    - the frame probe passes;
    - terrain textures line up with prop placement, as in the Map window shot;
    - animated props play;
    - walking in Play mode with a CharacterController stands on the terrain and
      the havk floors of the raid;
    - water sits at the shore;
    - sky and sun look right;
    - draw calls from the Frame Debugger, and the import time, are recorded.
  - **Blender:** the same scene viewed, a Cycles render at the spawn point,
    and the animated props play on the timeline.
  - **Results.** Both lists record their results, with dates, in the spec's
    amendments. Anything that was not tried is listed as not tried.

## Done means

- `gw2dat_cli world-export --out-dir` and File ▸ Export World (Map)… export
  Spirit Vale, Queensdale and Lion's Arch.
  - Every manifest validates against the committed schema.
  - Counts match `WorldScene`.
  - Every cell has a baked albedo, and its colliders cover the scene's
    collision.
- The Blender importer builds Spirit Vale headless with the probe passing, and
  the Blender manual checklist has been run.
- The Unity importer builds Spirit Vale in a VRChat 2022.3 project with Poiyomi
  materials, prefabs, instances, colliders, water, sky and sun, with the probe
  passing. The manual checklist is recorded. If no VRChat Unity project was
  available, the spec says so and done is "dry run + schema", stated as such.
- Every UNPROVEN setting is in the manifest's `settings` and `warnings` and in
  its code comment, with its source.
- The old `MapScene` / `map_fly` / `export_map_gltf` path is gone, and the main
  window's map entry points to the Map window.
- Pure, render, exportgltf, schema, Blender and dat tests pass.

## Plan shape (at most 15 tasks)

1. Measure and settle the open inputs:
   - per-block budget figures on the three maps;
   - `wantGame` false versus true `materials.json` equality;
   - one model glb → Blender → FBX timing;
   - the Blender FBX axis settings that make the probe pass in a Blender
     re-import.

   Results go into this spec's amendments.
2. `world/cells.h`: partition and bucketing, with tests.
3. `world`: `collision_instance_triangles` (refactoring `CollisionIndex`),
   `terrain_texture_ids`, `water_colour`, with tests (plus the render
   agreement test).
4. `world/terrain_bake.h`, with tests.
5. `exportgltf`: `GltfExportOptions::rootMatrix`, image by URI, and
   `write_vrchat_folder` options (no embedded textures, root). The
   single-model VRChat export stays byte-identical, and a test pins that.
6. Cell glTF writer (terrain, water, collider parts) and the probe, with tests.
7. Manifest writer and `castlemist-world.schema.json`.
8. `export_world_folder` orchestration: `ModelSource`, the dat loader moved
   out of the CLI, sky, light, spawn, report, and the synthetic end-to-end
   test.
9. `validate_manifest.py` (schema fallback checker, semantic checks, `--plan`)
   and its ctest.
10. CLI `--out-dir` and its options.
11. `castlemist_world.py` (`--to-fbx`, `--import`) and the headless Blender
    ctest; `run_blender` wiring.
12. Unity importer C# (manifest reader, materials.json → Poiyomi, prefabs,
    controllers, scene, colliders, sky, light, probe, summary).
13. Dat test on Spirit Vale; measured sizes and times on all three maps into
    the amendments.
14. File ▸ Export World (Map)… and removal of `MapScene` / `map_fly` /
    `export_map_gltf` (D8).
15. Docs: `docs/testing.md` checklists, the `docs/research` notes' cross-links,
    and the manual runs recorded.

## Stage B (later, not in "done")

- **LODs.**
  - `<Model>_LODn` meshes sharing vertex accessors (`gw2-model-lods.md` §7).
  - Raw `lod1/lod2` and the `OVERRIDE_LOD` flag per placement in the manifest.
  - The Unity importer builds LODGroups, computing screen heights per instance
    with the note's formula and `unityScale` (both UNPROVEN).
- **A terrain normal map per cell**, blended with the bake weights, behind
  `terrainNormalMaps` (tangent sign UNPROVEN).
- **Water bump, fresnel and panning** as a Poiyomi normal map with pan (units
  UNPROVEN).
- **Occlusion.** Automated occlusion-culling bake settings in the Unity
  importer. Stage A only sets the static flags.
- **Map window "Export…" button.**
- **Collider simplification** (welding and decimation) if the checklist shows
  colliders are a cost.
- **A `.fbx`-free Unity path** through a glTF importer package, if one proves
  reliable.

## Out of scope

- Udon and gameplay data. Update 2026-10-18: zone models (foliage) are now sub-project 5 (`2026-10-16-world-foliage-design.md`), and Udon is sub-project 6.
- Placed effects and particles; the model folders' `_particles.json` is copied,
  not built.
- Terrain as a splat shader in Unity: the four ground textures and pages
  blended live, at full ground detail.
- Game DXBC in Unity.
- Reflection probes (the skybox spec's sub-project 2).
- Lightmap baking.
- Quest/Android builds and their limits.
- Rivers and shore surfaces.
- Per-placement `color`, `constants` and `blitTextures`.
- Terrain `uvData` animation.
- Collision that follows animated props.
- Zone models.
- VRChat scene-descriptor setup and upload.
- Poiyomi locking.

## Amendments (2026-10-15, after implementation)

- **D8 done (Task 15).** File ▸ Export World (Map)… replaced Export glTF (Map)… on the same menu id, and the old path is
  removed: `map_scene.cpp` (`build_map_scene`, `build_map_zone_layer`, `augment_map_scene_game`), `MapScene` /
  `MapInstance`, `map_fly.cpp` / `detail/map_fly.h` / `flycam.cpp` (`RenderMode::MapFly`, the fly camera: `WorldView`
  does not use it), `map_export.cpp` (`export_map_gltf`) and their tests. Kept: `set_scene` / `SceneInstance`, `MapEnvRig`
  / `set_environment_rig`, `mesh_bake`. A map entry previews its `world_summary` text and points to Tools ▸ Map Window….

- **D1, cell budget (Task 13).** `cellMaxTriangles` is 500 000, not the table's 1 000 000. The budget rule gave it:
  on Queensdale 0 of 35 blocks split at 1 000 000, so the default is Queensdale's p90 block (496 740 triangles) rounded
  up to 500 000. At 500 000, Queensdale splits 3 of 35 blocks (44 cells) and has no cell over budget. Lion's Arch
  splits 12 of 15 (93 cells, one 1 x 1 cell over budget) and Spirit Vale 8 of 21 (66 cells, one 1 x 1 cell over budget).
  Spirit Vale was 36 cells at 1 000 000, so its `--plan` line changed (see "Spirit Vale PLAN" below).
- **D5, sun direction (Task 13).** The stored `sunDir` is the direction the light travels, not the direction toward the
  sun as `map_types.h:37` and this section say. Every day preset measured has z > 0 in map space, where up is -Z
  (Queensdale (0.809, 0, 0.588), Lion's Arch (0.747, 0, 0.664), Spirit Vale (0.707, 0, 0.707)); read as "toward the
  sun", all three suns would be below the horizon. The manifest's `unityForward` and `blenderDirection` follow the
  stored vector (`map_to_unity(sunDir)`, `map_to_blender(sunDir)`), and `toSun = -sunDir`. Spirit Vale's
  `unityForward` is (0.707, -0.707, 0), so the light shines down. The final review's fixes (I7) settle the rest; see
  "Fix wave" below.
- **Axis conversion (Task 1, Task 12).** D6 says a failing frame probe is fixed "in the FBX axis settings of
  `castlemist_world.py`, never in the importer". The measurement says otherwise. On Blender 4.5 the glb lands on
  `map_to_blender` (east +X, north +Y, up +Z). Unity turns the FBX 180 degrees about Y (east -X, north -Z) unless
  `ModelImporter.bakeAxisConversion = true`; with it, east +X, north +Z, up +Y, no negative-scale transform and the
  winding right. `axis_forward` '-Z' and 'Z' give the same Unity result. The FBX settings stay the VRChat script's
  (`axis_forward='-Z'`, `axis_up='Y'`, `FBX_SCALE_ALL`), and the importer sets `bakeAxisConversion = true`
  (`FrameProbe.BakeAxisConversion`; the postprocessor sets it on first import, and the importer re-imports any `.fbx`
  that has the wrong value). Proven on Unity 6000.0.42f1 (Task 1) and on 2022.3.22f1, the user's editor (Task 12):
  bake on passes on both, bake off fails on both ("east,north"). In Task 14 the manifest's
  `settings.unityAxisConversion` source text and the code comments that said "never into this importer" or "2022.3
  not run" were corrected. On the Blender side (Task 1), `transform_apply` in 4.5 does not flip the winding, so
  `--to-fbx` also flips the normals of a mesh whose world determinant was negative.
- **Armature models (Task 11).** Blender 4.5's glTF importer plays a rig under a det -1 root mirrored: the rig stays
  mirrored with the faces inward, and the clip moves and turns the wrong way (skinned probe: east 150 -> 50 instead of
  150 -> 250). The script imports a root-matrix-free copy of a skinned glb, then rebuilds the rig at identity (rest
  bones, keyframes and the skinned mesh conjugated by the reflection). The glb and FBX round-trips then have
  `negScale=0`, outward faces and the right clip motion. In Unity 2022.3.22f1 the skinned probe imports un-mirrored
  with its clip moving the right way (it also passes with bake off, so it does not prove the bake setting).
- **Model loading, `wantGame = true` (Task 8).** The spec's "Model loading" paragraph says `wantGame = false` if
  `materials.json` is byte-equal. It is not: 18 entries differ (blend, preset, cull, emission). The dat source
  therefore defaults to `wantGame = true`, which costs 2.2 s on Spirit Vale (model stage 26.7 s against 24.5 s,
  about 9 %). The map window's `wantGame = false` path has no render state, so its blend modes come from pixel
  guesses.
- **RGB albedo (Task 8).** The terrain albedo PNG is written as RGB (the spec's "Output folder" format) through the
  new `encode_png_pixels(w, h, channels, pixels)`, because `encode_png` only takes RGBA `ModelTextureCPU`.
- **Images, textures and the model folders (Task 5, Task 8).** A model's glb references its images by URI
  (`Images/<fileId>.png`), not embedded. Images the exporter generates (baked emissive, `_metalRough`, baked diffuse)
  have no dat fileId, are named by image name, and are sanitised to `[A-Za-z0-9._-]` on the URI path. Task 5 decided
  that the spec's "point to the folder's `Textures/` PNGs" means the glb's own raw images, from which the Poiyomi maps
  in `Textures/` are built. Task 8 (W8) then moved dat textures to a shared `textures/<fileId>.png` pool, written once,
  with relative URIs from the model glbs; generated images stay per model. Spirit Vale went from 2.14 to 1.66 GB; the
  pool is 1154 files, 246 MB. The per-material Poiyomi maps in `models/*/Textures/` are about 959 MB, often a diffuse
  copy; content-hash pooling is later work. Done in world-foliage Task 15d (2026-10-17): generated
  maps and images go once per content into `textures/gen/`, and materials are shared by content key (see the
  world-foliage spec's amendment).
- **Instance positions (Task 7, W7).** Instance positions are in map units relative to the cell origin; the importer
  applies `unityScale` on the map root. `instance.model` is an index into the manifest's `models[]`, not the scene's
  models.
- **Animated instances are not static (Task 12).** The Unity importer clears the static flags on animated instances
  (a moving object must not be static), and on water. D6's "static on props" holds for the other props.
- **Terrain diagonal (Task 13).** The cell terrain quads are split along `CollisionIndex`'s diagonal (NW-SE,
  p00-p11), not NE-SW as first written: with NE-SW only 24 of 32 downward rays agreed with the scene's
  `CollisionIndex`, with NW-SE all 32 do. The world view (`world_view.cpp` `grid_indices`) drew NE-SW until the fix
  wave (M1) and now draws NW-SE too. Which diagonal the game uses is unproven.
  **Superseded 2026-10-18** (world-foliage fix wave, review I2): all three now split NE-SW, the
  client's ground query as measured by the zone scatter (4535 / 4535 collider z); the 32 rays still agree
  because the mesh and `CollisionIndex` changed together.
- **Poiyomi (Task 12).** Poiyomi 10 is untested: none exists on this machine. Poiyomi Pro 9.0.alpha18 was copied
  read-only from `C:/UnityProjects/2019WorldUDON-Migrated/Assets/_PoiyomiShaders` into a scratch project, and the
  importer built Poiyomi materials from it (shader found; `_Mode`, queue, `RenderType`, `_MochieBRDF`, `MOCHIE_PBR`
  and the multipliers set). The preset table follows the research note for 10; the property names for 10 are
  unchecked.
- **FBX time (Task 11, Task 12).** The fixture's 4 glbs convert in 0.8 s (about 0.2 s a file). Spirit Vale: 417 `.fbx`
  in 116 s (`world-fbx`). The 30-minute batching rule was not triggered and is not implemented: one Blender session,
  a 4 h limit.
- **Batchmode (Task 12).** The importer ran in batchmode on Unity 2022.3.22f1 and 6000.0.42f1 with 0 errors and no
  `error CS` or `warning CS`. The fixture's `PLAN` equals the dry run (with and without Poiyomi, both editors).
  Spirit Vale (the 36-cell export, 2022.3.22f1, with Poiyomi 9 and without): `PLAN` equals the dry run; the importer
  took 59 s with Poiyomi, 75 s on a first import without it and 29 s on a re-import; 36 cells centred; 2354 instance
  matrices within 2.7e-5 of `m`. It was not re-run on Task 13's 66-cell export.
- **Measurements (Task 13).** Three maps at `cellMaxTriangles` 500 000. The measure test is gated by
  `CM_WORLD_EXPORT_MEASURE=1` (about 10 GB, 5 minutes).

  | | Queensdale 192711 | Lion's Arch 191000 | Spirit Vale 1151420 |
  |---|---|---|---|
  | cells | 44 | 93 | 66 |
  | chunks | 532 | 228 | 250 |
  | models (failed) | 2181 (35) | 1699 (52) | 392 (12) |
  | props (skipped) | 11 687 (195) | 18 572 (263) | 2425 (71) |
  | animated (resolved) | 280 (269) | 443 (294) | 116 (106) |
  | blocks split | 3 of 35 | 12 of 15 | 8 of 21 |
  | block triangles, min / median / p90 / max | 68 178 / 243 442 / 496 740 / 926 814 | 293 288 / 929 914 / 2 990 140 / 3 085 850 | 4096 / 32 768 / 1 608 220 / 2 136 910 |
  | block texture MB, median / p90 / max | 71.0 / 120.0 / 147.6 | 246.5 / 516.6 / 584.9 | 3.06 / 232.1 / 312.0 |
  | folder | 3.78 GB | 4.28 GB | 1.66 GB |
  | prop textures (report) | 3151 MB | 3600 MB | 1336 MB |
  | export seconds | 98.9 | 100.4 | 52.1 |
  | models with a primitive that has no material | 27 | 32 | 10 |

  Prop textures are 3.2 GB on Queensdale and 3.6 GB on Lion's Arch, far above the spec's estimate of 0.3 to 0.8 GB;
  the per-material Poiyomi maps dominate. 12 to 52 models fail per map ("no drawable meshes"), and every model listed
  without instances is a failed one. On Windows an image path over MAX_PATH (260) fails to write (3 Spirit Vale models
  under a long `TMP`); there is no `\\?\` support.
- **Spirit Vale PLAN (Task 14).** Re-exported at the 500 000 budget with
  `gw2dat_cli world-export --out-dir <dir> --no-fbx` (67 s); `validate_manifest.py <dir> --plan` passes and prints
  `PLAN materials=1057 prefabs=380 controllers=37 cells=66 instances=2354 colliders=52 terrainColliders=66 water=0 lights=1 skybox=1`.
  Task 12's line (`materials=1027 ... cells=36 ... colliders=25 terrainColliders=36`) is from the 1 000 000 budget.
- **Blender manual checklist (Task 14, 2026-10-15, Blender 4.5.3 headless, the 66-cell Spirit Vale export).**
  - Tried, `castlemist_world.py --import <dir> --verify`: 380 models, 66 cells and 2354 collection-instance empties
    (413 to 450 s), 66 terrain images, collider collections hidden, frame probe `negScale=0 OK` (east +X, north +Y,
    up +Z; extents 300 : 200 : 100). `--verify` reports FAIL, `MATRIX_MAX_ERR 0.000368` against its 1e-4 limit: the
    placements differ from the manifest's Unity TRS by more than 1e-4 on this map. The fixture passes, and Unity's
    matrices were within 2.7e-5. Not investigated.
  - Tried, Cycles at the spawn (16 samples, 480 x 270, CPU, no denoise): the spawn equals the frame probe's position
    (0, 1536, 6020.8), and a camera 70 above it is inside the probe and sees only sky. A camera behind and above the
    spawn shows the terrain and props with the probe bars at the spawn, but dark (the sun does not reach the area at
    this sample count). An overview from above shows the terrain, props and probe in place. The renders are in the
    session scratchpad and are not committed.
  - Tried, the timeline (frames 1 to 250, 24 fps): 37 animated armatures, every F-curve (13 390) has a Cycles
    modifier, and bone poses changed on 9 of the 37 at frames 8, 16, 41, 100 and 250. The other 28 did not differ
    from frame 1 at those frames; not investigated.
  - Not tried: a person watching the timeline play, the materials in a shaded viewport, a lit render of the spawn
    area, and water (Spirit Vale has none).
- **Unity manual checklist (Task 12, 2026-10-15).** Done: items 1 to 3 in batchmode on both editors; the results are
  in `tools/unity/CHECKLIST.md`. Not tried: items 4 to 11 (terrain against props, animated props in Play mode,
  colliders, water, sky and sun, the Poiyomi look, draw calls and import time in the Frame Debugger, a VRChat build).
  No VRChat 2022.3 project with Poiyomi 10 was used, and the user's projects in `C:/UnityProjects` were not opened.
- **Task 15 (done).** See the D8 note above.
- **Fix wave after the final review (2026-10-16).** Behaviour changes:
  - **I1, region.** `--region` snaps outward to whole blocks (`CellPlan::region`), so split cells are never cut; the
    manifest's `terrain.region` is that kept rectangle, not the request. A region that keeps no cells is an error,
    and the CLI refuses an inverted or negative region.
  - **I3, FBX.** `castlemist_world.py --to-fbx` now records the `.fbx` files in `manifest.json` itself (as
    `set_manifest_fbx` does), so either `--to-fbx` or `gw2dat_cli world-fbx --dir` makes a folder Unity-ready; every
    "no FBX" message names both.
  - **I6, overwrite.** `--overwrite` and the GUI's "Replace?" clear a folder only when its `manifest.json` parses with
    `"format": "castlemist-world"`; any other non-empty folder is refused, with the reason.
  - **I7, sun.** The env `sunDir` (`MapEnvLight::sunDir`) and `sky.json`'s `sun.direction` are the direction the
    light travels (values unchanged). The world view negates the env vector once, in `set_world`, so its shaders get
    toward-sun and the Map Window is lit from above (before: from below). `MapEnvRig::sunDir` stays toward-sun.
  - Also: writes past MAX_PATH go through extended-length paths (I4); closing the app during an export asks, cancels
    the export and Blender, and Blender runs in a kill-on-close job (I5); empty models are counted apart as
    `modelsEmpty` in the report (M2).
  - **Lion's Arch, re-measured** with the template refreshed for the 2026-10-14 patch (`--no-fbx`, into a
    165-character folder): 96 cells, 1651 models written, 48 failed (all "no drawable meshes"), 18 319 instances,
    validator OK. The 93-cell row above is Task 13's, on the earlier template.
