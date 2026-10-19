# World Materials and Motion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The map window draws each map with its own look and movement: terrain painted from its blend pages and ground textures, props with their diffuse textures, water in its env-preset colours, UV scroll, and animated props playing the clip their placement names. Models also stream in faster.

**Architecture:** `native`/`extract` read the new fields by template name and decode textures once, capped. `world` keeps them raw and adds the pure terrain blend and clip lookup. `render` gets a texture cache and material passes inside `WorldView`. These extend `world_view.cpp`, `world_draw.*`, `world_props.*` and `mesh_bake.*`. CPU prep moves to the model-load workers. `ui` adds the Materials/Motion toggles, the status fields and the headless hooks.

**Tech Stack:** C++20, MinGW-w64 GCC, CMake + Ninja presets, Direct3D 11, Win32, the repo's test framework (`tests/framework/test_framework.h`, `CM_TEST`, `CHECK*`, `SKIP`).

**Spec:** docs/superpowers/specs/2026-10-14-world-materials-design.md

## Global Constraints

- Build: `export PATH="/c/msys64/ucrt64/bin:$PATH"; cmake --build --preset debug`. Tests: `build/debug/bin/cm_test_<suite>.exe <filter>` (the filter is a `group.name` prefix) and `ctest --preset debug`. All paths are relative to the worktree root `C:/Users/vital/Downloads/castlemist/castlemist/.claude/worktrees/world-materials`.
- Map space only: left-handed, +X east, +Y north, up = −Z. "Above" means smaller Z. Depth below the water plane is `terrainZ − planeZ`.
- Layer rules: `world` depends on nothing above `extract`. Nothing below `render` mentions Direct3D. `WorldView` owns its own device and never touches the old renderer's globals (`src/render/detail/state.h`, `detail/map_fly.h`). This plan adds one edge, `render → exportgltf`, for `profile_for` and `decode_blend` (Task 10). It makes no cycle, because `exportgltf` depends only on `extract` and `world`.
- Data stays as stored, in `world`. New fields are copied by template field name and kept raw. They are interpreted in one place in `render`.
- UNPROVEN values are labelled settings in `WorldMaterialSettings` (Task 8). Each one is named in `WorldView::warnings()` and in its code comment, with the note section. Values from the spec: `groundRepeatsPerChunk = 8` (T3D's guess, note §1.6; `uvScale` stays 0 in `WorldScene`). `uvScrollUnit = per second` and `offset = frac(scroll · t)` on the UV set `uvIndex` (note §4). All animated props start at t = 0 and loop (note §5). The water plane covers the whole map and is depth-tested (note §6). `maxTextureSize = 512`, mip-mapped. The process-wide `set_texture_full_res` is never flipped.
- Exact values from the spec: luminance (0.30, 0.59, 0.11); saturation `2·modx.g`; brightness `2·modx.r`; ground alpha + 0.01; an all-zero picker gives `w = (0,0,0,1/255)`; specular `nrm.a·0.854902`; rim `saturate(2·modx.b − 1)·0.239216`; fresnel `(1 − saturate(N·V))^8`. Clip below alpha 0.25. `MatVtx` is 32 bytes (position f32×3, normal+AO UNORM8×4, uv0 f32×2, uv1 f32×2). The terrain vertex grows from 16 to 20 bytes, adding `(s, t)` as UNORM16×2. `SkinVtx` holds bone indices UINT8×4 and weights UNORM8×4. Mips are CPU box-filtered down to 4×4. The HLSL must match the CPU blend within 2/255 per channel.
- Targets (amendment): Queensdale models stream in well under 156 s, and the largest UI-thread `add_models` batch takes under 50 ms.
- Never read Gw2.dat directly. Dat tests go through castlemist code and SKIP without `GW2_TEST_DAT`. The dat is `E:/Games/gw2/Guild Wars 2/Gw2.dat`, passed as `GW2_TEST_DAT` to tests and `GW2_DAT` to the app. The template is `C:/Users/vital/Downloads/castlemist/castlemist/dumps/packfile/gw2_packfile.json`. Tests get it through `GW2_TEST_TEMPLATE`. The app finds it when the working directory is the main checkout.
- Headless runs use `GW2_DUMP_DIR` pointing to an existing directory.
- T3D is GPL-3 and castlemist is MIT: re-derive behaviour, cite file:line, never copy code.
- Doc comments follow the repo style (`/// @brief`, `/// @file`) and cite note sections.
- Every commit message ends with a blank line and then `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Use `git commit -m "<subject>" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"`.

## Review Focus

- **A texture that arrives after its model.** Another worker can claim the diffuse fileId, so its pixels come in a later batch. Until then the sub-range must draw in the AO look; after that it must draw textured. A null SRV must never be bound with the textured flag set. → Task 10 test `world_materials.textured_props_draw_per_submesh`.
- **Odd texture shapes through the cap and the mips.** This covers non-power-of-two sizes, 1×N strips, and textures already ≤ 4×4. A zero dimension or an endless mip loop would crash or hang the worker. → Task 4 tests `capped.halve_odd_clamps`, `capped.mips_stop_at_4x4`.
- **Animated models that cannot be posed.** This covers a clip with duration 0 (fmod by zero gives NaN) and a model with more than 256 joints (UINT8 bone indices). Either must give a finite palette or stay in bind pose and be counted, never a frame of NaN vertices. → Task 12 tests `world_materials.pose_palette_zero_duration_no_nan`, `world_materials.animated_instances_counted` (its 300-joint case).
- **Device reset after textures arrived.** Recovery re-adds models but not textures. Materials must fall back to the AO look with no stale SRV. → Task 8 test `world_view.recreate_drops_textures`.
- **Terrain chunks with missing inputs.** This covers absent ground slots, picker weight that falls only on absent slots, unresolved chunks and solid pages. None may produce NaN or black. → Task 6 test `terrain_blend.absent_slot_gets_no_weight`; Task 9 tests `world_materials.unresolved_chunk_draws_ao_look`, `world_materials.solid_page_is_one_texel`.

---

### Task 1: fileId lookups by binary search (amendment 3)

**Files:**
- Modify: `src/native/gw2dat.cpp` (`get_by_base_id` :179, `get_by_file_id` :188), `include/castlemist/native/gw2dat.h` (doc comment at :110-111)
- Test: Create `tests/test_native_dat_lookup.cpp`; add it to the `native` SOURCES in `tests/CMakeLists.txt`

**Interfaces:**
- Produces the same signatures as today: `uint32_t get_by_base_id(Gw2Dat&, uint32_t file_id)` and `std::vector<uint32_t> get_by_file_id(Gw2Dat&, uint32_t base_id)`. The precondition is now documented: the two lists are sorted, as `load_dat_file` leaves them.

Decision: `load_dat_file` already sorts `mft_file_id_data_list` by `file_id` (gw2dat.cpp:87) and `mft_base_id_data_list` by `base_id` (:100). So the amendment's "index built once, read-only afterwards" is these two lists, searched with `std::lower_bound`. There is no new member and nothing to build, and the worker pool cannot see the lists change. Every lookup that goes through these functions benefits: `load_model_by_fileid`, `texture_entry`, `consecutive_files`. Task 17 records this in the spec Amendments.

- [ ] **Step 1: Write failing tests**
  - `dat_lookup.file_to_base_hit_and_miss`: `mft_file_id_data_list = {{10,3},{20,5},{20,6},{30,7}}` gives `get_by_base_id(d,20) == 5` (the first equal entry, as the linear scan returned), `(d,25) == 0`, `(d,5) == 0`, `(d,31) == 0`.
  - `dat_lookup.base_to_files_sorted`: `mft_base_id_data_list = {{{9,4},3},{{1},5}}` gives `get_by_file_id(d,3) == std::vector<uint32_t>{4,9}`; `get_by_file_id(d,4)` is empty.
  - `dat_lookup.lookups_are_logarithmic`: 800 000 entries with `file_id = 2k+1`, `base_id = k+1`, and 20 000 lookups spread over the range. Every result is correct and the loop finishes in under 200 ms (`std::chrono::steady_clock`). The linear scan takes seconds.
- [ ] **Step 2: Run** `build/debug/bin/cm_test_native.exe dat_lookup.` → the timing test FAILS.
- [ ] **Step 3: Implement** both functions with `std::lower_bound` over the sorted lists. `get_by_file_id` still returns a sorted copy.
- [ ] **Step 4: Run** `cm_test_native.exe dat_lookup.` → PASS; then `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `perf(native): fileId and baseId lookups by binary search over the sorted MFT lists`.

### Task 2: Extractor readers (texTransforms, animSequence, color, presetIndex, water presets, uvData)

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp`: `Material` (:150), `parseMaterials` (:3482), `MapProp` (:1635) and `readProp` in `parseMapProps` (:2337), `MapEnvLight` (:1254) and `parseMapEnv` (:1318-1327), `MapTerrainMaterials::Chunk` (:1717) and its `uvData` read (:1810), plus the new `parseMapEnvWater` after `countEnvWaterPresets` (:2124)
- Test: Create `tests/test_native_materials.cpp` (`native` suite)

**Interfaces:**
- Produces (namespace `castlemist::model`):
  ```cpp
  struct MatTexTransform {            // ModelMaterialTexTransformV*, as stored; absent fields 0
      uint32_t flags = 0; uint8_t uvIndex = 0, columns = 0, rows = 0; uint16_t count = 0; float fps = 0;
      float scroll[2] = {0, 0}, scrollFreq[2] = {0, 0}, scale[2] = {0, 0}, scaleFreq[2] = {0, 0};
      float rotate = 0, rotate2 = 0;  // the template names two fields `rotate`: first, second
  };
  std::vector<MatTexTransform> Material::texTransforms;
  uint64_t Extractor::MapProp::animSequence = 0;   // propAnimArray only (PackMapPropObjAnimSeqV*)
  uint8_t  Extractor::MapProp::color[4] = {0, 0, 0, 0};
  int      Extractor::MapEnvLight::presetIndex = -1;   // the lighting[] index parseMapEnv used
  struct Extractor::MapTerrainMaterials::UvData { float translation[2]{}, xScaleRange[2]{}, yScaleRange[2]{}, scaleSpeed[2]{}; float rotation = 0; };
  UvData Extractor::MapTerrainMaterials::Chunk::uv;   // read when hasUvData
  struct Extractor::MapEnvWater {     // PackMapEnvDataWaterV*, as stored
      uint32_t waterFlags = 0; float animAmplitude = 0, animChoppiness = 0, animWind[2]{}, bumpAmount = 0;
      float bumpAngle[2]{}, bumpScale[2]{}, bumpSpeed[2]{}, bumpTile[2]{};   // ...0 / ...1
      float patternAngle = 0, patternTile = 0, patternSpeed = 0, patternEdge = 0;
      uint8_t surfaceShallowColor[4]{}, surfaceDeepColor[4]{}, patternColor[4]{};
      float surfaceFresnel = 0, distortAmount = 0, depthAttenuation = 0;
      uint32_t materialFileId = 0;                 // materialFilename
      std::vector<uint32_t> textureFileIds;        // textureFilenames (0 kept)
      std::vector<uint32_t> constantTokens; std::vector<std::array<float, 4>> constantValues;
      float foamSpawn = 0, foamDissolve = 0, foamDepthAttenuation = 0; uint8_t foamColor0[4]{}, foamColor1[4]{};
      uint32_t bumpTextureFileId = 0, patternTextureFileId = 0;   // older versions' own fields (V29: bumpTexture, patternTexture)
  };
  std::vector<MapEnvWater> Extractor::parseMapEnvWater();   // dataGlobal's `water`
  ```
- How each field is read. All template field names were checked against `gw2_packfile.json`.
  - `texTransforms`: the element struct comes from the field's `element.struct` and each field is read by name. V45 has `random` where V66 has `flags`, so `flags` stays 0 there. The two `rotate` fields are found by position, the way `parseTerrainMaterials` finds its two `flags` (gw2model.hpp:1753-1761).
  - `parseMapEnvWater`: go env → `dataGlobal` (kind `ptr`, `target.struct`, as gw2model.hpp:2139) → `water`. Accept both `array_ptr` (inline elements) and `ptr_array_ptr` (one pointer per element). Both kinds exist (:2133).
  - `uvData`: a `ptr` whose `target.struct` is `PackMapTerrainChunkUVDataV*`. Read its fields by name when the pointer is non-null.
  - `color`/`animSequence`: read by name in `readProp`. `propInstanceArray` transforms keep zeros.
  - `presetIndex`: the explicit preset when valid, else `bestPi`. It stays −1 when the result is not present.

- [ ] **Step 1: Write failing tests.** Build the packfile by hand: copy the `put16/put32/make_packfile` pattern of `tests/test_core_packfile.cpp:18-53` and use 32-bit pointers (pfVersion 1). Self-relative pointers are measured from their own address. `arrayAt` reads a count and then a pointer at +4. The minimal template has `"chunks": {"MODL": {"<the chunk version you write>": "TRoot"}}`, `TRoot{permutations: array_ptr<TPerm>}`, `TPerm{materials: ptr_array_ptr<TMat>}`, and `TMat{texTransforms: array_ptr<TT>}`. `TT` takes the field list of `ModelMaterialTexTransformV66` (or `V45`) copied from the template. Call `Extractor(bytes, tpl).extract()`.
  - `native_materials.tex_transforms_read_by_name`: two V66 entries, `{uvIndex 1, columns 1, rows 1, count 1, scroll (0, 0.2), rotate 0.25, rotate2 0.5}` and `{uvIndex 0, scroll (0, 0.2)}`. Expect `materials[0].texTransforms.size() == 2`, `[0].uvIndex == 1`, `[0].scroll[1] ≈ 0.2`, `[0].rotate ≈ 0.25`, `[0].rotate2 ≈ 0.5`, `[1].uvIndex == 0`.
  - `native_materials.tex_transforms_v45_without_flags`: V45 fields (`random, uvIndex, columns, rows, count, fps, scroll`). Expect `flags == 0`, `uvIndex` read, `scale == (0,0)`.
  - `native_materials.no_tex_transforms_field_is_empty`.
- [ ] **Step 2: Run** `cm_test_native.exe native_materials.` → FAIL (does not compile).
- [ ] **Step 3: Implement** the six readers as specified. Task 15 checks MapProp, presetIndex, water and uvData against real maps.
- [ ] **Step 4: Run** `cm_test_native.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(native): read texTransforms, prop animSequence/color, env water presets, terrain uvData by field name`.

### Task 3: ModelLoadOptions, header-only textures, texTransforms, parallel clip tokens

**Files:**
- Modify: `include/castlemist/extract/entry_extractor.h` (:225), `include/castlemist/extract/model_types.h` (`ModelMaterialCPU` :97, `ModelPreview` :479), `src/extract/internal.h` (:170), `src/extract/model_preview.cpp` (`get_texture` :113-125, effect guess :187-194, materials loop, clip loop :418-427, `load_model_by_fileid` :577), `src/extract/texture_source.cpp` (next to `peek_mft_atex` :49)
- Test: Create `tests/test_extract_model_load.cpp` (`extract` suite, which already has `PRIVATE_HEADERS src/extract`)

**Interfaces:**
- Consumes: `castlemist::model::Material::texTransforms` (Task 2).
- Produces:
  ```cpp
  struct ModelLoadOptions { bool wantGame = true; bool decodeTextures = true; };            // entry_extractor.h
  std::shared_ptr<ModelPreview> load_model_by_fileid(Gw2Dat& dat, uint32_t file_id, const ModelLoadOptions& opts);
  // the existing bool overload stays and forwards {want_game, true}
  struct ModelTexTransformCPU { /* the fields of castlemist::model::MatTexTransform, same names */ };   // model_types.h
  std::vector<ModelTexTransformCPU> ModelMaterialCPU::texTransforms;   // copied as stored
  std::vector<uint64_t> ModelPreview::animClipTokens;                  // parallel to animClips
  // internal.h
  std::shared_ptr<ModelPreview> build_model_preview(const std::vector<uint8_t>&, Gw2Dat&, const nlohmann::json&, const ModelLoadOptions&);
  void append_decoded_clips(const std::vector<castlemist::model::AnimClip>& clips, ModelPreview& out,
                            const std::function<castlemist::granny::Anim(const castlemist::model::AnimClip&)>& decode);
  bool peek_texture_by_fileid(Gw2Dat& dat, uint32_t fileId, ModelTextureCPU& out);
  ```
- Rules:
  - With `decodeTextures == false`, `get_texture` calls `peek_texture_by_fileid`. That reads the header of the fileId's own entry only and fills `fileId, baseId, width, height, fmt, mipCount, isNormal = is_normal_format(fmt)`. `rgba` stays empty. The diffuse and normal choice (by area and `isNormal`, :164-176) is unchanged.
  - The pixel effect guess (:193) runs only when `rgba` is non-empty. On the map path the view decides it from `CappedTexture::looksLikeEffect` instead (Tasks 4 and 10).
  - The existing bool overloads forward `{want_game, true}`.
  - `append_decoded_clips` pushes to `animationTokens` for every clip. For each clip that decodes valid, it pushes to `animClips`, `animClipBank` and `animClipTokens` together. The default decode is `granny::parse` on non-empty `rawGranny`; otherwise the clip counts as invalid.

- [ ] **Step 1: Write failing tests**
  - `model_load.clip_tokens_stay_parallel_when_one_fails` (spec): three `AnimClip` tokens `463, 6351, 464399` ("on", "off", "open"), with a fake decoder that fails the second. Expect `animationTokens == {463, 6351, 464399}`, `animClips.size() == 2`, `animClipTokens == {463, 464399}`, `animClipBank.size() == 2`.
  - `model_load.no_clips_leaves_all_empty`.
  - `model_load.options_default_decode_everything`: `ModelLoadOptions{}` has `wantGame && decodeTextures`.
- [ ] **Step 2: Run** `cm_test_extract.exe model_load.` → FAIL (does not compile).
- [ ] **Step 3: Implement.** Copy `m.texTransforms` into `mat.texTransforms` field by field in the materials loop.
- [ ] **Step 4: Run** `cm_test_extract.exe` → PASS; `ctest --preset debug` → PASS. The header-only path is checked on real data in Task 15.
- [ ] **Step 5: Commit** `feat(extract): ModelLoadOptions with header-only textures; texTransforms; animClipTokens parallel to animClips`.

### Task 4: decode_texture_capped

**Files:**
- Modify: `include/castlemist/extract/model_types.h`, `include/castlemist/extract/entry_extractor.h`, `src/extract/internal.h`, `src/extract/texture_source.cpp`
- Test: `tests/test_extract_model_load.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct CappedTexture {                       // model_types.h
      uint32_t fileId = 0, baseId = 0;         // baseId: the entry decoded (the reduced member when GW2 ships one)
      int width = 0, height = 0;               // mip 0
      std::vector<std::vector<uint8_t>> mips;  // RGBA8; empty = decode failed
      bool isNormal = false, hasCutout = false, looksLikeEffect = false;
      size_t bytes() const;                    // sum of mips[i].size()
  };
  bool decode_texture_capped(Gw2Dat& dat, uint32_t fileId, int maxSize, CappedTexture& out);   // entry_extractor.h
  void halve_rgba(const std::vector<uint8_t>& in, int w, int h, std::vector<uint8_t>& out, int& ow, int& oh);   // internal.h
  std::vector<std::vector<uint8_t>> build_mips(std::vector<uint8_t> level0, int w, int h);                    // internal.h
  ```
- Rules:
  - The entry is `texture_entry(dat, fileId, /*full=*/false)` (texture_source.cpp:228), decoded with `decode_mft_atex`. This takes the reduced member regardless of `g_tex_full_res`.
  - `halve_rgba` runs while `max(w, h) > maxSize`. It is a 2×2 box average with rounding `(a+b+c+d+2)>>2`, gives `ow = max(1, w/2)` and `oh = max(1, h/2)`, and clamps reads at odd edges.
  - Mips halve the same way until both sides are ≤ 4. A level 0 that is already ≤ 4×4 gets no further mips.
  - `hasCutout = !isNormal && compute_alpha_cutout(mips[0])` (:112) and `looksLikeEffect = looks_like_effect(mips[0])`.
  - `out.fileId` is set even on failure. The caller passes a failed texture (empty `mips`) on so it can be counted.

- [ ] **Step 1: Write failing tests**
  - `capped.halve_box_filters`: 2×2 texels with R = 0, 255, 255, 0 → 1×1 with R 128.
  - `capped.halve_odd_clamps`: 3×1 → 1×1 (no zero dimension); 1×5 → 1×2.
  - `capped.mips_stop_at_4x4`: 32×8 gives levels 32×8, 16×4, 8×2, 4×1 (4 levels). 4×4 gives 1 level. 6×3 gives 6×3, 3×1 (2 levels).
  - `capped.bytes_sum`: mips of 8×8 and 4×4 → `bytes() == 320`.
- [ ] **Step 2: Run** `cm_test_extract.exe capped.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_extract.exe` → PASS.
- [ ] **Step 5: Commit** `feat(extract): decode_texture_capped: reduced member, capped, box-filtered mips to 4x4`.

### Task 5: WorldScene additions and load_world wiring

**Files:**
- Modify: `include/castlemist/world/world_scene.h`; `src/world/terrain.cpp` (`build_terrain` :165, `resolve_terrain_materials` :365-425, warnings :440-442); `src/world/props.cpp` (`build_props` :207-214); `include/castlemist/world/load_world.h` (`EnvSources` :52); `src/world/load_world.cpp` (`build_water` env line :100-102, `build_environment` :126, `attach_environment` :144)
- Test: `tests/test_world.cpp`

**Interfaces:**
- Consumes: Task 2's readers.
- Produces:
  ```cpp
  struct TerrainUvData { float translation[2]{}, xScaleRange[2]{}, yScaleRange[2]{}, scaleSpeed[2]{}; float rotation = 0; };
  // TerrainMaterial
  std::vector<uint32_t> texIndexFileIds;   // texFiles[texIndexArray[j]].fileId by position j; 0 for a page entry or an index past texFileArray
  uint32_t hiResMaterialFileId = 0;        // hiResMaterial.materialFile
  bool hasUvData = false; TerrainUvData uvData;   // as stored
  float Terrain::swapDistance = 0;         // trn.swapDistance as stored
  uint64_t PropInstance::animSequence = 0; uint8_t PropInstance::color[4] = {0, 0, 0, 0};
  std::vector<castlemist::model::Extractor::MapEnvWater> Environment::water;   // dataGlobal presets; light.presetIndex picks one
  std::vector<castlemist::model::Extractor::MapEnvWater> EnvSources::water;
  ```
- Warnings:
  - The uvData line becomes `"terrain materials: N chunks have a uvData (kept as stored in TerrainMaterial::uvData; not animated: which ground UV it moves is per-AMAT, note §1.5-1.6)"`.
  - `build_environment` adds `"environment: K dataGlobal water presets kept as stored; the view pairs water preset k with the chosen lighting preset k (presetIndex P), UNPROVEN (note §6)"`. With no presets it adds `"environment: no dataGlobal water presets; the view draws flat water"`.
  - `build_water`'s env line becomes `"env: N water presets in all env blocks; the dataGlobal ones are in environment.water, the per-zone ones are not read"`.

- [ ] **Step 1: Write failing tests** (reuse `make_prop` and the hand-built pattern of `world.terrain_material_index_out_of_range`, test_world.cpp:268):
  - `world.props_keep_anim_sequence_and_color`: a `propAnimArray` prop with `animSequence 463` and `color {0x4A,0x81,0x64,0x80}` → the PropInstance has both.
  - `world.terrain_materials_keep_positions_and_uvdata`: `texIndices {0,1,2,3,4}`, where entry 4 is a `blend` page with fileId 0, plus `hiMaterialFileId 9` and `hasUvData` with `uv.translation (0, 0.2)`. Expect `texIndexFileIds == {11,12,13,14,0}`, `hiResMaterialFileId == 9`, `uvData.translation[1] ≈ 0.2`, and a warning containing "note §1.5".
  - `world.terrain_keeps_swap_distance`: `MapTerrain.swapDistance = 39936` → `terrain.swapDistance == 39936`.
  - `world.environment_keeps_water_presets`: `EnvSources` with `light.present`, `light.presetIndex 1` and three presets (the first with `materialFileId 187551`) → `environment.water.size() == 3`, `light.presetIndex == 1`, and a warning containing "note §6".
- [ ] **Step 2: Run** `cm_test_world.exe world.` → FAIL.
- [ ] **Step 3: Implement.** `attach_environment` reads `ex.parseMapEnvWater()` in its own `try`; a failure becomes `"environment: water presets not read: <what>"`.
- [ ] **Step 4: Run** `cm_test_world.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(world): keep terrain material positions, uvData, swapDistance, prop animSequence/color and env water presets`.

### Task 6: terrain_blend, page_uv, ground_uv, resolve_prop_clip

**Files:**
- Create: `include/castlemist/world/terrain_blend.h`, `src/world/terrain_blend.cpp`, `include/castlemist/world/anim.h`, `src/world/anim.cpp`
- Test: Create `tests/test_world_materials.cpp`; add it to the `world` SOURCES

**Interfaces:**
- Produces (namespace `castlemist::world`):
  ```cpp
  /// UNPROVEN: T3D's hard-coded guess (TerrainRenderer.ts:405-406), note §1.6. The render and export default.
  inline constexpr float kGroundRepeatsPerChunk = 8.0f;
  struct BlendTexel { float rgba[4]; };                  // linear 0..1, as decoded
  struct TerrainBlendInput {
      BlendTexel picker, modx;                           // page samples at the chunk's page UV
      BlendTexel ground[4];                              // color, colorb, colorc, colord at the ground UV
      bool present[4] = {true, true, true, true};        // false: that slot binds no texture (fileId 0)
  };
  void terrain_blend(const TerrainBlendInput&, float albedoOut[3], float weightsOut[4]);   // weightsOut[i] = weight of ground[i], sum 1
  void page_uv(const TerrainMaterial&, float s, float t, float uv[2]);   // s from the west edge, t from the north edge, in [0,1]
  void ground_uv(float s, float t, float repeats, float uv[2]);
  int resolve_prop_clip(const std::vector<uint64_t>& clipTokens, uint64_t animSequence);   // index or -1
  ```
- The algorithm is note §1.2 to the letter, and the HLSL in Task 9 is a line-for-line copy of it.
  - Picker R weights `colord` (ground[3]), G weights `colorc` (ground[2]), B weights `colorb` (ground[1]), A weights `color` (ground[0]).
  - castlemist's rule for a slot that binds no texture: its weight is 0. If every weight is 0 after that, the note's `1/255` goes to `color`, or to the first present slot when `color` is absent.
  - `h = ground.a + 0.01` and `k = w·h / dot(w,h)`. Then the luminance, saturation and brightness steps exactly as the note gives them.
  - No clamp is applied.
- `page_uv = pickerOffset + pickerScale·(s, t)` with no edge inset (T3D's 1/1024 inset is UNPROVEN, note §1.6). `ground_uv = (s·repeats, t·repeats)`.
- `resolve_prop_clip`: a token of 0 gives −1. Otherwise return the first index whose token equals `animSequence`, or −1. There is no fallback (spec, "Clips resolve exactly or not at all").

- [ ] **Step 1: Write failing tests** (spec values; modx `(0.5, 0.5, 0, 0)` unless stated):
  - `terrain_blend.one_hot_pickers_select_each_ground`: picker (1,0,0,0) gives albedo = ground[3].rgb and weights {0,0,0,1}. (0,1,0,0) gives ground[2], (0,0,1,0) gives ground[1], (0,0,0,1) gives ground[0].
  - `terrain_blend.all_zero_picker_is_color`: → ground[0].rgb, weights {1,0,0,0}.
  - `terrain_blend.alpha_biases_weight`: picker (0,0,0.5,0.5), ground[0].a = 0, ground[1].a = 1 → `weights[0]/weights[1] ≈ 0.01/1.01` (±1e-5), weights[2] = weights[3] = 0, sum 1.
  - `terrain_blend.modx_half_is_neutral`: color = (0.2, 0.4, 0.6) alone → albedo (0.2, 0.4, 0.6).
  - `terrain_blend.modx_g_zero_is_grey_at_luminance`: the same with modx (0.5, 0) → every channel 0.362 (= 0.30·0.2 + 0.59·0.4 + 0.11·0.6).
  - `terrain_blend.absent_slot_gets_no_weight`: picker (1,0,0,0) with `present[3] = false` → ground[0] alone, every output finite.
  - `terrain_blend.page_uv_covers_sub_rect` (spec): offset (0.25, 0.75), scale 0.25: (0,0) → (0.25, 0.75), (1,1) → (0.5, 1.0), (0.5,0.5) → (0.375, 0.875).
  - `terrain_blend.ground_uv_repeats`: (0.5, 0.25, 8) → (4, 2).
  - `anim.resolve_exact_hit`: tokens {192720390330, 463, 6351}, 463 → 1. `anim.resolve_miss`: 464399 → −1. `anim.resolve_zero_token`: 0 → −1, even when a clip token is 0.
- [ ] **Step 2: Run** `cm_test_world.exe terrain_blend.` and `cm_test_world.exe anim.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_world.exe` → PASS.
- [ ] **Step 5: Commit** `feat(world): terrain_blend (note §1.2), page/ground UV rules, resolve_prop_clip`.

### Task 7: CPU prep on the workers (amendment 2): MatVtx, SkinVtx, per-submesh ranges

**Files:**
- Modify: `src/render/detail/mesh_bake.h`, `src/render/mesh_bake.cpp` (`flatten` :183-219); `src/render/world_props.h`, `src/render/world_props.cpp` (`prepare_world_model` :12); `src/render/world_draw.h`, `src/render/world_draw.cpp` (`MeshGPU`, `upload_mesh` :41, `InstancedBatch::draw` :174-177 strides); `include/castlemist/render/world_view.h`; `src/render/world_view.cpp` (`add_models` :570-591); `src/ui/detail/map_window_loader.h`; `src/ui/map_window_loader.cpp` (worker :160-180); `src/ui/map_window.cpp` (`MapWin::models`, `on_models` :509-516, `recover_device` :375)
- Create: `include/castlemist/render/world_prep.h`
- Test: `tests/test_render_world_view.cpp`

**Interfaces:**
- Consumes: `ModelLoadOptions`, `animClipTokens` (Task 3), `resolve_prop_clip`, `PropInstance::animSequence` (Tasks 5 and 6).
- Produces:
  ```cpp
  // detail/mesh_bake.h
  struct SubRange { uint32_t first = 0, count = 0, material = 0; };   // inside one LOD range; material = ModelMeshCPU::materialIndex
  struct MatVtx { float px, py, pz; uint8_t nx, ny, nz, ao; float u0, v0, u1, v1; };   // static_assert 32
  struct SkinVtx { uint8_t bone[4]; uint8_t weight[4]; };                             // static_assert 8
  void flatten(const ModelPreview&, std::vector<MapVtx>&, std::vector<uint32_t>&,
               std::vector<std::pair<uint32_t, uint32_t>>&, std::vector<std::vector<SubRange>>* subRanges);   // the 4-arg form stays for map_fly
  // world_props.h: PreparedModel gains
  std::vector<MatVtx> matVerts; std::vector<SkinVtx> skinVerts; std::vector<std::vector<SubRange>> subRanges;   // per LOD
  PreparedModel prepare_world_model(const ModelPreview& m, bool withSkin = false);
  // world_draw.h: MeshGPU gains
  UINT stride = sizeof(MapVtx); std::vector<std::vector<SubRange>> subRanges; Microsoft::WRL::ComPtr<ID3D11Buffer> vbSkin;
  // include/castlemist/render/world_prep.h
  struct PreparedModel;   // src/render/world_props.h
  struct WorldModelLoad {
      uint32_t model = 0;                               // index into WorldScene::models
      std::shared_ptr<const ModelPreview> preview;      // picking, materials, joints, clips; null = failed
      std::shared_ptr<const PreparedModel> prep;        // null or !ok = failed
  };
  std::shared_ptr<const PreparedModel> prepare_world_model_shared(const ModelPreview& m, bool withSkin);   // no D3D; any thread
  // world_view.h
  void WorldView::add_models(const std::vector<WorldModelLoad>& batch);   // GPU buffers and instance placement only
  void WorldView::add_models(const std::vector<std::pair<uint32_t, std::shared_ptr<const ModelPreview>>>& batch);
      // kept: prepares across cores on the calling thread, then forwards (tests, tools)
  // map_window_loader.h
  struct MapWinModelQueue { /* ... */ std::vector<std::vector<uint64_t>> sequencesOf; };   // per model: distinct non-zero animSequence, filled in the constructor
  struct MapWinModelsMsg { uint64_t gen = 0; std::vector<castlemist::render::WorldModelLoad> batch; bool workerDone = false; };
  ```
- Rules:
  - `prepare_world_model` flattens as today and bakes AO on the `MapVtx`. It then builds `matVerts` in the same vertex order: `flatten` appends each mesh's vertices in mesh order (mesh_bake.cpp:192-202). `u0, v0` come from `GVertex u, v` and `u1, v1` from `uv1[0]`. `verts` is cleared afterwards. `upload_mesh` uploads `matVerts` (stride 32) when non-empty, else `verts` (stride 16, for collision and water).
  - `subRanges[lod]` lists, mesh by mesh, the indices that mesh appended in that LOD (mesh_bake.cpp:204-217), skipping empty ones.
  - With `withSkin`: `bone = bidx`, `weight = round(bwt·255)`. A mesh with `!hasSkin` gets weights {0,0,0,0} (identity in Task 12's VS). A model with more than 256 joints gets no `skinVerts`.
  - Worker:
    - Call `load_model_by_fileid(*dat, fileId, ModelLoadOptions{false, false})`.
    - Set `withSkin` when any `resolve_prop_clip(m->animClipTokens, s) >= 0` over `queue->sequencesOf[mi]`.
    - Without `withSkin`, clear `animClips` and `animClipTokens`.
    - Stop clearing `m->textures`: it is header-only now, and the materials index it.
    - Push `{mi, m, prepare_world_model_shared(*m, withSkin)}`.
  - `MapWin::models` becomes `std::vector<castlemist::render::WorldModelLoad>` and `recover_device` re-adds it.

- [ ] **Step 1: Write failing tests**
  - `world_view.flatten_subranges_cover_each_lod_once` (spec): a model with `cube_model`'s mesh (material 0) and a second mesh (material 1) that has fewer LODs. For every LOD: `subRanges[lod][0].first == lodRanges[lod].first`, the ranges are consecutive (`first[i+1] == first[i] + count[i]`), the counts sum to `lodRanges[lod].second`, and the materials are {0, 1}.
  - `world_view.prepare_matvtx_carries_uvs`: `sizeof(MatVtx) == 32` and `sizeof(SkinVtx) == 8`. A vertex with `u, v = (0.25, 0.5)` and `uv1[0] = (2, 3)` comes out with those values in `matVerts`, and `verts` is empty.
  - `world_view.prepare_skin_only_when_asked`: `withSkin` false gives empty `skinVerts`. True gives `skinVerts.size() == matVerts.size()`; weights (0.5, 0.25, 0.25, 0) become {128, 64, 64, 0}; a `hasSkin == false` mesh gets {0,0,0,0}.
  - `world_view.prepared_add_matches_preview_add` (hidden window as in :233, SKIP without a device): adding `{{0, cube}}` and adding `{WorldModelLoad{0, cube, prepare_world_model_shared(*cube, false)}}` give the same `visible`, `draws` and `ktris` after `render`.
- [ ] **Step 2: Run** `cm_test_render.exe world_view.` → FAIL (does not compile).
- [ ] **Step 3: Implement** the render changes, then the loader and window changes. The ui builds against the new message type.
- [ ] **Step 4: Run** `cm_test_render.exe` and `cm_test_ui.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `perf(render): prepare world models on the load workers; add_models only uploads; MatVtx and per-submesh ranges`.

### Task 8: TextureCache and texture streaming

**Files:**
- Create: `src/render/world_materials.h`, `src/render/world_materials.cpp`
- Modify: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`, `include/castlemist/render/world_prep.h`, `src/ui/detail/map_window_loader.h`, `src/ui/map_window_loader.cpp`, `src/ui/detail/app_state.h` (:373), `src/ui/map_window.cpp` (handlers, the `WM_DESTROY` drain range :682-684, `mapwin_free_payload`)
- Test: Create `tests/test_render_world_materials.cpp`; add it to the `render` SOURCES. Also `tests/test_render_world_view.cpp` and `tests/test_ui_map_window.cpp`

**Interfaces:**
- Consumes: `CappedTexture`, `decode_texture_capped` (Task 4); the `WorldScene` fields (Task 5).
- Produces:
  ```cpp
  // world_view.h
  struct WorldMaterialSettings {        // every UNPROVEN value is named in WorldView::warnings() (Task 9)
      float groundRepeatsPerChunk = castlemist::world::kGroundRepeatsPerChunk;   // UNPROVEN, note §1.6
      float uvScrollUnitSeconds = 1;    // UNPROVEN: offset = frac(scroll * t / unit) on uvIndex, note §4
      bool terrainNormalMaps = true;    // UNPROVEN sign: tangent +X east, bitangent = increasing t (south), note §1.2, §2
      float terrainSpecPower = 32;      // castlemist's choice: forward stand-in for the light buffer's L.a, note §1.4
      int maxTextureSize = 512;         // memory bound (spec)
      int waterBumpTextureIndex = 1;    // UNPROVEN beyond AMAT 187551 (ssBump0 = textureIndex 1), note §6
      float waterTileUnit = 1000;       // UNPROVEN: units of bumpTile/bumpSpeed/bumpAngle, note §6
      float waterSpecPower = 64;        // castlemist's choice
      bool srgbColourTextures = true;   // castlemist's choice: colour textures decode sRGB to linear; the shaders gamma-encode (world_view_shaders.h:53)
  };
  void WorldView::add_textures(const std::vector<std::shared_ptr<const CappedTexture>>& batch);   // UI thread
  // WorldViewStats gains
  int textures = 0, texturesFailed = 0; float textureMB = 0;
  // world_materials.h
  class TextureCache {
  public:
      enum class State { Unknown, Ready, Failed };
      void add(ID3D11Device* dev, const CappedTexture& t);   // once per fileId; R8G8B8A8_TYPELESS, every mip, a UNORM and a UNORM_SRGB view; empty mips -> Failed
      ID3D11ShaderResourceView* srv(uint32_t fileId, bool srgb) const;   // null unless Ready
      State state(uint32_t fileId) const;
      int ready() const; int failed() const; size_t bytes() const;       // bytes = sum over mips of w*h*4
      void clear();
  };
  // world_prep.h
  std::vector<uint32_t> world_texture_ids(const castlemist::world::WorldScene& s, const WorldMaterialSettings& cfg);
  // map_window_loader.h
  struct MapWinTextureClaims { bool claim(uint32_t fileId); /* false for 0 or already taken */ private: std::mutex m_; std::unordered_set<uint32_t> taken_; };
  // MapWinModelsMsg gains: std::vector<std::shared_ptr<const CappedTexture>> textures;
  struct MapWinTexturesMsg { uint64_t gen = 0; std::vector<std::shared_ptr<const CappedTexture>> textures; bool done = false; };
  constexpr UINT WM_APP_MAPWIN_TEXTURES = WM_APP + 23;   // app_state.h
  void mapwin_start_terrain_textures(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                                     std::shared_ptr<const castlemist::world::WorldScene> scene, std::shared_ptr<MapWinTextureClaims> claims);
  int mapwin_start_models(HWND, uint64_t, std::shared_ptr<Gw2Dat>, std::shared_ptr<MapWinModelQueue>, std::shared_ptr<MapWinTextureClaims>);
  ```
- Rules:
  - `world_texture_ids` returns distinct non-zero ids in this order: per chunk, in index order, `textureFileIds[0..3]`, `normalFileIds[0..3]`, `pickerFileId`, `picker2FileId`. Then the water bump `environment.water[light.presetIndex].textureFileIds[cfg.waterBumpTextureIndex]` when it exists.
  - Model workers: for every material with `diffuseTex >= 0` and `diffuseUv <= 1` whose fileId `claim()`s, call `decode_texture_capped(*dat, id, WorldMaterialSettings{}.maxTextureSize, t)` and attach the result to the batch. A batch posts at 32 models, every 250 ms, or once its textures reach 16 MB (`bytes()`), which keeps each UI-thread add under the 50 ms target.
  - Terrain thread: posts the same way, with `done` on its last post. It starts before `mapwin_start_models`; copy the dat `shared_ptr` before the `std::move` at map_window.cpp:478.
  - UI thread: `on_models` calls `add_textures(msg.textures)` then `add_models(msg.batch)`, timed together in `addMs`.
  - Device recovery re-adds models but not textures. Clear the cache and flash `"Textures are not restored after a device reset; reopen the window to see them."`.

- [ ] **Step 1: Write failing tests**
  - `world_materials.texture_cache_counts_bytes_and_failures` (WARP device from `D3D11CreateDevice`, SKIP when none): fileId 1 at 8×8 with mips {8×8, 4×4} gives `ready() == 1`, `bytes() == 320`, and non-null `srv(1,true)` and `srv(1,false)` that differ. Adding fileId 1 again keeps `ready() == 1`. fileId 2 with empty mips gives `failed() == 1`, `State::Failed` and a null `srv`. `clear()` resets all counts to 0.
  - `world_materials.texture_ids_cover_terrain_and_water`: chunk 0 has textures {11,12,0,0}, normals {21,0,0,0}, pickers 31/32. Chunk 1 has textures {11,13,0,0}, normals {21,0,0,0}, picker 0 (solid), picker2 33. `environment.water[1].textureFileIds = {0, 186359}` and `presetIndex 1`. Expect `{11,12,21,31,32,13,33,186359}`.
  - `world_view.recreate_drops_textures` (Review Focus): `create`, `set_world`, `add_textures`, `add_models`, `create` again, `set_world`, `add_models`. Then `render` succeeds and `stats().textures == 0`.
  - `map_window.texture_claims_once`: `claim(5)` true, `claim(5)` false, `claim(0)` false, `claim(6)` true.
- [ ] **Step 2: Run** `cm_test_render.exe world_materials.` and `cm_test_ui.exe map_window.texture` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_render.exe`, `cm_test_ui.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(render): TextureCache keyed by fileId; textures decoded once on the workers and uploaded with each batch`.

### Task 9: Terrain material pass

**Files:**
- Modify: `include/castlemist/render/world_view.h`; `src/render/world_view.cpp` (`upload_chunk` :300-350, `draw` :391-423, `set_world`); `src/render/world_view_shaders.h` (`kTerrain` reads the 20-byte vertex; new `kTerrainMat`); `src/render/world_materials.h/.cpp`
- Test: `tests/test_render_world_materials.cpp`

**Interfaces:**
- Consumes: `TextureCache` (Task 8); `terrain_blend`, `page_uv`, `ground_uv` (Task 6, which defines the rule the HLSL copies).
- Produces:
  ```cpp
  enum class WorldShading { Geometry, Materials };   // Materials after create()
  void WorldView::set_shading(WorldShading s); WorldShading WorldView::shading() const;
  void WorldView::set_material_settings(const WorldMaterialSettings& s); const WorldMaterialSettings& WorldView::material_settings() const;
  /// Verification hook: chunk @p chunk's §1.2 albedo only (no light, fog or gamma), linear, into a size x size
  /// RGBA8 UNORM offscreen target drawn orthographically top down; row 0 = north, column 0 = west.
  /// False when the chunk would not draw textured.
  bool WorldView::capture_chunk_albedo(size_t chunk, int size, std::vector<uint8_t>& rgba);
  std::vector<std::string> WorldView::warnings() const;   // material_setting_warnings(settings) + count lines (Tasks 10-13 add)
  std::vector<std::string> material_setting_warnings(const WorldMaterialSettings& s);   // world_materials.h
  // WorldViewStats gains
  int chunksUntextured = 0;   // drawn in the AO look: unresolved, no pages, or a colour texture pending/failed
  ```
- Rules:
  - The terrain vertex is a local `TerrainVtx` in world_view.cpp: `MapVtx` fields plus `uint16_t s, t`, static_assert 20. s is `i/(n−1)` from the west and t is `j/(n−1)` from the north. Both terrain pipelines read this vertex buffer; `kTerrain` ignores `s, t`.
  - The VS computes the page and ground UVs from a per-chunk cbuffer holding `pickerOffset`, `pickerScale`, `groundRepeatsPerChunk` and the present mask.
  - The PS follows §1.2 as Task 6 transcribes it, then the note's lighting in forward form:
    - `L.rgb` is the AO pass's hemisphere plus sun, from the `WorldCB` values (world_view.cpp:368-384).
    - `L.a = pow(saturate(N·H), terrainSpecPower)`.
    - `rgb = alb·L.rgb + nrm.a·0.854902·sunCol·L.a + saturate(2·modx.b − 1)·0.239216·sunCol·(1 − saturate(N·V))^8`. `shSunColor.w` is taken as 1 (UNPROVEN), then fog and gamma as `kTerrain`.
    - With `terrainNormalMaps`, N = TBN·(nrm.xyz·2 − 1), with T = +X, B = −Y and the normal from `chunk_normals`. A missing normal slot is flat.
  - Sampling: pages use clamp and ground uses wrap, both trilinear. Colour textures use the sRGB view when `srgbColourTextures`; normals and pages use UNORM.
  - A solid page is a per-chunk 1×1 UNORM texture made in `set_world` from `pickerSolid`/`picker2Solid` in stored byte order (UNPROVEN, note §1.6).
  - A chunk draws textured when all of these hold: `Materials`, `material.resolved`, `pickerScale > 0`, both pages available (Ready or solid), and every non-zero colour slot Ready. Otherwise it takes the `kTerrain` AO path and is counted in `chunksUntextured`.
  - `material_setting_warnings` writes one line per setting, with its value, "UNPROVEN" or "castlemist's choice", and the note section. It adds fixed lines for "all animated props start at t = 0 and loop (note §5)", "pickerSolid byte order (note §1.6)", "water preset k pairs with lighting preset k (note §6)" and "terrain uvData recorded, not animated (note §1.5)".

- [ ] **Step 1: Write failing tests**
  - `world_materials.chunk_albedo_matches_cpu_bake` (spec), set up so that one texel maps to one pixel and bilinear filtering is exact:
    - Scene: one chunk of 17×17 flat samples, rect (0,0,1600,1600), material resolved, `textureFileIds {101,102,103,104}`, normals 0, `pickerFileId 201`, `picker2FileId 202`, `pickerOffset (0.25, 0.75)`, `pickerScale 0.25`.
    - Textures: 101-104 are 1×1 with distinct RGB and alpha 0, 64, 128, 255. 201 is 64×64 with texel (x, y) = (4x, 4y, 255 − 4x, 2(x+y)). 202 is 64×64 with (128 + x, 128 − y, 0, 0). One mip each.
    - Run `capture_chunk_albedo(0, 16, out)`; it must return true.
    - CPU side: pixel (x, y) reads page texel (16 + x, 48 + y). Ground = `srgb_to_linear(rgb)` (test-local helper) with alpha raw. Run `terrain_blend` and clamp to [0,1].
    - Every channel must be within 2/255.
  - `world_materials.unresolved_chunk_draws_ao_look` (Review Focus): `resolved = false` → `render` succeeds, `stats().chunksUntextured == 1`, capture returns false.
  - `world_materials.solid_page_is_one_texel` (Review Focus): picker 0 with `pickerSolid {0,0,0,0}`, picker2 0 with `picker2Solid {128,128,0,0}` → the capture equals ground[0] within 2/255.
  - `world_materials.setting_warnings_name_note_sections`: lines contain `groundRepeatsPerChunk = 8` with `§1.6`, `uvScrollUnitSeconds` with `§4`, `terrainNormalMaps` with `UNPROVEN`, `pickerSolid` with `§1.6`, `t = 0` with `§5`, `waterTileUnit` with `§6`, and `water preset` with `§6`.
- [ ] **Step 2: Run** `cm_test_render.exe world_materials.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_render.exe` → PASS, including the existing `world_view.renders_two_terrain_chunks`, whose chunks are unresolved.
- [ ] **Step 5: Commit** `feat(render): terrain material pass: blend pages and ground textures per note §1.2, forward lighting`.

### Task 10: Prop material pass

**Files:**
- Modify: `CMakeLists.txt` (render DEPS gain `castlemist::exportgltf`, at :154); `src/render/world_draw.h/.cpp` (a material draw beside `InstancedBatch::draw`, :134); `src/render/world_view.cpp`; `src/render/world_view_shaders.h` (`kPropMat`); `src/render/world_materials.h/.cpp`
- Test: `tests/test_render_world_materials.cpp`

**Interfaces:**
- Consumes: `exportgltf::profile_for` (shader_profiles.h:132), `alpha_tested` (:126), `decode_blend` (blend_mode.h:33); `SubRange`, `MatVtx` (Task 7); `TextureCache` (Task 8).
- Produces:
  ```cpp
  enum class WorldAlpha : uint8_t { Opaque, Clip, Blend, Additive };
  WorldAlpha world_alpha_mode(const ModelMaterialCPU& mat, bool diffuseHasCutout, bool diffuseLooksLikeEffect);
  struct PropMaterial { uint32_t diffuseFileId = 0; uint8_t uvSet = 0; bool textured = false; int scroll = -1; };   // scroll: Task 11
  std::vector<PropMaterial> prop_materials(const ModelPreview& m);   // per ModelMaterialCPU; textured = diffuseTex >= 0 && diffuseUv <= 1
  std::vector<size_t> blend_back_to_front(const std::vector<float>& nearestDistance);   // farthest first; ties by index
  // WorldViewStats gains
  int materialsUntextured = 0;   // sub-range draws this frame in the AO look (no diffuse, UV2+, pending or failed texture)
  ```
- Rules:
  - `world_alpha_mode`:
    - `isEffect = mat.isEffect || (!mat.hasRenderState && diffuseLooksLikeEffect)`. This reproduces model_preview.cpp:187-194 for header-only loads.
    - `b = decode_blend(mat.renderState, mat.hasRenderState, isEffect, alpha_tested(profile_for(mat, mat.renderState)))`.
    - Without a render state: `diffuseHasCutout` gives Clip, then `isEffect` gives Additive (decode_blend's preset), else Opaque. This is the export's order at material_export.cpp:164-170.
    - With one, use the preset (Custom resolves to `nearest`): Opaque gives Opaque; Cutout gives Clip; Additive and SoftAdditive give Additive; everything else gives Blend.
  - Pass 1 draws Opaque and Clip. Clip discards where the diffuse alpha is below 0.25.
  - Pass 2 draws Blend (SrcAlpha / InvSrcAlpha) and Additive (SrcAlpha / One) with depth writes off. Buckets are ordered by `blend_back_to_front` on each bucket's nearest instance distance; there is no per-instance sort, which is an approximation and is documented.
  - For each `(model, lod)` bucket there is one instance upload (unchanged). Then each `SubRange` gets one per-material cbuffer write (uvSet, uvOffset, alpha mode, textured flag), one SRV bind and one `DrawIndexedInstanced(count, n, first, 0, 0)`.
  - Shading: `albedo × AO × (hemi + sun)` with the budget of `kInstanced` (world_view_shaders.h:104-113). Albedo is the diffuse rgb, from the sRGB view when `srgbColourTextures`. `ModelMaterialCPU::tint` is not applied, because it is model_preview's guess.
  - Untextured sub-ranges, and every draw under `Geometry`, use `kPropTint` in the AO look.
  - The model's material state is built in `add_models` from `prop_materials(*preview)`. The texture's `hasCutout` and `looksLikeEffect` are read when it is Ready, so the alpha mode is resolved at draw time.

- [ ] **Step 1: Write failing tests**
  - `world_materials.alpha_mode_rules`:
    - No render state: a cutout gives Clip; `isEffect` gives Additive; `looksLikeEffect` gives Additive; a plain material gives Opaque.
    - `hasRenderState` with `renderState = (5ull<<12)|(6ull<<16)` gives Blend.
    - `hasRenderState`, `renderState 0`, `materialFile 512093` (a clipping profile, shader_profiles.cpp:237-238) gives Clip.
  - `world_materials.prop_materials_pick_uv_and_fileid`: diffuse fileId 77 on UV0 gives textured with uvSet 0; UV1 gives textured with uvSet 1; UV2 gives untextured; `diffuseTex −1` gives untextured.
  - `world_materials.blend_back_to_front`: {10, 30, 20} gives {1, 2, 0}; {5, 5} gives {0, 1}.
  - `world_materials.textured_props_draw_per_submesh` (GPU; Review Focus): a cube split into two meshes with two materials and diffuse fileIds 501 and 502. After `add_models` and `render`, draws equal two per visible bucket and `materialsUntextured == 2`. After `add_textures` for 501 and 502 and another `render`, draws are unchanged and `materialsUntextured == 0`.
  - `world_materials.geometry_shading_is_subproject2_look` (GPU): view A has textures and `set_shading(Geometry)`; view B has none. Their screenshots are byte-identical. A under `Materials` differs from A under `Geometry`.
- [ ] **Step 2: Run** `cm_test_render.exe world_materials.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_render.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(render): prop material pass: per-submesh draws, diffuse textures, opaque/clip then sorted blend`.

### Task 11: Time, Motion and UV scroll

**Files:**
- Modify: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`, `src/render/world_materials.h/.cpp`, `src/render/world_view_shaders.h`
- Test: `tests/test_render_world_materials.cpp`

**Interfaces:**
- Consumes: `ModelMaterialCPU::texTransforms` (Task 3); `PropMaterial` (Task 10).
- Produces:
  ```cpp
  void WorldView::set_motion(bool on); bool WorldView::motion() const;   // on after create()
  void WorldView::set_time(double seconds);                              // the window drives the clock
  double WorldView::time() const;                                        // effective: motion ? seconds : 0
  bool WorldView::animating() const;   // Materials && motion && (a playing instance, a scrolling material or a water preset is loaded)
  bool tex_transform_animates(const ModelTexTransformCPU& t);   // columns == rows == count == 1, scrollFreq == scale == scaleFreq == (0,0), rotate == rotate2 == 0
  void uv_scroll_offset(const ModelTexTransformCPU& t, double seconds, float unitSeconds, float out[2]);   // frac(scroll * seconds / unitSeconds), frac(x) = x - floor(x), in double
  // WorldViewStats gains
  int scrollingMaterials = 0;
  ```
- Rules:
  - `PropMaterial::scroll` is the first `texTransforms` entry whose `uvIndex == uvSet` and that animates.
  - Entries that do not animate are counted in the warning line `"N texTransforms entries are not scroll-only (flipbook, scale or rotate); drawn still (note §4)"`.
  - The offset goes into the per-material cbuffer and is added to the sampled UV set.
  - Geometry shading ignores scroll.

- [ ] **Step 1: Write failing tests**
  - `world_materials.scroll_offset_is_frac` (spec): scroll (0, 0.2) at t 2.5 gives (0, 0.5); at t 6 gives (0, 0.2) (±1e-5). (0, −0.1) at t 1 gives (0, 0.9). A 2 s unit with (0, 0.2) at t 2.5 gives (0, 0.25).
  - `world_materials.only_scroll_only_entries_animate`: the census entry (uvIndex 1, columns = rows = count = 1, scroll (0, 0.2)) animates. `columns 2`, `count 0`, `scale (1,1)` and `rotate 0.1` each do not.
  - `world_materials.scroll_entry_matches_diffuse_uv`: `diffuseUv 0` with `texTransforms [{uvIndex 1}, {uvIndex 0}]` gives `scroll == 1` (the shape of model 1126949).
  - `world_view.motion_off_freezes_time`: `set_motion(false); set_time(2.5)` gives `time() == 0`; `set_motion(true)` gives 2.5.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `cm_test_render.exe` → PASS.
- [ ] **Step 5: Commit** `feat(render): material clock, Motion toggle and texTransforms UV scroll`.

### Task 12: Skinned prop pass

**Files:**
- Modify: `include/castlemist/render/world_view.h`, `src/render/world_view.cpp`, `src/render/world_draw.h/.cpp` (skinned buckets), `src/render/world_materials.h/.cpp`, `src/render/world_view_shaders.h` (`kPropSkinned`)
- Test: `tests/test_render_world_materials.cpp`

**Interfaces:**
- Consumes: `granny::composePose`, `skinMatrix`, `trackIndexByName` (`include/castlemist/native/granny_pose.hpp:98,153,88`); `resolve_prop_clip` (Task 6); `SkinVtx` (Task 7).
- Produces:
  ```cpp
  struct PoseRig {   // per (model, clip), built in add_models from the kept preview
      std::vector<castlemist::granny::PoseBone> bones;   // {name.c_str(), parent, localPos, localQuat, localScale, invWorld} of each ModelJoint, as skeleton.cpp:23-29
      const castlemist::granny::Anim* clip = nullptr;
      std::unordered_map<std::string, int> trackByName;
  };
  double clip_time(double seconds, float duration);   // fmod into [0, duration); duration <= 1e-6 -> 0
  void pose_palette(const PoseRig& rig, double seconds, std::vector<float>& palette);   // 16 floats per bone: skinMatrix (row-major, row-vector)
  bool WorldView::focus_animated(WorldCamera& cam) const;                         // frames the playing instance nearest the current camera from 4x its radius; false when none
  bool WorldView::animated_screen_rect(const WorldCamera& cam, int rect[4]) const;   // pixel rect of the projected sphere of the instance focus_animated picks
  // WorldViewStats gains
  int animated = 0, unresolvedClips = 0;   // playing instances / animSequence placements left in bind pose
  ```
- Rules:
  - In `add_models`, each instance of model m with `animSequence != 0` gets `clip = resolve_prop_clip(preview->animClipTokens, seq)`.
  - It counts as unresolved, stays static and is drawn in bind pose when the clip is −1, or when the model has no `skinVerts` (more than 256 joints, or the worker saw no resolvable sequence).
  - Resolved instances go into skinned buckets keyed `(model, clip, lod)`.
  - Each frame, one `pose_palette` runs at `time()` for every `(model, clip)` with a visible bucket. Its instances share it, because all of them start at t = 0 and loop, using the wrap at gw2bgfx_view.cpp:1263-1267.
  - Every palette is packed into one dynamic `StructuredBuffer<float4x4>` uploaded once per frame. A cbuffer holds each bucket's base index.
  - VS: a weight sum below 1e-3 gives identity (unskinned meshes). Otherwise apply `Σ wᵢ·(p × P[base + boneᵢ])` with renormalised weights, then the instance's world columns. Rigid single-bone meshes are weight-1 vertices (model_preview.cpp:329-333).
  - Skinning applies only under `Materials`; `Geometry` draws everything static. Picking stays in bind pose, and collision does not move (both out of scope).
  - Warning: `"N animated placements name no clip of their model; drawn in bind pose (fallback rule UNPROVEN, note §5)"`.

- [ ] **Step 1: Write failing tests**
  - `world_materials.pose_palette_bind_is_identity`: two joints (the child at (0,0,10) with `invWorld` translation −10) and a valid `Anim` of duration 1 with no tracks → the palette is identity for both bones (±1e-5).
  - `world_materials.pose_palette_zero_duration_no_nan` (Review Focus): duration 0 at 7.3 s → every float is finite.
  - `world_materials.clip_time_wraps`: (2.5, 1) → 0.5; (3, 0) → 0; (0, 2) → 0.
  - `world_materials.animated_instances_counted` (GPU; Review Focus): model 0 has 2 joints, a rigid-bound mesh, `animClips {valid, duration 1}` and `animClipTokens {463}`, prepared `withSkin`. Its three props have `animSequence` 463, 6351 and 0. After `render`: `animated == 1`, `unresolvedClips == 1`, and `warnings()` holds a line with "1 animated placement" and "note §5". The same model with 300 joints gives `animated == 0` and `unresolvedClips == 2`.
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `cm_test_render.exe` → PASS.
- [ ] **Step 5: Commit** `feat(render): animated props play their named clip: one CPU pose per (model, clip), instanced skinning`.

### Task 13: Water pass

**Files:**
- Modify: `src/render/world_view.cpp` (`build_water` :261, `set_world` :559-567), `src/render/world_view_shaders.h` (`kWater`), `src/render/world_materials.h/.cpp`
- Test: `tests/test_render_world_materials.cpp`

**Interfaces:**
- Consumes: `Environment::water`, `MapEnvLight::presetIndex` (Task 5); `TextureCache` (Task 8).
- Produces:
  ```cpp
  void water_colour(const uint8_t stored[4], float rgb[3]);   // bytes 0,1,2 as R,G,B / 255: the order parseMapEnv reads env colours (gw2model.hpp:1300); UNPROVEN, note §6
  void water_bump_uv(float x, float y, float tile, float speed, float angle, double seconds, float tileUnit, float uv[2]);
      // uv = R(angle)·(x, y)·tile / tileUnit + (speed·seconds, 0); angle in radians; units UNPROVEN, note §6
  float water_depth_mix(float terrainZ, float planeZ, float depthAttenuation);   // saturate((terrainZ − planeZ) / depthAttenuation); 0 when depthAttenuation <= 0
  // WorldViewStats gains
  bool waterPreset = false;
  ```
- Rules:
  - The preset is `environment.water[light.presetIndex]` when `0 <= presetIndex < size`. Otherwise the view keeps the sub-project 2 flat water and warns `"water: no preset for lighting preset P; flat water (note §6)"`.
  - Height texture: R32F over the terrain sample grid, `(chunksX·(samples−1)+1) × (chunksY·(samples−1)+1)`. Take every k-th sample so neither side exceeds 4096. Texels outside any chunk get `planeZ`. It is built in `set_world` only when the plane exists.
  - PS:
    - The base colour lerps from `surfaceShallowColor` to `surfaceDeepColor` by `water_depth_mix`.
    - Two bump samples from `textureFileIds[waterBumpTextureIndex]` (UNORM, ·2−1, uv from `water_bump_uv` with `bumpTile0/1`, `bumpSpeed0/1`, `bumpAngle0/1`) give `N = normalize(float3((n0.xy + n1.xy)·bumpAmount, −1))`. A missing texture gives a flat N.
    - Fresnel is `pow(1 − saturate(N·V), surfaceFresnel)`.
    - `rgb = base·(hemi + sun) + sunCol·pow(saturate(N·H), waterSpecPower)`, and alpha is `lerp(0.45, 1, fresnel)`.
    - It stays depth-tested, without depth writes, and drawn last.

- [ ] **Step 1: Write failing tests**
  - `world_materials.water_colour_stored_order`: {0xA5, 0xDF, 0x5C, 0} → (165, 223, 92)/255.
  - `world_materials.water_bump_uv_units`: (1000, 0, tile 1, speed 0, angle 0, t 0, unit 1000) → (1, 0). Angle π/2 → (0, 1). Speed 0.5 at t 2 → (2, 0).
  - `world_materials.water_depth_mix`: (15.66, 0, 15.66) → 1; (−5, 0, 15) → 0 (terrain above the plane); attenuation 0 → 0.
  - `world_view.water_preset_used_when_in_range` (GPU): a plane, `water.size() == 2` and `presetIndex 1` give `waterPreset` true. `presetIndex 5` gives false and a warning containing "note §6".
- [ ] **Step 2: Run** → FAIL. **Step 3: Implement.** **Step 4: Run** `cm_test_render.exe` → PASS.
- [ ] **Step 5: Commit** `feat(render): water drawn from the env preset: depth tint, scrolled bump, fresnel, sun specular`.

### Task 14: Map window UI, status and headless hooks

**Files:**
- Modify: `src/ui/map_window.cpp` (toolbar :823-835, `update_status` :389, `headless_capture` :413-442, `maybe_capture` :444, `tick`, `on_world` warnings :471-474); `src/ui/detail/map_window.h`; `src/ui/detail/app_state.h` (after :326); `src/ui/application.cpp` (:788-799); `include/castlemist/core/env.h` (table)
- Test: `tests/test_ui_map_window.cpp`, `tests/test_ui_layout.cpp` (id table :131-141)

**Interfaces:**
- Consumes: `set_shading`, `set_motion`, `set_time`, `animating`, `warnings`, `focus_animated`, `animated_screen_rect`, and the stats fields (Tasks 8-13).
- Produces:
  ```cpp
  constexpr UINT_PTR ID_MW_MATERIALS = 2221, ID_MW_MOTION = 2222;   // app_state.h
  struct MapWinHeadless { /* ... */ bool geometry = false; bool fixedTime = false; double time = 0; bool focusAnimated = false; };
  bool mapwin_shading_is_geometry(const std::string& text);              // "geometry", any case -> true; anything else -> false (materials)
  std::wstring mapwin_material_status(const castlemist::render::WorldViewStats& st);   // L"tex %d (%.1f MB)  anim %d/%d"
  ```
- Rules:
  - The toolbar adds **Materials** and **Motion** (`BS_AUTOCHECKBOX | BS_PUSHLIKE`, both checked) after Water. They call `set_shading` and `set_motion`.
  - Clock: with `fixedTime`, call `set_time(time)` once. Otherwise accumulate `dt` only while Motion is on and call `set_time` every tick. Every tick with `view.animating()` sets `dirty`.
  - The warnings panel shows the scene warnings followed by `view.warnings()`. It is refreshed when the world arrives and when the last model is in.
  - The status line appends `mapwin_material_status`.
  - Headless:
    - Wait also for the terrain texture thread's `done`.
    - With `focusAnimated`, use `focus_animated` for the capture camera (else `frame_all`).
    - The stats line adds `shading=%s time=%.2f textures=%d textureMB=%.1f texturesFailed=%d animated=%d unresolvedClips=%d chunksUntextured=%d materialsUntextured=%d waterPreset=%d animRect=%d,%d,%d,%d`, with −1s when there is no rect.
  - Env:
    - `GW2_MAPWIN_SHADING` goes through `mapwin_shading_is_geometry(core::env::text(...))`.
    - `GW2_MAPWIN_TIME` is read with `env::is_set` plus `env::real(name, 0.0f, -1e9f)`, clamped to ≥ 0. The minimum must be below 0 so that `0` is honoured; `real` ignores values ≤ minimum (env.h:59).
    - `GW2_MAPWIN_FOCUS=animated` sets `focusAnimated`.
  - The `env.h` table gains these three rows.

- [ ] **Step 1: Write failing tests**
  - `map_window.shading_text`: "geometry" and "GEOMETRY" are true; "materials", "" and "foo" are false.
  - `map_window.material_status_text`: `textures 12, textureMB 3.44, animated 5, unresolvedClips 1` → `L"tex 12 (3.4 MB)  anim 5/1"`.
  - The existing id-uniqueness test's table gains `ID_MW_MATERIALS` and `ID_MW_MOTION`.
- [ ] **Step 2: Run** `cm_test_ui.exe map_window.` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `cm_test_ui.exe` → PASS; `ctest --preset debug` → PASS.
- [ ] **Step 5: Commit** `feat(ui): Map window Materials and Motion toggles, texture/animation status, GW2_MAPWIN_SHADING/TIME/FOCUS`.

### Task 15: Dat tests

**Files:**
- Modify: `tests/test_world_dat.cpp` (it links `world` and `extract`; reuse `shared_dat`, `ensure_template` and `kTestMaps`, :19-139)

**Interfaces:**
- Consumes: `load_world`, `decode_texture_capped`, `load_model_by_fileid(…, ModelLoadOptions)`, `resolve_prop_clip`, `castlemist::model::detokenizeName64`.
- Every command: `GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" GW2_TEST_TEMPLATE="C:/Users/vital/Downloads/castlemist/castlemist/dumps/packfile/gw2_packfile.json" build/debug/bin/cm_test_world_dat.exe world_dat.materials_`

- [ ] **Step 1: Measure (replaces the spec's "count the plan's first task measures").** The readers this count needs only exist after Tasks 2-6, so it is measured here. Write `world_dat.materials_spirit_vale_clips_resolve`:
  - For each distinct model of the `propAnimArray` placements of 1151420, load it with `{false, false}`.
  - Count distinct (model, animSequence) pairs and placements that resolve.
  - Print both counts once, then run.
  - The pairs must be 37 of 41 (note §5). The four misses must be exactly (1127039, "zeropose"), (1151335, "zeropose"), (740373, "on") and (1150399, "noarch"), compared with `detokenizeName64`. If they are not, stop and report: the reader or the resolution disagrees with the note.
  - Pin the placement count as `kSpiritValeResolvedPlacements` with a comment giving the date and client build. Remove the print.
- [ ] **Step 2: Write the remaining tests**
  - `world_dat.materials_terrain_textures_decode` (spec): for each map, for every resolved chunk, every non-zero colour and normal fileId and each page fileId decodes with `decode_texture_capped(dat, id, 512, t)`. Each result has `max(w, h) <= 512` and a last mip ≤ 4×4. A chunk with page fileId 0 has `pickerScale > 0` (a solid page). Cache by fileId inside the test. Expect 0 failures.
  - `world_dat.materials_model_1126949_tex_transforms` (spec): with `{false, false}`, `materials[0].texTransforms.size() == 2`, `[0].uvIndex == 1`, `[1].uvIndex == 0`, both `scroll ≈ (0, 0.2)` and `columns = rows = count = 1`. Loading with `{false, true}` gives the same diffuse fileId per material, and the header-only `rgba` is empty.
  - `world_dat.materials_queensdale_water_presets` (spec):
    - 192711 has non-empty `environment.water`, and every `materialFileId` is in {187551, 188555}.
    - One preset matches the note's first preset (±0.01): `bumpTile (5.01, 13.66)`, `bumpSpeed (1.31, 0.64)`, `patternTile 4.58`, `surfaceShallowColor {0xA5,0xDF,0x5C,0x00}`, `surfaceDeepColor {0x36,0x4F,0x21,0x00}`, `surfaceFresnel 3.89`, `depthAttenuation 15.66`, `distortAmount 0.01`.
    - `0 <= light.presetIndex < water.size()`.
    - Rule: if no dataGlobal preset matches, the note's preset is in a zone block. Stop and report rather than widen the reader without a spec amendment.
  - `world_dat.materials_scene_fields`: Spirit Vale has 36 chunks with `hasUvData` (note §1.5), one of them with `uvData.translation ≈ (0, 0.2)`. `terrain.swapDistance` is 39936 on Queensdale and 24576 on Spirit Vale. Every resolved Queensdale chunk has `texIndexFileIds.size() == 10`.
- [ ] **Step 3: Run** → PASS, and SKIP without `GW2_TEST_DAT`.
- [ ] **Step 4: Commit** `test(world): materials and motion against the three test maps`.

### Task 16: Headless verification across the three maps

**Files:** none committed. The numbers go into Task 17. Scripts live in the scratchpad and run with `python -I`; they read only castlemist's BMP and `mapwin.txt` outputs.

- [ ] **Step 1: Run** from the main checkout:
  ```bash
  export PATH="/c/msys64/ucrt64/bin:$PATH"
  D=C:/Users/vital/Downloads/castlemist/castlemist/dumps/mapwin; mkdir -p "$D"
  EXE=C:/Users/vital/Downloads/castlemist/castlemist/.claude/worktrees/world-materials/build/debug/bin/castlemist.exe
  cd C:/Users/vital/Downloads/castlemist/castlemist
  run() { GW2_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" GW2_DUMP_DIR="$D" GW2_MAPWIN=$1 GW2_MAPWIN_SHOT="$D/$2.bmp" "${@:3}" "$EXE"; cp "$D/mapwin.txt" "$D/$2.txt"; }
  run 192711 qd_mat env GW2_MAPWIN_TIME=0
  run 192711 qd_geo env GW2_MAPWIN_TIME=0 GW2_MAPWIN_SHADING=geometry
  run 191000 la_mat env GW2_MAPWIN_TIME=0
  run 1151420 sv_t0 env GW2_MAPWIN_TIME=0 GW2_MAPWIN_FOCUS=animated
  run 1151420 sv_t25 env GW2_MAPWIN_TIME=2.5 GW2_MAPWIN_FOCUS=animated
  ```
- [ ] **Step 2: Check:**
  - Every line has `ok=1`, and `textures > 0` for the materials runs.
  - `qd_mat`'s luma variance is greater than 1.5 × `qd_geo`'s.
  - Inside `sv_t0`'s `animRect`, at least one pixel differs from `sv_t25` by more than 8/255.
  - Queensdale `models_s` is well under 156 and `add_ms_max` is under 50.
  - Record `models_s`, `total_s`, `add_ms_median`, `add_ms_max`, `textures`, `textureMB`, `chunksUntextured`, `animated` and `unresolvedClips` for each map.
  - If a check or target fails, report the numbers; do not loosen a threshold. For `add_ms_max`, first try lowering Task 8's 16 MB batch cap, then rerun.

### Task 17: Docs and spec amendments

**Files:**
- Modify: `docs/architecture.md` (the `render → exportgltf` edge; `world/terrain_blend.h`, `world/anim.h`; `render/world_materials.*`, `render/world_prep.h`), `docs/using-castlemist.md` (Map Window: Materials and Motion, the status fields, the warnings that list UNPROVEN settings), `docs/testing.md` (`GW2_MAPWIN_SHADING`, `GW2_MAPWIN_TIME`, `GW2_MAPWIN_FOCUS`, the new stats fields, Task 16's table beside the sub-project 2 table at :122-126), `docs/superpowers/specs/2026-10-14-world-materials-design.md` (an "Amendments (after implementation)" section), and the roadmap line for sub-project 3 in `docs/superpowers/specs/2026-10-08-world-data-core-design.md`

- [ ] **Step 1: Write the docs.** The Amendments section lists what shipped differently from the spec:
  - fileId lookup is a binary search over the lists `load_dat_file` already sorts, not a separate hash index;
  - render now depends on exportgltf;
  - `TerrainBlendInput::present` and the absent-slot rule;
  - the extra settings: `srgbColourTextures`, `terrainSpecPower`, `waterBumpTextureIndex`, `waterTileUnit`, `waterSpecPower`;
  - header-only textures, with the pixel effect guess moved to the view;
  - textures are not restored after a device reset;
  - the 16 MB batch cap and the terrain texture thread;
  - `GW2_MAPWIN_FOCUS`, `animRect` and the other stats fields;
  - the Spirit Vale placement count was measured in Task 15, and its value;
  - Task 16's measured numbers against the targets.
- [ ] **Step 2: Run** `ctest --preset debug` → PASS.
- [ ] **Step 3: Commit** `docs: world materials and motion`.
