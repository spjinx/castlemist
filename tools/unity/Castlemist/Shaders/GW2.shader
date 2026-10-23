// castlemist: the one shader for GW2 world materials in VRChat (built-in pipeline). Lit props and
// unlit GW2 effects (parallax layers, masks, steps, fades) are blocks of this shader, switched by
// material floats, not keywords (GW2Core.cginc; README.md maps materials.json to properties).
// Particles use Castlemist/GW2 Particle.

Shader "Castlemist/GW2"
{
    Properties
    {
        [Header(Colour)]
        _Color ("Colour", Color) = (1, 1, 1, 1)
        _MainTex ("Base colour (RGB), alpha (A)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _MainTexUV ("Base colour UV", Float) = 0
        [ToggleUI] _AlphaTest ("Alpha test", Float) = 0
        _Cutoff ("Alpha cutoff", Range(0, 1)) = 0.5
        [ToggleUI] _AlphaPremultiply ("Premultiply alpha", Float) = 0

        [Header(Detail mod)]
        [ToggleUI] _UseDetailMod ("Detail mod", Float) = 0
        _DetailMod ("Mod map (RGB, 0.5 = no change)", 2D) = "grey" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _DetailModUV ("Mod UV", Float) = 1
        _DetailModStrength ("Mod strength", Range(0, 1)) = 1

        [Header(Normal)]
        [ToggleUI] _UseNormalMap ("Normal map", Float) = 0
        [Normal][NoScaleOffset] _BumpMap ("Normal map", 2D) = "bump" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _BumpMapUV ("Normal UV", Float) = 0
        _BumpScale ("Normal strength", Float) = 1

        [Header(Packed map)]
        [ToggleUI] _UsePacked ("Packed map", Float) = 0
        [NoScaleOffset] _PackedMap ("Packed map (linear)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _PackedMapUV ("Packed UV", Float) = 0
        _Metallic ("Metallic (x map R)", Range(0, 1)) = 0
        _Glossiness ("Smoothness (x map G)", Range(0, 1)) = 0.5
        _ReflectionTint ("Reflection tint", Color) = (1, 1, 1, 1)
        _ReflectionStrength ("Reflection strength", Range(0, 1)) = 1
        _SpecularTint ("Specular tint", Color) = (1, 1, 1, 1)
        _SpecularStrength ("Specular strength", Range(0, 5)) = 1

        [Header(Emission)]
        [ToggleUI] _UseEmission ("Emission", Float) = 0
        [NoScaleOffset] _EmissionMap ("Emission map (RGB)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _EmissionMapUV ("Emission map UV", Float) = 0
        [NoScaleOffset] _EmissionMask ("Emission mask (linear)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _EmissionMaskUV ("Emission mask UV", Float) = 0
        [Enum(R, 0, G, 1, B, 2, A, 3)] _EmissionMaskChannel ("Emission mask channel", Float) = 0
        [HDR] _EmissionColor ("Emission colour", Color) = (1, 1, 1, 1)
        _EmissionStrength ("Emission strength", Float) = 1
        _EmissionPtrb ("gloptrb (emission perturb strength)", Float) = 0

        [Header(Parallax layer (GW2 fx parallax))]
        [ToggleUI] _UseParallax ("Parallax layer", Float) = 0
        _ParallaxTex ("Parallax colour (RGB), alpha (A)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _ParallaxUV ("Parallax UV", Float) = 0
        [NoScaleOffset] _OffsetTex ("UV offset map (RG, linear)", 2D) = "black" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _OffsetUV ("Offset UV", Float) = 3
        _ParaPer ("paraper (offset map strength)", Float) = 0.125
        _ParDist ("pardist (view parallax depth)", Float) = 0.5

        [Header(Mask and perturb)]
        [NoScaleOffset] _MaskTex ("Mask (linear)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _MaskUV ("Mask UV", Float) = 1
        [Enum(R, 0, G, 1, B, 2, A, 3)] _MaskChannel ("Mask channel", Float) = 0
        [ToggleUI] _UseMaskConstant ("Constant mask (a GW2 placeholder layer)", Float) = 0
        _MaskConstant ("Constant mask colour", Vector) = (1, 1, 1, 1)
        [NoScaleOffset] _PerturbTex ("Perturb (RG, linear)", 2D) = "black" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _PerturbUV ("Perturb UV", Float) = 2
        [ToggleUI] _PerturbSigned ("Perturb is signed (x 2 - 1)", Float) = 0
        _MskPtrb ("mskptrb (mask perturb strength)", Float) = 0
        _BasePtrb ("difptrb (colour perturb strength)", Float) = 0
        [ToggleUI] _PerturbAlphaOnly ("Perturb the colour's alpha only", Float) = 0
        _ModPtrb ("modptrb (mod perturb strength, x mask R)", Float) = 0

        [Header(Interior layer (GW2 prop interior))]
        [ToggleUI] _UseInterior ("Interior layer", Float) = 0
        [NoScaleOffset] _InteriorTex ("Interior colour (RGB), shine (A)", 2D) = "black" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _InteriorUV ("Interior UV", Float) = 1
        _InteriorNormalShift ("Normal shift", Float) = 0.01833
        [Enum(Interior behind colour, 0, Layer over colour, 1, Decal (A is coverage), 2)] _InteriorMode ("Mix", Float) = 0
        [ToggleUI] _InteriorStep ("Step the mix (stepmin, decfade)", Float) = 0
        _InteriorStepOffset ("decfade (step offset)", Float) = 1

        [Header(Screen layer (GW2 add))]
        [ToggleUI] _UseScreenLayer ("Screen layer", Float) = 0
        [NoScaleOffset] _ScreenTex ("Screen layer (RGB)", 2D) = "black" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _ScreenUV ("Screen layer UV", Float) = 1
        [ToggleUI] _RampRGBA ("Ramp tints the colour too (RGBA)", Float) = 0
        [ToggleUI] _PerturbNormal ("Perturb the normal map too", Float) = 0

        [Header(Water (GW2 3121604))]
        [ToggleUI] _UseWater ("Water", Float) = 0
        [NoScaleOffset] _FlowTex ("Flow (RG, linear)", 2D) = "black" {}
        [ToggleUI] _UseWaterCubes ("GW2 cubemaps (else the reflection probe)", Float) = 0
        [NoScaleOffset] _ProjCube ("Projection cube (GW2 space)", Cube) = "black" {}
        [NoScaleOffset] _ReflCube ("Reflection cube (GW2 space)", Cube) = "black" {}
        _WaterEdge ("Edge colour (edgcolr)", Color) = (0, 1, 1, 0)
        _WaterFalloff ("falloff (x..y edge, z..w caustics)", Vector) = (1, 0.2, 0, 1)
        _CausScale ("causscl (caustics per GW2 unit)", Float) = 0.0025
        _FlowScale ("flowscl", Float) = 0.0075
        _FlowStrength ("flowstr", Float) = 0.15
        _ScrollSpeed ("scrspd (caustics xy, flow zw)", Vector) = (-0.04, 0.05, 0.04, -0.1)
        _WaveColor ("waveclr (caustics colour)", Color) = (0.6, 1, 2, 0)
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254

        [Header(Vertex colour and flow (GW2 fx flow))]
        [ToggleUI] _UseVertexColor ("Colour x vertex colour", Float) = 0
        [ToggleUI] _UseFlow ("Flow (vertex colour = parameters)", Float) = 0
        _Flow ("flow (uv scale, strength, power, scroll)", Vector) = (1, 1, 1, 0)
        _FlowStart ("startcl", Color) = (1, 1, 1, 1)
        _FlowEnd ("endcl", Color) = (1, 1, 1, 1)
        _FlowStep ("stepcol (min, max)", Vector) = (0, 1, 0, 0)
        [ToggleUI] _FlowPremul ("Flow colour x its alpha (premultiplied)", Float) = 0

        [Header(Multiply decals and cubemap (GW2 fx multiply, fx cubemap))]
        [ToggleUI] _UseEnvCube ("Cubemap by view (perturb = _PerturbTex)", Float) = 0
        [NoScaleOffset] _EnvCube ("Cubemap (GW2 space)", Cube) = "black" {}
        [ToggleUI] _UseBumpShade ("Bump shading (2x multiply)", Float) = 0
        [ToggleUI] _UseBumpShadeTex ("Bump shading x colour map", Float) = 0
        [NoScaleOffset] _BumpShadeTex ("Bump shading colour", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _BumpShadeUV ("Bump shading colour UV", Float) = 1
        _BumpShadeOpacity ("opacity", Float) = 1
        [ToggleUI] _UseFogNeutral ("Fog to grey (multiply)", Float) = 0
        _FogNeutral ("Fog grey", Float) = 1

        [Header(Fire (GW2 fx fire 740364 965703 977200 1171331))]
        [ToggleUI] _UseFire ("Fire (procedural flame; unlit, premultiplied)", Float) = 0
        [Enum(Flow flame 740364, 0, Stepped flame 965703, 1, Noise particle 977200, 2, Dyed noise particle 1171331, 3)] _FireMode ("Fire shader", Float) = 0
        [ToggleUI] _FireNoEdge ("No silhouette fade (2037995)", Float) = 0
        [NoScaleOffset] _FireDetail ("firedetail (G, linear)", 2D) = "black" {}
        [NoScaleOffset] _FireFlowB ("flowb (RG, linear)", 2D) = "grey" {}
        _FireUV0 ("fireduv (v scroll, scale cycles, scale min, max)", Vector) = (0, 0, 1, 1)
        _FireUV1 ("flowuv (v scroll, scale cycles, scale min, max)", Vector) = (0, 0, 1, 1)
        _FireUV2 ("flowbuv (v scroll, scale cycles, scale min, max)", Vector) = (0, 0, 1, 1)
        _FireParams ("Fire params (by mode; see CastlemistMaterials.Fire)", Vector) = (0, 0, 0, 1)
        _FireParams2 ("Fire params 2", Vector) = (0, 0, 0, 0)
        _FireParams3 ("Fire params 3", Vector) = (0, 1, 1, 0)
        _FireColorL ("colorl (raw GW2 value)", Vector) = (1, 1, 1, 1)
        _FireColorH ("colorh (raw GW2 value)", Vector) = (1, 1, 1, 1)
        _FireDye ("Dye palette colour, raw GW2 value (1171331 dyedot)", Vector) = (1, 1, 1, 1)

        [Header(Wave (castlemist cloth wave))]
        [ToggleUI] _UseWave ("Wave", Float) = 0
        [Enum(GW2 cloth pins (vertex colour), 0, UV0 V, 1, UV0 V inverted, 2, Everywhere, 3)] _WaveMask ("Wave weight", Float) = 0
        _WaveAmplitude ("Amplitude (Unity units)", Float) = 0.6
        _WaveSpeed ("Speed", Float) = 2.2
        _WaveFrequency ("Ripples along the cloth", Float) = 1.4
        _WaveSway ("Sway with the wind", Float) = 0.35
        _WaveDir ("Wind direction (world)", Vector) = (1, 0, 0.3, 0)

        [Header(Cutout and burn edge (GW2 opacity, burnoff, burncol))]
        [ToggleUI] _UseCutout ("Cutout", Float) = 0
        [NoScaleOffset] _CutoutTex ("Cutout (linear)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _CutoutUV ("Cutout UV", Float) = 1
        [Enum(R, 0, G, 1, B, 2, A, 3)] _CutoutChannel ("Cutout channel", Float) = 0
        _CutoutScale ("opacity (x the cutout; discard below 0.5)", Float) = 1
        _BurnOffset ("burnoff", Float) = 0
        _BurnColor ("burncol (edge glow; black = none)", Color) = (0, 0, 0, 0)

        [Header(Alpha shaping)]
        [ToggleUI] _UseMask ("Opacity mask (the mask channel x alpha)", Float) = 0
        [ToggleUI] _UseRamp ("View-angle ramp", Float) = 0
        [NoScaleOffset] _RampTex ("Ramp (R at |N.V|, linear)", 2D) = "white" {}
        [ToggleUI] _UseStep ("Step (stepmin / stepcol)", Float) = 0
        _StepMin ("stepmin", Range(0, 0.999)) = 0.75
        _StepCol ("stepcol (x scale, w offset)", Vector) = (1, 1, 1, 1)
        _AlphaScale ("Alpha scale (fade)", Float) = 1
        [ToggleUI] _SoftFade ("Soft depth fade (needs a depth texture)", Float) = 0
        _DepDist ("depdist (Unity units)", Float) = 0.4

        [Header(UV sets and animation (scroll u v per second, rotate radians per second))]
        [ToggleUI] _UV1FromUV4 ("GW2 UV1 from UV4 (UV1 left to lightmaps)", Float) = 0
        _UVAnim0 ("UV0", Vector) = (0, 0, 0, 0)
        _UVAnim1 ("UV1", Vector) = (0, 0, 0, 0)
        _UVAnim2 ("UV2", Vector) = (0, 0, 0, 0)
        _UVAnim3 ("UV3", Vector) = (0, 0, 0, 0)

        [Header(Lighting)]
        [ToggleUI] _Unlit ("Unlit (GW2 effect shaders)", Float) = 0
        [ToggleUI] _UseRim ("Rim light", Float) = 0
        _RimColor ("Rim colour (rimcp rgb)", Color) = (1, 1, 1, 1)
        _RimPower ("Rim power (rimcp w)", Float) = 3
        [ToggleUI] _RimShineFade ("Rim fades with shine", Float) = 0
        [ToggleUI] _RimUseMask ("Rim x (1 - mask channel)", Float) = 0
        [ToggleUI] _PremultipliedBase ("Base colour is premultiplied", Float) = 0
        [ToggleUI] _LightVolumes ("Use Light Volumes", Float) = 1
        [ToggleUI] _LightVolumeSpecular ("Light Volume speculars", Float) = 1
        _LightVolumesBias ("Light Volumes bias (along the normal)", Float) = 0
        _PointLightVolumesShading ("Point Light Volumes shading", Float) = 2

        [Header(Render state)]
        [HideInInspector] _Mode ("castlemist preset (_Mode, as Poiyomi's)", Float) = 0
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlend ("Src blend", Float) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlend ("Dst blend", Float) = 0
        [Enum(UnityEngine.Rendering.BlendOp)] _BlendOp ("Blend op", Float) = 0
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlendAlpha ("Src blend alpha", Float) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlendAlpha ("Dst blend alpha", Float) = 0
        [Enum(UnityEngine.Rendering.BlendOp)] _BlendOpAlpha ("Blend op alpha", Float) = 0
        [Enum(Off, 0, On, 1)] _ZWrite ("ZWrite", Float) = 1
        [Enum(UnityEngine.Rendering.CompareFunction)] _ZTest ("ZTest", Float) = 4
        [Enum(UnityEngine.Rendering.CullMode)] _Cull ("Cull", Float) = 2
        [ToggleUI] _AdditiveFog ("Fog to black (additive blends)", Float) = 0
    }

    SubShader
    {
        Tags { "RenderType" = "Opaque" "Queue" = "Geometry" }

        Pass
        {
            Name "FORWARD"
            Tags { "LightMode" = "ForwardBase" }
            Blend [_SrcBlend] [_DstBlend], [_SrcBlendAlpha] [_DstBlendAlpha]
            BlendOp [_BlendOp], [_BlendOpAlpha]
            ZWrite [_ZWrite]
            ZTest [_ZTest]
            Cull [_Cull]

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment fragBase
            #pragma multi_compile_fwdbase
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #pragma skip_variants DYNAMICLIGHTMAP_ON
            #define CM_PASS_FORWARDBASE
            #include "GW2Core.cginc"
            ENDCG
        }

        Pass
        {
            Name "FORWARD_DELTA"
            Tags { "LightMode" = "ForwardAdd" }
            Blend [_SrcBlend] One
            BlendOp [_BlendOp]
            ZWrite Off
            ZTest LEqual
            Cull [_Cull]

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment fragAdd
            #pragma multi_compile_fwdadd_fullshadows
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #define CM_PASS_FORWARDADD
            #include "GW2Core.cginc"
            ENDCG
        }

        Pass
        {
            Name "ShadowCaster"
            Tags { "LightMode" = "ShadowCaster" }
            ZWrite On
            ZTest LEqual
            Cull [_Cull]

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertShadow
            #pragma fragment fragShadow
            #pragma multi_compile_shadowcaster
            #pragma multi_compile_instancing
            #define CM_PASS_SHADOWCASTER
            #include "GW2Core.cginc"
            ENDCG
        }

        Pass
        {
            Name "META"
            Tags { "LightMode" = "Meta" }
            Cull Off

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertMeta
            #pragma fragment fragMeta
            #define CM_PASS_META
            #include "GW2Core.cginc"
            ENDCG
        }
    }
    FallBack Off
    CustomEditor "Castlemist.GW2GUI"
}
