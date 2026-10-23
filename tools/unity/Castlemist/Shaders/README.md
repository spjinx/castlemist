# castlemist GW2 shaders (VRChat, built-in pipeline)

Used by the importer's Shader option (Castlemist GW2); `Castlemist/GW2` also draws the GW2
parallax effects in the Poiyomi mode. Both compile in Unity 2022.3.22 with VRC Light Volumes
3.0.1 (checked in batchmode, renders compared).

**One shader, no keywords** (project choice 2026-10-22): every GW2 feature is a block of
`Castlemist/GW2`, switched by a material float (uniform branching), so a world needs no keyword
budget and one variant per Unity pass setup. The only keywords are Unity's own (fog, lights,
shadows, lightmaps, stereo). A per-family bake that constant-folds the floats (as Poiyomi's
ThryEditor optimizer does per material) can come later if profiling, e.g. on Quest, needs it.
All GW2 maps share two inline sampler states (`sampler_trilinear_repeat`, `sampler_linear_clamp`),
since D3D11 allows only 16 samplers per pixel shader.

| File | What |
|---|---|
| `GW2.shader` | `Castlemist/GW2`: every GW2 world material but particles. ForwardBase, ForwardAdd, ShadowCaster, Meta |
| `GW2Core.cginc` | its passes and blocks (the block list and the GW2 formulas are in its header) |
| `GW2 Particle.shader` | `Castlemist/GW2 Particle`: every GW2 particle system, unlit; the families of GW2's particle pixel shaders (`_PtcShader`), plus a second pass for 1601524's add layer |
| `GW2Particle.cginc` | its passes, built on `GW2Core.cginc` (`CM_PASS_PARTICLE`; the family formulas are in its header) |
| `GW2 Distort.shader` | `Castlemist/GW2 Distort`: GW2's screen distortion (a GrabPass, which a material float cannot switch off) |
| `CastlemistLightVolumes.cginc` | includes VRC Light Volumes, or stubs (see below) |

Unity 2022.3, `#pragma target 3.5`, GPU instancing, single-pass stereo instanced
(the `UNITY_VERTEX_OUTPUT_STEREO` macros), fog. No Poiyomi or ThryEditor dependency.

## VRC Light Volumes

`CastlemistLightVolumes.cginc` includes
`Packages/red.sim.lightvolumes/Shaders/LightVolumes.cginc` (package 3.0.1 in the
GW2Porting project; public API v2/v3). In a project without the package,
uncomment `#define CASTLEMIST_NO_LIGHT_VOLUMES` at the top of that file: stubs
with the same signatures then use the light probes only.

Lit, ForwardBase:
- No lightmap: when `_LightVolumes` is on and `LightVolumesEnabled()`,
  `LightVolumeSHSpecular(worldPos, L0, L1r, L1g, L1b, spec, albedo, smoothness, metallic, normal, viewDir, normal * _LightVolumesBias, _PointLightVolumesShading)`,
  then the indirect diffuse is `LightVolumeEvaluate(normal, L0, L1r, L1g, L1b)`.
  Otherwise (no volumes in the scene, or the toggle off) the indirect diffuse is `ShadeSH9`
  (full L2 probes, not the package's L1-only fallback).
- Lightmapped (`LIGHTMAP_ON`): lightmap (directional when baked so) plus
  `LightVolumeAdditiveSHSpecular` (additive volumes and point light volumes only), the
  same split as the package's own `Light Volume PBR` shader.
- The volume specular (`spec`) is added on top of the BRDF when
  `_LightVolumeSpecular` is on, scaled by the packed map's specular mask and the specular tint.
- Reflection probes still give the indirect specular (Unity's `UnityGI_IndirectSpecular`,
  box projection and blending), scaled by the reflection mask and the reflection tint.

Particles are unlit, as GW2's particle shaders that castlemist ports are (the lit ones, e.g.
339180 and 48318, are drawn unlit with a warning).

## materials.json -> `Castlemist/GW2`

The importer (CastlemistMaterials) writes every block float on every build, so a re-import never
keeps a block from before.

| materials.json | Property | Notes |
|---|---|---|
| `maps.baseColor.file`, `.uv` | `_MainTex`, `_MainTexUV` (0-3) | `_Color` white |
| `maps.normal.file`, `.uv` | `_BumpMap`, `_BumpMapUV`, `_UseNormalMap` 1 | `_BumpScale` 1 |
| `maps.packed.file`, `.uv` | `_PackedMap`, `_PackedMapUV`, `_UsePacked` 1 | R metal, G smooth, B reflection mask, A specular mask; set `_Metallic` 1 and `_Glossiness` 1 (multipliers, as Poiyomi's) |
| `reflectionTint` | `_ReflectionTint` | |
| `specularTint` | `_SpecularTint` | |
| `maps.extras[]` with `use` `"detail-multiply2x"` (role `mod`) | `_DetailMod`, `_DetailModUV`, `_UseDetailMod` 1 | albedo x 2 x mod (`unity_ColorSpaceDouble`); `_DetailModStrength` 1 |
| `maps.emissionMap` (else `emissionBaked`) | `_EmissionMap`, `_EmissionMapUV`, `_UseEmission` 1 | |
| `maps.emissionMask` | `_EmissionMask`, `_EmissionMaskUV`, `_EmissionMaskChannel` (0 R) | |
| `emission.color`, `emission.strength` | `_EmissionColor`, `_EmissionStrength` | strength <= 0: 1, as the Poiyomi path |
| `texTransforms[]` (per UV set) | `_UVAnim<uv>` = (scroll u, -scroll v, -rotate, 0) | scroll per second, rotate radians per second about the UV centre (UNPROVEN units); V and the rotation negated (FBX v up). An older export: the manifest's `uvScroll` |
| `profile` fx-parallax-* | `_UseParallax` 1, `_Unlit` 1, `_AlphaTest` 1 (`_Cutoff` 1/255): see the parallax section | |
| `unlit` | `_Unlit` 1 | |
| `settings.uv1InUv4` (manifest) | `_UV1FromUV4` 1 | |
| `mode` / `preset` | `_Mode` (bookkeeping), and the preset row's `_SrcBlend`, `_DstBlend`, `_ZWrite`, queue, `RenderType` tag | same rows as `PoiyomiMaterials.PresetFor` |
| mode 1 / 9 (Cutout, TransClipping) | `_AlphaTest` 1, `_Cutoff` = `alphaCutoff` | |
| mode 3 (Transparent) | `_AlphaPremultiply` 1 | |
| additive presets (dst One) | `_AdditiveFog` 1 | fog to black |
| `exact: false` + `blend` | `_SrcBlend`, `_DstBlend`, `_BlendOp`, `_SrcBlendAlpha`, `_DstBlendAlpha`, `_BlendOpAlpha` via `PoiyomiMaterials.UnityFactor` / `UnityOp` | |
| `cull` | `_Cull` (0 Off, 2 Back) | Off: back faces flip the normal (VFACE) |
| `renderQueueOffset` | `material.renderQueue` = preset queue + offset | |
| `maps.cubemaps[]` (role `projection`, `reflect`; fx-cubemap: `cubemap`) | `_ProjCube`, `_ReflCube`, `_UseWaterCubes` 1; `_EnvCube`, `_UseEnvCube` 1 | GW2's own DDS cube, written as `.dds` with its mip chain completed (Unity refuses a partial one); faces as stored, looked up with the GW2-space direction (x, y north, z down) |
| COLOR_0 (world export: `GltfExportOptions::vertexColors`) | mesh colours, read by `_UseVertexColor` / `_UseFlow` | GW2's vertex colour as stored; the Blender FBX step writes it linear |
| `decal`, `decalMask`, `rim`, `projector` outside the blocks below | not mapped | |

A blended material (ZWrite off) gets `material.SetShaderPassEnabled("ShadowCaster", false)`;
the shadow pass clips only with `_AlphaTest`.

## `Castlemist/GW2 Particle`

The one shader for every ripped GW2 particle (2026-10-22). GW2 draws particles with a small set
of particle pixel shaders, read here from the AMATs' DXBC (`gw2dat_cli amat --dump-all`;
`AMATDBG=1` lists every technique, pass and effect with its blend). Each is a `_PtcShader`
family, written with GW2's own formulas (`GW2Particle.cginc`'s header): 0 basic (a stand-in),
1 soft additive, 2 premultiplied, 3 step dissolve, 4 alpha-test fade (+ add layer), 5 flow,
6 fire (GW2Core's fire block), 7 flow layer. `CastlemistMaterials.BuildParticleMaterial`
picks the family (`FamilyOf`: the material's constants and profile) and warns for an AMAT not
yet read from the game. Screen-distortion particles (709206, 959502) get `Castlemist/GW2 Distort`.

| materials.json | Property | Notes |
|---|---|---|
| `blend` (raw factors, always) | `_SrcBlend`, `_DstBlend`, ... | the AMAT's blend: the families' colour maths assumes it |
| `maps.baseColor` | `_MainTex` | the cloud's texCoordRect as its tiling / offset (ParticleEffects) |
| `gw2.depfade` / `depdist` / `depdis` (else 17) | `_DepDist` (x unity scale), `_SoftFade` | the literal 17 is GW2's default in the shaders; 0 = no fade |
| `gw2.step`, `gw2.diffade` | `_StepMin`, `_AlphaScale` | step family; without `diffade` the fade is the vertex red (48767) |
| extra `add`, `gw2.addcolr`, `gw2.stepmin` | `_AddTex`, `_AddColor`, `_AddStepMin`, `_UseAddLayer` 1 | 1601524's pass 1 (SrcAlpha / One), the "Always" pass |
| extra `flow`, `gw2.flow` / `startcl` / `endcl` / `stepcol` | `_FlowTex`, `_Flow`, `_FlowStart`, `_FlowEnd`, `_FlowStep` | flow family (882183) |
| extras `opacitytile`, `flow`, `gw2.uvscrol` / `flowuv` | `_LayerTex`, `_FlowTex`, `_LayerScroll`, `_LayerFlowUV` | flow layer (1764372) |
| fx-fire | as `Castlemist/GW2`'s fire block | modes 2 / 3 (977200, 1171331) |
| `texTransforms[]` UV0 | `_UVAnim0` | |

The renderer needs the custom vertex streams Position, Color, UV, UV2, AnimBlend,
StableRandomXYZ (`CastlemistMaterials.ApplyParticleStreams`): UV2 and AnimBlend blend flipbook
frames as GW2's flipbook variants do (uv1 = the next frame, uv2.x the blend), and the stable
randoms stand in for GW2's per-particle uv1-uv3 phases in the fire and flow shaders (UNPROVEN:
GrCloud's vertex fill is not read). Colour space: GW2 shades in gamma; the families multiply
colours in the project's space, use `unity_ColorSpaceDouble` for GW2's x 2, raise a coverage
folded into a colour to 2.2 in linear projects, and take the vertex colour back to gamma where
GW2 reads it as parameters (flow thresholds, step fades). Data maps (noise, mask, flow, add)
must import linear (`CastlemistPostprocessor.RoleOf`).

Unity's particle-system GPU instancing (`UnityStandardParticleInstancing.cginc`) is not set up;
`multi_compile_instancing` covers FX meshes on MeshRenderers.

## Scene look: fog, ambient, post-processing, probes (2026-10-22)

`Editor/SceneSetup.cs` runs after the import's sky and sun and reads `manifest.environment`
(the map's env chunk; the C++ export's contract). Each part has an `ImportOptions` toggle (the
window's "Scene look" block; batch `-castlemistNoEnv fog,ambient,post,lightvolumes,reflectionprobes,occlusion|all`),
all on by default; the Quest preset turns post-processing off (VRChat runs none on Android).
Everything it places sits under the unscaled `GW2_<fileId>_Environment`; the import logs an `ENV` line.
All GW2 -> Unity mappings below are **UNPROVEN readings** of raw data (`EnvironmentMath` in
`WorldManifest.cs`, unit-tested in `Tests~`), not measurements against the client.

- **Haze volume** (the default fog path; toggle "Haze volume (GW2 height fog)", batch name
  `haze`; off in the Quest preset): `GW2 Haze`, a Unity cube under the map root, 2x the map's
  largest side + 200 units, no collider, shadows or probes, with `Castlemist/GW2 Haze`
  (`GW2 Haze.shader`, after Silent's SkyProbe Fog, MIT; GW2's distance + height formula in its
  header) in `GW2_<fileId>_Haze.mat`. The material gets the haze raw: `_HazeNear/_HazeFar/_HazeHeight`
  (RGBA), `_DistRange` in GW2 units (the shader divides by `_UnityScale`), `_HeightRange` as
  stored (map z, up = -Z; from `heightRangeUnity` as (-hi, -lo) when the raw pair is missing,
  UNPROVEN), `_DepthCue`, `_UnityScale`, `_MapOriginY`, `_ApplySkybox` 1. Unity's fog is turned
  off (the volume fogs everything drawn before it, avatars too). It needs the camera depth
  texture (in VRChat the sun's shadows give it); transparents drawn after queue
  Transparent-216 are not fogged by it (accepted for now). The reference camera's far clip
  covers the cube.
- **Fog** (the fallback: haze volume off, `haze` null or the shader missing; `UNITY_APPLY_FOG` in these shaders, Poiyomi's own fog): Linear, because the haze is a
  distance range (`distRange`) and Exponential has no start. The haze amount is read as a line
  through (`distRange[0]`, `near.a`) and (`distRange[1]`, `far.a`); Unity's start / end are its
  0 and 1 crossings, x unityScale. Unity fog has one colour: near and far blended by their
  alphas. **Not supported**: height haze (`haze.height`, `heightRange`: built-in fog has no
  height term), per-zone haze (`zones`), `depthCue`, the sky (Skybox/Panoramic is not fogged).
- **Ambient**: Trilight from the fills `lights[1..]`, each a Lambert light along `unityForward`
  (sky = max(0, L.y), ground = max(0, -L.y), equator = the ring mean sqrt(1 - L.y^2) / pi).
  Trilight rather than a hand-set SH probe: Unity recomputes `ambientProbe` from the mode on
  every environment update and bake, so only the mode's colours survive a bake. No fills: the
  Flat `light.ambient` as before (it is the same fill, so it is not added). The sun stays
  `light.sun`; it turns Mixed (Baked Indirect) when light volumes are on.
- **Post-processing** (Post Processing Stack v2, `com.unity.postprocessing`; the code compiles
  only with `UNITY_POST_PROCESSING_STACK_V2` and the asmdef's `CASTLEMIST_PPV2`): a global
  volume on the `PostProcessing` layer (made in the first free layer 22-31), profile
  `GW2_<fileId>_PostProcess.asset`. Bloom: intensity `glowAmplify`, threshold max(`glowLevel.rgb`),
  tint `glow.rgb`. Colour grading in LDR mode (GW2's LUT is a LDR strip): saturation
  (`saturation` - 1) x 100, colour filter white -> `tintColor` by `tintAmount`, and `effect.clut`
  as the LDR lookup strip when it is width = height^2 (`environment/clut_*.png` imports linear,
  uncompressed, no mips, clamp: `CastlemistPostprocessor.ApplyLutSettings`; its slice order and
  v direction against PPv2's are UNPROVEN). MSVO ambient occlusion at `ssaoAmount`. No depth of
  field (VR). The `PostProcessLayer` goes on the VRC scene descriptor's Reference Camera (VRChat
  SDK3: a disabled `ReferenceCamera` is made and linked; the bake menu links it to a descriptor
  added later), else `Camera.main`, else a new `Main Camera` at the spawn.
- **VRC Light Volumes** (`red.sim.lightvolumes` 3.x, by reflection): one `LightVolumeInstance`
  per cell over its renderer bounds, adaptive resolution, at most `lightVolumeBudget` voxels a
  cell (65 536; Quest 16 384) and 1 voxel per unit, registered with the scene's
  `LightVolumeManager` by the package's own onboarding. 3.x's `LightVolumeSetup` / `LightVolume`
  are migration-only legacy types and are not used. Unlit until baked.
- **Reflection probes**: one baked, box-projected probe per cell (256 px, Quest 128), captured
  2 units above the ground at the cell centre; the default reflection is the skybox.
- **Occlusion and GI flags**: opaque renderers (every material's queue < 2450) Occluder +
  Occludee, cutout and transparent Occludee only, ContributeGI on queue < 2500 with
  `receiveGI = Light Probes` (GW2's UV1 is no lightmap UV: nothing is lightmapped), and
  ReflectionProbeStatic. Animated props, foliage, particle and skinned renderers are left as the
  importer set them. One `OcclusionArea` over the map; the scene's LightingSettings
  `GW2_<fileId>_Lighting.lighting` (baked GI, no realtime GI, Progressive GPU, Baked Indirect).
- **Bake**: **Castlemist > Bake Lighting and Occlusion (Unity / Bakery)** saves nothing itself: save the scene,
  then it runs `Lightmapping.BakeAsync` (light probes, reflection probes, light volumes) and,
  when that completes, `StaticOcclusionCulling.GenerateInBackground`.

## UNPROVEN / open

- The uvScroll unit (per second) is the export's own UNPROVEN reading.
- The "detail-multiply2x" reading of the `mod` map comes from the shader profiles, not from
  the game's pixel shader.
- Not reproduced yet: `addnormal` / `decalnormal` normal blending, GW2's `Time` pulses (e.g.
  1676989's reveal, whose `color.w` is 0 at rest), GW2's own fresnel reflection on the lit props,
  1676989's luminance step term, and the AlphaRef discard of 2591650 (the engine's AlphaRef is not
  in the export).
- The soft depth fades (`depfade`, `dfadevl`, `depdist`) are set but off on meshes (`_SoftFade` 0);
  particles have them on. Either fades only when a camera depth texture is bound (`CM_SoftFade`).
- Particle families read so far: the AMATs of the Firebrand tomes (2026-10-22). The Tome's rune
  shaders (1764349, 1764348, 1764443, 1764445, 1764347) and the lit particle shaders (339180,
  48318) are not transcribed: they draw as the family their constants suggest, with a warning.
  1764372's flow map (fileId 1764337) does not decode (not ATEX; probably a plain DDS) and is
  missing from the export. 965703's `fadedif` and 27304's `stepmin` are 0 as stored
  (runtime-driven, UNPROVEN), so those are invisible until animated.
- 630598 (armour glass) has no diffuse in the MODL (a runtime character composite): grey stands in.
- Still unsupported: armor-silk (2507831, 3252849) and jade-interior (2472137), character shaders.
- fx-flow (2591650, 2759298), 709501 and 2434562 are ported from the DXBC but not seen drawn: on
  the Mists vault no mesh uses them, and no mesh there has a vertex colour.
- Lightmaps share TEXCOORD1 with GW2's UV1: a model whose maps use UV1 must not be baked with
  "Generate Lightmap UVs". Light Volumes are the intended lighting path.
- Not handled: realtime GI (`DYNAMICLIGHTMAP_ON` is skipped), subtractive lighting mode, and
  shadowmask beyond Unity's attenuation macros.

## The parallax block of `Castlemist/GW2`

A port of the game's parallax-layer effect, from its DXBC (the bgfx blobs keep the uniform
names, so the constant mapping is exact; the formula is in GW2Core.cginc's header). The
importer turns it on for the `fx-parallax-layer` (842652), `fx-parallax-ramp` (1507908: the Mists vault walls
2039598, planes 3560838) and `fx-parallax-fade` (1730340: domes, stars) profiles, in **both**
shader modes (Poiyomi cannot draw it). Each MODL texTransform animates its UV set (UV3's
rotation is the swirl); units UNPROVEN. The soft depth fade is off by default (it needs the
camera depth texture; in VRChat, a shadow-casting light).

## GW2 UV1 and lightmaps

Unity's lightmapper owns UV1, and Generate Lightmap UVs overwrites it, but GW2 samples real
maps on UV1 (masks, `mod` detail, decals). A world export with `settings.uv1InUv4` copies GW2's
UV1 into UV4 as well; the castlemist shaders then read it from there (`_UV1FromUV4`, set by the
importer). Poiyomi selects UV0-UV3 only, so its UV1 maps still break under baked lightmaps.

## Port status (the Mists vault 3560860, then the AMAT scan of 2026-10-22)

Every mesh material of the Mists vault import is drawn by a castlemist shader (audit
2026-10-22: 227 `Castlemist/GW2`, 4 `Castlemist/GW2 Distort`, no fallbacks). Read from each
AMAT's DXBC: `gw2dat_cli amat --file-id <amat> --out-dir <d>` dumps the blobs, D3DDisassemble
gives the DXBC, the blob's uniform table names the constants, and the material's texture tokens
name the slots.

| GW2 | Profile / AMAT | Block(s) |
|---|---|---|
| Parallax layers | fx-parallax-layer 842652, fx-parallax-ramp 1507908, fx-parallax-fade 1730340 | parallax |
| Orb interiors, cores | prop-interior 3036412, 1601802, 2314655 | interior, perturb, mask, step, cutout, rim |
| Burn-off | prop-burn 3122105 | cutout and burn edge, decal |
| Ramp glows | fx-ramp-glow 1998356, fx-perturb-mod-glow 574526 | ramp, perturb |
| Soft-additive effects | fx-soft-additive 882285, 1478777, 709501, 2434562 | premultiplied base, ramp, mask, vertex colour |
| Perturbed glow | fx-perturb-glow 1946706 | perturb |
| Lit props | default 3576632, 3576633, 3121869; prop-metalmask 3121953; prop-lit* | lit, rim (rimcp) |
| Distortion | fx-distort-opaque 50159, fx-distort-opaque-mask 339341, fx-distort-mask 47396, fx-distort-step 592660 / 27304, fx-distort-soft 709206 (particles), fx-distort-lit 630598 (armour) | `GW2 Distort` |
| Multiply decals | fx-multiply 15206 (DstColor / Zero), fx-multiply-bump 157432, 3718974 (2x multiply: bump shading against flat) | fogneutral, bumpshade |
| View cubemap | fx-cubemap 221571 (Twilight's starfield) | envcube |
| Fire | fx-fire 740364, 965703 (flame meshes); 977200, 1171331 (particles) | fire (`_FireMode` 0-3) |
| Glass and water refraction | glass-refract 49659, glass-refract-mask 63923, water-refract 57026 | `GW2 Distort` `_Glass` 1 / 2 (lit: Light Volumes or SH, sun, probe) |
| Rift water | 3121604 | water: GW2's cubes, triplanar caustics, flow |
| Flow | fx-flow 2591650, 2759298 | flow (parameters in the vertex colour) |
| Singletons | 3100353 (perturbed normal, cut at 0.25), 1676989 (ramp, step, premultiplied), 80417 (screen layer, ramp RGBA) | as named |
