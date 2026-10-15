/// @file
/// @brief HLSL for WorldView (adapted from shaders::kMapFly in detail/shaders.h).
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

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
    // L.rgb: the AO pass's hemisphere + sun (up is -Z); L.a: the sun specular.
    float3 hemi = lerp(uGroundCol, uSkyCol, saturate(-N.z * 0.5 + 0.5));
    float3 Lrgb = (hemi + uSunCol * saturate(dot(N, uSunDir))) * ao * uExposure;
    float  La   = pow(saturate(dot(N, H)), uSpecPower);
    float  fres = pow(1.0 - saturate(dot(N, V)), 8.0);
    // shSunColor.w taken as 1 (UNPROVEN).
    float3 col = alb * Lrgb + nm.a * 0.854902 * uSunCol * La + saturate(2.0 * mx.b - 1.0) * 0.239216 * uSunCol * fres;
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
/// Shading is kInstanced's budget with the diffuse rgb as the albedo
/// (sRGB view when srgbColourTextures): albedo x AO x (hemi + sun), then fog.
/// ModelMaterialCPU::tint is not applied (model_preview's guess). Slot t0 is
/// the diffuse, s0 wraps. MatCB (b1) per draw: uUvSet picks uv0 or uv1,
/// uUvOffset is added to it (Task 11 scroll), uAlphaMode 0 opaque, 1 clip
/// (discard below alpha 0.25), 2 blend (out alpha = diffuse alpha), 3
/// additive (rgb pre-faded by fog toward black, out alpha = diffuse alpha).
/// uTextured 0 falls back to the instance tint (WorldView draws untextured
/// sub-ranges with kInstanced instead, so the AO look stays byte-identical).
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
};
cbuffer MatCB : register(b1) {
    float2 uUvOffset; float uUvSet; float uAlphaMode;
    float uTextured;  float3 _mpad;
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
    if (uTextured == 0) d = float4(i.tint.rgb, 1.0);
    if (uAlphaMode == 1 && d.a < 0.25) discard;
    float3 N = normalize(i.nrm);
    float ao = lerp(1.0, i.aod.x, uAoStrength);
    float3 hemi = lerp(uGroundCol, uSkyCol, saturate(-N.z * 0.5 + 0.5));   // up is -Z
    float  ndl  = saturate(dot(N, uSunDir));
    float3 col  = d.rgb * (hemi + uSunCol * ndl) * ao * uExposure;
    float f = saturate((i.aod.y - uFogStart) / max(1.0, uFogEnd - uFogStart));
    col = (uAlphaMode == 3) ? col * (1.0 - f) : lerp(col, uFogCol, f);
    float a = (uAlphaMode >= 2) ? d.a : 1.0;
    return float4(pow(saturate(col), 1.0 / 2.2), a);
}
)";

} // namespace castlemist::render::world_shaders
