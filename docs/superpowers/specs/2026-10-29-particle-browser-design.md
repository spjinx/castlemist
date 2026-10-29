# Particle Browser — Design (Particle Browser sub-project C)

Date: 2026-10-29
Status: written as the record (autonomous run; programme: 2026-10-28-effect-decode-design.md).

## Goal

One window in the castlemist app to find any GW2 effect, see it run with the client's maths,
read every decoded field with its meaning, isolate single emitters, and export it for Unity —
replacing the "export, import, eyeball, tweak, repeat" loop.

## What exists to build on

- `fxsim::Sim` (B): deterministic sim + `Stats`; the main preview already draws from it
  (`render/particles.cpp`), every sprite alignment case and each material's blend.
- `fx-census` / `fx-census --what effects` (A): the feature scan and the type-183 census.
- `skillfx::read_effect`, `effect_class_of_kind` (A): effect objects, their class, model, sound.
- The Texture Gallery (`src/ui/texture_gallery*.cpp`, `src/ui/detail/texture_gallery.h`): the
  pattern for a top-level browser window, a background scan cached to `dumps/index/` tied to
  the dat fingerprint, a virtual owner-drawn list, pushing an entry into the main preview.

## Design

1. **Window**: View > Particle Browser... opens a resizable top-level window: list on the
   left, inspector on the right; the main window's preview shows the selected effect.
2. **Two sources** (tabs over the list):
   - **Effects**: every type-183 object (≈46k): uid, codename, class (`effect_class_name`),
     model fileId, sound, child count for compounds. Read from the content packs on open
     (~1 min, on a worker; cached beside the content map like the npc catalogue).
   - **Models**: every MODL that carries effects, with per-model feature counts (clouds,
     emitters, streaks, lightning, lights, mesh emitters, bursts, alignment cases, clip effect
     tracks). Filled by a **background scan** (the census walk, `Extractor::extractEffects`),
     resumable, cached to `dumps/index/gw2_fx_index.bin` keyed on the dat fingerprint; rows
     appear as the scan reaches them, with a progress line ("123,456 / 200,172 models").
3. **Filters**: class (Effects tab); feature checkboxes (Models tab: has streak / lightning /
   mesh emitter / burst / clip tracks, alignment case); search by id, id range, pasted list or
   codename substring.
4. **Selecting** a row loads its model into the main preview (an effect row loads its model;
   a compound lists its children in the inspector, each clickable).
5. **Inspector**:
   - the tree cloud → emitter, each emitter with its decoded fields grouped (spawn, motion,
     colour, size, sprite, flipbook, wind, flags) and units/meanings from the research note
     (e.g. "alignment 1 → axis billboard along motion", "flags 0x400 → scale preset reversed",
     "emitterFlags 0x8 → burst"); raw values beside them;
   - **solo / mute** per emitter (the preview draws only the chosen ones);
   - the sim's **approximations** list, so it is clear what is not yet the client's behaviour;
   - **stats** (`fxsim::Stats`): live count, spread percentiles, mean speed, alpha and size over
     life, per emitter, refreshed while it plays.
6. **Timeline**: play / pause / restart; a time slider that seeks by re-running the sim from 0
   at the fixed step (deterministic, so a seek is exact); seed; speed (0.1×–4×).
7. **Export**: "Export for Unity…" writes the existing model export (glb, materials,
   `_particles.json`) for the selection; "Copy IDs" puts the selected ids on the clipboard;
   "Save stats…" writes `stats_json` (D compares Unity against it).

## Code

- `src/ui/particle_browser.cpp` (window, list, inspector, timeline) and testable units in
  `src/ui/detail/particle_browser.h` + `src/ui/particle_browser_model.cpp`: the fx index file
  (read / write / merge, fingerprint check), row filtering and search parsing (reusing the
  gallery's id-list parser), the emitter field describer (field → label, value, meaning), and
  the seek helper (`seek(Sim&, input, seed, dt, t)`).
- `render`: `set_effect_solo(std::vector<bool>)` and `effect_sim()` access for the timeline /
  stats (the sim stays owned by the renderer).
- Menu entry and `GW2_PARTICLES=<fileId>` (+ `GW2_PARTICLES_SHOT`, `_QUIT`) for a headless
  screenshot, as the gallery does.

## Testing

- Unit (`cm_test_ui`): fx index round-trip and fingerprint mismatch → rescan; filter and search
  parsing; describer output for known flag / alignment / burst values; seek determinism (seek to
  t equals stepping to t).
- App: headless shot of the browser on 1450958 (pillar) with emitter 2 soloed.

## Out of scope

Unity changes (D); NPC links (E); editing effect data.
