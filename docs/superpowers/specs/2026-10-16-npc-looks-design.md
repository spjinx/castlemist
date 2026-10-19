# NPC Looks — Design (stage 4 of the NPC catalogue)

Date: 2026-10-16
Status: record. The user chose to extend the composite parser additively and to leave model NPCs until they have an example.

## Goal

Race-body NPCs (humans, charr, norn, sylvari, asura and "creature" body types; 9 986 of the 23 653 characters) export
and preview fully dressed: body, face, hair, armour pieces, dyes, skin/hair/eye colours and body shape.

Out of scope here: model NPCs that look incomplete in the viewer (waiting on an example from the user); display names.

## What the data says (measured 2026-10-15 on the live dat, through castlemist's CLI)

- A type-67 character's variable tail holds **look records** of 32 bytes: `{u64 race token, u64 look token, u64 flags, u64 0}`.
  Both tokens are 5-bit packed names, e.g. race `humanfemale`, look `xpkurzwar`. An NPC can have several records (one per
  race or random variant). 9 986 characters carry at least one, with 3 805 distinct look tokens. The most used are
  `banditm` (179), `banditmd` (142) and `vigilhvysrv` (113).
- The looks are defined in the **composite** file (`cmpc`, chunk `comp`, `PackCompositeV20`), per race/gender in
  `raceSexData[].variants[]` (`PackCompositeVariantV20`):
  `{token, boneScaleIndex, components[{nameToken, color0..3}], eyeColor, hairColor, hairColor2, patternColor, skinColor, skinIndex, skinStyle}`.
  Each colour is `PackCompositeColorV20 {byte brightness, contrast, hue, saturation, lightness}`. The race record also has
  `variantRefRace` (u64).
- 3 781 of the 3 805 look tokens occur in the composite file, covering 16 547 of 16 578 look references (99.8%).
- The content store holds no composite tokens for these NPCs (checked to 3 refs deep), so the composite variants are the
  only source.

## Ground rules

- **Extend `format/composite` additively** (user decision). Add `CompositeVariant` and `CompositeRace::variants` /
  `variant_ref_race`. Existing fields, names and behaviour stay unchanged, and the existing composite tests still pass
  unmodified.
- Do not modify content_map, content_schema, content_store, type_names, or any existing label/name. Additive CLI output
  fields are fine.
- Never read Gw2.dat directly.

## Design

1. **Parser:** `parse_composite` also reads each race's `variants` (layout above) and `variantRefRace`.
2. **Look records:** `NpcRecord` gains `std::vector<NpcLook> looks` (`{race, look_token}`, in record order). The existing
   `race` field stays as is (the first record's race). Cache version 3.
3. **Variant → assembly inputs** (`npc/look.h`, npc layer gains a dependency on ripper):
   `npc_assembly(const NpcRecord&, size_t look_index, const Composite&) -> optional<{CharacterManifest, AssemblyOptions, notes}>`.
   - Find the race record whose `name`, lowercased, starts with the look's race token. The token is truncated at 12
     letters (`sylvarifemal`).
   - Find the variant whose `token` equals the look token. If it's absent there and `variant_ref_race` names another
     race, look in that race.
   - Each component token found in the race's `file_data` with type 8–14 becomes a `ManifestPiece`. Its slot comes from
     the piece type (8 Boots, 9 Coat, 10 Gloves, 11 Helm, 12 Leggings, 13 HelmAquatic, 14 Shoulders), with `skin_token`
     set to the component token, status Ok, and up to four dyes from color0..3.
   - A component whose token is one of the race's `faces` or `hair_styles` sets `options.face` / `options.hair` to its
     index.
   - The race's `ears` and `skin_styles` are handled the same way when present.
   - `boneScaleIndex` → `options.physique`; `skinStyle` → `options.skin_style`.
   - Colours: skinColor → skin_tint, hairColor/hairColor2 → hair_tint/hair_tint2, eyeColor → eye_tint,
     patternColor → pattern_tint.
   - `manifest.race` and `gender` come from the race name (e.g. "Human", "Female"). For a creature race, use the composite
     race name as given.
   - Anything unresolved becomes a note: an unknown token, a missing variant, a colour conversion not proven.
4. **Colour bytes → DyeShift.** This must be **measured, not assumed**. Test candidate byte→float mappings against the
   content map's dye palettes (`cmap::palette`, which holds `ColorShift` floats for every dye colour). The right mapping
   makes most variant component colours equal (within quantisation) to some real palette colour. If no candidate reaches
   ≥ 90%, ship with tints off (textures as authored) and add the note `"colours not applied: byte format unproven"`.

   **Measured 2026-10-16:** over 6 race/genders (HumanFemale/Male, CharrMale, NornFemale, SylvariMale, AsuraFemale;
   114 828 non-zero, non-neutral colour tuples) against every colour of all 85 content-map palettes (2 070 distinct
   shifts, all materials). Best mapping: `brightness = byte - 128`, `contrast = byte/128`, `hue = byte * 360/256` degrees,
   `saturation = byte/128`, `lightness = byte/128`; `{128,128,0,128,128}` is the identity shift. Exact (quantised) hit
   rate against a real palette colour: 79.3% overall (components 78.9%; with only each colour's first material 63.0%;
   skin 83.8%, eye 85.7%, pattern 86.5%, hair 78.6%, hair2 76.4%). Rivals: hue as raw degrees 3.1%, hue*255/360 46.7%,
   unsigned brightness 0%, brightness*2+128 4.2%, sat/light swapped 0.5%, contrast/sat/light *100 0%. The misses are hand-tuned
   colours a few steps (often multiples of 5) from a palette entry, plus two sentinel tuples (3,0,181,219,3) and
   (0,102,219,3,0) that occur in pairs.
   **Decision (controller, 2026-10-16):** the 90% bar was the wrong test, because variants use hand-tuned colours that
   are in no palette. 79.3% exact against 0-47% for every rival mapping, and an identity tuple that decodes to no shift,
   is enough: `variant_color_shift` returns the measured mapping for every tuple, `npc_assembly` applies component dyes
   and the skin/hair/hair2/eye/pattern tints, and adds the single note "colours: measured mapping (79.3% exact palette
   match)". `boneScaleIndex` is never applied as a physique (values run to 153 against 14 presets): physique stays -1
   with the note "physique N: not in composite" (silent for 0xFFFFFFFF).
5. **Export:** `export_npc` for a race-body NPC with a resolvable look writes `body.glb` with
   `ripper::assemble_character(manifest, dat_path, body.glb, options)`, using look 0 by default. The manifest gains
   `looks` (all records), `look_used` and the assembly report's parts. The CLI `npc-export` gains `--look N`.
   `npc-show` prints the looks.
6. **UI:** Export already goes through `export_npc`, so race NPCs become dressed with no UI change.

## Testing

- **Unit:**
  - composite variants parsed from a synthetic composite (following tests/test_format_composite.cpp);
  - look records read with a type guard on the race token;
  - `npc_assembly` mapping from a synthetic Composite (pieces, face/hair indices, physique, notes);
  - the colour conversion function on its measured examples.
- **Real data:**
  - `composite-types` reports variant counts per race;
  - `npc-export` of a `banditm` NPC writes a `body.glb` whose assembly report has `used` parts for hair, face and at
    least one armour piece;
  - glb header check: glTF version 2, ≥ 1 skin.
