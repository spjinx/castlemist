# Character Fetch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** From a saved GW2 API key, list an account's characters and resolve one character's equipped gear into a `CharacterManifest` (item → skin → dat fileIds, with dye colors), shown in a UI dialog and emitted by `gw2dat_cli character`.

**Architecture:** A new `character` layer (`include/castlemist/character/`, `src/character/`) with no UI dependency: an injectable `HttpClient` (WinHTTP in production, a fake in tests), a typed `Gw2Api`, a plain-JSON `KeyStore`, a pure `resolve_character()` over an `AssetLookup` seam onto `cmap`, and manifest JSON I/O. The UI dialog and the CLI command are thin front ends over `fetch_character()`.

**Tech Stack:** C++20, MinGW, CMake presets, WinHTTP, nlohmann::json (`ext::json`), castlemist's in-house `test_framework.h`.

**Spec:** `docs/superpowers/specs/2026-10-03-character-fetch-design.md`

## Global Constraints

- Network only on explicit user action (dialog Fetch / CLI `character`), only to `https://api.guildwars2.com`.
- No new third-party dependencies: WinHTTP (system lib `winhttp`) + `ext::json`.
- The API key travels only in an `Authorization: Bearer <key>` header — never in a URL, log line, exception message or manifest.
- Keys are stored in plain text in `api_keys.json`, format `{"keys":[{"name":"Main","key":"..."}]}`, git-ignored as `/api_keys.json`.
- `character` layer has no UI dependency and is safe on a background thread.
- No automated test touches the network.
- Namespace: `castlemist::character`. Build: `cmake --build --preset debug`. Test binaries: `build/debug/bin/cm_test_<layer>.exe [filter]`.
- Do not modify files outside each task's **Files** list.

**Deviations from the spec (decided while planning, from repo conventions):**
- CLI prints JSON on stdout (every `gw2dat_cli` command does), not a text table. It takes `--cmap <content_map.bin>` (default: `content_map.bin` next to the exe — the GUI's cache) instead of `--dat`; building the map stays a GUI action.
- Dye column in the dialog shows `name #RRGGBB` text, not drawn swatches (swatches are cosmetic; YAGNI for sub-project 1).

## Review Focus

1. **Character names with spaces and non-ASCII letters** (e.g. `Þórr Skýfaðir`) must URL-encode as UTF-8 percent-escapes and fetch correctly — Task 3 test `url_encode_utf8_and_space`.
2. **HTTP 206 Partial Content** on batched `ids=` calls (GW2 returns it when some ids are unknown, e.g. removed items) is a success with fewer results — Task 3 test `batch_206_is_success`; Task 4 test `item_missing_from_api_uses_cmap_skin`.
3. **Pieces with no dye slots** (weapons, back items) and **null dyes** must give an empty/defaulted dye list, never a crash or a phantom 4-dye entry — Task 4 tests `weapon_has_no_dyes`, `null_dye_falls_back_to_skin_default`.
4. **Malformed `api_keys.json`** (hand-edited) must report an error and must not be overwritten — Task 1 test `malformed_file_is_error_and_untouched`.
5. **Wrong/revoked key and missing scopes** must produce messages that name the problem and never contain the key — Task 3 tests `http_401_message_has_no_key`, `missing_scopes_lists_builds`.

---

### Task 1: `character` layer scaffold + KeyStore

**Files:**
- Create: `include/castlemist/character/key_store.h`, `src/character/key_store.cpp`
- Create: `tests/test_character_key_store.cpp`
- Modify: `CMakeLists.txt` (add layer after the `format` layer, ~line 112)
- Modify: `tests/CMakeLists.txt` (add `castlemist_add_test(character ...)`)
- Modify: `.gitignore` ("local only" section)

**Interfaces:**
- Produces:
  - `struct ApiKey { std::string name; std::string key; };`
  - `class KeyStore` with
    `bool load(const std::filesystem::path& file, std::string* error);` (missing file → `true`, empty store; malformed → `false`, `*error` set, store unchanged),
    `bool save(const std::filesystem::path& file, std::string* error) const;` (pretty-printed, 2-space indent),
    `const std::vector<ApiKey>& list() const;`,
    `void add(const std::string& name, const std::string& key);` (same name → replace key, keeps position),
    `bool rename(const std::string& from, const std::string& to);` (false if `from` missing or `to` exists),
    `bool remove(const std::string& name);`,
    `const ApiKey* get(const std::string& name) const;`
  - `std::filesystem::path find_castlemist_root(const std::filesystem::path& start_dir);` — first of `start_dir` and its ancestors containing a `CMakeLists.txt` whose text contains `project(castlemist`; else `start_dir`.
  - `std::filesystem::path default_key_file();` — `find_castlemist_root(<dir of GetModuleFileNameW>) / "api_keys.json"`.

- [ ] **Step 1: Wire the layer and test binary**

`CMakeLists.txt`: `castlemist_add_layer(character DEPS castlemist::format ext::json winhttp)`.
`tests/CMakeLists.txt`:
```cmake
castlemist_add_test(character
    SOURCES test_character_key_store.cpp
    DEPS    castlemist::character ext::json)
target_compile_definitions(cm_test_character PRIVATE
    CM_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
```
`.gitignore`, under "local only": a comment line `# GW2 API keys (plain text, see src/character/key_store.cpp).` then `/api_keys.json`.

- [ ] **Step 2: Write the failing tests** in `tests/test_character_key_store.cpp` (group `key_store`), each using a fresh temp dir under `std::filesystem::temp_directory_path() / "cm_test_keys_<test>"`:
  - `missing_file_loads_empty`: `load` → `true`, `list().empty()`.
  - `round_trip`: add `Main`/`AAAA`, `Alt`/`BBBB`; save; load into a new store → same two keys in order.
  - `add_same_name_replaces`: add `Main`/`A` then `Main`/`B` → size 1, `get("Main")->key == "B"`.
  - `rename_and_remove`: rename `Main`→`Alt` fails when `Alt` exists; succeeds otherwise; `remove("nope") == false`.
  - `malformed_file_is_error_and_untouched`: write `{"keys": [` to the file; `load` → `false`, error non-empty; file bytes unchanged afterwards.
  - `root_found_from_nested_dir`: temp tree `root/CMakeLists.txt` (contains `project(castlemist`) + `root/build/debug/bin/`; `find_castlemist_root(root/build/debug/bin) == root`.
  - `root_falls_back_to_start_dir`: dir with no such file → returns the start dir.

- [ ] **Step 3: Run to verify failure**

Run: `cmake --preset debug && cmake --build --preset debug --target cm_test_character`
Expected: compile error (`key_store.h` not found).

- [ ] **Step 4: Implement `key_store.h/.cpp`** per the Interfaces block. Parse with nlohmann (`json::parse(..., nullptr, false)` and check `is_discarded()`); a `keys` entry missing `name` or `key` strings is a malformed file.

- [ ] **Step 5: Run tests**

Run: `cmake --build --preset debug --target cm_test_character && build/debug/bin/cm_test_character.exe`
Expected: all `key_store.*` PASS.

- [ ] **Step 6: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt .gitignore include/castlemist/character/key_store.h src/character/key_store.cpp tests/test_character_key_store.cpp
git commit -m "character: new layer with plain-JSON API key store"
```

- [ ] **Step 7 (controller only, not a subagent): seed the user's key**

Write `api_keys.json` at the repo root with one key named `Main` (the key the user supplied in the conversation). Confirm `git status` does not list it.

---

### Task 2: HTTP client + URL encoding

**Files:**
- Create: `include/castlemist/character/http.h`, `src/character/http.cpp`
- Create: `tests/test_character_http.cpp`, `tests/fake_http.h`
- Modify: `tests/CMakeLists.txt` (add the test source)

**Interfaces:**
- Produces:
  - `using Headers = std::vector<std::pair<std::string, std::string>>;`
  - `struct HttpResponse { int status = 0; std::string body; std::string error; };` (`status == 0` ⇒ transport failure, `error` set)
  - `class HttpClient { public: virtual ~HttpClient() = default; virtual HttpResponse get(const std::string& url, const Headers& headers) = 0; };`
  - `class WinHttpClient final : public HttpClient` — HTTPS GET, 15 000 ms resolve/connect/send/receive timeouts, `User-Agent: castlemist`; `error` from `GetLastError()` text, never containing headers.
  - `std::string url_encode(std::string_view utf8);` — RFC 3986 unreserved (`A-Za-z0-9-._~`) pass through, everything else `%XX` uppercase per byte.
  - `tests/fake_http.h`: `struct FakeHttpClient : HttpClient` with `std::map<std::string, HttpResponse> routes;` and `std::vector<std::pair<std::string, Headers>> calls;` — unknown URL → `{404, "{\"text\":\"no such route\"}"}`; plus `std::string read_fixture(const char* name)` reading `CM_TEST_DATA_DIR "/character/" name`.

- [ ] **Step 1: Write failing tests** (group `http`):
  - `url_encode_utf8_and_space`: `url_encode("Þórr Skýfaðir") == "%C3%9E%C3%B3rr%20Sk%C3%BDfa%C3%B0ir"`; `url_encode("A-z_0.~") == "A-z_0.~"`.
  - `fake_records_headers`: a `get` through `FakeHttpClient` records the URL and headers.

- [ ] **Step 2: Run to verify failure** — build `cm_test_character`; expected compile error.

- [ ] **Step 3: Implement `http.h/.cpp` and `tests/fake_http.h`** per Interfaces (WinHTTP: `WinHttpCrackUrl` → `WinHttpConnect` → `WinHttpOpenRequest(..., WINHTTP_FLAG_SECURE)` → `WinHttpAddRequestHeaders` → send/receive/read loop; status via `WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER`).

- [ ] **Step 4: Run tests** — `build/debug/bin/cm_test_character.exe http` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "character: WinHTTP client, url_encode, fake client for tests"`

---

### Task 3: API fixtures + `Gw2Api`

**Files:**
- Create: `tools/character/capture_fixtures.py`
- Create: `tests/data/character/{tokeninfo,characters,core,equipmenttabs,items,skins,colors}.json` (generated)
- Create: `include/castlemist/character/gw2_api.h`, `src/character/gw2_api.cpp`
- Create: `tests/test_character_api.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `HttpClient`, `Headers`, `url_encode` (Task 2).
- Produces:
  - `constexpr const char* kApiBase = "https://api.guildwars2.com";`
  - `class ApiError : public std::runtime_error { public: ApiError(int status, const std::string& msg); int status() const; };`
  - `struct TokenInfo { std::string name; std::vector<std::string> permissions; };`
  - `std::vector<std::string> missing_scopes(const TokenInfo&);` — of `account`, `characters`, `builds`, in that order.
  - `struct CharacterCore { std::string name, race, gender, profession; int level = 0; };`
  - `struct EquipmentEntry { uint32_t item_id = 0; std::string slot; std::optional<uint32_t> skin; std::array<std::optional<uint32_t>, 4> dyes{}; std::vector<uint32_t> upgrades, infusions; };`
  - `struct EquipmentTab { int tab = 0; std::string name; bool is_active = false; std::vector<EquipmentEntry> equipment; };`
  - `struct ApiItem { uint32_t id = 0; std::string name, type; std::optional<uint32_t> default_skin; };`
  - `struct DyeSlot { uint32_t color_id = 0; std::string material; };`
  - `using DyeSlots = std::vector<std::optional<DyeSlot>>;`
  - `struct ApiSkin { uint32_t id = 0; std::string name, type, weight_class; DyeSlots dye_default; std::map<std::string, DyeSlots> dye_overrides; };` (override keys as the API gives them, e.g. `"CharrFemale"`)
  - `struct ApiColor { uint32_t id = 0; std::string name; std::map<std::string, std::array<uint8_t, 3>> rgb; };` (keys `cloth`, `leather`, `metal`, `fur`)
  - `class Gw2Api { public: Gw2Api(HttpClient& http, std::string key); TokenInfo token_info(); std::vector<std::string> character_names(); CharacterCore character_core(const std::string& name); std::vector<EquipmentTab> equipment_tabs(const std::string& name); std::map<uint32_t, ApiItem> items(const std::vector<uint32_t>& ids); std::map<uint32_t, ApiSkin> skins(const std::vector<uint32_t>& ids); std::map<uint32_t, ApiColor> colors(const std::vector<uint32_t>& ids); };`
- Request shape: every call sends `Authorization: Bearer <key>` and `X-Schema-Version: latest`. Paths: `/v2/tokeninfo`, `/v2/characters`, `/v2/characters/<url_encode(name)>/core`, `/v2/characters/<url_encode(name)>/equipmenttabs?tabs=all`, `/v2/items?ids=1,2,3` (same for `skins`, `colors`). Batches chunked at 200 ids; empty id list → no request, empty map.
- Status handling: 200 and 206 succeed. 401/403 → `ApiError(status, "API key rejected (HTTP <n>) - check the key on account.arena.net")`. Other non-2xx → `ApiError(status, "GW2 API error (HTTP <n>): <body.text if present>")`. `status == 0` → `ApiError(0, "Could not reach api.guildwars2.com: <error>")`. Unparseable body → `ApiError(status, "GW2 API returned malformed JSON for <path without query>")`.

- [ ] **Step 1: Write and run the capture script**

`tools/character/capture_fixtures.py` (stdlib only): reads the `Main` key from `api_keys.json` at the repo root; fetches the seven endpoints above with the bearer header; picks the first character whose active equipment tab has at least one entry with a `skin` field and one non-null dye; gathers that tab's item ids → items, skin ids (overrides + item `default_skin`) → skins, color ids (equipment dyes + skin default dyes) → colors. Scrubbing before writing: `tokeninfo.name` → `"Test Key"`, `tokeninfo.id` → `"00000000-0000-0000-0000-000000000000"`; `characters` → `["Test Character", "Second Character", ...]` (same count); `core.name` → `"Test Character"`. Writes pretty JSON (2 spaces) to `tests/data/character/`.

Run: `python tools/character/capture_fixtures.py`
Expected: seven files written; `grep -ri "<the real key's first 8 chars>" tests/data/character` finds nothing, and no real character name appears.

- [ ] **Step 2: Write failing tests** (group `api`), using `FakeHttpClient` routes loaded from the fixtures. Assertions read expected values from the fixture JSON (parsed with nlohmann in the test) rather than hard-coding the account's gear:
  - `sends_bearer_and_schema_headers`: after `token_info()`, `calls[0]` headers contain `{"Authorization","Bearer KEY"}` and `{"X-Schema-Version","latest"}`, and the URL does not contain `KEY`.
  - `token_info_parses`: name `"Test Key"`, permissions match the fixture.
  - `missing_scopes_lists_builds`: `TokenInfo{"", {"account","characters"}}` → `{"builds"}`; the fixture's token → empty.
  - `character_names_and_core`: names equal the fixture list; `character_core("Test Character")` hits `/v2/characters/Test%20Character/core` and race/gender/profession/level match.
  - `equipment_tabs_parse`: tab count, active flag and per-entry `item_id`/`slot`/`skin`/`dyes` match the fixture (null dye → `nullopt`).
  - `items_skins_colors_parse`: every fixture id is a key in the result; one skin's `dye_default` sizes and materials match; one color's `rgb["cloth"]` matches.
  - `batch_chunks_at_200`: `items()` with 450 ids makes 3 requests with 200/200/50 ids.
  - `batch_206_is_success`: route returns status 206 with a one-element array → map of size 1, no throw.
  - `http_401_message_has_no_key`: route 401 → `ApiError` with `status()==401`, message contains `"API key rejected"` and not `"KEY"`.
  - `transport_failure_message`: `{0, "", "timeout"}` → message starts `"Could not reach api.guildwars2.com"`.

- [ ] **Step 3: Run to verify failure** — expected compile error (`gw2_api.h`).

- [ ] **Step 4: Implement `gw2_api.h/.cpp`** per Interfaces. Use `.value(...)`/`contains` for optional fields; absent `skin`/`default_skin` → `nullopt`; skins without `details.dye_slots` → empty `dye_default`.

- [ ] **Step 5: Run tests** — `cm_test_character.exe api` → PASS.

- [ ] **Step 6: Commit**

```bash
git add tools/character/capture_fixtures.py tests/data/character tests/CMakeLists.txt include/castlemist/character/gw2_api.h src/character/gw2_api.cpp tests/test_character_api.cpp
git commit -m "character: typed GW2 API client with scrubbed real-account fixtures"
```

---

### Task 4: Resolver + `fetch_character`

**Files:**
- Create: `include/castlemist/character/manifest.h` (types), `include/castlemist/character/resolver.h`, `src/character/resolver.cpp`
- Create: `include/castlemist/character/fetch.h`, `src/character/fetch.cpp`
- Create: `tests/test_character_resolver.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 3 types and `Gw2Api`; `castlemist::cmap::{built, resolve_all, item_links, CONTENT_TYPE_SKIN}`.
- Produces (`manifest.h`):
  - `enum class PieceStatus { Ok, NoSkin, Unresolved, NoContentMap };`
  - `const char* to_string(PieceStatus);` → `"ok"`, `"no_skin"`, `"unresolved"`, `"no_content_map"`; `std::optional<PieceStatus> piece_status_from_string(std::string_view);`
  - `struct ManifestDye { uint32_t color_id = 0; std::string color_name, material; std::array<uint8_t, 3> rgb{}; bool known = false; };`
  - `struct ManifestPiece { std::string slot; uint32_t item_id = 0; std::string item_name; uint32_t skin_id = 0; std::string skin_name, weight_class; std::vector<uint32_t> file_ids; std::vector<ManifestDye> dyes; PieceStatus status = PieceStatus::NoSkin; };`
  - `struct CharacterManifest { std::string name, race, gender, profession; int level = 0; int tab_id = 0; std::string tab_name; std::vector<ManifestPiece> pieces; };`
- Produces (`resolver.h`):
  - `class AssetLookup { public: virtual ~AssetLookup() = default; virtual bool built() const = 0; virtual std::vector<uint32_t> skin_assets(uint32_t skin_id) const = 0; virtual std::optional<uint32_t> item_skin(uint32_t item_id) const = 0; };`
  - `class CmapAssetLookup final : public AssetLookup` — `built()` = `cmap::built()`; `skin_assets` = `cmap::resolve_all(CONTENT_TYPE_SKIN, id)`; `item_skin` = first `item_links(id)` entry with `type == CONTENT_TYPE_SKIN && via_item == 0`.
  - `std::vector<uint32_t> collect_item_ids(const EquipmentTab&);`
  - `std::vector<uint32_t> collect_skin_ids(const EquipmentTab&, const std::map<uint32_t, ApiItem>&, const AssetLookup&);`
  - `std::vector<uint32_t> collect_color_ids(const EquipmentTab&, const std::map<uint32_t, ApiSkin>&);` — all three sorted, de-duplicated.
  - `CharacterManifest resolve_character(const CharacterCore&, const EquipmentTab&, const std::map<uint32_t, ApiItem>&, const std::map<uint32_t, ApiSkin>&, const std::map<uint32_t, ApiColor>&, const AssetLookup&);`
- Produces (`fetch.h`):
  - `struct TabSummary { int tab = 0; std::string name; bool is_active = false; };`
  - `struct FetchResult { CharacterManifest manifest; std::vector<TabSummary> tabs; };`
  - `FetchResult fetch_character(Gw2Api& api, const std::string& name, std::optional<int> tab, const AssetLookup& assets);` — core → tabs → pick `tab` (else the active one; requested tab absent → `ApiError(0, "Character has no equipment tab <n>")`) → items → skins → colors → `resolve_character`.

**Resolution rules (per equipment entry, in tab order):**
1. `skin_id` = entry `skin`, else `items[item_id].default_skin`, else `assets.item_skin(item_id)`, else 0.
2. `skin_id == 0` → `NoSkin` (no file_ids, no dyes).
3. `!assets.built()` → `NoContentMap`; else `file_ids = assets.skin_assets(skin_id)`, empty → `Unresolved`, else `Ok`.
4. Dye slots: `skins[skin_id].dye_overrides[race + gender]` if present, else `dye_default`. For slot `i` (only as many slots as that list has): color = entry `dyes[i]` if set, else the slot's default `color_id`; `material` from the slot (null slot → skip the dye entirely). `rgb = colors[color].rgb[material]`; `known = false` when the color or that material is missing.
5. Names: `item_name`/`skin_name`/`weight_class` from the maps; empty when absent.

- [ ] **Step 1: Write failing tests** (group `resolver`) with hand-built small inputs and a `FakeAssets` (`built` flag, `std::map<uint32_t, std::vector<uint32_t>> skins`, `std::map<uint32_t,uint32_t> item_skins`):
  - `transmute_override_wins`: entry `skin=10`, item `default_skin=20` → `skin_id 10`.
  - `default_skin_used_without_override`: no `skin` → `skin_id 20`.
  - `item_missing_from_api_uses_cmap_skin`: item absent from items map, `item_skins[7]=30` → `skin_id 30`, `item_name` empty, status `Ok` when `skins[30]` non-empty.
  - `trinket_is_no_skin`: no skin anywhere → `NoSkin`, empty `file_ids`/`dyes`.
  - `unresolved_and_no_content_map`: built + empty assets → `Unresolved`; `built=false` → `NoContentMap`.
  - `null_dye_falls_back_to_skin_default`: skin default slots `[{5,"cloth"},{6,"leather"}]`, entry dyes `[100, null, null, null]` → dyes `[100/cloth, 6/leather]`, rgb taken from the matching material.
  - `race_gender_override_slots`: skin overrides `"CharrFemale"` used when core is Charr/Female.
  - `weapon_has_no_dyes`: skin with empty `dye_default`, entry dyes `[1,2,3,4]` → `dyes.empty()`.
  - `unknown_color_is_marked`: color id absent from colors map → `known == false`, piece still `Ok`.
  - `collect_ids_dedup_sorted`: two rings with the same item → `collect_item_ids` has it once; output sorted.
  - `fetch_character_end_to_end` (group `fetch`): `FakeHttpClient` with the Task 3 fixtures + `FakeAssets{built=true}` where every fixture skin id maps to `{1}` → manifest name `"Test Character"`, `tab_id` equals the fixture's active tab, piece count equals that tab's equipment count, no `NoContentMap`.
  - `fetch_missing_tab_throws`: `tab=99` → `ApiError` mentioning `"no equipment tab 99"`.

- [ ] **Step 2: Run to verify failure** — compile error.

- [ ] **Step 3: Implement** `manifest.h`, `resolver.h/.cpp`, `fetch.h/.cpp` per the rules above.

- [ ] **Step 4: Run tests** — `cm_test_character.exe resolver` and `... fetch` → PASS; full `cm_test_character.exe` → PASS.

- [ ] **Step 5: Commit** — `git commit -m "character: resolve equipment to skins, fileIds and dyes; fetch_character"`

---

### Task 5: Manifest JSON

**Files:**
- Create: `include/castlemist/character/manifest_json.h`, `src/character/manifest_json.cpp`
- Create: `tests/test_character_manifest_json.cpp`
- Modify: `tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `manifest.h` (Task 4).
- Produces: `nlohmann::json manifest_to_json(const CharacterManifest&);` and `CharacterManifest manifest_from_json(const nlohmann::json&);` (throws `std::runtime_error("unsupported manifest version")` unless `"version" == 1`).
- Shape: `{"version":1,"name","race","gender","profession","level","tab":{"id","name"},"pieces":[{"slot","item_id","item_name","skin_id","skin_name","weight_class","file_ids":[...],"status":"ok","dyes":[{"color_id","color_name","material","rgb":"#RRGGBB","known"}]}]}`. `rgb` is uppercase hex.

- [ ] **Step 1: Write failing tests** (group `manifest_json`):
  - `round_trip`: a manifest with two pieces (one `Ok` with two dyes, one `NoSkin`) → `manifest_from_json(manifest_to_json(m))` equal field by field.
  - `rgb_is_hex`: rgb `{255, 16, 0}` serializes as `"#FF1000"`.
  - `rejects_other_versions`: `"version": 2` → throws.

- [ ] **Step 2: Run to verify failure** — compile error.
- [ ] **Step 3: Implement** per Interfaces.
- [ ] **Step 4: Run tests** — `cm_test_character.exe manifest_json` → PASS.
- [ ] **Step 5: Commit** — `git commit -m "character: manifest JSON (v1) read/write"`

---

### Task 6: `gw2dat_cli character`

**Files:**
- Modify: `tools/gw2dat_cli/main.cpp` (header comment command list, new `cmd_character`, dispatch + usage string)
- Modify: `tools/CMakeLists.txt` (`gw2dat_cli` DEPS add `castlemist::character`)

**Interfaces:**
- Consumes: `KeyStore`, `default_key_file`, `WinHttpClient`, `Gw2Api`, `missing_scopes`, `CmapAssetLookup`, `fetch_character`, `manifest_to_json`, `cmap::load`.
- Command: `character (--key-name NAME | --key KEY) [--character NAME] [--tab N] [--out FILE] [--cmap FILE]`
  - Key: `--key` wins; else `--key-name` from `default_key_file()`; neither → `fail("need --key or --key-name")`; unknown name → `fail("no saved key named '<name>' in <path>")`.
  - Always calls `token_info()` first; missing scopes → `fail("API key is missing scopes: builds, ...")`.
  - No `--character` → `{"ok":true,"characters":[...]}`.
  - With `--character`: loads `--cmap` (default `content_map.bin` beside the exe; a load failure is not fatal — pieces become `no_content_map`), runs `fetch_character`, emits `{"ok":true,"cmap":<bool built>,"tabs":[{"id","name","active"}],"manifest":{...}}`; `--out` also writes the manifest JSON (pretty, 2 spaces) to that file.

- [ ] **Step 1: Implement `cmd_character`** per Interfaces (existing `need`/`has`/`fail`/`emit` helpers).
- [ ] **Step 2: Build** — `cmake --build --preset debug --target gw2dat_cli` → succeeds.
- [ ] **Step 3: Verify error paths offline**
  - `build/debug/bin/gw2dat_cli.exe character` → `{"ok":false,"error":"need --key or --key-name"}`, exit 1.
  - `... character --key-name Nope` → error naming `Nope` and the key file path.
- [ ] **Step 4: Verify live** (the one sanctioned network check for the CLI)
  - `... character --key-name Main` → `ok:true`, 9 character names.
  - `... character --key-name Main --character "<one of them>" --out %TEMP%/m.json` → `ok:true`, pieces present; with a GUI-built `content_map.bin` beside the exe, armor pieces are `ok` with non-empty `file_ids`. The output does not contain the key.
- [ ] **Step 5: Commit** — `git commit -m "gw2dat_cli: character command (list characters, emit manifest)"`

---

### Task 7: Shared content-map service (UI)

**Files:**
- Create: `src/ui/content_map_service.cpp`
- Modify: `src/ui/detail/app_state.h` (declarations), `src/ui/chat_link_dialog.cpp` (remove `g_cmap_building`, `cmap_cache_path`, the build body of `cl_resolve_asset`, and `cl_rebuild_map`'s body; call the service)

**Interfaces:**
- Produces (declared in `app_state.h`):
  - `enum class CmapEnsure { Ready, Building, Started, NeedDat, NeedIndex };`
  - `CmapEnsure ensure_content_map(HWND notify);` — `Ready` if built or the disk cache loads; `Building` if a build is running (adds `notify` to the waiters); `NeedDat` / `NeedIndex` for the two existing message-box cases (caller shows the message); otherwise starts the existing background build and returns `Started`. On completion, `WM_APP_CMAP_DONE` is posted to every waiter that `IsWindow()`.
  - `void rebuild_content_map(HWND notify);` — no-op returning while building; else `cmap::clear()`, delete cache, `ensure_content_map(notify)`.
  - `std::wstring cmap_cache_path();` (moved unchanged)
  - Waiters: `std::vector<HWND>` guarded by a `std::mutex`, de-duplicated.

- [ ] **Step 1: Move the code** into `content_map_service.cpp`; rewrite `cl_resolve_asset` as a `switch` over `ensure_content_map(g_cl_wnd)` keeping today's exact status texts and message boxes; `cl_rebuild_map` → `rebuild_content_map(g_cl_wnd)`.
- [ ] **Step 2: Build + existing tests** — `cmake --build --preset debug && ctest --preset debug` → all pass.
- [ ] **Step 3: Manual check** — launch castlemist, open the index + dat, Tools › Decode Chat Link, paste an item link (e.g. Astralaria), Resolve → assets listed as before; Rebuild map → rebuilds and lists again.
- [ ] **Step 4: Commit** — `git commit -m "ui: move content-map build into a shared service"`

---

### Task 8: Character dialog (UI)

**Files:**
- Create: `src/ui/character_dialog.cpp`, `src/ui/character_keys_dialog.cpp`
- Modify: `src/ui/detail/app_state.h` (IDs, messages, entry points), `src/ui/window_proc.cpp` (menu item + command case), `src/ui/chat_link_dialog.cpp` (rename `cl_open_fid` → shared `navigate_to_file_id`)

**Interfaces:**
- Consumes: Task 1–5 APIs; `ensure_content_map`, `rebuild_content_map` (Task 7); the chat-link dialog's file-id navigation (lift `cl_open_fid` into a shared `void navigate_to_file_id(uint32_t fid);` declared in `app_state.h`, defined in `chat_link_dialog.cpp`).
- Produces: `void open_character_dialog(HWND owner);`, `void open_character_keys_dialog(HWND owner, std::function<void()> on_changed);`
- IDs (verified free): `ID_TOOLS_CHARACTER = 1019`; `ID_CH_KEY_COMBO 2150`, `ID_CH_MANAGE 2151`, `ID_CH_FETCH 2152`, `ID_CH_CHAR_LIST 2153`, `ID_CH_TAB_COMBO 2154`, `ID_CH_TABLE 2155`, `ID_CH_OPEN_MODEL 2156`, `ID_CH_SAVE 2157`, `ID_CH_CLOSE 2158`, `ID_CH_BUILD_MAP 2159`; keys window `ID_CK_LIST 2160`, `ID_CK_NAME 2161`, `ID_CK_KEY 2162`, `ID_CK_SHOW 2163`, `ID_CK_SAVE 2164`, `ID_CK_RENAME 2165`, `ID_CK_REMOVE 2166`, `ID_CK_CLOSE 2167`.
- Messages: `WM_APP_CHAR_NAMES_DONE = WM_APP + 6`, `WM_APP_CHAR_FETCH_DONE = WM_APP + 7`; the worker heap-allocates its result (`std::vector<std::string>` names or `FetchResult`, or an error string) and passes it in `lParam`; the UI thread takes ownership with `std::unique_ptr`.
- Menu: `Tools › &Character Ripper...` directly after "Decode Chat Link".
- Text: character names and all API strings are UTF-8 → convert with `MultiByteToWideChar(CP_UTF8, ...)`; selected names back with `WideCharToMultiByte(CP_UTF8, ...)`.

**Behavior:**
- Opens with the key combo filled from `KeyStore::load(default_key_file())` (load error → status line shows it, combo empty). Manage keys… opens the keys window (`ES_PASSWORD` key field, a Show checkbox toggles the mask); saving writes the file and refreshes the combo.
- Fetch (background): `token_info` → missing scopes → status `"Key is missing scopes: builds. Add them at account.arena.net/applications."`; else character list filled.
- Selecting a character (background): `ensure_content_map` is called first; any result other than `Ready` still fetches, and the status line notes `"Content map not built - models unavailable (Build map)"`. Then `fetch_character(..., selected tab or nullopt, CmapAssetLookup)`; the tab combo is filled from `FetchResult::tabs` (active preselected; changing it refetches).
- Table (report-view ListView): columns Slot, Item, Skin, Dyes (`"Abyss #1A1A1A, ..."`), Status.
- Open model: enabled for an `Ok` row → `navigate_to_file_id(file_ids[0])`.
- Save manifest…: `GetSaveFileNameW`, default name `<character>.json`, writes `manifest_to_json(...).dump(2)`.
- Build map: `rebuild_content_map(hwnd)` if a map exists, else `ensure_content_map(hwnd)`; on `WM_APP_CMAP_DONE` the current character is re-resolved.
- While a request is running, Fetch and the character list are disabled; errors (`ApiError::what()`) go to the status line.

- [ ] **Step 1: Implement** both dialogs, IDs, messages, the menu entry, and `navigate_to_file_id`.
- [ ] **Step 2: Build + tests** — `cmake --build --preset debug && ctest --preset debug` → all pass.
- [ ] **Step 3: Manual check (live)**
  - Tools › Character Ripper… → key `Main` listed → Fetch → 9 characters.
  - Pick a character → table fills; armor rows `ok` with dyes; trinkets `no_skin`.
  - Switch equipment tab → table changes. Open model on an armor row → main browser jumps to that fileId and previews the model.
  - Save manifest → JSON matches the CLI's for the same character/tab.
  - Manage keys: add a bogus key `Bad` → Fetch → "API key rejected (HTTP 401)…"; remove it.
- [ ] **Step 4: Commit** — `git commit -m "ui: Character Ripper dialog (keys, characters, resolved equipment)"`
