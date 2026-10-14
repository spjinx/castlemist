/// @file
/// @brief HLSL for WorldView (adapted from shaders::kMapFly in detail/shaders.h).
///
/// @warning Not a public header -- internal to `src/render/`.

#pragma once

namespace castlemist::render::world_shaders {

/// @brief Terrain: map-space MapVtx (no instance stream), one albedo per draw.
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

} // namespace castlemist::render::world_shaders
