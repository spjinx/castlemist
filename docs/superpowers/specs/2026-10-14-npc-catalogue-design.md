# NPC Catalogue — Design

Date: 2026-10-14
Status: record (written under the standing "keep going" instruction; not waiting on sign-off)

## Goal

A catalogue of every NPC, creature and enemy in the client's data that you can
**browse** in castlemist and **export** per NPC to Blender or Unity. Each entry
lists its body (model or race), movement set (animation clips), weapons and
ability visuals (skill references).

## What the user decided

- **Abilities mean visuals only:** animation clips plus the files a skill uses
  (effect models, sounds). No gameplay data (damage, AI, cooldowns, stats).
- **Layered entries.** A *creature* is one body model. *Variants* are the NPCs
  that use it; re-skins of one model as different creatures are common (one
  model, fileId 104256, is the body of 587 content objects).
- **Purpose.** NPCs brought back to life in Blender, Unity or the World
  Engine, plus quick lookup in the app.

## Ground rules (from this session)

- **The content map is the authoritative map.** The catalogue reads it
  (`cmap::objects()`, `resolve_all()`, `users_of()`, `item_links()`,
  `resolve()`) and **does not change it, the shared cntc readers
  (`content_schema`, `content_store`) or any labels**. Everything new lives
  in a new `npc` layer, plus additions to the CLI and UI.
- **Never touch Gw2.dat directly.** All reads go through castlemist's own dat
  and index code.
- Nothing is renamed. If an existing name looks wrong, it is reported, not
  changed.

## Data model

Content type **67** is the character definition. The content map already
holds every type-67 object, keyed by dataId, with its asset fileIds in object
order. Measured on the 2026-10 dat: 23 653 objects. Their assets are an
optional AMSP sound script, then an optional rigged MODL body.

| field | source |
|---|---|
| `id` (dataId), body `model` fileId, `sound_script` fileId | content map: `resolve_all(67, id)`, classified by the index DB container (`MODL` / `AMSP`) |
| `codename` (internal slug, e.g. `vl8Av.4gynM`) | `cmap::name_for_fileid` is session-only and keyed by file, so the npc reader reads the object's own string fixups |
| `weapons[3]` (item dataIds) | npc reader: refs at **+192 / +200 / +208**, kept only if the target is an Item (type 35) whose itemType @+44 is 24 (Weapon) |
| weapon skin and model | content map: `item_links(item)` → skin dataId → `resolve(SKIN, skin)` |
| `skills[]` (skill dataIds) | npc reader: refs at **+120 / +168**, kept only if the target is type 64 |
| `race` (composite body key, e.g. `humanfemale`, `creaturehm`) | npc reader: first u64 in the object that decodes (5-bit letters, low bits first) to `(human|charr|norn|sylvari|asura)(male|femal…)` or `creature[a-z]{2,3}` |
| `modifier` (`undead` / `ghost`) | npc reader: u64 at +88 decoding to exactly that word |

**How the npc reader follows references** (the same two rules the content map
already uses for item → skin):

1. A `fileIndices` slot is an index into the one shared fileRefs table.
2. An `externalOffsets` targetFileIndex names a pack by its position in
   fileId order, counted from the pack that carries fileRefs. `localOffsets`
   point inside the same pack.

The npc reader parses packs with the existing read-only
`cschema::parse_content_pack` / `get_objects` and does its own pass over the
cntc entries, the same way `cmap::build` gets them.

**Slot offsets are empirical.** Each one is guarded by a check of the
target's type. An offset that misses yields nothing rather than a wrong
value, and the catalogue reports per-field coverage counts, so a game patch
that moves a field shows up as a drop in coverage.

**Not in the client data or not found yet** (recorded, not guessed):
- **Display names.** Not located; entries are labelled by codename and
  dataId. Skill names do come from the public API (castlemist already caches
  them in `content_names.tsv`).
- **Humanoid outfits.** Race-body NPCs reference no armour items. These
  entries carry `race` and weapons only, and export writes no body for them
  (see Export).
- **Scale, tint and spawn placement in maps.** Not decoded.

## Creatures and clips

- A **creature** is a distinct body `model` fileId together with the NPCs
  that use it.
- **Clips** come from `load_model_by_fileid(model)`, which already merges the
  model's imported animation banks. Clip names are the Granny names (e.g.
  `wgalunge08`).
- For browsing, each clip gets a grouping key: the name with trailing digits
  and spaces removed and, when it ended in a variant number and starts with
  three letters, those three letters split off as a prefix (`wga` + `lunge`;
  `zeropose` stays whole). This is only a grouping;
  how the client picks a clip for a skill is unproven, so nothing claims a
  skill-to-clip link.

## Architecture

```
ui (NPC window) ─┐            gw2dat_cli (npc-list / npc-show / npc-export)
                 └── npc ── exportgltf ── extract ── format(cmap, cschema) ── db
```

- **New layer `npc`** (`include/castlemist/npc/`, `src/npc/`, depends on
  extract and exportgltf):
  - `catalogue.h` — `NpcRecord`, `Catalogue`, `build_catalogue(dat_path,
    cntc_entries, file_ids, progress)` (needs the content map loaded),
    `save`/`load` (JSON, `npc_catalogue.json` beside the exe), `coverage()`.
  - `tokens.h` — 5-bit packed-name decode and the clip grouping key.
  - `export.h` — `export_npc(dat, record, out_dir)`.
- **CLI:**
  - `npc-list [--model F] [--race R] [--limit N]`
  - `npc-show --id N`, which adds the clip list
  - `npc-export --id N --out-dir D`

  Each builds or loads the content map and the catalogue the way `users`
  does.
- **UI:** a **Tools ▸ NPC catalogue…** window. It has a filter box (id,
  codename, race, model fileId, skill name), a creatures list (model, variant
  count), a variants table for the selected creature (id, codename, race,
  weapons, skills), a clip list, and buttons: **Open model** (the existing
  open-by-fileId path), **Export…** (folder picker). The catalogue builds on a
  background thread after the content map, following
  `content_map_service`'s pattern.

## Export

`export_npc` writes `<out>/<id>_<codename>/` containing:

- **`body.glb`** — `load_model_by_fileid(model)` then `export_model_gltf`:
  skeleton, skin, textures and every clip. Only when the NPC has a body
  model.
- **`weapon_<slot>.glb`** — each weapon's skin model, the same way.
- **`manifest.json`** — id, codename, model, race, modifier, clips (name,
  group, duration), weapons (slot, item, skin, model, file), skills (id,
  fileIds), and `notes` for anything not exported (race-body NPC without
  outfit data; weapon with no resolvable skin).

Weapons are not attached to bones; which bone a weapon attaches to isn't
known for NPCs. Blender and Unity import `body.glb` and the weapons
separately. World Engine placement is out of scope until spawn data is found.

## Errors

- A missing content map leads to a clear error ("build the content map first")
  and no partial catalogue.
- A failed pack read or parse is counted and listed in `Catalogue::warnings`,
  and the build continues.
- An export step that fails (model load, glb write) is recorded in the
  manifest `notes`; the other files are still written.

## Testing

- **Unit tests (`cm_test_npc`)** on synthetic packs, using the
  PackBuilder approach from `test_format_content_schema.cpp`. They cover:
  weapon, skill and race extraction with the type guards; external refs by
  fileId order; token decode; clip grouping; and the JSON round trip.
- **Real-data checks through the CLI** (not the dat directly):
  - catalogue counts: about 23.6k NPCs, about 13.5k with a body model;
  - `npc-show` for a model-104256 NPC lists its clips;
  - `npc-export` writes a `body.glb` whose header and JSON chunk parse, with
    one glTF animation per clip and a skin.

## Stages (one plan each)

1. **Data:** the `npc` layer, catalogue, cache, `npc-list` and `npc-show`.
2. **Export:** `export_npc` and `npc-export`.
3. **UI:** the NPC catalogue window.

## Open research (not blocking)

- Display-name textId for characters.
- Humanoid NPC outfit data.
- The skill → clip selection rule.
- NPC spawn placement in maps.
