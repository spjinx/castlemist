---
name: gw2-skill-effects
description: "How GW2 skills reach their visual effects in the cntc content datastore (type-183 effect objects), measured on Firebrand; mantras, tome bundles, and the unreferenced effects"
metadata:
  type: project
---

Measured 2026-10-22 on the live dat with `gw2dat_cli cgraph` (content-graph walk; modes
`--content-type/--content-id [--depth --reverse]`, `--prefix NS.`, `--files A,B`,
`--find HEX`, `--hex-of CODENAME`). Firebrand is the worked example.

## Objects

| type | what | evidence |
|---|---|---|
| 64 | skill (also boons/conditions: 717, 718, 719, 720, 721, 725, 726, 727, 736–743, 873, 1122, 1187 are boon/condition skills, not in the public API) | dataId @+40 = API id |
| 183 | **effect**. Optional AMSP sound + optional MODL. dataId = a small `kind` | 64 skill → 183 at +184/+216/+248/... |
| 65 | skill bar / profession table. Guardian 253 = every spec's virtues; tome bars 5887/5890/5979 | slots every 112 bytes from +192 |
| 35 | item. Each **tome is a bundle item**: +176 → skin 7532, +264 → its type-65 chapter bar, +272 → a type-37 object | Bp5rI namespace |
| 77 | buff / trait-like (icon + back-ref to a skill at +136) | |
| 377 | skill fact (icon) | tmGkI namespace |

Effect object layout: 16-byte GUID @+0, type @+16 (0xb7), uid @+20, dataId (`kind`) @+40.
`kind` values seen: **1** = group (refs to child effects at +112, +120), **5** = no files,
**6** = model and/or sound, **10** = model+sound (a shared weapon-swing effect, 57192),
**11** = no files, **0** = sound only. Names for these are not known; record the numbers.

Codename namespaces: `Bp5rI.` = Firebrand skills (137 type-64 objects, plus bundle items,
bars), `Bmt2N.` = Firebrand effects (68 type-183 objects, namespace index 3349).

## Mantras

Every Firebrand mantra has the same shape (Mantra of Solace 41714 shown):

- +232 → a "Mantra Active Skill" helper (68678) that lists the charges again;
- +736 → the first charge skill (41475 Restoring Reprieve) — **no effect of its own**;
  its visuals are the boons it applies (717 Protection, 873, 743 Aegis → effect 57178);
- +744 → the final charge (42960 Rejuvenating Respite) → effect **1764310**, shared by all
  six mantras' final charges;
- a 624xx group object (62463) → charge 1, charge 2 and a third skill (45141) carrying
  effect **1764482** twice (no sound), shared by all six mantras.

Mantra of Flame is shifted by 32 bytes (+264/+768/+776/+784) and has its own effect
1764358 at +184 (shared with Flame Rush).

## Tomes

Tome of Justice 44364 (+ its twin 68647) → effect 1764329 (9 clouds, 19 emitters, 1 light,
**one 660-triangle mesh = the floating book**). Resolve → 1764313, Courage → 1764322.
Chapters are NOT linked from the tome skill: they live on the bundle item's type-65 bar
(Justice 5887: 41258, 40635, 42449, 41169, 40015, 42898). Chapter skills mostly have no
effect object (41258 Searing Spell: none anywhere in its graph); 42898 → 296840.
Skin 7532 resolves only to a 1 KB DXT3 texture (64430), not a model.

## Unreferenced effects

51 of the 68 `Bmt2N.` effect objects (28 distinct effect models in the Firebrand fx
range 1764294–1764500; 16 copies of 1764358) are referenced by **nothing found**:

- no local/external pointer from any cntc object (all 32 packs);
- no trackedReferences (array 8; it parses cleanly — every target is an object start —
  but only targets types 64 and 0);
- their GUID occurs only in the object itself (all cntc content, anic, the 72 human-male
  animation banks, Gw2-64.exe);
- their uid and codename occur nowhere else; rootIndex is 0xFFFFFFFF for every effect.

The exe explains why (next section): the client also plays effects by a u32 id that it
receives at runtime.

## How the client reaches effects (Ghidra, 2026-10-22)

Static read of `Gw2-64.exe` (project `~/gw2re`, scripts in `tools/ghidra/`). The client
code lives in `Gw2\Game\Effect\Cli\`: `EfCliContext`, `EfCliEffect`, and one class per
effect type (`EfCliEffectCompound`, `...Model`, `...Audio`, `...Decal`, `...Material`,
`...PostProc`, `...ShakeSpring`). These likely match the `kind` numbers, but the mapping
is not proven.

Every place the client fetches an effect by id calls the content store's lookup with
**type 0xB7 (183) and a u32 id** (27 sites, 23 functions). The ones that matter:

- **`EfCliMsg.cpp`: two server-message handlers.** The message holds a count byte (+2) and
  an array of u32 effect ids (+3). Each id is looked up as type 183 and handed to the
  effect context (vtable +0x58 in one handler, +0x80 in the other). **The server names
  effects by id**, so a skill-to-effect link that only the server knows never appears
  in client data.
- **`AnmlAnimTriggers.cpp`: the animation-trigger listener** (assert text `effectDef`).
  It looks up an effect id in a runtime hash map at listener+0x58 (12-byte entries
  `{key, effectId, valid}`), keyed by a u32 from its owner (vtable +0x218), then plays
  the effect. No function with an AnmlAnimTriggers assert fills that map, so it is
  populated elsewhere at runtime.
- Also: `AvCharEffect.cpp`, `AvGadgetContainer.cpp`, `MslCliMissile.cpp` (missiles),
  `EvtCliEvent.cpp`, `CnUtil.h`.

Conclusion: the 51 unreferenced Firebrand effects are almost certainly played by id,
through server messages and/or the animation-trigger map. That id appears nowhere in
client content (the uid search found only the object itself), so **the skill-to-effect
pairing for them is not recoverable from client files**. Recovering it would take
watching live traffic, which this project does not do (static analysis only). For the
port: pair them with skills by eye, or offer them unpaired.

## Names from Elite Insights (no game needed)

Elite Insights' `GW2EIEvtcParser/ParserHelpers/GUIDs/EffectGUIDs.cs` names effects by
**the same 16 raw GUID bytes as an effect object's +0** (`FirebrandTomeOfJusticeOpen` =
`D573910F…` = `Bmt2N.a7wVK`, model 1764329). The community built that table from arcdps
combat logs, which record each effect a client plays together with its GUID. Matched
2026-10-22: **21 of the 68 Firebrand effects**, 14 of them orphans:

- every mantra's **Cone** (its own model each: Flame 1764377, Liberation 1764382, Lore
  1764387, Potence 1764394, Truth 1764404; Solace's cone uses 1764358 and its *Symbol*
  1764399) and **Symbol** (mostly 1764358);
- tome chapters: ScorchedAftermath2 1764457, ShiningRiver1 1764474, StalwartStand2
  1764441, ValiantBulwark 1764345;
- the three tome openings, and Symbol of Vengeance (normal + Writ of Persistence).

The other 37 (Tome of Justice's chapters among them) aren't in EI: it only lists effects
it uses for its own mechanics.

The content map (`cmap::users_of`) can't see type-183 users: it keys objects by
(type, dataId) and effect dataIds repeat. Use `cgraph --files`.
