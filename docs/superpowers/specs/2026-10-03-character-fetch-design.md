# Character Fetch — Design (sub-project 1 of the Character Ripper)

Date: 2026-10-03
Status: approved in brainstorming, pending written-spec review

## Goal

Point castlemist at a GW2 API key, list that account's characters, pick one,
and produce a **CharacterManifest**: everything the character wears, resolved
from API ids down to dat fileIds, with dye colors. The manifest is the input to
every later sub-project.

The end goal of the whole effort is a **rigged, assembled character usable as a
VRChat avatar** (via glb -> Blender -> Unity). This sub-project only fetches and
resolves; it writes no models.

## Roadmap (the whole effort)

| # | Sub-project | Output |
|---|---|---|
| 1 | **Character fetch** (this spec) | CharacterManifest (UI table + JSON) |
| 2 | Per-piece export | One dyed `.glb` per equipped piece, correct race/gender variant |
| 3 | Assembly | One rigged `.glb`: base body + armor on a shared skeleton, body-part hiding, weapons on attachment bones |
| 4 | Head & hair | User picks the race/gender's stock face, hairstyle and hair color from presets; attached in the assembly |

Each sub-project gets its own spec -> plan -> implementation cycle. Known open
research for later: how armor skins encode race/gender variants (2), the dye
channel-mask shader (2), attachment points and body-part hiding (3).

## Constraints

- **Network is opt-in and narrow.** This is castlemist's first network access.
  It happens only when the user clicks Fetch in the character dialog or runs
  the `character` CLI command, and only to `https://api.guildwars2.com`.
  Nothing else in the app goes online.
- **No new third-party dependencies.** WinHTTP (system) + the already-linked
  `nlohmann::json`.
- **Keys are stored in plain text** in a local, git-ignored file (user's
  explicit choice — no registry, no encryption).
- **Core module has no UI dependency** and is safe on a background thread, like
  `extract_entry` and the glTF exporter.

## Components

New module: `include/castlemist/character/`, `src/character/`.

### `http`
- `struct HttpResponse { int status; std::string body; std::string error; };`
- `class HttpClient { virtual HttpResponse get(const std::string& url,
  const std::vector<std::pair<std::string,std::string>>& headers) = 0; };`
- `WinHttpClient` — HTTPS GET via WinHTTP, 15 s timeout, `User-Agent: castlemist`.
- Tests use a `FakeHttpClient` mapping URL -> canned response.

### `gw2_api`
Typed calls over an `HttpClient&` and a key, sent as an
`Authorization: Bearer <key>` header (never in the URL, so it can't leak into
logs or error messages). Each parses JSON into plain structs:

| Call | Endpoint | Result |
|---|---|---|
| `token_info` | `/v2/tokeninfo` | key name, permissions list |
| `character_names` | `/v2/characters` | `vector<string>` |
| `character_core` | `/v2/characters/:name/core` | race, gender, profession, level |
| `equipment_tabs` | `/v2/characters/:name/equipmenttabs?tabs=all` | per tab: tab id, name, is_active, equipment[] |
| `items` | `/v2/items?ids=...` | id, name, type, default_skin, details.weight_class |
| `skins` | `/v2/skins?ids=...` | id, name, type, details.dye_slots.default[] (color_id, material) |
| `colors` | `/v2/colors?ids=...` | id, name, cloth/leather/metal/fur `rgb` |

Equipment entry fields kept: `id` (item), `slot`, `skin` (optional transmute
override), `dyes` (4 entries, nullable), `upgrades`, `infusions`.
Character names are URL-encoded. Batched `ids=` calls are chunked at 200 ids.
A whole character is ~6 requests; no rate-limit handling beyond that.

`required_scopes()` = `account`, `characters`, `builds`. `missing_scopes(token_info)`
returns which are absent so the UI can name them.

### `key_store`
- File: `api_keys.json`, format `{"keys":[{"name":"Main","key":"..."}]}`.
- Location: the first ancestor of the exe's directory that contains castlemist's
  top-level `CMakeLists.txt` (the repo root for dev builds); otherwise the exe's
  own directory (packaged release).
- API: `load(path)`, `save(path)`, `list()`, `add(name,key)`, `rename`, `remove`,
  `get(name)`. Names are unique; `add` with an existing name replaces its key.
- `.gitignore` gains `/api_keys.json` under "local only".

### `resolver`
Pure function, no I/O:

```
CharacterManifest resolve_character(const CharacterCore&, const EquipmentTab&,
                                    const ItemMap&, const SkinMap&, const ColorMap&,
                                    const AssetLookup&);
```

`AssetLookup` is a small interface over `cmap` (`item_links`, `resolve_all`,
`built`) so tests can supply an in-memory fake.

Per slot it produces a `ManifestPiece`:
- `slot`, `item_id`, `item_name`
- `skin_id`: equipment `skin` if present, else the item's `default_skin` from
  the API, else the first `CONTENT_TYPE_SKIN` link from `cmap::item_links`
- `skin_name`, `weight_class` (armor only)
- `file_ids`: `cmap::resolve_all(CONTENT_TYPE_SKIN, skin_id)` (model first)
- `dyes[4]`: `{color_id, color_name, material, rgb}`; when the equipment dye is
  null the skin's default dye slot is used; `material` comes from the skin's
  `dye_slots`, and `rgb` is the color's entry for that material
- `status`: `ok` | `no_skin` (empty/hidden slot or skin-less item such as
  trinkets) | `unresolved` (skin not in the dat) | `no_content_map`

`CharacterManifest` = `name`, `race`, `gender`, `profession`, `level`,
`tab_id`, `tab_name`, `pieces[]`.

### `manifest_json`
`to_json(CharacterManifest)` / `from_json(...)`, with a `"version": 1` field.
Round-trips exactly.

## Front ends

### UI — `src/ui/character_dialog.cpp`
A modeless tool window in the style of the chat-link dialog:
- Key dropdown + "Manage keys..." (add / rename / remove; key field masked
  except while editing)
- **Fetch** -> character list; selecting a character fetches and resolves it
- Equipment-tab dropdown (active tab preselected)
- Slot table: slot, item, skin, dyes (color swatches + names), status
- Row action **Open model** -> reuses the main window's file-id navigation
- **Save manifest...** -> JSON file
- Status line for progress and errors; all network work on a background thread

Opened from the menu next to "Decode Chat Link".

**Shared cmap build:** the content-map build/load currently lives inside
`chat_link_dialog.cpp`. It moves into a shared UI helper used by both dialogs.
This touches `chat_link_dialog.cpp`, so it waits until the user's current
uncommitted work there is committed.

### CLI — `gw2dat_cli character`
```
gw2dat_cli character (--key-name NAME | --key KEY) [--character "Name"]
                     [--tab N] [--out manifest.json] [--dat Gw2.dat]
```
- No `--character`: print the character list.
- With `--character`: resolve and print a slot table, or write JSON with `--out`.
- `--dat`/cmap cache is used for fileIds when available; otherwise pieces are
  `no_content_map`.

## Data flow

1. Choose key -> `token_info` -> if scopes are missing, stop and name them.
2. `character_names` -> user picks one.
3. `character_core` + `equipment_tabs`.
4. Collect item/skin/color ids from the chosen tab -> one batched `items`,
   `skins`, `colors` round (skins includes item default skins, so `items`
   runs before `skins`).
5. `resolve_character` -> manifest -> table and/or JSON.

## Error handling

- Network/HTTP failure, 401/403 (bad key): a clear status message; no partial
  manifest is shown.
- Missing scopes: the message names each missing scope.
- Content map not built: the API part still completes; rows are
  `no_content_map`; the dialog offers the existing Build action.
- Skin not in the dat (e.g. game patched since the cache was built): that row
  is `unresolved`; the other rows are unaffected.
- Unknown dye ids or empty slots: carried through with their status, never
  silently dropped.

## Testing

- `tests/data/character/` holds canned API JSON (tokeninfo, characters, core,
  equipmenttabs, items, skins, colors), captured once from a real account and
  scrubbed of the account name.
- Unit tests (`tests/test_character_*.cpp`, a new `cm_test_character` binary via
  `castlemist_add_test(character ...)` on the in-house `test_framework.h`):
  - API parsing of each canned response via `FakeHttpClient`
  - scope check
  - resolver: transmute override vs default skin; null dye falls back to the
    skin default; material -> rgb pick; `no_skin` / `unresolved` /
    `no_content_map` statuses (fake `AssetLookup`)
  - key store round trip and root-path fallback
  - manifest JSON round trip
- No automated test touches the network. One manual live run of the CLI
  against a real key before calling it done.

## Out of scope (for this sub-project)

Model export, dye baking, race/gender variant selection, assembly, heads/hair,
inventory/bank contents, outfits/gliders/mounts (the API does not report which
of these is equipped).
