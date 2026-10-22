// castlemist GW2 shaders: VRC Light Volumes (red.sim.lightvolumes) or a stand-in.
//
// By default this includes the package's own LightVolumes.cginc (VRC Light
// Volumes 2.x/3.x public API: LightVolumesEnabled, LightVolumeSHSpecular,
// LightVolumeAdditiveSHSpecular, LightVolumeEvaluate, LightVolumeSH_L0).
// A project without the package fails to compile it: uncomment the define
// below and the shaders use Unity's light probes only, through stubs with the
// same signatures.
//
// #define CASTLEMIST_NO_LIGHT_VOLUMES

#ifndef CASTLEMIST_LIGHT_VOLUMES_INCLUDED
#define CASTLEMIST_LIGHT_VOLUMES_INCLUDED

#include "UnityCG.cginc"

#ifndef CASTLEMIST_NO_LIGHT_VOLUMES

#include "Packages/red.sim.lightvolumes/Shaders/LightVolumes.cginc"

#else

// Stand-ins: never enabled; the SH and L0 fall back to the light probe, the
// additive terms are zero (as the package returns with no volume in the scene).
float LightVolumesEnabled() { return 0; }

float3 LightVolumeEvaluate(float3 worldNormal, float3 L0, float3 L1r, float3 L1g, float3 L1b)
{
    return L0 + float3(dot(L1r, worldNormal), dot(L1g, worldNormal), dot(L1b, worldNormal));
}

float3 LightVolumeSH_L0(float3 worldPos, float3 worldPosOffset)
{
    return float3(unity_SHAr.w, unity_SHAg.w, unity_SHAb.w);
}

void LightVolumeSHSpecular(float3 worldPos, out float3 L0, out float3 L1r, out float3 L1g, out float3 L1b, out float3 specular,
                           float3 albedo, float smoothness, float metallic, float3 worldNormal, float3 viewDir,
                           float3 worldPosOffset, float pointLightShading)
{
    L0 = float3(unity_SHAr.w, unity_SHAg.w, unity_SHAb.w);
    L1r = unity_SHAr.xyz; L1g = unity_SHAg.xyz; L1b = unity_SHAb.xyz;
    specular = 0;
}

void LightVolumeAdditiveSHSpecular(float3 worldPos, out float3 L0, out float3 L1r, out float3 L1g, out float3 L1b, out float3 specular,
                                   float3 albedo, float smoothness, float metallic, float3 worldNormal, float3 viewDir,
                                   float3 worldPosOffset, float pointLightShading)
{
    L0 = 0; L1r = 0; L1g = 0; L1b = 0; specular = 0;
}

#endif

#endif
