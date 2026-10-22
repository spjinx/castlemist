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
| `GW2 Particle.shader` | `Castlemist/GW2 Particle`: particle systems, unlit, any blend |
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

Particle: `_LightVolumeTint` > 0 multiplies the colour by
`lerp(1, LightVolumeSH_L0(worldPos, 0), _LightVolumeTint)`, per vertex. The pass is tagged
`ForwardBase` so Unity sets the renderer's light probe SH (the fallback when there are no volumes).

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

| Property | What |
|---|---|
| `_MainTex` | texture (x vertex colour x `_TintColor` x `_Intensity`) |
| `_TintColor` (HDR), `_Intensity` | colour, brightness |
| `_UVScroll` | xy UV per second |
| `_SrcBlend`, `_DstBlend`, `_BlendOp` | default SrcAlpha One (additive); Fade = SrcAlpha OneMinusSrcAlpha |
| `_AlphaPremultiply` | rgb x alpha (for One One) |
| `_InvFade` | soft particles (needs the camera depth texture; `SOFTPARTICLES_ON` from Quality settings) |
| `_LightVolumeTint` | 0..1, see above |
| `_ZWrite`, `_ZTest`, `_Cull`, `_AdditiveFog` | render state; fog to black by default |

Unity's particle-system GPU instancing (`UnityStandardParticleInstancing.cginc`) is not set up;
`multi_compile_instancing` covers FX meshes on MeshRenderers.

## UNPROVEN / open

- The uvScroll unit (per second) is the export's own UNPROVEN reading.
- The "detail-multiply2x" reading of the `mod` map comes from the shader profiles, not from
  the game's pixel shader.
- Not reproduced yet: `addnormal` / `decalnormal` normal blending, GW2's `Time` pulses (e.g.
  1676989's reveal, whose `color.w` is 0 at rest), GW2's own fresnel reflection on the lit props,
  1676989's luminance step term, and the AlphaRef discard of 2591650 (the engine's AlphaRef is not
  in the export).
- The soft depth fades (`depfade`, `dfadevl`, `depdist`) are set but off (`_SoftFade` 0): they need
  the camera depth texture.
- Particle-only ports (977200, 1171331, 709206) wait for a particle import; 965703's `fadedif` and 27304's
  `stepmin` are 0 as stored (runtime-driven, UNPROVEN), so those are invisible until animated.
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
