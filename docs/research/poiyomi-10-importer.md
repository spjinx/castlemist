---
name: poiyomi-10-importer
description: "Facts for castlemist's Unity importer: Poiyomi Toon 10 shader name, VPM package, exact property names (preset, packed PBR map, emission 0-3, UV/pan), keywords and locking via ThryEditor, and the Unity editor APIs to build materials, remap the FBX and save a prefab"
---

# Poiyomi Toon 10 importer: research

Research for sub-project 3 of
`docs/superpowers/specs/2026-10-07-vrchat-model-export-design.md` (the Unity
importer that turns a castlemist VRChat export folder into Poiyomi materials
and a prefab). Done 2026-10-07, read-only, from:

- the Poiyomi Toon source at `github.com/poiyomi/PoiyomiToonShader`, branch
  `master`, which is release **v10.0.24** (2026-10-02). The task brief said
  10.0.20; that was the release of 2026-09-15 and has since been followed by
  10.0.21 to 10.0.24. Property names were read from
  `_PoiyomiShaders/Shaders/10.0/Toon/Poiyomi Toon.shader` at that commit
  (line numbers below refer to that file).
  <https://github.com/poiyomi/PoiyomiToonShader/blob/master/_PoiyomiShaders/Shaders/10.0/Toon/Poiyomi%20Toon.shader>
- ThryEditor (Poiyomi's inspector and the shader optimiser / locker) at
  `github.com/poiyomi/ThryEditor`, branch `master`, package version 2.74.3.
- poiyomi.com docs, Unity 2022.3 docs and VRChat creator docs, linked per fact.

Anything not confirmed is marked **UNCONFIRMED**.

## 1. Shader, package, detection

| Fact | Value | Source |
|---|---|---|
| Main shader name (`Shader.Find`) | `.poiyomi/Poiyomi Toon` | first line of `Poiyomi Toon.shader` (link above) |
| Other 10.0 variants | `.poiyomi/Poiyomi Toon Two Pass`, `.poiyomi/Poiyomi Toon World`, `.poiyomi/Poiyomi Toon Outline Early`, `.poiyomi/Poiyomi Toon Grab Pass`, plus `Lil Fur`, `Lil Fur Two Pass`, `URP*` files | first line of each file in `_PoiyomiShaders/Shaders/10.0/Toon/` |
| 9.3 shaders still shipped | yes, `_PoiyomiShaders/Shaders/9.3/Toon/*` (same package) | repo tree, <https://github.com/poiyomi/PoiyomiToonShader/tree/master/_PoiyomiShaders/Shaders> |
| VPM repo URL | `https://poiyomi.github.io/vpm/index.json` | <https://www.poiyomi.com/download>, <https://www.poiyomi.com/blog/2023-07-27-poiyomi-toon-vcc> |
| Package id | `com.poiyomi.toon` (latest 10.0.24, `"unity": "2021.3"`) | <https://raw.githubusercontent.com/poiyomi/PoiyomiToonShader/master/package.json>, <https://poiyomi.github.io/vpm/index.json> |
| Dependency | `com.poiyomi.thryeditor` `>=2.74.3` | same `package.json` (`vpmDependencies`) |
| Conflicts | `legacyPackages: ["com.poiyomi.pro"]`; `legacyFolders: Assets\_PoiyomiShaders` (a VPM install removes an old .unitypackage copy) | same `package.json` |
| Other install route | `.unitypackage` from GitHub/BOOTH/Discord, imported under `Assets/_PoiyomiShaders` | <https://www.poiyomi.com/download> |
| Version string inside the shader | hidden property `shader_master_label`, display name `<color=#E75898ff>Poiyomi 10.0.24</color>` | `Poiyomi Toon.shader` line 5 |

Detection from an Editor script (recommended, both routes covered):

1. `Shader s = Shader.Find(".poiyomi/Poiyomi Toon");` null = not installed.
   `Shader.Find` works in the Editor for any shader in the project
   (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/Shader.Find.html>).
2. VPM install: `UnityEditor.PackageManager.PackageInfo.FindForAssetPath(AssetDatabase.GetAssetPath(s))`
   returns the package (`name == "com.poiyomi.toon"`, `version == "10.0.24"`),
   or null when the shader is not in a package, i.e. the .unitypackage install
   (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/PackageManager.PackageInfo.FindForAssetPath.html>).
3. Either route: parse the version out of the description of
   `shader_master_label` (`s.FindPropertyIndex("shader_master_label")`, then
   `s.GetPropertyDescription(i)`, regex `Poiyomi (\d+\.\d+\.\d+)`). This is a
   Poiyomi convention, not a documented API.

Compile-time detection: an importer asmdef can use **Version Defines** on
resource `com.poiyomi.toon` / `com.poiyomi.thryeditor` to define a symbol only
when the package is present
(<https://docs.unity3d.com/2022.3/Documentation/Manual/ScriptCompilationAssemblyDefinitionFiles.html>).
That only works for the VPM route; a .unitypackage install is not a package.
ThryEditor's asmdef is named `ThryAssemblyDefinition`, Editor-only,
`autoReferenced: true`
(<https://github.com/poiyomi/ThryEditor/blob/master/Editor/ThryAssemblyDefinition.asmdef>).

Install location of VPM packages (`Packages/com.poiyomi.toon/...`) is the
usual VCC layout but **UNCONFIRMED** by the VCC docs pages fetched
(<https://vcc.docs.vrchat.com/vpm/>, <https://vcc.docs.vrchat.com/vpm/packages/>);
the importer should not hard-code paths and use the steps above.

## 2. Property names

All from `Poiyomi Toon.shader` v10.0.24. Many are declared `Int`; in ShaderLab
`Int` is a legacy type backed by a float
(<https://docs.unity3d.com/2022.3/Documentation/Manual/SL-Properties.html>),
so set every one of them with `Material.SetFloat`.

### Base colour, alpha, normal

| What | Property | Notes (line) |
|---|---|---|
| Main texture | `_MainTex` (2D, sRGB) | 63 |
| Main colour/tint | `_Color` | 61 |
| Main UV | `_MainTexUV` | 64 |
| Main panning | `_MainTexPan` (Vector2) | 65 |
| Ignore texture alpha | `_MainIgnoreTexAlpha` | 68 |
| Alpha cutoff | `_Cutoff`, Range(0, 1.001), default 0.5 | 91 |
| Force opaque | `_AlphaForceOpaque` (default 1) | 219 |
| Alpha to coverage | `_AlphaToCoverage` | 226 |
| Premultiply | `_AlphaPremultiply` | 287 |
| Normal map | `_BumpMap` (2D, `[Normal]`) | 70 |
| Normal strength | `_BumpScale`, Range(0, 10), default 1 | 71 |
| Normal UV / pan | `_BumpMapUV`, `_BumpMapPan` | 72-73 |

### Rendering preset and render state

Preset: `_Mode` (`ThryWideEnum`): **Opaque 0, Cutout 1, TransClipping 9,
Fade 2, Transparent 3, Additive 4, Soft Additive 5, Multiplicative 6,
2x Multiplicative 7** (line 47). Raw state: `_SrcBlend`, `_DstBlend`,
`_BlendOp`, `_SrcBlendAlpha`, `_DstBlendAlpha`, `_BlendOpAlpha`, `_ZWrite`,
`_ZTest`, `_Cull` (`UnityEngine.Rendering.CullMode`: 0 Off, 1 Front, 2 Back,
default 2) (lines 5654-5690). ForwardAdd pass: `_AddSrcBlend`, `_AddDstBlend`,
`_AddSrcBlendAlpha`, `_AddDstBlendAlpha`. Outline pass: `_OutlineSrcBlend`,
`_OutlineDstBlend`, `_OutlineSrcBlendAlpha`, `_OutlineDstBlendAlpha`,
`_OutlineBlendOp`, `_OutlineBlendOpAlpha`. SubShader default tags:
`"RenderType"="Opaque" "Queue"="Geometry" "VRCFallback"="Standard"`.

**Setting `_Mode` from a script does nothing else.** The preset is a list of
`on_value_actions` in the property's display string, run by ThryEditor's
inspector: `DefineableAction.Perform` → `MaterialHelper.SetValueAdvanced`,
which reads `ShaderEditor.Active` (the open inspector) and writes
`material.renderQueue` for `render_queue` and the `RenderType` override tag for
`render_type`
(<https://github.com/poiyomi/ThryEditor/blob/master/Editor/DataStructs/DefineableAction.cs>,
<https://github.com/poiyomi/ThryEditor/blob/master/Editor/Helpers/MaterialHelper.cs>).
There is no public "apply preset to a Material" call; the importer must write
the same values itself. The table, read from the action lists (lines 48-56):

| Preset (`_Mode`) | queue | RenderType | Src | Dst | ZWrite | `_Cutoff` | `_AlphaForceOpaque` | `_AlphaPremultiply` | AddSrc/AddDst |
|---|---|---|---|---|---|---|---|---|---|
| Opaque 0 | 2000 | Opaque | 1 | 0 | 1 | 0 | 1 | 0 | 1/1 |
| Cutout 1 | 2450 | TransparentCutout | 1 | 0 | 1 | .5 | 0 | 0 | 1/1 |
| TransClipping 9 | 2460 | TransparentCutout | 5 | 10 | 1 | 0.01 | 0 | 0 | 5/1 |
| Fade 2 | 3000 | Transparent | 5 | 10 | 0 | 0.002 | 0 | 0 | 5/1 |
| Transparent 3 | 3000 | Transparent | 1 | 10 | 0 | 0 | 0 | **1** | 1/1 |
| Additive 4 | 3000 | Transparent | 1 | 1 | 0 | 0 | 0 | 0 | 1/1 |
| Soft Additive 5 | 3000 | Transparent | 4 | 1 | 0 | 0 | 0 | 0 | 4/1 |
| Multiplicative 6 | 3000 | Transparent | 2 | 0 | 0 | 0 | 0 | 0 | 2/1 |
| 2x Multiplicative 7 | 3000 | Transparent | 2 | 3 | 0 | 0 | 0 | 0 | 2/1 |

Common to all: `_BlendOp=0`, `_BlendOpAlpha=4`, `_SrcBlendAlpha=1`,
`_DstBlendAlpha=1`, `_AddSrcBlendAlpha=0`, `_AddDstBlendAlpha=1`,
`_AlphaToCoverage=0`, `_ZTest=4`; outline factors mirror Src/Dst. Factors are
`UnityEngine.Rendering.BlendMode` (0 Zero, 1 One, 2 DstColor, 3 SrcColor,
4 OneMinusDstColor, 5 SrcAlpha, 6 OneMinusSrcColor, 7 DstAlpha,
8 OneMinusDstAlpha, 9 SrcAlphaSaturate, 10 OneMinusSrcAlpha). Note the
preset writes `_Cutoff`, so castlemist's `alphaCutoff` must be applied
**after** the preset. Queue/ZWrite values agree with the docs
(<https://www.poiyomi.com/general/render-preset>), which say presets
"automatically configure your Rendering settings". The importer should copy
the full action string at runtime instead of hard-coding it: parse the
`_Mode` property description (`shader.GetPropertyDescription`) for
`{value:N,actions:[...]}` so a Poiyomi update that changes a preset is picked
up. Per-material `renderQueueOffset` then goes on top of `material.renderQueue`.

Custom blend (castlemist `Custom`): set `_Mode` to the nearest preset, then
override `_SrcBlend`/`_DstBlend`/`_BlendOp` directly; Poiyomi exposes them as
plain properties (line 5687-5688).

### Reflections & Specular (packed map)

Module toggle: `_MochieBRDF` with `[ThryToggle(MOCHIE_PBR)]` → keyword
**`MOCHIE_PBR`** (`#pragma shader_feature_local_fragment MOCHIE_PBR`)
(lines 2170, 13316).

| What | Property | Default |
|---|---|---|
| Packed map | `_MochieMetallicMaps` (`ThryRGBAPacker(R Metallic Map, G Smoothness Map, B Reflection Mask, A Specular Mask, linear, ...)`) | white |
| Its UV / pan | `_MochieMetallicMapsUV`, `_MochieMetallicMapsPan` | 0 |
| Channel selectors | `_MochieMetallicMapsMetallicChannel` (0=R), `_MochieMetallicMapsRoughnessChannel` (1=G), `_MochieMetallicMapsReflectionMaskChannel` (2=B), `_MochieMetallicMapsSpecularMaskChannel` (3=A); enum R0 G1 B2 A3 White4 | R,G,B,A |
| Inverts | `_MochieMetallicMapInvert`, `_MochieRoughnessMapInvert`, `_MochieReflectionMaskInvert`, `_MochieSpecularMaskInvert` | 0 |
| Metallic multiplier | `_MochieMetallicMultiplier` | **0** |
| Smoothness multiplier | `_MochieRoughnessMultiplier` (despite the name it is smoothness; label "Smoothness") | 1 |
| Reflection tint | `_MochieReflectionTint` | white |
| Specular tint | `_MochieSpecularTint` | white |
| Reflection / specular visibility | `_MochieReflectionStrength` (0-1), `_MochieSpecularStrength` (0-5) | 1, 1 |
| Fallback cubemap | `_MochieReflCube`, `_MochieForceFallback` | none |

Lines 2169-2200. Castlemist's packed layout (R metal, G smooth, B reflection
mask, A specular mask) is exactly Poiyomi's default channel assignment, so no
channel selector needs changing. **Pitfall:** the shader computes
`metallic = _MochieMetallicMultiplier * map[R]` (line ~32261), and the
multiplier defaults to 0, so the importer must set
`_MochieMetallicMultiplier = 1` or the metal channel is ignored. The docs
confirm both sliders multiply the map
(<https://www.poiyomi.com/shading/reflections-and-specular>) and that the
packed map must be sRGB **Off**.

### Emission, slots 0-3

Four slots (<https://www.poiyomi.com/special-fx/emission>). Slot 0 has no
suffix; slots 1-3 append the digit, but not always in the same place:

| What | Slot 0 | Slot n (1-3) |
|---|---|---|
| Enable | `_EnableEmission` → keyword **`_EMISSION`** | `_EnableEmission{n}` → **`POI_EMISSION_{n}`** |
| Emission map (colour, sRGB on) | `_EmissionMap` | `_EmissionMap{n}` |
| Map UV / pan | `_EmissionMapUV`, `_EmissionMapPan` | `_EmissionMap{n}UV`, `_EmissionMap{n}Pan` |
| Emission mask (data, sRGB off) | `_EmissionMask` | `_EmissionMask{n}` |
| Mask UV / pan | `_EmissionMaskUV`, `_EmissionMaskPan` | `_EmissionMask{n}UV`, `_EmissionMask{n}Pan` |
| Mask channel (R0 G1 B2 A3) | `_EmissionMaskChannel` | `_EmissionMask{n}Channel` |
| Mask invert | `_EmissionMaskInvert` | `_EmissionMaskInvert{n}` |
| Colour (HDR) | `_EmissionColor` | `_EmissionColor{n}` |
| Strength, Range(0,20) | `_EmissionStrength` | `_EmissionStrength{n}` |
| Use base colour as map | `_EmissionBaseColorAsMap` | `_EmissionBaseColorAsMap{n}` |

Lines 3082-3100 (slot 0), 3189-3205, 3294-3310, 3399-3415. `[ThryToggle(...)]`
declarations: `_EMISSION` uses `#pragma shader_feature`, the others
`shader_feature_local` (lines 13279-13291). **Pitfall:** `_EmissionStrength*`
defaults to **0**; enabling the slot alone shows nothing. Emission map default
is white and mask default white, so an unset map means "colour everywhere"
(docs: "If no map is defined, this color will be used entirely").

### UV channels and panning

Every texture's UV selector is a `ThryWideEnum(UV0 0, UV1 1, UV2 2, UV3 3,
Panosphere 4, World Pos 5, Local Pos 8, Polar UV 6, Distorted UV 7, Matcap 9,
Screen Space 10)` (e.g. line 64). The vertex input reads only `TEXCOORD0..3`
(`uv0..uv3`, line ~10927), so **mesh UV channels UV0-UV3 only**; the docs say
"all of the first 4 UV maps (UV0, UV1, UV2, UV3) can be directly used for
almost any texture" (<https://www.poiyomi.com/general/textures-and-colors>).
castlemist's warning for UV sets past 3 is correct.

Panning: `POI_PAN_UV(uv, pan) = uv + POI_TIME.x * pan` (line 6016) with
`POI_TIME = _Time` unless VRChat network time is selected (line 7037), and
Unity's `_Time.x` is t/20 (<https://docs.unity3d.com/2022.3/Documentation/Manual/SL-UnityShaderVariables.html>).
So a GW2 scroll of `v` UV/second is `*Pan = 20 * v`. **UNCONFIRMED** what unit
GW2's `intscru`/`intscrv` are in; the importer should document the conversion
it uses.

## 3. Keywords and locking

- **What locking is.** ThryEditor's Shader Optimizer writes a per-material
  copy of the shader (`Hidden/Locked/...`) with unused features removed and
  non-animated properties baked as constants
  (<https://www.poiyomi.com/general/locking>,
  `ShaderOptimizer.cs` line ~3339).
- **Must it be locked for upload?** Effectively yes, but it happens on its
  own: ThryEditor implements `IVRCSDKPreprocessAvatarCallback`
  (`LockMaterialsOnUpload`) and `IVRCSDKBuildRequestedCallback`
  (`LockMaterialsOnWorldUpload`), locking every material on the avatar/world
  at build time (`ShaderOptimizer.cs` lines 3213-3280,
  <https://github.com/poiyomi/ThryEditor/blob/master/Editor/ShaderOptimizer.cs>;
  docs: "the Auto-Locking process runs automatically for any Poiyomi Materials
  found in an Avatar or World whenever a Build/Upload is triggered").
  Any unlocked Poiyomi shader that still reaches a build is stripped by
  `StripUnlockedShadersFromBuild` (dialog: "This will cause pink materials").
  Unlocked shaders also carry `_ForgotToLockMaterial` (line 41) so the VRChat
  SDK warns.
- **Can a script lock?** Yes, public API (`[PublicAPI]`), namespace
  `Thry.ThryEditor`:
  `ShaderOptimizer.LockMaterials(IEnumerable<Material>, ShaderOptimizer.ProgressBar = None)`
  and `ShaderOptimizer.UnlockMaterials(...)`; `ShaderOptimizer.IsMaterialLocked(Material)`.
  The older `SetLockedForAllMaterials` is `[Obsolete]`.
  Lock state is the property `_ShaderOptimizerEnabled` (line 42) but must be
  changed only through the API.
- **Setting properties on an unlocked material** is normal and is how the
  importer should work: all features are live in the unlocked shader
  (`#if defined(PROP_X) || !defined(OPTIMIZER_ENABLED)` guards, e.g. line 9074).
  The importer should **not** lock: locking compiles a shader per material
  (slow) and the upload hook does it anyway. On a locked material, setting
  non-animated properties has no effect (they are constants in the generated
  shader; the docs say such properties are greyed out).
- **Keywords matter for the lock.** When locking, every keyword in
  `material.shaderKeywords` becomes a `#define`, and `PROP_<NAME>` is defined
  for each assigned texture (`ShaderOptimizer.cs` ~1449-1471). `ifex` blocks
  are removed by property value (e.g. `//ifex _EnableEmission==0`, line 3081).
  A `SetFloat("_MochieBRDF", 1)` without `EnableKeyword("MOCHIE_PBR")` leaves a
  mismatched material. Two ways to fix:
  1. call `EnableKeyword` yourself for each toggle you set (table below), or
  2. call `Thry.ShaderEditor.FixKeywords(IEnumerable<Material>)` (public
     static, `Editor/ThryEditor.cs` line 1519), which reads every
     `ThryToggle` on the shader and sets keywords from property values
     (float `== 1` → on; for texture props, texture present → on).
  ThryEditor also runs `FixKeywords` before locking when
  `Config.fixKeywordsWhenLocking` is true, which is the default
  (`Editor/Config.cs` line 64), so the upload path self-heals. Doing (1)
  keeps the material correct even in the unlocked preview and does not need
  a compile-time reference to Thry.

## 4. Feature toggles → properties/keywords

The inspector's section headers (`m_start_*`) only reference a toggle
property; the toggle itself is a `[ThryToggle(KEYWORD)]` float. For the
importer:

| Module | Property = 1 | Keyword |
|---|---|---|
| Reflections & Specular | `_MochieBRDF` | `MOCHIE_PBR` |
| Emission 0 | `_EnableEmission` | `_EMISSION` |
| Emission 1/2/3 | `_EnableEmission1/2/3` | `POI_EMISSION_1/2/3` |

`[ToggleUI]` properties (e.g. `_EmissionBaseColorAsMap`, `_AlphaForceOpaque`)
have no keyword; just set the float. Other modules follow the same pattern;
look up `ThryToggle(` beside the property in the shader before using one.
"Animated (when locked)" is a per-material override tag
`<property>Animated` (`ShaderOptimizer.SetAnimatedTag`, `IsAnimated`, lines
685-706); not needed unless the importer emits material animations.

## 5. Unity side (VRChat: Unity 2022.3.22f1)

VRChat's current Unity version is **2022.3.22f1**
(<https://creators.vrchat.com/sdk/upgrade/current-unity-version/>). All APIs
below exist in 2022.3.

### Trigger

- `AssetPostprocessor.OnPostprocessAllAssets(string[] imported, string[]
  deleted, string[] moved, string[] movedFrom, bool didDomainReload)` runs
  "after importing of any number of assets is complete"; "you can safely
  perform any asset database operations" inside it, but any imports it
  triggers call it again, so guard against recursion; must be `static`
  (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetPostprocessor.OnPostprocessAllAssets.html>).
  When a whole folder is dropped in, the `.fbx`, PNGs and `materials.json`
  arrive in one batch; key off `materials.json` in `importedAssets`.
- `OnPreprocessTexture()` runs "just before the texture importer is run" and
  is the place to set import defaults
  (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetPostprocessor.OnPreprocessTexture.html>).
  Because castlemist's file names carry the role (`<Mat> - Normal.png`,
  `- Packed.png`, `- EmissionMask.png`, ...), the importer can set
  texture type and sRGB here, before the first import, when a sibling
  `../materials.json` exists, instead of reimporting afterwards.
  `OnPreprocessModel()` likewise for the FBX.
- A `ScriptedImporter` for `.json` is a poor fit: `.json` is a native Unity
  extension (TextAsset); `ScriptedImporterAttribute.overrideFileExtensions`
  lists extensions the importer "can handle in addition to the default", i.e.
  the user would have to pick it per file in the Inspector
  (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetImporters.ScriptedImporterAttribute.html>).
  A ScriptedImporter also cannot write separate `.mat`/`.prefab` assets
  (it adds sub-objects with `AddObjectToAsset`/`SetMainObject`,
  <https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetImporters.ScriptedImporter.html>).
  A dedicated extension (e.g. `.castlemist`) would make a ScriptedImporter
  possible, but materials would then be read-only sub-assets, which users
  can't tweak. The AssetPostprocessor route is the better match.

### Textures

`TextureImporter` (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/TextureImporter.html>):
`textureType = TextureImporterType.NormalMap` for `Normal.png`;
`sRGBTexture = false` for Packed, EmissionMask, Distortion and other data maps
(Poiyomi docs: "It's very important to set the sRGB setting to Off for
textures that are not being used directly as color",
<https://www.poiyomi.com/general/textures-and-colors>); BaseColor and
EmissionMap keep sRGB on; `alphaIsTransparency` for cutout/blended base
colours. Outside a preprocessor, change then `SaveAndReimport()`.

### Materials

`new Material(Shader.Find(".poiyomi/Poiyomi Toon"))`, set properties,
then `AssetDatabase.CreateAsset(mat, "<folder>/Materials/<Mat>.mat")`. On a
re-import, load the existing `.mat` and update it in place (keeps the GUID
the FBX remap and prefab point at, keeps user edits if desired).
`AssetDatabase.StartAssetEditing()/StopAssetEditing()` batch the writes and
need try/finally ("if an exception occurs between the two function calls, the
AssetDatabase will be unresponsive",
<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetDatabase.StartAssetEditing.html>).

### FBX material remap

- `ModelImporter.materialLocation`: `External` (extract) or `InPrefab`
  (sub-assets) (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ModelImporterMaterialLocation.html>).
- Exact remap per material: `importer.AddRemap(new
  AssetImporter.SourceAssetIdentifier(typeof(Material), "<fbx material
  name>"), mat)` then `AssetDatabase.WriteImportSettingsIfDirty(path)` +
  `AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate)`; a type
  mismatch or null silently keeps the internal material
  (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/AssetImporter.AddRemap.html>).
- Name-based alternative: `SearchAndRemapMaterials(ModelImporterMaterialName,
  ModelImporterMaterialSearch)` "search[es] the project for matching materials
  and use them instead of the internal materials"
  (<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/ModelImporter.SearchAndRemapMaterials.html>).
  `AddRemap` is preferred: castlemist controls the names, and a project-wide
  search can grab a same-named material from another export.

### Prefab

`PrefabUtility.SaveAsPrefabAsset(GameObject, path[, out bool])`: input must
not be a child inside a prefab instance; **if the input is a prefab instance
root the result is a Prefab Variant**; returns **null inside an asset batch
editing operation**, so call it after `StopAssetEditing()`; overwriting an
existing prefab matches objects by name
(<https://docs.unity3d.com/2022.3/Documentation/ScriptReference/PrefabUtility.SaveAsPrefabAsset.html>).
Choice: `PrefabUtility.InstantiatePrefab(fbxModel)` → variant of the FBX
(follows FBX reimports) or `Object.Instantiate(fbxModel)` → standalone prefab.
Destroy the temporary instance afterwards.

## Recommended architecture

```
Assets/Castlemist/Editor/  (asmdef: Editor only; optional Version Define
                            POIYOMI_TOON on com.poiyomi.toon)
  CastlemistPostprocessor : AssetPostprocessor
    OnPreprocessTexture  -> role from "<Mat> - <Role>.png" when ../materials.json
                            exists: NormalMap / sRGB off / alphaIsTransparency
    OnPostprocessAllAssets -> for each imported */materials.json:
                              EditorApplication.delayCall += Build(folder)
                              (guard: skip if the build is running)
  CastlemistBuilder.Build(folder)
    1. parse materials.json (JsonUtility or bundled parser; check "poiyomi":"10")
    2. Shader.Find(".poiyomi/Poiyomi Toon"); missing -> one dialog, stop
    3. Start/StopAssetEditing: create/update Materials/<Mat>.mat
         preset table (parsed from _Mode's actions) -> queue, RenderType tag,
         blend/zwrite; then _Cutoff, _Cull, renderQueue += offset
         _MainTex/_Color, _BumpMap/_BumpScale, UV selectors
         packed -> _MochieMetallicMaps, _MochieBRDF=1 + MOCHIE_PBR,
                   _MochieMetallicMultiplier=1, tints
         emission -> slot 0 (map + mask, own UVs, _EmissionStrength>0, _EMISSION),
                     extra slots 1-3 when needed (POI_EMISSION_n)
    4. ModelImporter.AddRemap per material -> WriteImportSettingsIfDirty -> ImportAsset
    5. instantiate FBX, SaveAsPrefabAsset("<Name>.prefab"), destroy instance
    6. log materials.json "warnings" + importer's own; never lock
```

Calling into ThryEditor is optional: setting keywords by hand avoids any
compile-time dependency, and upload-time locking runs `FixKeywords` anyway.
If wanted, reflection on `Thry.ShaderEditor.FixKeywords` keeps the script
compiling when Poiyomi is missing.

## Open questions

1. GW2 `intscru`/`intscrv` unit (UV per second?), which sets the ×20
   conversion to Poiyomi `*Pan`.
2. VPM install folder (`Packages/com.poiyomi.toon`) not stated in VCC docs;
   detection doesn't depend on it.
3. Whether to write the preset table from the parsed `_Mode` description
   (robust to Poiyomi changes, depends on Thry's string format) or hard-code it.
4. Prefab as a variant of the FBX vs standalone.
5. Whether Poiyomi Pro (`com.poiyomi.pro`, 11.x in the same VPM repo) should
   be accepted; its shader name and property compatibility are **UNCONFIRMED**.
6. Re-import policy: overwrite user edits on existing `.mat`s or only fill
   new ones.
7. Distortion map: no Poiyomi target was researched here (Poiyomi has
   "Distorted UV" UV mode and a distortion flow section; mapping
   `glowperturb` to it needs a separate look).
