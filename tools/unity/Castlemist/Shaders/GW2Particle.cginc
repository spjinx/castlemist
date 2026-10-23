// castlemist: the passes of "Castlemist/GW2 Particle", the one shader for every GW2 particle
// (README.md). Built on GW2Core.cginc's declarations and blocks (the pass defines CM_PASS_PARTICLE).
//
// Each _PtcShader value is one family of GW2's particle pixel shaders, transcribed from the AMATs'
// DXBC (gw2dat_cli amat --dump-all + D3DDisassemble, 2026-10-22; the AMATs read are named per
// family; CastlemistMaterials.BuildParticleMaterial picks the family). What every family shares,
// as GW2's particle shaders have it:
//   vertex     GW2's particle vertex is POSITION, NORMAL, COLOR, TEXCOORD0..3. Unity's particle
//              renderer feeds (CastlemistMaterials.ParticleStreams): TEXCOORD0 = (UV, UV2: the
//              flipbook's next frame), TEXCOORD1 = (AnimBlend, StableRandom.xyz). GW2 uv1..uv3:
//              the flipbook shaders read uv1 as the next frame and uv2.x as the frame blend; the
//              fire and flow shaders read uv1 / uv2.x / uv3 as per-particle phases, given here the
//              stable per-particle randoms (rand.xy, rand.z, rand.yz; UNPROVEN: GrCloud's vertex
//              fill not read). _MainTex_ST (the cloud's texCoordRect) maps every UV, as GW2's
//              CPU-built vertices carry the rect
//   soft       smoothstep01(saturate((scene depth - depth) / D)), D = depfade / depdist, else the
//              shaders' literal 17 GW2 units (_DepDist, Unity units; _SoftFade 0 when D is 0)
//   colour     GW2 works in gamma space on an 8-bit target. Here colours multiply in the project's
//              space (a product converts exactly), GW2's x 2 is unity_ColorSpaceDouble, a coverage
//              folded into a colour is a^2.2 in linear projects (CM_PtcCov), and the vertex colour
//              read as a PARAMETER (flow thresholds, step fades) is taken back to gamma (vcG)
//   fog        Unity's fog: to black for additive blends (_AdditiveFog), as GW2's x (1 - fog)
// Families (r = the result written; T = _MainTex, frame-blended with _FrameBlend; vc = vertex colour):
//   0 basic        (no GW2 shader read: a stand-in) r = T x vc, rgb x coverage when the blend's
//                  source factor is One (_AlphaPremultiply)
//   1 soft         fx-soft-additive 19092, 54360, 25489, 43029 (One / InvSrcColor): a = T.a vc.a
//                  soft; rgb = 2 T vc a. _PtcTexAlphaRgb 0 (43029): rgb x vc.a soft only, a = T.a
//   2 premult      fx-premultiplied 20760, 53260 (One / InvSrcAlpha): r = soft (2 T vc.rgb vc.a,
//                  T.a vc.a)
//   3 step         fx-premultiplied 19116, 14196, 48767 (One / InvSrcAlpha): the vertex alpha
//                  dissolves the sprite: s = step, x = smooth(sat((s lum(T) + s vc.a - s)/(1-s))),
//                  y = the same with T.a, A = lerp(x, y, T.a) x diffade (_AlphaScale) or x vc.r
//                  (_PtcFadeVcR, 48767); r = A soft T, rgb x 2 vc.rgb with _PtcVcColour
//   4 fade         1601524 pass 0, fx-alpha 19255 (SrcAlpha / InvSrcAlpha): a = T.a vc.a soft,
//                  discard below AlphaRef (_Cutoff); rgb = T (1601524: no vertex colour), x 2 vc.rgb
//                  with _PtcVcColour (19255). 1601524's pass 1 is the add layer (fragAddLayer)
//   5 flow         882183, 1710790 (SrcAlpha / InvSrcAlpha): the vertex colour is the parameters:
//                  T at uv0 + (_FlowTex at (uv0 + rand.x + (t flow.w, 0)) flow.x, x 2 - 1) x
//                  |vc.r|^flow.z flow.y; a = smooth(sat((T.a - vc.g) / (vc.b - vc.g))) vc.a soft,
//                  discard below AlphaRef; rgb = 2 T lerp(endcl, startcl, smooth(sat((a - stepcol.x)
//                  / (stepcol.y - stepcol.x))))
//   6 fire         fx-fire 977200, 1171331 (One / InvSrcAlpha): GW2Core's fire block (CM_FireCore
//                  modes 2 / 3) x soft
//   7 flow layer   1764372 (One / InvSrcColor): L = _LayerTex at uv0 uvscrol.xy + frac(t uvscrol.zw)
//                  + 0.3 (_FlowTex at flowuv.xy (ndc x distance + 1) / 2 + frac(t flowuv.zw), x 2 - 1),
//                  distance from the camera in GW2 units; a = L.a T.a vc.a soft; rgb = 2 L T vc a
// The add layer (second pass, _UseAddLayer; 1601524 pass 1, SrcAlpha / One): w = smooth(sat((s add.r
// + s vc.r - s) / (1 - s))), s = stepmin; a = add.a vc.a w soft addcolr.a, discard below AlphaRef;
// rgb = addcolr (the burning edge of a page).
// UV maths is in GW2's convention (v down, CM_FireG / CM_FIRE_TEX), so offsets keep GW2's signs.

#ifndef CASTLEMIST_GW2_PARTICLE_INCLUDED
#define CASTLEMIST_GW2_PARTICLE_INCLUDED

#include "GW2Core.cginc"

float _PtcShader;
half _PtcTexAlphaRgb, _PtcVcColour, _PtcFadeVcR, _FrameBlend;
half _UseAddLayer;
UNITY_DECLARE_TEX2D_NOSAMPLER(_AddTex);
half4 _AddColor;
float _AddStepMin;
UNITY_DECLARE_TEX2D_NOSAMPLER(_LayerTex);
float4 _LayerScroll, _LayerFlowUV;

struct appdataP
{
    float4 vertex : POSITION;
    half4 color : COLOR;
    float4 tc0 : TEXCOORD0;   // UV, UV2 (the next flipbook frame)
    float4 tc1 : TEXCOORD1;   // AnimBlend, StableRandom.xyz
    UNITY_VERTEX_INPUT_INSTANCE_ID
};

struct v2fP
{
    float4 pos : SV_POSITION;
    float4 uv01 : TEXCOORD0;      // GW2 uv0 (frame A), uv1 (rand.xy)
    float4 uv23 : TEXCOORD1;      // GW2 uv2 (rand.z, 0), uv3 (rand.yz)
    float4 frameB : TEXCOORD2;    // the next frame's UV, its blend, the camera distance (GW2 units)
    float4 screenPos : TEXCOORD3;
    half4 color : TEXCOORD4;
    UNITY_FOG_COORDS(5)
    UNITY_VERTEX_OUTPUT_STEREO
};

v2fP vertParticle(appdataP v)
{
    v2fP o;
    UNITY_INITIALIZE_OUTPUT(v2fP, o);
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
    o.pos = UnityObjectToClipPos(v.vertex);
    float3 wp = mul(unity_ObjectToWorld, v.vertex).xyz;
    float2 a = CM_Animate(v.tc0.xy * _MainTex_ST.xy + _MainTex_ST.zw, _UVAnim0);
    float2 b = CM_Animate(v.tc0.zw * _MainTex_ST.xy + _MainTex_ST.zw, _UVAnim0);
    float3 rnd = v.tc1.yzw;
    o.uv01 = float4(a, rnd.xy);
    o.uv23 = float4(rnd.z, 0, rnd.yz);
    o.frameB = float4(b, v.tc1.x, distance(_WorldSpaceCameraPos, wp) / max(_UnityScale, 1e-6));
    o.color = v.color;
    o.screenPos = ComputeScreenPos(o.pos);
    COMPUTE_EYEDEPTH(o.screenPos.z);
    UNITY_TRANSFER_FOG(o, o.pos);
    return o;
}

// A coverage that GW2 folds into a gamma-space colour, for this project's space.
half CM_PtcCov(half a) { return IsGammaSpace() ? a : pow(max(a, 0), 2.2); }

// The vertex colour as GW2 has it (gamma), for the families that read it as parameters.
half4 CM_PtcGamma(half4 c) { return IsGammaSpace() ? c : half4(LinearToGammaSpace(c.rgb), c.a); }

// _MainTex at frame A, blended toward frame B by the particle's AnimBlend (_FrameBlend).
half4 CM_PtcTex(v2fP i)
{
    half4 t = CM_TEX(_MainTex, i.uv01.xy);
    if (_FrameBlend > 0.5 && i.frameB.z > 0) t = lerp(t, CM_TEX(_MainTex, i.frameB.xy), i.frameB.z);
    return t;
}

half4 CM_PtcFog(v2fP i, half4 c)
{
    fixed4 fogColor = _AdditiveFog > 0.5 ? fixed4(0, 0, 0, 0) : unity_FogColor;
    UNITY_APPLY_FOG_COLOR(i.fogCoord, c, fogColor);
    return c;
}

half4 fragParticle(v2fP i) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
    half4 vc = i.color;
    half4 vcG = CM_PtcGamma(vc);
    half soft = CM_SoftFade(i.screenPos);
    half3 x2 = unity_ColorSpaceDouble.rgb;
    float t = _Time.y;
    half4 o;
    if (_PtcShader < 0.5)
    {
        o = CM_PtcTex(i) * vc;
        o.a *= soft;
        if (_AlphaPremultiply > 0.5) o.rgb *= CM_PtcCov(o.a);
    }
    else if (_PtcShader < 1.5)
    {
        half4 c = CM_PtcTex(i);
        half3 rgb = c.rgb * vc.rgb * x2;
        half a = c.a * vc.a * soft;
        o = _PtcTexAlphaRgb > 0.5 ? half4(rgb * CM_PtcCov(a), a) : half4(rgb * CM_PtcCov(vc.a * soft), c.a);
    }
    else if (_PtcShader < 2.5)
    {
        half4 c = CM_PtcTex(i);
        half a = c.a * vc.a * soft;
        o = half4(c.rgb * vc.rgb * x2 * CM_PtcCov(vc.a * soft), a);
    }
    else if (_PtcShader < 3.5)
    {
        half4 c = CM_PtcTex(i);
        float s = _StepMin, k = 1.0 / max(1.0 - s, 1e-4);
        float base = vcG.a * s - s;
        half lum = dot(IsGammaSpace() ? c.rgb : LinearToGammaSpace(c.rgb), half3(0.3, 0.59, 0.11));
        half xs = CM_Smooth01(saturate(k * (s * lum + base)));
        half ys = CM_Smooth01(saturate(k * (s * c.a + base)));
        half A = lerp(xs, ys, c.a) * (_PtcFadeVcR > 0.5 ? vcG.r : _AlphaScale) * soft;
        o = half4(c.rgb * CM_PtcCov(A), c.a * A);
        if (_PtcVcColour > 0.5) o.rgb *= vc.rgb * x2;
    }
    else if (_PtcShader < 4.5)
    {
        half4 c = CM_PtcTex(i);
        half a = c.a * vc.a * soft;
        clip(a - _Cutoff);
        o = half4(_PtcVcColour > 0.5 ? c.rgb * vc.rgb * x2 : c.rgb, a);
    }
    else if (_PtcShader < 5.5)
    {
        float2 g0 = CM_FireG(i.uv01.xy);
        float k = pow(abs(vcG.r), _Flow.z) * _Flow.y;
        float2 f = CM_FIRE_TEX(_FlowTex, (g0 + i.uv01.z + float2(t * _Flow.w, 0)) * _Flow.x).xy * 2.0 - 1.0;
        half4 c = CM_FIRE_TEX(_MainTex, g0 + f * k);
        half a = CM_Smooth01(saturate((c.a - vcG.g) / max(vcG.b - vcG.g, 1e-4))) * vc.a * soft;
        clip(a - _Cutoff);
        half w = CM_Smooth01(saturate((a - _FlowStep.x) / max(_FlowStep.y - _FlowStep.x, 1e-4)));
        o = half4(c.rgb * lerp(_FlowEnd.rgb, _FlowStart.rgb, w) * x2, a);
    }
    else if (_PtcShader < 6.5)
    {
        // a particle faces the camera: its normal is the view direction
        o = CM_FireCore(i.uv01, i.uv23, vcG, -UNITY_MATRIX_V[2].xyz) * soft;
        o.a = saturate(o.a);
    }
    else
    {
        float2 g0 = CM_FireG(i.uv01.xy);
        float2 ndc = i.screenPos.xy / max(i.screenPos.w, 1e-6) * 2.0 - 1.0;
        float2 fuv = _LayerFlowUV.xy * (ndc * i.frameB.w + 1.0) * 0.5 + frac(t * _LayerFlowUV.zw);
        float2 f = CM_FIRE_TEX(_FlowTex, fuv).xy * 2.0 - 1.0;
        half4 L = CM_FIRE_TEX(_LayerTex, g0 * _LayerScroll.xy + frac(t * _LayerScroll.zw) + 0.3 * f);
        half4 c = CM_PtcTex(i) * vc;
        half a = L.a * c.a * soft;
        o = half4(L.rgb * c.rgb * x2 * CM_PtcCov(a), a);
    }
    return CM_PtcFog(i, o);
}

// The add layer (second pass): see the header.
half4 fragAddLayer(v2fP i) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
    if (_UseAddLayer < 0.5) discard;
    half4 vcG = CM_PtcGamma(i.color);
    half2 add = CM_FIRE_TEX(_AddTex, CM_FireG(i.uv01.xy)).ra;
    float s = _AddStepMin;
    half w = CM_Smooth01(saturate((s * add.x + s * vcG.r - s) / max(1.0 - s, 1e-4)));
    half a = add.y * i.color.a * w * CM_SoftFade(i.screenPos) * _AddColor.a;
    clip(a - _Cutoff);
    half4 o = half4(_AddColor.rgb, a);
    UNITY_APPLY_FOG_COLOR(i.fogCoord, o, fixed4(0, 0, 0, 0));
    return o;
}

#endif // CASTLEMIST_GW2_PARTICLE_INCLUDED
