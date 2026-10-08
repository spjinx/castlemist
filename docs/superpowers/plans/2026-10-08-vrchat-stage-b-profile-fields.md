# VRChat Export Stage B — Profile Fields Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Six new ShaderProfile fields so the shaders the second survey could not map (cutout layer, metal mask, opacity+glow alpha, parallax base colour, rim ramp, projector) export correctly or with structured data for the Unity importer.

**Architecture:** Each field is data on `ShaderProfile` (shader_profiles.h/.cpp), consumed by `Builder` in vrchat_maps.cpp, serialised by vrchat_export.cpp into `materials.json`. One new shared map, `alphaMask`, carries opacity/cutout that lives on its own UV set. The `.glb` keeps its current behaviour unless a task says otherwise.

**Tech Stack:** C++20 (MSYS2 ucrt64 g++, CMake + Ninja), nlohmann::json, castlemist test framework (`CM_TEST`/`CHECK*`).

**Spec:** `docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md` (sections 2–4). Shader facts: `docs/research/gw2-material-channels.md` section 8 (8.2 per-AMAT findings, 8.3 families, 8.4 proposed rows, "New fields"). The research note is the authority for every formula below; where this plan and the note disagree, the note wins.

## Global Constraints

- Build from PowerShell with `$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH` first; from Git Bash the compiler fails silently.
- Unit tests: `cmake --build build/debug --target cm_test_exportgltf`, run `build/debug/bin/cm_test_exportgltf.exe` with `build/debug/bin` on PATH. Full suite: `ctest --test-dir build/debug`. Release: `cmake --build build/release --target gw2dat_cli castlemist`.
- Real checks: `build\release\bin\gw2dat_cli.exe model --template dumps\packfile\gw2_packfile.json --dat "E:\Games\gw2\Guild Wars 2\Gw2.dat" --file-id <id> --vrchat %TEMP%\<dir> [--blender -]`. Never open, read or grep Gw2.dat directly. Exports only under %TEMP%.
- Profiles are switched on fields, never on names. Only hand-read AMAT ids go in the table (no signature-only ids). Table-integrity test (no duplicate id) stays green.
- Values castlemist can't read are `null` in `materials.json`; every approximation adds a warning naming the layer/condition.
- Poiyomi textures read UV0–UV3 only.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **A layer named by a new field is missing or failed to decode:** that output is skipped with a warning; the rest of the material exports. Each task adds one such test.
2. **A layer named by a new field is a 4×4 placeholder:** treated as its constant (UV-independent), never as a missing map. Task 1 and Task 4 test it.
3. **Two outputs want the same slot** (e.g. OpacityAndGlow emission when the material also has a glow layer; alphaMask from both a cutout layer and an opacity layer): the higher-priority source wins and a warning names the loser. Tasks 3 and 4 test it.
4. **Existing profiles are unchanged:** every new field defaults to "off"; the existing tests (79) still pass unmodified.

---

### Task 1: `alphaMask` output + `cutoutLayer` (511663, 53858)

**Files:** shader_profiles.h/.cpp, vrchat_maps.h/.cpp, vrchat_export.cpp, tests/test_exportgltf_vrchat.cpp, spec section 3 (one paragraph).

**Interfaces — produces:**
- `MaterialMaps::alphaMask` (MapSlot, greyscale in RGB, A 255) + `float alphaMaskCutoff` (−1 = opacity, not a cutoff) + source string.
- `ShaderProfile::cutoutRole` (std::string, empty = none) and `enum class CutoutChannels { R, RxA } cutoutChannels`.
- materials.json `maps.alphaMask {file, uv, fileId, source, cutoff}`; file `Textures/<Mat> - AlphaMask.png`.

- [ ] Tests first: `cutout_layer_becomes_alpha_mask` (511663-style: cutout layer on UV1 with R×A → alphaMask uv 1, values R·A/255, cutoff 0.5, preset Cutout, cutout not an extra); `cutout_layer_with_diffuse_holes` (53858-style: cutout R on UV2 + diffuse alpha → BaseColor alpha keeps holes `saturate(2a)<0.5`, alphaMask = cutout.R); `cutout_layer_missing_warns`; `cutout_placeholder_is_constant` (constant ≥ 0.5 → no alphaMask, no holes; < 0.5 → whole material cut, warning). Profile rows: 511663 gets `cutoutRole "cutout"`, RxA, clips=true (keeps weapon-cutout-glow name); new row for 53858 per the note (shine, mod, cutout R on UV2, clips) — read 8.3 "cutout + mod" for its exact fields.
- [ ] Implement; the glTF alpha mode for these materials stays MASK (BaseColor alpha) — the alphaMask is VRChat-folder only; a warning on 511663 says the cutout is a dissolve that never cuts at rest (cutfade).
- [ ] Real check: Skyforged Hammer 3123167 material 1 → profile weapon-cutout-glow, alphaMask uv 1 (cutfade note), glow still mapped.
- [ ] Commit `feat(exportgltf): alpha mask on its own UV; cutout-layer profiles`.

### Task 2: `AlphaUse::OpacityAndGlow` (44709)

**Interfaces — consumes:** Task 1's alphaMask is not used here. **Produces:** `AlphaUse::OpacityAndGlow`.

- [ ] Tests first: `opacity_and_glow_splits_alpha` — alphas {0, 64, 127, 128, 255} → BaseColor A {0, 128, 254, 255, 255} (±1); EmissionMask {0,0,0,0,255} = `saturate(2a−1)`; EmissionMap = base colour; preset Fade (SrcA/InvSrcA word); `opacity_and_glow_with_glow_layer_warns` (a real glow layer wins the emission slot, warning names the self-illumination). Row: new `fx-alpha-glow` = 44709 per 8.2 (ramp × diffade not mapped → warning naming `ramp`/`diffade`).
- [ ] Implement in `build_base_color` / `build_emission`.
- [ ] Real check: model 44730 material 3.
- [ ] Commit `feat(exportgltf): opacity-and-glow diffuse alpha`.

### Task 3: `maskRole` (3121953)

**Produces:** `ShaderProfile::maskRole` (std::string, default "mask"); every mask-channel field reads the layer with that role.

- [ ] Tests first: `mask_role_reads_named_layer` — profile maskRole "metalmask", maskMetal G → Packed R = metalmask.G, metalSource "metalmask.G", metalmask not an extra; existing armor-mask test unchanged. Row: new `prop-metalmask` = 3121953 only (561567 lit core + metalmask.G metal + mod) per 8.3.
- [ ] Implement (replace hard-coded "mask" lookups with `profile_.maskRole`).
- [ ] Real check: Astral Ribbons 3121936 is not this shader — find a model using 3121953 from the census note (8.3 names one) or report none found.
- [ ] Commit `feat(exportgltf): mask channels read a named role`.

### Task 4: `baseColorRole` + opacity layer (842652)

**Produces:** `ShaderProfile::baseColorRole` (std::string, empty = diffuse), `opacityRole` + `Channel opacityChannel` (opacity from a layer on its own UV → Task 1's `alphaMask` with cutoff −1).

- [ ] Tests first: `base_color_from_named_role` — base colour from a `parallax` layer (UV0), the castlemist "diffuse" exported as an extra with use "uv-offset"; `opacity_layer_becomes_alpha_mask` — mask.R on UV1 → alphaMask uv 1, cutoff −1, BaseColor A = parallax.A; `cutout_and_opacity_both_warn` (Review Focus 3: cutout wins, warning names opacity). Row: new `fx-parallax-layer` = 842652 per 8.2 (baseColorRole "parallax", opacityRole "mask" R, Opacity, warnings for parallax/perturb and unlit).
- [ ] Implement in `collect_layers` / `build_base_color`.
- [ ] Real check: Astralaria 1200313 materials 3/4.
- [ ] Commit `feat(exportgltf): base colour and opacity from named layers`.

### Task 5: `rimRampRole` (1465623)

**Produces:** `ShaderProfile::rimRampRole`, `Channel rimMaskChannel` (+ mask role from Task 3); `MaterialMaps::rim` {present, color (ramp's average of its brightest half, bytes/255, sRGB-encoded), maskSlot (greyscale of the mask channel, may be a constant), scroll (voffset raw or null)}; materials.json `rim {color, mask: {file|constant, uv, channel}, scroll}` or null; the ramp itself exported as an extra with use "rim-ramp".

- [ ] Tests first: `rim_ramp_records_colour_and_mask` (ramp 4×1 gradient, mask placeholder red → rim.color from the ramp, mask constant 1.0, warning "rim ramp is a view-angle lookup"); `rim_ramp_missing_warns`. Row: 1465623 → new `weapon-rim-ramp` = 561567 lit core (clip, shine, conduct) + rimRampRole "ramp", rim mask = mask.R.
- [ ] Implement.
- [ ] Real check: Astral Ribbons 3121936 material 0.
- [ ] Commit `feat(exportgltf): rim-ramp glow described for the importer`.

### Task 6: `projectorRole` (77238 family)

**Produces:** `ShaderProfile::projectorRole`; materials.json `projector {file, uv, fileId, falloff: [prjfall.x, prjfall.y] | null, coverage: "saturate(2a)"}`; the projector layer written as `Textures/<Mat> - Projector.png`; warning "projector blends by world-up facing: not baked".

- [ ] Tests first: `projector_layer_described` (falloff from namedConstantVectors `prjfall`; projector consumed, not an extra); `projector_missing_warns`. Rows: new `prop-projector` = 77238, 835499, 69856, 69792 (no clip) and 512093, 512112 (clip) per 8.3 — base: prop-lit with mod.
- [ ] Implement.
- [ ] Real check: model 2141433 material 7 (77238).
- [ ] Commit `feat(exportgltf): projector layers described for the importer`.

### Task 7: Coverage re-check + docs

- [ ] Re-run the census sample from the research note's method on the same step (every 20th MODL) only if a script exists in `%TEMP%\cm_matsurvey2`; otherwise re-export the 6 real-check models and Task 8's 12-model list from the first plan (`docs/superpowers/plans/2026-10-07-vrchat-model-export.md` Task 8) and confirm no profile regressed.
- [ ] Spec: section 2 profile table gains the new rows; section 4 schema gains `alphaMask`, `rim`, `projector`. Research note 8.4: mark each Stage B row "implemented".
- [ ] Full suite + release build green. Commit `docs: Stage B profile fields`.
