// A map's cloud layers (environment.clouds; docs/research/gw2-sky.md §10) as planes drawn by
// Castlemist/GW2 Cloud. Each layer is the client's 11 x 11 soft-edged grid (0x140ca3290), built once;
// the shader centres it under the camera at the layer's altitude, so one layer below the camera and
// one above both draw (the "top and bottom" clouds). Values from attribute set 0 (day); fadeEnd /
// fadeWidth are the last set's (the client's import keeps those, §10.1). The sun and the haze come
// from the same manifest environment the rest of the scene uses; the sun's colour x intensity is also
// the ambient stand-in (GW2's SH is UNPROVEN; GW2 Cloud.shader's header says why).
// Draw order: farthest from the spawn first (the GPU order is UNPROVEN, §4; sky_bake does the same),
// from Transparent-30: after the sky cards, before the scene's own transparents (2026-10-25: in the
// shared Transparent queue their huge bounds sorted them over the map's particles, which the game
// draws over its sky). _HeightOffset (Unity units, castlemist's) is the user's and survives a rebuild.
// The objects sit under the map root at identity, so their matrices are the map frame; not static,
// huge bounds (the plane moves with the camera, so it must never be culled).
//
// 2026-10-25.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace Castlemist
{
    public static class GW2Clouds
    {
        public const string GroupName = "GW2 Clouds";
        const string ShaderName = "Castlemist/GW2 Cloud";

        /// Builds every layer of @p env's clouds (environment, as the manifest writes it) under
        /// @p mapRoot, replacing an earlier build. @p texturePath maps a layer's texture to its asset
        /// path; meshes and materials go to @p assetFolder (updated in place). @p spawnAltitude is
        /// the spawn's height (GW2 units, up = -Z negated) for the draw order. Returns the layers built.
        public static int Build(GameObject mapRoot, Dictionary<string, object> env, Func<string, string> texturePath,
                                string assetFolder, float unityScale, double spawnAltitude, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            var layers = env != null && env.TryGetValue("clouds", out object c) ? c as List<object> : null;
            if (layers == null || layers.Count == 0) return 0;
            Shader shader = Shader.Find(ShaderName);
            if (shader == null) { warnings.Add("clouds: no " + ShaderName + " shader: not built"); return 0; }
            EnsureFolder(assetFolder);
            Mesh mesh = SaveMesh(BuildMesh(), assetFolder + "/CloudGrid.asset");

            // Shared inputs: F, the sky's day LightIntensity, the GW2 sun, the day haze.
            float F = (float)Num(env, "skyDistance", 24576);
            var li = Arr(env, "skyLightIntensity");
            float skyLI = li.Length > 0 ? (float)li[0] : 1f;
            SkyInputs(env, "clouds", warnings, out Vector3 sunMap, out Color sunCol, out Color fogFar, out Vector4 fogDist);

            var group = new GameObject(GroupName);
            group.transform.SetParent(mapRoot.transform, false);
            var built = new List<KeyValuePair<double, Material>>();
            for (int i = 0; i < layers.Count; ++i)
            {
                var L = layers[i] as Dictionary<string, object>;
                if (L == null) continue;
                string name = "Layer " + i;
                var attrs = L.TryGetValue("attributes", out object ao) ? ao as List<object> : null;
                var A = attrs != null && attrs.Count > 0 ? attrs[0] as Dictionary<string, object> : null;
                if (A == null) { warnings.Add("clouds: " + name + " has no attribute set 0: not built"); continue; }
                string tex = Str(L, "texture") is string t ? texturePath(t) : null;
                Texture2D texture = tex != null ? RawTexture(tex, TextureWrapMode.Repeat) : null;
                if (texture == null) { warnings.Add("clouds: " + name + " texture " + (tex ?? "null") + " not found: not built"); continue; }
                double altitude = Num(L, "altitude", 0);
                Material mat = SaveMaterial(shader, L, A, texture, assetFolder + "/" + name + ".mat");
                mat.SetVector("_Plane", new Vector4(F, (float)Num(L, "m", 1), (float)Num(L, "scale", 1), (float)Num(L, "depth", 0)));
                mat.SetVector("_Attr", new Vector4((float)Num(A, "brightness", 1), (float)Num(A, "density", 1),
                                                   (float)Num(A, "haze", 0), (float)Num(A, "lightIntensity", 0) * skyLI));
                mat.SetVector("_SunDir", sunMap.normalized);
                mat.SetColor("_SunColor", sunCol);
                mat.SetColor("_Ambient", sunCol);   // UNPROVEN stand-in for GW2's SH (GW2Sky.cginc)
                mat.SetColor("_FogFar", fogFar);
                mat.SetVector("_FogDist", fogDist);
                mat.SetFloat("_UnityScale", unityScale);
                var go = new GameObject("GW2 Cloud " + name);
                go.transform.SetParent(group.transform, false);
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                var mr = go.AddComponent<MeshRenderer>();
                mr.sharedMaterial = mat;
                mr.shadowCastingMode = ShadowCastingMode.Off;
                mr.receiveShadows = false;
                mr.lightProbeUsage = LightProbeUsage.Off;   // the scene's ambient SH, not a probe at a moving plane
                mr.reflectionProbeUsage = ReflectionProbeUsage.Off;
                mr.allowOcclusionWhenDynamic = false;
                GameObjectUtility.SetStaticEditorFlags(go, 0);
                built.Add(new KeyValuePair<double, Material>(Math.Abs(altitude - spawnAltitude), mat));
            }
            // farthest first: Transparent-30, -29, ...
            built.Sort((a, b) => b.Key.CompareTo(a.Key));
            for (int i = 0; i < built.Count; ++i)
            {
                built[i].Value.renderQueue = (int)RenderQueue.Transparent - 30 + i;
                EditorUtility.SetDirty(built[i].Value);
            }
            return built.Count;
        }

        /// The client's grid (0x140ca3290(0.5, 10)): 11 x 11 vertices, x, y in [-0.5, 0.5] step 0.1
        /// (local x, z), vertex alpha 255 (1 - min(r / 0.5, 1)^4).
        static Mesh BuildMesh()
        {
            const int N = 10;
            var verts = new Vector3[(N + 1) * (N + 1)];
            var cols = new Color32[verts.Length];
            for (int y = 0; y <= N; ++y)
                for (int x = 0; x <= N; ++x)
                {
                    float gx = x / (float)N - 0.5f, gy = y / (float)N - 0.5f;
                    float r = Mathf.Min(Mathf.Sqrt(gx * gx + gy * gy) / 0.5f, 1f);
                    byte a = (byte)Mathf.RoundToInt(255f * (1f - r * r * r * r));
                    verts[y * (N + 1) + x] = new Vector3(gx, 0, gy);
                    cols[y * (N + 1) + x] = new Color32(a, a, a, a);
                }
            var tris = new int[N * N * 6];
            int k = 0;
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x)
                {
                    int i0 = y * (N + 1) + x, i1 = i0 + 1, i2 = i0 + N + 1, i3 = i2 + 1;
                    tris[k++] = i0; tris[k++] = i2; tris[k++] = i1;
                    tris[k++] = i1; tris[k++] = i2; tris[k++] = i3;
                }
            var mesh = new Mesh { name = "GW2 Cloud Grid", vertices = verts, colors32 = cols, triangles = tris };
            // drawn wherever the camera is: never culled
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 1e8f);
            return mesh;
        }

        /// The inputs the sky layers share (GW2Sky.cginc): the GW2 sun's travel direction in map axes,
        /// its colour x intensity (also the ambient stand-in), and the day haze's farColor and
        /// (distRange d0, d1, nearColor.w). @p what names the caller in warnings.
        internal static void SkyInputs(Dictionary<string, object> env, string what, List<string> warnings,
                                       out Vector3 sunMap, out Color sunCol, out Color fogFar, out Vector4 fogDist)
        {
            sunMap = new Vector3(0, 0, 1);
            sunCol = Color.white;
            var lights = env.TryGetValue("lights", out object lo) ? lo as List<object> : null;
            if (lights != null && lights.Count > 0 && lights[0] is Dictionary<string, object> sun)
            {
                var uf = Arr(sun, "unityForward");
                if (uf.Length >= 3) sunMap = new Vector3((float)uf[0], (float)uf[2], -(float)uf[1]);   // Unity -> map axes
                var col = Arr(sun, "color");
                float k = (float)Num(sun, "intensity", 1);   // colour x intensity: the shader's sunClr and ambient stand-in
                if (col.Length >= 3) sunCol = new Color((float)col[0] * k, (float)col[1] * k, (float)col[2] * k, 1);
            }
            else warnings.Add(what + ": no environment.lights[0]: lit by a white sun overhead");
            fogFar = new Color(0, 0, 0, 0);
            fogDist = new Vector4(0, 1, 1, 0);
            var haze = env.TryGetValue("haze", out object ho) ? ho as Dictionary<string, object> : null;
            var far = haze != null ? Arr(haze, "far") : new double[0];
            var near = haze != null ? Arr(haze, "near") : new double[0];
            var dist = haze != null ? Arr(haze, "distRange") : new double[0];
            if (far.Length >= 4 && dist.Length >= 2)
            {
                fogFar = new Color((float)far[0], (float)far[1], (float)far[2], (float)far[3]);
                fogDist = new Vector4((float)dist[0], (float)dist[1], near.Length >= 4 ? (float)near[3] : 1f, 0);
            }
            else warnings.Add(what + ": no environment.haze: not fogged");
        }

        /// The layer texture as GW2 samples it: raw values (sRGB off), mipmapped, @p wrap.
        internal static Texture2D RawTexture(string path, TextureWrapMode wrap)
        {
            var ti = AssetImporter.GetAtPath(path) as TextureImporter;
            if (ti == null) return null;
            if (ti.sRGBTexture || ti.wrapMode != wrap || !ti.mipmapEnabled || ti.alphaIsTransparency)
            {
                ti.sRGBTexture = false;
                ti.wrapMode = wrap;
                ti.mipmapEnabled = true;
                ti.alphaIsTransparency = false;
                ti.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
        }

        internal static Mesh SaveMesh(Mesh mesh, string path)
        {
            var existing = AssetDatabase.LoadAssetAtPath<Mesh>(path);
            if (existing == null) { AssetDatabase.CreateAsset(mesh, path); return mesh; }
            EditorUtility.CopySerialized(mesh, existing);
            existing.bounds = mesh.bounds;
            EditorUtility.SetDirty(existing);
            return existing;
        }

        static Material SaveMaterial(Shader shader, Dictionary<string, object> L, Dictionary<string, object> A, Texture2D texture, string path)
        {
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat == null) { mat = new Material(shader); AssetDatabase.CreateAsset(mat, path); }
            mat.shader = shader;
            mat.SetTexture("_MainTex", texture);
            mat.SetFloat("_Altitude", (float)Num(L, "altitude", 0));
            mat.SetFloat("_CutOut", (float)Num(L, "cutOut", 1000));
            mat.SetVector("_Fade", new Vector4((float)Num(L, "fadeEnd", 0), (float)Num(L, "fadeWidth", 0), 0, 0));
            Vector2 vel = V2(A, "velocity");
            mat.SetVector("_Velocity", new Vector4(vel.x, vel.y, 0, 0));
            return mat;
        }

        internal static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }
        internal static string Str(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) ? v as string : null; }
        internal static double Num(Dictionary<string, object> d, string k, double def)
        {
            object v;
            return d.TryGetValue(k, out v) && v is double x ? x : def;
        }
        internal static double[] Arr(Dictionary<string, object> d, string k)
        {
            object v;
            if (!d.TryGetValue(k, out v) || !(v is List<object> l)) return new double[0];
            var r = new double[l.Count];
            for (int i = 0; i < l.Count; ++i) r[i] = l[i] is double x ? x : 0;
            return r;
        }
        internal static Vector2 V2(Dictionary<string, object> d, string k)
        {
            var a = Arr(d, k);
            return a.Length >= 2 ? new Vector2((float)a[0], (float)a[1]) : Vector2.zero;
        }
    }
}
