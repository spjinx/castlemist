# Export Skybox (Map) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export any GW2 map's sky as Unity-ready skybox images — raw cube faces where the game stores a cube, and a CPU bake of the composed sky (panoramas, stars, clouds, sky cards, haze) for every map.

**Architecture:** `Extractor::parseMapSky()` reads the `env` chunk into a plain `MapSky`. A pure sampler (`sky_bake`) turns decoded images plus one sky mode into `radiance(direction)`, implementing only what `docs/research/gw2-sky.md` proves. Projection writers (`sky_project`) turn any radiance callback into an equirect or six faces. `sky_export` ties them together in two stages: a dat-reading stage on the caller's thread and a pure writing stage that can run on a worker thread.

**Tech Stack:** C++20, nlohmann::json, stb_image_write (`ext::stb`), the repo's own test framework (`tests/framework/test_framework.h`), CMake/Ninja presets.

**Spec:** `docs/superpowers/specs/2026-10-08-skybox-export-design.md`

## Global Constraints

- Output layout, verbatim from the spec: `<Name>/sky.json`, `<Name>/<mode>/baked/equirect.png` (4096x2048), `<Name>/<mode>/baked/{px,nx,py,ny,pz,nz}.png` (1024² each), `<Name>/<mode>/skybox/{px,nx,py,ny,pz,nz}.png` only when the map stores a cube for that mode.
- `<mode>` names are `mode0`..`mode3` unless `gw2-sky.md` proves the time-of-day names.
- Raw cube faces are reoriented only (axis remap + 90° rotations/flips); never resampled.
- Nothing unproven goes into the bake: an unproven layer is left out and named in `sky.json` `warnings`.
- PNG, 8-bit RGBA. No HDR.
- Global sky only; `dataLocalArray` is not read.
- Gw2.dat is only ever opened through castlemist code (`Gw2Dat`, `gw2dat_cli`) — never read or searched directly.
- Dat-backed tests skip when the dat is absent (`SKIP(...)`), like `tests/test_extract_dat.cpp`. On this machine set `GW2_TEST_DAT="E:\Games\gw2\Guild Wars 2\Gw2.dat"`.
- Test map fileIds: `187611` (panorama sky, modes 0–3, `starFile` 187544, mode 0 = NE 187554 / SW 187556 / T 187558, mode 3 repeats mode 0) and `3264516` (cube sky in modes 0–1: E 3263205, W 3263207, N 3263209, S 3263211, B 3263213, T 3263215; modes 2–3 null).

## Review Focus

1. **Unity face orientation.** A face that looks right alone but is mirrored or rotated in Unity's `Skybox/6 Sided` is the likeliest user-visible bug. Task 3 pins the face table and checks seams against neighbouring faces; Task 6 adds a cross-check that the raw faces of 3264516 also join seamlessly.
2. **A map with no `env` chunk, or no sky textures.** Expect a clean "no sky" report with nothing written, not a crash or an empty folder. Task 5 test `skyexport.no_sky_writes_nothing`.
3. **A sky texture that fails to decode** (bad fileId, non-texture entry). Expect that layer to be skipped and warned; if it's the base panorama, that mode's bake is skipped and the other modes still export. Task 5 test `skyexport.missing_layer_is_warned`.
4. **Re-exporting into an existing folder.** Expect files to be overwritten, not appended or left stale (e.g. an old `skybox/` from another map). Task 5 test `skyexport.overwrites_existing_folder`.
5. **Older env versions (v29–v74)** missing `skyModeCubeTex`/`skyCards`. Expect `parseMapSky` to return the fields it has and leave the rest empty. Task 1 test `mapsky.missing_fields_stay_empty`.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/castlemist/native/gw2model.hpp` (modify) | `MapSky*` structs + `Extractor::parseMapSky()` beside `parseMapEnv` |
| `docs/research/gw2-sky.md` (create) | proven sky maths: projection, modes, layers, cards, haze, axes, face orientation |
| `include/castlemist/exportgltf/sky_project.h`, `src/exportgltf/sky_project.cpp` (create) | `Image`, face table, equirect and cube writers over a radiance callback |
| `include/castlemist/exportgltf/sky_bake.h`, `src/exportgltf/sky_bake.cpp` (create) | the sampler: `SkySampler` from decoded images + one `MapSkyMode` |
| `include/castlemist/exportgltf/sky_export.h`, `src/exportgltf/sky_export.cpp` (create) | `load_sky_inputs` (dat stage), `write_skybox` (pure stage), `sky.json`, mode merging, raw faces |
| `tools/gw2dat_cli/main.cpp` (modify) | `skybox` command |
| `src/ui/detail/app_state.h`, `src/ui/window_proc.cpp`, `src/ui/view_controls.cpp`, `src/ui/file_ops.cpp` (modify) | File → Export Skybox (Map)… |
| `tests/test_exportgltf_sky.cpp` (create), `tests/test_extract_dat.cpp` (modify), `tests/CMakeLists.txt` (modify) | tests |
| `docs/using-castlemist.md` (modify) | menu + CLI rows |

New `.cpp` files under `src/exportgltf/` are picked up by `castlemist_add_layer(exportgltf ...)` in the root `CMakeLists.txt`; check how that function collects sources and add them explicitly if it lists files.

Tasks 1, 2 and 3 are independent and can run in parallel. Task 4 needs 1 and 2. Task 5 needs 1, 3 and 4. Task 6 needs 5.

---

### Task 1: `parseMapSky()`

**Files:**
- Modify: `include/castlemist/native/gw2model.hpp` (structs next to `MapEnvLight` ~line 1145; method next to `parseMapEnv` ~line 1158)
- Test: `tests/test_extract_dat.cpp`

**Interfaces:**
- Consumes: existing `Extractor` helpers `findChunk`, `fieldOffset`, `follow`, `arrayAt`, `resolveVariant`, `typeSize`, `decodeFilename(typeName, structStart)`, `rdf`, `rd8`.
- Produces (namespace `castlemist::model`, nested in `Extractor` like `MapEnvLight`):

```cpp
struct MapSkyCardAttr {
    uint32_t texture = 0;
    float azimuth = 0, latitude = 0, density = 0, hazeDensity = 0, minHaze = 0,
          lightIntensity = 0, brightness = 0, speed = 0;
    float scale[2] = {0, 0};
    float textureUV[4] = {0, 0, 1, 1};
    uint8_t flags = 0;
};
struct MapSkyCard {
    std::string name;
    uint32_t flags = 0;
    float location[3] = {0, 0, 0};
    MapSkyCardAttr day, night;
    uint32_t materialFile = 0;               // material->filename, 0 if none
};
struct MapSkyCloudAttr { float brightness = 0, density = 0, haze = 0, lightIntensity = 0,
                         velocity[2] = {0, 0}, fadeWidth = 0, fadeEnd = 0; };
struct MapSkyCloudLayer {
    std::string name;
    uint32_t texture = 0;
    float altitude = 0, cutOut = 0, depth = 0, extent = 0, scale = 0;
    std::vector<MapSkyCloudAttr> attributes; // one per preset, as stored
};
struct MapSkyMode {
    uint32_t ne = 0, sw = 0, top = 0;        // skyModeTex[i]
    uint32_t cube[6] = {0, 0, 0, 0, 0, 0};   // skyModeCubeTex[i], stored order E, W, N, S, B, T
    bool hasPanorama() const { return ne && sw && top; }
    bool hasCube() const;                    // all six non-zero
};
struct MapSkyParams {                        // PackMapEnvDataSky*
    uint8_t flags = 0;
    float dayBrightness = 0, dayHazeBottom = 0, dayHazeDensity = 0, dayHazeFalloff = 0,
          dayLightIntensity = 0, dayStarDensity = 0,
          nightBrightness = 0, nightHazeBottom = 0, nightHazeDensity = 0, nightHazeFalloff = 0,
          nightLightIntensity = 0, nightStarDensity = 0, verticalOffset = 0;
};
struct MapSky {
    bool present = false;                    // env chunk + dataGlobal found
    uint16_t envVersion = 0;
    std::vector<MapSkyMode> modes;           // index = sky mode; max(len(skyModeTex), len(skyModeCubeTex))
    uint32_t starFile = 0;
    std::vector<MapSkyCloudLayer> clouds;
    std::vector<MapSkyCard> cards;
    MapSkyParams params;
};
MapSky parseMapSky();
```

- [ ] **Step 1: Write the failing tests** in `tests/test_extract_dat.cpp`, reusing that file's dat/template helpers to load a map by fileId:

```cpp
CM_TEST(mapsky, panorama_map_187611) {
    // load fileId 187611, Extractor ex(bytes, tpl)
    auto s = ex.parseMapSky();
    CHECK(s.present);
    CHECK_EQ(s.modes.size(), 4u);
    CHECK_EQ(s.modes[0].ne, 187554u); CHECK_EQ(s.modes[0].sw, 187556u); CHECK_EQ(s.modes[0].top, 187558u);
    CHECK_EQ(s.modes[3].ne, 187554u);
    CHECK_EQ(s.starFile, 187544u);
    CHECK_FALSE(s.modes[0].hasCube());
    CHECK_EQ(s.clouds.size(), 4u);
    CHECK_EQ(s.clouds[0].texture, 186345u);
}
CM_TEST(mapsky, cube_map_3264516) {
    auto s = ex.parseMapSky();               // fileId 3264516
    CHECK(s.modes[0].hasCube());
    CHECK_EQ(s.modes[0].cube[0], 3263205u);  // E
    CHECK_EQ(s.modes[0].cube[5], 3263215u);  // T
    CHECK(s.modes[1].hasCube());
    CHECK_FALSE(s.modes[2].hasCube());
}
CM_TEST(mapsky, sky_cards_have_textures) {
    // fileId 187611 and 3264516: at least one has cards; every card with a
    // non-zero day.texture has a finite azimuth/latitude
}
CM_TEST(mapsky, missing_fields_stay_empty) {
    // A map whose env version predates skyModeCubeTex (find one with the index
    // DB: chunks.fourcc='env.' and version < 47, else SKIP): present == true,
    // every modes[i].cube is zero, no throw.
}
CM_TEST(mapsky, no_env_chunk) {
    // A non-map packfile (any model fileId already used in this file): present == false
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug --target cm_test_dat && GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" ./build/debug/bin/cm_test_dat.exe mapsky`
Expected: compile error, `parseMapSky` not a member.

- [ ] **Step 3: Implement the structs and `parseMapSky()`** following `parseMapEnv`'s template navigation exactly: `env` → `dataGlobal` → `resolveVariant("PackMapEnvDataGlobal", ver)`, then each field by name; nested struct type names come from the field's `element.struct` / `target.struct`, never hard-coded versions. Every missing field leaves its default. Wrap each section (modes, clouds, cards, params) in its own `try` so one bad section doesn't drop the others.

- [ ] **Step 4: Run to verify they pass** (same command). Expected: all `mapsky.*` PASS (or SKIP without the dat).

- [ ] **Step 5: Commit**

```bash
git add include/castlemist/native/gw2model.hpp tests/test_extract_dat.cpp
git commit -m "feat(extract): parseMapSky reads the env chunk's sky"
```

---

### Task 2: Sky research note

**Files:**
- Create: `docs/research/gw2-sky.md`

**Interfaces:**
- Consumes: the engine shader dump (`tools/shaders/extract_exe_shaders.py`, `tools/shaders/SHADER_PIPELINE.md`, `docs/research/gw2-exe-shaders.md` — the 16 sky/atmosphere PS candidates, e.g. #1611, in `stage_candidates.json`); IDA annotations in `tools/gw2_annotations.json` for `EnvContext_LoadSkyTextures`, `EnvContext_SetOverrideSkyTextures`, `MapMod_Environment_*`; decoded textures via `gw2dat_cli texture --dat <dat> --file-id <id> --out <png>`.
- Produces: a note Task 4 implements and Task 5's axis code follows. Each section ends with a **Formula** or **Table** block in plain maths/HLSL-like pseudocode, and cites its evidence (shader index + instruction lines, function address, or decoded-texture observation).

Required sections (a section that can't be proven says **UNPROVEN** with what is missing, and Task 4 leaves that layer out):

1. **Axes.** GW2 world direction → Unity direction (Unity: left-handed, +Y up, +Z forward), including which GW2 axis is north/east. Cross-check against `MapEnvRig.sunDir` ("-Z is up") and the glTF map export's root rotation in `transform_math.cpp`.
2. **Panorama projection.** For NE, SW and T: the direction each texel (u,v) covers, as a formula, including where the horizon sits, what the lower (mirrored-looking) half is for, and how T joins the panoramas.
3. **Sky modes.** What each `skyModeTex` index is (time of day), and which of `day`/`night` attributes (cards, clouds, sky params) each mode uses.
4. **Layer order and blending:** base, stars (`starFile`, `*StarDensity`), cloud layers (each `MapSkyCloudLayer`/`MapSkyCloudAttr` field's role, which `attributes[]` index applies per mode, and the frame chosen for a static bake: scroll offset 0), sky cards (placement from `azimuth`/`latitude`, angular size from `scale`, `textureUV` crop, `brightness`, `density`, blend mode), haze (`*Haze*` fields).
5. **Brightness and range.** How `*Brightness`/`*LightIntensity` scale the result, and the mapping into 8-bit (tonemap/clamp) the game applies or the closest honest equivalent.
6. **Cube sky faces.** For `skyModeCubeTex` E/W/N/S/B/T: which Unity face each becomes and the rotation/flip needed so it matches the face table in `sky_project.h` (Task 3).

- [ ] **Step 1:** Extract the sky PS candidates and disassemble them; identify which one draws the base sky (samples 3 textures: NE, SW, T), which draw cards/clouds/stars.
- [ ] **Step 2:** Write each section with its evidence. Decode 187554/187556/187558 and the 3264516 cube faces to PNG in the scratchpad and confirm the projection by eye: the NE right edge continues into the SW left edge, and the top cap's border matches the panoramas' upper edge.
- [ ] **Step 3: Self-check.** Every Formula/Table names its evidence; every UNPROVEN says what would prove it.
- [ ] **Step 4: Commit**

```bash
git add docs/research/gw2-sky.md
git commit -m "docs(research): GW2 sky projection, layers and axes"
```

---

### Task 3: Projection writers

**Files:**
- Create: `include/castlemist/exportgltf/sky_project.h`, `src/exportgltf/sky_project.cpp`
- Create: `tests/test_exportgltf_sky.cpp`; Modify: `tests/CMakeLists.txt` (add the file to `castlemist_add_test(exportgltf SOURCES ...)`)

**Interfaces:**
- Produces (namespace `castlemist::exportgltf::sky`, all directions in Unity space):

```cpp
struct Rgb { float r = 0, g = 0, b = 0; };                 // linear-ish, 0..1 after tonemap
using Radiance = std::function<Rgb(const float dir[3])>;   // dir normalized
struct Image { int width = 0, height = 0; std::vector<uint8_t> rgba; }; // RGBA8, row 0 = top
enum class Face { PX, NX, PY, NY, PZ, NZ };
struct FaceBasis { float forward[3], right[3], up[3]; };   // image right/up as seen from inside
const FaceBasis& face_basis(Face f);
const char* face_file(Face f);                             // "px".."nz"
const char* face_unity_slot(Face f);                       // "_LeftTex" etc.
Image render_equirect(const Radiance& r, int width, int height);
Image render_face(const Radiance& r, Face f, int size);
void  face_texel_dir(Face f, int x, int y, int size, float out[3]); // texel centre
bool  write_png(const Image& img, const std::string& path, std::string& error);
```

Face table (viewer inside the cube, Unity axes), and Unity `Skybox/6 Sided` slots:

| Face | forward | right | up | Unity slot |
|---|---|---|---|---|
| PZ | +Z | +X | +Y | `_FrontTex` |
| NZ | −Z | −X | +Y | `_BackTex` |
| PX | +X | −Z | +Y | `_LeftTex` |
| NX | −X | +Z | +Y | `_RightTex` |
| PY | +Y | +X | −Z | `_UpTex` |
| NY | −Y | +X | +Z | `_DownTex` |

Texel direction: `dir = normalize(forward + a*right + b*up)` with `a = (2*(x+0.5)/size − 1)`, `b = 1 − 2*(y+0.5)/size`.

Equirect follows Unity's `Skybox/Panoramic` (latitude-longitude): for texel (x, y), `u = (x+0.5)/W`, `t = (y+0.5)/H`; `latitude = t*π` (0 at +Y), `longitude = (0.5 − u)*2π`; `dir = (sin(latitude)*cos(longitude), cos(latitude), sin(latitude)*sin(longitude))`.

Colour → byte: `round(clamp(c,0,1)*255)`, alpha 255.

- [ ] **Step 1: Write the failing tests** in `tests/test_exportgltf_sky.cpp`:

```cpp
CM_TEST(skyproject, face_centres_point_forward) {
    for each Face f: face_texel_dir(f, 63, 63, 128, d) is within 0.01 of face_basis(f).forward
}
CM_TEST(skyproject, axis_marker_lands_on_its_face) {
    // radiance = red where dot(dir, +X) > 0.99, else black
    Image px = render_face(r, Face::PX, 64); centre pixel red
    every other face has no red pixel
    // repeat for all six axes
}
CM_TEST(skyproject, neighbouring_face_edges_agree) {
    // radiance = colour encoding dir (r=(x+1)/2, g=(y+1)/2, b=(z+1)/2), size 64
    // for every pair of adjacent faces, edge-texel dirs that share an edge
    // differ by < 2/64 rad; checks PZ right edge vs PX left edge, PZ top vs PY bottom,
    // PZ bottom vs NY top, PX right vs NZ left, NZ right vs NX left, NX right vs PZ left
}
CM_TEST(skyproject, equirect_known_directions) {
    // 512x256 with the dir-encoding radiance, decoded back to a direction:
    // texel (256,128) ≈ (+1,0,0); texel (384,128) ≈ (0,0,−1); texel (128,128) ≈ (0,0,+1);
    // row 0 ≈ +Y; last row ≈ −Y (each within 0.02)
}
CM_TEST(skyproject, png_round_trip) {
    // write_png to a temp file, read size back (stbi_info): 8x4 RGBA, returns true
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug --target cm_test_exportgltf && ./build/debug/bin/cm_test_exportgltf.exe skyproject`
Expected: compile error, `sky_project.h` not found.

- [ ] **Step 3: Implement** `sky_project.cpp`; `write_png` uses `stbi_write_png` like `texture_export.cpp:96` (include the stb header the same way that file does), creating parent directories with `std::filesystem::create_directories`.

- [ ] **Step 4: Run to verify they pass** (same command). Expected: all `skyproject.*` PASS.

- [ ] **Step 5: Commit**

```bash
git add include/castlemist/exportgltf/sky_project.h src/exportgltf/sky_project.cpp tests/test_exportgltf_sky.cpp tests/CMakeLists.txt
git commit -m "feat(exportgltf): equirect and cube-face writers for skies"
```

---

### Task 4: Sky sampler

**Files:**
- Create: `include/castlemist/exportgltf/sky_bake.h`, `src/exportgltf/sky_bake.cpp`
- Test: `tests/test_exportgltf_sky.cpp`
- Modify (only if the note requires a field Task 1 didn't read): `include/castlemist/native/gw2model.hpp` + a `mapsky.*` test for it

**Interfaces:**
- Consumes: `MapSky`, `MapSkyMode`, `MapSkyCard`, `MapSkyCloudLayer`, `MapSkyParams` (Task 1); `Rgb`, `Radiance`, `Image` (Task 3); `docs/research/gw2-sky.md` (Task 2).
- Produces (namespace `castlemist::exportgltf::sky`):

```cpp
using TextureMap = std::unordered_map<uint32_t, Image>;   // fileId -> decoded RGBA8
struct BakeResult {
    Radiance radiance;                     // captures what it needs by value
    std::vector<std::string> layers;       // layers actually included, e.g. "base", "stars", "clouds", "cards"
    std::vector<std::string> warnings;     // layers left out and why (UNPROVEN, missing texture)
    bool ok = false;                       // false when the base panorama is missing
};
BakeResult make_sky_sampler(const castlemist::model::Extractor::MapSky& sky, size_t modeIndex,
                            const TextureMap& textures);
void gw2_to_unity(const float gw2[3], float unity[3]);    // gw2-sky.md §1
```

- [ ] **Step 1: Write the failing tests** (synthetic textures, no dat). Each one builds a `MapSky` by hand and small `Image`s:

```cpp
CM_TEST(skybake, missing_base_is_not_ok) {
    // modes[0].ne = 1 but TextureMap lacks 1 -> ok == false, warnings non-empty
}
CM_TEST(skybake, panorama_seams_are_continuous) {
    // NE, SW, T filled with a gradient that encodes the source direction per
    // gw2-sky.md §2's formula; sample radiance across the NE↔SW seams and the
    // panorama↔T boundary at 1° steps: neighbouring samples differ by < 0.05
}
CM_TEST(skybake, panorama_texel_maps_to_its_direction) {
    // for 8 texels of NE/SW/T, place a single bright texel; the radiance at the
    // direction §2 assigns it is bright, and 10° away is dark
}
CM_TEST(skybake, sky_card_sits_at_azimuth_latitude) {
    // base all black, one card with a white texture at azimuth/latitude from §4;
    // radiance at gw2_to_unity(card direction) is non-black; opposite direction is black
    // (skip this test with SKIP("cards UNPROVEN in gw2-sky.md") if §4 cards are UNPROVEN)
}
CM_TEST(skybake, unproven_layers_are_warned_not_baked) {
    // every layer gw2-sky.md marks UNPROVEN appears in warnings and not in layers
}
CM_TEST(skybake, gw2_to_unity_up_is_plus_y) {
    // the GW2 up direction from §1 maps to (0,1,0); north and east map per §1's table
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug --target cm_test_exportgltf && ./build/debug/bin/cm_test_exportgltf.exe skybake`
Expected: compile error, `sky_bake.h` not found.

- [ ] **Step 3: Implement** the sampler exactly as `gw2-sky.md` states, layer by layer, with bilinear texture lookup and wrap/clamp as the note specifies. Each layer's code carries a comment naming the note section it implements. No constant may appear that the note doesn't give.

- [ ] **Step 4: Run to verify they pass** (same command). Expected: `skybake.*` PASS (the card test may SKIP per the note).

- [ ] **Step 5: Commit**

```bash
git add include/castlemist/exportgltf/sky_bake.h src/exportgltf/sky_bake.cpp tests/test_exportgltf_sky.cpp
git commit -m "feat(exportgltf): sky sampler from the GW2 sky research"
```

---

### Task 5: Skybox export

**Files:**
- Create: `include/castlemist/exportgltf/sky_export.h`, `src/exportgltf/sky_export.cpp`
- Test: `tests/test_exportgltf_sky.cpp` (pure stage), `tests/test_extract_dat.cpp` (end to end)

**Interfaces:**
- Consumes: `parseMapSky` (Task 1), `render_equirect`/`render_face`/`write_png`/`face_*` (Task 3), `make_sky_sampler`/`gw2_to_unity`/`TextureMap` (Task 4), `decode_texture_rgba(Gw2Dat&, uint32_t, ModelTextureCPU&)` (`entry_extractor.h`), `parseMapEnv(preset)` for each mode's sun.
- Produces (namespace `castlemist::exportgltf::sky`):

```cpp
struct SkyInputs {
    castlemist::model::Extractor::MapSky sky;
    TextureMap textures;                              // every fileId the sky names that decoded
    std::vector<std::string> decodeWarnings;          // "fileId N: not a decodable texture"
    std::vector<castlemist::model::Extractor::MapEnvLight> sunPerMode; // parseMapEnv(preset) per mode
    uint32_t mapFileId = 0;
};
SkyInputs load_sky_inputs(Gw2Dat& dat, const std::vector<uint8_t>& mapBytes,
                          const nlohmann::json& tpl, uint32_t mapFileId);
struct SkyExportOptions { int faceSize = 1024; int equirectWidth = 4096; };  // height = width/2
struct SkyExportReport {
    bool ok = false; std::string error;                // "no sky" when nothing to write
    std::string folder;
    int modesWritten = 0, rawSkyboxes = 0;
    std::vector<std::string> warnings;
};
SkyExportReport write_skybox(const SkyInputs& in, const std::string& parentDir,
                             const std::string& name, const SkyExportOptions& opt = {});
nlohmann::json report_json(const SkyExportReport& r);  // for the CLI
```

`write_skybox` behaviour:
- `!in.sky.present` or no mode has a panorama or a cube → `ok=false`, `error="no sky"`, nothing created.
- Deletes `<parentDir>/<name>` first if it exists, then writes fresh.
- Mode merging: mode *j* aliases the first earlier mode *i* whose `ne/sw/top/cube[]` are identical and, per `gw2-sky.md` §3, uses the same day/night attribute set. Aliased modes write no folder.
- Raw faces: for each mode with `hasCube()`, each stored face is reoriented per `gw2-sky.md` §6 (rotation/flip of the decoded `Image` only) and written as `skybox/<face_file>.png`.
- Bake: for each non-aliased mode with `hasPanorama()` and `make_sky_sampler(...).ok`, write `baked/equirect.png` and the six `baked/<face_file>.png`.
- `sky.json`:

```json
{
  "map": 187611, "envVersion": 76,
  "modes": [
    { "name": "mode0", "aliasOf": null,
      "sources": { "ne": 187554, "sw": 187556, "top": 187558, "cube": null, "stars": 187544,
                   "clouds": [186345], "cards": [] },
      "baked": true, "layers": ["base", "stars"], "skybox": false,
      "sun": { "direction": [0.0, 0.7, 0.7], "color": [1.0, 0.91, 0.75], "intensity": 1.3 },
      "unitySlots": { "px": "_LeftTex", "nx": "_RightTex", "py": "_UpTex",
                      "ny": "_DownTex", "pz": "_FrontTex", "nz": "_BackTex" } }
  ],
  "warnings": []
}
```

(`sun.direction` is in Unity axes via `gw2_to_unity`; the values above are illustrative.)

- [ ] **Step 1: Write the failing tests.** Pure stage in `tests/test_exportgltf_sky.cpp` (hand-built `SkyInputs`, output to a temp dir under `std::filesystem::temp_directory_path()`):

```cpp
CM_TEST(skyexport, no_sky_writes_nothing) {
    // sky.present = false -> ok == false, error == "no sky", folder does not exist
}
CM_TEST(skyexport, panorama_mode_writes_baked_files) {
    // one mode with 8x4 NE/SW/T images, opt {faceSize 16, equirectWidth 32}
    // -> mode0/baked/equirect.png (32x16) + six 16x16 faces + sky.json; no mode0/skybox
}
CM_TEST(skyexport, cube_mode_writes_raw_faces) {
    // one mode with six 8x8 cube images -> mode0/skybox/{px..nz}.png, each 8x8;
    // sky.json modes[0].skybox == true
}
CM_TEST(skyexport, duplicate_modes_alias) {
    // modes 0 and 3 identical -> no mode3 folder; sky.json modes[3].aliasOf == "mode0"
}
CM_TEST(skyexport, missing_layer_is_warned) {
    // mode0 base present but its cloud texture absent from TextureMap -> mode0 baked,
    // warnings mention the cloud fileId; mode1 with missing base -> not baked, warned,
    // mode0 still written
}
CM_TEST(skyexport, overwrites_existing_folder) {
    // pre-create <name>/mode0/skybox/stale.png; export a panorama-only sky;
    // stale.png and mode0/skybox are gone
}
```

End to end in `tests/test_extract_dat.cpp`:

```cpp
CM_TEST(skyexport_dat, map_187611) {
    // load_sky_inputs on fileId 187611 + write_skybox(opt {64, 128}) to temp:
    // ok, modesWritten == 3 (mode3 aliases mode0), mode0/baked/equirect.png exists
}
CM_TEST(skyexport_dat, map_3264516_raw) {
    // ok, rawSkyboxes == 2, mode0/skybox/px.png exists and is the decoded face's size
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset debug && ./build/debug/bin/cm_test_exportgltf.exe skyexport && GW2_TEST_DAT="E:/Games/gw2/Guild Wars 2/Gw2.dat" ./build/debug/bin/cm_test_dat.exe skyexport_dat`
Expected: compile error, `sky_export.h` not found.

- [ ] **Step 3: Implement** `load_sky_inputs` (decode every distinct non-zero fileId in the sky with `decode_texture_rgba`, converting `ModelTextureCPU` to `Image`) and `write_skybox` as specified above. If `cm_test_dat` doesn't link `castlemist::exportgltf`, add it to that target's `DEPS` in `tests/CMakeLists.txt`.

- [ ] **Step 4: Run to verify they pass** (same command). Expected: all PASS (dat ones SKIP without the dat).

- [ ] **Step 5: Commit**

```bash
git add include/castlemist/exportgltf/sky_export.h src/exportgltf/sky_export.cpp tests/
git commit -m "feat(exportgltf): skybox export folder with sky.json"
```

---

### Task 6: CLI, menu, docs, and a real-map check

**Files:**
- Modify: `tools/gw2dat_cli/main.cpp` (new `cmd_skybox`, dispatch next to `cmd_map` ~line 2382, usage string)
- Modify: `src/ui/detail/app_state.h` (new id), `src/ui/window_proc.cpp` (menu item after line 35, initial grey-out after 43, `case` near 1267), `src/ui/view_controls.cpp` (enable with `mapReady`, line 30), `src/ui/file_ops.cpp` (`do_export_skybox_map`, `on_skybox_export_done`)
- Modify: `docs/using-castlemist.md` (menu table row next to "Export glTF (Map)..."; a `gw2dat_cli skybox` example next to line 137)

**Interfaces:**
- Consumes: `load_sky_inputs`, `write_skybox`, `report_json` (Task 5); `castlemist::tpl::get_or_auto_load()`; `get_by_file_id(g_app->data_gw2, g_app->current_mft_index + 1)` for the map fileId (as `do_export_vrchat_model` does); `g_app->current_entry` decompressed bytes.
- Produces:
  - `gw2dat_cli skybox --dat <dat> --file-id <map> --template <json> --out <dir> [--size <face px>]` — writes `<dir>/<name>` where name = `--name` or `map_<fileId>`; prints `report_json` and exits non-zero when `!ok`.
  - `constexpr UINT_PTR ID_FILE_EXPORT_SKYBOX_MAP = 2186;` with menu text `L"Export S&kybox (Map)..."`; a message id `WM_APP_SKYBOX_EXPORT_DONE` defined beside the existing `WM_APP_*` export ids with the next free value.

UI flow: `load_sky_inputs` runs on the UI thread (it uses `g_app->data_gw2`, which is only safe there) under a wait cursor; then `write_skybox` runs on a detached thread, posting `WM_APP_SKYBOX_EXPORT_DONE` with a heap `SkyExportReport*` exactly like `on_vrchat_model_done`. The folder picker is the same save dialog as `do_export_vrchat_model` (directory = parent, stem = name). The done message box says: folder, modes written, raw skyboxes found, warning count, and "Use baked/equirect.png with Skybox/Panoramic, or the six faces with Skybox/6 Sided (slots in sky.json)."

- [ ] **Step 1: Write the failing check.** Run the existing UI id-uniqueness test after adding only the constant: `cmake --build --preset debug --target cm_test_ui && ./build/debug/bin/cm_test_ui.exe` — expected PASS (confirms 2186 is free; if it fails, pick the next free id and repeat).
- [ ] **Step 2: Implement** `cmd_skybox` and the UI handler.
- [ ] **Step 3: Verify the CLI on both test maps**

Run:
```bash
D="E:/Games/gw2/Guild Wars 2/Gw2.dat"; O="<scratchpad>/skyout"
./build/debug/bin/gw2dat_cli.exe skybox --dat "$D" --file-id 187611 --template dumps/packfile/gw2_packfile.json --out "$O"
./build/debug/bin/gw2dat_cli.exe skybox --dat "$D" --file-id 3264516 --template dumps/packfile/gw2_packfile.json --out "$O"
./build/debug/bin/gw2dat_cli.exe skybox --dat "$D" --file-id 1151420 --template dumps/packfile/gw2_packfile.json --out "$O"
```
Expected: each prints `"ok":true`; `map_187611/mode0/baked/equirect.png` is 4096x2048. Open `map_187611/mode0/baked/equirect.png` and the six faces of `map_3264516/mode0/skybox/` and check by eye: the horizon is level, the sky is up, and the faces placed in the cross layout (PY above PZ; NX, PZ, PX, NZ in a row; NY below PZ) join without visible seams. Report what you see with the images' paths.

- [ ] **Step 4: Verify the menu** by building `castlemist` and confirming the item exists and greys out when a model (not a map) is selected: `cmake --build --preset debug --target castlemist` succeeds; the reviewer checks the `view_controls.cpp` enable line.
- [ ] **Step 5: Run the full suite**

Run: `ctest --preset debug` (with `GW2_TEST_DAT` set). Expected: no failures.

- [ ] **Step 6: Commit**

```bash
git add tools/gw2dat_cli/main.cpp src/ui docs/using-castlemist.md
git commit -m "feat: Export Skybox (Map) in the CLI and File menu"
```

---

## After the plan

Visual check against the game needs the user's in-game sky screenshots (e.g. Lion's Arch day and night) next to the bake. Collect them after Task 6, before calling sub-project 1 done.
