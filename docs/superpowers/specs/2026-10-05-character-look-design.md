# Character Looks -- Design (sub-project 4 of the Character Ripper)

Date: 2026-10-05
Status: written autonomously at the user's direction; decisions recorded here.
Builds on: `2026-10-04-character-assembly-design.md`.
Research: `docs/research/gw2-armor-skins-and-dyes.md` section 6.

## Goal

Let a ripped character carry the look the API can't give -- face, hair style,
skin colour, hair colour -- chosen from the game's own character-creator
options, saved per character locally, and applied on export.

## Decisions

- **Source of truth is the dat**, not the API (no appearance there) and not
  the running game (third-party policy, fragile). The user matches by eye,
  like a makeover kit.
- **Colours are colour ids** from the race's palettes (stable across builds),
  0 = as authored. Styles are indices into the Composite race lists.
- **Palettes come from the content map** (cmap::palette, cached with it). The
  race -> palette table is fixed in `ripper/look.cpp` (identified by colour
  against the wiki; the dat only names palettes obfuscated).
- **Storage:** `character_looks.json` beside `api_keys.json`, git-ignored,
  keyed by character name.
- **Applying:** skin shift on body/face/ears (and hair texels outside the hair
  masks); hair colours on the hair's dye channels; hair colour 2 defaults to
  hair colour. Armor keeps its own dyes; its skin patches show the tinted body.
- Eye colour, sylvari pattern/glow, norn tattoos: later (palettes partly known).

## Components

1. `format/content_map`: type-9 colours and type-147 palettes, resolved
   in-pack and across packs, in the cache (GC8N).
2. `character/look_store`: `CharacterLook`, `LookStore`, `default_look_file()`.
3. `ripper/look`: `race_palettes`, `palette_shift`, `swatch_rgb`, `apply_look`.
4. `ripper/assemble`: `AssemblyOptions::skin_tint/hair_tint/hair_tint2`;
   `bake_dyes(..., rest)` for the skin colour outside dye masks.
5. CLI: `palette`, `look-options`, `look-set`, and `character-assemble`
   applies the saved look (`--skin-color/--hair-color/--hair-color2/--face/
   --hair` override, `--no-look` skips).
6. UI: a Look row in the Character Ripper -- face / hair style spinners, skin
   and hair colour swatch pickers -- saved per character and used by Export
   character.

## Testing

Unit: palette parse (local + external colour, cache round trip); look store
round trip / malformed file; `bake_dyes` rest colour. Live: sylvari female
options (21 faces, 38 hair styles, 96 skin colours; hair swatches = wiki);
Musa assembled with skin 310 / hair 123 / hair style 5 rendered in Blender.
