// A map's texture sky cards (environment.skyCards; docs/research/gw2-sky.md §9) as quads drawn by
// Castlemist/GW2 Sky Card: the moon, cloud wisps, star fields and the like that GW2 draws at the sky
// distance F around the camera, each turning about the zenith at its own speed. One mesh per card holds
// its quad as the client builds it (0x140ca7280): corners (F, +-1000, +-1000) normalised to length F,
// scaled by (1, sx, sy), with uvs from textureUV = (uLeft, uRight, 1 - vTop, 1 - vBottom) (§9.3).
// Day values; material cards (no texture) are left out, as the world export's skybox bake keeps them.
// Draw order: card order, from Transparent-50 (after the haze volume, which skips the sky as GW2's
// haze does; before the cloud layers at Transparent-30, as sky_bake composes them; the GPU order is
// UNPROVEN, §4). At most 20 texture cards fit before the clouds.
// The objects sit under the map root at identity, so their matrices are the map frame; not static,
// huge bounds (the quads move with the camera).
//
// 2026-10-25.

using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using static Castlemist.GW2Clouds;

namespace Castlemist
{
    public static class GW2SkyCards
    {
        public const string GroupName = "GW2 Sky Cards";
        const string ShaderName = "Castlemist/GW2 Sky Card";

        /// Builds every texture card of @p env's skyCards (environment, as the manifest writes it)
        /// under @p mapRoot, replacing an earlier build. @p texturePath maps a texture to its asset
        /// path; meshes and materials go to @p assetFolder (updated in place). Returns the cards built.
        public static int Build(GameObject mapRoot, Dictionary<string, object> env, Func<string, string> texturePath,
                                string assetFolder, float unityScale, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            var cards = env != null && env.TryGetValue("skyCards", out object c) ? c as List<object> : null;
            if (cards == null || cards.Count == 0) return 0;
            Shader shader = Shader.Find(ShaderName);
            if (shader == null) { warnings.Add("sky cards: no " + ShaderName + " shader: not built"); return 0; }
            EnsureFolder(assetFolder);

            float F = (float)Num(env, "skyDistance", 24576);
            var li = Arr(env, "skyLightIntensity");
            float skyLI = li.Length > 0 ? (float)li[0] : 1f;
            var skyHaze = env.TryGetValue("skyHaze", out object sh) ? sh as Dictionary<string, object> : null;
            var hb = skyHaze != null ? Arr(skyHaze, "bottom") : new double[0];
            var hf = skyHaze != null ? Arr(skyHaze, "falloff") : new double[0];
            var hd = skyHaze != null ? Arr(skyHaze, "density") : new double[0];
            float hazeBottom = hb.Length > 0 ? (float)hb[0] : 0f, hazeFalloff = hf.Length > 0 ? (float)hf[0] : 1f,
                  hazeDensity = hd.Length > 0 ? (float)hd[0] : 0f;
            SkyInputs(env, "sky cards", warnings, out Vector3 sunMap, out Color sunCol, out Color fogFar, out Vector4 _);

            var group = new GameObject(GroupName);
            group.transform.SetParent(mapRoot.transform, false);
            int built = 0;
            for (int i = 0; i < cards.Count; ++i)
            {
                var K = cards[i] as Dictionary<string, object>;
                if (K == null) continue;
                string name = "Card " + i;
                if (K.TryGetValue("materialFileId", out object mf) && mf is double)
                {
                    warnings.Add("sky cards: " + name + " is a material card (" + (long)(double)mf + "): left in the skybox bake");
                    continue;
                }
                var A = K.TryGetValue("day", out object dy) ? dy as Dictionary<string, object> : null;
                if (A == null) { warnings.Add("sky cards: " + name + " has no day set: not built"); continue; }
                string tex = Str(A, "texture") is string t ? texturePath(t) : null;
                Texture2D texture = tex != null ? RawTexture(tex, TextureWrapMode.Clamp) : null;
                if (texture == null) { warnings.Add("sky cards: " + name + " texture " + (tex ?? "null") + " not found: not built"); continue; }

                Mesh mesh = SaveMesh(BuildQuad(F, V2(A, "scale"), Arr(A, "textureUV")), assetFolder + "/" + name + ".asset");
                var mat = AssetDatabase.LoadAssetAtPath<Material>(assetFolder + "/" + name + ".mat");
                if (mat == null) { mat = new Material(shader); AssetDatabase.CreateAsset(mat, assetFolder + "/" + name + ".mat"); }
                mat.shader = shader;
                mat.SetTexture("_MainTex", texture);
                mat.SetVector("_Card", new Vector4((float)Num(A, "azimuth", 0), (float)Num(A, "latitude", 0) * Mathf.PI / 2,
                                                   (float)Num(A, "speed", 0), 0));
                mat.SetVector("_Attr", new Vector4((float)Num(A, "brightness", 1), (float)Num(A, "density", 1),
                                                   (float)Num(A, "minHaze", 0), (float)Num(A, "lightIntensity", 0) * skyLI));
                mat.SetVector("_Haze", new Vector4(hazeBottom, hazeFalloff, (float)Num(A, "hazeDensity", 0) * hazeDensity, 0));
                mat.SetVector("_SunDir", sunMap.normalized);
                mat.SetColor("_SunColor", sunCol);
                mat.SetColor("_Ambient", sunCol);   // UNPROVEN stand-in for GW2's SH (GW2Sky.cginc)
                mat.SetColor("_FogFar", fogFar);
                mat.SetFloat("_UnityScale", unityScale);
                mat.renderQueue = (int)RenderQueue.Transparent - 50 + built;   // card order
                EditorUtility.SetDirty(mat);

                var go = new GameObject("GW2 Sky " + name);
                go.transform.SetParent(group.transform, false);
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                var mr = go.AddComponent<MeshRenderer>();
                mr.sharedMaterial = mat;
                mr.shadowCastingMode = ShadowCastingMode.Off;
                mr.receiveShadows = false;
                mr.lightProbeUsage = LightProbeUsage.Off;
                mr.reflectionProbeUsage = ReflectionProbeUsage.Off;
                mr.allowOcclusionWhenDynamic = false;
                GameObjectUtility.SetStaticEditorFlags(go, 0);
                built++;
            }
            return built;
        }

        /// The card's quad (0x140ca7280, §9.3): x toward the card, y = eL (uLeft side), z = eD (bottom).
        static Mesh BuildQuad(float F, Vector2 scale, double[] uv)
        {
            float sx = Mathf.Max(scale.x, 1e-6f), sy = Mathf.Max(scale.y, 1e-6f);
            float x = uv.Length >= 4 ? (float)uv[0] : 0, y = uv.Length >= 4 ? (float)uv[1] : 1;
            float z = uv.Length >= 4 ? (float)uv[2] : 1, w = uv.Length >= 4 ? (float)uv[3] : 0;
            var corners = new[] { new Vector3(F, 1000, -1000), new Vector3(F, 1000, 1000), new Vector3(F, -1000, -1000), new Vector3(F, -1000, 1000) };
            var verts = new Vector3[4];
            for (int i = 0; i < 4; ++i)
            {
                Vector3 p = corners[i].normalized * F;
                verts[i] = new Vector3(p.x, p.y * sx, p.z * sy);
            }
            var uvs = new[] { new Vector2(x, 1 - z), new Vector2(x, 1 - w), new Vector2(y, 1 - z), new Vector2(y, 1 - w) };
            var mesh = new Mesh { name = "GW2 Sky Card", vertices = verts, uv = uvs, triangles = new[] { 0, 1, 2, 2, 1, 3 } };
            // drawn wherever the camera is: never culled
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 1e8f);
            return mesh;
        }
    }
}
