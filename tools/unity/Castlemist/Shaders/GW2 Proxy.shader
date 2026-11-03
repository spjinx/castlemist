// Castlemist/GW2 Proxy (2026-10-26): a far cell's stand-in (CellProxies): its props merged into one
// mesh, each submesh coloured by its material's mean texture colour (vertex colour). Lit as the GW2
// shader lights at a distance: the sun's N.L (no shadow: proxies sit past the shadow distance) plus
// the Trilight ambient (unity_AmbientSky / Equator / Ground, which GW2DayNight drives). The
// ShadowCaster pass puts it in the camera depth texture, so the GW2 Haze volume fogs it.
Shader "Castlemist/GW2 Proxy"
{
    Properties
    {
        _Tint ("Tint", Color) = (1, 1, 1, 1)
    }
    SubShader
    {
        Tags { "Queue" = "Geometry" "RenderType" = "Opaque" }
        Pass
        {
            Tags { "LightMode" = "ForwardBase" }
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"
            #include "Lighting.cginc"

            half4 _Tint;

            struct appdata { float4 vertex : POSITION; float3 normal : NORMAL; half4 color : COLOR; UNITY_VERTEX_INPUT_INSTANCE_ID };
            struct v2f { float4 pos : SV_POSITION; half3 col : COLOR; float3 n : TEXCOORD0; UNITY_VERTEX_OUTPUT_STEREO };

            v2f vert(appdata v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.n = UnityObjectToWorldNormal(v.normal);
                o.col = v.color.rgb * _Tint.rgb;
                return o;
            }

            half3 Trilight(float3 n)
            {
                return n.y > 0 ? lerp(unity_AmbientEquator.rgb, unity_AmbientSky.rgb, n.y)
                               : lerp(unity_AmbientEquator.rgb, unity_AmbientGround.rgb, -n.y);
            }

            half4 frag(v2f i) : SV_Target
            {
                float3 n = normalize(i.n);
                half3 light = _LightColor0.rgb * saturate(dot(n, normalize(_WorldSpaceLightPos0.xyz))) + Trilight(n);
                return half4(i.col * light, 1);
            }
            ENDCG
        }
        Pass
        {
            Tags { "LightMode" = "ShadowCaster" }
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_shadowcaster
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"
            struct v2f { V2F_SHADOW_CASTER; UNITY_VERTEX_OUTPUT_STEREO };
            v2f vert(appdata_base v)
            {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                TRANSFER_SHADOW_CASTER_NORMALOFFSET(o)
                return o;
            }
            float4 frag(v2f i) : SV_Target { SHADOW_CASTER_FRAGMENT(i) }
            ENDCG
        }
    }
    Fallback Off
}
