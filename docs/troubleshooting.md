# Troubleshooting

Problems people actually hit, roughly in the order you'd hit them. Each one
says what you see, why, and the fix.

## Setting up and building

**`fetch_externals.ps1` won't run: "running scripts is disabled on this system".**
PowerShell's execution policy. Run it exactly as written, with
`powershell -ExecutionPolicy Bypass -File tools\setup\fetch_externals.ps1`,
which allows just that one run.

**`fetch_externals.ps1` fails on bgfx with "Filename too long".**
Windows' 260-character path limit. The script turns on Git's long-path support
for these clones; if it still fails, the castlemist folder itself is buried too
deep. Move it somewhere shorter (e.g. `C:\dev\castlemist`) and re-run. Or leave
out `-WithBgfx`: only the optional **Game 1:1** view needs it.

**`cmake --preset ...` says "external/xxx is missing".**
A library isn't in `external/`. Run `fetch_externals.ps1` again; it fills in
whatever is missing.

**The build says `FAILED` with no error message.**
`g++` can't find its own helper DLLs because `C:\msys64\ucrt64\bin` isn't on
`PATH`. Add it (getting-started §1, step 4), **open a new terminal**, and build
again.

**`cmake` says "No CMAKE_CXX_COMPILER could be found" or "Ninja not found".**
Same cause, or the MSYS2 packages aren't installed. Check `g++ --version` and
`ninja --version` work in a new terminal.

**castlemist starts, then closes after two seconds with no window.**
Exit code `0xC0000139`: Windows loaded a mismatched copy of the MinGW runtime
DLLs from somewhere else on `PATH`. Use the `release` preset (it links the
runtime in), or see [building.md](building.md#the-startup-failure-you-will-otherwise-hit).

## Opening the archive

**The archive won't open.**
If the game or its launcher is running or patching, it may have the file
locked. Close Guild Wars 2 and try again.

**Models and maps show hex / "No struct template is loaded".**
You haven't done getting-started §5, or the file isn't where castlemist looks.
It must be `dumps\packfile\gw2_packfile.json` inside the castlemist folder, and
castlemist must run from inside that folder (e.g. `build\release\bin\`), because
it searches upwards from its own location. Or load it by hand with **File > Load
Struct JSON...**.

**After a game patch, models that used to work come up empty.**
A chunk changed version. Regenerate the struct template
([struct-template.md](struct-template.md)).

**The Type / Container columns are empty and the filter boxes are disabled.**
No index is loaded. Build one (getting-started §6) and save it as
`dumps\index\gw2_index.db` so it loads by itself; or **File > Open Index DB...**.

## Names and search

**The Name column is empty everywhere.**
The content map hasn't been built yet. Run **Tools > Download all game names**
(it builds the map first), or **Tools > Decode Chat Link... > Resolve assets**.
Both need the index loaded.

**Names show `…` and fill in slowly as I scroll.**
They're being fetched on demand. Run **Tools > Download all game names** once
and they all come from the cache from then on.

**"Game names stopped early (...)".**
The reason is in the brackets: usually you're offline or the GW2 API is down or
slow. Run it again later; it resumes where it stopped. It's paced to stay under
the API's rate limit, so you won't get banned for running it.

**A name search finds nothing.**
Names are searched only once they're downloaded; the status bar says *no names
yet* when none are. Also check the three filter boxes: the search only shows
results that match them too. **Clear** resets everything.

**Typing in the search box searches by id instead of name.**
Text that is only digits (and spaces) is treated as an id. Add a word.

**New items from a patch have no name.**
Rebuild the content map (**Tools > Decode Chat Link... > Rebuild map**), then
run **Download all game names** again; it only fetches what's new.

## Character Ripper

**"API key rejected".**
The key is mistyped, deleted, or missing a permission. It needs **account**,
**characters** and **builds** (account.arena.net > Applications).

**Some slots say `no_skin` / aren't exported.**
Rings, accessories and amulets have no 3D model in the game, so there's
nothing to export. Everything else should resolve; if armor doesn't, rebuild
the content map (after a patch the cached one can be stale).

**Export for VRChat says Blender wasn't found.**
Install Blender in its default location
(`C:\Program Files\Blender Foundation\...`); castlemist looks there.

## Ghidra (struct template)

See the table at the end of [struct-template.md](struct-template.md#if-it-goes-wrong).

## Still stuck

Open an issue at <https://github.com/spjinx/castlemist/issues> with what you did,
what you saw (a screenshot of the status bar helps), and your Windows and GCC
versions (`g++ --version`).
