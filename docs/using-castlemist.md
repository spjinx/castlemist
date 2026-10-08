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
| Export Skybox (Map)... | the selected map's sky as a folder of PNGs: `sky.json`, and per sky mode (`day`, `night`, `mode2`, `mode3`) `baked/equirect.png` + six `baked/` faces, plus `skybox/` faces when the map stores its own cube; the name you type becomes the folder name |

### Tools

| item | does |
| ---- | ---- |
| Decode Chat Link... | paste a chat link (`[&AgF+KQEA]`) to see what it encodes, resolve it to its models and textures, and jump to them. **Rebuild map** here rebuilds the content map after a patch |
| Character Ripper... | fetch and export your characters (getting-started §8) |
| Download all game names | fetch every name at once (getting-started §7) |
| Decode Token / Filename Bytes... | decode the game's packed token and filename values (research aid) |

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
gw2dat_cli skybox  --dat $dat --file-id 187611  --template dumps\packfile\gw2_packfile.json --out skies    # folder skies\map_187611 (--name, --size <face px>)

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
