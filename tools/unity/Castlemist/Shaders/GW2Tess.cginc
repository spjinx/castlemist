// GPU tessellation for Castlemist/GW2 Tessellated (2026-10-22): subdivides the mesh so the wave
// block (CM_Wave, GW2Core.cginc) bends it smoothly. Included after GW2Core.cginc by the forward and
// shadow passes; the hull stage leaves a patch whole (factor 1) when the wave is off, and fades the
// subdivision out over _TessMaxDistance from the camera. Factors from Unity's screen-space edge
// length (_TessEdgeLength pixels per edge), at most _TessMaxFactor. The domain stage interpolates
// the vertex and runs the pass's own vertex function (vert / vertShadow), so every block, the wave
// included, sees the subdivided vertices.

#ifndef CASTLEMIST_GW2_TESS_INCLUDED
#define CASTLEMIST_GW2_TESS_INCLUDED

#include "Tessellation.cginc"

float _TessEdgeLength, _TessMaxDistance, _TessMaxFactor;

struct CMTessVertex
{
    float4 vertex : INTERNALTESSPOS;
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

struct CMTessFactors
{
    float edge[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

CMTessVertex tessvert(appdata v)
{
    CMTessVertex o;
    UNITY_SETUP_INSTANCE_ID(v);
    UNITY_TRANSFER_INSTANCE_ID(v, o);
    o.vertex = v.vertex;
    o.normal = v.normal;
    o.tangent = v.tangent;
    o.uv0 = v.uv0;
    o.uv1 = v.uv1;
    o.uv2 = v.uv2;
    o.uv3 = v.uv3;
    o.uv4 = v.uv4;
    o.color = v.color;
    return o;
}

CMTessFactors tesshull_const(InputPatch<CMTessVertex, 3> p)
{
    CMTessFactors f;
    float4 t = float4(1, 1, 1, 1);
    if (_UseWave > 0.5)
    {
        // No frustum culling here: the shadow pass renders from the light, not the camera.
        t = UnityEdgeLengthBasedTess(p[0].vertex, p[1].vertex, p[2].vertex, max(_TessEdgeLength, 1.0));
        float3 c = mul(unity_ObjectToWorld, float4((p[0].vertex.xyz + p[1].vertex.xyz + p[2].vertex.xyz) / 3.0, 1.0)).xyz;
        float fade = saturate(1.0 - distance(c, _WorldSpaceCameraPos) / max(_TessMaxDistance, 1e-3));
        t = max(1.0, lerp(1.0, min(t, max(_TessMaxFactor, 1.0)), fade));
    }
    f.edge[0] = t.x;
    f.edge[1] = t.y;
    f.edge[2] = t.z;
    f.inside = t.w;
    return f;
}

[UNITY_domain("tri")]
[UNITY_partitioning("fractional_odd")]
[UNITY_outputtopology("triangle_cw")]
[UNITY_patchconstantfunc("tesshull_const")]
[UNITY_outputcontrolpoints(3)]
CMTessVertex tesshull(InputPatch<CMTessVertex, 3> p, uint id : SV_OutputControlPointID)
{
    return p[id];
}

appdata CM_TessInterpolate(const OutputPatch<CMTessVertex, 3> p, float3 b)
{
    appdata v;
    UNITY_INITIALIZE_OUTPUT(appdata, v);
    UNITY_TRANSFER_INSTANCE_ID(p[0], v);
    #define CM_TESS_LERP(f) v.f = p[0].f * b.x + p[1].f * b.y + p[2].f * b.z;
    CM_TESS_LERP(vertex)
    CM_TESS_LERP(normal)
    CM_TESS_LERP(tangent)
    CM_TESS_LERP(uv0)
    CM_TESS_LERP(uv1)
    CM_TESS_LERP(uv2)
    CM_TESS_LERP(uv3)
    CM_TESS_LERP(uv4)
    CM_TESS_LERP(color)
    #undef CM_TESS_LERP
    v.normal = normalize(v.normal);
    return v;
}

#if defined(CM_PASS_SHADOWCASTER)
[UNITY_domain("tri")]
v2fShadow tessdomain(CMTessFactors f, const OutputPatch<CMTessVertex, 3> p, float3 b : SV_DomainLocation)
{
    return vertShadow(CM_TessInterpolate(p, b));
}
#else
[UNITY_domain("tri")]
v2f tessdomain(CMTessFactors f, const OutputPatch<CMTessVertex, 3> p, float3 b : SV_DomainLocation)
{
    return vert(CM_TessInterpolate(p, b));
}
#endif

#endif
