# NPC Catalogue — Stage 1 (Data) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new `npc` layer that builds the NPC catalogue (every type-67 character with model, sound script, race, weapons, skills), caches it as JSON, and exposes it through `gw2dat_cli npc-list` / `npc-show`.

**Architecture:** A read-only cntc pack parser private to the layer reads character fields. Body model and sound script come from the content map (`cmap::resolve_all`), and weapon skins from `cmap::item_links`. Nothing in format/extract/db changes.

**Tech Stack:** C++20, MinGW UCRT64 gcc, CMake/Ninja, nlohmann::json, castlemist test framework (`CM_TEST`, `CHECK`, `CHECK_EQ`).

**Spec:** `docs/superpowers/specs/2026-10-14-npc-catalogue-design.md`

## Global Constraints

- Do not modify `content_map`, `content_schema`, `content_store`, `type_names` or any existing label. Add, never rename.
- Never read Gw2.dat directly; real-data checks go through `gw2dat_cli` with `--dat "E:/Games/gw2/Guild Wars 2/Gw2.dat" --index <main checkout>/dumps/index/gw2_index.db`.
- Build and test only through `scratchpad/cmbuild.sh` (`build [targets]`, `test`): it fixes PATH (ucrt64 first) and TMP.
- Weapons: refs at +192/+200/+208, kept only when the target is type 35 with u32 @+44 == 24. Skills: refs at +120/+168, kept only when the target is type 64.
- Race: first u64 decoding to `^(human|charr|norn|sylvari|asura)(male|femal[a-z]*)$` or `^creature[a-z]{2,3}$`. Modifier: u64 at +88 decoding to exactly `undead` or `ghost`.
- Cache file: `npc_catalogue.json` beside the exe (CLI: `exe_dir()`), with `"version": 1`.

## Review Focus

- A ref at a weapon or skill slot that targets another type (e.g. a type-140 record): drop it. Test `drops_a_slot_ref_whose_target_has_the_wrong_type`.
- A type-67 object with no assets (absent from the content map): still listed, with model 0. Test `lists_a_character_the_content_map_does_not_hold`.
- A pack with fewer than 11 array descriptors, or a truncated one: skip it, add a warning, keep going. Test `a_truncated_pack_is_a_warning_not_a_crash`.
- A token whose 5-bit groups include 27–31: it is not a name (empty). Test `rejects_groups_above_z`.
- A corrupt or old-version cache file: `load` returns nullopt and the CLI rebuilds. Test `load_rejects_wrong_version`.

---

## File structure

- `include/castlemist/npc/tokens.h`, `src/npc/tokens.cpp` — `decode_token`, `clip_group`, `clip_prefix`.
- `src/npc/pack.h`, `src/npc/pack.cpp` — private pack parser and pack set (layer-internal header).
- `include/castlemist/npc/catalogue.h`, `src/npc/catalogue.cpp` — records, build, coverage, JSON save/load.
- `tests/test_npc.cpp` — `cm_test_npc` (PRIVATE_HEADERS `src/npc`), including a `PackBuilder` copied from `tests/test_format_content_schema.cpp` (same layout: PF/"cntc", "Main" @12, flags @28, 11 descriptors {u32 count, i64 self-rel ptr} @32).
- Modify `CMakeLists.txt` (add layer after `exportgltf`: `castlemist_add_layer(npc DEPS castlemist::extract castlemist::exportgltf ext::json)`), `tests/CMakeLists.txt` (target), `tools/gw2dat_cli/CMakeLists.txt` or wherever `gw2dat_cli` links (add `castlemist::npc`), `tools/gw2dat_cli/main.cpp` (commands).

### Task 1: Layer scaffold and tokens

**Files:** Create `include/castlemist/npc/tokens.h`, `src/npc/tokens.cpp`, `tests/test_npc.cpp`; modify `CMakeLists.txt`, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
- `std::string castlemist::npc::decode_token(uint64_t token)` — clear the top 4 bits, then read 5-bit groups from the low end: 1..26 = `a`..`z`, 0 = space. Stop when the remaining value is 0. Return "" if any group is greater than 26, or the result is empty.
- `std::string clip_group(const std::string& name)` — strip trailing digits and spaces. If the name *had* trailing digits and the remainder is longer than 3 with 3 leading letters, drop those 3. Then trim spaces from both ends.
- `std::string clip_prefix(const std::string& name)` — the 3 dropped letters, or "".

- [ ] **Step 1: Failing tests.**
  - `decodes_a_packed_clip_name`: `decode_token(0x80000029dd5604f7) == "wgalunge"`, and `decode_token(0x2cdf07c8ba) == "zeropose"`.
  - `rejects_groups_above_z`: `decode_token(0x1f) == ""`.
  - `groups_clips_by_action`: `clip_group("wgalunge08") == "lunge"`, `clip_prefix("wgalunge08") == "wga"`, `clip_group("uamland    l08") == "land    l"`, `clip_group("zeropose") == "zeropose"` and `clip_prefix("zeropose") == ""` (no variant number, so no prefix split). The spec says the grouping is only for browsing.
- [ ] **Step 2:** `cmbuild.sh build cm_test_npc` fails (no layer).
- [ ] **Step 3:** Implement and add the layer and test target.
- [ ] **Step 4:** `cmbuild.sh build cm_test_npc && build/debug/bin/cm_test_npc.exe` reports all passed.
- [ ] **Step 5:** Commit `feat(npc): layer scaffold and packed-name tokens`.

### Task 2: Pack parser and pack set

**Files:** Create `src/npc/pack.h`, `src/npc/pack.cpp`; extend `tests/test_npc.cpp`.

**Interfaces — Produces (namespace `castlemist::npc::detail`):**
```cpp
struct Ref { uint32_t field; uint32_t target_file_index; uint32_t target_offset; bool external; };
struct Object {
    uint32_t type = 0, begin = 0, end = 0;          // [begin,end) in Pack::content
    std::vector<Ref> refs;                          // localOffsets + externalOffsets inside the object
    std::vector<std::pair<uint32_t, uint32_t>> files;  // {field, fileRefs index}
    std::vector<std::pair<uint32_t, std::string>> strings;  // {field, string}
    uint32_t data_id = 0;                           // u32 @+40, read at parse
};
struct Pack {
    uint32_t base_id = 0, file_id = 0;
    std::vector<uint32_t> file_refs;                // array 2 decoded via cschema::decode_fileref_pair
    std::vector<uint8_t> content;                   // array 10
    std::vector<Object> objects;                    // sorted by begin, duplicates dropped
    const Object* object_at(uint32_t begin) const;  // exact start, else nullptr
    uint32_t u32(const Object&, uint32_t field) const;  // 0 past end
    uint64_t u64(const Object&, uint32_t field) const;
};
std::optional<Pack> parse_pack(std::span<const uint8_t> bytes, uint32_t base_id, uint32_t file_id);
struct PackSet {
    std::vector<Pack> packs;
    void finish();                                  // order packs by file_id from the fileRefs pack
    const std::vector<uint32_t>& shared_file_refs() const;
    // Resolve a ref from packs[from]: local -> same pack; external -> the pack at
    // position target_file_index counted in file_id order from the fileRefs pack.
    std::pair<const Pack*, const Object*> resolve(size_t from, const Ref& r) const;
};
```
Arrays: 3 indexEntries (16 B, type @+0, offset @+4), 4 localOffsets (u32 reloc; the u32 at content[reloc] is the target offset), 5 externalOffsets (8 B {reloc, tfi}; the u32 at content[reloc] is the target offset), 6 fileIndices (u32 reloc; u32 at content[reloc] = fileRefs index), 7 stringIndices (u32 reloc; u32 at content[reloc] = strings index), 9 strings (8-byte self-rel ptr → UTF-16, ASCII-narrowed), 10 content.

- [ ] **Step 1: Failing tests.**
  - `parses_objects_refs_files_and_strings`: a pack with two objects (types 67 and 35); a local ref at the 67's +200 → the 35; a fileIndices slot at +240 holding index 1; a string slot naming `"vl8Av.4gynM"`. The parsed object has those three entries, and `resolve` returns the type-35 object.
  - `resolves_external_refs_by_file_id_order_from_the_file_refs_pack`: three packs with baseIds out of order (fileIds 900 = fileRefs pack, 901, 902). A tfi=2 ref from the 901 pack resolves into the 902 pack.
  - `a_truncated_pack_is_a_warning_not_a_crash`: `parse_pack` of the first 40 bytes is nullopt.
- [ ] **Step 2:** Run; fails to compile.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Tests pass.
- [ ] **Step 5:** Commit `feat(npc): read-only cntc pack parser with in-pack and cross-pack refs`.

### Task 3: Character fields

**Files:** Modify `src/npc/pack.h` / add `src/npc/character.cpp`; extend tests.

**Interfaces — Produces:**
```cpp
struct CharacterFields { uint32_t id = 0; std::string codename, race, modifier;
                         std::vector<std::pair<uint32_t,uint32_t>> weapons;  // {slot field, item dataId}
                         std::vector<uint32_t> skills; };                  // skill dataIds, slot order
CharacterFields read_character(const PackSet&, size_t pack_index, const Object&);  // namespace detail
```
Codename = the first string that `is_content_slug` accepts (declared in `castlemist/extract/content_types.h`).

- [ ] **Step 1: Failing tests.**
  - `reads_weapons_skills_race_and_modifier`: a weapon item at +200 (type 35, @+44 = 24, dataId 76158); a skill at +120 (type 64, dataId 5491); the u64 `humanfemale` packed at +632; `undead` at +88. Expect weapons `{{200, 76158}}`, skills `{5491}`, race `"humanfemale"`, modifier `"undead"`.
  - `drops_a_slot_ref_whose_target_has_the_wrong_type`: +192 → a type-140 object; +168 → a type-35 object. Weapons and skills are empty.
  - `a_non_weapon_item_is_not_a_weapon`: +200 → type 35 with @+44 = 0 (Armor). Weapons are empty.
- [ ] **Step 2–4:** fail, implement, pass.
- [ ] **Step 5:** Commit `feat(npc): character weapons, skills, race and modifier with type guards`.

### Task 4: Catalogue, coverage and cache

**Files:** Create `include/castlemist/npc/catalogue.h`, `src/npc/catalogue.cpp`; extend tests.

**Interfaces — Produces (namespace `castlemist::npc`):**
```cpp
struct Weapon { uint32_t slot = 0, item = 0, skin = 0, model = 0; };
struct NpcRecord { uint32_t id = 0; std::string codename; uint32_t model = 0, sound_script = 0;
                   std::string race, modifier; std::vector<Weapon> weapons; std::vector<uint32_t> skills; };
struct Coverage { size_t npcs = 0, with_model = 0, with_race = 0, with_weapons = 0, with_skills = 0; };
struct Catalogue {
    std::vector<NpcRecord> npcs;                     // sorted by id
    std::vector<std::string> warnings;
    Coverage coverage() const;
    const NpcRecord* find(uint32_t id) const;
    std::vector<const NpcRecord*> by_model(uint32_t model) const;
};
using ContainerOf = std::function<std::string(uint32_t file_id)>;  // index-DB container ("MODL", "AMSP", ...)
Catalogue build_catalogue(const std::vector<castlemist::cmap::PackBytes>& packs, const ContainerOf& container_of);
Catalogue build_catalogue(const std::string& dat_path, const std::vector<MftData>& entries,
                          const std::vector<uint32_t>& file_ids, const ContainerOf& container_of,
                          const std::function<void(size_t, size_t)>& progress);
bool save(const Catalogue&, const std::filesystem::path&);
std::optional<Catalogue> load(const std::filesystem::path&);   // nullopt unless "version" == 1
```
Model and sound script are the first fileIds in `cmap::resolve_all(67, id)` whose container is `MODL` or `AMSP` respectively. Weapon skin is the first `cmap::item_links(item)` entry with `type == cmap::CONTENT_TYPE_SKIN`, and its model is `cmap::resolve(CONTENT_TYPE_SKIN, skin)`. The content map must already be built or loaded; if `!cmap::built()`, return an empty catalogue with the warning `"content map not loaded"`. The dat overload decompresses each entry the way `cmap::build` does (`read_entry_bytes` + method0, using the 8-byte header skip seen in `content_map.cpp: decompress`).

- [ ] **Step 1: Failing tests.**
  - `builds_records_from_the_content_map_and_character_fields`: synthetic packs containing a fileRefs pack, a type-67 with +224/+240 file slots and a +200 weapon, an item with a +176 skin link, and a skin. After `cmap::build_from_packs` on the same packs and a `container_of` stub mapping the two fileIds to AMSP/MODL, the record has model, sound_script, weapon{200, item, skin, skin model}. Coverage: npcs 1, with_model 1, with_weapons 1.
  - `lists_a_character_the_content_map_does_not_hold`: a 67 with no file slots is present with model 0.
  - `without_a_content_map_the_catalogue_is_empty_with_a_warning`.
  - `round_trips_through_json` and `load_rejects_wrong_version`.
- [ ] **Step 2–4:** fail, implement, pass.
- [ ] **Step 5:** Commit `feat(npc): catalogue from the content map plus character fields, JSON cache`.

### Task 5: CLI `npc-list` / `npc-show` and real-data check

**Files:** Modify `tools/gw2dat_cli/main.cpp` and the CLI's CMake link line.

**Interfaces — Consumes:** Task 4. **Produces:**
- `ensure_npc(a)` — load `npc_catalogue.json` (or `--npc-cache P`); otherwise `ensure_cmap(a)` and build with the cntc entries collected exactly as `ensure_cmap` does. `container_of` = `castlemist::db::lookup(get_by_base_id(dat, fid)).container`. Then save.
- `npc-list [--model F] [--race R] [--limit N]` → `{"ok", "coverage", "warnings", "npcs":[{id, codename, model, race, modifier, weapons:[item...], skills:[...]}]}`. Default limit is 100.
- `npc-show --id N` → the record, plus `clips:[{name, group, prefix, duration}]` from `load_model_by_fileid(dat, model)->animClips` when the model is non-zero.

- [ ] **Step 1:** Implement both commands (CLI glue; the logic is tested in Tasks 1–4).
- [ ] **Step 2:** `cmbuild.sh build gw2dat_cli`.
- [ ] **Step 3: Real data.** `npc-list --limit 3` prints coverage with npcs ≈ 23 653 and with_model ≈ 13 534 (from the content map). `npc-list --model 104256 --limit 5` lists NPCs. `npc-show --id <one of them>` lists clips including names that end in digits (e.g. `…08`).
- [ ] **Step 4:** Full suite `cmbuild.sh test` passes.
- [ ] **Step 5:** Commit `feat(cli): npc-list and npc-show`.
