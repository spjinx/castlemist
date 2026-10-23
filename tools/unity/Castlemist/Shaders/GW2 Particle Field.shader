// Castlemist/GW2 Particle Field: a map's ambient particle field (stars, motes, dust), the tile of
// particles GW2 wraps around the camera (docs/research/gw2-particle-fields.md). A port of the
// client's own field shaders, read from Gw2-64.exe (vertex shader type 2 at 0x141d74130, type 1 at
// 0x141d7c230; the pixel shader alike), 2026-10-23:
//   the mesh holds one tile, positions in [0, 1)^3 in GW2 map axes (z = height, up = -Z). Each
//   particle drifts along the field direction, wobbles by the deviation, and is wrapped around the
//   camera; the tile sits ahead of the camera (T / 2 + 65 units along the view). Alpha = opacity x
//   twinkle x the tile-edge fade 1 - (2p - 1)^4, capped at |map z| / 100, on for lifetime and off for
//   period. Colour = 2 x the frame; alpha x the texture's. Soft against the scene over 120 units.
// Vertex streams (ParticleFields.cs builds them): COLOR = (corner u, corner v, seed, opacity),
// TEXCOORD0 = half size (x, y) GW2 units, TEXCOORD1 = (angle, spin rad/s),
// TEXCOORD2 = (frame, corner tag).
// Blend: additive (flag 0x20, MAP_ENV_PFIELD_BLEND; its state 0x141d84b28 read as SrcAlpha One,
// UNPROVEN). Cutouts (clipAA..FC) are not ported: map 3811773 has none.
Shader "Castlemist/GW2 Particle Field"
{
    Properties
    {
        _MainTex ("Texture (texPath)", 2D) = "white" {}
        [Header(PackMapEnvDataPField)]
        _FieldType ("type (1: horizontal wobble, 2: full)", Float) = 2
        _Extent ("extent: tile size, GW2 units", Float) = 54464
        _Deviation ("deviation, GW2 units", Float) = 0
        _Speed ("speed (min, max), deviationSpeed (min, max)", Vector) = (1, 1, 1, 1)
        _Direction ("fieldDirection (normalised, map axes)", Vector) = (0, 0, 0, 0)
        _Life ("period (min, max), lifetime (min, max) s", Vector) = (0, 0, 0, 0)
        _Frames ("frame offset, cols, rows, fps", Vector) = (0, 1, 1, 0)
        _Visibility ("Visibility", Range(0, 1)) = 1
        [Header(Map frame)]
        _UnityScale ("Unity units per GW2 unit", Float) = 0.0254
        [ToggleUI] _SoftFade ("Soft against the scene (needs a depth texture)", Float) = 1
    }
    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" "PreviewType" = "Plane" }
        Pass
        {
            Blend SrcAlpha One
            ZWrite Off
            Cull Off
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.5
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);
            float4 _CameraDepthTexture_TexelSize;
            float _FieldType, _Extent, _Deviation, _Visibility, _UnityScale, _SoftFade;
            float4 _Speed, _Direction, _Life, _Frames;
            // the object is a child of the map root at identity (ParticleFields.cs): its matrices are the map frame

            struct appdata
            {
                float4 vertex : POSITION;
                float4 color : COLOR;
                float2 size : TEXCOORD0;
                float2 spin : TEXCOORD1;
                float2 frame : TEXCOORD2;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct v2f
            {
                float4 pos : SV_POSITION;
                float4 uv : TEXCOORD0;      // (frame u, v, next frame u, v)
                float3 fade : TEXCOORD1;    // (alpha, eye depth, frame blend)
                float4 screen : TEXCOORD2;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            // GW2 map axes (x, y, z) <-> the map root's local frame (x, -z, y)
            float3 MapToLocal(float3 g) { return float3(g.x, -g.z, g.y); }
            float3 LocalToMap(float3 l) { return float3(l.x, l.z, -l.y); }
            // the client's wave: frac -> |2f - 1|^2 (3 - 2 |2f - 1|)
            float3 Wave(float3 f) { float3 w = abs(frac(f) * 2 - 1); return w * w * (3 - 2 * w); }
            float Wave(float f) { return Wave(f.xxx).x; }

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float t = _Time.y, T = _Extent, s = v.color.z;
                float3 p = v.vertex.xyz;
                // fldprta: the camera in tiles, the tile centred T / 2 + 65.05 ahead along the view
                float3 cam = LocalToMap(mul(unity_WorldToObject, float4(_WorldSpaceCameraPos, 1)).xyz);
                float3 fwd = LocalToMap(normalize(mul((float3x3)unity_WorldToObject, -UNITY_MATRIX_V[2].xyz)));
                float3 camT = (cam - T * 0.5 + (T * 0.5 + 65.05) * fwd) / T;
                float r0 = dot(p, p);
                float3 q;
                float tw;
                if (_FieldType < 1.5)
                {
                    // type 1: drift by speed x time x 100 / T, wobble in x and y only
                    float h = r0 * s;
                    float drift = lerp(_Speed.x, _Speed.y, frac(h)) * t * (100 / T);
                    q = p + _Direction.xyz * drift;
                    float ph = h + lerp(_Speed.z, _Speed.w, s) * t * (100 / T);
                    float a = ph + 0.5, b = ph * 0.5;
                    float3 w1 = Wave(float3(a, b, h));
                    float x = ph - w1.x;   // r0.x = ph - wave(a)
                    float3 w2 = Wave(float3(x, a * 2.223, a * 3.223 + 0.1234));
                    q.xy += float2(w1.y * w2.x, w2.y + w2.z) * (_Deviation / T);
                    tw = min(w1.z * _Visibility * 4, 1);
                }
                else
                {
                    // type 2: drift by speed x time x 0.025 tiles, wobble on every axis
                    float h = r0 * s;
                    q = p + _Direction.xyz * (lerp(_Speed.x, _Speed.y, s) * t * 0.025);
                    float ph = lerp(_Speed.z, _Speed.w, frac(h)) * t * (100 / T);
                    float x = r0 * s + ph;
                    float4 w = float4(Wave(float3(x + 0.5, x * 0.5, 2 * x + 0.5)), Wave(h * 100 + ph * 4));
                    q += w.xyz * (_Deviation / T);
                    tw = w.w * w.w; tw *= tw;
                    tw *= _Visibility;
                }
                float3 wrapped = frac(q - camT);
                float3 g = (wrapped + camT) * T;                         // map units
                // billboard: the view's right / up turned by angle + spin x time; size in map units
                float ang = v.spin.x + v.spin.y * t;
                float sn, cs;
                sincos(ang, sn, cs);
                float3 right = UNITY_MATRIX_V[0].xyz, up = UNITY_MATRIX_V[1].xyz;
                float3 ax = (cs * right + sn * up) * v.size.x, ay = (cs * up - sn * right) * v.size.y;
                float2 c = v.color.xy * 2 - 1;
                float3 world = mul(unity_ObjectToWorld, float4(MapToLocal(g), 1)).xyz + (ax * c.x + ay * c.y) * _UnityScale;
                // alpha: opacity x twinkle x the tile-edge fade, capped by height, gated by the cycle
                float3 e = wrapped * 2 - 1; e *= e;
                float3 edge = 1 - e * e;
                float alpha = min(min(edge.x, edge.y), edge.z) * tw * v.color.w;
                float life = lerp(_Life.z, _Life.w, s), period = lerp(_Life.x, _Life.y, s);
                float cycle = (life + period) * 0.999995;
                if (cycle > 0)
                {
                    float ph = frac((s * cycle + t) / cycle);
                    alpha *= 1 - ceil(saturate(ph * cycle - life));
                }
                else alpha = 0;
                alpha = min(abs(g.z) * 0.01, alpha);
                world *= alpha > 0 ? 1 : 0;   // the client collapses an invisible particle
                o.pos = UnityWorldToClipPos(world);
                // flipbook: frame + offset over cols x rows, the next frame for the blend
                float cols = max(_Frames.y, 1), rows = max(_Frames.z, 1);
                float anim = _Frames.w > 0 ? t * _Frames.w : 0;
                float f0 = v.frame.x + _Frames.x + floor(anim), f1 = f0 + 1;
                float2 col = frac(float2(f0, f1) / cols) * cols, row = frac(floor(float2(f0, f1) / cols) / rows) * rows;
                float cu = floor(abs(v.frame.y) + 0.5), cv = floor(v.frame.y + 1.1);
                o.uv = float4((col.x + cu) / cols, 1 - (row.x + 1 - cv) / rows, (col.y + cu) / cols, 1 - (row.y + 1 - cv) / rows);
                o.fade = float3(alpha, -UnityWorldToViewPos(world).z, frac(anim));
                o.screen = ComputeScreenPos(o.pos);
                return o;
            }

            fixed4 frag(v2f i) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float4 a = tex2D(_MainTex, i.uv.xy), b = tex2D(_MainTex, i.uv.zw);
                float4 c = lerp(a, b, i.fade.z);
                float soft = 1;
                if (_SoftFade > 0.5 && _CameraDepthTexture_TexelSize.z > 16)
                {
                    float scene = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture, UNITY_PROJ_COORD(i.screen)));
                    soft = saturate((scene - i.fade.y) / (120 * _UnityScale));
                    soft = soft * soft * (3 - 2 * soft);
                    soft *= soft;
                }
                return float4(c.rgb * 2, c.a * i.fade.x * soft);
            }
            ENDCG
        }
    }
    CustomEditor "Castlemist.GW2ParticleFieldGUI"
}
