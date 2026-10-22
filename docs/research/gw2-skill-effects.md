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

So either the client reaches them by a mechanism not yet seen (most likely through the
chapter/charge skills' animations: `ModelAnimationData` has `triggerTokens`,
`properties{id,time,val,strVal filename}`), or they are retired content (the 2022 mantra
rework). Chapter skills having no effects while showing fire in game points to the first.

The content map (`cmap::users_of`) can't see type-183 users: it keys objects by
(type, dataId) and effect dataIds repeat. Use `cgraph --files`.
