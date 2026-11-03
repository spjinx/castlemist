---
name: gw2-anim-blend-trees
description: "GW2's animation state machine (anic/mach) and blend trees (content type 101, AnimBlendTreeDef): actionData is a 5-way tagged variant, not a dword; blend trees are flat BlendOp node lists referenced by GUID; clip ids are slot templates"
metadata:
  node_type: memory
  type: project
  modified: 2026-11-02
---

# Animation state machine and blend trees

Follows on from [[gw2-animation-banks]], which listed "clip blending and the
state machine that picks a clip" as open. Static reads only: Gw2-64.exe
(PE ts 1792519412) through a scratch Ghidra project imported with
`-noanalysis`, plus the DAT through castlemist's own tools.

## Granny is not the blend tree

GW2 statically links Granny 2.9 but uses only its low-level layer: one
`granny_control` per clip, weights, ease in/out, track masks, mirroring,
`Granny_BlendLocalPoses` (see `Arena\Engine\Model\ModelGrannyUtil.cpp` in
tools/gw2_annotations.json). Granny Animation Studio's graph runtime is not
used. The graph is ArenaNet's own, in `Gw2\Game\AnimationBlendTree\`:
`AbtBlendTree.cpp`, `AbtBlendOps.cpp`, `AbtLayeredAnimController.cpp`.

## Where the data lives

| piece | where | notes |
|---|---|---|
| state machines | `anic` packfile, base id **98781** (the only one), chunk `mach` v1 | 270 machines, 1481 model->machine bindings |
| sequences | same file, chunk `seqn` v0 | 1853 `PackAnimSequenceV0 {qword sequence; animationData[]}` |
| fallbacks | same file, chunk `fall` v0 | `{sourceAnim; targetAnims[]}` |
| IK config | same file, chunk `cnfg` v0 | aim-IK and IK chains |
| blend trees | content (cntc), **type 101** = `AnimBlendTreeDef` | 377 objects; GUID at +0 |

## `actionData` is a tagged variant (the old dump was wrong)

`PackAnimMachineActionV1.actionData` is field kind **0x1C**. The template
generator mapped 0x1C to `dword` ("unconfirmed; 4B like dword",
tools/structs/gen_gw2_json_ghidra.py). In the exe the field entry's
`child` (+16) points at a table of struct descriptors and `count` (+24) is
the number of alternatives (5 here). In the file it is **12 bytes:
`{u32 typeIndex; i64 selfRelPtr}`** (64-bit packfile, pack 4).

Reading it as one dword per element is why `animmach` showed lists like
`[4, 0x00111774]`: that is element 0's type and the low half of its pointer.

Alternatives (`PackAnimMachineAction*V1`, index = typeIndex):

| # | struct | fields |
|---|---|---|
| 0 | PlayAnimation | `sequence` qword, `blendInTime` f32, `blendOutTime` f32, `flags` u32 |
| 1 | PlayBlendTree | `blendTree` byte16 (content GUID of a type-101 object), `blendInTime` f32 |
| 2 | PlayBlendTreeAlias | `blendTreeAlias` qword (token64 name, e.g. `moving`, `land`, `slope`, `airlean`), `blendInTime` f32 |
| 3 | SetAnimationOptions | `blendInTime`, `blendOutTime` f32, `flags` u32 |
| 4 | SetRotation | `rotation` float4 |

Counts over the whole `mach` chunk: 45003 PlayAnimation, 14317 SetRotation,
11581 SetAnimationOptions, 461 PlayBlendTreeAlias, 364 PlayBlendTree
(151 distinct trees). SetRotation sits on exactly the diagonal states
("Air Glide Backward Left", "Air Fall Forward Right"): the client plays the
straight clip and rotates it, so diagonal clips do not exist.

Blend times are real seconds: Air Glide enters with 0.2 s; Land ->
Ground Alert 0.1 in / 0.1 out, with transition *variants* (token 2/7/19/3)
overriding blendOut to 0.4.

The same 0x1C kind appears elsewhere in the template. Any field typed
`dword` that came from 0x1C is suspect.

## Sequence ids are name templates

`PlayAnimation.sequence` is a 12-slot token64 in the bone-name packing
(`detokenizeName64`), but slots can be **0 = filled at runtime**:

```
_gmrun____f    _gmwalk___b    _amgland__fs    _wmswim___f    _gtrunfalt
```

The first slot is the stance (`u`, `w`, `c`, `j`, `q`... seen filled in
other ids: `ugmrun`, `wgmrun`, `uwhstunned`); the tail slots carry a
direction (`f`/`b`/`l`/`r`) and modifiers (`s`). Only 135 of 622 machine
sequence ids are keys of `seqn`, so `seqn` is not the general
sequence-to-clip map. How the client fills the slots, and how `fall`
resolves a missing clip, are still open.

## Blend tree layout (content type 101)

Native struct, pointers are offsets into the owning pack's `content`
(`cgraph --hex-type 101 --len N` dumps all of them with the object's
`offset`). Header, object-relative:

| off | what |
|---|---|
| +0 | GUID (16) |
| +16 | u32 content type (101) |
| +20 | uid |
| +40 | ptr -> node records |
| +48 | u32 node count |
| +56 | u32, usually 200 (ms; 100/133/250 also seen) |
| +64 | ptr (-> a pointer to the root node record, in the one checked) |
| +72 | u32 0..4, likely the layer (`Content::ANIM_BLEND_TREE_LAYERS`, `..._LAYER_TRANSITION`) |
| +76 | u32 0/1 |

Node record, 32 bytes: `{GUID node; u32 opType @+16; ptr data @+24}`.
Children are stored as **pointers to sibling records**; the builder
(`FUN_141183940`) resolves each one to an index (`FUN_141185ee0`), so a
tree is a flat DAG of "BlendOps". 27 op types (0..0x1A); default case
asserts `No valid case for switch variable 'blendOpBlueprint.type'`.

Builder scale constants: `int * 0.001` (ms -> s) and `* 0.01` (percent).

Op types by frequency over all 377 trees, with the child slots and params
the builder reads (data-relative offsets):

| type | count | children | params seen |
|---|---|---|---|
| 0 | 462 | - | leaf: qword @0 (clip/sequence token), ms @0x24 (-1 = none), ms @0x28/0x2c, u32 @8/0x20/0x40, priorityOverride (-1..9) |
| 1 | 436 | - | leaf: like 0 plus `minLoops`/`maxLoops` (@0x10/@0x14, asserts min <= max) |
| 4 | 243 | @0, @8 | none |
| 19 | 226 | | (not yet read) |
| 12 | 189 | @8, @0x10 | param id @0, ms @0x18, value @0x1c, u32 @0x20.. |
| 10 | 129 | @8, @0x10 | param id @0, value @0x18 (*0.01), @0x1c.. |
| 20 | 106 | @0x20 | param id @0, u32 @4 (2 = comparison op?), value @8, ms @0xc, ms @0x10 |
| 16 | 61 | @8, @0x10 | qword @0, ms @0x18, u32 @0x1c |
| 15 | 44 | 2 | **two** param ids @0/@4 (0x12/0x13, 0x39/0x3a, 0x37/0x38), ms @8/@0xc |
| 6 | 39 | array @8 (n @0x10) of `{child, u32, ms, u32}` | weighted random options with cooldowns |
| 21 | 36 | | (not yet read) |
| 5 | 0 | 8 slots @8..@0x40 | 8-way directional; unused in current data |

The small leading integers (0x11, 0x13, 0x2b, 0x37, 0x5d...) look like
**animation parameter ids** (the input that drives a blend or switch). Not
yet mapped to names.

Assert text in `AbtBlendOps.cpp` names op behaviour: `m_minLoops <=
m_maxLoops`, `m_priorityOverride` in [-1, 10), `randSynch`, `aliasData`,
`directionIndex >= 0`, `m_weightedOptions` with per-option `cooldowns`,
foot-crossover windows `m_crossover{Lower,Upper}BoundPercent{,Left,Right}`
in [0, 1]. `AbtBlendTree.cpp` asserts `velocityPos > 0`, `velocityNeg < 0`,
`timeSecPos/Neg`, `easeTimeSec >= 0`, `blendWeight` in [0, 1],
`blueprint->priority` in [0, 10), `replacementPriority`.

Example: tree `T0xy2.Rnqiz` (GUID 8d1b9d8d64862d4ab86253501d22a84f, 23
nodes) is the mount landing tree, played by the "Mount Air Jump/Fall"
transitions of machines 1, 2 and 22.

## Op semantics read so far (vtables)

Each op is a class with a 16-17 slot vtable; slot 4 ("finished?") and the
last slot (activate/choose child) are the ones that differ. Ops reach the
character's **animation parameters** through
`owner->(+0x30)->(+0x168)->(+0x80)`: vfunc `+0x198` reads parameter `id`
(arg 2 = flag bit 1 of the op), `+0xb8`/`+0xb0` give that parameter's
min/max, `+0xc0` tests a **tag token** (qword) and returns bool, `+200` says
whether parameters are available.

| type | what it does | evidence |
|---|---|---|
| 0 | clip request: token, blendIn/out (@+0x30/+0x34 of the op), priority | slot 16 `FUN_14118e620` hands back those fields |
| 1 | clip request with a random loop count in [minLoops, maxLoops], optionally shared through a `randSynch` group; priority override or the tree's | `FUN_14118f890` |
| 4 | runs both children; done when both are | slot 4 `FUN_141189e90` |
| 10 | **latched** switch: on entry, normalise parameter `id` by its min/max (or the op's own range if flag bit 0), pick child B if > threshold else A; held until exit | `FUN_14118cd60` |
| 12 | same test as 10 but re-evaluated continuously (returns the branch each update) | `FUN_14118d1b0` |
| 16 | continuous switch on a tag token | `FUN_14118d090` |
| 17 | latched switch on a tag token | `FUN_14118cf40` |
| 19 | passive gate: checks the owner for something (vfunc +0x38), plays nothing | `FUN_14118b830` |
| 24 | latched switch on a token through a second provider (vfunc +0x58) | `FUN_14118caf0` |

Still to read: 6, 7 (weighted random choice, see the cooldown asserts), 15
(two parameter ids, probably a 2D blend), 20, 21, 2, 3, 5, 8, 9, 11, 13, 14,
18, 22, 23, 25, 26. Note the builder switch's 0x17-0x1A cases map to ctors
`FUN_141188ca0` (0x17), `FUN_141188780` (0x18), `FUN_1411887d0` (0x19),
`FUN_141188830` (0x1A).

## Who uses blend trees: mounts

Over all 270 machines, 774 of the 825 blend-tree actions sit in `Mount *`
states. Everything else is plain clips: 44752 PlayAnimation actions outside
mount states against 51 tree actions (a few `Ground *`/`Air *` states).
So character and NPC locomotion is state machine + clips + SetRotation for
diagonals + speed sync; the blend trees matter mainly for mounts.

## Animation parameters

The provider is the character's **movement manager**:
`AbtLayeredAnimController.cpp` asserts `topmost.agent`, `rootCharacter`,
`moveMgr` along exactly the chain the blend ops use (`FUN_14117ce10`). The
same function is the **speed sync**: a playing clip with a parameter id
(@+0x44) gets playback rate `moveMgr.Get(id) / clip.speed` (clip vfunc
+0x178), optionally clamped (@+0x50/+0x54).

Values live in 0x48-byte records indexed by id (`MvMovementEvaluator.cpp`,
`FUN_1414343b0`): min @+0, max @+4, value @+8, alternate min/max
@+0xc/+0x10 (picked by a flag), multipliers @+0x3c/+0x40. Content movement
packages drive them through curves (types 0..5: linear, logistic, ...).

No name table exists in the exe (no enum strings). Usage, from all 377
trees (type 15 = two ids, likely an x/y pair):

| id | uses | notes |
|---|---|---|
| 0x37 / 0x38 | 76 / 43 | thresholds 0.1, 1.0; also a type-15 pair |
| 0x2a | 75 | 0.1, 50 |
| 0x5d | 51 | always 50 (or 38): a 0-100 scale |
| 0x11 | 51 | 50, 95, 3 |
| 0x12 / 0x13 | 25 / 37 | type-15 pair; 0x13 alone with 0.01 |
| 0x3e | 29 | 1.0, 3.0; also on player `Ground Run Forward` |
| 0x2b | 25 | 0.01, 0.1; on player `Ground Run/Walk Forward` |
| 0x19/0x1a, 0x2d/0x2e, 0x39/0x3a | few | type-15 pairs |

Names are not known; do not label these without evidence.

Content type ids from the exe's `{name, typeId}` table (0x142371d80):
CraftingRecipeDef 12, GuildUpgradeDef 27, ItemDef 35, MapDef 45, SkillDef
64, AnimationListenerDef 100, AnimBlendTreeDef 101, DynamicCameraDef 180,
DynamicCameraTransitionDef 181, ItemConversionArrayDef 237, ItemCrestDef
238, ItemDefaultDef 239, MovementModifierDef 303, MovementSettingsDef 305.

## Fixed in castlemist (2026-11-02)

- Both template generators emit kind 0x1C as
  `{"kind":"variant","alternatives":[...]}`; dumps/packfile/gw2_packfile.json
  regenerated (only PackAnimMachineActionV0/V1 changed, 10 alternative
  structs added).
- `BinaryParser` (generic tree) and `Extractor::fieldSize` know `variant`
  (4 + pointer size); `AnimMachineAction` carries kind, type and the decoded
  fields; `animmach` prints them (sequence names via `detokenizeName64`).
  Checked against an independent Python decode of all 71726 actions:
  identical.
- `cgraph --hex-type T [--len N]` dumps every content object of a type.

## Character locomotion -> Unity animator (2026-11-02)

Three loading bugs stood between the machine and a playable character; all fixed:

- **`fileref` is a pointer.** Template kind 0x1B is a self-relative pointer to
  the compressed fileId pair, like `filename` (Composite reads its
  `meshBase`/`meshOverlap` one pointer apart). The parsers had it as an inline
  4-byte scalar, so `PackAnimModelV1.modelFileId` came out as the raw offset
  (5.4 million) and the struct 4 bytes short (an old special case papered over
  that). Now pointer-sized and decoded. The bindings are real fileIds: the
  playable races' Composite skeleton files are bound directly, e.g.
  SylvariFemale 31210 -> machine 129, HumanFemale 29193 -> 102, HumanMale
  29175 -> 0, CharrMale 28835 -> 25.
- **Imported banks stream their clips.** `ModelAnimationDataV33.animLod`
  (`ModelAnimationLodV25 {data, fileFull}`) names an animation-only file with
  the full clip; imported banks keep no inline `data` at all. castlemist read
  only `data`, so a race got 99 clips instead of ~1700. `resolveAnimImports`
  now loads `fileFull` for such clips (the bank entry keeps its token and
  moveSpeed) and opens up to 256 imports (a race names 72; the cap was 64).
  Sylvari female: 1716 clips, ~10 s.
- **VRChat renaming dropped every limb track.** `make_vrchat_ready` renamed
  joints to humanoid names but not the clip tracks the glTF writer matches by
  name, so clips kept only the root. Tracks are renamed with their joints now.

Clip names follow the template slots with spaces for empty ones and a two-digit
variant suffix (`ugmrun    f `, `hamfall   f08`). The unarmed stance `u` covers
35 of the 41 locomotion templates of machine 129, the most of any letter.

`gw2dat_cli character-vrchat ... --animator [--stance u]`
(src/ripper/animator.cpp) resolves the race's machine for one stance (stance,
then `u`, then any; variant 0 first), keeps the ground/air locomotion states,
carries only their clips (31 for Sylvari female) renamed clean, and writes
`<name>.animator.json`. Unity: Castlemist > Character > Build GW2 Animator
(tools/unity/Castlemist/Editor/GW2Animator.cs) builds a VRChat-parameter
controller from it: a 2D freeform directional tree on VelocityX/Z with each
walk/run/sprint clip at its authored speed (moveSpeed x 0.0254: walk 2.0, run
5.3, sprint 7.5 m/s) and the diagonals as baked copies turned by GW2's yaw;
turn-in-place, jump -> fall, and GW2's landing clips as states; GW2's crossfade
times on every transition. Positive GW2 yaw turns the body left ("Walk Right" =
walk forward at 270 deg; "Walk Left" = walk *backward* at 270 deg). Verified in
the GW2Porting editor: a real Animator with root motion off reaches every state
and the turned clips face -30 / +90 / +45 deg as planned.

**Stances, start/stop, water (same day, later).** `--stance u,w,g` plans
several stances into one export (version-2 plan: one entry per stance, plus each
stance's own `_gmdraw` / `_gmstow` clips). Stance letters, from the clips:
`u` unarmed, `w` one-handed main hand (`wgaaxetoss`: axe is here, with sword,
mace...), `g` greatsword (`ggaairscythe`, `ggaspin`). A weapon stance idles in
"Ground Alert" (`ggmalert`, weapon ready); its relaxed "Ground Idle" falls back
to unarmed. The kept states now include Ground Alert and the water states
(surface swim = one breaststroke clip turned in 8 directions; underwater =
real f/b/l/r swim clips). Sylvari female, u+w+g: 117 clips.

The Unity side moved to its own package, tools/unity/CastlemistAvatar (no
dependency on the world importer; the VRChat parts compile only with the
Avatars SDK). Base controller: a sub-state machine per stance on the Int
GW2Stance (draw on entry, idle/alert, fidget, directional start clips, four
move states over one tree so each stop plays its own stop clip, turns,
directional jump/fall trees, landings), water at the top on GW2Swim / GW2Dive.
Expression parameters: 10 bits. Checked in AV2024 on a real Animator: starts,
stops, stance switches with draw, jump/fall/land, swim/dive and fidget all
route as planned, and each stance renders its own poses.

**Moves (evades, slides, combat, gliding).** The plan now keeps every state
but mounts (`is_character_state`; Sylvari female u+w+g: 179 clips). Findings:
every stance dodges with the one-handed evade set (`wgmevade_f/b/l/r`; GW2's
standing dodge is the backward one); knockdowns, downed and deaths are
once-through clips that hold their last pose, with get-up clips
(`ughgetupflb`, `ughgetup_f`, `udtalive`) the client plays on its own (no
machine transition); gliding is entered from a fall through `uattumgfal`;
limping has its own directional set. VRChat drives them through one synced Int
`GW2Move` from menu buttons (one-shots) and toggles (held moves), table
`GW2Animator.Moves`. Moves are entered by explicit transitions from each
stance's ordinary states (kept first in their transition lists), not Any
State: an Any State entry re-fires from a move's own follow-on state (downed
-> downed loop, glide open -> glide) and restarts it. Water is reached through
the stance machines' exits for the same reason.

Two Unity traps on the way: an explicit `ModelImporter.clipAnimations` list
imports only the takes on it, so a re-export with more clips loses the new ones
unless they are merged in; and an avatar holds its controller / menu /
parameter assets by GUID, so the builder empties and refills them in place
rather than re-creating them (the parameter list and root menu also keep
entries that are not GW2's).

**Emotes (Action layer).** The `emoc` file (one, base id 816197; chunk `anim`,
PackEmoteAnimationsV3) lists 314 emote clip tokens, each with per-model timings
{ModelFile, BlendIn, BlendOut, Intro/Loop/OutroDuration ms, StartOffset}; a
race's skeleton file is the ModelFile. An emote's parts are separate clips: the
token's variant nibble 4 = intro, 8 = loop, 12 = outro (each part's length is
its clip's length; `ugesit04/08/12`). A loop duration of 0 = a one-shot. The
player emotes are picked by a name table (`emote_label`); 39 for Sylvari
female. Unity: GW2Actions.cs builds the Action layer (VRChat playable-layer and
tracking hand-off), looping emotes as menu toggles, one-shots as buttons.

**Slide.** GW2's slide is authored on a ~27 deg downhill (feet at different
heights); on flat ground it hangs in the air. The builder levels it (rotates
the foot spread flat) and lowers the root until the lower ankle is at standing
height.

**Face on an edited avatar.** A humanoid take drops non-humanoid bones (the face)
unless the clip mask lists every transform; the importer now does. GW2 animates
the face through lips, cheeks, brows and lids (`JawBone` has no curves of its
own; its motion arrives as the humanoid jaw). GW2Face.cs retargets that onto an
avatar's own face rig (name map, motion as change from rest in a face frame
both rigs share, scaled by face width) into per-avatar face clips and a synced,
masked layer per avatar in both controllers.

PlayAnimation flag 0x10 sits on every one-shot clip (jumps, landings,
transition clips, fidgets) and on no looping one; the Unity side treats it as
"play once". Inferred from the data, not read from the client.

## Open

- Semantics of the op types not listed above (the constructors
  are `FUN_141189520` (0), `FUN_141189410` (1), `FUN_141188740` (4),
  `FUN_1411888b0` (5), `FUN_141189610` (6), `FUN_141188b80` (10),
  `FUN_141188d80` (12), `FUN_141188e10` (15), `FUN_141188c40` (16),
  `FUN_141189720` (19), `FUN_141188940` (20), `FUN_141189200` (21)).
  Vtables: 0 `0x142156b68`, 1 `0x142156ad8`, 2 `0x142156d18`,
  3 `0x142156c88`, 4 `0x142156e28`, 5 `0x142156ea8`, 6 `0x142156fb8`,
  7 `0x142157040`.
- Parameter id -> meaning. Next lead: the writers of record+8 (who sets
  each id), or the type-303 movement packages that drive them. Also the tag
  tokens of types 16/17/24.
- Template slot filling and `fall` resolution.
- What decides which state is active (the machine's transitions are named
  events like "Land", "Land Hard"; who fires them).
