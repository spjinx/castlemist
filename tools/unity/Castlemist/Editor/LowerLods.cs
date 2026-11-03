// castlemist world import: lower LODs (2026-10-27, the user: "lower lods, proxies more present", room to
// add onto the map later). Castlemist > Lower LODs, and the importer's prefab step (after LodGroups.Build).
//   which        every model prefab (Prefabs/<fileId>.prefab, not foliage) whose LODGroup's last level draws
//                at least MinTriangles in MeshRenderers (skinned ones are left alone): 1076 of Mad King's
//                Labyrinth's 2680 LODGroups had one level only, full detail until the prop culled
//   the level    one more level after the last: each of the last level's meshes simplified to Ratio of its
//                triangles (Qem: quadric edge collapse on the position-welded mesh, so no cracks at UV and
//                normal seams; each corner keeps the source vertex at its new position whose UV0 and normal
//                are nearest its own), stopping early at MaxError of the mesh's size; a mesh it cannot halve
//                is drawn as it is. Objects "<base>_LOD<n>" next to their source, same materials and flags;
//                meshes in Meshes/Lods/ (no mesh compression: it quantizes the UVs, see BuildSize)
//   heights      the old last level ends at SplitFactor x the cull height (the cull distance / SplitFactor),
//                the new one at the cull height: the prop draws exactly as far as before
//   instances    placements with their own LOD array (OVERRIDE_LOD heights, LodGroups.Override) get the
//                prefab's levels with their own heights; LodGroups.Override keeps the split on re-import
// Rerunning replaces the generated level (the meshes are reused from Meshes/Lods/).

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class LowerLods
    {
        public const int MinTriangles = 150;     // the last level's triangles: below it nothing is generated
        public const int MinMeshTriangles = 60;  // a smaller mesh is drawn as it is in the new level
        public const float Ratio = 0.3f;         // of each mesh's triangles
        public const float MaxError = 0.02f;     // of the mesh's bounds diagonal, the collapse error that stops it
        public const float SplitFactor = 3f;
        public const string MeshFolder = "Meshes/Lods";

        static readonly Dictionary<Mesh, Mesh> cache = new Dictionary<Mesh, Mesh>();

        [MenuItem("Castlemist/Lower LODs (simplified extra level, in place)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply()); }

        [MenuItem("Castlemist/Optimization Pass (lower LODs, proxies, compression)")]
        public static void PassMenu() { Debug.Log("CASTLEMIST " + Pass()); }

        /// The whole pass on the open world scene: Apply, the cell proxies rebuilt (CellProxies, nearer
        /// and with smaller props since 2026-10-27), BuildSize.Optimize, the prop textures crunched at
        /// 1024 px and the sound effects at a lower Vorbis quality. One line per part.
        public static string Pass()
        {
            var parts = new List<string> { Apply() };
            parts.Add(CellProxies.Build(CellProxies.NearDistance, true));
            parts.Add(BuildSize.Optimize(true, true, true));
            parts.Add(BuildSize.PropTextures(BuildSize.MaxTexture, BuildSize.CrunchQuality));
            parts.Add(BuildSize.SmallerSounds(BuildSize.SoundQuality));
            return string.Join("\n", parts);
        }

        static string SceneRoot()
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            return string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json") ? null : root;
        }

        /// Every model prefab extended, then the scene's placements with their own LOD arrays given the
        /// new level; returns a one-line report.
        public static string Apply()
        {
            string root = SceneRoot();
            if (root == null) return "lower LODs: the open scene is not in a castlemist export folder";
            cache.Clear();
            int prefabs = 0, examined = 0;
            foreach (string g in AssetDatabase.FindAssets("t:Prefab", new[] { root + "/Prefabs" }))
            {
                string path = AssetDatabase.GUIDToAssetPath(g);
                if (path.Contains("/Foliage/")) continue;
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
                if (asset == null || asset.GetComponent<LODGroup>() == null) continue;
                ++examined;
                GameObject contents = PrefabUtility.LoadPrefabContents(path);
                try
                {
                    bool had = Strip(contents);
                    if (Extend(contents, root) || had) { PrefabUtility.SaveAsPrefabAsset(contents, path); if (GeneratedLevels(contents.GetComponent<LODGroup>().GetLODs()) > 0) ++prefabs; }
                }
                finally { PrefabUtility.UnloadPrefabContents(contents); }
            }
            int own = 0, mismatched = 0;
            foreach (LODGroup lg in Object.FindObjectsByType<LODGroup>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (!PrefabUtility.IsPartOfPrefabInstance(lg)) continue;
                MeshRenderer[] gens = lg.GetComponentsInChildren<MeshRenderer>(true).Where(r => IsGenerated(r)).ToArray();
                if (gens.Length == 0) continue;
                LOD[] ls = lg.GetLODs();
                if (GeneratedLevels(ls) == 0)
                {
                    LODGroup src = PrefabUtility.GetCorrespondingObjectFromSource(lg);
                    LOD[] pl = src != null ? src.GetLODs() : null;
                    if (pl == null || pl.Length != ls.Length + 1) { ++mismatched; continue; }
                    var map = new Dictionary<Renderer, Renderer>();
                    foreach (Renderer r in lg.GetComponentsInChildren<Renderer>(true))
                    {
                        Renderer s = PrefabUtility.GetCorrespondingObjectFromSource(r);
                        if (s != null) map[s] = r;
                    }
                    var nl = new LOD[pl.Length];
                    for (int k = 0; k < pl.Length; ++k)
                    {
                        Renderer[] rs = pl[k].renderers.Where(r => r != null && map.ContainsKey(r)).Select(r => map[r]).ToArray();
                        float h = k < ls.Length - 1 ? ls[k].screenRelativeTransitionHeight : ls[ls.Length - 1].screenRelativeTransitionHeight;
                        nl[k] = new LOD(h, rs) { fadeTransitionWidth = pl[k].fadeTransitionWidth };
                    }
                    Resplit(nl);
                    lg.SetLODs(nl);
                    ++own;
                }
                else
                {
                    // Following the prefab, a cull height the placement overrides by index (Performance.KeepNear)
                    // now lands on the level before the new one: the smaller of the two is its cull again.
                    int n = ls.Length;
                    float cull = Mathf.Min(ls[n - 1].screenRelativeTransitionHeight, ls[n - 2].screenRelativeTransitionHeight);
                    ls[n - 1].screenRelativeTransitionHeight = cull;
                    Resplit(ls);
                    SetHeights(lg, ls);
                }
                Performance.KeepNear(lg, Performance.KeepNearMetres);
                CopyBlocks(gens);
            }
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "lower LODs: " + prefabs + " of " + examined + " model prefab(s) with a simplified extra level (" + Ratio * 100 + "% of the last level's triangles, from 1/" +
                   SplitFactor + " of the cull distance); " + own + " placement(s) with their own heights given it" +
                   (mismatched > 0 ? "; " + mismatched + " placement(s) whose levels no longer match their prefab left as they were" : "");
        }

        /// @p ls's heights on @p lg through the serialized fields (a prefab instance overrides those values
        /// only, not its LOD renderer lists), or SetLODs when the level count differs.
        static void SetHeights(LODGroup lg, LOD[] ls)
        {
            var so = new SerializedObject(lg);
            SerializedProperty arr = so.FindProperty("m_LODs");
            if (arr == null || arr.arraySize != ls.Length) { lg.SetLODs(ls); return; }
            for (int k = 0; k < ls.Length; ++k)
            {
                SerializedProperty h = so.FindProperty("m_LODs.Array.data[" + k + "].screenRelativeHeight");
                if (h != null && !Mathf.Approximately(h.floatValue, ls[k].screenRelativeTransitionHeight)) h.floatValue = ls[k].screenRelativeTransitionHeight;
            }
            so.ApplyModifiedPropertiesWithoutUndo();
        }

        /// True for a renderer drawing a mesh of Meshes/Lods/.
        public static bool IsGenerated(Renderer r)
        {
            var mf = r != null ? r.GetComponent<MeshFilter>() : null;
            if (mf == null || mf.sharedMesh == null) return false;
            return AssetDatabase.GetAssetPath(mf.sharedMesh).Contains("/" + MeshFolder + "/");
        }

        /// 1 when the last of @p lods is a generated level, else 0.
        public static int GeneratedLevels(LOD[] lods)
        {
            int n = lods.Length;
            return n > 1 && lods[n - 1].renderers.Any(r => IsGenerated(r)) ? 1 : 0;
        }

        /// The generated (last) level's start: the level before it ends at SplitFactor x the cull height,
        /// or halfway (geometrically) to its own start when that is nearer.
        public static void Resplit(LOD[] lods)
        {
            int n = lods.Length;
            if (n < 2) return;
            float cull = lods[n - 1].screenRelativeTransitionHeight;
            float prev = n >= 3 ? lods[n - 3].screenRelativeTransitionHeight : 1f;
            float s = Mathf.Max(cull, 1e-5f) * SplitFactor;
            if (s >= prev * 0.98f) s = Mathf.Sqrt(Mathf.Max(cull, 1e-5f) * prev);
            lods[n - 2].screenRelativeTransitionHeight = Mathf.Clamp(s, cull + 1e-6f, Mathf.Max(prev * 0.999f, cull + 2e-6f));
        }

        /// Removes an earlier generated level and its objects from @p root; true when there was one.
        public static bool Strip(GameObject root)
        {
            LODGroup lg = root.GetComponent<LODGroup>();
            bool any = false;
            if (lg != null)
            {
                LOD[] ls = lg.GetLODs();
                if (GeneratedLevels(ls) > 0)
                {
                    var keep = new LOD[ls.Length - 1];
                    Array.Copy(ls, keep, keep.Length);
                    keep[keep.Length - 1].screenRelativeTransitionHeight = ls[ls.Length - 1].screenRelativeTransitionHeight;
                    lg.SetLODs(keep);
                    any = true;
                }
            }
            foreach (MeshRenderer r in root.GetComponentsInChildren<MeshRenderer>(true).Where(x => IsGenerated(x)).ToArray())
            {
                Object.DestroyImmediate(r.gameObject);
                any = true;
            }
            return any;
        }

        /// One simplified level after @p root's last (see the header); false when it has none to add.
        public static bool Extend(GameObject root, string exportRoot)
        {
            LODGroup lg = root.GetComponent<LODGroup>();
            if (lg == null) return false;
            LOD[] lods = lg.GetLODs();
            if (lods.Length == 0 || GeneratedLevels(lods) > 0) return false;
            int n = lods.Length;
            Renderer[] last = lods[n - 1].renderers.Where(r => r != null).ToArray();
            if (last.Any(r => r is SkinnedMeshRenderer)) return false;
            int before = 0;
            foreach (Renderer r in last)
            {
                Mesh m = r is MeshRenderer ? r.GetComponent<MeshFilter>()?.sharedMesh : null;
                if (m != null) before += Triangles(m);
            }
            if (before < MinTriangles) return false;
            var made = new List<Renderer>();
            var keep = new List<Renderer>();
            int after = 0;
            foreach (Renderer r in last)
            {
                MeshFilter mf = r is MeshRenderer ? r.GetComponent<MeshFilter>() : null;
                Mesh src = mf != null ? mf.sharedMesh : null;
                Mesh low = src != null ? Simplified(src, exportRoot) : null;
                if (low == null) { keep.Add(r); if (src != null) after += Triangles(src); continue; }
                after += Triangles(low);
                made.Add(Clone(r, low, n));
            }
            if (made.Count == 0 || after > before * 0.8f)
            {
                foreach (Renderer r in made) Object.DestroyImmediate(r.gameObject);
                return false;
            }
            var nl = new LOD[n + 1];
            Array.Copy(lods, nl, n);
            nl[n] = new LOD(lods[n - 1].screenRelativeTransitionHeight, made.Concat(keep).ToArray()) { fadeTransitionWidth = lods[n - 1].fadeTransitionWidth };
            Resplit(nl);
            lg.SetLODs(nl);
            return true;
        }

        static Renderer Clone(Renderer r, Mesh low, int level)
        {
            string b;
            ModelLods.LevelOf(r.name, out b);
            var go = new GameObject(b + "_LOD" + level);
            Transform t = go.transform;
            t.SetParent(r.transform.parent, false);
            t.localPosition = r.transform.localPosition;
            t.localRotation = r.transform.localRotation;
            t.localScale = r.transform.localScale;
            go.layer = r.gameObject.layer;
            go.tag = r.gameObject.tag;
            GameObjectUtility.SetStaticEditorFlags(go, GameObjectUtility.GetStaticEditorFlags(r.gameObject));
            go.AddComponent<MeshFilter>().sharedMesh = low;
            var mr = go.AddComponent<MeshRenderer>();
            // Static flags include ContributeGI, but a new renderer defaults to Lightmaps.
            // Generated LODs use the map's Light Volumes; exclude them from surface atlases.
            RepairLighting(mr);
            mr.sharedMaterials = r.sharedMaterials;
            mr.shadowCastingMode = r.shadowCastingMode;
            mr.receiveShadows = r.receiveShadows;
            mr.lightProbeUsage = r.lightProbeUsage;
            mr.reflectionProbeUsage = r.reflectionProbeUsage;
            mr.motionVectorGenerationMode = r.motionVectorGenerationMode;
            mr.allowOcclusionWhenDynamic = r.allowOcclusionWhenDynamic;
            mr.enabled = r.enabled;
            go.SetActive(r.gameObject.activeSelf);
            return mr;
        }

        /// A placed prop's per-renderer property blocks (PropColors' glow colour) copied onto its generated
        /// renderers, from the source each was made from (the sibling of the same base name, highest level).
        static void CopyBlocks(MeshRenderer[] gens)
        {
            var mpb = new MaterialPropertyBlock();
            foreach (MeshRenderer g in gens)
            {
                // Also repair scene overrides left by an earlier optimization pass.
                RepairLighting(g);
                string gb;
                int level = ModelLods.LevelOf(g.name, out gb);
                Renderer best = null;
                int bestLevel = -1;
                Transform p = g.transform.parent;
                if (p == null) continue;
                foreach (Transform s in p)
                {
                    Renderer r = s.GetComponent<MeshRenderer>();
                    if (r == null || r == g || IsGenerated(r)) continue;
                    string sb;
                    int k = ModelLods.LevelOf(s.name, out sb);
                    if (sb == gb && k < level && k > bestLevel) { best = r; bestLevel = k; }
                }
                if (best == null || !best.HasPropertyBlock()) continue;
                best.GetPropertyBlock(mpb);
                g.SetPropertyBlock(mpb);
            }
        }

        /// Generated distance geometry must neither contribute GI nor allocate surface lightmaps.
        public static void RepairLighting(MeshRenderer r)
        {
            StaticEditorFlags flags = GameObjectUtility.GetStaticEditorFlags(r.gameObject);
            if ((flags & StaticEditorFlags.ContributeGI) != 0)
                GameObjectUtility.SetStaticEditorFlags(r.gameObject, flags & ~StaticEditorFlags.ContributeGI);
            SceneSetup.ProbesOnly(r);
        }

        static int Triangles(Mesh m)
        {
            long n = 0;
            for (int s = 0; s < m.subMeshCount; ++s) if (m.GetTopology(s) == MeshTopology.Triangles) n += m.GetIndexCount(s) / 3;
            return (int)n;
        }

        /// @p src simplified (Meshes/Lods/, made once); null when it is small or would not halve.
        static Mesh Simplified(Mesh src, string exportRoot)
        {
            if (cache.TryGetValue(src, out Mesh hit)) return hit;
            Mesh result = null;
            if (Triangles(src) >= MinMeshTriangles)
            {
                string guid;
                long lid;
                string key = AssetDatabase.TryGetGUIDAndLocalFileIdentifier(src, out guid, out lid) ? guid.Substring(0, 8) + "_" + (lid & 0xFFFFFFF) : src.GetInstanceID().ToString();
                string folder = exportRoot + "/" + MeshFolder;
                EnsureFolder(folder);
                string path = folder + "/" + Safe(src.name) + "_" + key + ".asset";
                result = AssetDatabase.LoadAssetAtPath<Mesh>(path);
                if (result == null)
                {
                    result = Qem.Simplify(src, Ratio, MaxError);
                    if (result != null && Triangles(result) > Triangles(src) * 0.5f) result = null;
                    if (result != null) AssetDatabase.CreateAsset(result, path);
                }
            }
            cache[src] = result;
            return result;
        }

        static string Safe(string s) { foreach (char c in Path.GetInvalidFileNameChars()) s = s.Replace(c, '_'); return s.Replace('|', '_'); }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            string parent = Path.GetDirectoryName(path).Replace('\\', '/');
            EnsureFolder(parent);
            AssetDatabase.CreateFolder(parent, Path.GetFileName(path));
        }

        /// Quadric error edge collapse (Garland and Heckbert) over the mesh's positions welded, every
        /// triangle's plane and, along open edges, a perpendicular plane (weight BorderWeight) so borders
        /// keep their outline. A collapse moves one end onto the other (no new positions, so every source
        /// attribute stays exact) and is refused when it would turn a triangle over.
        static class Qem
        {
            const double BorderWeight = 8.0;

            struct Entry { public double Cost; public int From, To, VFrom, VTo; }

            public static Mesh Simplify(Mesh src, float ratio, float maxErrorFrac)
            {
                Vector3[] v = src.vertices;
                int vc = v.Length;
                if (vc < 3) return null;
                Vector3[] nrm = src.normals;
                var uv0 = new List<Vector2>();
                src.GetUVs(0, uv0);
                bool hasN = nrm != null && nrm.Length == vc, hasUv = uv0.Count == vc;
                float diag = src.bounds.size.magnitude;
                if (!(diag > 0f)) return null;

                // Positions welded on a grid of diag / 1e5.
                float q = diag * 1e-5f;
                var weld = new Dictionary<Vector3Int, int>();
                var posOf = new int[vc];
                var pos = new List<Vector3>();
                for (int i = 0; i < vc; ++i)
                {
                    var key = new Vector3Int(Mathf.RoundToInt(v[i].x / q), Mathf.RoundToInt(v[i].y / q), Mathf.RoundToInt(v[i].z / q));
                    if (!weld.TryGetValue(key, out int p)) { p = pos.Count; weld[key] = p; pos.Add(v[i]); }
                    posOf[i] = p;
                }
                int pc = pos.Count;
                var rawAt = new List<int>[pc];
                for (int i = 0; i < pc; ++i) rawAt[i] = new List<int>(2);
                for (int i = 0; i < vc; ++i) rawAt[posOf[i]].Add(i);

                // Triangles: raw corners, their submesh.
                int subs = src.subMeshCount;
                var corners = new List<int>();
                var subOf = new List<int>();
                for (int s = 0; s < subs; ++s)
                {
                    if (src.GetTopology(s) != MeshTopology.Triangles) return null;
                    int[] idx = src.GetTriangles(s);
                    for (int i = 0; i + 2 < idx.Length; i += 3) { corners.Add(idx[i]); corners.Add(idx[i + 1]); corners.Add(idx[i + 2]); subOf.Add(s); }
                }
                int tc = subOf.Count;
                var cp = new int[tc * 3];   // corner positions
                for (int i = 0; i < tc * 3; ++i) cp[i] = posOf[corners[i]];
                var dead = new bool[tc];
                int live = 0;
                var trisOf = new List<int>[pc];
                for (int i = 0; i < pc; ++i) trisOf[i] = new List<int>(6);
                var Q = new double[pc * 10];
                var edgeUse = new Dictionary<long, int>();
                var edgeTri = new Dictionary<long, int>();
                for (int t = 0; t < tc; ++t)
                {
                    int a = cp[3 * t], b = cp[3 * t + 1], c = cp[3 * t + 2];
                    if (a == b || b == c || a == c) { dead[t] = true; continue; }
                    ++live;
                    trisOf[a].Add(t); trisOf[b].Add(t); trisOf[c].Add(t);
                    Vector3 n = Vector3.Cross(pos[b] - pos[a], pos[c] - pos[a]);
                    if (n.sqrMagnitude < 1e-20f) continue;
                    n.Normalize();
                    double d = -Vector3.Dot(n, pos[a]);
                    AddPlane(Q, a, n, d, 1); AddPlane(Q, b, n, d, 1); AddPlane(Q, c, n, d, 1);
                    for (int e = 0; e < 3; ++e)
                    {
                        long k = Key(cp[3 * t + e], cp[3 * t + (e + 1) % 3]);
                        edgeUse[k] = edgeUse.TryGetValue(k, out int u) ? u + 1 : 1;
                        edgeTri[k] = t;
                    }
                }
                foreach (var kv in edgeUse)
                {
                    if (kv.Value != 1) continue;
                    int a = (int)(kv.Key >> 32), b = (int)(kv.Key & 0xFFFFFFFF);
                    int t = edgeTri[kv.Key];
                    Vector3 fn = Vector3.Cross(pos[cp[3 * t + 1]] - pos[cp[3 * t]], pos[cp[3 * t + 2]] - pos[cp[3 * t]]);
                    Vector3 bn = Vector3.Cross(pos[b] - pos[a], fn);
                    if (bn.sqrMagnitude < 1e-20f) continue;
                    bn.Normalize();
                    double d = -Vector3.Dot(bn, pos[a]);
                    AddPlane(Q, a, bn, d, BorderWeight); AddPlane(Q, b, bn, d, BorderWeight);
                }

                var version = new int[pc];
                var gone = new bool[pc];
                var heap = new List<Entry>(edgeUse.Count);
                foreach (long k in edgeUse.Keys) Push(heap, Q, pos, version, (int)(k >> 32), (int)(k & 0xFFFFFFFF));
                int target = Mathf.Max(4, Mathf.RoundToInt(live * ratio));
                double maxCost = (double)(diag * maxErrorFrac) * (diag * maxErrorFrac) * 4.0;

                while (live > target && heap.Count > 0)
                {
                    Entry e = Pop(heap);
                    if (gone[e.From] || gone[e.To] || version[e.From] != e.VFrom || version[e.To] != e.VTo) continue;
                    if (e.Cost > maxCost) break;
                    int a = e.From, b = e.To;
                    if (Flips(a, b, trisOf[a], cp, dead, pos)) continue;
                    foreach (int t in trisOf[a])
                    {
                        if (dead[t]) continue;
                        bool hasB = cp[3 * t] == b || cp[3 * t + 1] == b || cp[3 * t + 2] == b;
                        if (hasB) { dead[t] = true; --live; continue; }
                        for (int c = 0; c < 3; ++c)
                        {
                            if (cp[3 * t + c] != a) continue;
                            cp[3 * t + c] = b;
                            corners[3 * t + c] = Nearest(corners[3 * t + c], rawAt[b], uv0, nrm, hasUv, hasN);
                        }
                        trisOf[b].Add(t);
                    }
                    for (int i = 0; i < 10; ++i) Q[b * 10 + i] += Q[a * 10 + i];
                    gone[a] = true;
                    ++version[a]; ++version[b];
                    var seen = new HashSet<int>();
                    foreach (int t in trisOf[b])
                    {
                        if (dead[t]) continue;
                        for (int c = 0; c < 3; ++c)
                        {
                            int u = cp[3 * t + c];
                            if (u != b && seen.Add(u)) Push(heap, Q, pos, version, b, u);
                        }
                    }
                }

                // Output: the used source vertices, compacted, every attribute copied.
                var remap = new int[vc];
                for (int i = 0; i < vc; ++i) remap[i] = -1;
                var order = new List<int>();
                var subIdx = new List<int>[subs];
                for (int s = 0; s < subs; ++s) subIdx[s] = new List<int>();
                for (int t = 0; t < tc; ++t)
                {
                    if (dead[t]) continue;
                    for (int c = 0; c < 3; ++c)
                    {
                        int r = corners[3 * t + c];
                        if (remap[r] < 0) { remap[r] = order.Count; order.Add(r); }
                        subIdx[subOf[t]].Add(remap[r]);
                    }
                }
                if (order.Count < 3) return null;
                var m = new Mesh { name = src.name + "_low" };
                if (order.Count > 65535) m.indexFormat = IndexFormat.UInt32;
                m.SetVertices(order.Select(i => v[i]).ToList());
                if (hasN) m.SetNormals(order.Select(i => nrm[i]).ToList());
                Vector4[] tan = src.tangents;
                if (tan != null && tan.Length == vc) m.SetTangents(order.Select(i => tan[i]).ToList());
                Color32[] col = src.colors32;
                if (col != null && col.Length == vc) m.SetColors(order.Select(i => col[i]).ToList());
                for (int ch = 0; ch < 8; ++ch)
                {
                    var attr = (VertexAttribute)((int)VertexAttribute.TexCoord0 + ch);
                    if (!src.HasVertexAttribute(attr)) continue;
                    int dim = src.GetVertexAttributeDimension(attr);
                    var u4 = new List<Vector4>();
                    src.GetUVs(ch, u4);
                    if (u4.Count != vc) continue;
                    if (dim <= 2) m.SetUVs(ch, order.Select(i => (Vector2)u4[i]).ToList());
                    else if (dim == 3) m.SetUVs(ch, order.Select(i => (Vector3)u4[i]).ToList());
                    else m.SetUVs(ch, order.Select(i => u4[i]).ToList());
                }
                m.subMeshCount = subs;
                for (int s = 0; s < subs; ++s) m.SetTriangles(subIdx[s], s, false);
                m.RecalculateBounds();
                return m;
            }

            static long Key(int a, int b) { return a < b ? ((long)a << 32) | (uint)b : ((long)b << 32) | (uint)a; }

            static void AddPlane(double[] Q, int p, Vector3 n, double d, double w)
            {
                int o = p * 10;
                double a = n.x, b = n.y, c = n.z;
                Q[o] += w * a * a; Q[o + 1] += w * a * b; Q[o + 2] += w * a * c; Q[o + 3] += w * a * d;
                Q[o + 4] += w * b * b; Q[o + 5] += w * b * c; Q[o + 6] += w * b * d;
                Q[o + 7] += w * c * c; Q[o + 8] += w * c * d; Q[o + 9] += w * d * d;
            }

            static double Eval(double[] Q, int a, int b, Vector3 p)
            {
                int i = a * 10, j = b * 10;
                double x = p.x, y = p.y, z = p.z;
                double q0 = Q[i] + Q[j], q1 = Q[i + 1] + Q[j + 1], q2 = Q[i + 2] + Q[j + 2], q3 = Q[i + 3] + Q[j + 3], q4 = Q[i + 4] + Q[j + 4];
                double q5 = Q[i + 5] + Q[j + 5], q6 = Q[i + 6] + Q[j + 6], q7 = Q[i + 7] + Q[j + 7], q8 = Q[i + 8] + Q[j + 8], q9 = Q[i + 9] + Q[j + 9];
                return q0 * x * x + 2 * q1 * x * y + 2 * q2 * x * z + 2 * q3 * x + q4 * y * y + 2 * q5 * y * z + 2 * q6 * y + q7 * z * z + 2 * q8 * z + q9;
            }

            static void Push(List<Entry> heap, double[] Q, List<Vector3> pos, int[] version, int a, int b)
            {
                double ab = Eval(Q, a, b, pos[b]), ba = Eval(Q, a, b, pos[a]);
                var e = ab <= ba ? new Entry { Cost = ab, From = a, To = b, VFrom = version[a], VTo = version[b] }
                                 : new Entry { Cost = ba, From = b, To = a, VFrom = version[b], VTo = version[a] };
                heap.Add(e);
                int i = heap.Count - 1;
                while (i > 0)
                {
                    int p = (i - 1) / 2;
                    if (heap[p].Cost <= heap[i].Cost) break;
                    Entry t = heap[p]; heap[p] = heap[i]; heap[i] = t;
                    i = p;
                }
            }

            static Entry Pop(List<Entry> heap)
            {
                Entry top = heap[0];
                int last = heap.Count - 1;
                heap[0] = heap[last];
                heap.RemoveAt(last);
                int i = 0, n = heap.Count;
                while (true)
                {
                    int l = 2 * i + 1, r = l + 1, m = i;
                    if (l < n && heap[l].Cost < heap[m].Cost) m = l;
                    if (r < n && heap[r].Cost < heap[m].Cost) m = r;
                    if (m == i) break;
                    Entry t = heap[m]; heap[m] = heap[i]; heap[i] = t;
                    i = m;
                }
                return top;
            }

            /// True when moving @p a onto @p b turns one of a's other triangles over (or flattens it).
            static bool Flips(int a, int b, List<int> tris, int[] cp, bool[] dead, List<Vector3> pos)
            {
                foreach (int t in tris)
                {
                    if (dead[t]) continue;
                    int p0 = cp[3 * t], p1 = cp[3 * t + 1], p2 = cp[3 * t + 2];
                    if (p0 == b || p1 == b || p2 == b) continue;
                    Vector3 n0 = Vector3.Cross(pos[p1] - pos[p0], pos[p2] - pos[p0]);
                    Vector3 q0 = p0 == a ? pos[b] : pos[p0], q1 = p1 == a ? pos[b] : pos[p1], q2 = p2 == a ? pos[b] : pos[p2];
                    Vector3 n1 = Vector3.Cross(q1 - q0, q2 - q0);
                    if (n1.sqrMagnitude < 1e-12f * n0.sqrMagnitude) return true;
                    if (Vector3.Dot(n0.normalized, n1.normalized) < 0.2f) return true;
                }
                return false;
            }

            /// Of the source vertices at the new position, the one whose UV0 and normal are nearest @p raw's.
            static int Nearest(int raw, List<int> cands, List<Vector2> uv, Vector3[] n, bool hasUv, bool hasN)
            {
                if (cands.Count == 1) return cands[0];
                int best = cands[0];
                float bestS = float.MaxValue;
                foreach (int c in cands)
                {
                    float s = 0f;
                    if (hasUv) s += (uv[c] - uv[raw]).sqrMagnitude;
                    if (hasN) s += 0.5f * (1f - Vector3.Dot(n[c], n[raw]));
                    if (s < bestS) { bestS = s; best = c; }
                }
                return best;
            }
        }
    }
}
