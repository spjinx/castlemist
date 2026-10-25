// Castlemist/GW2 Sky Card: one of a map's texture sky cards (environment.skyCards; docs/research/gw2-sky.md
// §9), a port of the client's day card program (blob 0x141d84d30: VS 1673, PS 1670), 2026-10-25:
//   the mesh is the card's quad as the client builds it (0x140ca7280): corners (F, +-1000, +-1000)
//   normalised to length F, then scaled by (1, sx, sy) (GW2SkyCards.cs bakes it; local x = toward the
//   card, y = eL, z = eD). The vertex stage turns it to the card's direction and puts it at the camera:
//     az = azimuth + spin, lat = latitude x pi/2 (§9.2); spin = speed x time, from 0 at load
//     c  = (cos lat cos az, cos lat sin az, -sin lat), eL = (-sin az, cos az, 0),
//     eD = (sin lat cos az, sin lat sin az, cos lat); P = camera + x c + y eL + z eD (map axes)
//   PS 1670 (§9.4), d = camera -> card: col = GW2Sky.cginc's lighting with v = d;
//     e = saturate(-d.z); f = saturate((e - hazeBottom) / hazeFalloff);
//     h = saturate((1 - f^2 (3 - 2f)) card.hazeDensity x sky hazeDensity + minHaze)
//     rgb = lerp(col, FogColorFar, h) brightness; a = T.a density. Blend SrcAlpha OneMinusSrcAlpha.
// Not ported: flag 8 / 0x10 (aim at `location`, scale by its distance) and flags 2 / 4 / 0x80 (hide by
// program or by the UNPROVEN `level`): map 506592's cards carry them with every location (0, 0, 0) and
// flag bytes that read as noise, and the game shows them turning; cardFade = 1; the day set only.
// The object is a child of the map root at identity (GW2SkyCards.cs): its matrices are the map frame.
Shader "Castlemist/GW2 Sky Card"
{
    Properties
    {
        _MainTex ("Texture (card texture, raw)", 2D) = "white" {}
        [Header(PackMapEnvDataSkyCard (day))]
        _Card ("azimuth rad, latitude rad (x pi/2), speed rad/s", Vector) = (0, 0, 0, 0)
        _Attr ("brightness, density, minHaze, lightIntensity x sky LightIntensity", Vector) = (1, 1, 0, 0)
        _Haze ("sky hazeBottom, hazeFalloff, card hazeDensity x sky hazeDensity", Vector) = (0, 1, 0, 0)
        [Header(Sun and fog (environment))]
        _SunDir ("sun direction (travel, map axes)", Vector) = (0, 0, 1, 0)
        [HDR] _SunColor ("sun colour x intensity (gamma)", Color) = (1, 1, 1, 1)
        [HDR] _Ambient ("ambient, the shRed/Green/Blue stand-in (gamma)", Color) = (1, 1, 1, 1)
        [ToggleUI] _SceneSH ("Use Unity's scene SH as the ambient instead", Float) = 0
        _FogFar ("haze farColor (gamma rgb, a)", Color) = (0, 0, 0, 1)
        [Header(Map frame)]
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254
    }
    SubShader
    {
        Tags { "Queue" = "Transparent-50" "RenderType" = "Transparent" "IgnoreProjector" = "True"
               "ForceNoShadowCasting" = "True" "DisableBatching" = "True" "PreviewType" = "Plane" }
        Pass
        {
            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            #pragma multi_compile_instancing
            #include "GW2Sky.cginc"

            sampler2D _MainTex;
            float4 _Card, _Attr, _Haze;

            struct appdata
            {
                float4 vertex : POSITION;   // the card's quad: x toward the card, y = eL, z = eD (GW2 units)
                float2 uv : TEXCOORD0;      // GW2 uv (v down) from textureUV
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;
                float3 d : TEXCOORD1;       // camera -> card, map axes
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float3 cam = GW2SkyCamera();
                float az = _Card.x + _Card.z * _Time.y, lat = _Card.y;
                float sa, ca, sl, cl;
                sincos(az, sa, ca);
                sincos(lat, sl, cl);
                float3 c = float3(cl * ca, cl * sa, -sl);
                float3 eL = float3(-sa, ca, 0);
                float3 eD = float3(sl * ca, sl * sa, cl);
                o.d = v.vertex.x * c + v.vertex.y * eL + v.vertex.z * eD;
                float3 world = mul(unity_ObjectToWorld, float4(MapToLocal(cam + o.d), 1)).xyz;
                o.pos = UnityWorldToClipPos(world);
                o.uv = v.uv;
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float3 d = normalize(i.d);
                float4 T = tex2D(_MainTex, float2(i.uv.x, 1 - i.uv.y));   // GW2's v down on Unity's rows
                GW2SkyLights k = GW2SkyInputs(d);
                float3 col = GW2SkyLit(T.rgb, d, _Attr.w, k);
                float e = saturate(-d.z);
                float f = saturate((e - _Haze.x) / max(_Haze.y, 1e-4));
                float h = saturate((1 - f * f * (3 - 2 * f)) * _Haze.z + _Attr.z);
                float3 rgb = lerp(col, k.fog, h) * _Attr.x;
                return float4(GW2SkyOut(rgb), saturate(T.a * _Attr.y));
            }
            ENDCG
        }
    }
    CustomEditor "Castlemist.GW2SkyCardGUI"
}
