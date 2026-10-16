# Architecture

castlemist is a stack of small libraries, one per layer. Dependencies point
downward only, and CMake enforces it: a layer can only include what its
`target_link_libraries` gives it.

```
app       WinMain, 13 lines                                    (executable)
 +-- ui         window procs, layout, widgets, content browser, dialogs
      +-- render     Direct3D 11: image, model, map scene, gizmo, particles, the map window's WorldView
      |    +-- sim        the GW2 cloth solver and its mesh bridge
      |    +-- world      WorldScene, collision, walking, prop streaming
      +-- exportgltf  binary glTF (.glb) writer (models, maps, animation)
      +-- extract    MFT entry -> previewable payload (the format dispatcher)
      |    +-- media      audio (dr_mp3 / stb_vorbis) and Bink video
      |    +-- format     single-format decoders
      +-- db         the SQLite index reader
           +-- core       byte readers, packfile traversal, text, env hooks
                +-- native  dat/MFT, method-0 codec, ATEX, MODL, granny
```

Each layer is one DLL in the development presets and one archive in the release
presets; `CASTLEMIST_SHARED_LAYERS` picks. Nothing in the code depends on which,
because symbols are exported wholesale rather than annotated.

`render` -> `world` is the map window's edge: `WorldView` draws a
`castlemist::world::WorldScene` directly, and `world` itself sits on `extract`
(it loads maps through the same packfile readers). Like `sim`, it is a
sideways edge that the picture shows as a branch of `render`.

`render` -> `exportgltf` is the materials edge: the Map window's prop material
pass picks each material's alpha mode with the glTF export's own rules
(`profile_for`, `alpha_tested`, `decode_blend`) instead of keeping a second
copy. `exportgltf` is CPU-only and depends on `extract` and `world`, never on
`render`, so there is no cycle.

`sim` is the other edge that does not read like the picture: it sits beside
`render` but depends on `extract`, because the solver is fed a `ModelMeshCPU`
straight out of model extraction. `exportgltf` sits beside it for the same
reason: it turns a `ModelPreview` into a binary glTF (`.glb`) file, and a map's
`world::WorldScene` into a world export folder (per-cell `.glb` and a manifest)
-- geometry, embedded PNG textures, skeleton/skin and animation all in one
self-contained file -- with no Direct3D involved. glTF was chosen over FBX
after a hand-written FBX binary writer proved to be fighting an undocumented
format (see `include/castlemist/exportgltf/gltf_export.h`'s comment for the
full rationale); for the final Unity/VRChat hop, open the `.glb` in Blender
and use Blender's own FBX exporter.

## The rule that shapes everything

**Extraction runs on a worker thread; rendering runs on the UI thread.**

That is why `extract` produces plain `std::vector` payloads — `ModelPreview`,
`AudioClipCPU`, the world layer's `WorldScene` — and never a Direct3D object. The renderer uploads
them later. It is also why `extract_entry` has two overloads: one that borrows
an open `Gw2Dat`, and one that takes a path and opens its own handle, touching
no shared mutable state.

Concretely: nothing below `ui` may include a ui header, and nothing below
`render` may mention Direct3D.

## The layers

### native

The file-format primitives recovered from the client: the dat/MFT index, the
method-0 Huffman+LZ77 codec and its encoder, the ATEX container family, MODL
geometry and materials, granny skeletons and animation, and the packfile
struct-template parser. `tools/gw2index`, `tools/gw2dat_cli` and both prototype
viewers link this same layer, so there is exactly one implementation of each
format in the repository.

### core

Dependency-free primitives, and therefore the easiest part to test.

- `castlemist::core::Bytes` — bounds-checked little-endian reads that saturate to zero
  instead of faulting. GW2 blobs are truncated, mis-flagged and full of
  self-relative pointers that land outside their chunk often enough that a
  reader which *cannot* fault is worth more than one that reports errors nobody
  checks.
- `castlemist::core::PackfileReader` — `PF` header, chunk walk, `array_ptr`, UTF-16
  strings, filename references. The pointer width is not a property of a struct:
  the same `ModelFileData` is 4-byte-pointered in an old MODL and 8-byte in a
  new one, and reading a 64-bit blob as 32-bit yields *zero-length arrays rather
  than an error*. That bug produced 0.0-second animation clips for months.
- `castlemist::core::to_ansi`, `bytes_to_wide`, `looks_like_text`, `format_mmss`, ...
- `castlemist::core::env` — the typed `GW2_*` debug hooks, listed in one table so the
  code and @ref md_docs_2testing "Testing" cannot drift.

### format

One decoder per file format, each independent of the others:
`castlemist::dds`, `castlemist::img` (JPEG via libjpeg-turbo, WebP via libwebp,
PNG via WIC, stb as the last resort), `castlemist::strs`, `castlemist::skeys`
(the RC4 string decryptor), `castlemist::chat` (`&[base64]` links),
`castlemist::cmap`, `castlemist::tpl` (the struct template registry).

### extract

The dispatcher plus one file per format family, sharing `src/extract/internal.h`:

| File | Handles |
|------|---------|
| `entry_extractor.cpp` | the dispatcher — sniff, then route |
| `image_preview.cpp` | signature sniffing, DDS and ATEX previews |
| `texture_source.cpp` | reading model textures, full/reduced pair resolution |
| `game_shader.cpp` | the game's own bgfx (DXBC) material shaders |
| `model_preview.cpp` | MODL → `ModelPreview` |
| `pimg_atlas.cpp` | paged-image atlases composited into one preview |
| `audio_bank.cpp` | AMSP sound scripts → playable clips |
| `content_store.cpp` | the cntc `PackContent` datastore |
| `pf_summary.cpp` | eula, ABIX, text-pack manifests |
| `cinematic.cpp` | CINP/CSCN subtitles and referenced movies |

Detection order matters: cheap magic checks first, structural packfile checks
next, and the "looks like text" heuristic last, because it accepts almost
anything.

### render

`castlemist::render`, split by pass. The shared Direct3D state lives in
`src/render/detail/state.h` as C++17 `inline` variables — one definition across
the layer, declared next to the comments that explain it.

| File | Handles |
|------|---------|
| `device.cpp` | device, swap chain, targets, pipeline objects |
| `game_material.cpp` | building D3D objects from the game's DXBC blobs |
| `geometry.cpp` | uploading a model or a scene; LOD and texture-size selectors |
| `skeleton.cpp` | bind pose, animation, bone palette, CPU skinning |
| `lighting.cpp` | the deferred light pre-pass and tone mapping |
| `particles.cpp` | the baked emitter system |
| `cloth_bridge.cpp` | driving the cloth solver from the displayed mesh |
| `gizmo.cpp` | the Blender-style transform gizmo and grid |
| `scene.cpp` | map drawing, picking, the inset preview |
| `model_renderer.cpp` | the single-model entry point and the orbit camera |
| `world_view.cpp` | `WorldView`: the map window's own device, swap chain, terrain chunks, water, layers, picking |
| `world_draw.cpp/.h` | `WorldView`'s instanced draw: per-instance culling, LOD and draw batches |
| `world_props.cpp/.h` | streamed prop models: CPU prep (`prepare_world_model`, run on the load workers), upload, per-model AO, per-submesh ranges, instance lists, pick triangles |
| `world_materials.cpp/.h` | the material pass: `TextureCache` (one GPU texture per fileId), prop alpha modes and exact blend factors, UV scroll, the material clock, animated-prop poses, water maths, and `material_setting_warnings` |
| `world_view_shaders.h` | the textured HLSL: terrain blend (`kTerrainMat`), props (`kPropMat`, skinned and static), water |
| `world_camera.cpp/.h` | `WorldCamera` -> view/projection matrices and the pixel-to-ray maths |
| `detail/mesh_bake.h` | the AO baker, vertex flattener and frustum planes used by `WorldView` |
| `detail/light_calib.h` | the measured light calibration (sun colour and ambient DC), shared by the Shader-mode material rig in `game_material.cpp` and `WorldView`'s textured shaders |

`detail/math.h` is row-major with a **row-vector** convention (`v * M`), matching
`tools/viewer/gw2viewer.cpp` exactly so that castlemist's own shaders and the game's DXBC
shaders see byte-identical matrices. Translation lives in the matrix's last row.

The map window's textured passes are configured by `WorldMaterialSettings`
(`include/castlemist/render/world_prep.h`, public because the load workers
need `maxTextureSize` and `world_texture_ids` without Direct3D). Every field
that is a guess or castlemist's own choice shows up in `WorldView::warnings()`
(`material_setting_warnings`) and so in the Map window's Warnings panel.

`detail/shaders.h` holds every HLSL source string, so a shader can be found and
diffed without reading pipeline code.

### world

`castlemist::world`: one `WorldScene` per map (`load_world`), in map space
(left-handed, +X east, +Y north, up = -Z). Besides the scene and its readers:

- `walk.h` -- `CollisionIndex` (collision and terrain triangles in map space,
  bucketed into an XY grid, with `raycast`) and `Walker`, the first-person
  controller (gravity, jump, slide, step-up, fall safety). Pure: no GPU, no dat.
- `terrain_blend.h` -- `terrain_blend`, the lo-res terrain blend of the game's
  pixel shader (research note 1.2) on the CPU, with the page and ground UV
  rules. `TerrainBlendInput::present[4]` marks a slot that binds no texture
  (fileId 0): it gets weight 0, castlemist's rule. The terrain HLSL repeats
  the same arithmetic and a test holds the two to 1/255.
- `anim.h` -- `resolve_prop_clip`, the exact match of a placement's
  `animSequence` token to a clip of its model (no fallback).
- `stream.h` -- `model_load_order`, the nearest-instance-first order in which
  the map window loads prop models; `rank_models`, the same rule over a subset;
  and `ModelQueue`, the thread-safe remainder that the map window re-sorts
  around the moving camera while its workers pop from the front.
- `terrain_mesh.h` -- `chunk_normals`, seam-aware per-sample terrain normals.

### ui

`castlemist::ui`, split by area of the window, sharing `src/ui/detail/app_state.h`:
`theme`, `layout`, `listview_util`, `view_controls`, `audio_ui`, `video_ui`,
`content_browser`, `preview`, `file_ops`, `chat_link_dialog`, `index_ui`,
`window_proc`, `application`, and `map_window` (with `map_window_loader` and
`map_window_prompt`): the Map window, a separate top-level window with its own
`WorldView`, worker threads and input handling. Its fly-camera and walk-delta
maths are plain functions in `detail/map_window.h` so the `ui` suite can test
them without a window.

`AppState` is one struct because the window has one selected entry. The header
also holds the control-id table, which a unit test checks for collisions —
duplicate ids do not fail to compile, they just make one control quietly stop
responding.

## Where the GW2 knowledge lives

Format facts belong in the comment next to the code that depends on them, not
in this file. The ones most likely to bite:

- **fileId is not baseId.** A file reference is a `{u16 lo, u16 hi}` pair folded
  back as `0xFF00 * hi + lo - 0xFF00FF`, with both halves `>= 0x100`. A raw
  dword read of the same bytes gives a plausible but wrong id.
- **GW2 is Z-up.** A map prop's yaw is a rotation about Z.
- **Textures ship in full/reduced pairs** at consecutive MFT indices, and a
  material may reference *either*. Following the reference literally renders
  armour at half resolution.
- **Game material vertex shaders take already-skinned vertices.** They have no
  blend inputs and no bone-matrix uniform, because the engine skins in a prior
  pass — so the renderer CPU-skins into a dynamic buffer for that path.
