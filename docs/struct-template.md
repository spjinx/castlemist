# Generating the struct template (`gw2_packfile.json`)

castlemist needs this file to read 3D models, maps, materials and the structure
tree. This page walks through making it from your own copy of the game, with
free tools (Ghidra + PyGhidra) or with IDA Pro.

**Why it's needed.** GW2's model and map files are packed C structs with no
field names on disk. The game executable carries a reflection table that
describes every one of them, so the game can read its own files. Both scripts
below find that table inside `Gw2-64.exe` and write it out as JSON. They only
read bytes from the executable; they don't need disassembly or decompilation,
and they never touch the running game.

**When to redo it.** Once, then only after a game patch that makes models stop
loading (they come up empty or as hex).

The result always goes here, in the castlemist folder:

```
dumps\packfile\gw2_packfile.json
```

---

## Option A: Ghidra + PyGhidra (free)

IDA Free can't run Python scripts, so this is the route if you don't own IDA
Pro. `tools/structs/gen_gw2_json_ghidra.py` is a line-for-line port of the IDA
script below.

### 1. Install Java 21

Ghidra 12 requires **JDK 21** (a newer or older Java will not do).

1. Download the **JDK 21** Windows x64 `.msi` from Adoptium:
   <https://adoptium.net/temurin/releases/?version=21&os=windows>
2. In the installer, enable **Set JAVA_HOME variable** (it's off by default,
   under the feature list).
3. Open a new terminal and check: `java -version` should say `21`.

If you already have another Java installed, Ghidra asks for the JDK 21 folder
on first launch. Point it at the Adoptium install (for example
`C:\Program Files\Eclipse Adoptium\jdk-21...`).

### 2. Install Python

PyGhidra needs **Python 3.9–3.14**. Install it from <https://www.python.org/downloads/windows/>
and tick **Add python.exe to PATH** on the first installer screen. Check with
`python --version`.

### 3. Install Ghidra

1. Download the latest **Ghidra 12** release zip from
   <https://github.com/NationalSecurityAgency/ghidra/releases>.
2. Extract it somewhere with a short path, e.g. `C:\ghidra`. There's no
   installer; the folder *is* the program.

### 4. Start Ghidra in PyGhidra mode

Don't use the usual `ghidraRun.bat`. Start Ghidra with:

```
C:\ghidra\ghidra_12.x_PUBLIC\support\pyghidraRun.bat
```

The first time, it sets up PyGhidra's Python environment from the copy bundled
with Ghidra, so it takes a minute. Answer **y** if it asks to install. After
that it opens the normal Ghidra window.

> Started Ghidra the normal way? The script will refuse to run with
> *"Ghidra was not started with PyGhidra"*. Close Ghidra and use
> `pyghidraRun.bat`.

### 5. Import the game executable

1. **File > New Project... > Non-Shared Project**, name it e.g. `gw2`.
2. **File > Import File...** and pick `Gw2-64.exe` from your game folder
   (e.g. `C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2-64.exe`).
   Accept the defaults (format *Portable Executable*). Importing takes a
   minute or two.
3. Double-click `Gw2-64.exe` in the project to open it in the **CodeBrowser**.
4. When it asks **Analyze now?**, click **No**. The script reads raw bytes,
   so it needs no analysis, which saves a long wait.

### 6. Run the script

1. **Window > Script Manager**.
2. Click the **Manage Script Directories** button (the icon with a list of
   folders, in the Script Manager's toolbar), click **+**, add your
   castlemist folder's `tools\structs`, and close that dialog.
3. In the Script Manager's filter box type `gen_gw2`, select
   **gen_gw2_json_ghidra.py**, and click **Run** (the green arrow).
4. Wait for the console (bottom of the CodeBrowser) to print something like:

   ```
   OK fileTypes=31 chunks=69 types=6582 ambiguous=['ANIM', 'GAME', ...]
   Wrote C:\...\castlemist\dumps\packfile\gw2_packfile.json
   ```

   (Those are the counts for the October 2026 game build; other builds differ
   a little.)

The script writes straight into your castlemist folder's `dumps\packfile\`.
If you copied the script somewhere else, it writes to your user folder instead
and prints that path; move the file into `dumps\packfile\`.

You can close Ghidra now. Keep the project if you like: re-running after a
patch means importing the new `Gw2-64.exe` and repeating step 6.

### Or: one command, no GUI

Once steps 1–4 are done (Java, Python, Ghidra, and PyGhidra installed by the
first `pyghidraRun.bat` launch), steps 5 and 6 can run as a single command. It
imports the executable into a throwaway project, runs the script, and deletes
the project again. It takes about 2 minutes. From the castlemist folder:

```powershell
C:\ghidra\ghidra_12.x_PUBLIC\support\pyghidraRun.bat -H "$env:TEMP\gw2ghidra" gw2 -import "C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2-64.exe" -noanalysis -scriptPath "$PWD\tools\structs" -postScript gen_gw2_json_ghidra.py -deleteProject
```

The folder after `-H` must exist (`mkdir $env:TEMP\gw2ghidra` first). Look for
the `OK fileTypes=...` and `Wrote ...` lines near the end of the output.

---

## Option B: IDA Pro

Needs IDA Pro or IDA Home (with IDAPython) and the x64 `Gw2-64.exe` loaded in
an IDB.

1. Open `Gw2-64.exe` in IDA (the initial auto-analysis can be skipped or left
   to run; the script only reads bytes).
2. **File > Script file...** and choose `tools\structs\gen_gw2_json.py`.
3. It writes `gw2_packfile.json` (and a `.done` summary) into IDA's current
   working folder, usually next to the IDB.
4. Move `gw2_packfile.json` into `dumps\packfile\` in your castlemist folder.

`tools\structs\dump_gw2_structs.py` writes the same reflection data as readable
text, handy for checking a field layout by hand.

---

## Check the result

From the castlemist folder:

```powershell
python -c "import json; j=json.load(open('dumps/packfile/gw2_packfile.json')); print(j['format'], 'chunks', len(j['chunks']), 'types', len(j['types']), 'MODL' in j['chunks'])"
```

A good file prints `gw2packfile`, roughly **70 chunks** and **6,500+ types**,
and `True`, and is about 2 MB. Then in castlemist, click any model entry: you
should get a 3D mesh instead of hex. (castlemist loads the file by itself; use
**File > Load Struct JSON...** if it was already running.)

## If it goes wrong

| symptom | likely cause | fix |
| ------- | ------------ | --- |
| *"Ghidra was not started with PyGhidra"* | launched with `ghidraRun.bat` | start with `support\pyghidraRun.bat` |
| Ghidra won't start, or asks for a JDK path | Java isn't 21 | install JDK 21 (step 1) and point Ghidra at it |
| `gen_gw2_json_ghidra.py` isn't in the Script Manager | its folder isn't a script directory | step 6.2 |
| The script prints `chunks=0` / `types=0` | wrong file imported (e.g. the 32-bit client or a launcher), or a patch reshaped the tables | import the 64-bit `Gw2-64.exe` from the game folder; if it still finds nothing after a patch, compare with an older JSON and open an issue |
| Models still show hex in castlemist | the JSON isn't where castlemist looks | it must be `dumps\packfile\gw2_packfile.json` inside the castlemist folder, and castlemist must run from inside that folder (e.g. `build\release\bin\`) |

The JSON's layout (field kinds, how pointers are encoded) is documented in
[generating-data.md](generating-data.md#1-the-struct-template--gw2_packfilejson).
