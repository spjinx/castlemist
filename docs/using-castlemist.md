# Using castlemist

A tour of the window, the menus and the command line, for after
[getting-started.md](getting-started.md). Each section says which data file
(if any) the feature needs.

## The window

```
+----------------------+------------------------------------+------------------+
| search + filters     | Compressed | Decompressed |         | archive info     |
|                      | Structure  | Preview              | selected entry   |
| file list            |                                    | index record     |
|  # Base ID File IDs  |   preview of the selected entry    | model details    |
|  ... Type  Name      |   (image, 3D view, audio, text)    | Game content     |
+----------------------+------------------------------------+------------------+
| status bar                                                                    |
+-------------------------------------------------------------------------------+
```

Drag the splitters between panes to resize them; drag column edges in the list
to resize or hide columns.

### The file list (left)

One row per asset in the archive.

| column | meaning |
| ------ | ------- |
| **Base ID** | the asset's number in the archive. Most ids you'll see quoted are base ids |
| **File ID(s)** | the other numbers the game uses to reach the same asset |
| Offset, Size, Uncompressed, Comp | where and how it's stored |
| **Type**, **Container** | what it is, e.g. `texture`, `packfile / MODL` *(needs the index)* |
| **Name** | the in-game name of what uses it, `(+N)` if more things share it *(needs the content map; fills in fully after **Download all game names**)* |

Click a column header to sort by it; click again to reverse.

### Searching

The box at the top left:

| type | finds |
| ---- | ----- |
| a name or part of one (`astralaria`, `20 slot`) | every asset used by something with that in its name |
| a number (`477426`) | that base id |
| a number with **By File ID** ticked | that file id |

Press **Search** or Enter; **Clear** resets the search and the filters.

With the index loaded, three boxes under it filter by **type**, **container**
and **content** (e.g. "Rigged - has SKEL" for models with a skeleton). They combine with the search
box.

### The middle tabs

| tab | shows |
| --- | ----- |
| **Compressed** / **Decompressed** | the raw bytes, before and after decompression, as hex |
| **Structure** | the parsed fields of a packfile, named *(needs the struct template)* |
| **Preview** | the entry itself: image, 3D model, map, sound, video or text |

### Previews

- **Textures:** zoom with the wheel; toggle the alpha channel.
- **Models** *(struct template)*: drag to orbit, wheel to zoom. The toolbar
  switches between shaded, plain and wireframe, shows the skeleton, plays
  embedded animations, picks LODs and submeshes, toggles cloth, and moves the
  model with the gizmo (**Move / Rotate / Scale**). **Textures** lists every
  texture the model uses. **UV Map** shows a submesh's UV layout. **Game 1:1**
  renders with the game's own shaders (needs the bgfx libraries from
  `fetch_externals.ps1 -WithBgfx`).
- **Materials** (model toolbar): an editor-style window for the materials
  **Shader** mode draws with, using only the game's own shaders and textures.
  Previews are spheres drawn with each material's real shader.
  - **Mesh Renderer**: one slot per material. Drag a material from the
    browser onto a slot, or press the slot's target button and double-click one.
  - **Inspector**: **Shader** picks the AMAT and its default or blended effect;
    **Textures** take any texture (click a slot, or drop one on it);
    **Properties** are the shader's uniforms (drag to scrub; right-click to
    revert or show as a colour).
  - **Browser**: every material and texture seen this session, with search.
    **Add model** / **Add texture** bring in more by file id.

  A blue bar marks what is edited. Edits stay with the model for the session
  and survive dye picks; **Save...** / **Load...** keep them as JSON. Full,
  Plain and **Game 1:1** are not affected.
- **Maps** *(struct template)*: the map's placed props (terrain and water
  aren't drawn yet); drag and zoom like models.
- **Sounds:** play, seek, and pick a clip in multi-sound banks.
- **Videos:** cinematics play *(needs `bink2w64.dll`, see getting-started §9)*.
- **String tables:** each record's text; encrypted ones are marked rather than
  shown as garbage.
- **Content packs (cntc):** a browser of the game's content database. Pick a
  **type** (Item, Skin, Map, ...), then an **entry** (with its **Name** and
  **API id**), then one of its **assets** to preview it.

### The right-hand panel

Everything known about the selected entry: archive header, the raw record, what
the index recorded (true type, real sizes, whether its compression flag is
honest), model details (meshes, materials, skeleton, textures), and **Game
content**: every item, skin, map, outfit, achievement or skill that uses this
file, with its name, its chat link and the items that unlock it. Select and
copy any of it.

## Menus

### File

| item | does |
| ---- | ---- |
| Open .dat... | open an archive (`Gw2.dat`, or your account's `Local.dat`) |
| Open Index DB... | open a previously built index (also reopens its archive) |
| Open File... | preview a single file from outside an archive |
| Build Index DB from .dat... | build the index (getting-started §6) |
| Load Struct JSON... | load `gw2_packfile.json` from somewhere else |
| Load String Keys... | load captured string keys (`textkeys.csv`) |
| Export Compressed / Decompressed... | save the selected entry's raw bytes |
| Export glTF (Model)... | the selected model as `.glb` with its textures |
| Export glTF (Model, Baked UV Atlas)... | the same, with all materials baked into one atlas texture |
| Export for VRChat (Model)... | the selected model as a folder for Unity/VRChat: `.glb`, `.fbx` + `.blend` (needs Blender), `Textures` (Poiyomi-ready PNGs) and `materials.json`; the name you type becomes the folder name |
| Export glTF (Map)... | the selected map scene as `.glb` |
| Export Skybox (Map)... | the selected map's sky as a folder of PNGs: `sky.json`, and per sky mode (`day`, `night`, `mode2`, `mode3`) `baked/equirect.png` + six `baked/` faces, plus `skybox/` faces when the map stores its own cube; the name you type becomes the folder name; a preview first lets you pick what shows below the horizon (fade to black / white, or the game's stretched horizon) |

### Tools

| item | does |
| ---- | ---- |
| Decode Chat Link... | paste a chat link (`[&AgF+KQEA]`) to see what it encodes, resolve it to its models and textures, and jump to them. **Rebuild map** here rebuilds the content map after a patch |
| Character Ripper... | fetch and export your characters (getting-started §8) |
| Download all game names | fetch every name at once (getting-started §7) |
| Decode Token / Filename Bytes... | decode the game's packed token and filename values (research aid) |
| Map Window... | open a map in its own window to fly over or walk on (see below) |
| Data status... | whether each file an export reads is current for the open Gw2.dat (see below) |

#### Data status

The right end of the status bar says whether everything castlemist exports from
is current: **Data: up to date**, or **⚠ Data: 2 stale** in amber. Click it (or
**Tools > Data status...**) for one row per input, each with its state, why,
and a fix button:

| row | judged by | fix |
| --- | --- | --- |
| Gw2.dat | the archive on disk still has the MFT castlemist loaded (the game can patch it while castlemist is open) | reopen it |
| Index | its `dat_fingerprint` (older indexes: size + entry count) against the open dat | rebuild (only changed entries are re-read) |
| Struct template | its `source.peTimestamp` against the installed `Gw2-64.exe`, and any chunk version in the index it has no struct for | regenerate with Ghidra, then Load Struct JSON |
| Content map | the dat fingerprint stamped when it was built | Rebuild content map |
| Game names | the dat fingerprint stamped when **Download all game names** finished | Download all names |
| API keys | that the file reads and has a key; **Check API keys online** asks the API whether each key still works and has its scopes | Manage keys |
| Saved looks, string keys, Blender | that they are present and readable; string keys are stale when older than the last game patch castlemist saw | |

*Stale* means made for an older game build. *Unknown* means nothing records
what a file was built from, usually because it predates this check. Rebuilding
it once stamps it. The stamps live in `data_stamps.json` beside the exe.
The Character Ripper repeats any stale rows in its status line when it opens.

#### Map Window

A separate top-level window that loads a whole map (terrain, props, collision,
water) and lets you fly over it or walk on it. It needs an open `.dat` and the
struct template (`gw2_packfile.json`).

**Opening it.** **Tools > Map Window...** asks for a map fileId and fills in the
selected entry's when that entry is a map. There is no toolbar button in the
main window. Only one Map window is open at a time; opening another replaces it.
It is a separate desktop window (not kept above the main one), so clicking a
prop brings its preview up in the main window in front of it.

**Loading.** The terrain appears first. Props then stream in nearest-first from
wherever the camera is (the queue re-sorts as you move), on a pool of worker threads, so the view fills in
around you. A full map takes about 15 to 30 seconds in the debug build (measured
totals are in [testing](testing.md#the-map-window)); you can fly or walk the
whole time. Textures are decoded once on the worker threads, capped at 512 px,
and uploaded with each batch of models; the terrain's textures arrive on their
own thread and the terrain is drawn untextured until they land.

**Controls**

| input | does |
| ----- | ---- |
| W A S D | move |
| Q / E | down / up (fly only) |
| Shift / Ctrl | fast (x6) / slow (x0.15) |
| right-drag | look |
| wheel | change speed (fly and walk each keep their own) |
| Tab | switch walk / fly |
| Space | jump (walk only) |
| left-click | pick the prop under the cursor (a click that moves more than 4 px is not a pick) |
| F, or **Reset** | frame the whole map from above |

**Walk vs fly.** Fly is a free camera. Walk puts the camera at eye height on
the ground, with gravity, jumping, sliding along walls and stepping up low
ledges against the map's own collision, plus its terrain heightfield. Walk uses
its own speed (300 map units per second to start), not the fly speed. Walk is
available once the collision index has finished building; Tab says so in the
status line if it is not, or if there is no ground under the camera (fly over
the map first). A fall that goes too far returns you to the last safe ground.

**Layers.** The **Terrain**, **Props**, **Collision** and **Water** check boxes
show or hide each layer (the **Materials** and **Motion** toggles are described
below). Collision is off by default: it is the map's physics
mesh, 988,986 triangles on Queensdale (world_dat test output; 2,078,522 with the terrain), and uploads to the GPU only the first
time you turn it on. The water is a single plane at the stored height.

**Foliage.** The **Foliage** button draws the map's scatter layer: the grass,
plants and trees the game plants from its zone data, as instanced models that
fade out with distance. Click the button to turn it on or off (the label shows
`Foliage: Colliders`, `Foliage: All` or `Foliage: Off`); click its arrow to
choose **Colliders** (only the trees that have a collision hull, which is what
you bump into when walking) or **All** (every zone instance; about 369,000 on
Queensdale). **All** is greyed out when the map's scatter could not be
reproduced. It starts on, in Colliders mode. The status line shows `fol V/N`
(foliage drawn of those placed). The scatter follows the game's own rule for
trees, and a transcribed rule for grass and plants that no collision hull can
confirm, so the Warnings list names those settings as UNPROVEN. No sway: foliage
stands still. One Queensdale grass model (57017) has no mesh and is left out.

**Sky.** The **Sky** button cycles the sky's time of day. **Approx sky**
(pressed by default) shows castlemist's approximate sky, which adds the
clouds, haze and material cards the proven bake leaves out, from inputs that
are labelled UNPROVEN; unpress it for the proven-only bake. The approximate
sky has not been compared with the game, and some maps (Spirit Vale) look very
different under it.

**Materials and Motion.** Two push-style toggles, both on by default.

- **Materials** draws the terrain with the map's own ground textures blended by
  its picker pages, and props with their diffuse textures, cut-out and blend
  modes, under the game-calibrated light. The water is drawn from the map's
  water preset (depth tint, scrolling bump normals, fresnel, a sun glint), not
  as a flat plane. Off, the window draws the plain shaded look it had before
  textures (the "Geometry" look). Anything that cannot be textured (a chunk
  with no colour texture, a prop with no usable diffuse, a blend mode with no
  exact equivalent) falls back to that plain look, and the counts are in the
  headless stats line (`chunksUntextured`, `materialsUntextured`).
- **Motion** plays the scene's time: animated props play their named clip
  (looping from t = 0), and prop materials whose UVs scroll scroll. Off, the
  clock stops and everything freezes where it was; on resumes from there. A long stall does not make the
  motion jump: the clock moves at most 0.1 s per step.

Terrain `uvData` (the Spirit Vale soul rivers) is read but not animated. Not
drawn in this version: shadows, normal, specular and glow maps on props,
reflections, flipbooks, placed effects and particles. A prop whose animation
names no clip of its model stays in its bind pose; the `anim` figure in the
status line counts them (Lion's Arch has 134 against 2 on Queensdale and 10 on
Spirit Vale, a finding not yet explained).

Textures are not restored after a graphics device reset: the window reloads
the map onto a new device and tells you so, and terrain, props and water fall
back to the plain look until you reopen the window.

**Picking.** Left-click (without dragging) a prop to open its model in the main
window's preview, with the usual Full / Shader / Game 1:1 views, dyes and
exports. The status line says which prop and model fileId. Props that have not
loaded yet, and hidden layers, cannot be picked.

**Status line and warnings.** The bottom line shows the load stage, models
loaded of total (and any that failed), then `tex N (X MB)  anim A/U` (GPU
textures and their size; props playing a clip, and props whose clip could not
be resolved), then visible items, draw calls,
kilotriangles, frame time in ms, position, mode and speed. The **Warnings**
button toggles a panel listing everything `load_world` could not read (the
same list `gw2dat_cli world` puts in its summary), followed by one line for
each material setting that is a guess or castlemist's own choice. A line that
says **UNPROVEN** is a value the dat or the research note does not settle (for
example the water bump units, the terrain `uvData` target, the order of the
terrain solid-colour bytes); a line that says **castlemist's choice** is a
number castlemist picked (for example the 512 px texture cap, the water
specular power, the water bump strength scale, the water alpha, and the dark
decal rule). Change either kind and the picture changes, so they are listed. A map that fails to load
shows its error in the status line and leaves the window open.

Without Materials, props are drawn with simple shading (a hemisphere and sun
light from the map's own lighting, ambient occlusion baked per model). With
it, the textured passes use the same measured light calibration as the main
window's Shader mode, with the map's own sun and fill intensities, not the
game's deferred lighting or shadows.

### View

**Theme:** dark (default), light, or a custom accent colour.

## The command line

`gw2dat_cli.exe` (next to `castlemist.exe`) does most of the same things
without a window and prints JSON, so it scripts well. The common ones:

```powershell
$dat = "C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat"

# What is this entry?
gw2dat_cli info    --dat $dat
gw2dat_cli lookup  --dat $dat --file-id 1200313
gw2dat_cli sniff   --dat $dat --base-id 477426

# Save it
gw2dat_cli extract --dat $dat --base-id 477426 --out model.bin       # decompressed bytes
gw2dat_cli texture --dat $dat --base-id 46403  --out icon.png        # decoded texture
gw2dat_cli model   --dat $dat --file-id 1766522 --template dumps\packfile\gw2_packfile.json --glb dagger.glb
gw2dat_cli model   --dat $dat --file-id 1766522 --template dumps\packfile\gw2_packfile.json --vrchat out   # folder out\model_1766522 for VRChat
gw2dat_cli skybox  --dat $dat --file-id 187611  --template dumps\packfile\gw2_packfile.json --out skies    # folder skies\map_187611 (--name, --size <face px>, --ground black|white|skirt, --ground-fade <deg>)

# A whole map as one scene (terrain, props, collision, water, sky): a JSON summary
# whose warnings include every dat chunk nothing reads yet
gw2dat_cli world   --dat $dat --file-id 192711 --template dumps\packfile\gw2_packfile.json --out summary.json

# A whole map as a cell folder for Unity or Blender (manifest.json, cells, models,
# foliage records, Unity Terrain tiles, sky). Keep the path short.
gw2dat_cli world-export --dat $dat --file-id 1151420 --template dumps\packfile\gw2_packfile.json --out-dir C:\cm\sv
#   --foliage off|colliders|all   which zone foliage records to write (default all)
#   --no-unity-terrain            leave out the terrain/ tiles the importer's Terrain mode uses
#   --sky-approx                  bake the approximate sky too (UNPROVEN inputs; off by default)
#   --no-fbx                      skip the Blender FBX step

# What uses a file, with names and chat links (needs the content map; --names goes online)
gw2dat_cli users --file-id 1200313 --names
gw2dat_cli users --content-type 66 --content-id 7562                 # the reverse: a skin's files

# Characters (API key saved in the Character Ripper)
gw2dat_cli character --key-name main                                  # list characters
gw2dat_cli character --key-name main --character "Name" --out m.json  # fetch one
gw2dat_cli character-assemble --manifest m.json --dat $dat --out char.glb
gw2dat_cli character-vrchat   --manifest m.json --dat $dat --out avatar.fbx
```

In Unity, put a skybox export on a `Skybox/Panoramic` material with
`<mode>/baked/equirect.png`, or on `Skybox/6 Sided` with the six faces
(`baked/` or `skybox/`); `sky.json` lists which face goes in which slot.

`gw2index.exe` builds the index from the command line
(getting-started §6), and the `mcp\` folder exposes the archive and the index to
AI agents; see [`mcp/README.md`](../mcp/README.md).
