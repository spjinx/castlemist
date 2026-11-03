// castlemist world export: one proxy mesh per map cell for distance (2026-10-26). Castlemist > Build Cell
// Proxies. Draw calls, not triangles, held Lion's Arch at 78-85 ms over the city (~12000 per frame,
// twice with the sun's depth pass); fewer triangles per prop would not have changed that.
//   which props    each Cell_<id>/Props placement with no Animator / Animation, at least MinSize across,
//                  active; of it, the MeshRenderers whose every material is a solid castlemist one
//                  (Castlemist/GW2 or GW2 Tessellated, opaque; with cutout, cut-out too: palms, nets,
//                  railings as solid shapes). Transparent (glass, water) and animated props stay real
//   geometry       each such placement's lowest LOD level that has them (its last LOD), in the cell's
//                  frame, one mesh per cell (Meshes/Proxies/<cell>.asset, Medium compression)
//   simplified     vertex clustering on a Cluster-metre grid: the vertices in one grid cell merge
//                  (mean position, normal and colour), triangles that collapse go (Lion's Arch's lowest
//                  LODs were still 8.9M triangles over 96 cells)
//   colour         2026-10-26: per vertex, its material's _MainTex (TexSize px, linear) at the vertex's UV
//                  x _Color (one mean per material read grey); without UVs, per submesh, the mean _MainTex colour (the texture's smallest mip, read
//                  linear) x _Color: vertex colour, drawn by Castlemist/GW2 Proxy
//   tiles          2026-10-26: by Tile-metre XZ tile, not GW2 cell (cells are ~300 m: the one you stood in
//                  drew all its props); proxies under the environment root's "GW2 Proxies"
//   switching      GW2CellSwitcher (under the environment root) turns a tile's merged renderers off and
//                  its proxy on beyond NearDistance, and its ambient Animators off (doors and demolishables
//                  keep theirs); the scene keeps the real props on, proxies off
// Remove (Castlemist > Remove Cell Proxies) deletes the proxies and the switcher.
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
    public static class CellProxies
    {
        public const string ProxyName = "GW2 Proxy";
        public const string SwitcherName = "GW2 Cell Switcher";
        public const float MinSize = 2f;          // metres (2026-10-27, 4 before: "proxies more present"; a 2 m prop still draws ~170 m out)
        public const float Cluster = 3f;          // metres: vertex clustering grid (Lion's Arch: 8.9M triangles unclustered, 7.9M at 1.5 m)
        public const float NearDistance = 110f;   // 2026-10-27: 150 before ("proxies more present"); at 75 the night's thin haze showed them plainly
        public const float Tile = 64f;            // metres: the switching grid
        public const string ContainerName = "GW2 Proxies";

        class Tiled
        {
            public readonly List<MeshRenderer> Geo = new List<MeshRenderer>();
            public readonly List<Renderer> Off = new List<Renderer>();
            public readonly List<Animator> Anims = new List<Animator>();
            public readonly List<GameObject> Particles = new List<GameObject>();
            public Bounds B;
        }

        [MenuItem("Castlemist/Build Cell Proxies (far cells as one mesh)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Build(NearDistance, true)); }

        [MenuItem("Castlemist/Remove Cell Proxies")]
        public static void RemoveMenu() { Debug.Log("CASTLEMIST " + Remove()); }

        /// @p cutout: cut-out props (palms, nets, railings) go in as solid shapes too (their mean colour over
        /// the texels they keep); without, they stay real at every distance.
        public static string Build(float near, bool cutout)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "proxies: the open scene is not in a castlemist export folder";
            GameObject mapRoot = EditorSceneManager.GetActiveScene().GetRootGameObjects()
                .FirstOrDefault(g => g.name.StartsWith("GW2_") && g.name.Substring(4).All(char.IsDigit));
            GameObject envRoot = ImmersiveLighting.EnvRoot();
            Type sw = SceneSetup.FindType("Castlemist.GW2CellSwitcher");
            if (mapRoot == null || envRoot == null) return "proxies: no GW2_<id> map root / environment root";
            if (sw == null) return "proxies: GW2CellSwitcher is not compiled (UdonSharp, CASTLEMIST_VRC)";
            Remove();
            string dir = Folder(root, "Meshes/Proxies");
            var mat = ProxyMaterial(root);
            var colours = new Dictionary<Material, Color>();
            smalls.Clear();
            coverage.Clear();

            // Placements by Tile-metre XZ tile (2026-10-26: GW2 cells are ~300 m across; switched per cell,
            // the one you stand in drew all its props). Animated ones contribute their Animators (frozen
            // far away), doors and demolishables excepted.
            Type door = SceneSetup.FindType("Castlemist.GW2ProximityDoor"), demolish = SceneSetup.FindType("Castlemist.GW2Demolishable");
            var tiles = new Dictionary<Vector2Int, Tiled>();
            // Props on map layers (festivals) switch at runtime: never merged, or a hidden festival would
            // still stand in the far tiles.
            var layered = new HashSet<GameObject>();
            Type layerType = SceneSetup.FindType("Castlemist.GW2MapLayer");
            if (layerType != null)
                foreach (Component l in Object.FindObjectsByType(layerType, FindObjectsInactive.Include, FindObjectsSortMode.None))
                    if (layerType.GetField("objects")?.GetValue(l) is GameObject[] objs) foreach (GameObject o in objs) if (o != null) layered.Add(o);
            foreach (Transform cell in mapRoot.transform)
            {
                if (!cell.name.StartsWith("Cell_")) continue;
                Transform props = cell.Find("Props");
                if (props == null) continue;
                foreach (Transform inst in props)
                {
                    if (!inst.gameObject.activeInHierarchy) continue;
                    // Every placement's particle systems (top ones) switch off with a far tile.
                    var pss = inst.GetComponentsInChildren<ParticleSystem>(true)
                        .Where(ps => ps.transform.parent == null || ps.transform.parent.GetComponentInParent<ParticleSystem>(true) == null)
                        .Select(ps => ps.gameObject).ToList();
                    if (pss.Count > 0)
                    {
                        Bounds pb = new Bounds(inst.position, Vector3.one);
                        var pkey = new Vector2Int(Mathf.FloorToInt(pb.center.x / Tile), Mathf.FloorToInt(pb.center.z / Tile));
                        if (!tiles.TryGetValue(pkey, out Tiled pt)) tiles[pkey] = pt = new Tiled { B = pb };
                        pt.B.Encapsulate(pb);
                        pt.Particles.AddRange(pss);
                    }
                    bool animated = inst.GetComponentInChildren<Animator>(true) != null || inst.GetComponentInChildren<Animation>(true) != null;
                    List<MeshRenderer> solid = animated || layered.Contains(inst.gameObject) ? new List<MeshRenderer>()
                        : inst.GetComponentsInChildren<MeshRenderer>(false).Where(r => r.enabled && Solid(r, cutout)).ToList();
                    bool special = (door != null && inst.GetComponentInChildren(door, true) != null) || (demolish != null && inst.GetComponentInChildren(demolish, true) != null);
                    var anims = animated && !special ? inst.GetComponentsInChildren<Animator>(true).ToList() : new List<Animator>();
                    if (solid.Count == 0 && anims.Count == 0) continue;
                    var rs = solid.Count > 0 ? solid.Cast<Renderer>().ToList() : inst.GetComponentsInChildren<Renderer>(false).ToList();
                    if (rs.Count == 0) continue;
                    Bounds b = rs[0].bounds;
                    foreach (Renderer r in rs) b.Encapsulate(r.bounds);
                    if (solid.Count > 0 && b.size.magnitude < MinSize) { solid.Clear(); if (anims.Count == 0) continue; }
                    var key = new Vector2Int(Mathf.FloorToInt(b.center.x / Tile), Mathf.FloorToInt(b.center.z / Tile));
                    if (!tiles.TryGetValue(key, out Tiled t)) tiles[key] = t = new Tiled { B = b };
                    t.B.Encapsulate(b);
                    t.Anims.AddRange(anims);
                    if (solid.Count == 0) continue;
                    List<MeshRenderer> src = solid;
                    LODGroup g = inst.GetComponent<LODGroup>();
                    if (g != null)
                    {
                        LOD[] ls = g.GetLODs();
                        for (int i = ls.Length - 1; i >= 0; --i)
                        {
                            var lv = ls[i].renderers.OfType<MeshRenderer>().Where(r => solid.Contains(r)).ToList();
                            if (lv.Count > 0) { src = lv; break; }
                        }
                    }
                    t.Geo.AddRange(src);
                    t.Off.AddRange(solid);
                }
            }

            var container = new GameObject(ContainerName);
            container.transform.SetParent(envRoot.transform, false);
            var proxies = new List<GameObject>();
            var rmin = new List<Vector2>();
            var rmax = new List<Vector2>();
            var start = new List<int>();
            var flat = new List<Renderer>();
            var astart = new List<int>();
            var aflat = new List<Animator>();
            var pstart = new List<int>();
            var pflat = new List<GameObject>();
            long tris = 0;
            int meshes = 0;
            foreach (var kv in tiles.OrderBy(k => k.Key.y).ThenBy(k => k.Key.x))
            {
                Tiled t = kv.Value;
                string tname = "Tile_" + kv.Key.x + "_" + kv.Key.y;
                GameObject go = null;
                if (t.Geo.Count > 0)
                {
                    go = new GameObject(ProxyName);
                    go.transform.SetParent(container.transform, false);
                    go.transform.position = new Vector3((kv.Key.x + 0.5f) * Tile, t.B.center.y, (kv.Key.y + 0.5f) * Tile);
                    Mesh mesh = Merge(t.Geo, go.transform, colours, tname);
                    if (mesh == null) { Object.DestroyImmediate(go); go = null; }
                    else
                    {
                        string path = dir + "/" + tname + ".asset";
                        if (AssetDatabase.LoadMainAssetAtPath(path) != null) AssetDatabase.DeleteAsset(path);
                        MeshUtility.SetMeshCompression(mesh, ModelImporterMeshCompression.Medium);
                        AssetDatabase.CreateAsset(mesh, path);
                        go.AddComponent<MeshFilter>().sharedMesh = mesh;
                        var mr = go.AddComponent<MeshRenderer>();
                        mr.sharedMaterial = mat;
                        mr.shadowCastingMode = ShadowCastingMode.Off;
                        mr.receiveShadows = false;
                        mr.lightProbeUsage = LightProbeUsage.Off;
                        mr.reflectionProbeUsage = ReflectionProbeUsage.Off;
                        go.SetActive(false);
                        tris += mesh.triangles.LongLength / 3;
                        ++meshes;
                    }
                }
                if (go == null && t.Anims.Count == 0 && t.Particles.Count == 0) continue;
                proxies.Add(go);
                rmin.Add(new Vector2(t.B.min.x, t.B.min.z));
                rmax.Add(new Vector2(t.B.max.x, t.B.max.z));
                start.Add(flat.Count);
                if (go != null) flat.AddRange(t.Off);
                astart.Add(aflat.Count);
                aflat.AddRange(t.Anims);
                pstart.Add(pflat.Count);
                pflat.AddRange(t.Particles);
            }
            pstart.Add(pflat.Count);
            // The per-cell proxy meshes from before tiles.
            foreach (string g in AssetDatabase.FindAssets("Cell_ t:Mesh", new[] { dir })) AssetDatabase.DeleteAsset(AssetDatabase.GUIDToAssetPath(g));
            astart.Add(aflat.Count);
            start.Add(flat.Count);
            var s = new GameObject(SwitcherName);
            s.transform.SetParent(envRoot.transform, false);
            Component c = SceneSetup.AddUdonAware(s, sw);
            SceneSetup.SetField(c, "proxies", proxies.ToArray());
            SceneSetup.SetField(c, "rectMin", rmin.ToArray());
            SceneSetup.SetField(c, "rectMax", rmax.ToArray());
            SceneSetup.SetField(c, "cellStart", start.ToArray());
            SceneSetup.SetField(c, "renderers", flat.ToArray());
            SceneSetup.SetField(c, "animStart", astart.ToArray());
            SceneSetup.SetField(c, "animators", aflat.ToArray());
            SceneSetup.SetField(c, "particleStart", pstart.ToArray());
            SceneSetup.SetField(c, "particles", pflat.ToArray());
            SceneSetup.SetField(c, "nearDistance", near);
            SceneSetup.CopyProxyToUdon(c);
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "proxies: " + proxies.Count + " tile(s) of " + Tile + " m (" + meshes + " proxy mesh(es), " + tris + " triangle(s)), " + flat.Count +
                   " renderer(s) stood in for, " + aflat.Count + " animator(s) frozen and " + pflat.Count + " particle system(s) off far away; real props within " + near + " m";
        }

        public static string Remove()
        {
            int n = 0;
            foreach (MeshRenderer r in Object.FindObjectsByType<MeshRenderer>(FindObjectsInactive.Include, FindObjectsSortMode.None))
                if (r != null && r.name == ProxyName && r.transform.parent != null && r.transform.parent.name.StartsWith("Cell_"))
                {
                    Object.DestroyImmediate(r.gameObject);
                    ++n;
                }
            GameObject envRoot = ImmersiveLighting.EnvRoot();
            Transform box = envRoot != null ? envRoot.transform.Find(ContainerName) : null;
            if (box != null) { n += box.childCount; Object.DestroyImmediate(box.gameObject); }
            Transform s = envRoot != null ? envRoot.transform.Find(SwitcherName) : null;
            if (s != null) Object.DestroyImmediate(s.gameObject);
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "proxies: " + n + " removed" + (s != null ? ", switcher removed" : "");
        }

        static bool Solid(Renderer r, bool cutout)
        {
            Material[] ms = r.sharedMaterials;
            if (ms.Length == 0) return false;
            foreach (Material m in ms)
            {
                if (m == null || m.shader == null) return false;
                string n = m.shader.name;
                if (n != "Castlemist/GW2" && n != "Castlemist/GW2 Tessellated") return false;
                if (m.renderQueue >= (cutout ? (int)RenderQueue.GeometryLast + 1 : (int)RenderQueue.AlphaTest)) return false;
                // 2026-10-26: leaf and branch cards (most of the texture cut away) became solid quads and
                // triangles far away (Mad King's Labyrinth's trees): they stay real, culled by their LODs
                if (m.renderQueue >= (int)RenderQueue.AlphaTest && Coverage(m) < SparseCoverage) return false;
            }
            return true;
        }

        const float SparseCoverage = 0.5f;
        static readonly Dictionary<Material, float> coverage = new Dictionary<Material, float>();

        /// The share of @p m's _MainTex (at TexSize px) its alpha test keeps (alpha >= _Cutoff); 1 without one.
        static float Coverage(Material m)
        {
            if (coverage.TryGetValue(m, out float c)) return c;
            c = 1f;
            Texture src = m.HasProperty("_MainTex") ? m.GetTexture("_MainTex") : null;
            if (src != null)
            {
                var rt = RenderTexture.GetTemporary(TexSize, TexSize, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
                Graphics.Blit(src, rt);
                var prev = RenderTexture.active;
                RenderTexture.active = rt;
                var tex = new Texture2D(TexSize, TexSize, TextureFormat.RGBA32, false, true);
                tex.ReadPixels(new Rect(0, 0, TexSize, TexSize), 0, 0);
                tex.Apply();
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(rt);
                float cut = m.HasProperty("_Cutoff") ? m.GetFloat("_Cutoff") : 0.5f;
                int kept = 0;
                Color[] px = tex.GetPixels();
                foreach (Color p in px) if (p.a >= cut) ++kept;
                c = (float)kept / px.Length;
                Object.DestroyImmediate(tex);
            }
            coverage[m] = c;
            return c;
        }

        /// @p rs merged into one mesh in @p frame's space, vertex-coloured per submesh material.
        static Mesh Merge(List<MeshRenderer> rs, Transform frame, Dictionary<Material, Color> colours, string name)
        {
            var v = new List<Vector3>();
            var n = new List<Vector3>();
            var col = new List<Color>();
            var idx = new List<int>();
            Matrix4x4 toFrame = frame.worldToLocalMatrix;
            foreach (MeshRenderer r in rs)
            {
                MeshFilter f = r.GetComponent<MeshFilter>();
                Mesh m = f != null ? f.sharedMesh : null;
                if (m == null) continue;
                Matrix4x4 M = toFrame * r.transform.localToWorldMatrix;
                Matrix4x4 N = M.inverse.transpose;
                Vector3[] mv = m.vertices;
                Vector3[] mn = m.normals;
                if (mv.Length == 0) continue;
                Material[] mats = r.sharedMaterials;
                Vector2[] uv = m.uv;
                bool hasUv = uv != null && uv.Length == mv.Length;
                for (int sub = 0; sub < m.subMeshCount; ++sub)
                {
                    if (m.GetTopology(sub) != MeshTopology.Triangles) continue;
                    Material sm = sub < mats.Length ? mats[sub] : null;
                    Color c = Mean(sm, colours);
                    Tex small = hasUv ? Small(sm) : null;
                    int[] t = m.GetTriangles(sub);
                    // Only the vertices this submesh uses, renumbered.
                    var map = new Dictionary<int, int>();
                    foreach (int i in t)
                    {
                        if (map.TryGetValue(i, out int j)) { idx.Add(j); continue; }
                        j = v.Count;
                        map[i] = j;
                        v.Add(M.MultiplyPoint3x4(mv[i]));
                        n.Add(mn.Length == mv.Length ? N.MultiplyVector(mn[i]).normalized : Vector3.up);
                        col.Add(small != null ? small.At(uv[i]) : c);
                        idx.Add(j);
                    }
                }
            }
            if (idx.Count == 0) return null;
            // The cell's frame is in GW2 units (the map root scales 0.0254): the grid in metres.
            Simplify(v, n, col, idx, Cluster / Mathf.Max(Mathf.Abs(frame.lossyScale.x), 1e-6f));
            if (idx.Count == 0) return null;
            var mesh = new Mesh { name = "Proxy_" + name, indexFormat = IndexFormat.UInt32 };
            mesh.SetVertices(v);
            mesh.SetNormals(n);
            mesh.SetColors(col);
            mesh.SetTriangles(idx, 0);
            mesh.RecalculateBounds();
            MeshUtility.Optimize(mesh);
            return mesh;
        }

        /// Vertex clustering in place: every vertex snaps to the mean of its @p size grid cell; triangles
        /// with two corners in one cell, and repeats, are dropped.
        static void Simplify(List<Vector3> v, List<Vector3> n, List<Color> col, List<int> idx, float size)
        {
            var cellOf = new Dictionary<Vector3Int, int>();
            var remap = new int[v.Count];
            var pos = new List<Vector3>(); var nor = new List<Vector3>(); var c = new List<Color>(); var cnt = new List<int>();
            for (int i = 0; i < v.Count; ++i)
            {
                var k = new Vector3Int(Mathf.FloorToInt(v[i].x / size), Mathf.FloorToInt(v[i].y / size), Mathf.FloorToInt(v[i].z / size));
                if (!cellOf.TryGetValue(k, out int j))
                {
                    j = pos.Count;
                    cellOf[k] = j;
                    pos.Add(Vector3.zero); nor.Add(Vector3.zero); c.Add(Color.clear); cnt.Add(0);
                }
                pos[j] += v[i]; nor[j] += n[i]; c[j] += col[i]; cnt[j]++;
                remap[i] = j;
            }
            var seen = new HashSet<(int, int, int)>();
            var outIdx = new List<int>(idx.Count / 4);
            for (int t = 0; t + 2 < idx.Count; t += 3)
            {
                int a = remap[idx[t]], b = remap[idx[t + 1]], d = remap[idx[t + 2]];
                if (a == b || b == d || a == d) continue;
                // One key per triangle whatever corner it starts at (winding kept).
                (int, int, int) key = a < b && a < d ? (a, b, d) : b < d ? (b, d, a) : (d, a, b);
                if (!seen.Add(key)) continue;
                outIdx.Add(a); outIdx.Add(b); outIdx.Add(d);
            }
            v.Clear(); n.Clear(); col.Clear();
            for (int j = 0; j < pos.Count; ++j)
            {
                v.Add(pos[j] / cnt[j]);
                n.Add(nor[j].sqrMagnitude > 1e-12f ? nor[j].normalized : Vector3.up);
                Color m = c[j] / cnt[j]; m.a = 1f;
                col.Add(m);
            }
            idx.Clear();
            idx.AddRange(outIdx);
        }

        /// A material's _MainTex at TexSize px, read linear (x _Color), sampled per vertex: the proxies keep
        /// their textures' colours (stone, roofs, wood) at no draw cost, not one mean per material.
        const int TexSize = 32;
        static readonly Dictionary<Material, Tex> smalls = new Dictionary<Material, Tex>();

        class Tex
        {
            public Color[] Px;
            public Vector4 St = new Vector4(1, 1, 0, 0);
            public Color At(Vector2 uv)
            {
                float u = uv.x * St.x + St.z, v = uv.y * St.y + St.w;
                int x = (int)((u - Mathf.Floor(u)) * TexSize) % TexSize, y = (int)((v - Mathf.Floor(v)) * TexSize) % TexSize;
                return Px[y * TexSize + x];
            }
        }

        static Tex Small(Material m)
        {
            if (m == null) return null;
            if (smalls.TryGetValue(m, out Tex t)) return t;
            Texture src = m.HasProperty("_MainTex") ? m.GetTexture("_MainTex") : null;
            if (src == null) { smalls[m] = null; return null; }
            var rt = RenderTexture.GetTemporary(TexSize, TexSize, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
            Graphics.Blit(src, rt);
            var prev = RenderTexture.active;
            RenderTexture.active = rt;
            var tex = new Texture2D(TexSize, TexSize, TextureFormat.RGBA32, false, true);
            tex.ReadPixels(new Rect(0, 0, TexSize, TexSize), 0, 0);
            tex.Apply();
            RenderTexture.active = prev;
            RenderTexture.ReleaseTemporary(rt);
            Color tint = m.HasProperty("_Color") ? m.GetColor("_Color").linear : Color.white;
            t = new Tex { Px = tex.GetPixels(), St = m.GetTextureScale("_MainTex") is Vector2 sc && m.GetTextureOffset("_MainTex") is Vector2 of ? new Vector4(sc.x, sc.y, of.x, of.y) : new Vector4(1, 1, 0, 0) };
            for (int i = 0; i < t.Px.Length; ++i) { Color p = t.Px[i] * tint; p.a = 1f; t.Px[i] = p; }
            Object.DestroyImmediate(tex);
            smalls[m] = t;
            return t;
        }

        /// @p m's mean _MainTex colour (linear, the texture's smallest mip by a 4x4 blit) x _Color.
        static Color Mean(Material m, Dictionary<Material, Color> cache)
        {
            if (m == null) return Color.grey;
            if (cache.TryGetValue(m, out Color c)) return c;
            c = Color.white;
            Texture t = m.HasProperty("_MainTex") ? m.GetTexture("_MainTex") : null;
            if (t != null)
            {
                var rt = RenderTexture.GetTemporary(4, 4, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.Linear);
                Graphics.Blit(t, rt);
                var prev = RenderTexture.active;
                RenderTexture.active = rt;
                var tex = new Texture2D(4, 4, TextureFormat.RGBA32, false, true);
                tex.ReadPixels(new Rect(0, 0, 4, 4), 0, 0);
                tex.Apply();
                RenderTexture.active = prev;
                RenderTexture.ReleaseTemporary(rt);
                // Cut-out: the texels it keeps (alpha-weighted); solid: all of them.
                bool cut = m.renderQueue >= (int)RenderQueue.AlphaTest;
                Color sum = Color.clear;
                float w = 0f;
                foreach (Color p in tex.GetPixels()) { float a = cut ? p.a : 1f; sum += p * a; w += a; }
                Object.DestroyImmediate(tex);
                c = w > 1e-4f ? sum / w : Color.grey;
            }
            if (m.HasProperty("_Color")) c *= m.GetColor("_Color").linear;
            c.a = 1f;
            cache[m] = c;
            return c;
        }

        static Material ProxyMaterial(string root)
        {
            string path = root + "/Materials/GW2_Proxy.mat";
            var m = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (m != null) return m;
            m = new Material(Shader.Find("Castlemist/GW2 Proxy")) { enableInstancing = true };
            AssetDatabase.CreateAsset(m, path);
            return m;
        }

        static string Folder(string root, string rel)
        {
            string cur = root;
            foreach (string part in rel.Split('/'))
            {
                if (!AssetDatabase.IsValidFolder(cur + "/" + part)) AssetDatabase.CreateFolder(cur, part);
                cur += "/" + part;
            }
            return cur;
        }
    }
}
