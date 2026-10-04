# Per-piece Export — Design (sub-project 2 of the Character Ripper)

Date: 2026-10-03
Status: written autonomously at the user's direction ("keep going until you
ABSOLUTELY need my assistance"); decisions recorded here instead of asked.
Builds on: `2026-10-03-character-fetch-design.md` (sub-project 1).
Research: `docs/research/gw2-armor-skins-and-dyes.md` (all format facts below
are from there).

## Goal

From a `CharacterManifest`, write one `.glb` per visible equipped piece:
- armor uses the **character's race/gender model**, with the character's
  **dyes baked into its diffuse texture**, its normal map, and its cut-out;
- weapons and back items use their own self-contained model.

Each `.glb` opens in Blender correctly textured on its own. Sub-project 3
(assembly) reuses the same building blocks.

## Out of scope

Assembled/rigged character, body/skin/head/hair (sub-projects 3-4); the
`Skin` material that armor shows through (exported untextured, neutral
tone); Sylvari glow; gem-store effect layers (they stay as the model's own
extra textures); per-piece hide flags (recorded, used by assembly).

## Components

### format: Composite decoder — `castlemist/format/composite.h`
Pure parse of the decompressed `cmpc` packfile (PackCompositeV20 layout in the
research note; 64-bit filerefs = 8-byte self-relative pointers).

```
struct BlitRect { uint32_t x0, y0, x1, y1; };
struct BlitRectSet { std::string name; uint32_t width, height; std::vector<BlitRect> rects; };
struct CompositeFileData { uint64_t token; uint8_t type; uint32_t mesh_base, mesh_overlap;
    std::array<uint32_t,4> mask_dye; uint32_t mask_cut, texture_base, texture_normal;
    uint32_t dye_flags, hide_flags, skin_flags; uint8_t blit_set; };
struct CompositeRace { std::string name; uint32_t skeleton_file;
    std::unordered_map<uint64_t, CompositeFileData> file_data; };
struct Composite { std::vector<BlitRectSet> blit_sets; std::vector<CompositeRace> races;
    const CompositeRace* race(std::string_view name) const; };
std::optional<Composite> parse_composite(std::span<const uint8_t> decompressed);
```
Fails (nullopt) on wrong container/chunk or any out-of-range pointer.

### format: skin appearance token — `content_map`
`build()` additionally records, for every type-66 object, the u64 at +200.
`uint64_t skin_token(uint32_t skin_id)` returns it (0 = unknown). It is part
of the disk cache (cache magic bumped so stale caches rebuild).

### extract: CPU texture decode made public
`bool decode_texture_rgba(Gw2Dat&, uint32_t file_id, ModelTextureCPU& out)` in
`entry_extractor.h`, wrapping the existing internal
`decode_texture_by_fileid`.

### character: manifest additions
- `ManifestPiece::skin_token` (u64; JSON hex string `"0x..."`), filled by the
  resolver from a new `AssetLookup::skin_token()`.
- `ManifestDye::shift` = `{brightness, contrast, hue, saturation, lightness}`
  of the dye's color for the slot's material (from `/v2/colors`), and
  `ApiColor` keeps those per material.
- `ManifestPiece::skin_type` ("Armor"/"Weapon"/"Back"/...) from `/v2/skins`.
- JSON stays version 1; the new fields are optional on read.

### ripper layer (new) — `include/castlemist/ripper/`, `src/ripper/`
Depends on character, extract, exportgltf, format, db.

1. **`dye.h`** — pure:
   `ColorMatrix dye_matrix(const DyeShift&)` (4x4, BGR convention, research §4);
   `std::array<uint8_t,3> apply(const ColorMatrix&, std::array<uint8_t,3> rgb)`;
   `void bake_dyes(std::vector<uint8_t>& rgba, int w, int h, const std::array<const std::vector<uint8_t>*,4>& masks (same size, RGBA), const std::array<std::optional<ColorMatrix>,4>&)`
   — per texel `out = lerp(out, M_i(base), m_i)` with `m_i` = the mask's
   luminance (DXTA masks decode to gray RGB); alpha untouched.
2. **`atlas.h`** — pure:
   `std::optional<BlitRect> piece_rect(const BlitRectSet&, float umin, float vmin, float umax, float vmax)`
   — the rect containing the UV box (in 1024-atlas px, 2 px tolerance);
   `ImageRgba crop_piece(const ImageRgba& tex, const BlitRect&)` — texture at
   2x anchored at the rect origin: the `(w/2) x (h/2)` top-left block, padded
   with transparent pixels where the texture is smaller;
   `void remap_uv(float& u, float& v, const BlitRect&)` — `u' = (u*1024-x0)/(x1-x0)`.
3. **`piece_export.h`** —
   `PieceExportResult export_piece(const PieceContext&, const ManifestPiece&, const std::string& glb_path)`
   where `PieceContext` holds the open `Gw2Dat`, the parsed `Composite`, the
   race key (`race + gender`), and armor material per dye slot.
   - **Armor** (composite entry for `race_key` + `skin_token` exists): load
     `mesh_base` with `extract_entry`; decode base/normal/masks/cut; bake dyes
     with the manifest's dyes (slot i -> mask i); find the rect from the
     meshes whose UVs fit a rect; crop the baked diffuse, normal and cut;
     multiply the cut into diffuse alpha; for each of those meshes set the
     material's diffuse/normal to the cropped textures and remap UVs; other
     meshes (Skin) keep no texture; export with `export_model_gltf`.
   - **Weapon/Back/other with a model** (no composite entry): export
     `file_ids[0]` as-is.
   - **No skin / unresolved**: skipped with a reason.
4. **`character_export.h`** —
   `CharacterExportReport export_character(const CharacterManifest&, const std::string& dat_path, const std::string& out_dir)`:
   opens the dat, finds the Composite file (index DB entry with chunk `comp`;
   fallback fileId 154681), parses it once, exports every piece to
   `<out_dir>/<NN>_<Slot>_<skin name>.glb`, writes `manifest.json` and
   `export_report.json` (per piece: file, status, mesh/texture fileIds, reason).

### Front ends
- **CLI** `gw2dat_cli character-export --manifest m.json --dat <Gw2.dat> --out <dir>`
  (needs the index DB open: `--index`, default `dumps/index/gw2_index.db`).
- **UI**: Character Ripper gains **Export pieces...** (folder picker, runs on a
  worker thread, status line reports "N of M pieces exported").
  "Open model" switches to the race-correct mesh when the Composite is loaded.

## Error handling
- No dat / no index / no Composite: export refuses with a message naming
  what to open.
- A piece whose model or textures fail to decode is reported and skipped; the
  rest still export.
- Missing dye mask or unknown color shift: that channel stays undyed
  (reported).

## Testing
- Unit (no dat): composite parser on a hand-built packfile (pointer math,
  filerefs, packed structs, bad pointers -> nullopt); dye matrix against all
  52 fixture colors (exact `rgb`); bake on tiny images; `piece_rect`,
  `crop_piece`, `remap_uv` on hand values; manifest JSON with/without the new
  fields; resolver fills `skin_token`.
- Live (skips without `GW2_TEST_DAT`, set to `E:\Games\gw2\Guild Wars 2\Gw2.dat`
  locally): Composite has 29 races; SylvariFemale + Warden Coat token ->
  mesh 40405, base 151455; exporting that piece yields a .glb whose diffuse
  is 192x256 and whose armor UVs lie in [0,1].
- Manual: export Musa Blossom's pieces and open them in Blender (screenshot
  check via castlemist's own model preview where possible).
