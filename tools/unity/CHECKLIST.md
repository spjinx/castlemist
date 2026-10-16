# Unity world importer: checklist

The checks for `tools/unity/Castlemist/Editor/` (spec
`docs/superpowers/specs/2026-10-15-world-export-design.md`, D6 and Testing,
"Manual checklists, Unity"). CI cannot compile or run the C#. Items 1 to 3 can
be run in batchmode. The rest need the editor, in a VRChat Unity 2022.3 project
with Poiyomi Toon 10, importing the Spirit Vale export (1151420; 66 cells at the default budget of 500 000 triangles a cell).

## Setup

Lighting comes out dark on some maps, by the spec's own choices (review M9):
the ambient is `fill x min(fillIntensity, 1)`, and Spirit Vale's
`fillIntensity` is 0, so Unity's Flat ambient is black; Blender's sun energy is
the raw intensity (1.3 on Spirit Vale). Both are labelled castlemist's choice.
Raise the ambient (Lighting window) or the sun's intensity to taste; nothing in
the manifest changes.

1. Export the map with `gw2dat_cli world-export --out-dir <dir>`. The export
   needs its `.fbx` files: the CLI writes them when Blender is found. If it did
   not, run `gw2dat_cli world-fbx --dir <dir>` or
   `blender -b --factory-startup -P tools/blender/castlemist_world.py -- --to-fbx <dir>`.
   Either one records the `.fbx` paths in `manifest.json`, which is where the
   importer looks for them.

   Path length: castlemist writes the folder through extended-length paths, so
   a deep output folder loses nothing (relative paths inside it reach about 125
   characters). Blender and Unity are not castlemist: unless Windows long paths
   are enabled (the `LongPathsEnabled` registry switch), keep the export's
   absolute path, and the project's `Assets/<export>` path, under about 130
   characters, or Blender's `--to-fbx` and Unity's import can fail on the
   deepest files.
2. Copy `tools/unity/Castlemist` into the project's `Assets/`. Then copy the
   export folder into `Assets/`. Do it in that order, so the postprocessor
   sets the texture roles and `bakeAxisConversion` on the first import. The
   importer re-imports any `.fbx` it finds with the wrong axis setting, and
   warns when it does.
3. **Castlemist ▸ Import World…**: pick the export folder. Keep or change
   **Unity scale**, which defaults to the manifest's 0.0254. That default is
   UNPROVEN (the "GW2 inches" reading, spec D7). Click **Import**. The Console
   then shows the warnings, the `PROBE` line and the `PLAN` line.
4. Batchmode, the same import:

   ```
   Unity.exe -batchmode -nographics -quit -projectPath <project>
     -executeMethod Castlemist.BatchEntry.ImportWorld -castlemistWorld Assets/<export>
     [-castlemistScale <s>] -logFile <log>
   ```

   The exit code is 0 when there are no errors, the probe passed, and the
   `CELL` and `INSTANCES` checks passed. Two other entry points exist:
   - `BatchEntry.CheckProbe -castlemistFile Assets/probe.fbx [-castlemistNoBakeAxis]`
     is the Task 1 axis probe.
   - `BatchEntry.CheckSkinnedProbe -castlemistFile Assets/<dir>/skinned_probe.fbx`
     checks the skinned probe. It reads the output of
     `cm_world_export_fixture <dir> --skinned-probe`, followed by `--to-fbx`.
5. Unity-free reader tests, which run locally and not in CI:
   `dotnet run --project tools/unity/Tests~ -- <export dir>`.

## Items

1. **The summary equals the dry run.** The importer's `PLAN …` line must equal
   `python tools/world/validate_manifest.py <export> --plan` character for
   character. For the fixture (`cm_world_export_fixture`), that line is
   `PLAN materials=5 prefabs=3 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0 foliageTrees=2`
   (the line had no `foliageTrees` field, and 4 materials and 2 prefabs, before
   the world-foliage work). In batchmode, `FOLIAGE_TREES checked=<n> ... OK`
   also checks every foliage tree instance against its record's `m`.
2. **The compile is clean.** No `error CS` and no `warning CS` from
   `Assets/Castlemist`.
3. **The frame probe passes.** It prints
   `PROBE east=(1,0,0) north=(0,0,1) up=(0,1,0) negScale=0 OK`. With
   `CheckSkinnedProbe`, the skinned probe also passes: un-mirrored, with the
   clip moving East +X, turning North to −X, and moving Up +Y.
4. **Terrain lines up with the props**, as in the Map window shot of the same
   place: the paths, the props on them, and the cliffs.
5. **Animated props play** in Play mode, looping, all starting at t = 0
   (clipPhase is UNPROVEN).
6. **The colliders work.** Walk in Play mode with a CharacterController. You
   should stand on the terrain and on the havk floors, such as the raid
   platforms. Things to note:
   - Unity's or VRChat's triangle limit on a non-convex static MeshCollider,
     if one is hit;
   - which layers VRChat's player collides with. The colliders are on Default.
7. **The water sits at the shore.** Spirit Vale has no water plane, so check
   this on a map that has one.
8. **The sky and the sun look right.** `sun.unityForward` is the direction
   the light travels, so it points down (y < 0) on a lit map; the importer
   warns if a manifest's points up.
9. **Poiyomi materials.** Opaque, cutout and blended props look right. Packed
   maps give metal and shine. Emission glows. Dark decals are Fade. Nothing
   is pink.
10. **Draw calls and import time.**
    - Read the draw calls off the Frame Debugger at the spawn point.
    - Static batching and GPU instancing are both set. Which one wins is
      UNPROVEN, so record what the Frame Debugger shows.
    - Record the import time from the `CASTLEMIST import … in N s` line.
11. **VRChat.** Add the scene descriptor by hand at `Spawn`; the importer does
    not add it. Then build and test once. Upload size is UNPROVEN and not
    enforced.

## Results

| date | editor | export | item | result |
|---|---|---|---|---|
| 2026-10-15 | 2022.3.22f1 batchmode, no Poiyomi (Standard fallback) | fixture | 1 | PASS: `PLAN materials=4 prefabs=2 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0`, equal to the dry run |
| 2026-10-15 | 2022.3.22f1 batchmode | fixture | 2 | PASS: no `error CS` or `warning CS` from Assets/Castlemist |
| 2026-10-15 | 2022.3.22f1 batchmode | fixture | 3 | PASS: `PROBE east=(1.000,0.000,0.000) north=(0.000,0.000,1.000) up=(0.000,1.000,0.000) negScale=0 OK`; skinned probe OK (rest, start, end) |
| 2026-10-15 | 2022.3.22f1 batchmode, Poiyomi Pro 9.0.α18 copied in (not 10) | fixture | 1-3, 9 (properties only) | PASS: shader found; preset, MOCHIE_PBR and multipliers set; the same PLAN line; probe OK |
| 2026-10-15 | 2022.3.22f1 batchmode, no Poiyomi | Spirit Vale, 36-cell export (before the 500 000 budget; the 66-cell export has not been imported) | 1-3 | PASS: `PLAN materials=1027 prefabs=380 controllers=37 cells=36 instances=2354 colliders=25 terrainColliders=36 water=0 lights=1 skybox=1`, equal to the dry run; probe OK; 36 cells centred; 2354 instance matrices within 2.7e-5 of `m`. First import 75 s in the importer (234 s wall, with Unity's import of 7 300 files); re-import 29 s |
| 2026-10-15 | 2022.3.22f1 batchmode, Poiyomi Pro 9.0.α18 | Spirit Vale, 36-cell export | 1-3 | PASS: the same PLAN line; probe OK; importer 59 s |
| 2026-10-15 | 6000.0.42f1 batchmode | fixture | 1-3 | PASS: the same PLAN line; probe OK; skinned probe OK |
| 2026-10-15 | dry run | Spirit Vale, 66-cell export (budget 500 000) | 1 | `python tools/world/validate_manifest.py <dir> --plan` prints `PLAN materials=1057 prefabs=380 controllers=37 cells=66 instances=2354 colliders=52 terrainColliders=66 water=0 lights=1 skybox=1`; the Unity importer was not run on it |
| 2026-10-16 | 2022.3.22f1 batchmode, no Poiyomi (Standard fallback) | fixture (with model 103 and foliage) | 1-3 | PASS: `PLAN materials=5 prefabs=3 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0 foliageTrees=2`, equal to the dry run; no `error CS` / `warning CS`; probe OK; `FOLIAGE_TREES checked=2 records=2 maxRelErr=1.19E-007 OK`; the 2 type-1 records counted as deferred in a warning |
| 2026-10-16 | 2022.3.22f1 batchmode, no Poiyomi | Spirit Vale, 66-cell export with foliage (world-foliage Task 8) | 1-3 | PASS: `PLAN materials=1061 prefabs=381 controllers=37 cells=66 instances=2354 colliders=52 terrainColliders=66 water=0 lights=1 skybox=1 foliageTrees=284`, equal to the dry run; probe OK; 66 cells centred; 2354 instances within 3.95e-6 and 284 foliage trees within 9.42e-7 of `m`. Importer 91 s (280 s wall, first import) |
| — | VRChat 2022.3 + Poiyomi 10, editor | Spirit Vale | 4-11 | not tried (no editor session; items 4 to 11 need one). Poiyomi 10 is not on this machine; only Pro 9.0.α18 was tried |
