// Castlemist/GW2 Cloud: one of a map's cloud layers (environment.clouds; docs/research/gw2-sky.md §10),
// a port of the client's cloud program (blob 0x141d72f80: VS 1645, PS 1644), 2026-10-25:
//   the mesh is the client's 11 x 11 grid (0x140ca3290): x, y in [-0.5, 0.5] (stored as local x, z),
//   vertex alpha 1 - min(r / 0.5, 1)^4. The vertex stage centres it under the camera at half-size F m
//   (Sw = 2 F m, m = 0.5925 + 0.4075 extent) and the absolute height z = -altitude (§10.2), so it is
//   drawn from above and below alike. UVs are anchored to the world and scroll with velocity / Sw.
//   VS 1645: n = normalize(camera - P); vertical fade vf = saturate((|cam.z - P.z| - fadeEnd) /
//   (fadeEnd + fadeWidth))^2; fog = min((1 - fz)(near.w - far.w) + far.w, fz) haze, fz =
//   saturate((viewZ - d0) / (d1 - d0)) over the haze's distRange (§A.6).
//   PS 1644: parallax uv2 = uv + (tex(uv).a - 0.5) depth n.xy; T = tex(uv2); the §9.4 lighting with n;
//   rgb = lerp(col, FogColorFar, fog) brightness; a = vertex alpha x density x T.a x soft x vf, soft =
//   smoothstep(saturate((scene depth - depth) / cutOut)). Blend SrcAlpha OneMinusSrcAlpha.
// The lighting, its UNPROVEN sun / ambient inputs, the gamma maths and the output clamp are GW2Sky.cginc's
// (2026-10-25: map 506592's upper layer, brightness 5 and lightIntensity -3, gives the game's gold streaks
// only with a light near white; Unity's scene SH turned it black, unclamped HDR bloomed it white).
// cloudFade = 1; attribute set 0 (day); the camera.z > level set and the layer order are not used. The
// texture is imported raw (sRGB off).
// The object is a child of the map root at identity (GW2Clouds.cs): its matrices are the map frame.
Shader "Castlemist/GW2 Cloud"
{
    Properties
    {
        _MainTex ("Texture (layer texture, raw)", 2D) = "white" {}
        [Header(PackMapEnvDataLayer)]
        _Altitude ("altitude, GW2 units (plane at z = -altitude)", Float) = 0
        _HeightOffset ("Height offset, Unity units (castlemist; 0 = GW2)", Float) = 0
        _Plane ("skyDistance F, m, scale, depth", Vector) = (24576, 1, 1, 0)
        _CutOut ("cutOut: soft-edge depth, GW2 units", Float) = 1000
        _Fade ("fadeEnd, fadeWidth, GW2 units", Vector) = (16, 33, 0, 0)
        [Header(Attributes (set 0 day))]
        _Attr ("brightness, density, haze, lightIntensity x sky LightIntensity", Vector) = (1, 1, 1, 1)
        _Velocity ("velocity (x, y), GW2 units/s", Vector) = (0, 0, 0, 0)
        [Header(Sun and fog (environment))]
        _SunDir ("sun direction (travel, map axes)", Vector) = (0, 0, 1, 0)
        [HDR] _SunColor ("sun colour x intensity (gamma)", Color) = (1, 1, 1, 1)
        [HDR] _Ambient ("ambient, the shRed/Green/Blue stand-in (gamma)", Color) = (1, 1, 1, 1)
        [ToggleUI] _SceneSH ("Use Unity's scene SH as the ambient instead", Float) = 0
        _FogFar ("haze farColor (gamma rgb, a)", Color) = (0, 0, 0, 1)
        _FogDist ("haze distRange (d0, d1) GW2 units, nearColor.w", Vector) = (2000, 40000, 1, 0)
        _FogAmount ("Fog amount (castlemist; 1 = GW2)", Range(0, 1)) = 1
        [Header(Map frame)]
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254
        [ToggleUI] _SoftFade ("Soft against the scene (needs a depth texture)", Float) = 1
    }
    SubShader
    {
        Tags { "Queue" = "Transparent-30" "RenderType" = "Transparent" "IgnoreProjector" = "True"
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
            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);
            float4 _CameraDepthTexture_TexelSize;
            float _Altitude, _HeightOffset, _CutOut, _FogAmount, _SoftFade;
            float4 _Plane, _Fade, _Attr, _Velocity, _FogDist;

            struct appdata
            {
                float4 vertex : POSITION;   // grid (x, 0, y), x, y in [-0.5, 0.5]
                float4 color : COLOR;       // a = 1 - min(r / 0.5, 1)^4
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float2 uv : TEXCOORD0;      // GW2 uv (v down)
                float3 n : TEXCOORD1;       // camera - P, map axes
                float3 fade : TEXCOORD2;    // (vertex alpha x vf, fog, eye depth Unity units)
                float4 screen : TEXCOORD3;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float3 cam = GW2SkyCamera();
                float F = _Plane.x, m = _Plane.y, k = m * _Plane.z;
                float Sw = max(2 * F * m, 1e-3);
                float2 g = v.vertex.xz;
                // World = [Sw 0 0 cam.x; 0 Sw 0 cam.y; 0 0 1 -altitude] (§10.2)
                float3 P = float3(g * Sw + cam.xy, -(_Altitude + _HeightOffset / _UnityScale));
                // uv = (meshUV + cam.xy / Sw + scroll) m scale; scroll += velocity dt / Sw from 0;
                // the offset taken mod 1 in texture space (the texture repeats) to keep it small
                float2 scroll = _Velocity.xy * _Time.y / Sw;
                o.uv = (g + 0.5) * k + frac((cam.xy / Sw + scroll) * k);
                float3 world = mul(unity_ObjectToWorld, float4(MapToLocal(P), 1)).xyz;
                o.pos = UnityWorldToClipPos(world);
                o.n = cam - P;
                float vf = saturate((abs(cam.z - P.z) - _Fade.x) / max(_Fade.x + _Fade.y, 1e-3));
                float viewZ = -UnityWorldToViewPos(world).z;
                float fz = saturate((viewZ / _UnityScale - _FogDist.x) / max(_FogDist.y - _FogDist.x, 1e-3));
                float fog = min((1 - fz) * (_FogDist.z - _FogFar.a) + _FogFar.a, fz) * _Attr.z * _FogAmount;
                o.fade = float3(v.color.a * vf * vf, fog, viewZ);
                o.screen = ComputeScreenPos(o.pos);
                return o;
            }

            // GW2's uv (v down) on Unity's texture (rows bottom-up)
            float4 Tex(float2 uv) { return tex2D(_MainTex, float2(uv.x, 1 - uv.y)); }

            fixed4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float3 n = normalize(i.n);
                float a0 = Tex(i.uv).a;
                float4 T = Tex(i.uv + (a0 - 0.5) * _Plane.w * n.xy);
                // §9.4 with d replaced by n
                GW2SkyLights k = GW2SkyInputs(n);
                float3 col = GW2SkyLit(T.rgb, n, _Attr.w, k);
                float3 rgb = lerp(col, k.fog, i.fade.y) * _Attr.x;
                float soft = 1;
                if (_SoftFade > 0.5 && _CameraDepthTexture_TexelSize.z > 16)
                {
                    float scene = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture, UNITY_PROJ_COORD(i.screen)));
                    soft = saturate((scene - i.fade.z) / max(_CutOut * _UnityScale, 1e-4));
                    soft = soft * soft * (3 - 2 * soft);
                }
                float a = saturate(i.fade.x * _Attr.y * T.a * soft);
                return float4(GW2SkyOut(rgb), a);
            }
            ENDCG
        }
    }
    CustomEditor "Castlemist.GW2CloudGUI"
}
