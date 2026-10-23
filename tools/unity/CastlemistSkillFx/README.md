# castlemist skill effects for Unity

Turns a `gw2dat_cli skillfx-export --fbx` folder into prefabs: one per GW2 skill, holding that
skill's effect models and their particle systems. Spec:
`docs/superpowers/specs/2026-10-22-skill-effects-design.md`; research:
`docs/research/gw2-skill-effects.md`.

## Use

1. `tools/unity/Castlemist` must already be in the project (the world importer: this folder uses its
   `ParticleEffects`, `CastlemistMaterials`, `MaterialsFile` and the Castlemist shaders). Copy this
   folder (`CastlemistSkillFx`) into `Assets/` beside it.
2. Copy the export folder (or a subset: `skill_set.json`, the `skills/<id>-*` folders wanted and the
   `models/<id>` folders their `skill.json` files name) under `Assets/`.
3. **Castlemist ▸ Import Skill Effects…** and pick that folder. Batchmode:
   `-executeMethod Castlemist.SkillFx.SkillFxImporter.BatchImport -castlemistSkillFx Assets/<folder> [-castlemistSkills 44364,41258]`.

## What it builds (under `<folder>/Unity/`)

| Asset | From |
|---|---|
| `Materials/<id>/<FileName>.mat` | `CastlemistMaterials.BuildModelMaterial`, per mesh material of `models/<id>/materials.json` |
| `Materials/Particles/*.mat` | `ParticleEffects` (`Castlemist/GW2 Particle`, one per atlas rect) |
| `Effects/<id>.prefab` | the model's `.fbx` (world settings, materials remapped by the world importer's name rule), or an empty root for a particle-only model, plus `ParticleEffects.AddTo` |
| `Skills/<id>-<slug>.prefab` | a root scaled by 0.0254 (GW2 inches → metres, the world importer's default, UNPROVEN) holding one instance of each effect prefab the skill's `skill.json` lists |

`ParticleEffects` reads the stored emitter ranges as `{min, max}`, which the client's loader
confirms (GrCloud.cpp; `docs/research/gw2-particle-system.md`, corrected 2026-10-22).

## First import (2026-10-22, GW2Porting, Unity 2022.3.22)

Tome of Justice and its five pages imported: 6 skill prefabs, 9 materials, 95 particle systems,
0 errors. `44364-tome-of-justice` plays its 19 emitters: the open book (about 0.9 m wide,
1.7 m above the prefab's origin), the rising rune glyphs, the flying pages, blue sparks, and the
light column and beams. Compared with castlemist's preview, two things are off. Both are in
`ParticleEffects`, so they show on world particles too:
- the `LightBms` streaks (Stretch mode) come out as broad, near-solid beige pillars, not thin rays;
- the `PaperA01` and `PaperB01` sprites are dark, where the game shows light pages.

## UNPROVEN / not yet

- Effects play continuously (looping). GW2 starts and stops them with the skill. Triggering them is
  stage 3 (the VRCFury avatar menu).
- The particle systems sit at the effect model's origin or its bones. Where the effect attaches on
  the caster (hand, ground, target) is not in the export yet.
- Character cast and hold animations are a separate sub-project.
