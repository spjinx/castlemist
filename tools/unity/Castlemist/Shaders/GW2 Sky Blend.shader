// Castlemist/GW2 Sky Blend (2026-10-25): the skybox as the exported day and night bakes
// (sky/<mode>/baked/equirect.png), lat-long like Skybox/Panoramic, lerp(night, day, _DayWeight):
// GW2 lerps every sky parameter by the one day weight t (gw2-sky.md §3); a lerp of the two bakes
// stands in for re-evaluating the sky at t (castlemist's choice). GW2DayNight drives _DayWeight.
Shader "Castlemist/GW2 Sky Blend"
{
    Properties
    {
        [NoScaleOffset] _DayTex ("Day (equirect)", 2D) = "grey" {}
        [NoScaleOffset] _NightTex ("Night (equirect)", 2D) = "black" {}
        _DayWeight ("Day weight t (1 day, 0 night)", Range(0, 1)) = 1
        _Exposure ("Exposure", Float) = 1
        _Rotation ("Rotation (degrees)", Float) = 0
    }
    SubShader
    {
        Tags { "Queue" = "Background" "RenderType" = "Background" "PreviewType" = "Skybox" }
        Cull Off ZWrite Off
        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            sampler2D _DayTex, _NightTex;
            float _DayWeight, _Exposure, _Rotation;
            struct appdata { float4 vertex : POSITION; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2f { float4 pos : SV_POSITION; float3 dir : TEXCOORD0; UNITY_VERTEX_OUTPUT_STEREO };
            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float a = radians(_Rotation), s = sin(a), c = cos(a);
                float3 p = v.vertex.xyz;
                o.pos = UnityObjectToClipPos(float3(c * p.x - s * p.z, p.y, s * p.x + c * p.z));
                o.dir = p;
                return o;
            }
            fixed4 frag(v2f i) : SV_Target
            {
                float3 d = normalize(i.dir);
                // Skybox/Panoramic's ToRadialCoords: longitude atan2(z, x), latitude acos(y)
                float2 uv = float2(0.5 - atan2(d.z, d.x) / (2 * UNITY_PI), 1 - acos(clamp(d.y, -1, 1)) / UNITY_PI);
                half3 day = tex2Dlod(_DayTex, float4(uv, 0, 0)).rgb;
                half3 night = tex2Dlod(_NightTex, float4(uv, 0, 0)).rgb;
                return half4(lerp(night, day, _DayWeight) * _Exposure, 1);
            }
            ENDCG
        }
    }
    Fallback Off
}
