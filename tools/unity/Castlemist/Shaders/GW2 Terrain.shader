// castlemist: GW2's hi-res terrain shader on a Unity Terrain (2026-10-27; AMAT 184988 ps 73, read from the
// game's shader). The export bakes each tile's GW2 blend into one terrain layer:
//   _Splat0   (the layer's diffuse)   rgb  the blended ground colour (picker, modx brightness / saturation)
//                                     a    the specular mask: the ground normal maps' alpha, blended, x 0.596
//   _Normal0  (the layer's normal map, imported as data, not as a Unity normal map)
//                                     rgb  the blended tangent-space normal (Unity Y+), a  the wet mask:
//                                          saturate((modx.g - 0.5) x 2)
// and GW2 shades it: lit colour + the sun's specular x the mask (Blinn-Phong, _SpecColor x _Shininess),
// then toward the environment's reflection by saturate(1 - N.V) x the wet mask (GW2: its sky cube,
// squared; here Unity's reflection probe, already linear). One layer per tile (tileSize = the tile), so
// the holes map shares its UV. Drawn without terrain instancing (the importer turns it off): the
// surface shader needs the mesh's normals for the reflection.
// 2026-10-27: the ambient is VRC Light Volumes' (the baked volumes and the realtime point lights, by the
// package's functions at the texel's world position and normal), as the GW2 shader's: the baked part
// never below _AmbientFloor (0.6, the props' value) of the GW2 Trilight, the point lights added; Unity's
// own ambient / probes where no volume reaches (LightVolumesEnabled off).

Shader "Castlemist/GW2 Terrain"
{
    Properties
    {
        _SpecColor ("Specular colour (x the mask)", Color) = (1, 1, 1, 1)
        _Shininess ("Shininess (power / 128)", Range(0.03, 1)) = 0.25
        _WetStrength ("Wet reflection strength", Range(0, 2)) = 1
        _AmbientFloor ("Ambient floor (x the GW2 Trilight ambient, under Light Volumes)", Range(0, 1)) = 0.6
        [HideInInspector] _MainTex ("BaseMap (RGB)", 2D) = "white" {}
        [HideInInspector] _Color ("Main Color", Color) = (1, 1, 1, 1)
        [HideInInspector] _TerrainHolesTexture ("Holes Map (RGB)", 2D) = "white" {}
    }

    SubShader
    {
        Tags { "Queue" = "Geometry-100" "RenderType" = "Opaque" "TerrainCompatible" = "True" }

        CGPROGRAM
        #pragma surface surf GW2Terrain vertex:vert addshadow fullforwardshadows exclude_path:deferred exclude_path:prepass
        #pragma multi_compile_local __ _ALPHATEST_ON
        #pragma target 3.0
        #include "UnityCG.cginc"
        #include "UnityPBSLighting.cginc"
        #include "CastlemistLightVolumes.cginc"

        half _NormalScale0, _Shininess, _WetStrength, _AmbientFloor;

        // the GW2 shader's (GW2Core CM_Trilight): the env's flat ambient by the normal's height
        half3 TerrainTrilight(float3 n)
        {
            return n.y > 0 ? lerp(unity_AmbientEquator.rgb, unity_AmbientSky.rgb, n.y)
                           : lerp(unity_AmbientEquator.rgb, unity_AmbientGround.rgb, -n.y);
        }

        // Blinn-Phong as the built-in terrain shaders, the indirect diffuse from the Light Volumes
        inline fixed4 LightingGW2Terrain(SurfaceOutput s, half3 viewDir, UnityGI gi) { return LightingBlinnPhong(s, viewDir, gi); }

        inline void LightingGW2Terrain_GI(SurfaceOutput s, UnityGIInput data, inout UnityGI gi)
        {
            gi = UnityGlobalIllumination(data, 1.0, s.Normal);
            if (LightVolumesEnabled() > 0.5)
            {
            #ifndef CASTLEMIST_NO_LIGHT_VOLUMES
                // 2026-10-27: as the GW2 shader (GW2Core, the forward base's ambient), so the ground and the
                // rocks light alike (the user): the baked volumes, never below _AmbientFloor of the Trilight,
                // then the realtime point lights on top
                float3 L0 = 0, L1r = 0, L1g = 0, L1b = 0;
                LV_LightVolumeRegularSH(data.worldPos, L0, L1r, L1g, L1b);
                LV_LightVolumeAdditiveSH(data.worldPos, L0, L1r, L1g, L1b);
                float3 baked = max(max(0, LightVolumeEvaluate(s.Normal, L0, L1r, L1g, L1b)), _AmbientFloor * TerrainTrilight(s.Normal));
                float3 P0 = 0, P1r = 0, P1g = 0, P1b = 0, lvSpec = 0;
                LV_PointLightVolumeSHSpecular(data.worldPos, s.Normal, data.worldViewDir, 0, 0.04, 3, P0, P1r, P1g, P1b, lvSpec);
                gi.indirect.diffuse = baked + max(0, LightVolumeEvaluate(s.Normal, P0, P1r, P1g, P1b));
            #else
                float3 L0, L1r, L1g, L1b;
                LightVolumeSH(data.worldPos, L0, L1r, L1g, L1b, 0, s.Normal);
                gi.indirect.diffuse = max(max(0, LightVolumeEvaluate(s.Normal, L0, L1r, L1g, L1b)), _AmbientFloor * TerrainTrilight(s.Normal));
            #endif
            }
        }

        sampler2D _Control, _Splat0, _Normal0, _TerrainHolesTexture;
        float4 _Splat0_ST;   // _Control_ST: the surface shader generator declares it (uv_Control)

        struct Input
        {
            float2 uv_Control;
            float3 worldRefl;
            float3 viewDir;
            INTERNAL_DATA
        };

        void vert(inout appdata_full v)
        {
            // Unity Terrain meshes carry no tangents: the built-in terrain shaders' frame
            v.tangent.xyz = cross(v.normal, float3(0, 0, 1));
            v.tangent.w = -1;
        }

        void surf(Input IN, inout SurfaceOutput o)
        {
            float2 uv = IN.uv_Control;   // one layer the size of the tile: its UV is the control's
            #ifdef _ALPHATEST_ON
                clip(tex2D(_TerrainHolesTexture, uv).r - 0.5);
            #endif
            half4 c = tex2D(_Splat0, TRANSFORM_TEX(uv, _Splat0));
            half4 n = tex2D(_Normal0, TRANSFORM_TEX(uv, _Splat0));
            half3 tn = n.xyz * 2 - 1;
            tn.xy *= (_NormalScale0 > 0 ? _NormalScale0 : 1);
            tn = normalize(tn);
            o.Normal = tn;
            o.Specular = _Shininess;
            o.Gloss = c.a;
            o.Alpha = 1;

            // GW2: lerp(lit, cube, saturate(1 - N.V) x wet): the lit share kept, the reflection added
            float3 refl = WorldReflectionVector(IN, tn);
            half fres = saturate(1 - dot(normalize(IN.viewDir), tn));
            half wet = saturate(n.a * fres * _WetStrength);
            half4 probe = UNITY_SAMPLE_TEXCUBE_LOD(unity_SpecCube0, refl, 1);
            half3 env = DecodeHDR(probe, unity_SpecCube0_HDR);
            o.Albedo = c.rgb * (1 - wet);
            o.Emission = env * wet;
        }
        ENDCG
    }
    Fallback "Nature/Terrain/Specular"
}
