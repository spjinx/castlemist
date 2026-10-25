// castlemist: what GW2's sky layers share (docs/research/gw2-sky.md §9.4, §10.4), for Castlemist/GW2 Cloud
// and Castlemist/GW2 Sky Card: the map-axes helpers, the sun / ambient / fog inputs and the lighting.
//   g    = ((1 - v.z) + sunDir.z v.z) (dot(sunDir, v) 0.25 + 0.75)^2
//   amb  = dot((-v, 1), shRed / shGreen / shBlue)
//   C    = lerp(amb, sunClr.rgb, g); X = C T + lum(T)^2 (C + T - 2 C T); M = (X + T) / 2; G = lerp(M, X, g)
//   col  = T + sunClr.w (G - T)
// v is the card's d (camera -> card) or the cloud's n (camera - P), map axes.
// Inputs the research leaves UNPROVEN (2026-10-25, castlemist's choice, matched by eye to an in-game
// shot of map 506592): sunClr = the manifest's GW2 sun colour x intensity; the ambient is a constant
// _Ambient (the importer sets the same light) or Unity's own L0 + L1 SH (_SceneSH). Maths in gamma
// space, as GW2: Color properties (linearised by Unity in a linear project) are taken back to gamma,
// and the output goes to linear with pow 2.2. The output is clamped to [0, 1], as a UNORM target
// clamps it (the client's target format is not traced; sky_bake assumes the same clamp).
#ifndef CASTLEMIST_GW2SKY_INCLUDED
#define CASTLEMIST_GW2SKY_INCLUDED

#include "UnityCG.cginc"

float4 _SunDir, _SunColor, _Ambient, _FogFar;
float _SceneSH, _UnityScale;

// GW2 map axes (x, y, z) <-> the map root's local frame (x, -z, y)
float3 MapToLocal(float3 g) { return float3(g.x, -g.z, g.y); }
float3 LocalToMap(float3 l) { return float3(l.x, l.z, -l.y); }

// The camera in the object's (= the map root's) frame, map axes.
float3 GW2SkyCamera() { return LocalToMap(mul(unity_WorldToObject, float4(_WorldSpaceCameraPos, 1)).xyz); }

struct GW2SkyLights { float3 sun, amb, fog; };

// The sun colour, the ambient for direction v (map axes) and FogColorFar, all in gamma.
GW2SkyLights GW2SkyInputs(float3 v)
{
    GW2SkyLights k;
    k.sun = _SunColor.rgb;
    k.amb = _Ambient.rgb;
    k.fog = _FogFar.rgb;
    if (_SceneSH > 0.5)
    {
        float4 d4 = float4(normalize(mul((float3x3)unity_ObjectToWorld, MapToLocal(-v))), 1);
        k.amb = max(float3(dot(unity_SHAr, d4), dot(unity_SHAg, d4), dot(unity_SHAb, d4)), 0);
    }
    if (!IsGammaSpace())
    {
        k.sun = LinearToGammaSpace(k.sun);
        k.amb = LinearToGammaSpace(k.amb);
        k.fog = LinearToGammaSpace(k.fog);
    }
    return k;
}

// §9.4's col for texel T seen along v, with sunClr.w = w.
float3 GW2SkyLit(float3 T, float3 v, float w, GW2SkyLights k)
{
    float3 sd = normalize(_SunDir.xyz + float3(0, 0, 1e-6));
    float s = dot(sd, v) * 0.25 + 0.75;
    float g = ((1 - v.z) + sd.z * v.z) * s * s;
    float3 C = lerp(k.amb, k.sun, g);
    float lum = dot(T, float3(0.3, 0.59, 0.11));
    float3 X = C * T + lum * lum * (C + T - 2 * C * T);
    float3 M = 0.5 * X + 0.5 * T;
    float3 G = lerp(M, X, g);
    return T + w * (G - T);
}

// The final colour: clamped as a UNORM target clamps it, then to the project's colour space.
float3 GW2SkyOut(float3 rgb)
{
    rgb = saturate(rgb);
    return IsGammaSpace() ? rgb : pow(rgb, 2.2);
}

#endif
