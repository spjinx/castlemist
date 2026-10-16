/// @file
/// @brief HLSL for WorldView (adapted from the main window's old mesh-only map view shader).
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

#include <cstring>
#include <string>

namespace castlemist::render::world_shaders {

/// @brief Terrain, untextured (the AO look): map-space TerrainVtx (no instance
///        stream; its (s, t) is not read), one albedo per draw.
///
/// The shading is the fly view's: hemisphere ambient (sky-facing is -Z, GW2's
/// up), one sun, baked AO, distance fog, gamma-encoded into a UNORM target.
inline constexpr const char* kTerrain = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
};

struct VSIn {
    float3 pos : POSITION;   // map space
    float4 nao : NORMAL;     // xyz = normal*0.5+0.5, w = baked AO (R8G8B8A8_UNORM)
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 nrm : NORMAL;
    float2 aod : TEXCOORD0;  // x = AO, y = distance to camera
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), uViewProj);
    o.nrm = i.nao.xyz * 2.0 - 1.0;
    o.aod = float2(i.nao.w, distance(i.pos, uCamPos));
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 N = normalize(i.nrm);
    float ao = lerp(1.0, i.aod.x, uAoStrength);
    // Up is -Z, so a sky-facing normal has N.z = -1.
    float3 hemi = lerp(uGroundCol, uSkyCol, saturate(-N.z * 0.5 + 0.5));
    float  ndl  = saturate(dot(N, uSunDir));
    float3 col  = uAlbedo * (hemi + uSunCol * ndl) * ao * uExposure;
    float f = saturate((i.aod.y - uFogStart) / max(1.0, uFogEnd - uFogStart));
    col = lerp(col, uFogCol, f);
    return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
}
)";

/// @brief Terrain, textured: the lo-res terrain PS's blend (note §1.2, as
///        castlemist::world::terrain_blend transcribes it, line for line) with
///        the note's lighting in forward form (§1.4).
///
/// Reads world_view.cpp's 20-byte TerrainVtx: kTerrain's position and
/// normal+AO, plus the chunk coordinate (s, t) as UNORM16 (s from the west,
/// t from the north). Slots: t0-t3 color, colorb, colorc, colord (sRGB view
/// when srgbColourTextures); t4 blend page, t5 modx page; t6-t9 the normal
/// maps (a missing one is bound flat). s0 wraps (ground), s1 clamps (pages).
inline constexpr const char* kTerrainMat = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
    float3 uMatSunCol;  float _pad3;   // calibrated light (detail/light_calib.h)
    float3 uMatSkyCol;  float _pad4;
};
cbuffer ChunkCB : register(b1) {
    float2 uPickerOffset; float uPickerScale; float uGroundRepeats;
    float4 uPresent;      // 1 where colour slot k binds a texture (terrain_blend's present[k])
    float uNormalMaps;    // terrainNormalMaps
    float uSpecPower;     // terrainSpecPower
    float uAlbedoOnly;    // capture_chunk_albedo: the linear albedo, no light, fog or gamma
    float _cpad;
};

Texture2D tColor0 : register(t0);
Texture2D tColor1 : register(t1);
Texture2D tColor2 : register(t2);
Texture2D tColor3 : register(t3);
Texture2D tBlend  : register(t4);
Texture2D tModx   : register(t5);
Texture2D tNormal0 : register(t6);
Texture2D tNormal1 : register(t7);
Texture2D tNormal2 : register(t8);
Texture2D tNormal3 : register(t9);
SamplerState sWrap  : register(s0);
SamplerState sClamp : register(s1);

struct VSIn {
    float3 pos : POSITION;   // map space
    float4 nao : NORMAL;     // xyz = normal*0.5+0.5, w = baked AO
    float2 st  : TEXCOORD0;  // chunk coordinate: s from the west, t from the north
};
struct VSOut {
    float4 pos  : SV_POSITION;
    float3 nrm  : NORMAL;
    float3 wp   : TEXCOORD0;
    float2 page : TEXCOORD1;  // page_uv
    float2 grnd : TEXCOORD2;  // ground_uv
    float  ao   : TEXCOORD3;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos  = mul(float4(i.pos, 1.0), uViewProj);
    o.nrm  = i.nao.xyz * 2.0 - 1.0;
    o.wp   = i.pos;
    o.page = uPickerOffset + uPickerScale * i.st;
    o.grnd = i.st * uGroundRepeats;
    o.ao   = i.nao.w;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 pk = tBlend.Sample(sClamp, i.page);
    float4 mx = tModx.Sample(sClamp, i.page);
    float4 g[4];
    g[0] = tColor0.Sample(sWrap, i.grnd);
    g[1] = tColor1.Sample(sWrap, i.grnd);
    g[2] = tColor2.Sample(sWrap, i.grnd);
    g[3] = tColor3.Sample(sWrap, i.grnd);

    // terrain_blend: picker R weights ground[3], G ground[2], B ground[1], A ground[0].
    float4 w = float4(pk.a, pk.b, pk.g, pk.r);
    bool any = false;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (uPresent[k] == 0) w[k] = 0;
        if (w[k] != 0) any = true;
    }
    if (!any) {
        int s = 0;
        if (uPresent[0] == 0) {
            if (uPresent[1] != 0) s = 1;
            else if (uPresent[2] != 0) s = 2;
            else if (uPresent[3] != 0) s = 3;
        }
        w = float4(s == 0, s == 1, s == 2, s == 3) * (1.0 / 255.0);
    }
    float4 kw;
    float d = 0;
    [unroll] for (int k1 = 0; k1 < 4; ++k1) {
        kw[k1] = w[k1] * (g[k1].a + 0.01);
        d += kw[k1];
    }
    kw /= d;
    float3 a = kw[0] * g[0].rgb + kw[1] * g[1].rgb + kw[2] * g[2].rgb + kw[3] * g[3].rgb;
    float lum = 0.30 * a.r + 0.59 * a.g + 0.11 * a.b;
    float3 alb = 2.0 * mx.r * (lum + (a - lum) * (2.0 * mx.g));
    if (uAlbedoOnly != 0) return float4(alb, 1.0);

    // nrm = the same weights over the normal maps (RGBA; a = specular mask).
    float4 nm = kw[0] * tNormal0.Sample(sWrap, i.grnd) + kw[1] * tNormal1.Sample(sWrap, i.grnd) +
                kw[2] * tNormal2.Sample(sWrap, i.grnd) + kw[3] * tNormal3.Sample(sWrap, i.grnd);
    float3 Ng = normalize(i.nrm);
    float3 N = Ng;
    if (uNormalMaps != 0) {
        // Tangent +X (east), bitangent -Y (increasing t, south), Gram-Schmidt on Ng.
        float3 T = normalize(float3(1, 0, 0) - Ng * Ng.x);
        float3 B = float3(0, -1, 0) - Ng * dot(Ng, float3(0, -1, 0));
        B = normalize(B - T * dot(T, B));
        float3 tn = normalize(nm.xyz * 2.0 - 1.0);
        N = normalize(T * tn.x + B * tn.y + Ng * tn.z);
    }
    float3 V = normalize(uCamPos - i.wp);
    float3 H = normalize(uSunDir + V);
    float ao = lerp(1.0, i.ao, uAoStrength);
    // L.rgb: the calibrated hemisphere + sun (up is -Z; ruling P6); L.a: the sun specular.
    float3 hemi = lerp(uGroundCol, uMatSkyCol, saturate(-N.z * 0.5 + 0.5));
    float3 Lrgb = (hemi + uMatSunCol * saturate(dot(N, uSunDir))) * ao * uExposure;
    float  La   = pow(saturate(dot(N, H)), uSpecPower);
    float  fres = pow(1.0 - saturate(dot(N, V)), 8.0);
    // shSunColor.w taken as 1 (UNPROVEN).
    float3 col = alb * Lrgb + nm.a * 0.854902 * uMatSunCol * La + saturate(2.0 * mx.b - 1.0) * 0.239216 * uMatSunCol * fres;
    float f = saturate((distance(i.wp, uCamPos) - uFogStart) / max(1.0, uFogEnd - uFogStart));
    col = lerp(col, uFogCol, f);
    return float4(pow(saturate(col), 1.0 / 2.2), 1.0);
}
)";

/// @brief Props, collision and water: model-space MapVtx (slot 0) plus a
///        per-instance stream (slot 1) of three world columns and a tint.
///
/// The constant buffer is kTerrain's (uAlbedo unused: the tint comes per
/// instance); tint.a is the output alpha, 1 for opaque layers.
inline constexpr const char* kInstanced = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
};

struct VSIn {
    float3 pos : POSITION;   // model space
    float4 nao : NORMAL;     // xyz = normal*0.5+0.5, w = baked AO
    float4 r0  : TEXCOORD0;  // world columns: map = (dot(p,r0), dot(p,r1), dot(p,r2))
    float4 r1  : TEXCOORD1;
    float4 r2  : TEXCOORD2;
    float4 tint: TEXCOORD3;  // rgb albedo, a = alpha
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 nrm : NORMAL;
    float2 aod : TEXCOORD0;  // x = AO, y = distance to camera
    float4 tint: TEXCOORD1;
};

VSOut VSMain(VSIn i) {
    float4 p = float4(i.pos, 1.0);
    float3 wp = float3(dot(p, i.r0), dot(p, i.r1), dot(p, i.r2));
    float3 n  = i.nao.xyz * 2.0 - 1.0;
    // As the fly view: the world columns rotate the normal too (exact for
    // uniform scale; the normalize absorbs the scale).
    float3 wn = float3(dot(n, i.r0.xyz), dot(n, i.r1.xyz), dot(n, i.r2.xyz));
    VSOut o;
    o.pos  = mul(float4(wp, 1.0), uViewProj);
    o.nrm  = wn;
    o.aod  = float2(i.nao.w, distance(wp, uCamPos));
    o.tint = i.tint;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float3 N = normalize(i.nrm);
    float ao = lerp(1.0, i.aod.x, uAoStrength);
    float3 hemi = lerp(uGroundCol, uSkyCol, saturate(-N.z * 0.5 + 0.5));   // up is -Z
    float  ndl  = saturate(dot(N, uSunDir));
    float3 col  = i.tint.rgb * (hemi + uSunCol * ndl) * ao * uExposure;
    float f = saturate((i.aod.y - uFogStart) / max(1.0, uFogEnd - uFogStart));
    col = lerp(col, uFogCol, f);
    return float4(pow(saturate(col), 1.0 / 2.2), i.tint.a);
}
)";

/// @brief Props, textured (spec PropMaterialPass): kInstanced's streams with
///        MatVtx's two UV sets, one diffuse per draw.
///
/// Shading: the diffuse rgb (sRGB view when srgbColourTextures) x AO x
/// (hemi + sun) with the calibrated light uMatSunCol / uMatSkyCol (ruling P6,
/// detail/light_calib.h; the ground term and exposure as kInstanced), then
/// fog. ModelMaterialCPU::tint is not applied (model_preview's guess). Slot t0
/// is the diffuse, s0 wraps. MatCB (b1) per draw: uUvSet picks uv0 or uv1,
/// uUvOffset is added to it (Task 11 scroll), uAlphaMode is the WorldAlpha:
///  0 opaque; 1 clip (discard below alpha 0.25); 2 blend (out alpha = diffuse
///  alpha); 3, 4 additive (rgb faded by fog toward black); 5 premultiplied
///  (fog toward fogCol x alpha); 6, 7 multiply / 2x (castlemist's choice: the
///  layer's own colour, unlit, gamma-encoded like the target, fogged toward the
///  neutral 1 or 0.5). uTextured is always 1: untextured sub-ranges draw with
///  kInstanced, so the AO look stays byte-identical to Geometry.
inline constexpr const char* kPropMat = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
    float3 uMatSunCol;  float _pad3;   // calibrated light (detail/light_calib.h)
    float3 uMatSkyCol;  float _pad4;
};
cbuffer MatCB : register(b1) {
    float2 uUvOffset; float uUvSet; float uAlphaMode;
    float uTextured;  float3 _mpad;   // uTextured: always 1, see above
};

Texture2D tDiffuse : register(t0);
SamplerState sWrap : register(s0);

struct VSIn {
    float3 pos : POSITION;   // model space
    float4 nao : NORMAL;     // xyz = normal*0.5+0.5, w = baked AO
    float2 uv0 : TEXCOORD4;
    float2 uv1 : TEXCOORD5;
    float4 r0  : TEXCOORD0;  // world columns
    float4 r1  : TEXCOORD1;
    float4 r2  : TEXCOORD2;
    float4 tint: TEXCOORD3;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 nrm : NORMAL;
    float2 aod : TEXCOORD0;  // x = AO, y = distance to camera
    float4 tint: TEXCOORD1;
    float2 uv  : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
    float4 p = float4(i.pos, 1.0);
    float3 wp = float3(dot(p, i.r0), dot(p, i.r1), dot(p, i.r2));
    float3 n  = i.nao.xyz * 2.0 - 1.0;
    float3 wn = float3(dot(n, i.r0.xyz), dot(n, i.r1.xyz), dot(n, i.r2.xyz));
    VSOut o;
    o.pos  = mul(float4(wp, 1.0), uViewProj);
    o.nrm  = wn;
    o.aod  = float2(i.nao.w, distance(wp, uCamPos));
    o.tint = i.tint;
    o.uv   = (uUvSet != 0 ? i.uv1 : i.uv0) + uUvOffset;
    return o;
}

float4 PSMain(VSOut i) : SV_Target {
    float4 d = tDiffuse.Sample(sWrap, i.uv);
    if (uAlphaMode == 1 && d.a < 0.25) discard;
    float f = saturate((i.aod.y - uFogStart) / max(1.0, uFogEnd - uFogStart));
    if (uAlphaMode >= 6) {   // multiply (DstColor / Zero) or 2x (DstColor / SrcColor)
        float n = (uAlphaMode == 7) ? 0.5 : 1.0;
        return float4(lerp(pow(saturate(d.rgb), 1.0 / 2.2), n.xxx, f), 1.0);
    }
    float3 N = normalize(i.nrm);
    float ao = lerp(1.0, i.aod.x, uAoStrength);
    float3 hemi = lerp(uGroundCol, uMatSkyCol, saturate(-N.z * 0.5 + 0.5));   // up is -Z
    float  ndl  = saturate(dot(N, uSunDir));
    float3 col  = d.rgb * (hemi + uMatSunCol * ndl) * ao * uExposure;
    if (uAlphaMode == 3 || uAlphaMode == 4) col *= 1.0 - f;
    else if (uAlphaMode == 5) col = lerp(col, uFogCol * d.a, f);
    else col = lerp(col, uFogCol, f);
    float a = (uAlphaMode >= 2) ? d.a : 1.0;
    return float4(pow(saturate(col), 1.0 / 2.2), a);
}
)";

/// @brief Skinned props (spec SkinnedPropPass): kPropMat's streams plus the
///        SkinVtx stream (slot 2: bone indices UINT8x4, weights UNORM8x4),
///        posed by one palette per (model, clip).
///
/// This is the vertex half only; prop_skinned_source() appends kPropMat's
/// PSMain and kInstanced's PSMain (renamed PSAo) verbatim, so a skinned prop
/// shades exactly as a static one in both the textured and the AO look.
///
/// Palette: `StructuredBuffer<float4x4>` at VS t1 (t0 is the PS diffuse),
/// 16 floats per bone from granny::skinMatrix: row-major, row-vector
/// (p' = p P, translation in the last row). A structured buffer's float4x4
/// loads column-major, so the matrix read is P transposed and mul(M, p)
/// is p P. SkinCB (b2) gives the bucket's base and bone count. Weights: an
/// index >= the bone count drops its weight; a sum below 1e-3 is identity
/// (an unskinned mesh); otherwise they are renormalised (Task 7 rounds each
/// one to 1/255, so they sum to 254..256/255).
inline constexpr const char* kPropSkinned = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
    float3 uMatSunCol;  float _pad3;   // calibrated light (detail/light_calib.h)
    float3 uMatSkyCol;  float _pad4;
};
cbuffer MatCB : register(b1) {
    float2 uUvOffset; float uUvSet; float uAlphaMode;
    float uTextured;  float3 _mpad;
};
cbuffer SkinCB : register(b2) {
    uint uPaletteBase; uint uBoneCount; uint2 _spad;
};

StructuredBuffer<float4x4> uPalette : register(t1);
Texture2D tDiffuse : register(t0);
SamplerState sWrap : register(s0);

struct VSIn {
    float3 pos : POSITION;   // model space, bind pose
    float4 nao : NORMAL;     // xyz = normal*0.5+0.5, w = baked AO
    float2 uv0 : TEXCOORD4;
    float2 uv1 : TEXCOORD5;
    float4 r0  : TEXCOORD0;  // world columns
    float4 r1  : TEXCOORD1;
    float4 r2  : TEXCOORD2;
    float4 tint: TEXCOORD3;
    uint4  bone: BLENDINDICES;
    float4 wt  : BLENDWEIGHT;
};
struct VSOut {
    float4 pos : SV_POSITION;
    float3 nrm : NORMAL;
    float2 aod : TEXCOORD0;  // x = AO, y = distance to camera
    float4 tint: TEXCOORD1;
    float2 uv  : TEXCOORD2;
};

VSOut VSMain(VSIn i) {
    float4 w = i.wt;
    [unroll] for (int k = 0; k < 4; ++k)
        if (i.bone[k] >= uBoneCount) w[k] = 0;
    float s = w.x + w.y + w.z + w.w;
    float3 sp = i.pos;
    float3 n  = i.nao.xyz * 2.0 - 1.0;
    if (s >= 1e-3) {
        w /= s;
        float3 ap = 0, an = 0;
        [unroll] for (int k2 = 0; k2 < 4; ++k2) {
            if (w[k2] == 0) continue;
            float4x4 M = uPalette[uPaletteBase + i.bone[k2]];   // P transposed: mul(M, v) = v P
            ap += w[k2] * mul(M, float4(sp, 1.0)).xyz;
            an += w[k2] * mul((float3x3)M, n);
        }
        sp = ap;
        n  = an;
    }
    float4 p = float4(sp, 1.0);
    float3 wp = float3(dot(p, i.r0), dot(p, i.r1), dot(p, i.r2));
    float3 wn = float3(dot(n, i.r0.xyz), dot(n, i.r1.xyz), dot(n, i.r2.xyz));
    VSOut o;
    o.pos  = mul(float4(wp, 1.0), uViewProj);
    o.nrm  = wn;
    o.aod  = float2(i.nao.w, distance(wp, uCamPos));
    o.tint = i.tint;
    o.uv   = (uUvSet != 0 ? i.uv1 : i.uv0) + uUvOffset;
    return o;
}
)";

/// @brief The water plane from its env preset (spec WaterPass, note §6): a
///        reconstruction from the stored fields, not the game's shader (which
///        needs scene depth, a scene-colour copy and a reflection target).
///
/// VS: the water quad's MapVtx positions are map space already (one identity
/// instance), so only POSITION is read. PS:
///  - depth: tHeight (t1, R32F map-wide terrain z, clamp sampler s1) at the
///    pixel's xy; outside the grid the terrain is the plane (depth 0).
///    mixD = saturate((terrainZ - planeZ) / depthAttenuation) (water_depth_mix).
///  - base = lerp(shallow, deep, mixD); the colours are water_colour()'s bytes
///    / 255, decoded sRGB -> linear when uSrgb (srgbColourTextures, as the
///    colour textures).
///  - bump: two samples of tBump (t0, UNORM, wrap s0) at uv_k = M_k xy + off_k,
///    M_k and off_k from water_bump_uv on the CPU (off_k frac'd), xy*2-1;
///    N = normalize(float3((n0.xy + n1.xy) * uBumpAmount, -1)) (up is -Z), uBumpAmount =
///    water_bump_strength(bumpAmount, waterBumpStrengthScale) (ruling P10); no
///    bump texture: N = (0, 0, -1).
///  - fresnel = pow(1 - saturate(N.V), surfaceFresnel).
///  - rgb = base (hemi + sun) + sunCol pow(saturate(N.H), waterSpecPower), with
///    the calibrated light uMatSunCol / uMatSkyCol of the other textured passes
///    (ruling P6, detail/light_calib.h); hemi and the ground term as kPropMat.
///    Then fog and gamma as every pass; alpha = lerp(0.45, 1, fresnel).
inline constexpr const char* kWater = R"(
cbuffer WorldCB : register(b0) {
    row_major float4x4 uViewProj;
    float3 uSunDir;     float uExposure;
    float3 uSunCol;     float uAoStrength;
    float3 uSkyCol;     float uFogStart;
    float3 uGroundCol;  float uFogEnd;
    float3 uFogCol;     float _pad0;
    float3 uCamPos;     float _pad1;
    float3 uAlbedo;     float _pad2;
    float3 uMatSunCol;  float _pad3;   // calibrated light (detail/light_calib.h)
    float3 uMatSkyCol;  float _pad4;
};
cbuffer WaterCB : register(b1) {
    float2 uHeightOrigin; float2 uHeightStep;   // map xy of texel (0,0) (west, north); map units per texel (x, -y)
    float2 uHeightSize;   float uPlaneZ; float uDepthAtt;
    float4 uShallow;      // rgb, w = uSrgb
    float4 uDeep;         // rgb, w = surfaceFresnel
    float4 uBumpM0;       // uv0 = (dot(m.xy, xy), dot(m.zw, xy)) + off.xy
    float4 uBumpM1;
    float4 uBumpOff;      // off0.xy, off1.xy
    float uBumpAmount; float uSpecPower; float uHasBump; float uHasHeight;
};

Texture2D tBump   : register(t0);
Texture2D tHeight : register(t1);
SamplerState sWrap  : register(s0);
SamplerState sClamp : register(s1);

struct VSIn { float3 pos : POSITION; };
struct VSOut {
    float4 pos : SV_POSITION;
    float3 wp  : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), uViewProj);
    o.wp  = i.pos;
    return o;
}

float3 srgb_to_linear(float3 c) {
    return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float4 PSMain(VSOut i) : SV_Target {
    float2 xy = i.wp.xy;
    float terrainZ = uPlaneZ;
    if (uHasHeight != 0) {
        float2 g = float2(xy.x - uHeightOrigin.x, uHeightOrigin.y - xy.y) / uHeightStep;
        if (all(g >= 0) && all(g <= uHeightSize - 1))
            terrainZ = tHeight.SampleLevel(sClamp, (g + 0.5) / uHeightSize, 0).r;
    }
    float mixD = uDepthAtt > 0 ? saturate((terrainZ - uPlaneZ) / uDepthAtt) : 0;
    float3 sh = uShallow.rgb, dp = uDeep.rgb;
    if (uShallow.w != 0) { sh = srgb_to_linear(sh); dp = srgb_to_linear(dp); }
    float3 base = lerp(sh, dp, mixD);

    float3 N = float3(0, 0, -1);
    if (uHasBump != 0) {
        float2 uv0 = float2(dot(uBumpM0.xy, xy), dot(uBumpM0.zw, xy)) + uBumpOff.xy;
        float2 uv1 = float2(dot(uBumpM1.xy, xy), dot(uBumpM1.zw, xy)) + uBumpOff.zw;
        float2 n0 = tBump.Sample(sWrap, uv0).xy * 2.0 - 1.0;
        float2 n1 = tBump.Sample(sWrap, uv1).xy * 2.0 - 1.0;
        N = normalize(float3((n0 + n1) * uBumpAmount, -1.0));
    }
    float3 V = normalize(uCamPos - i.wp);
    float3 L = normalize(uSunDir);
    float3 H = normalize(L + V);
    float fresnel = pow(max(1.0 - saturate(dot(N, V)), 1e-6), max(uDeep.w, 0.0));   // max: no pow(0, 0) NaN
    float3 hemi = lerp(uGroundCol, uMatSkyCol, saturate(-N.z * 0.5 + 0.5));   // up is -Z
    float3 sun  = uMatSunCol * saturate(dot(N, L));
    float3 col  = base * (hemi + sun) * uExposure + uMatSunCol * pow(saturate(dot(N, H)), uSpecPower);
    float f = saturate((distance(i.wp, uCamPos) - uFogStart) / max(1.0, uFogEnd - uFogStart));
    col = lerp(col, uFogCol, f);
    return float4(pow(saturate(col), 1.0 / 2.2), lerp(0.45, 1.0, fresnel));
}
)";

/// @brief kPropSkinned's whole source: its VS, then kPropMat's PSMain
///        (textured) and kInstanced's PSMain as PSAo (the AO look), each
///        copied from its own shader so the two cannot drift apart.
inline std::string prop_skinned_source() {
    auto ps_of = [](const char* src) {
        const std::string s(src);
        const size_t at = s.find("float4 PSMain(");
        return at == std::string::npos ? std::string() : s.substr(at);
    };
    std::string ao = ps_of(kInstanced);
    if (!ao.empty()) ao.replace(0, std::strlen("float4 PSMain"), "float4 PSAo");
    return std::string(kPropSkinned) + ps_of(kPropMat) + ao;
}

} // namespace castlemist::render::world_shaders
