# Texture Gallery (castlemist app) — design brief

Approved by the user on 2026-10-25 in the session that surveyed GW2's UI art for a GW2-styled
GMMenu (VRChat world, GW2Porting project). This brief is the hand-off to a new session: build
the gallery as designed below. Out of scope: the GMMenu reskin itself (it uses the gallery
afterwards).

## Why

Picking GW2 UI art (window panels, title bars, buttons, checkboxes, close icons) means scanning
thousands of textures. The app previews one dat entry at a time; static contact sheets were too
slow. A thumbnail browser over the whole dat fixes that and stays useful for anything else.

## What exists to build on

- `src/ui/index_ui.cpp`, `castlemist/db/index_db.h`: the gw2index SQLite
  (`dumps/index/gw2_index.db`, table `entries(base_id, magic, type, size_final, ...)`).
  `magic` / `type` give every entry's kind without decoding: `ATEX`/`ATEU`/`ATEP`/`CTEX`
  (type `texture`), `DDS ` (`dds`), `RIFF` (`riff`, WebP), `.PNG` (`png`).
- `castlemist/extract/entry_extractor.h`: `extract_entry(const std::string& file_path,
  const MftData&)` is the background-thread-safe decode (fills `preview_pixels`,
  `preview_width/height`, `is_image`). Base id = MFT index + 1.
- `castlemist::format::img` (`sdk_image.h`) decodes WebP / PNG / JPEG.
- `src/ui/look_dialog.cpp` draws an owner-drawn thumbnail grid from `HBITMAP`s
  (`to_bitmap`, `CreateDIBSection`); `src/ui/texture_panel.cpp` makes thumbnails too.
- `src/ui/content_browser.cpp` shows how a browser pushes an entry into the main preview
  (`castlemist::gfx::set_texture(...)`, `castlemist::texpanel::set_model`).
- `gw2dat_cli texbatch` (added 2026-10-25) reads headers / writes PNGs for a list of base ids in
  one dat load: handy for checking the gallery's decode against known output.

Survey facts (map 506592 session): ATEU = 46,766 entries; 25,862 are 64x64 and 6,734 128x128
(icons). Filtering ATEU to DXT3/DXT5/BC7/DXT2, non-square-icon sizes, longest side <= 1024 leaves
~2,320 entries, which hold the window art (painted panels 19234-19389, ragged window masks
19665/19669/19673/20097, title/tab bars 21924-21933, dark panels 21951-21982, checkbox/radio
atlas 42001, plain close X 63375/63376, highlight brushes 22054-22073). The 100 WebP and 216 PNG
entries are mostly launcher art; the guild-panel icons are PNGs around 487630-487890.

## Design (approved)

1. **Window**: View > Texture Gallery... opens a resizable top-level window.
2. **Source list**: every image entry from the index (texture / dds / riff / png types), read
   once; no decoding to list.
3. **Filters** (top bar):
   - Kind checkboxes: UI (ATEU), World (ATEX/ATEP/CTEX), Web/launcher (WebP/PNG), DDS.
   - Size: min / max of the longest side; "exclude square icon sizes" (16, 32, 64, 128).
   - Has alpha: from the format (DXT3/DXT5/BC7/DXT2, PNG/WebP with alpha).
     (Format and size need the texture header: read it lazily, or extend the index; see notes.)
   - Search: one id, a range (`21900-22100`) or a pasted list.
4. **Grid**: owner-drawn, virtual scrolling; thumbnails 64 / 96 / 128 px with id and size under
   each, checkerboard behind alpha. Only visible cells decode, on a background worker pool, into
   a memory-capped LRU cache; scrolling 46k entries stays smooth.
5. **Interaction**: click shows the texture full size in the main preview (the existing texture
   view and info panel); Ctrl / Shift multi-select; "Export selected..." writes `<id>.png` into a
   chosen folder; "Copy IDs" puts the selection on the clipboard (how the user hands picks over).
6. **Code**: `src/ui/texture_gallery.cpp` (window + grid) and a small testable unit under
   `src/ui/detail` (filter + id-list parsing, thumbnail cache); a menu entry in the app.
7. **Tests**: unit tests for the filter and id-range parsing and the cache eviction; build the
   app and screenshot the gallery filtered to UI, icons excluded (the ~2,320 candidates above).

## Notes for the implementer

- Width / height / format are not in the index table; reading the 12-byte ATEX header needs a
  decompress of the entry's start. Options: read headers lazily as cells come into view (the
  filter then refines as headers arrive), or a one-off header scan cached to a side file
  (`texbatch` does all 46,766 ATEU headers in ~55 s). Pick one and say which.
- Build with ucrt64 first on PATH and TMP set (memory reference-build-env); only castlemist's own
  code opens the dat. Match the surrounding code's style (comment density, naming).
- The user prefers no per-task reviews and no full spec documents for small work.
