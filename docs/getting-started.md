# Getting started

From a fresh Windows PC to browsing your Guild Wars 2 archive, searching it by
item name, and ripping a character. Follow it top to bottom the first time;
each step says what it unlocks, so you can stop once you have what you need.

| step | you get | time |
| ---- | ------- | ---- |
| [1. Install the tools](#1-install-the-tools) | a compiler and build system | ~10 min |
| [2. Get the code and libraries](#2-get-the-code-and-libraries) | a buildable checkout | ~2 min |
| [3. Build](#3-build) | `castlemist.exe` | ~5 min |
| [4. Open your Gw2.dat](#4-open-your-gw2dat) | browse textures, sounds, strings | instant |
| [5. Generate the struct template](#5-generate-the-struct-template) | 3D models, maps, the structure tree | ~15 min, once |
| [6. Build the index](#6-build-the-index) | type filters, real sizes, fast search | a while, once |
| [7. Game names and name search](#7-game-names-and-name-search) | the Name column, search by item name | ~15 min, once |
| [8. Character Ripper](#8-character-ripper-optional) *(optional)* | your characters as rigged models | ~5 min |
| [9. Extras](#9-extras-optional) *(optional)* | cinematics, encrypted strings, agents | — |

Something not working? [troubleshooting.md](troubleshooting.md) covers every
error this guide has run into.

> **What castlemist reads.** Only files you already have: your own `Gw2.dat`
> and the game executable. Nothing from the game ships with castlemist, and
> nothing it generates should be shared or committed: every data file is
> specific to your game build.

---

## 1. Install the tools

You need Windows 10 or 11 (64-bit) and an installed copy of Guild Wars 2.

### MSYS2 (compiler, CMake, Ninja)

1. Install MSYS2 from <https://www.msys2.org/> (keep the default
   `C:\msys64`).
2. Open **MSYS2 UCRT64** from the Start menu and run:

   ```bash
   pacman -Syu
   ```

   If it closes the window, reopen **MSYS2 UCRT64** and run it again until it
   reports nothing to do.
3. Install the toolchain:

   ```bash
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```

4. Add `C:\msys64\ucrt64\bin` to your Windows `PATH`: press Start, type
   **environment variables**, open **Edit the system environment variables >
   Environment Variables...**, select **Path** under *User variables*, click
   **Edit > New**, paste `C:\msys64\ucrt64\bin`, and click OK on every dialog.

   > This step matters more than it looks. Without it, `g++` fails **silently**:
   > the build says `FAILED` with no error message at all.

5. Open a **new** terminal (PowerShell, or Git Bash) and check:

   ```powershell
   g++ --version
   cmake --version
   ninja --version
   ```

   All three should print a version. GCC 13 or newer and CMake 3.21 or newer.

### Git

Install Git for Windows from <https://git-scm.com/download/win> (the defaults
are fine).

### Optional extras

| tool | needed for | get it |
| ---- | ---------- | ------ |
| Ghidra 12 + JDK 21 + Python 3.13 (not 3.14) | step 5 (the struct template), if you don't have IDA Pro | see [struct-template.md](struct-template.md); after installing Python or Java, open a **new** terminal (no reboot needed) |
| Blender 4.x | step 8's **Export for VRChat** | <https://www.blender.org/download/> (default install location) |

---

## 2. Get the code and libraries

```powershell
git clone https://github.com/spjinx/castlemist.git
cd castlemist
```

castlemist builds against about a dozen open-source libraries. They are not in
git (they are hundreds of megabytes). One script downloads all of them into
`external/` at the exact versions the build expects:

```powershell
powershell -ExecutionPolicy Bypass -File tools\setup\fetch_externals.ps1 -WithBgfx
```

It takes about half a minute and is safe to re-run; anything already present is
skipped. `-WithBgfx` also fetches the renderer Guild Wars 2 itself uses, which
powers the optional **Game 1:1** view. Leave it off if you don't have Git on
`PATH`; everything else still works.

What it fetches, and why each exists, is listed in
[`external/README.md`](../external/README.md).

---

## 3. Build

From the `castlemist` folder:

```powershell
cmake --preset release
cmake --build --preset release
```

That produces a single self-contained program:

```
build\release\bin\castlemist.exe
build\release\bin\gw2dat_cli.exe      (command-line tool, optional)
build\release\bin\gw2index.exe        (command-line indexer, optional)
```

Run `castlemist.exe` from there. It finds the `dumps\` folder and your
settings by looking upwards from its own location, so **keep it inside the
repository folder** rather than copying it elsewhere.

> **Planning to change the code?** Use the `debug` preset instead
> (`cmake --preset debug`, `cmake --build --preset debug`, then
> `ctest --preset debug` to run the tests). It builds one DLL per layer so
> rebuilds are fast. [building.md](building.md) has the details.

---

## 4. Open your Gw2.dat

1. Start `castlemist.exe`.
2. **File > Open .dat...** and pick your `Gw2.dat`. Common locations:

   | install | path |
   | ------- | ---- |
   | Steam | `C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat` |
   | ArenaNet installer | `C:\Program Files\Guild Wars 2\Gw2.dat` |

   Close the game first if you can. castlemist only reads the archive, but the
   game rewrites it while patching.

3. The left list fills with every asset (about 820,000). Click one to preview
   it. Textures, sounds, string tables and videos work right away.

You can also open it straight from a terminal, optionally jumping to an entry
by its base id:

```powershell
build\release\bin\castlemist.exe "C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat" 2871
```

**Models and maps show raw bytes at this point.** That is expected; the next
step fixes it.

---

## 5. Generate the struct template

Guild Wars 2's model, map and material files are packed structs with no field
names. The game executable carries a description of every one of them, and
`gw2_packfile.json` is that description pulled out of your copy of
`Gw2-64.exe`. With it, castlemist can read 3D models, maps, materials and the
structure tree.

This is a one-time job (repeat it only after a game patch that breaks model
loading). There are two ways to do it:

- **Ghidra (free)**, through PyGhidra: about 15 minutes, most of it installing
  Ghidra.
- **IDA Pro**, if you already have it.

Both are walked through step by step in
**[struct-template.md](struct-template.md)**. Either way you end up with:

```
dumps\packfile\gw2_packfile.json
```

castlemist loads it automatically from there the next time it needs it (or use
**File > Load Struct JSON...**). Click a model entry to check: you should see
the mesh instead of hex.

---

## 6. Build the index

The index is a database of every entry in the archive: its real type, the
container it belongs to, its true decompressed size, and which chunks it holds.
It powers the **Type** and **Container** columns, the three filter boxes above
the list, and fast searching.

1. Do step 5 first if you can. The index records chunk versions using the
   struct template and leaves them blank without it.
2. **File > Build Index DB from .dat...**
3. Pick your `Gw2.dat`, then save the database as
   **`dumps\index\gw2_index.db`** inside the castlemist folder (so it loads
   automatically on every start).
4. Progress shows in the status bar. It reads the whole ~90 GB archive, so
   expect it to take a while (much faster on an SSD); you can keep browsing
   meanwhile. If you stop it, running it again picks up where it left off.
5. When it asks **Open it now?**, say yes.

From now on castlemist opens the index, and the `Gw2.dat` it was built from,
by itself at startup.

When it finishes, castlemist also starts step 7 automatically.

> Prefer the command line? `gw2index.exe` builds the same database:
> `build\release\bin\gw2index.exe --dat "<path to>\Gw2.dat" --out dumps\index\gw2_index.db --template dumps\packfile\gw2_packfile.json --threads 16`

---

## 7. Game names and name search

The archive names nothing: every file is a number. castlemist connects those
numbers to the game's own items, skins, maps and so on through the game's
**content map**, and fetches their names from the official GW2 API (public,
no key or account needed).

### Download every name once

If step 6 just finished, this is already running. Otherwise:
**Tools > Download all game names (GW2 API)**.

1. If the content map hasn't been built yet, castlemist builds it first (a
   minute or two; it needs the index from step 6).
2. Then it downloads about 168,000 names, a few hundred at a time, staying
   under the API's rate limit. The status bar counts up; the whole run takes
   about 15 minutes. You can keep browsing while it runs.
3. When it says **Game names ready**, you're done. The names are saved in
   `content_names.tsv` next to `castlemist.exe`, so this never needs to run
   again (unless a game patch adds new items you want named).

If it stops early (Wi-Fi dropped, API down), the status bar says why. Run it
again and it continues from where it stopped.

### What you get

- **Name column** in the file list: every file the game uses for an item, skin,
  map, etc. shows that thing's name. `(+N)` means N more things share the same
  file. Click the **Name** header to sort the whole archive A–Z.
- **Game content** in the right-hand panel: for the selected file, every game
  object that uses it, with its chat link (like `[&CmoZAAA=]`, paste it into
  in-game chat) and the items that unlock it.
- **Name search**: type any name, or part of one, into the search box at the
  top left and press **Search**:

  | you type | you get |
  | -------- | ------- |
  | `astralaria` | the legendary's icon and models |
  | `20 slot` | every 20-slot bag's files |
  | `477426` | base id 477426 (numbers alone still search by id) |
  | `477426` with **By File ID** ticked | the entry for file id 477426 |

  Name search combines with the three filter boxes, so you can search
  `mordrem` with the type filter set to textures. **Clear** shows everything
  again.

Without the download, names still appear; they're fetched for whatever rows
you scroll past, just more slowly.

---

## 8. Character Ripper *(optional)*

Fetches one of your characters through the official API and rebuilds it from
your `Gw2.dat` as a rigged, dyed 3D model.

### Make an API key

1. Go to <https://account.arena.net/applications> and sign in.
2. **New Key**, give it any name, and tick **account**, **characters** and
   **builds**. Create it and copy the key.

### Use it

1. **Tools > Character Ripper... (GW2 API)**.
2. **Manage keys...**: paste the key, give it a name, **Save / Add**. Keys are
   stored in plain text in `api_keys.json` at the repository root (ignored by
   git). Only add keys to a machine you trust.
3. Pick the key, click **Fetch characters**, choose a character and an
   equipment tab. The table shows each slot's item, skin, dyes and whether it
   resolved to a model.
4. Then:

   | button | what it does |
   | ------ | ------------ |
   | **Edit look...** | face, hair, ears, skin pattern, every colour, face-detail sliders and physique, from the game's own character-creator options |
   | **Export pieces...** | each armor piece, weapon and back item as its own `.glb` |
   | **Export character...** | the whole character on one skeleton as a single rigged `.glb` (tick **Combine into one file** for one mesh) |
   | **Export for VRChat...** | a humanoid avatar `.fbx`, built through Blender (installed in its default location) |
   | **Save manifest...** | what was fetched, as JSON, for the command line |

The `.glb` files open in Blender (**File > Import > glTF 2.0**), and in most 3D
tools and viewers.

---

## 9. Extras *(optional)*

| want | do | details |
| ---- | -- | ------- |
| Cinematics to play | extract the video codec from the archive: `build\release\bin\gw2dat_cli.exe extract --dat "<path to>\Gw2.dat" --base-id 136046 --out dumps\binaries\bink2w64.dll` (base id as of the October 2026 game build; the link says how to find it if it moves) | [generating-data.md §4](generating-data.md#4-the-bink-runtime----bink2w64dll) |
| Encrypted string tables readable | capture string keys from a running client (advanced) | [generating-data.md §3](generating-data.md#3-string-keys--textkeyscsv-and-strs_textbasecsv) |
| An AI agent to query the archive | the MCP servers in `mcp/` | [`mcp/README.md`](../mcp/README.md) |
| Scripting / batch work | `gw2dat_cli.exe` (JSON output) | [using-castlemist.md](using-castlemist.md#the-command-line) |

---

## Where everything lives

All of these are generated on your machine, ignored by git, and safe to delete
(castlemist rebuilds or asks for them again).

| file | made by | holds |
| ---- | ------- | ----- |
| `dumps\packfile\gw2_packfile.json` | step 5 | struct template |
| `dumps\index\gw2_index.db` | step 6 | the index |
| `build\<preset>\bin\content_map.bin` | step 7 (first use) | the content map cache |
| `build\<preset>\bin\content_names.tsv` | step 7 | downloaded game names |
| `api_keys.json` | step 8 | your API keys (plain text) |
| `character_looks.json` | step 8 | saved character looks |
| `dumps\binaries\bink2w64.dll` | step 9 | the video codec |

**After a game patch:** re-run step 6 (it only re-scans what changed), then
rebuild the content map (**Tools > Decode Chat Link... > Rebuild map**) and run
**Download all game names** again to pick up new items. Redo step 5 only if
models stop loading.

## Next

- [using-castlemist.md](using-castlemist.md): a tour of every panel, the
  exporters, and the command line.
- [troubleshooting.md](troubleshooting.md): when something goes wrong.
- [architecture.md](architecture.md), [building.md](building.md),
  [testing.md](testing.md): if you want to work on castlemist itself.
