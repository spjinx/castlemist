// castlemist: the one shader for GW2 particles in VRChat (built-in pipeline). Every GW2 particle
// pixel-shader family the importer knows is a value of _PtcShader, transcribed from the game's
// DXBC; the fire block and the maps are GW2Core.cginc's (GW2Particle.cginc has the formulas,
// README.md the property list). Unlit; the blend is the AMAT's. Needs the particle renderer's
// custom vertex streams (CastlemistMaterials.ParticleStreams): UV, UV2, AnimBlend, StableRandomXYZ.

Shader "Castlemist/GW2 Particle"
{
    Properties
    {
        _PtcShader ("GW2 particle shader", Float) = 0
        _MainTex ("Colour (RGB), alpha (A)", 2D) = "white" {}
        [HideInInspector] _Color ("Colour", Color) = (1, 1, 1, 1)
        [ToggleUI] _FrameBlend ("Blend flipbook frames (UV2, AnimBlend)", Float) = 1
        [ToggleUI] _PtcTexAlphaRgb ("Soft: colour x the texture's alpha", Float) = 1
        [ToggleUI] _PtcVcColour ("Colour x 2 x vertex colour", Float) = 1
        [ToggleUI] _PtcFadeVcR ("Step: fade by the vertex red (else Alpha scale)", Float) = 0
        [ToggleUI] _AlphaPremultiply ("Basic: colour x alpha (One-source blends)", Float) = 0
        _Cutoff ("AlphaRef (discard below)", Range(0, 1)) = 0.0039216

        [Header(Alpha shaping)]
        _StepMin ("step / stepmin", Range(0, 0.999)) = 0.75
        _AlphaScale ("diffade (alpha scale)", Float) = 1
        [ToggleUI] _SoftFade ("Soft depth fade (needs a depth texture)", Float) = 1
        _DepDist ("depfade / depdist (Unity units)", Float) = 0.4318

        [Header(Flow vertex colour holds the parameters)]
        [NoScaleOffset] _FlowTex ("Flow (RG, linear)", 2D) = "grey" {}
        _Flow ("flow (uv scale, strength, power, scroll)", Vector) = (1, 1, 1, 0)
        _FlowStart ("startcl", Color) = (1, 1, 1, 1)
        _FlowEnd ("endcl", Color) = (1, 1, 1, 1)
        _FlowStep ("stepcol (min, max)", Vector) = (0, 1, 0, 0)

        [Header(Flow layer 1764372)]
        [NoScaleOffset] _LayerTex ("Layer (opacitytile; nebula background)", 2D) = "white" {}
        _LayerScroll ("uvscrol (uv scale xy, scroll per second zw)", Vector) = (1, 1, 0, 0)
        _LayerFlowUV ("flowuv (scale xy, scroll per second zw)", Vector) = (0, 0, 0, 0)

        [Header(Nebula 728777)]
        [NoScaleOffset] _PerturbTex ("perturb (RG, linear)", 2D) = "grey" {}
        _BasePtrb ("perturb (offset strength)", Float) = 0.1
        _PtcUVSS ("uvss (perturb scroll xy per second, uv scale z)", Vector) = (0, 0, 1, 0)
        _ScreenTile ("zotile (background tiles across the screen)", Float) = 5

        [Header(Add layer second pass 1601524)]
        [ToggleUI] _UseAddLayer ("Add layer", Float) = 0
        [NoScaleOffset] _AddTex ("add (R step, A coverage)", 2D) = "black" {}
        _AddColor ("addcolr", Color) = (1, 1, 1, 1)
        _AddStepMin ("stepmin", Range(0, 0.999)) = 0.75

        [Header(Fire GW2 fx fire 977200 1171331)]
        [HideInInspector] _UseFire ("Fire", Float) = 0
        [Enum(Flow flame 740364, 0, Stepped flame 965703, 1, Noise particle 977200, 2, Dyed noise particle 1171331, 3)] _FireMode ("Fire shader", Float) = 2
        [HideInInspector] _FireNoEdge ("No silhouette fade", Float) = 1
        [NoScaleOffset] _MaskTex ("mask", 2D) = "white" {}
        [NoScaleOffset] _FireDetail ("firedetail (G, linear)", 2D) = "black" {}
        [NoScaleOffset] _FireFlowB ("flowb (RG, linear)", 2D) = "grey" {}
        [NoScaleOffset] _RampTex ("ramp", 2D) = "white" {}
        _FireUV0 ("fireduv", Vector) = (0, 0, 1, 1)
        _FireUV1 ("flowuv", Vector) = (0, 0, 1, 1)
        _FireUV2 ("flowbuv", Vector) = (0, 0, 1, 1)
        _FireParams ("Fire params (perturb, scalep, speedp, speed)", Vector) = (0, 1, 0, 0)
        _FireParams2 ("Fire params 2 (scale, scaleb)", Vector) = (1, 1, 0, 0)
        _FireParams3 ("Fire params 3", Vector) = (0, 1, 1, 0)
        _FireColorL ("colorl (raw GW2 value)", Vector) = (1, 1, 1, 1)
        _FireColorH ("colorh (raw GW2 value)", Vector) = (1, 1, 1, 1)
        _FireDye ("Dye palette colour (1171331)", Vector) = (1, 1, 1, 1)

        [Header(UV)]
        _UVAnim0 ("UV0 animation (scroll xy, rotate z)", Vector) = (0, 0, 0, 0)
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254

        [Header(Render state)]
        [HideInInspector] _Mode ("castlemist preset (_Mode, as Poiyomi's)", Float) = 5
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlend ("Src blend", Float) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlend ("Dst blend", Float) = 6
        [Enum(UnityEngine.Rendering.BlendOp)] _BlendOp ("Blend op", Float) = 0
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlendAlpha ("Src blend alpha", Float) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlendAlpha ("Dst blend alpha", Float) = 6
        [Enum(UnityEngine.Rendering.BlendOp)] _BlendOpAlpha ("Blend op alpha", Float) = 0
        [Enum(Off, 0, On, 1)] _ZWrite ("ZWrite", Float) = 0
        [Enum(UnityEngine.Rendering.CompareFunction)] _ZTest ("ZTest", Float) = 4
        [Enum(UnityEngine.Rendering.CullMode)] _Cull ("Cull", Float) = 0
        [ToggleUI] _AdditiveFog ("Fog to black (additive blends)", Float) = 1
    }

    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" "PreviewType" = "Plane" }

        Pass
        {
            Name "FORWARD"
            Tags { "LightMode" = "ForwardBase" }
            Blend [_SrcBlend] [_DstBlend], [_SrcBlendAlpha] [_DstBlendAlpha]
            BlendOp [_BlendOp], [_BlendOpAlpha]
            ZWrite [_ZWrite]
            ZTest [_ZTest]
            Cull [_Cull]
            ColorMask RGB

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertParticle
            #pragma fragment fragParticle
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #define CM_PASS_PARTICLE
            #include "GW2Particle.cginc"
            ENDCG
        }

        // GW2 1601524's pass 1 (SrcAlpha / One, from its AMAT): off unless _UseAddLayer (the importer
        // also disables the "Always" pass of a material without one).
        Pass
        {
            Name "ADDLAYER"
            Tags { "LightMode" = "Always" }
            Blend SrcAlpha One
            ZWrite Off
            ZTest [_ZTest]
            Cull [_Cull]
            ColorMask RGB

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vertParticle
            #pragma fragment fragAddLayer
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #define CM_PASS_PARTICLE
            #include "GW2Particle.cginc"
            ENDCG
        }
    }
    FallBack Off
    CustomEditor "Castlemist.GW2ParticleGUI"
}
