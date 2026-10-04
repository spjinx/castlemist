# Per-piece Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export each equipped piece of a `CharacterManifest` as its own `.glb`: armor with the character's race/gender model, dyes baked into its diffuse, normal map and cut-out; weapons/back as-is.

**Architecture:** A pure Composite-file decoder (format), a skin appearance token recorded by the content map, small manifest additions (character), and a new `ripper` layer with pure dye/atlas math plus the dat-driven piece/character exporters. CLI and UI call `export_character()`.

**Tech Stack:** C++20, MinGW, CMake presets, nlohmann::json, existing extract/exportgltf layers, in-house `test_framework.h`.

**Spec:** `docs/superpowers/specs/2026-10-03-piece-export-design.md` (format facts: `docs/research/gw2-armor-skins-and-dyes.md`)

## Global Constraints

- Never read Gw2.dat except through castlemist code paths that read single entries by fileId; never load or copy the whole archive.
- Live tests read the dat path from `GW2_TEST_DAT` and `SKIP` without it; run them locally with `GW2_TEST_DAT=E:\Games\gw2\Guild Wars 2\Gw2.dat`.
- Builds run from PowerShell (`cmake --build --preset debug`); tests from either shell (`build/debug/bin/cm_test_<layer>.exe [filter]`).
- Atlas math: 1024-px atlas; piece texture drawn at 2x anchored at its rect origin (`tex_px = (atlas_px - rect_origin) / 2`); UVs unflipped.
- Dye math: research §4 matrix, applied to BGR, truncate then clamp 0..255; must equal the API `rgb` for every fixture color/material.
- Composite layout: PackCompositeV20 at file offset 28; `array_ptr` = `{u32 count, i64 rel}`, `wchar_ptr`/`fileref` = `i64 rel`; RaceData 224 B; FileData 103 B; BlitRectSet 40 B; packed.
- Manifest JSON stays `"version": 1`; new fields are optional on read.

## Review Focus

1. **A race/gender with no composite entry for a token** (e.g. a cultural armor on another race, or NPC races) must report "no appearance for <race>" for that piece, not crash or export the HumanMale default silently — Task 7 test `armor_without_race_entry_is_reported`.
2. **Textures smaller than the half-rect** (e.g. 128x128 shoulders in a 256x256 rect is exact; a smaller one must pad, not read out of bounds) — Task 6 test `crop_pads_small_texture`.
3. **Null/unused dye channels** (mask fileref 0, or no dye in that slot) leave texels untouched — Task 5 test `bake_skips_missing_mask_and_dye`.
4. **Malformed or truncated Composite bytes** (pointer past the end) → nullopt, never a crash — Task 1 test `bad_pointer_is_nullopt`.
5. **Old manifest files without the new fields** still load — Task 4 test `old_manifest_without_new_fields_loads`.

---

### Task 1: Composite decoder

**Files:** Create `include/castlemist/format/composite.h`, `src/format/composite.cpp`, `tests/test_format_composite.cpp`; modify `tests/CMakeLists.txt` (format SOURCES).

**Interfaces — Produces:** exactly the spec's `BlitRect`, `BlitRectSet`, `CompositeFileData`, `CompositeRace`, `Composite` (with `race(std::string_view)`), and `std::optional<Composite> parse_composite(std::span<const uint8_t>)`, namespace `castlemist::composite`.

- [ ] **Step 1: Failing tests** (group `composite`). A `CompositeBuilder` in the test writes a minimal valid file: PF header (`"PF"`, u16 5, u16 0, u16 12, `"cmpc"`), chunk header at 12 (`"comp"`, u32 size, u16 version 19, u16 16, u32 0), root at 28; helpers place arrays/strings/filerefs at chosen offsets and write self-relative pointers.
  - `parses_race_and_file_data`: one race `"SylvariFemale"` (skeleton fileref -> 31210) with one FileData token `0x00000348C28A32A3`, type 9, meshBase 40405, maskDye {151449,151451,151453,0}, base 151455, normal 151457, hideFlags 2048, blitRectIndex 2 → all fields equal; `race("SylvariFemale")` non-null; `race("Nope")` null.
  - `parses_blit_rect_sets`: one set `"ArmorHeavy"` 1024x1024 with rects {(0,512,384,1024),(896,256,1024,512)}.
  - `fileref_zero_and_unset`: a null pointer and a pair with a half < 0x100 both give 0.
  - `bad_pointer_is_nullopt`: raceSexData pointer beyond the buffer → nullopt.
  - `wrong_container_is_nullopt`: `"cntc"` instead of `"cmpc"`.
  - `live_sylvari_female_warden_coat` (skips without `GW2_TEST_DAT`): load dat, fileId 154681 → `read_entry_bytes` + `cmp::decompress_method0`, parse → 29 races; SylvariFemale token `0x00000348C28A32A3` → mesh 40405, base 151455, masks {151449,151451,151453,0}; HumanMale token `0x0125089844F38644` → mesh 2693820.
- [ ] **Step 2: Run, verify compile failure.** `cm_test_format composite`.
- [ ] **Step 3: Implement** per the research layout; bounds-check every read; fileref decode = `cschema::decode_fileref_pair` at the pointed address; strings UTF-16 → UTF-8 (ASCII expected).
- [ ] **Step 4: Run → PASS** (live test PASS with `GW2_TEST_DAT` set).
- [ ] **Step 5: Commit** `format: decode the character Composite file (race models, dye masks, atlas rects)`.

### Task 2: Skin appearance token in the content map

**Files:** Modify `include/castlemist/format/content_map.h`, `src/format/content_map.cpp`, `tests/test_format_content_schema.cpp`.

**Interfaces — Produces:** `uint64_t cmap::skin_token(uint32_t skin_id)` (0 = unknown).

- [ ] **Step 1: Failing test** `cmap_skin_token_recorded` (uses the existing `PackBuilder` + `build_from_packs`): a type-66 object with dataId 517 and u64 `0x00000348C28A32A3` at +200 → `skin_token(517)` equals it; unknown id → 0; survives `save()`/`clear()`/`load()` round trip through a temp file.
- [ ] **Step 2: Run → FAIL.**
- [ ] **Step 3: Implement**: in `parse_cntc`, for `type == CONTENT_TYPE_SKIN` with `o + 208 <= nextOff` read the u64 at +200 into `g_skin_tokens[id]`; `clear()` empties it; cache: bump `kCacheMagic` to `'GC6N'` (`0x4E364347`), append `u32 count` + `{u32 id, u64 token}` records after the item links; `load()` reads them when present.
- [ ] **Step 4: Run → PASS**, plus the whole `cm_test_format`.
- [ ] **Step 5: Commit** `format/cmap: record each skin's composite appearance token`.

### Task 3: Public CPU texture decode

**Files:** Modify `include/castlemist/extract/entry_extractor.h`, `src/extract/entry_extractor.cpp`; test in `tests/test_extract_dat.cpp`.

**Interfaces — Produces:** `bool decode_texture_rgba(Gw2Dat& dat, uint32_t file_id, ModelTextureCPU& out);` (wraps internal `decode_texture_by_fileid`).

- [ ] **Step 1: Failing live test** `texture_decodes_by_file_id` (group of the dat test): fileId 151455 → true, 512x256, `rgba.size()==512*256*4`; fileId 0 → false.
- [ ] **Step 2: Run → compile failure.** **Step 3: Implement.** **Step 4: Run → PASS** (`GW2_TEST_DAT` set). **Step 5: Commit** `extract: expose CPU texture decode by fileId`.

### Task 4: Manifest additions

**Files:** Modify `include/castlemist/character/{gw2_api.h,manifest.h,resolver.h}`, `src/character/{gw2_api.cpp,resolver.cpp,manifest_json.cpp}`, `tests/test_character_{api,resolver,manifest_json}.cpp`.

**Interfaces — Produces:**
- `struct DyeShift { float brightness = 0, contrast = 1, hue = 0, saturation = 1, lightness = 1; };`
- `ApiColor::shift` : `std::map<std::string, DyeShift>` per material.
- `ManifestDye::shift` : `std::optional<DyeShift>`; `ManifestPiece::skin_type` : `std::string`; `ManifestPiece::skin_token` : `uint64_t`.
- `AssetLookup::skin_token(uint32_t skin_id) const` (pure virtual; `CmapAssetLookup` → `cmap::skin_token`; test fakes return a map value or 0).
- JSON: dye `"shift": {"brightness",...}` (omitted when absent), piece `"skin_type"`, `"skin_token": "0x%016llX"`; reading tolerates their absence.

- [ ] **Step 1: Failing tests:** `api.colors_keep_shift` (fixture color 1 cloth brightness 15, contrast 1.25, hue 38, saturation 0.28125, lightness 1.44531); `resolver.piece_carries_token_type_and_shift`; `manifest_json.round_trip` extended with the new fields; `manifest_json.old_manifest_without_new_fields_loads`.
- [ ] **Step 2: Run → FAIL. Step 3: Implement. Step 4: Run → PASS** (whole `cm_test_character`). **Step 5: Commit** `character: carry skin token, skin type and dye shift in the manifest`.

### Task 5: Dye math (`ripper` layer scaffold)

**Files:** Create `include/castlemist/ripper/dye.h`, `src/ripper/dye.cpp`, `tests/test_ripper_dye.cpp`; modify `CMakeLists.txt` (`castlemist_add_layer(ripper DEPS castlemist::character castlemist::extract castlemist::exportgltf castlemist::format castlemist::db ext::json)` after exportgltf), `tests/CMakeLists.txt` (`castlemist_add_test(ripper ...)` with `CM_TEST_DATA_DIR`).

**Interfaces — Produces:** `using ColorMatrix = std::array<std::array<double,4>,4>;` `ColorMatrix dye_matrix(const character::DyeShift&);` `std::array<uint8_t,3> apply(const ColorMatrix&, std::array<uint8_t,3> rgb);` `void bake_dyes(std::vector<uint8_t>& rgba, int w, int h, const std::array<const std::vector<uint8_t>*,4>& masks, const std::array<std::optional<ColorMatrix>,4>& dyes);` (namespace `castlemist::ripper`).

- [ ] **Step 1: Failing tests** (group `dye`): `matches_api_for_every_fixture_color` — for each color/material in `tests/data/character/colors.json`, `apply(dye_matrix(shift), base_rgb) == rgb`; `identity_shift_is_identity`; `bake_full_mask_applies_matrix` (2x1 image, mask 255 → texel == `apply`); `bake_half_mask_lerps` (mask 128 → midpoint ±1); `bake_skips_missing_mask_and_dye` (null mask or nullopt dye → unchanged); `bake_keeps_alpha`.
- [ ] **Step 2: Run → FAIL. Step 3: Implement** (mask weight = luminance `(r+g+b)/3/255`). **Step 4: PASS. Step 5: Commit** `ripper: dye color matrix and texture bake`.

### Task 6: Atlas math

**Files:** Create `include/castlemist/ripper/atlas.h`, `src/ripper/atlas.cpp`, `tests/test_ripper_atlas.cpp`.

**Interfaces — Produces:** `struct ImageRgba { int w = 0, h = 0; std::vector<uint8_t> px; };` `std::optional<composite::BlitRect> piece_rect(const composite::BlitRectSet&, float umin, float vmin, float umax, float vmax);` `ImageRgba crop_piece(const ImageRgba& tex, const composite::BlitRect&);` `void remap_uv(float& u, float& v, const composite::BlitRect&);`

- [ ] **Step 1: Failing tests** (group `atlas`): `rect_contains_uv_box` (ArmorHeavy rects; coat box u 0.0018–0.374, v 0.5018–0.9973 → (0,512,384,1024); boots box 0.879–0.995 / 0.255–0.495 → (896,256,1024,512)); `no_rect_is_nullopt`; `crop_takes_half_rect_block` (512x256 tex, coat rect → 192x256, pixel (10,20) preserved); `crop_pads_small_texture` (64x64 tex, rect 256x256 → 128x128, pixels beyond 64 transparent); `remap_uv_maps_rect_to_unit` (u=384/1024 in rect x0=0..384 → 1.0; v=512/1024 → 0.0).
- [ ] **Step 2: FAIL. Step 3: Implement. Step 4: PASS. Step 5: Commit** `ripper: character-atlas rect lookup, crop and UV remap`.

### Task 7: Piece and character export

**Files:** Create `include/castlemist/ripper/{piece_export.h,character_export.h}`, `src/ripper/{piece_export.cpp,character_export.cpp}`, `tests/test_ripper_export.cpp`.

**Interfaces — Produces:**
- `struct PieceContext { Gw2Dat* dat; const composite::Composite* comp; std::string race_key; };`
- `struct PieceExportResult { bool ok = false; std::string glb_path, status, reason; uint32_t mesh = 0, texture_base = 0; };`
- `PieceExportResult export_piece(const PieceContext&, const character::ManifestPiece&, const std::string& glb_path);`
- `struct CharacterExportReport { std::vector<std::pair<std::string, PieceExportResult>> pieces; std::string error; size_t exported() const; };`
- `std::optional<composite::Composite> load_composite(Gw2Dat& dat);` (index DB entry with chunk `comp` if a DB is open, else fileId 154681)
- `CharacterExportReport export_character(const character::CharacterManifest&, const std::string& dat_path, const std::string& out_dir);`
- Statuses: `"armor"`, `"model"`, `"skipped"`; skip reasons: `"no skin"`, `"no appearance for <race_key>"`, `"model failed to load"`, `"texture failed to decode"`.

- [ ] **Step 1: Failing live tests** (group `export`, skip without `GW2_TEST_DAT`): `exports_sylvari_female_warden_coat` — manifest piece {Coat, skin 517, token 0x00000348C28A32A3, type Armor, dyes slot0 Celestial/slot1 Abyssal Sun with shifts from the fixtures} → ok, status armor, mesh 40405, .glb exists; parse the .glb JSON chunk: an image of 192x256 is embedded (check PNG IHDR) and the armor primitive's TEXCOORD_0 accessor min/max lie in [0,1]; `armor_without_race_entry_is_reported` (race_key "CharrFemale", token of Holographic greaves 0x0125089844F38644 which has no CharrFemale entry → skipped, reason starts "no appearance for"); `weapon_exports_own_model` (piece WeaponA1 skin 8813 with file_ids {2163020} and no token → status model); `export_character_writes_report` (2-piece manifest → 2 files + `export_report.json`).
- [ ] **Step 2: FAIL. Step 3: Implement** per spec §ripper.3/4: armor meshes = those whose UV box fits a rect of `comp->blit_sets[fd.blit_set]`; textures appended to `ModelPreview::textures` (fileId kept for naming); material diffuse/normal indices set; non-armor meshes get no texture; `export_model_gltf(model, path)`. File names `NN_<Slot>_<sanitized skin name>.glb`.
- [ ] **Step 4: PASS** with `GW2_TEST_DAT` set; full `ctest` green. **Step 5: Commit** `ripper: export race-correct, dyed armor pieces and whole characters`.

### Task 8: Front ends

**Files:** Modify `tools/gw2dat_cli/main.cpp`, `tools/CMakeLists.txt` (DEPS + `castlemist::ripper castlemist::db`), `CMakeLists.txt` (ui DEPS + `castlemist::ripper`), `src/ui/character_dialog.cpp`, `src/ui/detail/app_state.h` (IDs `ID_CH_EXPORT = 2168`, message `WM_APP_CHAR_EXPORT_DONE = WM_APP + 8`).

- [ ] **Step 1: CLI** `character-export --manifest <json> --dat <path> --out <dir> [--index <db>]` (index default `dumps/index/gw2_index.db`, opened if present) → emits `{"ok":true,"exported":N,"pieces":[{"slot","status","file","reason"}]}`.
- [ ] **Step 2: Verify live:** run the CLI on Musa Blossom's manifest (fetched with `character --key-name Main --character "Musa Blossom" --out m.json`) → 13 exported (7 armor + back + 5 weapons), 5 skipped trinkets.
- [ ] **Step 3: UI** "Export pieces..." button (folder picker via `SHBrowseForFolderW`), worker thread calls `export_character(g_ch.current->manifest, g_app->data_gw2.file_info.file_path, folder)`; disabled while busy and when no dat is open; status "Exported N of M pieces to <folder>". Spec's "Open model uses the race mesh" is satisfied by opening `report.pieces[i].mesh` after an export; before one it keeps `file_ids[0]`.
- [ ] **Step 4: Verify** build, `ctest`, and drive the dialog (scratch PowerShell driver) → export runs, files appear.
- [ ] **Step 5: Commit** `cli/ui: character-export and Export pieces...`.
