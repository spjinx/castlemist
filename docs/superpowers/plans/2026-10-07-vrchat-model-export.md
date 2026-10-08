# Export for VRChat (Model) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A "Export for VRChat (Model)…" export that writes a folder (.glb, .fbx with every clip, Poiyomi-ready PNG maps, `materials.json`) for any previewed model, with each material's maps and blend mode worked out per GW2 shader profile.

**Architecture:** Three pure units in `castlemist::exportgltf` — blend decode, shader profiles, map building — feed a folder writer (`write_vrchat_folder`) that also calls the existing `export_model_gltf`. `castlemist::ripper::export_vrchat_model` adds the Blender step (reusing `find_blender`/`run_blender`), and the CLI and UI call that. The Blender script is fixed to keep animations.

**Tech Stack:** C++20 (MSYS2 ucrt64 g++, CMake + Ninja), nlohmann::json, stb_image_write, castlemist's own test framework (`CM_TEST`/`CHECK*`), Blender 4.5 headless + Python for the FBX step.

**Spec:** `docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md`. Texture conventions: `docs/research/gw2-material-channels.md` (its section 4 is the profile table; where spec and note differ, the note wins). Poiyomi facts: `docs/research/poiyomi-10-importer.md`.

## Global Constraints

- Build from PowerShell with `$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH` first; from Git Bash the compiler fails silently.
- Unit tests: `cmake --build build/debug --target cm_test_exportgltf` then run `build/debug/bin/cm_test_exportgltf.exe` with `build/debug/bin` on PATH. Full suite: `ctest --test-dir build/debug`.
- Never open, read or grep `E:\Games\gw2\Guild Wars 2\Gw2.dat` directly; only through castlemist tools (`build\release\bin\gw2dat_cli.exe`). Template: `dumps\packfile\gw2_packfile.json`.
- Target shader: Poiyomi Toon 10 (10.0.24). Poiyomi textures read UV0–UV3 only.
- Diffuse alpha (where a profile reads holes/shine): 0–63 hole (only when the profile clips), 64–127 opaque matte, 128–255 shine = `saturate(2a − 1)`. glTF `alphaCutoff` 0.25.
- Packed map layout (Poiyomi): R metal, G smooth, B reflection mask, A specular mask.
- Values castlemist can't read are `null` in `materials.json`, never guessed; anything approximate adds a string to that material's `warnings`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Commit only once the user has agreed to commits on this branch.

## Review Focus

1. **Materials with no game shader** (map-path models, loose files; `hasRenderState == false`): must still export, Opaque or Additive per `isEffect`, default profile, warnings — not crash or emit a blend table entry of zeros. Test in Task 1 (`decode_blend_without_render_state`) and Task 4 (`folder_for_material_without_game_shader`).
2. **A layer that failed to decode** (`texIndex` −1 or empty pixels): that map is skipped with a warning; the rest of the material and folder still export. Test in Task 3 (`missing_layer_is_skipped_with_warning`).
3. **Two materials with the same name, or names with `/ \ : * ? " < > |` or non-ASCII**: PNG names must be unique and legal; `materials.json` must point at the files actually written. Test in Task 4 (`material_file_names_are_unique_and_legal`).
4. **Blender missing or failing**: `.glb`, PNGs and `materials.json` still written; the report says why there is no `.fbx`; on failure `<Name> blender.log` exists. Test in Task 6 (`no_blender_still_writes_folder`).
5. **Model with no skeleton** (props) through the Blender script: exports a plain-mesh `.fbx` instead of exiting. Verified in Task 5 (prop step).

---

### Task 1: Blend decode

**Files:**
- Create: `include/castlemist/exportgltf/blend_mode.h`, `src/exportgltf/blend_mode.cpp`
- Create: `tests/test_exportgltf_vrchat.cpp` (new test file for Tasks 1–4; give it its own small fixture helpers — the ones in `test_exportgltf_writer.cpp` are file-local)
- Modify: `tests/CMakeLists.txt` (add `test_exportgltf_vrchat.cpp` to `castlemist_add_test(exportgltf SOURCES ...)`)

**Interfaces:**
- Produces:
  ```cpp
  namespace castlemist::exportgltf {
  enum class BlendPreset { Opaque, Cutout, Fade, TransClipping, Transparent,
                           Additive, SoftAdditive, Multiplicative, Multiplicative2x, Custom };
  struct BlendInfo {
      BlendPreset preset = BlendPreset::Opaque;
      BlendPreset nearest = BlendPreset::Opaque; // == preset unless preset is Custom
      bool exact = true;
      int srcRgb = 0, dstRgb = 0, srcA = 0, dstA = 0, eqRgb = 0, eqA = 0;
      bool alphaTest = false;
  };
  BlendInfo decode_blend(uint64_t bgfxState, bool hasRenderState, bool isEffect, bool alphaTest);
  const char* poiyomi_preset_name(BlendPreset);   // "Opaque","Cutout","Fade","TransClipping","Transparent","Additive","Soft Additive","Multiplicative","2x Multiplicative","Custom"
  int poiyomi_mode_value(BlendPreset);            // _Mode: Opaque 0, Cutout 1, Fade 2, Transparent 3, Additive 4, Soft Additive 5, Multiplicative 6, 2x Multiplicative 7, TransClipping 9; Custom -> value of `nearest`
  enum class GltfAlphaMode { Opaque, Mask, Blend };
  GltfAlphaMode gltf_alpha_mode(const BlendInfo&);
  }
  ```
  Bit layout: srcRgb = `(state >> 12) & 0xF`, dstRgb `>> 16`, srcA `>> 20`, dstA `>> 24`; eqRgb = `(state >> 28) & 0x7`, eqA = `(state >> 31) & 0x7` (0 Add, 1 Sub, 2 RevSub, 3 Min, 4 Max).

- [ ] **Step 1: Write the failing tests** in `tests/test_exportgltf_vrchat.cpp`, one `CM_TEST(vrchat, ...)` per row of the spec's blend table, each building the state word from the survey's real values:
  - `decode_opaque` — `0x0`, alphaTest false → Opaque, exact; alphaTest true → Cutout.
  - `decode_fade` — `0x6565000` → Fade; with alphaTest → TransClipping.
  - `decode_one_invsrccolor` — `0x4242000` → SoftAdditive, `exact == false`, srcRgb 2, dstRgb 4.
  - `decode_premultiplied` — `0x6262000` → Transparent, exact.
  - `decode_additive` — `0x2525000` and `0x2222000` → Additive, exact.
  - `decode_multiply` — `0x1919000` → Multiplicative; `0x3939000` → Multiplicative2x.
  - `decode_soft_additive_exact` — src 10 (InvDstColor) / dst 2 → SoftAdditive, exact.
  - `decode_revsub` — `0x2222000 | (uint64_t(0x12) << 28)` → Custom, nearest Multiplicative, eqRgb 2, `exact == false`.
  - `decode_unknown_pair` — src 7 / dst 2 → Custom, nearest Additive; src 7 / dst 6 → nearest Fade.
  - `decode_blend_without_render_state` — hasRenderState false: isEffect true → Additive (exact, factors 0); false → Opaque; false + alphaTest → Cutout.
  - `poiyomi_names_and_modes` — every preset's name and `_Mode` value as listed above.
  - `gltf_alpha_modes` — Opaque → Opaque, Cutout/TransClipping → Mask, others → Blend; Custom uses `nearest`.

- [ ] **Step 2: Build and run; expect compile failure** (`blend_mode.h` missing).

- [ ] **Step 3: Implement `blend_mode.cpp`.** Pure; no dependency beyond `<cstdint>`. Pattern-match (srcRgb, dstRgb, eqRgb) per the spec table; for Custom, `nearest` by destination: One → Additive, InvSrcAlpha → Fade, Zero → Opaque, else Fade.

- [ ] **Step 4: Run tests; expect all `vrchat` tests to pass** and the existing 14 exportgltf tests still pass.

- [ ] **Step 5: Commit** — `feat(exportgltf): decode GW2 blend words into Poiyomi presets`.

---

### Task 2: Shader profiles

**Files:**
- Create: `include/castlemist/exportgltf/shader_profiles.h`, `src/exportgltf/shader_profiles.cpp`
- Modify: `include/castlemist/extract/model_types.h` (`ModelMaterialCPU`: add `uint32_t materialId = 0; uint32_t materialFlags = 0;`)
- Modify: `src/extract/model_preview.cpp` (fill both from `m.materialId` / `m.materialFlags` where the material is built, beside `mat.materialFile = m.materialFile;`)
- Test: `tests/test_exportgltf_vrchat.cpp`

**Interfaces:**
- Consumes: `BlendInfo` (Task 1) — only for the trait test.
- Produces:
  ```cpp
  enum class AlphaUse { HolesAndShine, Shine, ReflectionOnly, Intensity, Opacity, InteriorWeight, Unused };
  enum class Channel : int8_t { None = -1, R = 0, G = 1, B = 2, A = 3 };
  enum class SpecLayer { None, GlossInAlpha, ExponentInAlpha };  // weapon-spec / legacy-spec
  struct ShaderProfile {
      std::string name;
      bool supported = true, clips = false;
      AlphaUse diffuseAlpha = AlphaUse::Shine;
      Channel maskMetal = Channel::None, maskGloss = Channel::None, maskSheen = Channel::None,
              maskGlow = Channel::None, maskGlowGate = Channel::None;
      SpecLayer specLayer = SpecLayer::None;
      int opacityTexture = -1;                 // index into textureFileIds whose R is opacity (legacy-untagged: 2)
      bool premultiplyRgbByAlpha = false;
      bool glowOnUv2MaskOnUv0 = false;         // weapon-spec swaps them
  };
  const ShaderProfile& profile_for(const ModelMaterialCPU& mat, uint64_t renderState);
  const ShaderProfile& default_profile();       // name "default": Shine, no clip, nothing interpreted
  ```

- [ ] **Step 1: Write the failing tests:**
  - `profile_by_amat` — materialFile 561567 → name "weapon-glow", clips true, HolesAndShine; 2348484 → "weapon-spec", specLayer GlossInAlpha, glowOnUv2MaskOnUv0 true; 13822 → "legacy-spec", clips false, ReflectionOnly, specLayer ExponentInAlpha; 14149 → "legacy-spec" with clips true, maskGlowGate R; 2449347 → "armor-mask" with maskMetal R, maskGloss G, maskSheen B, maskGlow A; 1171332 → "armor-mask-noglowA" (maskGlow None); 15999 → "prop-lit-noclip" (clips false); 2472137 → "jade-interior", supported false, InteriorWeight; 2507831 → "armor-silk", supported false.
  - `profile_by_untagged_trait` — materialFile not in table, materialId 0, materialFlags 0, every extraTextures role empty, renderState `0x6565000` → "legacy-untagged", opacityTexture 2.
  - `profile_default_for_unknown` — materialFile 999999999, tagged textures → `default_profile()`: name "default", clips false, Shine, all mask channels None.

- [ ] **Step 2: Run; expect compile failure.**

- [ ] **Step 3: Implement.** The table is a `static const` array of `{amat fileIds…, ShaderProfile}` built from the research note's section 4 rows — every AMAT listed there, including the "same blend word, not disassembled" ones for fx-soft-additive (`premultiplyRgbByAlpha` true, Intensity) and fx-premultiplied, and the fx-fire / fx-distort / armor-silk rows as `supported = false`. Lookup by `mat.materialFile`, then the legacy-untagged trait, then default.

- [ ] **Step 4: Run tests; expect pass.** Also rebuild `gw2dat_cli` (release) to confirm `model_preview.cpp` compiles.

- [ ] **Step 5: Commit** — `feat(exportgltf): GW2 shader profiles from the material-channel survey`.

---

### Task 3: Map building

**Files:**
- Create: `include/castlemist/exportgltf/vrchat_maps.h`, `src/exportgltf/vrchat_maps.cpp`
- Test: `tests/test_exportgltf_vrchat.cpp`

**Interfaces:**
- Consumes: `BlendInfo`, `ShaderProfile`, `profile_for` (Tasks 1–2).
- Produces:
  ```cpp
  struct MapSlot { ModelTextureCPU tex; uint8_t uv = 0; uint32_t fileId = 0; std::string source; bool present = false; };
  struct MaterialMaps {
      MapSlot baseColor, normal, packed, emissionMap, emissionMask, emissionBaked, distortion;
      struct Extra { std::string role, use; MapSlot slot; };
      std::vector<Extra> extras;
      std::string metalSource, smoothSource, reflectionSource, specularSource; // e.g. "mask.R", "diffuseAlpha", "conduct", "mtlness", "none"
      std::array<float, 3> emissionColor = {1, 1, 1};
      std::optional<std::array<float, 3>> specularTint, reflectionTint;
      std::vector<std::string> warnings;
  };
  MaterialMaps build_material_maps(const ModelPreview& model, const ModelMaterialCPU& mat,
                                   const BlendInfo& blend, const ShaderProfile& profile);
  ```
  Named constants are read from `mat.namedConstants` (first float only today); `speccp`/`envcr`/`envcp` tints need all 3 components — add `std::array<float,4>` storage: extend `ModelMaterialCPU` with `std::vector<std::pair<std::string, std::array<float,4>>> namedConstantVectors;` filled in `model_preview.cpp` alongside `namedConstants` (same loop, `c.value[0..3]`).

- [ ] **Step 1: Write the failing tests** (synthetic 2×2 or 4×4 textures):
  - `alpha_bands_with_clipping_profile` — diffuse alphas {30, 90, 128, 255}, weapon-glow profile, Cutout blend → baseColor alphas {0, 255, 255, 255}; packed G (smooth) {0, 0, 0, 255} (±1); packed A equals packed G; smoothSource "diffuseAlpha".
  - `alpha_bands_without_clipping` — same texture, legacy-spec 13822 (ReflectionOnly) → baseColor alpha all 255; packed B = shine {0,0,0,255}; reflectionSource "diffuseAlpha".
  - `armor_mask_channels` — armor-mask profile, mask layer RGBA (200, 100, 50, 30) → packed R 200, G 100; metalSource "mask.R"; emissionMask from mask A.
  - `weapon_glow_emission` — glow layer (UV0) + glowmask (UV2) → emissionMap present uv 0, emissionMask present uv 2, emissionBaked present; distortion from a `glowperturb` layer on UV1 with source "glowperturb".
  - `weapon_spec_swapped_uvs` — weapon-spec: glow layer on UV2, glowmask on UV0 → emissionMap.uv 2, emissionMask.uv 0; packed G from specular layer A; smoothSource "specular.A".
  - `conduct_as_metal` — weapon-glow, no mask, namedConstants `conduct` 0.4 → metalSource "conduct", packed R = 102 (±1) (`255 · clamp(conduct, 0, 1)`); `conduct` 1.5 → 255.
  - `normal_rebuilt_from_rg` — normal texel (255, 128, 0, 0) → B ≈ 128 (x=1,y≈0 → z≈0 → 128), A 255; G flipped (`255 − G`) by default.
  - `premultiplied_effect_rgb` — fx-soft-additive profile, texel (200, 100, 50, 128) → baseColor (100, 50, 25) ±1, alpha 128.
  - `missing_layer_is_skipped_with_warning` — a `glowmask` extra with `texIndex` −1 → emissionMask.present false, a warning mentioning "glowmask", the rest present. *(Review Focus 2)*
  - `placeholder_texture_becomes_constant` — a 4×4 uniform white layer in a mask role → not written as a map; warning names "placeholder".
  - `uv_above_three_warns` — glowmask on UV4 → warning containing "UV4".

- [ ] **Step 2: Run; expect compile failure.**

- [ ] **Step 3: Implement.** Per-texel loops over decoded RGBA8. Layers come from `mat.extraTextures` by `role` ("glow", "glowmask", "glowperturb"/"perturb", "mask", "specular", "mod", "decal", "height"); diffuse from `diffuseTex`, normal from `normalTex`. When maps of different sizes combine (e.g. shine from diffuse into packed), packed takes the diffuse size and mask-derived channels are nearest-sampled. Emission colour for the bake = glow's average colour (reuse the logic of `glow_colour` in `material_export.cpp`; move it into `vrchat_maps.cpp` and call it from both). One `const bool kFlipNormalGreen = true;` at the top of the file (spec "Open questions"; the atlas bake already flips and was checked in Blender).

- [ ] **Step 4: Run tests; expect pass** (and the existing exportgltf tests).

- [ ] **Step 5: Commit** — `feat(exportgltf): build Poiyomi maps per shader profile`.

---

### Task 4: Folder writer, materials.json, glTF alpha mode

**Files:**
- Create: `include/castlemist/exportgltf/vrchat_export.h`, `src/exportgltf/vrchat_export.cpp`
- Modify: `src/exportgltf/material_export.cpp` (alpha mode from `decode_blend` + `profile_for`; `alphaCutoff` 0.25; packed map → glTF `metallicRoughnessTexture` (B = metal, G = 1 − smooth) when present)
- Modify: `tests/test_exportgltf_writer.cpp` only if an existing assertion changes because of the new alpha mode (keep the change minimal and say why in the test).
- Test: `tests/test_exportgltf_vrchat.cpp`

**Interfaces:**
- Consumes: Tasks 1–3; `export_model_gltf` (existing, `gltf_export.h`).
- Produces:
  ```cpp
  struct VrchatFolderResult {
      bool ok = false; std::string error;
      std::string folder, glb, materialsJson, particlesJson;     // UTF-8 paths
      size_t materials = 0, clips = 0;
      std::vector<std::string> warnings;                          // all materials' warnings, prefixed "<Mat>: "
  };
  VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                         const std::string& name, uint32_t modelFileId);
  std::string safe_file_name(const std::string& s);   // illegal chars -> '_', trimmed, never empty
  ```
  `materials.json` schema exactly as spec section 4, plus `"usedByMeshes"`, `"preset"` (Poiyomi name), `"mode"` (`_Mode` value), `"exact"`, raw `"blend"` incl. `"eqRgb"`/`"eqA"`, `"profile"`, map `"source"` strings, `"panning": {"u":..,"v":..,"unit":"gw2-raw"}` or `null`.

- [ ] **Step 1: Write the failing tests:**
  - `folder_layout` — quad model with one material + a glow layer → files exist: `<Name>.glb`, `materials.json`, `Textures/<Mat> - BaseColor.png`, `- Normal.png` (when a normal exists), `- Packed.png`, `- EmissionMap.png`; JSON `materials[0].maps.baseColor.file` equals the relative path written.
  - `materials_json_fields` — preset, mode, exact, blend factors, profile name, alphaCutoff 0.25 with `alphaCutoffIsDefault` true, `gw2` contains every namedConstant, `animations` lists clip names, `particles` null when no effects.
  - `zero_triangle_material` — a second material no mesh uses → `usedByMeshes` false, still has maps.
  - `material_file_names_are_unique_and_legal` — two materials named `"A:B"` and `"A?B"` → two different legal file names, both referenced correctly. *(Review Focus 3)*
  - `folder_for_material_without_game_shader` — `hasRenderState` false, `isEffect` true → preset "Additive", profile "default", a warning, folder complete. *(Review Focus 1)*
  - `gltf_alpha_mode_from_profile` — weapon-glow material with a holes texture → glb material `alphaMode` "MASK", `alphaCutoff` 0.25; default profile with the same texture → "OPAQUE".

- [ ] **Step 2: Run; expect compile failure.**

- [ ] **Step 3: Implement** `write_vrchat_folder`: create folder + `Textures/`, build maps per material, write PNGs (`save_texture_png`), call `export_model_gltf(model, <folder>/<Name>.glb)`, copy its particle sidecar result path into JSON, write `materials.json` (pretty, 2-space). Per-material triangle use = sum of `indices.size()/3` over meshes with that `materialIndex`.

- [ ] **Step 4: Run tests; expect pass, and the full exportgltf group green.**

- [ ] **Step 5: Commit** — `feat(exportgltf): VRChat export folder with materials.json`.

---

### Task 5: Blender script keeps animations

**Files:**
- Modify: `tools/blender/castlemist_vrchat.py`
- Create: `tests/blender/check_fbx.py` (headless checker: imports an .fbx, prints `ACTIONS <name> <keyframes> <moving 0|1>` per action, `MESHES n`, `MATERIALS n`, `ARMATURE 0|1`)

**Interfaces:**
- Consumes: a .glb from `export_model_gltf` / `write_vrchat_folder`.
- Produces: CLI `blender -b --factory-startup -P castlemist_vrchat.py -- <in.glb> <out.fbx> [--mode model|character]` (default `character` keeps today's behaviour for `character-vrchat`).

- [ ] **Step 1: Baseline.** Export the dagger: `gw2dat_cli model --template … --dat … --file-id 1766522 --glb %TEMP%\dagger.glb`, convert with the current script, run `check_fbx.py`. Expected today: 0 actions.

- [ ] **Step 2: Change the script:**
  - `--mode` argument; character-only steps (piece joining by `<piece>_<n>`, eye bones, UVDiscard layer naming) run only in `character` mode; `model` mode joins nothing.
  - No `transform_apply` on an armature that has animation data; instead export with `bake_space_transform=True` and the existing axis/scale options. For meshes without armature, `transform_apply` stays.
  - No armature: don't exit; export meshes.
  - FBX export: `bake_anim=True, bake_anim_use_all_actions=True, bake_anim_use_nla_strips=False, bake_anim_force_startend_keying=True, bake_anim_simplify_factor=0.0`.

- [ ] **Step 3: Verify with the dagger:** `check_fbx.py` lists 5 actions (`zeropose`, `StowedA`, `DrawingA`, `DrawnA`, `StowingA`), 1+ meshes, armature 1. (They are constant poses: only presence is checked.)

- [ ] **Step 4: Verify a moving clip.** Find a weapon or back item whose clips move: `gw2dat_cli skel --dat … --file-id <id> --clip N` and look for curves with `totalKnots` > 1 per curve (start with legendaries from the survey: Pharus 2083162, Exordium, Astralaria 1200313). Export, convert, check: same action count as the .glb's `animations`, at least one action `moving 1`. Record the model id in the commit message.

- [ ] **Step 5: Verify a prop with no armature** (any prop from `docs/research/gw2-model-lods.md`, e.g. 1713091): exports an .fbx, `ARMATURE 0`, meshes > 0. *(Review Focus 5)*

- [ ] **Step 6: Verify characters still convert:** re-run the previous character flow only if a manifest exists (`gw2dat_cli character-vrchat`); otherwise state in the commit that it was not re-run.

- [ ] **Step 7: Commit** — `fix(blender): keep animations in the VRChat FBX; model mode; meshes without armature`.

---

### Task 6: `export_vrchat_model` (Blender step)

**Files:**
- Modify: `include/castlemist/ripper/vrchat.h`, `src/ripper/vrchat.cpp`
- Test: `tests/test_ripper_export.cpp` (existing ripper test group)

**Interfaces:**
- Consumes: `write_vrchat_folder` (Task 4); `find_blender`, `run_blender` (existing, `vrchat.cpp`; extend `run_blender` with the `--mode` argument).
- Produces:
  ```cpp
  struct VrchatModelReport {
      bool ok = false; std::string error;
      std::string folder, glb, fbx, blender, blenderLog;   // fbx empty when Blender didn't run or failed
      size_t materials = 0, clips = 0;
      std::vector<std::string> warnings;
  };
  VrchatModelReport export_vrchat_model(const ModelPreview& model, const std::string& parentDirUtf8,
                                        const std::string& name, uint32_t modelFileId,
                                        const VrchatOptions& vrc = {});
  ```
  Folder = `<parentDir>/<safe_file_name(name)>`. `VrchatOptions.blender_exe = "-"` means "don't run Blender" (for tests).

- [ ] **Step 1: Write the failing test** `CM_TEST(ripper, no_blender_still_writes_folder)` — quad model, `blender_exe = "-"` → ok true, fbx empty, `blender` explains it was skipped, `.glb` and `materials.json` exist. *(Review Focus 4)*

- [ ] **Step 2: Run; expect compile failure.**

- [ ] **Step 3: Implement:** `write_vrchat_folder`, then Blender in `model` mode writing `<Name>.fbx` (+ `.blend`); on failure keep the log as `<Name> blender.log` and put its path in `blenderLog`.

- [ ] **Step 4: Run `cm_test_ripper`; expect pass.**

- [ ] **Step 5: Commit** — `feat(ripper): export_vrchat_model`.

---

### Task 7: CLI `--vrchat` and the File menu item

**Files:**
- Modify: `tools/gw2dat_cli/main.cpp` (`cmd_model`: `--vrchat <dir>` beside the existing `--glb`)
- Modify: `src/ui/detail/app_state.h` (new `ID_FILE_EXPORT_VRCHAT_MODEL`, next free id after 2184), `src/ui/window_proc.cpp` (menu item "Export for &VRChat (Model)..." after the glTF model items; enable/disable with them; command handler), `src/ui/file_ops.cpp` (`do_export_vrchat_model(HWND)`: folder prompt, background thread, `WM_APP_` done message reporting folder, `.fbx` or the reason it's missing, and the warning count)
- Modify: `docs/using-castlemist.md` (menu table row + CLI line)

**Interfaces:**
- Consumes: `export_vrchat_model` (Task 6).

- [ ] **Step 1: CLI.** `--vrchat <dir>` calls `export_vrchat_model(*pv, dir, <name>, fileId)` where name = the model's content name if `cmap` gives one, else `model_<fileId>`; emits `{"vrchat": {folder, glb, fbx, blender, materials, clips, warnings}}`.

- [ ] **Step 2: Verify CLI on the Forged Dagger** (`--file-id 1766522 --vrchat %TEMP%\vrc`): folder has `.glb`, `.fbx`, `.blend`, `materials.json`, `Textures\` with `- BaseColor`, `- Normal`, `- Packed`, `- EmissionMap`, `- EmissionMask`, `- Distortion` for `Mat_561567`; JSON profile "weapon-glow", preset "Cutout", emissionMask uv 2.

- [ ] **Step 3: UI.** Menu item + handler, following `do_export_gltf_model` (copy the model, run on a thread, post a done message). The folder prompt can reuse the existing save dialog with the chosen file's stem as the name and its directory as the parent.

- [ ] **Step 4: Verify UI** by launching the app on a model (`GW2_AUTOLOAD=<mftIndex>`, e.g. 526223 for the dagger), using the menu, and checking the folder; build `castlemist` (release) clean.

- [ ] **Step 5: Docs + commit** — `feat: Export for VRChat (Model) in the File menu and gw2dat_cli`.

---

### Task 8: End-to-end check across the survey

**Files:**
- Modify: `docs/research/README.md` (link the four new research notes)

- [ ] **Step 1:** Run `gw2dat_cli model … --vrchat` on one model per supported profile from the research note's section 6 table (Forged Dagger, Twilight, Incinerator, Frostfang, Wings of Dwayna, a SotO weapon with 2348484, an armor piece with 2449347, Jade Tech Scepter). For each: JSON valid, profile as expected, no "default profile" warning, maps present per the profile, `.fbx` produced.

- [ ] **Step 2:** Open two exports' maps in castlemist's channel viewer (File → Open File… on the PNGs) and confirm the packed channels match their `source` strings.

- [ ] **Step 3:** Run the full suite (`ctest --test-dir build/debug`) and build release; all green.

- [ ] **Step 4: Commit** — `docs: link VRChat export research notes`.
