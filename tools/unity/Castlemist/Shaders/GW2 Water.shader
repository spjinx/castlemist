// Castlemist/GW2 Water (2026-10-25): the map's water plane, the castlemist Map Window's water
// (world_view_shaders.h kWater) in Unity. Not the game's shader (AMAT 187551 needs scene depth, a
// scene colour copy and a reflection target, gw2-world-materials.md §6); a reconstruction from the
// env water preset with the Map Window's UNPROVEN units:
//   base     _MainTex, the water bake (one map-wide texture at _MapRect, GW2Water): rgb
//            lerp(shallow, deep, depth mix), alpha
//            0.45 + 0.55 x the mix (terrain_bake.cpp)
//   bump     two samples of _BumpTex at uv_k = R(angle_k) mapXY x tile_k / _TileUnit + (speed_k
//            _SpeedScale t, 0) (water_bump_uv's form; mapXY = world xz / _UnityScale, angles degrees),
//            xy x 2 - 1. 2026-10-25: the Map Window's units (1000 map units per tile, speed in tiles
//            per second) tiled and raced in VRChat; 4000 and x 0.02 are castlemist's looks;
//            N = normalize((n0 + n1) x _BumpAmount, up)
//   light    base x (ambient SH + sun N.L) + sun x (N.H)^_SpecPower, + the reflection probe along
//            reflect(-V, N) x fresnel, fresnel = (1 - N.V)^_Fresnel
//   alpha    lerp(lerp(0.45, 0.9, mix), 1, fresnel)
//   haze     2026-10-25: the GW2 Haze volume's formula (GW2 Haze.shader) at the water's own pixel,
//            colour x (1 - fog.a) + fog.rgb, with the haze material's values (GW2Water copies
//            them, GW2DayNight lerps them): the volume reads the depth texture, which the water
//            (transparent, no depth write) is not in, so it fogged the sea bed's distance instead
Shader "Castlemist/GW2 Water"
{
    Properties
    {
        _MainTex ("Water bake (rgb colour, a depth)", 2D) = "white" {}
        _MapRect ("Bake rect (map x0, y0, width, height; 0: the mesh UV)", Vector) = (0, 0, 0, 0)
        _BumpTex ("Bump (preset textureFilenames[1])", 2D) = "bump" {}
        _BumpTile ("Bump tiles (x, y: bumpTile0 / 1)", Vector) = (5, 13.6, 0, 0)
        _BumpSpeed ("Bump speeds (x, y: bumpSpeed0 / 1)", Vector) = (1.3, 0.64, 0, 0)
        _BumpAngle ("Bump angles, degrees (x, y)", Vector) = (0, 0, 0, 0)
        _BumpAmount ("Bump strength (bumpAmount x 0.02)", Float) = 0.1
        _TileUnit ("Map units per tile", Float) = 4000
        _SpeedScale ("Speed scale (tiles per second per bumpSpeed)", Float) = 0.02
        _UnityScale ("Unity metres per map unit", Float) = 0.0254
        _Fresnel ("Fresnel power (surfaceFresnel)", Float) = 3.3
        _SpecPower ("Sun specular power", Float) = 64
        _Reflection ("Reflection probe strength", Range(0, 1)) = 0.6
        [Header(GW2 haze copied from the haze material by HazeCopy)]
        [ToggleUI] _UseHaze ("Apply the GW2 haze", Float) = 0
        _HazeNear ("nearColor", Color) = (0.6, 0.65, 0.7, 1)
        _HazeFar ("farColor", Color) = (0.6, 0.65, 0.7, 1)
        _HazeHeight ("heightColor", Color) = (0.6, 0.65, 0.7, 0)
        _HazeDistRange ("distRange (d0, d1) GW2 units", Vector) = (2000, 40000, 0, 0)
        _HazeHeightRange ("heightRange (h0, h1)", Vector) = (0, -2000, 0, 0)
        _HazeDepthCue ("depthCue", Float) = 0.001
        _HazeMapOriginY ("Map root world Y", Float) = 0
        _HazeUnityScale ("Unity units per GW2 unit", Float) = 0.0254
        _HazeAmount ("Haze amount", Range(0, 2)) = 1
    }
    SubShader
    {
        Tags { "Queue" = "Transparent-100" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
        Pass
        {
            Tags { "LightMode" = "ForwardBase" }
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"
            #include "Lighting.cginc"
            #include "UnityStandardUtils.cginc"
            #include "GW2HazeFog.cginc"

            sampler2D _MainTex; float4 _MainTex_ST;
            sampler2D _BumpTex;
            float4 _BumpTile, _BumpSpeed, _BumpAngle;
            float4 _MapRect;
            float _BumpAmount, _TileUnit, _SpeedScale, _UnityScale, _Fresnel, _SpecPower, _Reflection;

            struct appdata { float4 vertex : POSITION; float2 uv : TEXCOORD0; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 wp : TEXCOORD1;
                UNITY_FOG_COORDS(2)
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = TRANSFORM_TEX(v.uv, _MainTex);
                o.wp = mul(unity_ObjectToWorld, v.vertex).xyz;
                UNITY_TRANSFER_FOG(o, o.pos);
                return o;
            }

            float2 BumpUV(float2 xy, float tile, float speed, float angleDeg)
            {
                float a = radians(angleDeg), c = cos(a), s = sin(a);
                float k = _TileUnit > 0 ? tile / _TileUnit : 0;
                return float2((c * xy.x - s * xy.y) * k + frac(speed * _SpeedScale * _Time.y), (s * xy.x + c * xy.y) * k);
            }

            fixed4 frag(v2f i) : SV_Target
            {
                float2 xy = i.wp.xz / max(_UnityScale, 1e-6);
                // one map-wide bake (GW2Water): looked up by position; else the mesh's own UV
                fixed4 bake = tex2D(_MainTex, _MapRect.z > 0 ? (xy - _MapRect.xy) / _MapRect.zw : i.uv);
                float mixD = saturate((bake.a - 0.45) / 0.55);
                float2 n0 = tex2D(_BumpTex, BumpUV(xy, _BumpTile.x, _BumpSpeed.x, _BumpAngle.x)).xy * 2 - 1;
                float2 n1 = tex2D(_BumpTex, BumpUV(xy, _BumpTile.y, _BumpSpeed.y, _BumpAngle.y)).xy * 2 - 1;
                float2 nb = (n0 + n1) * _BumpAmount;
                float3 N = normalize(float3(nb.x, 1, nb.y));   // GW2 (x, y, -1), up -Z -> Unity (x, 1, y)
                float3 V = normalize(_WorldSpaceCameraPos - i.wp);
                float3 L = normalize(_WorldSpaceLightPos0.xyz);
                float3 H = normalize(L + V);
                float fresnel = pow(max(1 - saturate(dot(N, V)), 1e-6), max(_Fresnel, 0));
                float3 ambient = ShadeSH9(float4(N, 1));
                float3 col = bake.rgb * (ambient + _LightColor0.rgb * saturate(dot(N, L)))
                           + _LightColor0.rgb * pow(saturate(dot(N, H)), _SpecPower);
                half4 probe = UNITY_SAMPLE_TEXCUBE(unity_SpecCube0, reflect(-V, N));
                col += DecodeHDR(probe, unity_SpecCube0_HDR) * fresnel * _Reflection;
                fixed4 o = fixed4(col, lerp(lerp(0.45, 0.9, mixD), 1, fresnel));
                if (!CM_ApplyGW2Haze(o, i.wp)) UNITY_APPLY_FOG(i.fogCoord, o);   // far water: the haze, not the fogged sea bed
                return o;
            }
            ENDCG
        }
    }
    Fallback Off
}
