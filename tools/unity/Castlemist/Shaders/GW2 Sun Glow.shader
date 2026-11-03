// Castlemist/GW2 Sun Glow (2026-10-29): a soft halo around the main directional light (the sun, or
// the moon at night), in its colour: a tight core and a wide faint bloom, both smooth exponentials of
// the angle from the light. In place of Silent's Sunshine Flare, whose dithered depth rays read as
// grain and whose lens ghosts did not suit moonlight. The mesh is a quad (corners in uv) that the
// vertex stage turns to face the camera on the light's direction, Distance metres out (inside the far
// clip), so nearer geometry hides it with the ordinary depth test: no depth texture is read.
// ForwardBase gives it _WorldSpaceLightPos0 / _LightColor0, as for Silent's flare.
Shader "Castlemist/GW2 Sun Glow"
{
    Properties
    {
        _Intensity ("Intensity", Range(0, 4)) = 0.6
        _Core ("Core radius (degrees)", Range(0.1, 10)) = 1.2
        _Halo ("Halo radius (degrees)", Range(1, 45)) = 9
        _HaloWeight ("Halo weight (to the core)", Range(0, 1)) = 0.25
        _Saturation ("Colour saturation", Range(0, 2)) = 0.6
        _Distance ("Distance (metres, clamped inside the far clip)", Float) = 3000
    }
    SubShader
    {
        Tags { "Queue" = "Transparent-40" "RenderType" = "Transparent" "IgnoreProjector" = "True"
               "ForceNoShadowCasting" = "True" "DisableBatching" = "True" "PreviewType" = "Plane" }
        Pass
        {
            Tags { "LightMode" = "ForwardBase" }
            Blend One One
            ZWrite Off
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"
            #include "UnityLightingCommon.cginc"

            float _Intensity, _Core, _Halo, _HaloWeight, _Saturation, _Distance;

            struct appdata
            {
                float4 vertex : POSITION;
                float2 uv : TEXCOORD0;      // -1..1 corners
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float3 dir : TEXCOORD0;     // camera -> this point, world
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float3 L = normalize(_WorldSpaceLightPos0.xyz);   // toward the light
                float3 up = abs(L.y) < 0.99 ? float3(0, 1, 0) : float3(1, 0, 0);
                float3 r = normalize(cross(up, L)), u = cross(L, r);
                float d = min(_Distance, _ProjectionParams.z * 0.9);
                float extent = d * tan(radians(min(_Halo * 3.0, 80.0)));   // the halo has faded by 3 radii
                float3 world = _WorldSpaceCameraPos + L * d + (r * v.uv.x + u * v.uv.y) * extent;
                o.dir = world - _WorldSpaceCameraPos;
                o.pos = UnityWorldToClipPos(world);
                if (dot(_LightColor0.rgb, 1) <= 0) o.pos = 0;   // no main light
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float3 L = normalize(_WorldSpaceLightPos0.xyz);
                float a = degrees(acos(saturate(dot(normalize(i.dir), L))));
                float g = exp(-a / max(_Core, 1e-3)) + _HaloWeight * exp(-a / max(_Halo, 1e-3));
                float w = saturate(1.0 - a / min(_Halo * 3.0, 80.0));
                g *= w * w;   // to 0 at the quad's edge, no visible border
                float3 c = _LightColor0.rgb;
                float grey = dot(c, float3(0.2126, 0.7152, 0.0722));
                c = lerp(grey.xxx, c, _Saturation);
                return fixed4(c * g * _Intensity, 0);
            }
            ENDCG
        }
    }
}
