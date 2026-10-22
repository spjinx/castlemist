// castlemist: GW2 particles and FX meshes for VRChat (built-in pipeline). Unlit:
// texture x vertex colour x tint x intensity, any blend the importer sets
// (default additive: SrcAlpha One), optional soft particles, optional tint by
// the VRC Light Volumes / light probe ambient at the vertex (L0 only).
// README.md has the property list.

Shader "Castlemist/GW2 Particle"
{
    Properties
    {
        _MainTex ("Texture", 2D) = "white" {}
        [HDR] _TintColor ("Tint", Color) = (1, 1, 1, 1)
        _Intensity ("Intensity", Float) = 1
        _UVScroll ("Scroll (xy, UV per second)", Vector) = (0, 0, 0, 0)
        [ToggleUI] _AlphaPremultiply ("Multiply colour by alpha (One One blends)", Float) = 0
        _InvFade ("Soft particles factor", Range(0.01, 3)) = 1
        _LightVolumeTint ("Light Volumes tint", Range(0, 1)) = 0

        [Header(Render state)]
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlend ("Src blend", Float) = 5
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlend ("Dst blend", Float) = 1
        [Enum(UnityEngine.Rendering.BlendOp)] _BlendOp ("Blend op", Float) = 0
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
            // ForwardBase so Unity sets the renderer's light probe SH (the Light Volumes fallback).
            Tags { "LightMode" = "ForwardBase" }
            Blend [_SrcBlend] [_DstBlend]
            BlendOp [_BlendOp]
            ZWrite [_ZWrite]
            ZTest [_ZTest]
            Cull [_Cull]
            ColorMask RGB

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_particles
            #pragma multi_compile_fog
            #pragma multi_compile_instancing

            #include "UnityCG.cginc"
            #include "CastlemistLightVolumes.cginc"

            sampler2D _MainTex;
            float4 _MainTex_ST;
            half4 _TintColor;
            half _Intensity;
            float4 _UVScroll;
            half _AlphaPremultiply;
            float _InvFade;
            half _LightVolumeTint;
            half _AdditiveFog;
            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);

            struct appdata
            {
                float4 vertex : POSITION;
                fixed4 color : COLOR;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                fixed4 color : COLOR;
                float2 uv : TEXCOORD0;
                half3 ambient : TEXCOORD1;
                UNITY_FOG_COORDS(2)
                #if defined(SOFTPARTICLES_ON)
                    float4 projPos : TEXCOORD3;
                #endif
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.color = v.color;
                o.uv = TRANSFORM_TEX(v.uv, _MainTex) + _UVScroll.xy * _Time.y;
                o.ambient = 1;
                if (_LightVolumeTint > 0)
                {
                    float3 worldPos = mul(unity_ObjectToWorld, v.vertex).xyz;
                    o.ambient = lerp(half3(1, 1, 1), max(0, LightVolumeSH_L0(worldPos, float3(0, 0, 0))), _LightVolumeTint);
                }
                #if defined(SOFTPARTICLES_ON)
                    o.projPos = ComputeScreenPos(o.pos);
                    COMPUTE_EYEDEPTH(o.projPos.z);
                #endif
                UNITY_TRANSFER_FOG(o, o.pos);
                return o;
            }

            half4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                half4 c = tex2D(_MainTex, i.uv) * i.color * _TintColor;
                c.rgb *= _Intensity * i.ambient;
                #if defined(SOFTPARTICLES_ON)
                    float sceneZ = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture, UNITY_PROJ_COORD(i.projPos)));
                    c.a *= saturate(_InvFade * (sceneZ - i.projPos.z));
                #endif
                if (_AlphaPremultiply > 0.5) c.rgb *= c.a;
                fixed4 fogColor = _AdditiveFog > 0.5 ? fixed4(0, 0, 0, 0) : unity_FogColor;
                UNITY_APPLY_FOG_COLOR(i.fogCoord, c, fogColor);
                return c;
            }
            ENDCG
        }
    }
    FallBack Off
    CustomEditor "Castlemist.GW2ParticleGUI"
}
