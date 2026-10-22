// castlemist: the passes of "Castlemist/GW2", the one shader for GW2 world materials (README.md).
//
// Every GW2 feature is a block switched by a material float (uniform branching), never by a
// keyword: one variant per Unity pass setup, no keyword budget (project choice 2026-10-22; a
// per-family bake can constant-fold them later). The including pass defines one of
// CM_PASS_FORWARDBASE, CM_PASS_FORWARDADD, CM_PASS_SHADOWCASTER or CM_PASS_META first.
//
// UV sets. uv0..uv3 are GW2's TEXCOORD0-3 (uv1 from TEXCOORD4 with _UV1FromUV4: the export's
// copy, so a baked lightmap may own UV1), each animated by _UVAnim<n> = the MODL texTransform
// of that set: scroll xy (UV per second) and rotate z (radians per second, about the UV centre;
// units UNPROVEN). Every map names its set with a _<Map>UV float (0-3).
//
// Blocks (materials.json -> properties: CastlemistMaterials):
//   perturb    _PerturbTex.xy at _PerturbUV (x 2 - 1 with _PerturbSigned): offsets the base colour's
//              UV (x _BasePtrb, GW2 difptrb) and the mask's (x _MskPtrb)
//   colour     _MainTex x _Color at its UV + perturb x _BasePtrb (only its alpha with
//              _PerturbAlphaOnly, GW2 574526); or, with _UseParallax, the
//              parallax layer _ParallaxTex sampled at uv + offset(_OffsetTex).xy x _ParaPer -
//              viewTS.xy x _ParDist (GW2 fx-parallax-*)
//   interior   _UseInterior: lerp(_InteriorTex at its UV - viewTS x _ParDist + normalTS.xy x
//              _InteriorNormalShift, colour, w) (GW2 prop-interior 3036412, 1601802, 2314655), w
//              the mask channel _MaskChannel, stepped with _InteriorStep (smoothstep01(saturate(
//              _StepMin (w + _InteriorStepOffset - 1) / (1 - _StepMin))), GW2 stepmin / decfade);
//              _InteriorMode 1 mixes the other way, lerp(colour, layer, w x saturate(2 layer.a))
//              (GW2 prop-burn 3122105's decal); _InteriorMode 2 is the export's decal map (A =
//              coverage): lerp(colour, layer, layer.a x w) (prop-decal, subsurface-decal)
//   cutout     _UseCutout: discard where _CutoutTex channel _CutoutChannel x _CutoutScale < 0.5
//              (GW2 opacity); + emission smoothstep01(saturate((rgb x _CutoutScale + _BurnOffset
//              - 1) x -10)) x _BurnColor x 2 along the edge (GW2 burnoff / burncol)
//   mod        _UseDetailMod: albedo x 2 x _DetailMod ("detail-multiply2x"), its UV + perturb x
//              _ModPtrb x mask.R (GW2 modptrb, 574526)
//   normal     _UseNormalMap: _BumpMap
//   packed     _UsePacked: _PackedMap R metal, G smoothness, B reflection mask, A specular mask
//   emission   _UseEmission: _EmissionMap (its UV + perturb x _EmissionPtrb, GW2 gloptrb) x mask
//              channel x _EmissionColor x _EmissionStrength (GW2 glow x glofade x 2)
//   alpha      colour.a, x ramp(|N.V|) (_UseRamp), x mask(uv + perturb.xy x _MskPtrb).r (_UseMask),
//              then the GW2 step (_UseStep: smoothstep01(saturate(stepmin (a + stepcol.w - 1) /
//              (1 - stepmin))) x stepcol.x) or x _AlphaScale, x the soft depth fade (_SoftFade);
//              _AlphaTest discards below _Cutoff
//   lighting   lit (Unity's standard BRDF, VRC Light Volumes or light probes, reflection probes)
//              or _Unlit (the colour as it is, fogged: GW2's effect shaders)
//   rim        _UseRim: + main light x _RimColor x (1 - N.V)^_RimPower (GW2 rimcp: rgb, w); with
//              _RimShineFade colour and power x (1 - shine), shine = the packed smoothness, and
//              with _RimUseMask x (1 - the mask channel) (the prop-metalmask lit core 3576633:
//              x (1 - metalmask.G))
//   screen     _UseScreenLayer: colour = c + s - c s with _ScreenTex (GW2 80417's "add" layer)
//   ramp rgba  _RampRGBA: the ramp (at |N.V|) multiplies the whole colour, not only the alpha (80417)
//   normal     _PerturbNormal: the normal map's UV moves with the colour's perturb (3100353)
//   water      _UseWater (GW2 3121604, ps 24 read 2026-10-22; unlit): the "projection" cube along
//              refract(view, N, 0.8), two time-drifted samples blended (in GW2 space: + (0, f, -0.5 f));
//              lerp toward _WaterEdge x edge by smoothstep01(saturate((1 - N.V) x 0.769)), edge =
//              smoothstep01 of (N.V - falloff.x) / (falloff.y - falloff.x); + the "reflect" cube along
//              reflect x (0.2, 0.3, 1); + triplanar caustics (_MainTex at position x causscl + t x
//              scrspd.xy + flow x flowstr, flow = _FlowTex at position x flowscl + t x scrspd.zw)
//              x smoothstep01 of (N.V - falloff.z) / (falloff.w - falloff.z) x _WaveColor; position
//              in GW2 units (world / _UnityScale). The cubes are GW2's own (castlemist writes them
//              as .dds, faces as stored: GW2 +x, -x, +y north, -y, +z down, -z), looked up with the
//              GW2-space direction; without them (_UseWaterCubes 0) the reflection probe stands in
//   vcolour    _UseVertexColor: the colour x the mesh's vertex colour (GW2 709501, 2434562)
//   flow       _UseFlow (GW2 fx-flow, 2591650 / 2759298): the colour's UV + (_FlowTex at (uv0 +
//              (t x flow.w, 0)) x flow.x) x 2 - 1) x |vc.r|^flow.z x flow.y; alpha = smoothstep01
//              of (colour.a - vc.g) / (vc.b - vc.g) x vc.a; the colour x lerp(startcl, endcl,
//              smoothstep01 of (alpha - stepcol.x) / (stepcol.y - stepcol.x)) x 2 (_FlowPremul: x
//              colour.a too). vc = the vertex colour: GW2 stores the effect's parameters there
//   envcube    _UseEnvCube (GW2 fx-cubemap 221571; unlit): (cube.rgb / cube.a)^2 along -V - 0.03 x
//              perturb.rgb (_PerturbTex), in GW2 space (the cube is GW2's own .dds)
//   bumpshade  _UseBumpShade (GW2 fx-multiply-bump 157432, 3718974; a 2x multiply decal): o =
//              dot(L, N mapped) - dot(L, N) + 0.5 with L the main light, N mapped from _BumpMap's
//              RG read raw (x 2 - 1, z rebuilt; a linear texture), i.e. the bump shading
//              against a flat surface; _UseBumpShadeTex: rgb = 2 x that x _BumpShadeTex x
//              _BumpShadeOpacity + 0.5. Worked in gamma space, as GW2 (the 2x factor is
//              converted for a linear project)
//   fogneutral _UseFogNeutral: fog toward grey _FogNeutral (a multiply's no-change colour: 1 for
//              DstColor / Zero, 0.5 for 2x), not the scene's fog colour
//   fire       _UseFire (GW2 fx-fire; port_fire/SPEC.md; unlit, output premultiplied, One / InvSrcAlpha).
//              GW2 UV space (v down: g = (u, 1 - v)); P(g, p) = (g.x s, g.y s + frac(t p.x)),
//              s = |frac(t p.y) 2 - 1| (p.w - p.z) + p.z. edge = max(0, 1 - 1.5 (1 - nzView)^2).
//              _FireMode 0 (740364): flow = _FlowTex(P(uv1, flowuv)) + _FireFlowB(P(uv1, flowbuv)) (x 2 - 1);
//              two phases p0 = frac(t flowspd), p1 = frac(t flowspd - 0.5), w = |2 p0 - 1|; fire =
//              lerp of _FireDetail.g at P(uv1, fireduv) + flow firedfl p; shape = lerp of _MainTex.g at
//              uv0 + flow lerp(ramprmp, _RampTex.r) firesfl p; K = smoothstep01(saturate((fire + 0.05)
//              2.036)) shape edge opacity; rgb = lerp(colorl, colorh, fire) 3.24 K, a = 1.8 K.
//              _FireMode 1 (965703): flow = _FlowTex at P(uv1, flowuv) + at P(uv1, 0.4 flowuv); r =
//              _RampTex.g; k = lerp(rampflw.xy, r), step = lerp(rampstp.xy, rampstp.zw, r); shape =
//              _MainTex.g(uv0 + flow k); fire = _FireDetail.g(P(uv1, fireduv) + flow k detailf); col =
//              lerp(colorl, colorh, smoothstep01 of fire over stepcol.xy); K = smoothstep01 of fire over
//              step x col.a shape edge fadedif; rgb = col K, a = K premult.
//              _FireMode 2 / 3 (977200 / 1171331, particles): p = 4 _MaskTex(up) _MainTex(uk scale +
//              (0, uv1.x + t speed)) _MainTex(uk scaleb + (0, uv1.y + t speed)), up = uv0 + perturb
//              (_FlowTex at 2 (q + (0, 2 t speedp)) + at q + (0, t speedp), x 2 - 1 each), q = (uv0 + uv3)
//              scalep, uk = up (uv2.x + 0.5); col = saturate(1 - p) colorl + colorh p (3: lerp(colorl,
//              colorh, p.r) normalize(_FireDye)); rgb = p col vc.rgb 2 vc.a, a = p.a vc.a.
//              All x the soft depth fade; colour (not coverage) to linear with pow 2.2 in linear
//              projects; alpha saturated (GW2's UNORM target clamps it)
//   premult    _PremultipliedBase: the base colour is already x its alpha (the export's
//              fx-soft-additive); an unlit effect then scales it by the alpha shaping only
// Shading formulas of the GW2 shaders these blocks come from are in the README's port table.
// The block switches are plain ifs on uniforms: the compiler branches or flattens them (no
// UNITY_BRANCH: fxc rejects gradient samples under forced branches it cannot prove uniform).

#ifndef CASTLEMIST_GW2_CORE_INCLUDED
#define CASTLEMIST_GW2_CORE_INCLUDED

#include "UnityCG.cginc"
#include "UnityStandardUtils.cginc"
#if defined(CM_PASS_FORWARDBASE) || defined(CM_PASS_FORWARDADD)
    #include "AutoLight.cginc"
    #include "UnityPBSLighting.cginc"
#endif
#if defined(CM_PASS_FORWARDBASE)
    #include "CastlemistLightVolumes.cginc"
#endif
#if defined(CM_PASS_META)
    #include "UnityMetaPass.cginc"
#endif

UNITY_DECLARE_TEX2D(_MainTex);     float4 _MainTex_ST;     float _MainTexUV;
half4 _Color;
half _AlphaTest, _Cutoff;
half _AlphaPremultiply;

half _UseDetailMod;
UNITY_DECLARE_TEX2D_NOSAMPLER(_DetailMod);   float4 _DetailMod_ST;   float _DetailModUV;
half _DetailModStrength;

half _UseNormalMap;
UNITY_DECLARE_TEX2D_NOSAMPLER(_BumpMap);     float _BumpMapUV;       half _BumpScale;

half _UsePacked;
UNITY_DECLARE_TEX2D_NOSAMPLER(_PackedMap);   float _PackedMapUV;
half _Metallic, _Glossiness;
half4 _ReflectionTint, _SpecularTint;
half _ReflectionStrength, _SpecularStrength;

half _UseEmission;
UNITY_DECLARE_TEX2D_NOSAMPLER(_EmissionMap); float _EmissionMapUV;
UNITY_DECLARE_TEX2D_NOSAMPLER(_EmissionMask); float _EmissionMaskUV; float _EmissionMaskChannel;
half4 _EmissionColor;
half _EmissionStrength;
float _EmissionPtrb;

half _UseParallax;
UNITY_DECLARE_TEX2D_NOSAMPLER(_ParallaxTex); float4 _ParallaxTex_ST; float _ParallaxUV;
UNITY_DECLARE_TEX2D_NOSAMPLER(_OffsetTex);   float _OffsetUV;
float _ParaPer, _ParDist;
half _UseMask;
UNITY_DECLARE_TEX2D_NOSAMPLER(_MaskTex);     float _MaskUV;     float _MaskChannel;
half _UseMaskConstant;   // the mask is a GW2 placeholder layer: one colour, _MaskConstant
half4 _MaskConstant;
UNITY_DECLARE_TEX2D_NOSAMPLER(_PerturbTex);  float _PerturbUV;
float _MskPtrb, _BasePtrb, _ModPtrb;
half _PerturbSigned, _PerturbAlphaOnly;
half _UseInterior, _InteriorStep, _InteriorMode;
UNITY_DECLARE_TEX2D_NOSAMPLER(_InteriorTex); float _InteriorUV;
float _InteriorNormalShift, _InteriorStepOffset;
half _UseCutout;
UNITY_DECLARE_TEX2D_NOSAMPLER(_CutoutTex); float _CutoutUV; float _CutoutChannel;
float _CutoutScale, _BurnOffset;
half4 _BurnColor;
half _UseRim, _RimShineFade, _RimUseMask, _PremultipliedBase;
half _UseScreenLayer, _RampRGBA, _PerturbNormal;
half _UseWater, _UseWaterCubes;
half _UseVertexColor, _UseFlow, _FlowPremul;
half _UseEnvCube, _UseBumpShade, _UseBumpShadeTex, _BumpShadeOpacity, _UseFogNeutral, _FogNeutral;
UNITY_DECLARE_TEXCUBE_NOSAMPLER(_EnvCube);
UNITY_DECLARE_TEX2D_NOSAMPLER(_BumpShadeTex); float _BumpShadeUV;
float4 _Flow, _FlowStep;
half4 _FlowStart, _FlowEnd;
// The flow block's UV offset for CM_Colour, set by CM_Surface (0 in the other passes).
static float2 cm_FlowOffset = 0;
half _UseFire;
float _FireMode;                      // 0 = 740364, 1 = 965703, 2 = 977200, 3 = 1171331
float4 _FireUV0, _FireUV1, _FireUV2;  // GW2 fireduv, flowuv, flowbuv: (v scroll /s, scale osc /s, scale min, scale max)
float4 _FireParams, _FireParams2, _FireParams3;  // per mode, see CastlemistMaterials.Fire
float4 _FireColorL, _FireColorH;      // GW2 colorl / colorh, raw (gamma) values
float4 _FireDye;                      // 1171331: the dye palette colour at dyedot (runtime; white by default)
UNITY_DECLARE_TEX2D_NOSAMPLER(_FireDetail);
UNITY_DECLARE_TEX2D_NOSAMPLER(_FireFlowB);
UNITY_DECLARE_TEX2D_NOSAMPLER(_FlowTex);
UNITY_DECLARE_TEXCUBE_NOSAMPLER(_ProjCube);
UNITY_DECLARE_TEXCUBE_NOSAMPLER(_ReflCube);
half4 _WaterEdge, _WaveColor;
float4 _WaterFalloff, _ScrollSpeed;
float _CausScale, _FlowScale, _FlowStrength, _UnityScale;
UNITY_DECLARE_TEX2D_NOSAMPLER(_ScreenTex); float _ScreenUV;
half4 _RimColor;
float _RimPower;
half _UseRamp;
UNITY_DECLARE_TEX2D_NOSAMPLER(_RampTex);
half _UseStep;
float _StepMin;
float4 _StepCol;
float _AlphaScale;
half _SoftFade;
float _DepDist;
UNITY_DECLARE_DEPTH_TEXTURE(_CameraDepthTexture);

// D3D11 allows 16 samplers per pixel shader and Unity takes several (shadows, lightmaps, probes,
// depth, Light Volumes): every GW2 map is a texture without a sampler, read with one of two
// inline sampler states (Unity builds them from their names): trilinear repeat for the maps,
// linear clamp for the ramp. Independent of what any texture slot holds.
SamplerState sampler_trilinear_repeat;
SamplerState sampler_linear_clamp;
#define CM_TEX(tex, uv) UNITY_SAMPLE_TEX2D_SAMPLER(tex, _trilinear_repeat, uv)
#define CM_TEX_CLAMP(tex, uv) UNITY_SAMPLE_TEX2D_SAMPLER(tex, _linear_clamp, uv)

float4 _UVAnim0, _UVAnim1, _UVAnim2, _UVAnim3;
float _UV1FromUV4;

half _Unlit;
half _AdditiveFog;
half _LightVolumes, _LightVolumesBias, _PointLightVolumesShading, _LightVolumeSpecular;

struct appdata
{
    float4 vertex : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
    float2 uv4 : TEXCOORD4;
    half4 color : COLOR;
    UNITY_VERTEX_INPUT_INSTANCE_ID
};

// TexTransform: scroll (xy, UV per second) and rotation (z, radians per second) about the UV centre.
float2 CM_Animate(float2 uv, float4 a)
{
    float s, c;
    sincos(a.z * _Time.y, s, c);
    float2 d = uv - 0.5;
    return float2(c * d.x - s * d.y, s * d.x + c * d.y) + 0.5 + a.xy * _Time.y;
}

void CM_AnimatedUVs(appdata v, out float4 uv01, out float4 uv23)
{
    uv01 = float4(CM_Animate(v.uv0, _UVAnim0), CM_Animate(_UV1FromUV4 > 0.5 ? v.uv4 : v.uv1, _UVAnim1));
    uv23 = float4(CM_Animate(v.uv2, _UVAnim2), CM_Animate(v.uv3, _UVAnim3));
}

// UV set `ch` (0-3) of the four.
float2 CM_PickUV(float4 uv01, float4 uv23, float ch)
{
    if (ch < 0.5) return uv01.xy;
    if (ch < 1.5) return uv01.zw;
    if (ch < 2.5) return uv23.xy;
    return uv23.zw;
}

float CM_Smooth01(float x) { return x * x * (3.0 - 2.0 * x); }

// The perturb map's offset (x 2 - 1 when _PerturbSigned); 0 when neither the colour nor the mask uses it.
float2 CM_Perturb(float4 uv01, float4 uv23)
{
    float2 p = 0;
    if (_BasePtrb != 0 || _MskPtrb != 0 || _ModPtrb != 0 || _EmissionPtrb != 0)
    {
        p = CM_TEX(_PerturbTex, CM_PickUV(uv01, uv23, _PerturbUV)).xy;
        if (_PerturbSigned > 0.5) p = p * 2.0 - 1.0;
    }
    return p;
}

// Mask channel `ch` (0 R, 1 G, 2 B, 3 A) at the mask's UV + perturb x _MskPtrb.
half CM_Mask(float4 uv01, float4 uv23, float2 perturb, float ch)
{
    half4 m = _UseMaskConstant > 0.5 ? _MaskConstant : CM_TEX(_MaskTex, CM_PickUV(uv01, uv23, _MaskUV) + perturb * _MskPtrb);
    return dot(m, half4(ch < 0.5, abs(ch - 1) < 0.5, abs(ch - 2) < 0.5, ch > 2.5));
}

// Colour and alpha before the alpha shaping: the base colour or the parallax layer, the interior
// lerp, the mod. viewTS: the view direction in the tangent frame; tnXY: the normal map's xy.
half4 CM_Colour(float4 uv01, float4 uv23, float2 viewTS, float2 tnXY)
{
    half4 c;
    float2 perturb = CM_Perturb(uv01, uv23);
    if (_UseParallax > 0.5)
    {
        float2 offset = CM_TEX(_OffsetTex, CM_PickUV(uv01, uv23, _OffsetUV)).xy;
        float2 cuv = CM_PickUV(uv01, uv23, _ParallaxUV) + offset * _ParaPer - viewTS * _ParDist;
        c = CM_TEX(_ParallaxTex, TRANSFORM_TEX(cuv, _ParallaxTex));
    }
    else
    {
        float2 buv = CM_PickUV(uv01, uv23, _MainTexUV) + cm_FlowOffset;
        if (_PerturbAlphaOnly > 0.5)
        {
            // 574526: the colour unperturbed, the alpha at the perturbed UV.
            c = UNITY_SAMPLE_TEX2D(_MainTex, TRANSFORM_TEX(buv, _MainTex)) * _Color;
            c.a = UNITY_SAMPLE_TEX2D(_MainTex, TRANSFORM_TEX((buv + perturb * _BasePtrb), _MainTex)).a * _Color.a;
        }
        else
            c = UNITY_SAMPLE_TEX2D(_MainTex, TRANSFORM_TEX((buv + perturb * _BasePtrb), _MainTex)) * _Color;
    }
    if (_UseInterior > 0.5)
    {
        float2 iuv = CM_PickUV(uv01, uv23, _InteriorUV) - viewTS * _ParDist + tnXY * _InteriorNormalShift;
        half4 interior = CM_TEX(_InteriorTex, iuv);
        float w = CM_Mask(uv01, uv23, perturb, _MaskChannel);
        if (_InteriorMode > 1.5) w *= interior.a;
        else if (_InteriorMode > 0.5) w *= saturate(2.0 * interior.a);
        if (_InteriorStep > 0.5)
            w = CM_Smooth01(saturate(_StepMin * (w + _InteriorStepOffset - 1.0) / max(1.0 - _StepMin, 1e-4)));
        c = _InteriorMode > 0.5 ? lerp(c, interior, w) : lerp(interior, c, w);
    }
    if (_UseScreenLayer > 0.5)
    {
        half3 sc = CM_TEX(_ScreenTex, CM_PickUV(uv01, uv23, _ScreenUV)).rgb;
        c.rgb = c.rgb + sc - c.rgb * sc;
    }
    if (_UseDetailMod > 0.5)
    {
        // detail-multiply2x: unity_ColorSpaceDouble makes 2 x mod right in gamma and linear space.
        float2 duv = CM_PickUV(uv01, uv23, _DetailModUV);
        if (_ModPtrb != 0) duv += perturb * _ModPtrb * CM_Mask(uv01, uv23, perturb, 0);
        half3 d = CM_TEX(_DetailMod, TRANSFORM_TEX(duv, _DetailMod)).rgb * unity_ColorSpaceDouble.rgb;
        c.rgb *= lerp(half3(1, 1, 1), d, _DetailModStrength);
    }
    return c;
}

// The GW2 alpha shaping of `a` (ramp, mask, step or scale), then the alpha test. ndv: |N.V|.
half CM_Alpha(half a, float4 uv01, float4 uv23, float ndv)
{
    if (_UseRamp > 0.5 && _RampRGBA < 0.5) a *= CM_TEX_CLAMP(_RampTex, saturate(ndv).xx).r;
    if (_UseMask > 0.5) a *= CM_Mask(uv01, uv23, CM_Perturb(uv01, uv23), _MaskChannel);
    if (_UseStep > 0.5)
        a = CM_Smooth01(saturate(_StepMin * (a + _StepCol.w - 1.0) / max(1.0 - _StepMin, 1e-4))) * _StepCol.x;
    else
        a *= _AlphaScale;
    return a;
}

// Soft depth fade: 0 where the surface meets scene geometry, 1 from _DepDist (Unity units) on.
half CM_SoftFade(float4 screenPos)
{
    half f = 1;
    if (_SoftFade > 0.5)
    {
        float scene = LinearEyeDepth(SAMPLE_DEPTH_TEXTURE_PROJ(_CameraDepthTexture, UNITY_PROJ_COORD(screenPos)));
        f = CM_Smooth01(saturate((scene - screenPos.z) / max(_DepDist, 1e-4)));
    }
    return f;
}

// The cutout texture's value (rgb, and the chosen channel in a) times _CutoutScale.
half4 CM_CutoutValue(float4 uv01, float4 uv23)
{
    half4 t = CM_TEX(_CutoutTex, CM_PickUV(uv01, uv23, _CutoutUV));
    float ch = _CutoutChannel;
    half v = dot(t, half4(ch < 0.5, abs(ch - 1) < 0.5, abs(ch - 2) < 0.5, ch > 2.5));
    return half4(t.rgb, v) * _CutoutScale;
}

void CM_Cutout(float4 uv01, float4 uv23)
{
    if (_UseCutout > 0.5) clip(CM_CutoutValue(uv01, uv23).a - 0.5);
}

// The burn edge's glow (GW2 burnoff / burncol).
half3 CM_Burn(float4 uv01, float4 uv23)
{
    half3 r = 0;
    if (_UseCutout > 0.5 && any(_BurnColor.rgb > 0))
    {
        half3 e = saturate((CM_CutoutValue(uv01, uv23).rgb + _BurnOffset - 1.0) * -10.0);
        r = e * e * (3.0 - 2.0 * e) * _BurnColor.rgb * 2.0;
    }
    return r;
}

half3 CM_Emission(float4 uv01, float4 uv23)
{
    half3 r = 0;
    if (_UseEmission > 0.5)
    {
        float2 euv = CM_PickUV(uv01, uv23, _EmissionMapUV);
        if (_EmissionPtrb != 0) euv += CM_Perturb(uv01, uv23) * _EmissionPtrb;
        half3 e = CM_TEX(_EmissionMap, euv).rgb;
        half4 m = CM_TEX(_EmissionMask, CM_PickUV(uv01, uv23, _EmissionMaskUV));
        float ch = _EmissionMaskChannel;
        half4 sel = half4(ch < 0.5, abs(ch - 1) < 0.5, abs(ch - 2) < 0.5, ch > 2.5);
        r = e * dot(m, sel) * _EmissionColor.rgb * _EmissionStrength;
    }
    return r + CM_Burn(uv01, uv23);
}

#if defined(CM_PASS_FORWARDBASE) || defined(CM_PASS_FORWARDADD)

struct v2f
{
    float4 pos : SV_POSITION;
    float4 uv01 : TEXCOORD0;
    float4 uv23 : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float3 worldNormal : TEXCOORD3;
    float4 worldTangent : TEXCOORD4;
    UNITY_LIGHTING_COORDS(5, 6)
    UNITY_FOG_COORDS(7)
    float4 screenPos : TEXCOORD8;
    half4 color : TEXCOORD11;
    #if defined(CM_PASS_FORWARDBASE)
        float4 lmap : TEXCOORD9;
        half3 vlight : TEXCOORD10;
    #endif
    UNITY_VERTEX_OUTPUT_STEREO
};

struct CMSurface
{
    half3 albedo;
    half alpha;
    half baseAlpha;   // the colour's own alpha, before the shaping
    float3 normal;
    half metallic, smoothness, reflMask, specMask;
};

v2f vert(appdata v)
{
    v2f o;
    UNITY_INITIALIZE_OUTPUT(v2f, o);
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
    o.pos = UnityObjectToClipPos(v.vertex);
    o.worldPos = mul(unity_ObjectToWorld, v.vertex).xyz;
    o.worldNormal = UnityObjectToWorldNormal(v.normal);
    o.worldTangent = float4(UnityObjectToWorldDir(v.tangent.xyz), v.tangent.w * unity_WorldTransformParams.w);
    CM_AnimatedUVs(v, o.uv01, o.uv23);
    o.color = v.color;
    o.screenPos = ComputeScreenPos(o.pos);
    COMPUTE_EYEDEPTH(o.screenPos.z);
    #if defined(CM_PASS_FORWARDBASE)
        o.lmap = 0;
        #if defined(LIGHTMAP_ON)
            o.lmap.xy = v.uv1 * unity_LightmapST.xy + unity_LightmapST.zw;
        #endif
        o.vlight = 0;
        #if defined(VERTEXLIGHT_ON)
            o.vlight = Shade4PointLights(unity_4LightPosX0, unity_4LightPosY0, unity_4LightPosZ0,
                                         unity_LightColor[0].rgb, unity_LightColor[1].rgb, unity_LightColor[2].rgb, unity_LightColor[3].rgb,
                                         unity_4LightAtten0, o.worldPos, o.worldNormal);
        #endif
    #endif
    UNITY_TRANSFER_LIGHTING(o, v.uv1);
    UNITY_TRANSFER_FOG(o, o.pos);
    return o;
}

// A Unity UV in GW2's convention (v down; the glTF import flipped it) and back for the sample.
float2 CM_FireG(float2 uv) { return float2(uv.x, 1.0 - uv.y); }
#define CM_FIRE_TEX(tex, g) CM_TEX(tex, float2((g).x, 1.0 - (g).y))

// GW2's animated UV of the fire shaders: scaled by a triangle wave between p.z and p.w (p.y
// cycles per second), scrolled in v by frac(t p.x).
float2 CM_FireUV(float2 g, float4 p, float t)
{
    float s = abs(frac(t * p.y) * 2.0 - 1.0) * (p.w - p.z) + p.z;
    return float2(g.x * s, g.y * s + frac(t * p.x));
}

// smoothstep01(saturate((x - lo) / (hi - lo))), a zero range kept finite.
half CM_FireStep(half x, float lo, float hi)
{
    float d = hi - lo;
    d = abs(d) < 1e-5 ? 1e-5 : d;
    return CM_Smooth01(saturate((x - lo) / d));
}

// GW2 does the fire in gamma space: its colour to the project's space (coverage stays linear).
half3 CM_FireColour(half3 c) { return IsGammaSpace() ? c : pow(max(c, 0), 2.2); }

// The fire (see the header): premultiplied rgb and the blend alpha (not yet saturated).
// n: the unflipped world normal (GW2's view-space normal fades back faces out).
half4 CM_Fire(v2f i, float3 n)
{
    float t = _Time.y;
    float2 g0 = CM_FireG(i.uv01.xy), g1 = CM_FireG(i.uv01.zw);
    half4 o;
    if (_FireMode < 1.5)
    {
        // GW2 view space is left-handed (+z forward): its normal z = -(Unity view-space z).
        float nz = dot(UNITY_MATRIX_V[2].xyz, n);
        half edge = max(0.0, 1.0 - 1.5 * (1.0 - nz) * (1.0 - nz));
        float2 flow = CM_FIRE_TEX(_FlowTex, CM_FireUV(g1, _FireUV1, t)).xy * 2.0 - 1.0;
        float2 base = CM_FireUV(g1, _FireUV0, t);
        if (_FireMode < 0.5)
        {
            // 740364. _FireParams = (flowspd, firedfl, firesfl, opacity), _FireParams2.xy = ramprmp.
            flow += CM_FIRE_TEX(_FireFlowB, CM_FireUV(g1, _FireUV2, t)).xy * 2.0 - 1.0;
            float p0 = frac(_FireParams.x * t), p1 = frac(_FireParams.x * t - 0.5);
            half w = abs(p0 * 2.0 - 1.0);
            float2 d = flow * _FireParams.y;
            half fire = lerp(CM_FIRE_TEX(_FireDetail, base + d * p0).g, CM_FIRE_TEX(_FireDetail, base + d * p1).g, w);
            half k = CM_Smooth01(saturate((fire + 0.05) * 2.03593));
            float2 ds = flow * lerp(_FireParams2.x, _FireParams2.y, CM_FIRE_TEX(_RampTex, g0).r) * _FireParams.z;
            half shape = lerp(CM_FIRE_TEX(_MainTex, g0 + ds * p0).g, CM_FIRE_TEX(_MainTex, g0 + ds * p1).g, w);
            half cover = k * shape * edge * _FireParams.w;
            o = half4(CM_FireColour(lerp(_FireColorL.rgb, _FireColorH.rgb, fire) * 3.24) * cover, 1.8 * cover);
        }
        else
        {
            // 965703. _FireParams = (rampflw.x, rampflw.y, detailf, fadedif), _FireParams2 = rampstp,
            // _FireParams3 = (stepcol.x, stepcol.y, premult, 0).
            flow += CM_FIRE_TEX(_FlowTex, CM_FireUV(g1, _FireUV1 * 0.4, t)).xy * 2.0 - 1.0;
            half r = CM_FIRE_TEX(_RampTex, g0).g;
            float k = lerp(_FireParams.x, _FireParams.y, r);
            float2 stp = lerp(_FireParams2.xy, _FireParams2.zw, r);
            half shape = CM_FIRE_TEX(_MainTex, g0 + flow * k).g;
            half fire = CM_FIRE_TEX(_FireDetail, base + flow * k * _FireParams.z).g;
            half4 col = lerp(_FireColorL, _FireColorH, CM_FireStep(fire, _FireParams3.x, _FireParams3.y));
            half cover = CM_FireStep(fire, stp.x, stp.y) * col.a * shape * edge * _FireParams.w;
            o = half4(CM_FireColour(col.rgb) * cover, cover * _FireParams3.z);
        }
    }
    else
    {
        // 977200 / 1171331 (GW2 vs 0: v0 = (uv0, uv3), v1 = (uv2, uv1), v3 = the vertex colour).
        // _FireParams = (perturb, scalep, speedp, speed), _FireParams2.xy = (scale, scaleb).
        float2 g2 = CM_FireG(i.uv23.xy), g3 = CM_FireG(i.uv23.zw);
        float pt = _FireParams.z * t;
        float2 q = (g0 + g3) * _FireParams.y;
        float2 a = CM_FIRE_TEX(_FlowTex, float2(q.x, q.y + 2.0 * pt) * 2.0).xy * 2.0 - 1.0;
        float2 b = CM_FIRE_TEX(_FlowTex, float2(q.x, q.y + pt)).xy * 2.0 - 1.0;
        float2 up = g0 + (a + b) * _FireParams.x;
        float2 uk = up * (g2.x + 0.5);
        float sv = t * _FireParams.w;
        half4 p = CM_FIRE_TEX(_MaskTex, up)
                * CM_FIRE_TEX(_MainTex, float2(uk.x * _FireParams2.x, uk.y * _FireParams2.x + g1.x + sv))
                * CM_FIRE_TEX(_MainTex, float2(uk.x * _FireParams2.y, uk.y * _FireParams2.y + g1.y + sv)) * 4.0;
        half3 col;
        if (_FireMode < 2.5) col = saturate(1.0 - p.rgb) * _FireColorL.rgb + _FireColorH.rgb * p.rgb;
        else col = lerp(_FireColorL.rgb, _FireColorH.rgb, p.r) * normalize(max(_FireDye.rgb, 0.001));
        o = half4(CM_FireColour(p.rgb * col * i.color.rgb * 2.0) * i.color.a, p.a * i.color.a);
    }
    return o;
}

CMSurface CM_Surface(v2f i, float facing, float3 viewDir)
{
    CMSurface s;
    float3 n = normalize(i.worldNormal);
    float3 t = i.worldTangent.xyz;
    t = dot(t, t) > 1e-8 ? normalize(t) : float3(0, 0, 0);  // a mesh without tangents keeps its normal
    float3 b = cross(n, t) * i.worldTangent.w;
    float2 viewTS = float2(dot(t, viewDir), dot(b, viewDir));

    half3 tn = half3(0, 0, 1);
    if (_UseNormalMap > 0.5)
    {
        float2 nuv = CM_PickUV(i.uv01, i.uv23, _BumpMapUV);
        if (_PerturbNormal > 0.5) nuv += CM_Perturb(i.uv01, i.uv23) * _BasePtrb;
        tn = UnpackScaleNormal(CM_TEX(_BumpMap, nuv), _BumpScale);
    }
    if (_UseFlow > 0.5)
    {
        float2 fuv = (i.uv01.xy + float2(_Time.y * _Flow.w, 0)) * _Flow.x;
        float k = pow(abs(i.color.r), _Flow.z) * _Flow.y;
        cm_FlowOffset = (CM_TEX(_FlowTex, fuv).xy * 2.0 - 1.0) * k;
    }
    half4 base;
    if (_UseFire > 0.5)
    {
        base = CM_Fire(i, n) * CM_SoftFade(i.screenPos);
        base.a = saturate(base.a);   // GW2's UNORM target clamps 1.8 K / premult > 1
    }
    else
        base = CM_Colour(i.uv01, i.uv23, viewTS, tn.xy);
    if (_UseVertexColor > 0.5) base *= i.color;
    half flowAlpha = 1;
    if (_UseFlow > 0.5)
    {
        flowAlpha = CM_Smooth01(saturate((base.a - i.color.g) / max(i.color.b - i.color.g, 1e-4))) * i.color.a;
        flowAlpha *= CM_SoftFade(i.screenPos);
        half t = CM_Smooth01(saturate((flowAlpha - _FlowStep.x) / max(_FlowStep.y - _FlowStep.x, 1e-4)));
        base.rgb *= lerp(_FlowStart.rgb, _FlowEnd.rgb, t) * unity_ColorSpaceDouble.rgb;
        if (_FlowPremul > 0.5) base.rgb *= base.a;
        base.a = flowAlpha;
    }
    if (_UseRamp > 0.5 && _RampRGBA > 0.5) base *= CM_TEX_CLAMP(_RampTex, saturate(abs(dot(n, viewDir))).xx);
    CM_Cutout(i.uv01, i.uv23);
    s.albedo = base.rgb;
    s.baseAlpha = base.a;
    s.alpha = (_UseFlow > 0.5 || _UseFire > 0.5) ? base.a : CM_Alpha(base.a, i.uv01, i.uv23, abs(dot(n, viewDir))) * CM_SoftFade(i.screenPos);
    if (_AlphaTest > 0.5) clip(s.alpha - _Cutoff);

    if (_UseNormalMap > 0.5) n = normalize(tn.x * t + tn.y * b + tn.z * n);
    s.normal = n * facing;  // two-sided (Cull Off) back faces

    half4 p = half4(1, 1, 1, 1);
    if (_UsePacked > 0.5) p = CM_TEX(_PackedMap, CM_PickUV(i.uv01, i.uv23, _PackedMapUV));
    s.metallic = saturate(p.r * _Metallic);
    s.smoothness = saturate(p.g * _Glossiness);
    s.reflMask = p.b;
    s.specMask = p.a;
    return s;
}

// The diffuse and specular colours, and the alpha written (premultiplied when asked).
half3 CM_Colours(CMSurface s, out half3 specColor, out half oneMinusReflectivity, out half outAlpha)
{
    half3 diffColor = DiffuseAndSpecularFromMetallic(s.albedo, s.metallic, specColor, oneMinusReflectivity);
    specColor *= s.specMask * _SpecularTint.rgb * _SpecularStrength;
    outAlpha = s.alpha;
    if (_AlphaPremultiply > 0.5)
    {
        // As Standard's _ALPHAPREMULTIPLY_ON: diffuse fades with alpha, reflections stay.
        diffColor *= s.alpha;
        outAlpha = 1 - oneMinusReflectivity + s.alpha * oneMinusReflectivity;
    }
    return diffColor;
}

#endif

#if defined(CM_PASS_FORWARDBASE)

half3 CM_Probe(float3 dir)
{
    half4 p = UNITY_SAMPLE_TEXCUBE_LOD(unity_SpecCube0, dir, 0);
    return DecodeHDR(p, unity_SpecCube0_HDR);
}

// Unity world direction -> GW2's (x, y north, z down).
float3 CM_ToGW2(float3 d) { return float3(d.x, d.z, -d.y); }

// One of the water's cubes at a GW2-space direction (the probe without them).
half3 CM_WaterCube(bool reflectCube, float3 g)
{
    half3 c = CM_Probe(float3(g.x, -g.z, g.y));
    if (_UseWaterCubes > 0.5)
        c = reflectCube ? UNITY_SAMPLE_TEXCUBE_SAMPLER(_ReflCube, _trilinear_repeat, g).rgb
                        : UNITY_SAMPLE_TEXCUBE_SAMPLER(_ProjCube, _trilinear_repeat, g).rgb;
    return c;
}

// GW2 3121604's stylised water (see the header).
half3 CM_Water(v2f i, float3 n, float3 v)
{
    float t = _Time.y;
    float3 inc = -v;
    float3 rdir = CM_ToGW2(refract(inc, n, 0.8));
    float f0 = frac(t * 0.5 + 0.5), f1 = frac(t * 0.5);
    half3 a = CM_WaterCube(false, rdir + float3(0, f0, -0.5 * f0));
    half3 b = CM_WaterCube(false, rdir + float3(0, f1, -0.5 * f1));
    half3 col = lerp(b, a, abs(f1 * 2.0 - 1.0));
    float ndv = saturate(dot(n, v));
    float e1 = CM_Smooth01(saturate((ndv - _WaterFalloff.x) / max(_WaterFalloff.y - _WaterFalloff.x, 1e-4)));
    float e2 = CM_Smooth01(saturate((ndv - _WaterFalloff.z) / max(_WaterFalloff.w - _WaterFalloff.z, 1e-4)));
    col = lerp(col, _WaterEdge.rgb * e1, CM_Smooth01(saturate((1.0 - ndv) * 0.769231)));
    col += CM_WaterCube(true, CM_ToGW2(reflect(inc, n))) * half3(0.2, 0.3, 1.0);
    // Triplanar caustics in GW2 units.
    float3 p = i.worldPos / max(_UnityScale, 1e-6);
    float3 w = max((abs(n) - 0.2) * 7.0, 0);
    w /= max(w.x + w.y + w.z, 1e-4);
    float2 sxy = t * _ScrollSpeed.xy, szw = t * _ScrollSpeed.zw;
    float2 flow = CM_TEX(_FlowTex, p.zy * _FlowScale + szw).xy * w.x + CM_TEX(_FlowTex, p.xz * _FlowScale + szw).xy * w.y +
                  CM_TEX(_FlowTex, p.xy * _FlowScale + szw).xy * w.z;
    float2 o = sxy + flow * _FlowStrength;
    half3 caus = UNITY_SAMPLE_TEX2D(_MainTex, p.zy * _CausScale + o).rgb * w.x + UNITY_SAMPLE_TEX2D(_MainTex, p.xz * _CausScale + o).rgb * w.y +
                 UNITY_SAMPLE_TEX2D(_MainTex, p.xy * _CausScale + o).rgb * w.z;
    return col + caus * e2 * _WaveColor.rgb;
}

half4 fragBase(v2f i, fixed facing : VFACE) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
    float3 viewDir = normalize(UnityWorldSpaceViewDir(i.worldPos));
    CMSurface s = CM_Surface(i, facing > 0 ? 1.0 : -1.0, viewDir);
    fixed4 fogColor = _AdditiveFog > 0.5 ? fixed4(0, 0, 0, 0) : unity_FogColor;
    if (_UseFogNeutral > 0.5) fogColor = fixed4(_FogNeutral.xxx, 1);

    if (_UseEnvCube > 0.5)
    {
        float3 g = CM_ToGW2(-viewDir) - 0.03 * CM_TEX(_PerturbTex, CM_PickUV(i.uv01, i.uv23, _PerturbUV)).rgb;
        half4 e = UNITY_SAMPLE_TEXCUBE_SAMPLER(_EnvCube, _trilinear_repeat, g);
        half3 ec = e.rgb / max(e.a, 1e-4);
        half4 ecol = half4(ec * ec, 1);
        UNITY_APPLY_FOG_COLOR(i.fogCoord, ecol, fogColor);
        return ecol;
    }
    if (_UseBumpShade > 0.5)
    {
        float3 L = _WorldSpaceLightPos0.xyz;   // the main directional light (0 without one: no change)
        float3 ng = normalize(i.worldNormal) * (facing > 0 ? 1.0 : -1.0);
        float3 tg = i.worldTangent.xyz;
        tg = dot(tg, tg) > 1e-8 ? normalize(tg) : float3(0, 0, 0);
        float3 bt = cross(ng, tg) * i.worldTangent.w;
        float2 nxy = CM_TEX(_BumpMap, CM_PickUV(i.uv01, i.uv23, _BumpMapUV)).rg * 2.0 - 1.0;
        float3 nm = normalize(nxy.x * tg + nxy.y * bt + sqrt(saturate(1.0 - dot(nxy, nxy))) * ng);
        half d = dot(L, nm) - dot(L, ng);
        half3 rgb = d.xxx;
        if (_UseBumpShadeTex > 0.5)
        {
            half3 t = CM_TEX(_BumpShadeTex, CM_PickUV(i.uv01, i.uv23, _BumpShadeUV)).rgb;
            #if !defined(UNITY_COLORSPACE_GAMMA)
                t = LinearToGammaSpace(t);
            #endif
            rgb *= 2.0 * t * _BumpShadeOpacity;
        }
        half4 b = half4(saturate(rgb + 0.5), saturate(d + 0.5));
        UNITY_APPLY_FOG_COLOR(i.fogCoord, b, fogColor);
        #if !defined(UNITY_COLORSPACE_GAMMA)
            // 2x multiply: GW2 scales the gamma-space target by 2b; linear needs (2b)^2.2.
            b.rgb = 0.5 * GammaToLinearSpace(2.0 * b.rgb);
        #endif
        return b;
    }
    if (_UseWater > 0.5)
    {
        half4 wcol = half4(CM_Water(i, s.normal, viewDir), 1);
        UNITY_APPLY_FOG_COLOR(i.fogCoord, wcol, fogColor);
        return wcol;
    }
    if (_Unlit > 0.5)
    {
        // GW2's effect shaders: the colour as it is, fogged.
        half4 u = half4(s.albedo + CM_Emission(i.uv01, i.uv23), s.alpha);
        if (_PremultipliedBase > 0.5) u.rgb *= s.alpha / max(s.baseAlpha, 1e-4);
        else if (_AlphaPremultiply > 0.5) u.rgb *= s.alpha;
        UNITY_APPLY_FOG_COLOR(i.fogCoord, u, fogColor);
        return u;
    }

    half3 specColor;
    half oneMinusReflectivity, outAlpha;
    half3 diffColor = CM_Colours(s, specColor, oneMinusReflectivity, outAlpha);

    UNITY_LIGHT_ATTENUATION(atten, i, i.worldPos);
    UnityLight light;
    light.color = _LightColor0.rgb * atten;
    light.dir = normalize(UnityWorldSpaceLightDir(i.worldPos));
    light.ndotl = saturate(dot(s.normal, light.dir));

    UnityIndirect ind;
    ind.diffuse = 0;
    ind.specular = 0;
    float3 L0 = 0, L1r = 0, L1g = 0, L1b = 0, lvSpec = 0;
    float3 lvOffset = s.normal * _LightVolumesBias;
    #if defined(LIGHTMAP_ON)
        half3 lm = DecodeLightmap(UNITY_SAMPLE_TEX2D(unity_Lightmap, i.lmap.xy));
        #if defined(DIRLIGHTMAP_COMBINED)
            fixed4 lmDir = UNITY_SAMPLE_TEX2D_SAMPLER(unity_LightmapInd, unity_Lightmap, i.lmap.xy);
            lm = DecodeDirectionalLightmap(lm, lmDir, s.normal);
        #endif
        ind.diffuse = lm;
        if (_LightVolumes > 0.5)
        {
            LightVolumeAdditiveSHSpecular(i.worldPos, L0, L1r, L1g, L1b, lvSpec, s.albedo, s.smoothness, s.metallic,
                                          s.normal, viewDir, lvOffset, _PointLightVolumesShading);
            ind.diffuse += max(0, LightVolumeEvaluate(s.normal, L0, L1r, L1g, L1b));
        }
    #else
        if (_LightVolumes > 0.5 && LightVolumesEnabled() > 0.5)
        {
            LightVolumeSHSpecular(i.worldPos, L0, L1r, L1g, L1b, lvSpec, s.albedo, s.smoothness, s.metallic,
                                  s.normal, viewDir, lvOffset, _PointLightVolumesShading);
            ind.diffuse = max(0, LightVolumeEvaluate(s.normal, L0, L1r, L1g, L1b));
        }
        else
            ind.diffuse = max(0, ShadeSH9(float4(s.normal, 1)));
    #endif

    UnityGIInput d;
    UNITY_INITIALIZE_OUTPUT(UnityGIInput, d);
    d.worldPos = i.worldPos;
    d.worldViewDir = viewDir;
    d.probeHDR[0] = unity_SpecCube0_HDR;
    d.probeHDR[1] = unity_SpecCube1_HDR;
    #if defined(UNITY_SPECCUBE_BLENDING) || defined(UNITY_SPECCUBE_BOX_PROJECTION)
        d.boxMin[0] = unity_SpecCube0_BoxMin;  // .w: the blend factor
    #endif
    #if defined(UNITY_SPECCUBE_BOX_PROJECTION)
        d.boxMax[0] = unity_SpecCube0_BoxMax;
        d.probePosition[0] = unity_SpecCube0_ProbePosition;
        d.boxMax[1] = unity_SpecCube1_BoxMax;
        d.boxMin[1] = unity_SpecCube1_BoxMin;
        d.probePosition[1] = unity_SpecCube1_ProbePosition;
    #endif
    Unity_GlossyEnvironmentData g = UnityGlossyEnvironmentSetup(s.smoothness, viewDir, s.normal, specColor);
    ind.specular = UnityGI_IndirectSpecular(d, 1.0, g) * s.reflMask * _ReflectionTint.rgb * _ReflectionStrength;

    half4 c = UNITY_BRDF_PBS(diffColor, specColor, oneMinusReflectivity, s.smoothness, s.normal, viewDir, light, ind);
    c.rgb += diffColor * i.vlight;
    if (_LightVolumeSpecular > 0.5) c.rgb += lvSpec * s.specMask * _SpecularTint.rgb * _SpecularStrength;
    c.rgb += CM_Emission(i.uv01, i.uv23);
    if (_UseRim > 0.5)
    {
        half k = _RimShineFade > 0.5 ? 1.0 - s.smoothness : 1.0;
        if (_RimUseMask > 0.5) k *= saturate(1.0 - CM_Mask(i.uv01, i.uv23, float2(0, 0), _MaskChannel));
        c.rgb += _LightColor0.rgb * _RimColor.rgb * k * pow(1.0 - saturate(dot(s.normal, viewDir)), max(_RimPower * (_RimShineFade > 0.5 ? 1.0 - s.smoothness : 1.0), 1e-3));
    }
    c.a = outAlpha;
    UNITY_APPLY_FOG_COLOR(i.fogCoord, c, fogColor);
    return c;
}

#endif

#if defined(CM_PASS_FORWARDADD)

half4 fragAdd(v2f i, fixed facing : VFACE) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
    if (_Unlit > 0.5) return 0;  // an unlit effect takes no added lights
    float3 viewDir = normalize(UnityWorldSpaceViewDir(i.worldPos));
    CMSurface s = CM_Surface(i, facing > 0 ? 1.0 : -1.0, viewDir);
    half3 specColor;
    half oneMinusReflectivity, outAlpha;
    half3 diffColor = CM_Colours(s, specColor, oneMinusReflectivity, outAlpha);

    UNITY_LIGHT_ATTENUATION(atten, i, i.worldPos);
    UnityLight light;
    light.color = _LightColor0.rgb * atten;
    light.dir = normalize(UnityWorldSpaceLightDir(i.worldPos));
    light.ndotl = saturate(dot(s.normal, light.dir));
    UnityIndirect ind;
    ind.diffuse = 0;
    ind.specular = 0;

    half4 c = UNITY_BRDF_PBS(diffColor, specColor, oneMinusReflectivity, s.smoothness, s.normal, viewDir, light, ind);
    c.a = outAlpha;
    UNITY_APPLY_FOG_COLOR(i.fogCoord, c, fixed4(0, 0, 0, 0));  // added light fogs to black
    return c;
}

#endif

#if defined(CM_PASS_SHADOWCASTER)

struct v2fShadow
{
    V2F_SHADOW_CASTER;
    float4 uv01 : TEXCOORD1;
    float4 uv23 : TEXCOORD2;
    UNITY_VERTEX_OUTPUT_STEREO
};

v2fShadow vertShadow(appdata v)
{
    v2fShadow o;
    UNITY_INITIALIZE_OUTPUT(v2fShadow, o);
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
    TRANSFER_SHADOW_CASTER_NORMALOFFSET(o)
    CM_AnimatedUVs(v, o.uv01, o.uv23);
    return o;
}

float4 fragShadow(v2fShadow i) : SV_Target
{
    UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i);
    CM_Cutout(i.uv01, i.uv23);
    if (_AlphaTest > 0.5) clip(CM_Colour(i.uv01, i.uv23, float2(0, 0), float2(0, 0)).a - _Cutoff);
    SHADOW_CASTER_FRAGMENT(i)
}

#endif

#if defined(CM_PASS_META)

struct v2fMeta
{
    float4 pos : SV_POSITION;
    float4 uv01 : TEXCOORD0;
    float4 uv23 : TEXCOORD1;
};

v2fMeta vertMeta(appdata v)
{
    v2fMeta o;
    UNITY_INITIALIZE_OUTPUT(v2fMeta, o);
    o.pos = UnityMetaVertexPosition(v.vertex, v.uv1, v.uv2, unity_LightmapST, unity_DynamicLightmapST);
    CM_AnimatedUVs(v, o.uv01, o.uv23);
    return o;
}

half4 fragMeta(v2fMeta i) : SV_Target
{
    UnityMetaInput m;
    UNITY_INITIALIZE_OUTPUT(UnityMetaInput, m);
    m.Albedo = CM_Colour(i.uv01, i.uv23, float2(0, 0), float2(0, 0)).rgb;
    m.Emission = CM_Emission(i.uv01, i.uv23);
    return UnityMetaFragment(m);
}

#endif

#endif // CASTLEMIST_GW2_CORE_INCLUDED
