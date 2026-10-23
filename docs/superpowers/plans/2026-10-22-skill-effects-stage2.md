# Skill Effects, Stage 2 (Unity prefab importer) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn a `skillfx-export` folder into Unity prefabs, one per skill, built with the
world importer's own particle code (`ParticleEffects`). Tome of Justice plays in Unity, and a
screenshot shows it.

**Architecture:** Each effect model is written exactly as the world export writes a model:
`models/<fileId>/` holding the `.glb`, `materials.json`, its textures, `<fileId>_particles.json`
and an `.fbx` from Blender. The world's frame and options come with it, so `ParticleEffects`
and `CastlemistMaterials` read it unchanged. A small `SkillFxImporter`, in its own folder
`tools/unity/CastlemistSkillFx/`, builds one prefab per effect model from public
`Castlemist.Editor` pieces, then one prefab per skill. `WorldImporter` is not changed.

**Tech Stack:** C++20 (exportgltf, skillfx, ripper's Blender call); C# for Unity 2022.3 (Built-in
RP); .NET 9 for the Unity-free reader tests; Blender 4.5; kitwright (HTTP JSON-RPC) into the user's
open GW2Porting editor.

**Spec:** docs/superpowers/specs/2026-10-22-skill-effects-design.md, section 2. Particle
semantics: docs/research/gw2-particle-system.md. Stored emitter ranges are `{min, max}` (verified
2026-10-22), which is how `ParticleEffects` already reads them.

## Global Constraints

- **Reuse, don't fork.** `ParticleEffects`, `CastlemistMaterials`, `MaterialsFile`,
  `MaterialContext` and `CastlemistPostprocessor` are used as they are. No edits to
  `tools/unity/Castlemist/`.
- **Same frame as a world model:** `VrchatFolderOptions` as `export_one_model` sets them
  (`rootMatrix = map_to_gltf_root(1.0)`, `uv1CopyToUv4`, `vertexColors`, `exportLods`), but with
  images local to the folder (no shared pools). Unity scale 0.0254 on the skill prefab's root,
  as `WorldImporter`'s default.
- **Unity verification only through kitwright** in the user's GW2Porting editor, under new folders
  `Assets/CastlemistSkillFx/` (scripts) and `Assets/CastlemistSkillFxExport/` (data). The game is
  never touched.
- **Build env** as in stage 1. `dotnet run --project tools/unity/Tests~` for the C# readers.

## Review Focus

- An effect model with no meshes (particles only): its prefab is an empty root with the particle
  systems, and there is no .fbx. Tests in Task 1 (the folder still writes) and Task 3.
- A model that two skills use: one `models/<id>/` folder, and both skill prefabs reference
  the same effect prefab. Test in Task 1.
- An effect whose model failed to export: skill.json keeps `error`, and the importer skips it with
  a warning. Test in Task 2 (reader) and Task 3.
- Blender not found: the export still succeeds without .fbx files, and the importer warns per
  model ("no .fbx"), as WorldImporter does.

---

### Task 1: Effect models in world-model folders (C++)

**Files:**
- Modify: `include/castlemist/skillfx/export.h`, `src/skillfx/export.cpp`: the layout and the
  `ModelExporter` contract
- Modify: `tools/gw2dat_cli/main.cpp`: the real exporter calls `write_vrchat_folder`; `--fbx`
  runs `castlemist::ripper::convert_world_to_fbx(out)`
- Test: `tests/test_skillfx.cpp`: update `export_layout_shared_and_failure`

**Interfaces:**
- `using ModelExporter = std::function<std::string(uint32_t model, const std::filesystem::path& folder)>;`
  The exporter writes `folder/<model>.glb`, `materials.json` and `<model>_particles.json`. It
  returns "" on success, else the error text.
- Layout: every exported model at `models/<id>/` (the `shared/` and per-skill model files go
  away). skill.json effects: `"folder": "models/<id>"` ("" when the model is 0 or failed), and
  `"glb"` keeps the relative `.glb` path.
- Unlinked effects: `unlinked/unlinked.json` with the same `folder` field.

- [ ] **Step 1:** Update the test. Two skills share model 100, model 200 fails, and an unlinked
  effect uses 300. Expect: exporter called once per model with `models/<id>`; skill.json
  `folder == "models/100"`; the failure's `error`; `unlinked.json` folder `models/300`.
- [ ] **Step 2:** Run `build/debug/bin/cm_test_skillfx.exe` → FAIL. **Step 3:** Implement.
  **Step 4:** Run → PASS.
- [ ] **Step 5:** CLI: the exporter loads the model and calls `write_vrchat_folder(*m, folder, id,
  id, vo)` with the world's options. `--fbx` calls `convert_world_to_fbx`. Rerun the real export
  with `--fbx`. Expect `models/1764329/{1764329.glb, materials.json, 1764329_particles.json,
  1764329.fbx}`, and the `.fbx` absent for particle-only 1764457.
- [ ] **Step 6:** Commit `feat(skillfx): effect models in world-model folders, with .fbx`.

### Task 2: Unity-free reader for the skill files

**Files:**
- Create: `tools/unity/CastlemistSkillFx/Editor/CastlemistSkillFx.Editor.asmdef` (Editor only,
  references `Castlemist.Editor`)
- Create: `tools/unity/CastlemistSkillFx/Editor/SkillFxFiles.cs`
- Modify: `tools/unity/Tests~/Castlemist.Tests.csproj`, `tools/unity/Tests~/Tests.cs`

**Interfaces (namespace `Castlemist.SkillFx`, no UnityEngine):**
```csharp
public sealed class SkillSetFile { public string SkillNamespace, EffectNamespace; public List<SkillRef> Skills; }
public sealed class SkillRef { public long Id; public string Label, Folder; }
public sealed class SkillFile { public long Id; public string Label, Codename; public List<EffectRef> Effects; }
public sealed class EffectRef { public long Model; public string Folder, Source, EiName, Codename, Error; public int Count; }
public static class SkillFxFiles {
    public static SkillSetFile ReadSkillSet(string json);   // throws ManifestException on a bad file
    public static SkillFile ReadSkill(string json);
    public static string Slug(string label);                 // as skillfx::slug
}
```
- [ ] **Step 1: Tests:**
  - `ReadSkillSet` and `ReadSkill` on trimmed copies of the real files (44364 with folder
    `models/1764329`);
  - an effect with `error` keeps it and has `Folder == ""`;
  - `Slug("Chapter 1: Searing Spell") == "chapter-1-searing-spell"`;
  - a missing `skills` array throws `ManifestException`.
- [ ] **Step 2:** `dotnet run --project tools/unity/Tests~` → FAIL. **Step 3:** Implement.
  **Step 4:** → PASS (old tests too).
- [ ] **Step 5:** Commit `feat(unity): skill effect file reader`.

### Task 3: SkillFxImporter (Unity editor)

**Files:**
- Create: `tools/unity/CastlemistSkillFx/Editor/SkillFxImporter.cs`, `tools/unity/CastlemistSkillFx/README.md`

**Interfaces:**
- Menu **Castlemist ▸ Import Skill Effects…**.
  `SkillFxImporter.Import(string assetFolder, IList<long> onlySkills = null) : ImportSummary`.
- For each model a chosen skill uses (once each):
  - materials: `MaterialsFile.Parse(models/<id>/materials.json)`, and for each entry with
    `UsedByMeshes`, `CastlemistMaterials.BuildModelMaterial(ctx, me, folder, false, null,
    <root>/Unity/Materials/<id>/<FileName>.mat)`. Then the .fbx gets
    `CastlemistPostprocessor.ApplyModelSettings`, the remap by the WorldImporter name rule (mirrored
    as `FbxMaterialNames`), animation off unless it has clips, and is re-imported.
  - prefab: an instance of the .fbx, or an empty `GameObject` named `<id>` when there is none,
    then `ParticleEffects.AddTo(inst, sidecar, mf, folder, ctx, <root>/Unity/Materials/Particles,
    warnings)`, saved as `<root>/Unity/Effects/<id>.prefab`.
- Per skill: `<root>/Unity/Skills/<id>-<slug>.prefab`, a root with `localScale = 0.0254`
  holding one instance of each effect prefab.
- `MaterialContext.Create(warnings, ShaderMode.Castlemist)`, the Castlemist shaders.

- [ ] **Step 1:** Write the importer and the README (usage, layout, what is UNPROVEN).
- [ ] **Step 2:** Compile check in GW2Porting: copy `tools/unity/CastlemistSkillFx` to
  `Assets/CastlemistSkillFx`, refresh through kitwright, read the console. Expected: no errors.
- [ ] **Step 3:** Commit `feat(unity): skill effect prefab importer`.

### Task 4: Tome of Justice in Unity, screenshot

- [ ] **Step 1:** Copy the export, limited to `skill_set.json`, the six Tome of Justice skills
  (44364, 41258, 40635, 42449, 40015, 42898) and the `models/` folders they use, to
  `Assets/CastlemistSkillFxExport/firebrand/`.
- [ ] **Step 2:** kitwright `execute_code`: `SkillFxImporter.Import(".../firebrand", {44364})`.
  Expected: 0 errors, particle systems > 0.
- [ ] **Step 3:** A preview scene: the `44364-tome-of-justice` prefab at the origin, a camera
  about 1.5 m back and 0.6 m up, `Simulate(2)` on every ParticleSystem, then `capture_scene_view`.
- [ ] **Step 4:** Compare with castlemist's preview (`scratchpad/fx_tome/shot_fx_a.png`):
  - the book is open and upright;
  - the particles rise above it;
  - the scale is plausible.
  Ledger any difference as a ruling. Commit the README note.

## Next

Stage 3: the VRCFury avatar menu and a minimal avatar project.
