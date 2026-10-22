# Skill Effects, Stage 1 (castlemist export) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `gw2dat_cli skillfx-export` writes a folder holding every Firebrand skill's effects
(.glb + particle sidecar per effect model), with chapters and mantra charges paired from
the user's arcdps logs. Tome of Justice comes out complete: the book, its opening fire
and all five pages.

**Architecture:** The cntc pack reader moves from `npc` into `format`, so it can be
shared. A new `skillfx` layer (on extract + exportgltf) does four things: discovers the
skill set from a codename namespace, collects each skill's content-linked effects,
reads arcdps `.evtc` logs and pairs the remaining effects to skills, and exports through
the existing `export_model_gltf`. `gw2dat_cli` gets `skillfx-export` and `evtc-pairs`.

**Tech Stack:** C++20, CMake/Ninja (MinGW UCRT64), the in-repo `CM_TEST` framework,
nlohmann::json, stb_image (zlib inflate for `.zevtc`).

**Spec:** docs/superpowers/specs/2026-10-22-skill-effects-design.md. Measurements:
docs/research/gw2-skill-effects.md.

## Global Constraints

- Never read Gw2.dat directly; only castlemist's dat/index code opens it.
- No guessed names: unknown numbers (effect `kind`) stay numbers; labels come from the
  API, the arcdps skill table, Elite Insights' `EffectGUIDs.cs`, or the codename.
- Missing data is reported (empty lists, `error` fields), never filled in.
- Build env: `export PATH="/c/msys64/ucrt64/bin:$PATH"`, `TMP`/`TEMP` in the scratchpad;
  configure with `-DCASTLEMIST_EXTERNAL_DIR=C:/Users/vital/Downloads/castlemist/castlemist/external`.
- Real-dat steps use `--dat "E:/Games/gw2/Guild Wars 2/Gw2.dat"` and
  `--index C:/Users/vital/Downloads/castlemist/castlemist/dumps/index/gw2_index.db`.
- Spec deviations, both settled by the logs: (1) structural labels apply only to bundle
  slots (chapters get their names from the arcdps skill table); (2) the skill namespace
  comes from `--seed` skill ids (Firebrand: `44364,41714`), not from an API spec query.

## Review Focus

- A skill whose first string is a fact label ("Base Cooldown"), not a codename: the
  codename must still be found (later string slots). Test in Task 2.
- An effect group (kind 1) that refers back to itself or a cycle: must not loop. Test in
  Task 2.
- A truncated `.evtc` (log cut mid-event) or a zip with a different first entry name:
  read the whole events, no crash. Test in Task 4.
- Shared effects (the epilogues' common GUID, symbol model 1764358): keep every skill
  that uses them, export the model once. Tests in Task 5 and Task 6.
- An effect model that fails to load: recorded as `{model, error}`, the rest still
  exports. Test in Task 6.

---

### Task 1: Move the cntc pack reader into `format`

**Files:**
- Create: `include/castlemist/format/cntc_pack.h` (contents of `src/npc/pack.h` + the
  `decompress_cntc` declaration from `src/npc/decompress.h`)
- Create: `src/format/cntc_pack.cpp` (contents of `src/npc/pack.cpp` + `decompress_cntc`
  from `src/npc/catalogue.cpp:19-33`)
- Delete: `src/npc/pack.h`, `src/npc/pack.cpp`, `src/npc/decompress.h`
- Modify: `src/npc/catalogue.cpp`, `src/npc/character.cpp`, `src/npc/character.h`,
  `tests/test_npc.cpp`, `tools/gw2dat_cli/main.cpp` (drop the `../../src/npc/` includes)

**Interfaces:**
- Produces: namespace `castlemist::cntc` with the unchanged types and functions `Ref`,
  `Object`, `Pack`, `PackSet`, `parse_pack(std::span<const uint8_t>, uint32_t base_id,
  uint32_t file_id)`, `decompress_cntc(std::span<const uint8_t> raw, bool compressed)`.
  Behaviour, field names and doc comments are unchanged; only the namespace and file move.

- [ ] **Step 1:** Move the code as listed. Replace `castlemist::npc::detail::` with
  `castlemist::cntc::` at every use (`grep -rn "npc::detail\|detail::Pack\|detail::parse_pack\|detail::Object\|detail::Ref\|detail::decompress_cntc" src tools tests`
  lists them). The npc-specific helpers in `character.h` stay in `npc::detail`.
- [ ] **Step 2:** Build and run the existing tests: `cmake --build build/debug && ctest --test-dir build/debug -R "^(format|npc)$" --output-on-failure`.
  Expected: both PASS with the same test counts as before the move.
- [ ] **Step 3:** `build/debug/bin/gw2dat_cli.exe cgraph <dat/index> --content-type 64 --content-id 44364 --depth 1`
  still lists `type 183 ... 1764329`.
- [ ] **Step 4:** Commit: `refactor(format): the cntc pack reader moves from npc to format`.

### Task 2: `skillfx` layer: effects of one skill

**Files:**
- Create: `include/castlemist/skillfx/effects.h`, `src/skillfx/effects.cpp`
- Modify: `CMakeLists.txt` (add `castlemist_add_layer(skillfx DEPS castlemist::extract castlemist::exportgltf castlemist::format ext::json ext::stb)` after `npc`)
- Modify: `tests/CMakeLists.txt` (add `castlemist_add_test(skillfx SOURCES test_skillfx.cpp DEPS castlemist::skillfx ext::json)`)
- Test: `tests/test_skillfx.cpp`. Copy the `PackBuilder` from `tests/test_npc.cpp:30-90`
  into an anonymous namespace there. Extend it with string slots (arrays 7 and 9) and
  file slots (arrays 2 and 6).

**Interfaces:**
- Consumes: `castlemist::cntc` (Task 1).
- Produces (namespace `castlemist::skillfx`):
  ```cpp
  using Guid = std::array<uint8_t, 16>;          // an object's first 16 bytes
  constexpr uint32_t kSkillType = 64, kEffectType = 183, kItemType = 35, kBarType = 65;
  struct Effect {
      uint32_t field = 0;      // slot in the referring object (0 for unlinked/paired)
      uint32_t kind = 0;       // the effect's dataId (@+40), unnamed
      std::string codename;
      uint32_t model = 0, sound = 0;   // first MODL / AMSP fileId, 0 = none
      Guid guid{};
      std::string source = "content";  // "content" | "arcdps"
      std::string ei_name;              // Elite Insights name, if known
      uint32_t count = 0;               // arcdps: casts the pairing was seen in
  };
  std::string codename(const cntc::Object& o);   // first string slot shaped like "Bp5rI.EWIp2"
  Guid guid_of(const cntc::Pack& p, const cntc::Object& o);
  /// container(fileId) -> "MODL"/"AMSP"/"" (the index DB lookup, injected).
  using ContainerOf = std::function<std::string(uint32_t)>;
  Effect read_effect(const cntc::PackSet& s, size_t pack, const cntc::Object& o, const ContainerOf& c);
  struct SkillLinks { std::vector<Effect> effects; std::vector<uint32_t> related_skills, applies; };
  SkillLinks find_effects(const cntc::PackSet& s, size_t pack, const cntc::Object& skill, const ContainerOf& c);
  ```
  `find_effects`: for each ref of the skill, in field order, a type-183 target is an
  effect. A kind-1 effect is a group: add each type-183 object it points at, recursively,
  guarded by a visited set. A type-64 target with dataId < 2000 goes to `applies` (boons
  and conditions); any other type-64 target goes to `related_skills`. Nothing else is
  followed. The codename slug shape is two groups of 5 characters from `[A-Za-z0-9+/]`
  joined by `.` (`content_map.cpp`'s `looks_like_content_slug` uses the same predicate).

- [ ] **Step 1: Write failing tests** in `tests/test_skillfx.cpp`:
  - `CM_TEST(skillfx, codename_skips_fact_labels)`: an object whose string slots are
    `"Base Cooldown"`, `"Trait Cooldown"`, `"Bp5rI.Bw99b"` gives `codename == "Bp5rI.Bw99b"`.
  - `CM_TEST(skillfx, effect_reads_model_and_sound)`: a type-183 object (dataId 6) with
    file slots → fileIds 1764329 (MODL) and 1764333 (AMSP). `read_effect` gives `kind 6`,
    `model 1764329`, `sound 1764333`, and `guid` equal to the object's first 16 bytes.
  - `CM_TEST(skillfx, find_effects_direct_group_boon_related)`: skill (64, dataId 44364)
    with refs +184 → effect A (kind 6); +200 → group G (kind 1) → effect B, and G → G
    (a cycle); +300 → skill 737; +400 → skill 68647. Expect effects `[A(field 184),
    B(field 200)]`, `applies == {737}`, `related_skills == {68647}`, and termination.
- [ ] **Step 2:** `cmake --build build/debug && build/debug/bin/cm_test_skillfx.exe` → FAIL (undefined symbols).
- [ ] **Step 3:** Implement `effects.h/.cpp` as specified.
- [ ] **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit: `feat(skillfx): the effects a skill links to in content`.

### Task 3: The skill set of a namespace, bundle slots, unlinked effects

**Files:**
- Create: `include/castlemist/skillfx/skill_set.h`, `src/skillfx/skill_set.cpp`
- Test: `tests/test_skillfx.cpp`

**Interfaces:**
- Consumes: Task 2.
- Produces:
  ```cpp
  struct SkillEntry {
      uint32_t id = 0; std::string codename, label;   // label filled in Task 6
      uint32_t bundle_item = 0; int bundle_slot = -1; // tome pages: item dataId + slot 0..5
      SkillLinks links;
  };
  struct SkillSet {
      std::string skill_namespace, effect_namespace;  // e.g. "Bp5rI.", "Bmt2N."
      std::vector<SkillEntry> skills;                  // by id
      std::vector<Effect> unlinked;                    // effect namespace, reached by no skill
  };
  SkillSet build_skill_set(const cntc::PackSet& s, const std::vector<uint32_t>& seed_skill_ids, const ContainerOf& c);
  /// Every type-183 object by GUID (for arcdps pairing).
  std::map<Guid, std::pair<size_t, const cntc::Object*>> effect_index(const cntc::PackSet& s);
  ```
  - `skill_namespace` is the codename prefix (up to and including the `.`) that most seed
    skills share. Every type-64 object with that prefix becomes an entry, with
    `find_effects`.
  - Each type-35 item with that prefix: its ref at **+264** points at a type-65 bar. The
    bar's refs at **+192 + 112·k** (k = 0..5) are type-64 skills. Those entries get
    `bundle_item = item dataId`, `bundle_slot = k`.
  - `effect_namespace` is the prefix most linked effects share.
  - `unlinked` = every type-183 object in that namespace that no entry's links contain
    (compared by GUID).

- [ ] **Step 1: Write failing tests:**
  - `CM_TEST(skillfx, set_from_namespace_with_bundle)`: a pack with skills `A.aaaaa`
    (seed, linked to effect `E.11111`), `A.bbbbb`, `C.ccccc` (another namespace), an item
    `A.item0` whose +264 points at a bar with +192 → `A.bbbbb` and +304 → `A.aaaaa`,
    and effects `E.11111`, `E.22222`. Expect: namespace `"A."`; entries for aaaaa and
    bbbbb only; bbbbb has `bundle_slot 0`, aaaaa `bundle_slot 1`; `effect_namespace "E."`;
    `unlinked` = `[E.22222]`.
  - `CM_TEST(skillfx, effect_index_by_guid)`: two effects with distinct GUIDs are both
    found, each by its own GUID.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit: `feat(skillfx): skill sets by codename namespace, tome bundles, unlinked effects`.

### Task 4: arcdps `.evtc` reader

**Files:**
- Create: `include/castlemist/skillfx/evtc.h`, `src/skillfx/evtc.cpp`, `src/skillfx/evtc_zip.cpp`
- Test: `tests/test_skillfx.cpp`

**Interfaces:**
- Produces:
  ```cpp
  struct EvtcAgent { uint64_t addr = 0; uint32_t prof = 0, elite = 0; std::string name; };
  struct EvtcEvent {   // revision 1, 64 bytes
      uint64_t time = 0, src = 0, dst = 0; int32_t value = 0, buff_dmg = 0;
      uint32_t overstack = 0, skill_id = 0;
      uint16_t src_instid = 0, dst_instid = 0, src_master = 0, dst_master = 0;
      uint8_t iff = 0, buff = 0, result = 0, is_activation = 0, is_buffremove = 0,
              is_ninety = 0, is_fifty = 0, is_moving = 0, statechange = 0,
              is_flanking = 0, is_shields = 0, is_offcycle = 0;
  };
  struct EvtcLog { std::string build; uint8_t revision = 0; uint16_t boss = 0;
                   std::vector<EvtcAgent> agents; std::map<int32_t, std::string> skills;
                   std::vector<EvtcEvent> events; };
  namespace evtc { constexpr uint8_t kIdToGuid = 46, kEffectGround = 60, kEffectAgent = 62,
                   kEffectOld1 = 45, kEffectOld2 = 51, kAnimationStart = 67, kBuffApply = 69; }
  std::optional<EvtcLog> read_evtc(std::span<const uint8_t> file);  // .evtc or .zevtc
  /// IDTOGUID: content id (skill_id) -> GUID = src(8 bytes LE) followed by dst(8 bytes LE).
  std::map<uint32_t, Guid> effect_guids(const EvtcLog& log);
  ```
  Layout: `"EVTC"`, an 8-char build date, a revision byte, u16 boss id, a pad byte (16
  bytes in all). Then u32 agent count, agents of 96 bytes (`addr u64, prof u32,
  elite u32`, 12 bytes of stats, name char[64] at +28, 4 pad). Then u32 skill count,
  skills of 68 bytes (`id i32, name char[64]`). Then 64-byte events until EOF; a trailing
  partial event is dropped. `.zevtc` is a zip: parse the first local file header
  (signature `PK\3\4`), then raw-deflate its data with `stbi_zlib_decode_noheader_malloc`
  (`evtc_zip.cpp` defines `STB_IMAGE_STATIC`, `STB_IMAGE_IMPLEMENTATION`, `STBI_ONLY_PNG`).
  A stored (method 0) entry is copied as-is.

- [ ] **Step 1: Write failing tests:** build a synthetic log in the test (header, one
  agent `addr 0x7d0, elite 62, "Musa"`, one skill `41258 "Chapter 1: Searing Spell"`,
  three events: IDTOGUID `skill_id 9001, src 0x0011223344556677, dst 0x8899aabbccddeeff`;
  ANIMATIONSTART `src 0x7d0, skill 41258, time 1000`; EFFECTAGENT `src 0x7d0, skill_id 9001,
  time 1400`) plus 10 stray bytes.
  - `CM_TEST(skillfx, evtc_reads_plain_log)`: three events, the agent's name and elite,
    the skill name; `effect_guids(log)[9001]` is the bytes `77 66 55 44 33 22 11 00 ff ee dd cc bb aa 99 88`.
  - `CM_TEST(skillfx, evtc_reads_stored_zip)`: the same bytes wrapped in a method-0 zip
    entry named `x` read identically.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Read the user's real log as a check:
  `C:/Users/vital/Documents/Guild Wars 2/addons/arcdps/arcdps.cbtlogs/Standard Kitty Golem (16199)/20261022-065900.zevtc`
  (deflated). A temporary `CM_TEST` guarded by `std::filesystem::exists` must see build
  `"20261021"`, an agent with `elite == 62`, and at least 200 IDTOGUID events.
- [ ] **Step 6:** Commit: `feat(skillfx): read arcdps evtc and zevtc logs`.

### Task 5: Pair effects with skills from logs

**Files:**
- Create: `include/castlemist/skillfx/pairing.h`, `src/skillfx/pairing.cpp`
- Test: `tests/test_skillfx.cpp`

**Interfaces:**
- Consumes: Tasks 3, 4.
- Produces:
  ```cpp
  struct Pairing { uint32_t skill_id = 0; std::string skill_name; Guid effect{}; uint32_t count = 0; };
  /// Markers: the player's ANIMATIONSTART (skill_id) and its BUFFAPPLY whose skill_id is
  /// in `marker_buffs` (instant skills). Each effect event (60/62/45/51) whose source is
  /// the player goes to the latest marker at or before it within `window_ms`.
  std::vector<Pairing> pair_effects(const std::vector<EvtcLog>& logs, uint32_t elite_spec,
                                    const std::set<uint32_t>& marker_buffs,
                                    uint32_t window_ms = 3000, uint32_t min_count = 2);
  /// Elite Insights' EffectGUIDs.cs: `Guid Name = new("HEX32")` lines.
  std::map<Guid, std::string> read_ei_guids(std::istream& cs);
  ```
  - The player is the agent with `elite == elite_spec`. A marker counts at most once per
    effect GUID: several effects of one cast each count once, and repeats of one GUID
    within the same marker count once.
  - Results with `count < min_count` are dropped. Sorted by `skill_id`, then by count
    descending. `skill_name` comes from the log's skill table.
  - `marker_buffs` = the ids of the `SkillSet` entries (the namespace's skills, which
    include the mantra charge buffs).

- [ ] **Step 1: Write failing tests** (synthetic logs as in Task 4):
  - `CM_TEST(skillfx, pairs_cast_then_effect)`: casts of 41258 at t=1000 and t=9000,
    effect G1 at 1400 and 9500 → one pairing `{41258, G1, count 2}`.
  - `CM_TEST(skillfx, instant_skill_via_buff_marker)`: an axe cast 44602 at 4000, buff
    apply 45447 at 4470, effect G2 at 4470, all repeated at 8000/8470 →
    `{45447, G2, 2}`, and nothing for 44602.
  - `CM_TEST(skillfx, single_sighting_dropped_and_shared_kept)`: G3 once after 41258 →
    dropped. G4 twice after 42898 and twice after 42925 → both pairings kept.
  - `CM_TEST(skillfx, ei_guids_parse)`: the line
    `    public static readonly Guid FirebrandTomeOfJusticeOpen = new("D573910FDB59434ABF6E7433061995BD");`
    maps the bytes `d5 73 91 0f ...` to `"FirebrandTomeOfJusticeOpen"`.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement. **Step 4:** Run → PASS.
- [ ] **Step 5:** Commit: `feat(skillfx): pair effects with skills from arcdps logs`.

### Task 6: Export and the CLI

**Files:**
- Create: `include/castlemist/skillfx/export.h`, `src/skillfx/export.cpp`
- Modify: `tools/gw2dat_cli/main.cpp` (commands `skillfx-export`, `evtc-pairs`), `tools/CMakeLists.txt` (link `castlemist::skillfx`)
- Test: `tests/test_skillfx.cpp`

**Interfaces:**
- Consumes: Tasks 3, 5; `castlemist::extract::load_model_by_fileid(Gw2Dat&, uint32_t, bool)`,
  `castlemist::exportgltf::export_model_gltf(const ModelPreview&, const std::string&)`.
- Produces:
  ```cpp
  /// Attach pairings (source "arcdps") to the entries they name, by effect GUID via
  /// effect_index; drop those effects from `unlinked`; fill each entry's label:
  /// api_names[id], else the logs' skill name, else "<bundle item> slot <k+1>"
  /// for bundle slots, else the codename. Effects get ei_name from `ei`.
  void apply_pairings(SkillSet& set, const std::vector<Pairing>& pairs,
                      const std::map<Guid, std::pair<size_t, const cntc::Object*>>& index,
                      const cntc::PackSet& s, const ContainerOf& c,
                      const std::map<uint32_t, std::string>& api_names,
                      const std::map<Guid, std::string>& ei);
  /// Export one model to `glb`: "" on success, else the error text (it lands in
  /// skill.json). Injected so tests need no dat.
  using ModelExporter = std::function<std::string(uint32_t model, const std::filesystem::path& glb)>;  // "" = ok, else error
  struct ExportReport { size_t skills = 0, models = 0, failed = 0; std::vector<std::string> notes; };
  ExportReport export_skill_set(const SkillSet& set, const std::filesystem::path& out, const ModelExporter& exp);
  ```
  Layout and fields as in the spec's "Export layout":
  - `out/skill_set.json`: namespaces, and the skills with id and label.
  - `out/skills/<id>-<slug(label)>/skill.json`: id, label, codename, bundle item/slot,
    `effects[]` (`field, kind, codename, model, sound, glb, source, ei_name, count`),
    `related_skills`, `applies`.
  - Models: `out/skills/<id>-<slug>/<model>.glb` when one skill uses them, else
    `out/shared/<model>.glb`; `glb` gives the path relative to `out`.
  - `out/unlinked/unlinked.json` plus their models.
  - Each model is exported once. `slug` lowercases and turns runs of anything outside
    `[a-z0-9]` into `-`.
  The real `ModelExporter` in the CLI loads the model with `load_model_by_fileid`, calls
  `export_model_gltf`, and also exports every mesh-particle emitter's `meshFileId` next
  to it.

- [ ] **Step 1: Write failing tests:**
  - `CM_TEST(skillfx, export_layout_shared_and_failure)`: two skills sharing model 100,
    one with model 200 whose exporter returns `"bad model"`, and one unlinked effect with
    model 300. Expect `shared/100.glb` exported once; the 200 skill's `skill.json` effect
    has `"error": "bad model"`; `unlinked/300.glb`; `report.failed == 1`.
  - `CM_TEST(skillfx, labels_api_then_log_then_slot)`: entry 44364 with API name
    `"Tome of Justice"`, 41258 known only to the logs as `"Chapter 1: Searing Spell"`,
    and an unnamed bundle slot get those three labels in that order of preference.
- [ ] **Step 2:** Run → FAIL. **Step 3:** Implement `export.cpp`.
- [ ] **Step 4:** Add the CLI commands:
  - `evtc-pairs --dat --index --seed 44364,41714 --log F [--log F...] [--ei EffectGUIDs.cs] --out pairs.json`
    builds the set for `marker_buffs`, then writes the pairings: skill id, name, effect
    GUID hex, count, ei_name, codename and model.
  - `skillfx-export --dat --index --seed ... [--log ...] [--ei ...] [--elite 62] --out DIR`.
    It loads packs the way `cgraph` does and fetches API names with `Gw2Api::names("skills", ids)`
    (online; when offline it goes on without them), then runs build → pair → apply → export.
  Run the unit tests → PASS.
- [ ] **Step 5:** Commit: `feat(skillfx): skillfx-export and evtc-pairs`.

### Task 7: Real export of Firebrand, checked against the logs

- [ ] **Step 1:** Fetch EI's table to the scratchpad:
  `curl -s https://raw.githubusercontent.com/baaron4/GW2-Elite-Insights-Parser/master/GW2EI.Library/GW2EI.Services/GW2EIEvtcParser/ParserHelpers/GUIDs/EffectGUIDs.cs -o <scratchpad>/EffectGUIDs.cs`
- [ ] **Step 2:** Run:
  `build/debug/bin/gw2dat_cli.exe skillfx-export --dat ... --index ... --seed 44364,41714 --log "<cbtlogs>/Standard Kitty Golem (16199)/20261022-064733.zevtc" --log ...065440 --log ...065900 --log ...070224 --ei <scratchpad>/EffectGUIDs.cs --out <scratchpad>/firebrand`
- [ ] **Step 3: Check** (each line must hold, from the research note's table):
  - `skills/44364-tome-of-justice/` has `1764329.glb` and `1764329_particles.json`, and
    the sidecar lists 9 clouds / 19 emitters.
  - Pages: 41258 → 1764455, 40635 → 1764407, 42449 → 1764300, 40015 → 1764457, all with
    `source "arcdps"`. 42898 has 296840 (`source "content"`).
  - Mantra buff 45447 has models 1764358 and 1764399, with `ei_name`s
    `FirebrandMantraOfSolaceCone`/`...Symbol`.
  - `unlinked/unlinked.json` no longer holds 1764455, 1764407, 1764300, 1764457, 1764465,
    1764409, 1764302, 1764474, 1764436, 1764411, 1764345, 1764441, 1764377.
  - `ctest --test-dir build/debug --output-on-failure` (the 11 known `dat` failures from
    the curated ids are pre-existing).
- [ ] **Step 4:** Add a "skillfx-export" usage paragraph to `docs/research/gw2-skill-effects.md` and commit:
  `docs(skillfx): first real Firebrand export`.

## Next

Stage 2 (Unity prefab importer) gets its own plan once this lands. It turns
`skills/44364-tome-of-justice/` into a playable prefab: the first visible, complete VFX.
