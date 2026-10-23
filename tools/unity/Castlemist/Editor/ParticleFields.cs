// A map's ambient particle fields (environment.particleFields; docs/research/gw2-particle-fields.md)
// as meshes drawn by Castlemist/GW2 Particle Field. Each field is one tile of particles that the
// shader wraps around the camera, so the mesh is built once, as the client builds it (0x140ca47a0):
//   * positions uniform in [0, 1)^3 (map axes), each pulled toward one of clusterCount random centres
//     by a random clustering amount along the wrapped (shortest) delta, then wrapped; flag 0x100
//     keeps the height;
//   * per particle a seed byte, opacity, size (flag 0x1: square), angle and spin, a random frame of
//     texColRow; four vertices share the position, the corner in COLOR.xy and TEXCOORD2.y.
// The client's random generator is not ported (the layout is random either way; `seed` seeds ours).
// The object sits under the map root at identity, so its matrices are the map frame; it is not
// static (batching would bake the tile).
//
// 2026-10-23: from the client's code and shaders; the blend (additive) and the lifetime / period
// order are readings (the research note's UNPROVEN list).

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace Castlemist
{
    public static class ParticleFields
    {
        public const string GroupName = "GW2 Particle Fields";
        const string ShaderName = "Castlemist/GW2 Particle Field";

        /// Builds every field of @p fields (environment.particleFields, as the manifest or
        /// `gw2dat_cli world --particle-fields` writes them) under @p mapRoot, replacing an earlier
        /// build. @p texturePath maps a field to its texture asset path (null: none). Meshes and
        /// materials go to @p assetFolder (updated in place). Returns the fields built.
        public static int Build(GameObject mapRoot, List<object> fields, Func<Dictionary<string, object>, string> texturePath,
                                string assetFolder, float unityScale, List<string> warnings)
        {
            Shader shader = Shader.Find(ShaderName);
            if (shader == null) { warnings.Add("particle fields: no " + ShaderName + " shader: not built"); return 0; }
            Transform old = mapRoot.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            if (fields == null || fields.Count == 0) return 0;
            EnsureFolder(assetFolder);
            var group = new GameObject(GroupName);
            group.transform.SetParent(mapRoot.transform, false);
            int built = 0;
            var names = new HashSet<string>();
            foreach (object o in fields)
            {
                var f = o as Dictionary<string, object>;
                if (f == null) continue;
                string name = Str(f, "name") ?? "field";
                int count = (int)Num(f, "particleCount", 0);
                if (count <= 0) continue;
                string tex = texturePath(f);
                Texture2D texture = tex != null ? AssetDatabase.LoadAssetAtPath<Texture2D>(tex) : null;
                if (texture == null) { warnings.Add("particle field " + name + ": texture " + (tex ?? "null") + " not found: not built"); continue; }
                string safe = Safe(name);
                while (!names.Add(safe)) safe += "_";
                Mesh mesh = SaveMesh(BuildMesh(f, count), assetFolder + "/" + safe + ".asset");
                Material mat = SaveMaterial(shader, f, texture, unityScale, assetFolder + "/" + safe + ".mat");
                var go = new GameObject("GW2 Field " + name);
                go.transform.SetParent(group.transform, false);
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                var mr = go.AddComponent<MeshRenderer>();
                mr.sharedMaterial = mat;
                mr.shadowCastingMode = ShadowCastingMode.Off;
                mr.receiveShadows = false;
                mr.lightProbeUsage = LightProbeUsage.Off;
                mr.reflectionProbeUsage = ReflectionProbeUsage.Off;
                GameObjectUtility.SetStaticEditorFlags(go, 0);
                built++;
            }
            return built;
        }

        /// The tile mesh of one field (0x140ca47a0).
        static Mesh BuildMesh(Dictionary<string, object> f, int count)
        {
            var rng = new System.Random((int)(uint)Num(f, "seed", 0));
            Func<float> R = () => (float)rng.NextDouble();
            Vector2 clustering = V2(f, "clustering"), opacity = V2(f, "opacity"), angle = V2(f, "angle"),
                    rotation = V2(f, "rotation"), scaleX = V2(f, "scaleX"), scaleY = V2(f, "scaleY");
            int flags = (int)Num(f, "flags", 0);
            var cr = Arr(f, "texColRow");
            int frames = Mathf.Max(1, (cr.Length > 0 ? (int)cr[0] : 1) * (cr.Length > 1 ? (int)cr[1] : 1));
            int centres = Mathf.Max(1, (int)Num(f, "clusterCount", 1));
            var centre = new Vector3[centres];
            for (int i = 0; i < centres; ++i) centre[i] = new Vector3(R(), R(), R());

            var pos = new Vector3[count * 4];
            var col = new Color32[count * 4];
            var uv0 = new Vector2[count * 4];
            var uv1 = new Vector2[count * 4];
            var uv2 = new Vector2[count * 4];
            var idx = new int[count * 6];
            // corners (u, v) and their TEXCOORD2.y tags, in the client's vertex order
            Vector2[] corner = { new Vector2(0, 1), new Vector2(1, 1), new Vector2(0, 0), new Vector2(1, 0) };
            float[] tag = { 0.25f, 0.75f, -0.25f, -0.75f };
            for (int i = 0; i < count; ++i)
            {
                var p = new Vector3(R(), R(), R());
                float t = Mathf.Lerp(clustering.x, clustering.y, R());
                if (t != 0)
                {
                    Vector3 c = centre[rng.Next(centres)];
                    Vector3 d = c - p;
                    for (int k = 0; k < 3; ++k)
                        if (Mathf.Abs(d[k]) > 0.5f) d[k] = -Mathf.Sign(d[k]) * (1 - Mathf.Abs(d[k]));   // the wrapped delta
                    if ((flags & 0x100) != 0) d.z = 0;
                    p += d * t;
                    for (int k = 0; k < 3; ++k) p[k] -= Mathf.Floor(p[k]);
                }
                byte seed = (byte)rng.Next(255);
                byte alpha = (byte)Mathf.Clamp(Mathf.RoundToInt(Mathf.Lerp(opacity.x, opacity.y, R()) * 255), 0, 255);
                float sy = Mathf.Lerp(scaleY.x, scaleY.y, R()), sx = Mathf.Lerp(scaleX.x, scaleX.y, R());
                if ((flags & 0x1) != 0) sy = sx;
                float spin = Mathf.Lerp(rotation.x, rotation.y, R()), ang = Mathf.Lerp(angle.x, angle.y, R());
                float frame = rng.Next(frames);
                for (int k = 0; k < 4; ++k)
                {
                    int v = i * 4 + k;
                    pos[v] = p;
                    col[v] = new Color32((byte)(corner[k].x * 255), (byte)(corner[k].y * 255), seed, alpha);
                    uv0[v] = new Vector2(sx, sy);
                    uv1[v] = new Vector2(ang, spin);
                    uv2[v] = new Vector2(frame, tag[k]);
                }
                int b = i * 6, q = i * 4;
                idx[b] = q; idx[b + 1] = q + 1; idx[b + 2] = q + 2;
                idx[b + 3] = q + 2; idx[b + 4] = q + 1; idx[b + 5] = q + 3;
            }
            var mesh = new Mesh { indexFormat = IndexFormat.UInt32 };
            mesh.vertices = pos;
            mesh.colors32 = col;
            mesh.SetUVs(0, uv0);
            mesh.SetUVs(1, uv1);
            mesh.SetUVs(2, uv2);
            mesh.SetIndices(idx, MeshTopology.Triangles, 0, false);
            // drawn wherever the camera is: never culled
            mesh.bounds = new Bounds(Vector3.zero, Vector3.one * 1e8f);
            return mesh;
        }

        static Mesh SaveMesh(Mesh mesh, string path)
        {
            var existing = AssetDatabase.LoadAssetAtPath<Mesh>(path);
            if (existing == null) { AssetDatabase.CreateAsset(mesh, path); return mesh; }
            EditorUtility.CopySerialized(mesh, existing);
            existing.bounds = mesh.bounds;
            EditorUtility.SetDirty(existing);
            return existing;
        }

        static Material SaveMaterial(Shader shader, Dictionary<string, object> f, Texture2D texture, float unityScale, string path)
        {
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat == null) { mat = new Material(shader); AssetDatabase.CreateAsset(mat, path); }
            mat.shader = shader;
            mat.SetTexture("_MainTex", texture);
            mat.SetFloat("_FieldType", (float)Num(f, "type", 2));
            mat.SetFloat("_Extent", (float)Num(f, "extent", 54464));
            mat.SetFloat("_Deviation", (float)Num(f, "deviation", 0));
            Vector2 sp = V2(f, "speed"), ds = V2(f, "deviationSpeed"), per = V2(f, "period"), life = V2(f, "lifetime");
            mat.SetVector("_Speed", new Vector4(sp.x, sp.y, ds.x, ds.y));
            var dir = Arr(f, "fieldDirection");
            var d = dir.Length >= 3 ? new Vector3((float)dir[0], (float)dir[1], (float)dir[2]) : Vector3.zero;
            mat.SetVector("_Direction", d.sqrMagnitude > 1e-12f ? (Vector4)d.normalized : Vector4.zero);
            mat.SetVector("_Life", new Vector4(per.x, per.y, life.x, life.y));
            var cr = Arr(f, "texColRow");
            int flags = (int)Num(f, "flags", 0);
            float fps = (flags & 0x200) != 0 ? (float)Num(f, "texFPS", 0) : 0f;
            mat.SetVector("_Frames", new Vector4(0, cr.Length > 0 ? (float)cr[0] : 1, cr.Length > 1 ? (float)cr[1] : 1, fps));
            mat.SetFloat("_Visibility", 1f);   // flags 0x2 / 0x4 (time of day) and the camera height: not ported
            mat.SetFloat("_UnityScale", unityScale);
            EditorUtility.SetDirty(mat);
            return mat;
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }
        static string Safe(string s)
        {
            foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_');
            return s.Replace('(', '_').Replace(')', '_');
        }
        static string Str(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) ? v as string : null; }
        static double Num(Dictionary<string, object> d, string k, double def)
        {
            object v;
            return d.TryGetValue(k, out v) && v is double x ? x : def;
        }
        static double[] Arr(Dictionary<string, object> d, string k)
        {
            object v;
            if (!d.TryGetValue(k, out v) || !(v is List<object> l)) return new double[0];
            var r = new double[l.Count];
            for (int i = 0; i < l.Count; ++i) r[i] = l[i] is double x ? x : 0;
            return r;
        }
        static Vector2 V2(Dictionary<string, object> d, string k)
        {
            var a = Arr(d, k);
            return a.Length >= 2 ? new Vector2((float)a[0], (float)a[1]) : Vector2.zero;
        }
    }
}
