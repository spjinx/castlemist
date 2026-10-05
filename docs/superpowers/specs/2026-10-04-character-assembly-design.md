# Character Assembly — Design (sub-project 3 of the Character Ripper)

Date: 2026-10-04
Status: written autonomously at the user's direction; decisions recorded here.
Builds on: `2026-10-03-piece-export-design.md` (sub-project 2).
Research: `docs/research/gw2-armor-skins-and-dyes.md` sections 3 and 5.

## Goal

From a `CharacterManifest`, write **one rigged `.glb`**: the character's race
skeleton with its bare body, a default head (face + hair), every visible armor
piece and the back item bound to that skeleton, weapons attached to their
holster bones, and **one shared texture atlas** rebuilt the way the game
composites it. It should import into Blender and carry on to Unity/VRChat
without hand-fixing.

## Decisions

- **Rebuild the game's atlas, don't crop.** One 1024x1024 diffuse (dyes baked)
  and one normal atlas; every atlas mesh keeps its original (wrapped) UVs and
  uses a single material. This is what makes armor `Skin` patches show the
  body skin. Per-piece export (sub-project 2) is unchanged.
- **Head:** face `faces[0]` and hair `hairStyles[0]` of the race until
  sub-project 4 adds per-character presets. Undyed hair, untinted skin.
- **Body parts are dropped under armor:** chest under Coat, feet under Boots,
  hands under Gloves, legs under Leggings. Their textures still go into the
  atlas (armor `Skin` meshes sample them).
- **Hair is dropped under a Helm** (they share the helm's atlas rect).
- **Aquatic slots (HelmAquatic, WeaponAquaticA/B) are left out.**
- **Weapons:** option `weapons = stowed | hands | none`, default `stowed`.
  Only weapon set A (WeaponA1, WeaponA2). A weapon is placed by matching its
  `actionpoint:` joint to the race skeleton's (table in the research note) and
  becomes rigid-skinned to that body joint; its own bones and animations are
  dropped.
- **Back item** merges onto the race skeleton by joint name; joints the race
  skeleton lacks (cape bones) are appended, parented by name.
- **Materials:** one `CharacterAtlas` material (alpha MASK) for atlas meshes;
  non-atlas hair and weapons keep their own materials/textures.

## Components (ripper layer)

1. **composite.h additions** — `CompositeRace::skin_styles`
   (`std::vector<std::array<uint64_t,4>>`, chest/feet/hands/legs), `faces`,
   `hair_styles` (`std::vector<uint64_t>`).
2. **`atlas_builder.h`** — pure:
   `AtlasRegion region_for(const BlitRectSet&, const std::vector<MeshUvInfo>&, bool include_skin)`
   (rects the meshes fall in + anchor = their top-left) and
   `void blit(ImageRgba& atlas, const ImageRgba& tex, const AtlasRegion&)`
   (texture at 2x from the anchor, written only inside the region's rects).
3. **`skeleton_merge.h`** — pure over `ModelPreview`:
   `int merge_into(ModelPreview& dst, const ModelPreview& src, ...)` appends
   src meshes with bone indices remapped to dst joints by name (missing joints
   appended); `void attach_rigid(ModelPreview& dst, const ModelPreview& weapon,
   const std::string& weapon_joint, const std::string& body_joint)` transforms
   weapon vertices into the body and skins them 100% to the body joint.
4. **`assemble.h`** —
   `AssemblyReport assemble_character(const CharacterManifest&, const std::string& dat_path, const std::string& glb_path, const AssemblyOptions&)`
   doing: composite + race → part list (body, head, armor) → models →
   dyed textures → atlas → merged model → weapons/back → `export_model_gltf`.
   The report lists every part used/dropped and why.
5. **Front ends** — CLI `gw2dat_cli character-assemble --manifest m.json --dat <dat> --out <file.glb> [--weapons stowed|hands|none]`;
   Character Ripper button **Export character...** (save dialog).

## Error handling

Missing race/composite → error. A part whose model or texture fails is
dropped and reported; the rest still assembles. A weapon with no matching
attach joint is dropped (reported).

## Testing

- Unit: `region_for` (single rect, two-rect legs anchored at (768,512), skin
  included/excluded); `blit` (2x placement, clipping to rects); `merge_into`
  (bone remap by name, appended joints with parent); `attach_rigid` (a vertex
  at the weapon joint lands on the body joint); composite parser reads
  skin styles / faces / hair styles.
- Live (GW2_TEST_DAT): assemble Musa (SylvariFemale, current manifest) →
  one glb, one skin with >= 153 joints, an atlas image of 1024x1024, body
  chest dropped (Coat worn), face present; Blender render inspected.

## Addendum 2026-10-05: separate mode (the default)

User direction: "just the body mesh separate ... and then put the objects
ONTO it, unless I say I want it combined."

- **Separate (default)** writes a folder: `body.glb` (all four bare-body
  parts -- nothing hidden, armor is removable -- plus face, ears and full
  hair), one `NN_<Slot>_<skin>.glb` per armor piece, one for the back item,
  one per weapon. Every file carries the full race skeleton (same joint names
  and bind pose), the same upright root and units, so each piece sits
  exactly on the body; in Blender, parent the pieces to the body's armature.
  An armor piece's own atlas also holds the bare-body textures, so its skin
  patches show skin. Weapons sit at their holster/hand pose.
- **Combined** (opt-in) is the single file described above.
- Hair layers blend over the face (alpha-over) instead of replacing it.
- **Back items with their own rig** (Mawdrey: its bones hang from the shared
  root, none weighted to body bones) are placed like weapons --
  `actionpoint:CStowBack` onto the body's `actionpoint:CHolsterBack` -- but
  keep their bones (`attach_skinned`), re-parented under the holster so they
  stay animatable. Back items skinned to body bones (capes) still merge by name.
- **The whole weapon kit** (A1, A2, B1, B2, AquaticA, AquaticB) is exported,
  superseding "only weapon set A". Holsters are chosen once for all outputs
  (`choose_holsters`): as many weapons as possible on a holster of their own,
  ties to earlier slots, then main hands right / off-hands left. A weapon left
  with only a shared holster still gets its file in separate mode; combined
  leaves it out (reported) rather than clip two weapons together. With
  `weapons=hands`, set A goes in the hands and the rest are stowed.
- **Dark face / black skin fix:** the glTF writer de-duplicated textures by dat
  fileId, and both baked atlases carried fileId 0, so the normal map slot got
  the diffuse atlas. Generated (fileId 0) textures are never shared now.
