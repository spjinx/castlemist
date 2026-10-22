// castlemist: GW2's screen-distortion effects for VRChat (built-in pipeline). The one exception to
// "one shader": a GrabPass cannot be switched off per material, so it lives here, and only these
// materials pay for the grab (one named grab, _CastlemistGrab, shared by all of them per frame).
// Ported from the game's DXBC (named uniforms):
//   offset = (map(uv).xy x 2 - 1) x distort (+ distort with _DistortBias, 50159, 49659, 63923)
//            x reflect(view, N).xy (_ViewReflect; not 339341) x mask(uv).R (_MaskOffset, 339341)
//            x a distance fade (_UseLit, 630598) in screen UV; a second map's normal is blended
//            in with _UseMap2 (47396)
//   the offset is dropped where a depth tap at the offset is in front of the surface (the game's
//   four-tap test; _DepthReject; needs the camera depth texture, otherwise nothing is rejected)
//   colour = grab(screen uv + offset) x _Tint (592660, 27304, 49659, 63923: modxcol x 2)
//            x vertex colour x 2 (_UseVertexColor, 709206)
//   alpha  = 1 (50159, 339341, 49659), or alphaMask(uv) with AlphaRef discard (47396, 63923 mask.G,
//            57026 mask.R, 709206: x vertex alpha), or the GW2 step of mask.G (stepmin, stepcol)
//            x ramp(|N.V|) x soft depth fade (592660, 27304)
// One lit path (the sun is Unity's main directional light, unshadowed: GW2's VSM is not ported;
// fres = 1 - N.V; the normal is the distortion map in the mesh tangent frame, the geometric
// normal where the mesh has no tangents):
//   lit glass (_UseLit, 630598): c = diffuse x grab x 2; alpha = saturate(2 x diffuse.a),
//            r = gloss = saturate(2 x diffuse.a - 1)
//   glass (_Glass 1, 49659 / 63923): c = grab x modxcol x 2; with a decal (_UseMainTex, the
//            same map slot) c = lerp(c', decal.rgb x (SH or Light Volumes + sun x N.L),
//            saturate(2 decal.a)) after the sun step below, r = min(1, 1 - saturate(2a) + saturate(2a - 1))
//   both:    c = lerp(c x (1 - 0.3 fres^6), saturate(c x sun x 1.5), saturate(0.25 (1 - N.L)(1 - V.L)))
//            c = lerp(c, probe(reflect(-V, N)), r x fres^1.5) + sun x _GlassSpec x pow(N.H, 256) x r
//   water (_Glass 2, 57026): nA = lerp(z, map2(uv1), mask.B), nB = lerp(z, map(uv0), mask.G);
//            A = grab(uv - (nA.xy + 1) distort), B = grab(uv + (nB.xy + 1) distort); n = 2 s
//            (s.z / s.s) - z, s = nA + nB; c = lerp(B, A, fres^1.5); c = lerp(c, probe, 0.25 fres^1.5)
//            + sun x speccp.rgb x 2 x pow(N.H, speccp.w); alpha = mask.R
// Map convention: an x-<role> data map is GW2's RG as stored; the exporter's "Normal" is a Unity
// normal map (green flipped; _DistortIsNormal / _DistortTex2IsNormal). Screen offsets use GW2's
// raw xy, lighting uses Unity's tangent convention (raw green flipped).

Shader "Castlemist/GW2 Distort"
{
    Properties
    {
        [NoScaleOffset] _DistortTex ("Distortion / normal map (RG, linear)", 2D) = "bump" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _DistortUV ("Distortion UV", Float) = 0
        [ToggleUI] _DistortIsNormal ("Distortion map is a Unity normal map", Float) = 0
        [ToggleUI] _UseMap2 ("Blend a second normal map", Float) = 0
        [NoScaleOffset] _DistortTex2 ("Second normal map (RG, linear)", 2D) = "bump" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _DistortUV2 ("Second map UV", Float) = 1
        [ToggleUI] _DistortTex2IsNormal ("Second map is a Unity normal map", Float) = 0
        _Distort ("distort (strength)", Float) = 0.05
        [ToggleUI] _DistortBias ("Add distort as a bias (50159)", Float) = 0
        [ToggleUI] _ViewReflect ("Offset x view reflection", Float) = 1
        [ToggleUI] _MaskOffset ("Offset x mask R (339341)", Float) = 0
        [ToggleUI] _DepthReject ("Keep foreground out (depth test)", Float) = 1
        [HDR] _Tint ("Tint (modxcol x 2)", Color) = (1, 1, 1, 1)
        [ToggleUI] _UseVertexColor ("Colour x vertex colour x 2, alpha x vertex alpha", Float) = 0

        [Header(Alpha)]
        [Enum(Opaque, 0, Alpha mask, 1, Stepped mask, 2)] _AlphaSource ("Alpha", Float) = 0
        [NoScaleOffset] _MaskTex ("Mask (linear)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _MaskUV ("Mask UV", Float) = 1
        [Enum(R, 0, G, 1, B, 2, A, 3)] _MaskChannel ("Mask channel", Float) = 0
        _Cutoff ("AlphaRef", Range(0, 1)) = 0.004
        _StepMin ("stepmin", Range(0, 0.999)) = 0.75
        _StepCol ("stepcol (x scale, w offset)", Vector) = (1, 1, 1, 1)
        [ToggleUI] _UseRamp ("View-angle ramp", Float) = 0
        [NoScaleOffset] _RampTex ("Ramp (R at |N.V|, linear)", 2D) = "white" {}
        [ToggleUI] _SoftFade ("Soft depth fade", Float) = 0
        _DepDist ("depdist (Unity units)", Float) = 0.4

        [Header(Lit glass (630598))]
        [ToggleUI] _UseLit ("Lit glass (diffuse, sun, reflection probe)", Float) = 0
        [NoScaleOffset] _MainTex ("Diffuse or decal (RGB colour, A coverage and gloss)", 2D) = "white" {}
        [Enum(UV0, 0, UV1, 1, UV2, 2, UV3, 3)] _MainUV ("Diffuse UV", Float) = 0
        [ToggleUI] _UseMainTex ("Diffuse from the map (else the colour below)", Float) = 0
        _LitDiffuse ("Diffuse when no map (A 0.5: opaque, no gloss)", Color) = (0.5, 0.5, 0.5, 0.5)
        _LitFadeDist ("Distortion fade distance (Unity units)", Float) = 77.9
        _SunScale ("Sun colour scale", Float) = 1

        [Header(Glass and water (49659 63923 57026))]
        [Enum(Off, 0, Glass, 1, Water, 2)] _Glass ("Refracting glass or water", Float) = 0
        _GlassSpec ("Specular colour (water: speccp)", Color) = (1, 1, 1, 1)
        _GlassSpecPower ("Specular power (256; water: speccp.w)", Float) = 256
        [ToggleUI] _GlassSpecDouble ("Specular x2 (water)", Float) = 0
        _ReflectionStrength ("Reflection probe strength", Float) = 1
        [ToggleUI] _LightVolumes ("Use Light Volumes (glass decal)", Float) = 1
        _LightVolumesBias ("Light Volumes bias (along the normal)", Float) = 0
        _PointLightVolumesShading ("Point Light Volumes shading", Float) = 2

        [Header(UV animation (scroll u v per second, rotate radians per second))]
        _UVAnim0 ("UV0", Vector) = (0, 0, 0, 0)
        _UVAnim1 ("UV1", Vector) = (0, 0, 0, 0)
        _UVAnim2 ("UV2", Vector) = (0, 0, 0, 0)
        _UVAnim3 ("UV3", Vector) = (0, 0, 0, 0)

        [Header(Render state)]
        [Enum(UnityEngine.Rendering.BlendMode)] _SrcBlend ("Src blend", Float) = 1
        [Enum(UnityEngine.Rendering.BlendMode)] _DstBlend ("Dst blend", Float) = 0
        [Enum(Off, 0, On, 1)] _ZWrite ("ZWrite", Float) = 0
        [Enum(UnityEngine.Rendering.CullMode)] _Cull ("Cull", Float) = 2
    }

    SubShader
    {
        // After the opaque scene and the other transparents' queue start: the grab sees them.
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" }

        GrabPass { "_CastlemistGrab" }

        Pass
        {
            // ForwardBase (was Always): Unity fills _WorldSpaceLightPos0 / _LightColor0 with the
            // main directional light and the SH only for forward passes; the lit blocks read them.
            // Still one draw per object; no fwdbase multi_compile (no shadows, no vertex lights:
            // GW2's VS point lights are left to Light Volumes).
            Tags { "LightMode" = "ForwardBase" }
            Blend [_SrcBlend] [_DstBlend]
            ZWrite [_ZWrite]
            Cull [_Cull]

            CGPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_fog
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"
            #include "UnityLightingCommon.cginc"
            #include "CastlemistLightVolumes.cginc"

            // Samplers: _CastlemistGrab, _CameraDepthTexture, _DistortTex (shared by every 2D map),
            // linear_clamp, unity_SpecCube0, plus Light Volumes' own (well under 16).
            UNITY_DECLARE_SCREENSPACE_TEXTURE(_CastlemistGrab);
            UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);
            UNITY_DECLARE_TEX2D(_DistortTex);
            UNITY_DECLARE_TEX2D_NOSAMPLER(_DistortTex2);
            UNITY_DECLARE_TEX2D_NOSAMPLER(_MaskTex);
            UNITY_DECLARE_TEX2D_NOSAMPLER(_RampTex);
            UNITY_DECLARE_TEX2D_NOSAMPLER(_MainTex);
            SamplerState sampler_linear_clamp;
            float _DistortUV, _DistortUV2, _UseMap2, _Distort, _DistortBias, _DepthReject, _ViewReflect, _MaskOffset;
            float _DistortIsNormal, _DistortTex2IsNormal;
            half4 _Tint;
            float _UseVertexColor;
            float _AlphaSource, _MaskUV, _MaskChannel, _Cutoff, _StepMin, _UseRamp, _SoftFade, _DepDist;
            float _UseLit, _MainUV, _UseMainTex, _LitFadeDist, _SunScale;
            half4 _LitDiffuse;
            float _Glass, _GlassSpecPower, _GlassSpecDouble, _ReflectionStrength;
            half4 _GlassSpec;
            half _LightVolumes, _LightVolumesBias, _PointLightVolumesShading;
            float4 _StepCol, _UVAnim0, _UVAnim1, _UVAnim2, _UVAnim3;

            struct appdata
            {
                float4 vertex : POSITION;
                float3 normal : NORMAL;
                float4 tangent : TANGENT;
                float4 color : COLOR;
                float2 uv0 : TEXCOORD0;
                float2 uv1 : TEXCOORD1;
                float2 uv2 : TEXCOORD2;
                float2 uv3 : TEXCOORD3;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f
            {
                float4 pos : SV_POSITION;
                float4 uv01 : TEXCOORD0;
                float4 uv23 : TEXCOORD1;
                float4 grabPos : TEXCOORD2;   // z: view depth
                float3 worldNormal : TEXCOORD3;
                float3 worldPos : TEXCOORD4;
                float2 viewReflect : TEXCOORD5;
                UNITY_FOG_COORDS(6)
                float4 worldTangent : TEXCOORD7;   // unnormalised (0 without mesh tangents); w: bitangent sign
                half4 color : COLOR;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            float2 Animate(float2 uv, float4 a)
            {
                float s, c;
                sincos(a.z * _Time.y, s, c);
                float2 d = uv - 0.5;
                return float2(c * d.x - s * d.y, s * d.x + c * d.y) + 0.5 + a.xy * _Time.y;
            }

            float2 UVSet(v2f i, float k)
            {
                return k < 0.5 ? i.uv01.xy : k < 1.5 ? i.uv01.zw : k < 2.5 ? i.uv23.xy : i.uv23.zw;
            }

            float Smooth01(float x) { return x * x * (3.0 - 2.0 * x); }

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv01 = float4(Animate(v.uv0, _UVAnim0), Animate(v.uv1, _UVAnim1));
                o.uv23 = float4(Animate(v.uv2, _UVAnim2), Animate(v.uv3, _UVAnim3));
                o.grabPos = ComputeGrabScreenPos(o.pos);
                COMPUTE_EYEDEPTH(o.grabPos.z);
                o.worldPos = mul(unity_ObjectToWorld, v.vertex).xyz;
                o.worldNormal = UnityObjectToWorldNormal(v.normal);
                // Not UnityObjectToWorldDir: it normalises, and a mesh without tangents gives 0 (NaN).
                o.worldTangent = float4(mul((float3x3)unity_ObjectToWorld, v.tangent.xyz), v.tangent.w * unity_WorldTransformParams.w);
                o.color = v.color;
                // The game's VS: the view-space reflection of the view direction about the normal.
                float3 viewPos = UnityObjectToViewPos(v.vertex);
                float3 nView = normalize(mul((float3x3)UNITY_MATRIX_IT_MV, v.normal));
                o.viewReflect = reflect(normalize(viewPos), nView).xy;
                UNITY_TRANSFER_FOG(o, o.pos);
                return o;
            }

            float3 Normal(float2 rg) { float2 n = rg * 2.0 - 1.0; return float3(n, sqrt(saturate(1.0 - dot(n, n)))); }

            // A tangent-space normal in Unity's convention, from a Unity normal map or a raw GW2 map.
            float3 TangentNormal(half4 t, float isUnityNormal)
            {
                if (isUnityNormal > 0.5) return UnpackNormal(t);
                float3 n = Normal(t.xy);
                return float3(n.x, -n.y, n.z);
            }

            // GW2's raw (x, y, z), which the screen offsets are made of.
            float3 Raw(float3 nUnity) { return float3(nUnity.x, -nUnity.y, nUnity.z); }

            // Tangent space -> world; the geometric normal where the mesh has no tangents.
            float3 ToWorld(v2f i, float3 wn, float3 t)
            {
                float3 wt = i.worldTangent.xyz;
                if (dot(wt, wt) < 1e-8 || abs(i.worldTangent.w) < 0.5) return wn;
                wt = normalize(wt);
                float3 wb = cross(wn, wt) * i.worldTangent.w;
                return normalize(t.x * wt + t.y * wb + t.z * wn);
            }

            // The grabbed scene at the pixel + off, with the game's foreground test (_DepthReject).
            half3 Grab(float2 suv, float2 off, float depth)
            {
                if (_DepthReject > 0.5)
                {
                    float sceneAtOffset = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE(_CameraDepthTexture, suv + off));
                    if (sceneAtOffset < depth) off = 0;   // the offset would pull in something in front
                }
                return UNITY_SAMPLE_SCREENSPACE_TEXTURE(_CastlemistGrab, saturate(suv + off)).rgb;
            }

            half3 Probe(float3 dir)
            {
                return DecodeHDR(UNITY_SAMPLE_TEXCUBE(unity_SpecCube0, dir), unity_SpecCube0_HDR) * _ReflectionStrength;
            }

            // GW2's shRed/shGreen/shBlue . (N, 1): VRC Light Volumes when the scene has them (as
            // Castlemist/GW2 does), else the light probes. GW2's VS point lights come with the volumes.
            half3 Ambient(float3 worldPos, float3 n, float3 v)
            {
                half3 a = ShadeSH9(float4(n, 1));
                if (_LightVolumes > 0.5 && LightVolumesEnabled() > 0.5)
                {
                    float3 L0 = 0, L1r = 0, L1g = 0, L1b = 0, spec = 0;
                    LightVolumeSHSpecular(worldPos, L0, L1r, L1g, L1b, spec, 0, 0, 0, n, v,
                                          n * _LightVolumesBias, _PointLightVolumesShading);
                    a = LightVolumeEvaluate(n, L0, L1r, L1g, L1b);
                }
                return max(0, a);
            }

            fixed4 frag(v2f i, fixed facing : VFACE) : SV_Target
            {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
                float2 suv = i.grabPos.xy / i.grabPos.w;
                float3 v = normalize(_WorldSpaceCameraPos - i.worldPos);
                float3 wn = normalize(i.worldNormal) * (facing > 0 ? 1.0 : -1.0);
                bool water = _Glass > 1.5;
                bool glass = _Glass > 0.5 && !water;
                bool lit = _UseLit > 0.5 || glass;   // the one lit path (630598, 49659, 63923)

                // The distortion map: Unity tangent convention (nt) and GW2's raw (n).
                float3 nt = TangentNormal(UNITY_SAMPLE_TEX2D(_DistortTex, UVSet(i, _DistortUV)), _DistortIsNormal);
                float3 n = Raw(nt);
                float3 N = wn;
                half3 col;

                if (water)
                {
                    // 57026: two normals weighted by the mask (G: map at UV0, B: map2 at UV1), each
                    // its own tap; no view reflection; the second tap is subtracted.
                    half4 m = UNITY_SAMPLE_TEX2D_SAMPLER(_MaskTex, _DistortTex, UVSet(i, _MaskUV));
                    float3 z = float3(0, 0, 1);
                    float3 nB = lerp(z, nt, m.g);
                    float3 nA = lerp(z, TangentNormal(UNITY_SAMPLE_TEX2D_SAMPLER(_DistortTex2, _DistortTex, UVSet(i, _DistortUV2)),
                                                      _DistortTex2IsNormal), m.b);
                    half3 sa = Grab(suv, -(Raw(nA).xy * _Distort + _Distort), i.grabPos.z);
                    half3 sb = Grab(suv, Raw(nB).xy * _Distort + _Distort, i.grabPos.z);
                    float3 s = nA + nB;
                    N = ToWorld(i, wn, 2.0 * s * (s.z / dot(s, s)) - z);
                    col = lerp(sb, sa, pow(max(1.0 - saturate(dot(N, v)), 1e-5), 1.5)) * _Tint.rgb;
                }
                else
                {
                    float2 off;
                    if (_UseMap2 > 0.5)
                    {
                        // 47396: the two normals blended, normalised by z, x 2.
                        float3 n2 = Normal(UNITY_SAMPLE_TEX2D_SAMPLER(_DistortTex2, _DistortTex, UVSet(i, _DistortUV2)).xy);
                        float3 s = n + n2;
                        off = s.xy * (s.z / dot(s, s)) * 2.0;
                    }
                    else off = n.xy;
                    off = off * _Distort + (_DistortBias > 0.5 ? _Distort : 0);
                    if (_ViewReflect > 0.5) off *= i.viewReflect;
                    // 339341: mask.R scales the offset (the mask is not an alpha there).
                    if (_MaskOffset > 0.5) off *= UNITY_SAMPLE_TEX2D_SAMPLER(_MaskTex, _DistortTex, UVSet(i, _MaskUV)).r;
                    // 630598: the offset fades out with view depth (the game's 1 - z x 0.000326).
                    if (_UseLit > 0.5) off *= saturate(1.0 - i.grabPos.z / max(_LitFadeDist, 1e-4));
                    col = Grab(suv, off, i.grabPos.z) * _Tint.rgb;
                    // 709206: x vertex colour x 2 (gamma 2x; unity_ColorSpaceDouble keeps it right in linear).
                    if (_UseVertexColor > 0.5) col *= i.color.rgb * unity_ColorSpaceDouble.rgb;
                    if (lit) N = ToWorld(i, wn, nt);
                }

                half a = 1;
                if (_AlphaSource > 0.5)
                {
                    half4 m = UNITY_SAMPLE_TEX2D_SAMPLER(_MaskTex, _DistortTex, UVSet(i, _MaskUV));
                    float ch = _MaskChannel;
                    a = dot(m, half4(ch < 0.5, abs(ch - 1) < 0.5, abs(ch - 2) < 0.5, ch > 2.5));
                    if (_AlphaSource > 1.5)
                        a = Smooth01(saturate(_StepMin * (a + _StepCol.w - 1.0) / max(1.0 - _StepMin, 1e-4))) * _StepCol.x;
                }
                if (_UseVertexColor > 0.5) a *= i.color.a;
                if (_UseRamp > 0.5)
                {
                    float ndv = saturate(abs(dot(normalize(i.worldNormal), v)));
                    a *= UNITY_SAMPLE_TEX2D_SAMPLER(_RampTex, _linear_clamp, ndv.xx).r;
                }
                if (_SoftFade > 0.5)
                {
                    float scene = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE(_CameraDepthTexture, suv));
                    a *= Smooth01(saturate((scene - i.grabPos.z) / max(_DepDist, 1e-4)));
                }

                if (lit || water)
                {
                    // The sun: shSunData (towards it) / shSunColor; unshadowed (GW2: VSM x shSunData.w).
                    float3 L = _WorldSpaceLightPos0.xyz - i.worldPos * _WorldSpaceLightPos0.w;
                    L = dot(L, L) > 1e-8 ? normalize(L) : float3(0, 1, 0);
                    half3 sun = _LightColor0.rgb * _SunScale;
                    float fres = 1.0 - saturate(dot(N, v));
                    float f15 = pow(max(fres, 1e-5), 1.5);
                    float3 h = normalize(v + L);
                    half3 spec = _GlassSpec.rgb * (_GlassSpecDouble > 0.5 ? unity_ColorSpaceDouble.rgb : 1.0);
                    half r = 0.25;   // water: 0.25 x fres^1.5 reflection, no reflection mask on the highlight
                    half sr = 1;
                    if (lit)
                    {
                        // 630598 (ps 11) and 49659 / 63923 (ps 31). The game works in tangent space; the
                        // dot products are the same in world.
                        half4 dif = _UseMainTex > 0.5 ? UNITY_SAMPLE_TEX2D_SAMPLER(_MainTex, _DistortTex, UVSet(i, _MainUV)) : _LitDiffuse;
                        half cover = saturate(2.0 * dif.a);
                        half gloss = saturate(2.0 * dif.a - 1.0);
                        // 630598: the diffuse (its slot fileId 0 on every model seen: a runtime composite,
                        // _LitDiffuse stands in) multiplies the grab; glass: the grab alone.
                        if (_UseLit > 0.5) { a = cover; col = dif.rgb * col * unity_ColorSpaceDouble.rgb; }
                        float f2 = fres * fres;
                        float dim = 1.0 - 0.3 * f2 * f2 * f2;                        // 1 - 0.3 x fres^6
                        float trans = saturate(0.25 * (1.0 + dot(N, -L)) * (1.0 + dot(v, -L)));
                        col = lerp(col * dim, saturate(col * sun * 1.5), trans);
                        if (_UseLit > 0.5) r = gloss;
                        else if (_UseMainTex > 0.5)
                        {
                            // 49659 / 63923: the decal (UV1) lit by SH + sun covers the refraction by
                            // saturate(2a); above a half it adds reflection back.
                            half3 light = Ambient(i.worldPos, N, v) + sun * saturate(dot(N, L));
                            col = lerp(col, dif.rgb * light, cover);
                            r = min(1.0, 1.0 - cover + gloss);
                        }
                        else r = 1;   // glass without a decal: clear
                        sr = r;
                    }
                    col = lerp(col, Probe(reflect(-v, N)), r * f15);
                    col += pow(saturate(dot(N, h)), _GlassSpecPower) * sun * spec * sr;
                }
                if (_AlphaSource > 0.5 || _UseLit > 0.5) clip(a - _Cutoff);
                fixed4 c = fixed4(col, a);
                UNITY_APPLY_FOG(i.fogCoord, c);
                return c;
            }
            ENDCG
        }
    }
    FallBack Off
}
