# NPC Catalogue — Stage 3 (UI) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A **Tools ▸ NPC catalogue…** window for looking up NPCs: filter, creatures grouped by body, the NPCs of the selected creature, their clips, and **Open model** / **Export…** buttons.

**Architecture:**
- **Pure grouping and filtering** in the `npc` layer (`browse.h`), unit-tested.
- **A Win32 tool window** in `ui` (`npc_dialog.cpp`), following `chat_link_dialog.cpp`. The catalogue is loaded or built on a background thread after `ensure_content_map`.
- **Open model** reuses `navigate_to_file_id`; **Export…** calls `npc::export_npc`.

**Tech Stack:** C++20, Win32 (comctl32 list views), castlemist ui/npc layers.

**Spec:** `docs/superpowers/specs/2026-10-14-npc-catalogue-design.md` (UI bullet under Architecture).

## Global Constraints

- Do not modify content_map, content_schema, content_store, type_names or any existing label/name. Add, never rename. Adding a Tools menu item, a command ID and a forward declaration in `app_state.h` is allowed.
- Never read Gw2.dat directly.
- Build and test only through `scratchpad/cmbuild.sh`.
- Menu text: `NPC &catalogue...`, placed after `&Character Ripper... (GW2 API)`.
- Catalogue cache: `npc_catalogue.json` beside the exe (same folder as the content-map cache, from `cmap_cache_path()`'s directory).
- Group key: `model` when non-zero; otherwise `race` (an empty race gives the group label `(no body)`).
- Filter: case-insensitive substring over the decimal id, codename, race, decimal model and skill names (when a name provider is given). An empty filter matches everything.

## Review Focus

- Opening the window before a dat or index is loaded: the status line says "Load a dat and its index first" and nothing builds. Covered in Task 2 (code path plus a manual check).
- The content map still building: the window waits for `WM_APP_CMAP_DONE`, then builds the catalogue. Task 2.
- A filter that matches nothing: empty lists, no crash. Test `a_filter_matching_nothing_gives_no_groups`.
- A race-body NPC selected: **Open model** is disabled or beeps (model 0); **Export…** still works (weapons + manifest). Task 2.
- Closing the window while the background build runs: the result is dropped safely (the window handle is null-checked when the build posts back). Task 2.

---

### Task 1: Grouping and filtering (pure)

**Files:** Create `include/castlemist/npc/browse.h`, `src/npc/browse.cpp`; extend `tests/test_npc.cpp`.

**Interfaces — Produces (namespace `castlemist::npc`):**
```cpp
struct Group { uint32_t model = 0; std::string race; std::vector<const NpcRecord*> npcs; };  // npcs sorted by id
using SkillName = std::function<std::string(uint32_t skill_id)>;                           // may be empty
bool matches(const NpcRecord&, const std::string& filter, const SkillName& skill_name);
std::vector<Group> group_creatures(const Catalogue&, const std::string& filter, const SkillName& skill_name);
std::string group_label(const Group&);   // "model <fileId>" or the race or "(no body)"
```
Groups are sorted by NPC count descending, then by model, then by race.

- [ ] **Step 1: Failing tests.**
  - `groups_by_model_then_race`: NPCs {1: model 104256}, {2: model 104256}, {3: model 0, race humanmale} give 2 groups; the first has model 104256 and 2 npcs; the second has race `humanmale` and label `"humanmale"`.
  - `filters_by_id_codename_race_model_and_skill_name`: `matches` is true for `"5758"` against id 5758; for `"VL8A"` against codename `vl8Av.4gynM`; for `"human"` against race humanmale; for `"10425"` against model 104256; and for `"fireball"` with a provider mapping skill 5491 → `"Fireball"`.
  - `a_filter_matching_nothing_gives_no_groups`: `group_creatures(cat, "zzzz", {})` is empty.
- [ ] **Step 2–4:** RED, implement, GREEN.
- [ ] **Step 5:** Commit `feat(npc): group and filter the catalogue for browsing`.

### Task 2: NPC catalogue window

**Files:** Create `src/ui/npc_dialog.cpp`. Modify `src/ui/window_proc.cpp` (menu item and command), `src/ui/detail/app_state.h` (`ID_TOOLS_NPC_CATALOGUE` and `void open_npc_catalogue(HWND owner);`), and `CMakeLists.txt` (add `castlemist::npc` to the `ui` layer DEPS).

**Interfaces — Consumes:** Task 1; stage 1 `npc::load/save/build_catalogue`; stage 2 `npc::export_npc`; `ensure_content_map(HWND)` and `cmap_cache_path()` (content_map_service.cpp); `navigate_to_file_id(uint32_t)`; `g_app->data_gw2`, `g_app->dat_loaded`, `g_app->index_loaded`.

Layout:
- A filter edit along the top (it re-filters on `EN_CHANGE`).
- A left list view **Creatures** with columns Body and NPCs.
- A right list view **NPCs** with columns Id, Codename, Race, Weapons, Skills.
- A bottom list box **Clips**, filled on **Show clips** by `load_model_by_fileid(g_app->data_gw2, model)` with "name — group (duration s)".
- Buttons: **Show clips**, **Open model** (`navigate_to_file_id(model)`), **Export…** (a folder picker, then `export_npc(g_app->data_gw2, rec, folder)`, with the result's folder and note count shown in the status line), **Close**.
- A status line with coverage counts after the build ("23653 NPCs · 13534 with a body model · …").

Build flow:
1. On open: if the dat or index isn't loaded, show the status "Load a dat and its index first".
2. Else call `ensure_content_map(hwnd)`. When it reports already built, or on `WM_APP_CMAP_DONE`, start a `std::thread` that loads the cache or builds the catalogue. Collect the cntc entries the way `content_map_service.cpp` collects them for the content map; `container_of` uses `castlemist::db::lookup(get_by_base_id(...)).container`.
3. Copy the MftData list and fileIds on the UI thread before starting the thread.
4. Save the result when it isn't empty, then post a private `WM_APP` message to the window with the result in a heap object; delete it if the window has gone.

Skill names: use the UI's existing name cache in `content_names.cpp`, if it has a lookup by API endpoint (`"skills"`) and id. Otherwise pass an empty provider and record a ruling.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** `cmbuild.sh build castlemist` (the app target) and `cmbuild.sh test -j4` (expect 16/16).
- [ ] **Step 3: No GUI launch from the agent** (it would leave a window on the user's desktop). Record in the report the manual checks for the user: open the window with a dat loaded, filter `104256`, Show clips, Open model, Export….
- [ ] **Step 4:** Commit `feat(ui): NPC catalogue window`.
