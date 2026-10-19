# NPC Catalogue — Stage 2 (Export) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export one catalogue NPC to a folder that Blender/Unity can import: `body.glb` (skeleton, skin, textures, all clips), one `weapon_<slot>.glb` per weapon, and `manifest.json`.

**Architecture:** Two parts in the `npc` layer.
- A pure part (folder name, manifest JSON) that is unit-tested without a dat.
- `export_npc`, which loads models with the existing `load_model_by_fileid` and writes them with the existing `export_model_gltf` using default options, the same as the app's model export.

Then a CLI command, `npc-export`.

**Tech Stack:** C++20, nlohmann::json, castlemist exportgltf/extract, castlemist test framework.

**Spec:** `docs/superpowers/specs/2026-10-14-npc-catalogue-design.md` (section "Export").

## Global Constraints

- Do not modify content_map, content_schema, content_store, type_names, exportgltf or extract. Add, never rename.
- Never read Gw2.dat directly; real-data checks go through `gw2dat_cli`.
- Build and test only through `scratchpad/cmbuild.sh`.
- Folder: `<out>/<id>_<codename>/`, with every character outside `[A-Za-z0-9._-]` in the codename replaced by `_`. An empty codename gives `<id>`.
- Files: `body.glb` only when `model != 0`; `weapon_<slot>.glb` (slot = the field offset: 192/200/208) only when the weapon's `model != 0`; `manifest.json` always.
- Manifest: `{"version":1,"id","codename","model","sound_script","race","modifier","clips":[{"name","group","prefix","duration"}],"weapons":[{"slot","item","skin","model","file"}],"skills":[...],"files":[...],"notes":[...]}`. `file` is "" when not written.
- Notes (exact text):
  - `"no body model: race-body NPC (<race>), outfit data not decoded"` when `model == 0` and race is non-empty;
  - `"no body model"` when `model == 0` and race is empty;
  - `"weapon <slot>: no skin model"` when a weapon's `model == 0`;
  - `"<file>: <error>"` when a load or export fails.
- Weapons are not attached to bones (spec).

## Review Focus

- A codename with `/` or `+` (real codenames are base64-ish): the folder name must be filesystem-safe. Test `folder_name_is_filesystem_safe`.
- A race-body NPC (model 0): only `manifest.json` is written, with the race note. Test `manifest_notes_a_race_body_npc`.
- A model fileId that fails to load: no throw, a note, and the other files still written. Covered in Task 2's real-data step with a bogus record (model 1).
- An out dir that doesn't exist yet: created. Task 2 real-data step.
- Re-exporting into an existing folder: overwrite files, no error. Task 2 real-data step (run twice).

---

### Task 1: Folder name and manifest (pure)

**Files:** Create `include/castlemist/npc/export.h`, `src/npc/export.cpp`; extend `tests/test_npc.cpp`.

**Interfaces — Produces (namespace `castlemist::npc`):**
```cpp
struct ClipInfo { std::string name; double duration = 0; };
struct WrittenWeapon { Weapon weapon; std::string file; };     // file "" when not written
std::string export_folder_name(const NpcRecord&);
nlohmann::json export_manifest(const NpcRecord&, const std::vector<ClipInfo>& clips,
                               const std::vector<WrittenWeapon>& weapons,
                               const std::vector<std::string>& files, const std::vector<std::string>& notes);
std::vector<std::string> base_notes(const NpcRecord&);         // the "no body model..." and "weapon <slot>: no skin model" notes
```
`clips[].group/prefix` come from `clip_group`/`clip_prefix`.

- [ ] **Step 1: Failing tests.**
  - `folder_name_is_filesystem_safe`: id 5758, codename `"vl8A/.4g+nM"` gives `"5758_vl8A_.4g_nM"`. An empty codename gives `"5758"`.
  - `manifest_notes_a_race_body_npc`: record model 0, race `"humanfemale"`, weapon {200, 76158, 0, 0}. `base_notes` is exactly `{"no body model: race-body NPC (humanfemale), outfit data not decoded", "weapon 200: no skin model"}`.
  - `manifest_lists_clips_weapons_and_files`: for clip `wgalunge08` (1.07 s), the manifest has `clips[0].group == "lunge"`, `prefix == "wga"`, `version == 1`, and weapons[0].file is the given file name.
- [ ] **Step 2–4:** RED (compile error), implement, GREEN.
- [ ] **Step 5:** Commit `feat(npc): export folder naming and manifest`.

### Task 2: `export_npc` and `gw2dat_cli npc-export`

**Files:** Modify `include/castlemist/npc/export.h`, `src/npc/export.cpp`, `tools/gw2dat_cli/main.cpp`.

**Interfaces — Consumes:** Task 1 and `ensure_npc` (CLI). **Produces:**
```cpp
struct ExportResult { bool ok = false; std::filesystem::path folder; std::vector<std::string> files, notes; };
ExportResult export_npc(Gw2Dat& dat, const NpcRecord&, const std::filesystem::path& out_dir);
```
- Create `out_dir/export_folder_name(r)` (create_directories).
- Body: `load_model_by_fileid(dat, r.model)`. When it's null, add the note `"body.glb: model failed to load"`. Otherwise call `exportgltf::export_model_gltf(*m, path)` with default options. On `!ok`, add the note `"body.glb: <error>"`. Clips for the manifest come from `m->animClips`.
- Weapons: the same, for each weapon with `model != 0`, writing `weapon_<slot>.glb`.
- Write `manifest.json` (pretty-printed, 2 spaces) last. `ok` is true when the manifest was written.
- Never throw. Catch `std::exception` around each file and record it as a note.
- CLI: `npc-export --id N --out-dir D` (both required; `--dat` required) prints `{"ok","folder","files","notes"}`.

- [ ] **Step 1:** Implement (glue over tested pieces; the pure parts are tested in Task 1).
- [ ] **Step 2:** `cmbuild.sh build gw2dat_cli cm_test_npc`.
- [ ] **Step 3: Real data** (out dir under the scratchpad).
  - `npc-export --id 5758 --out-dir <scratch>/npcx` prints ok, with files `body.glb`, `manifest.json` and any weapons.
  - Check body.glb: bytes 0..4 are `glTF`, the version u32 is 2, and the first chunk's type is `JSON`. Parse that JSON and expect `animations` length equal to the manifest's clip count, and `skins` length ≥ 1. Do this with a short Python one-off in the scratchpad that reads only the exported .glb, never the dat.
  - Run the same export again: still ok (overwrite).
  - Export a race-body NPC: pick one from `npc-list --race humanfemale --limit 1`; only `manifest.json` is written, with the race note.
- [ ] **Step 4:** Full suite passes.
- [ ] **Step 5:** Commit `feat(npc): export an NPC to body/weapon glb and manifest; cli npc-export`.
