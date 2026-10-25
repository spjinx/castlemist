// Castlemist/GW2 Haze: GW2's map haze (distance + height fog) as a screen-space fog volume.
//
// Based on Silent's SkyProbe Fog 1.1 (https://gitlab.com/s-ilent/skyprobe-fog, MIT licence:
// Copyright (c) Silent; the depth reconstruction, the oblique-frustum correction by Dj Lukis.LT and
// the probe colour are its). The fog maths are replaced by GW2's own: the engine's VSCalculateFog
// (docs/research/gw2-sky.md A.2; its HLSL source is text in Gw2-64.exe: fogs = 1 - saturate(pow(2,
// -fogs)), base 2, 2026-10-23), per pixel at the scene point behind the volume:
//   dist   = |p - camera| (GW2 units), z = map height (up = -Z: z = -y in the Unity frame)
//   dFog   = 1 - min(2^(-(dist - d0) / (d1 - d0)), 1)               distRange (d0, d1)
//   hFog   = 1 - min(2^(-(z - h0) / (h1 - h0)), 1)                  heightRange (h0, h1)
//   hAmt   = min(hFog, height.a) x min(dist x depthCue, 1)
//   dCol   = far + (near - far) (1 - dFog), dAmt = min(dFog, far.a + (near.a - far.a)(1 - dFog))
//   fog    = (height.rgb hAmt (1 - dAmt) + dCol dAmt,  hAmt (1 - dAmt) + dAmt)
//   colour = scene (1 - fog.a) + fog.rgb       (Blend One OneMinusSrcAlpha)
// GW2 skips the sky (its sky draws carry their own haze), as does this by default.
//
// Use: a cube with this material, Cull Front, covering the map (the importer makes it). It fogs
// everything drawn before it, avatars included, so Unity's own fog should be off. It needs the
// camera depth texture: in VRChat a directional light with shadows (the GW2 sun has them).
Shader "Castlemist/GW2 Haze"
{
    Properties
    {
        [Header(GW2 haze PackMapEnvDataHaze)]
        _HazeNear ("nearColor (rgb, a)", Color) = (0.6, 0.65, 0.7, 1)
        _HazeFar ("farColor (rgb, a)", Color) = (0.6, 0.65, 0.7, 1)
        _HazeHeight ("heightColor (rgb, a = max amount)", Color) = (0.6, 0.65, 0.7, 0)
        _DistRange ("distRange (d0, d1) GW2 units", Vector) = (2000, 40000, 0, 0)
        _HeightRange ("heightRange (h0, h1) map z, GW2 units", Vector) = (0, -2000, 0, 0)
        _DepthCue ("depthCue (per GW2 unit)", Float) = 0.001
        [Header(Map frame)]
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254
        _MapOriginY ("Map root world Y", Float) = 0
        _Amount ("Amount", Range(0, 2)) = 1
        [Header(Silent probe scattering)]
        _ProbeBlend ("Tint the fog by the reflection probe", Range(0, 1)) = 0
        _BlurLevel ("Probe blur level", Range(0, 7)) = 4
        [Header(System)]
        [ToggleUI] _ApplySkybox ("Don't fog the skybox", Float) = 1
        [Enum(UnityEngine.Rendering.CullMode)] _CullMode ("Cull", Float) = 1
    }
    SubShader
    {
        Tags
        {
            "Queue" = "Transparent-216"
            "RenderType" = "Custom"
            "ForceNoShadowCasting" = "True"
            "IgnoreProjector" = "True"
            "DisableBatching" = "True"
        }
        ZWrite Off
        ZTest Always
        Cull [_CullMode]
        Blend One OneMinusSrcAlpha

        Pass
        {
            Tags { "LightMode" = "Always" }
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            half4 _HazeNear, _HazeFar, _HazeHeight;
            float4 _DistRange, _HeightRange;
            float _DepthCue, _UnityScale, _MapOriginY, _Amount, _ProbeBlend, _BlurLevel, _ApplySkybox;

            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);

            struct appdata
            {
                float4 vertex : POSITION;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float4 depthTextureUv : TEXCOORD0;
                float4 rayFromCamera : TEXCOORD1;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            // Dj Lukis.LT's oblique view frustum correction (VRChat mirrors use such frusta)
            // https://github.com/lukis101/VRCUnityStuffs/blob/master/Shaders/DJL/Overlays/WorldPosOblique.shader
            inline float4 CalculateObliqueFrustumCorrection()
            {
                float x1 = -UNITY_MATRIX_P._31 / (UNITY_MATRIX_P._11 * UNITY_MATRIX_P._34);
                float x2 = -UNITY_MATRIX_P._32 / (UNITY_MATRIX_P._22 * UNITY_MATRIX_P._34);
                return float4(x1, x2, 0, UNITY_MATRIX_P._33 / UNITY_MATRIX_P._34 + x1 * UNITY_MATRIX_P._13 + x2 * UNITY_MATRIX_P._23);
            }
            inline float CorrectedLinearEyeDepth(float z, float correctionFactor)
            {
                return 1.f / (z / UNITY_MATRIX_P._34 + correctionFactor);
            }
            float SceneZDefaultValue()
            {
                #if UNITY_REVERSED_Z
                    return 0.f;
                #else
                    return 1.f;
                #endif
            }

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float4 worldPosition = mul(unity_ObjectToWorld, v.vertex);
                o.pos = mul(UNITY_MATRIX_VP, worldPosition);
                o.depthTextureUv = ComputeGrabScreenPos(o.pos);
                o.rayFromCamera.xyz = worldPosition.xyz - _WorldSpaceCameraPos.xyz;
                o.rayFromCamera.w = dot(o.pos, CalculateObliqueFrustumCorrection());
                // The cube's own depth means nothing (ZTest Always): every vertex at mid-depth, so a
                // far plane nearer than the cube's faces (VRChat's 1000 against a 2300-unit cube) does
                // not clip the haze away and leave it in the corners of the view only. A vertex behind
                // the camera (w < 0) stays outside the clip range as before.
                o.pos.z = 0.5 * o.pos.w;
                return o;
            }

            // GW2's VSCalculateFog (see the header), distances in GW2 units.
            half4 GW2Fog(float dist, float z)
            {
                float dFog = 1.0 - min(exp2(-(dist - _DistRange.x) / max(_DistRange.y - _DistRange.x, 1e-3)), 1.0);
                float hSpan = _HeightRange.y - _HeightRange.x;
                hSpan = abs(hSpan) < 1e-3 ? (hSpan < 0 ? -1e-3 : 1e-3) : hSpan;
                float hFog = 1.0 - min(exp2(-(z - _HeightRange.x) / hSpan), 1.0);
                float hAmt = min(hFog, _HazeHeight.a) * min(dist * _DepthCue, 1.0);
                half4 dCol = _HazeFar + (_HazeNear - _HazeFar) * (1.0 - dFog);
                float dAmt = min(dFog, dCol.a);
                half3 rgb = _HazeHeight.rgb * hAmt * (1.0 - dAmt) + dCol.rgb * dAmt;
                return half4(rgb, hAmt * (1.0 - dAmt) + dAmt);
            }

            half4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float perspectiveDivide = 1.f / i.pos.w;
                float4 rayFromCamera = i.rayFromCamera * perspectiveDivide;
                float2 depthTextureUv = i.depthTextureUv.xy * perspectiveDivide;

                float sceneZ = SAMPLE_DEPTH_TEXTURE(_CameraDepthTexture, depthTextureUv);
                if (sceneZ == SceneZDefaultValue())
                {
                    clip(-_ApplySkybox);   // the sky: GW2 hazes it in the sky draws, not here
                    sceneZ = 0;
                }
                float sceneDepth = CorrectedLinearEyeDepth(sceneZ, rayFromCamera.w);   // already divided above
                float3 worldPosition = rayFromCamera.xyz * sceneDepth + _WorldSpaceCameraPos.xyz;

                float s = max(_UnityScale, 1e-6);
                float dist = length(worldPosition - _WorldSpaceCameraPos.xyz) / s;
                float z = -(worldPosition.y - _MapOriginY) / s;   // map z, up = -Z
                half4 fog = GW2Fog(dist, z);
                fog = saturate(fog * _Amount);

                if (_ProbeBlend > 0)
                {
                    // Silent's scattering: the reflection probe along the view ray tints the fog.
                    half4 p = UNITY_SAMPLE_TEXCUBE_LOD(unity_SpecCube0, rayFromCamera.xyz, _BlurLevel);
                    half3 probe = DecodeHDR(p, unity_SpecCube0_HDR);
                    fog.rgb = lerp(fog.rgb, probe * fog.a, _ProbeBlend);
                }
                return fog;   // premultiplied: scene (1 - a) + rgb
            }
            ENDCG
        }
    }
    CustomEditor "Castlemist.GW2HazeGUI"
}
