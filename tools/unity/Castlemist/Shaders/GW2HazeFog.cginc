// GW2HazeFog.cginc (2026-10-25): the GW2 Haze volume's formula (GW2 Haze.shader's header, the
// engine's VSCalculateFog) for a transparent surface to fog itself at its own pixel. The volume
// fogs what is in the camera depth texture; a surface drawn after it, or one that writes no depth
// (the water, the refracting glass), is not, so it applies this with the haze material's values
// (copied by the importer: HazeCopy; lerped by GW2DayNight). _UseHaze 0: nothing.
#ifndef GW2_HAZE_FOG_INCLUDED
#define GW2_HAZE_FOG_INCLUDED

float _UseHaze, _HazeDepthCue, _HazeMapOriginY, _HazeAmount, _HazeUnityScale;
half4 _HazeNear, _HazeFar, _HazeHeight;
float4 _HazeDistRange, _HazeHeightRange;

// GW2 Haze.shader's GW2Fog, distances in GW2 units.
half4 CM_GW2Fog(float dist, float z)
{
    float dFog = 1.0 - min(exp2(-(dist - _HazeDistRange.x) / max(_HazeDistRange.y - _HazeDistRange.x, 1e-3)), 1.0);
    float hSpan = _HazeHeightRange.y - _HazeHeightRange.x;
    hSpan = abs(hSpan) < 1e-3 ? (hSpan < 0 ? -1e-3 : 1e-3) : hSpan;
    float hFog = 1.0 - min(exp2(-(z - _HazeHeightRange.x) / hSpan), 1.0);
    float hAmt = min(hFog, _HazeHeight.a) * min(dist * _HazeDepthCue, 1.0);
    half4 dCol = _HazeFar + (_HazeNear - _HazeFar) * (1.0 - dFog);
    float dAmt = min(dFog, dCol.a);
    half3 rgb = _HazeHeight.rgb * hAmt * (1.0 - dAmt) + dCol.rgb * dAmt;
    return half4(rgb, hAmt * (1.0 - dAmt) + dAmt);
}

// @p c fogged at world position @p wp; its alpha moves toward 1 by the fog (a far surface becomes
// the haze, not the scene behind it). False (and @p c unchanged) when _UseHaze is off.
bool CM_ApplyGW2Haze(inout half4 c, float3 wp)
{
    if (_UseHaze < 0.5) return false;
    float us = max(_HazeUnityScale, 1e-6);
    half4 fog = saturate(CM_GW2Fog(distance(wp, _WorldSpaceCameraPos) / us, -(wp.y - _HazeMapOriginY) / us) * _HazeAmount);
    c.rgb = c.rgb * (1.0 - fog.a) + fog.rgb;
    c.a = lerp(c.a, 1.0, fog.a);
    return true;
}

#endif
