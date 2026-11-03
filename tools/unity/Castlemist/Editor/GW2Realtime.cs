// castlemist world export: GW2's map and model lights as realtime VRC Light Volumes point lights
// (2026-10-26). Castlemist > GW2 Realtime Lights. GW2 draws them per pixel, unshadowed, every frame;
// baked into Light Volumes they smear over the coarse voxels (~9 m in Lion's Arch) and fill the atlas.
// A Point Light Volume (red.sim.lightvolumes 3.x) is the same thing: analytic, unshadowed, clustered
// on screen, and it lights avatars too.
//   each light     a PointLightVolumeInstance on the light's own GameObject: colour and intensity as
//                  the Light's (the package uploads Color.linear x Intensity, as Unity does), the
//                  Light's range, through FalloffPath (a LUT: Unity's built-in point light falloff,
//                  1 / (1 + 25 r^2) faded to 0 over the last 20%, by r = distance / range; the spot
//                  cone across the other axis, the package's LUT layout: u = sqrt(cone), v = r).
//                  Dynamic under an Animator (lights on animated bones). The Light and its
//                  BakeryPointLight are disabled: nothing bakes them twice. ImmersiveLighting's glow
//                  lights (bounce off glowing materials, which GW2 has not) are switched off
//   streaming      the package renders 128 at once, by registry order: GW2Streamer (under the
//                  environment root) keeps per Cell-metre XZ cell the lights reaching the cell, then
//                  the rest by range / distance past their range (within MaxReach), Budget at most
// 2026-10-27 (the user: "lights need to be a tad more intense to match GW2's light scalability"): the LUT
// is GW2's falloff, not Unity's: full strength to the light's near distance, then linear to 0 at its far
// distance (the stored nearDistance / farDistance; the linear ramp is the usual deferred-light reading,
// UNPROVEN). Unity's 1 / (1 + 25 r^2) is at 14 % by half the range; GW2's lights carry. One LUT per
// near / far ratio, rounded to a quarter (environment/GW2_PointFalloff_n<k>.exr); map lights find
// theirs in the manifest by position, other lights take 0 (most of GW2's: 80 of the Labyrinth's 216).
// Revert (Castlemist > GW2 Baked Lights) switches the Lights and Bakery lights back on and removes
// the point lights and the streamer.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class GW2Realtime
    {
        public const string StreamerName = "GW2 Light Streamer";
        public const float Cell = 32f;
        public const int Budget = 64;            // of the package's 128 (2026-10-26: 120 cost per pixel everywhere)
        public const float MaxReach = 60f;       // metres past a light's range it can still be chosen (was 150)
        const int LutSize = 128;

        [MenuItem("Castlemist/GW2 Realtime Lights (Light Volumes point lights)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply()); }

        [MenuItem("Castlemist/GW2 Baked Lights (undo realtime lights)")]
        public static void RevertMenu() { Debug.Log("CASTLEMIST " + Revert()); }

        static Type PointType() { return SceneSetup.FindType("VRCLightVolumes.PointLightVolumeInstance"); }

        /// Every non-directional Light of the open scene as a point light volume, streamed; one-line report.
        public static string Apply()
        {
            Type pt = PointType(), st = SceneSetup.FindType("Castlemist.GW2Streamer");
            if (pt == null) return "realtime lights: no VRC Light Volumes 3.x";
            if (st == null) return "realtime lights: GW2Streamer is not compiled (UdonSharp, CASTLEMIST_VRC)";
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            GameObject envRoot = ImmersiveLighting.EnvRoot();
            if (string.IsNullOrEmpty(root) || envRoot == null) return "realtime lights: the open scene is not a castlemist world";
            Dictionary<Vector3Int, float> nearByPos = MapLightNear(root);
            Type bpl = SceneSetup.FindType("BakeryPointLight");
            MethodInfo sync = SceneSetup.FindType("VRCLightVolumes.PointLightVolumeEditorUtility")?.GetMethod("Sync", BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static);

            var lights = Object.FindObjectsByType<Light>(FindObjectsInactive.Include, FindObjectsSortMode.None)
                .Where(l => l.type == LightType.Point || l.type == LightType.Spot).OrderBy(l => l.name).ToList();
            // ImmersiveLighting's glow lights stand in for bounce off glowing materials, which GW2 does not
            // have: off (and out of the bake), not converted.
            int glows = 0;
            foreach (Light l in lights.Where(Glow).ToList())
            {
                l.enabled = false;
                if (bpl != null && l.GetComponent(bpl) is Behaviour gb) gb.enabled = false;
                lights.Remove(l);
                ++glows;
            }
            var items = new List<GameObject>();
            var pos = new List<Vector2>();
            var range = new List<float>();
            int dynamic = 0;
            foreach (Light l in lights)
            {
                GameObject go = l.gameObject;
                Component p = go.GetComponent(pt);
                if (p == null) p = SceneSetup.AddUdonAware(go, pt);
                float near = nearByPos.TryGetValue(Vector3Int.RoundToInt(go.transform.position * 4f), out float nr) ? nr : 0f;
                Texture2D lut = FalloffLut(root, Mathf.Round(near * 4f) / 4f);
                bool dyn = go.GetComponentInParent<Animator>(true) != null;
                if (dyn) ++dynamic;
                bool spot = l.type == LightType.Spot;
                SceneSetup.SetField(p, "IsDynamic", dyn);
                SceneSetup.SetField(p, "LightType", spot ? 1 : 0);
                SceneSetup.SetField(p, "Color", l.color);
                SceneSetup.SetField(p, "Intensity", l.intensity);
                SceneSetup.SetField(p, "Projection", 1);   // LUT: the Light's own range
                SceneSetup.SetField(p, "FalloffLUT", lut);
                // The package scales Range by the object's mean lossy scale (map lights sit under the
                // map root, 0.0254 per GW2 unit): world metres back to its local units.
                Vector3 ls = go.transform.lossyScale;
                float scale = Mathf.Max((Mathf.Abs(ls.x) + Mathf.Abs(ls.y) + Mathf.Abs(ls.z)) / 3f, 1e-6f);
                SceneSetup.SetField(p, "Range", l.range / scale);
                SceneSetup.SetField(p, "Shadows", false);
                if (spot) SceneSetup.SetField(p, "Angle", Mathf.Clamp(l.spotAngle * 0.5f, 0.05f, 180f) * Mathf.Deg2Rad);
                if (sync != null) sync.Invoke(null, new object[] { p, false, false, false });
                else SceneSetup.CopyProxyToUdon(p);
                l.enabled = false;
                Component b = bpl != null ? go.GetComponent(bpl) : null;
                if (b is Behaviour bb) bb.enabled = false;
                items.Add(go);
                pos.Add(new Vector2(go.transform.position.x, go.transform.position.z));
                range.Add(l.range);
            }
            if (sync == null) Debug.LogWarning("CASTLEMIST realtime lights: no PointLightVolumeEditorUtility.Sync in this package version: select the lights once so the package syncs them");
            foreach (GameObject r in EditorSceneManager.GetActiveScene().GetRootGameObjects()) if (r.name.StartsWith("GW2_")) ImmersiveLighting.Register(r);

            string grid = Streamer(envRoot, st, items, pos, range);
            RefreshManager(true);
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "realtime lights: " + items.Count + " point light volume(s) (" + dynamic + " dynamic), Lights and Bakery lights off, " +
                   glows + " glow light(s) off; " + grid;
        }

        static bool Glow(Light l) { return l.transform.parent != null && l.transform.parent.name == "GW2Lights"; }

        /// The scene's LightVolumeManager re-applied (the package's ApplySettings: buffers, and with
        /// @p textures the custom texture array the falloff LUT goes in), as its inspector does.
        public static string RefreshManager(bool textures)
        {
            Type backend = SceneSetup.FindType("VRCLightVolumes.LightVolumeManagerEditorBackend");
            Type mt = SceneSetup.FindType("VRCLightVolumes.LightVolumeManager");
            MethodInfo apply = backend?.GetMethod("ApplySettings", BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static);
            if (apply == null || mt == null) return "no ApplySettings in this package version";
            int n = 0;
            foreach (Object m in Object.FindObjectsByType(mt, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                apply.Invoke(null, new object[] { m, true, textures, false, true, true });
                // Outside Play Mode ApplySettings can leave the custom texture array unbuilt (every LUT
                // light then samples 0): build it and upload now.
                if (textures) mt.GetMethod("ReinitializeCustomTextures", Type.EmptyTypes)?.Invoke(m, null);
                mt.GetMethod("UpdateVolumes", Type.EmptyTypes)?.Invoke(m, null);
                ++n;
            }
            return n + " manager(s) refreshed";
        }

        /// GW2Streamer for @p items (at XZ @p pos, reaching @p range) under @p envRoot, replacing an earlier one.
        static string Streamer(GameObject envRoot, Type st, List<GameObject> items, List<Vector2> pos, List<float> range)
        {
            Transform old = envRoot.transform.Find(StreamerName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            if (items.Count == 0) return "no streamer (no lights)";
            Vector2 min = pos[0], max = pos[0];
            foreach (Vector2 p in pos) { min = Vector2.Min(min, p); max = Vector2.Max(max, p); }
            min -= Vector2.one * Cell; max += Vector2.one * Cell;
            int nx = Mathf.CeilToInt((max.x - min.x) / Cell), nz = Mathf.CeilToInt((max.y - min.y) / Cell);
            var start = new List<int>(nx * nz + 1);
            var flat = new List<int>();
            float halfDiag = Cell * 0.7072f;
            int most = 0, full = 0;
            var near = new List<KeyValuePair<float, int>>();
            var far = new List<KeyValuePair<float, int>>();
            for (int cz = 0; cz < nz; ++cz)
                for (int cx = 0; cx < nx; ++cx)
                {
                    start.Add(flat.Count);
                    var c = new Vector2(min.x + (cx + 0.5f) * Cell, min.y + (cz + 0.5f) * Cell);
                    near.Clear(); far.Clear();
                    for (int i = 0; i < items.Count; ++i)
                    {
                        float d = Vector2.Distance(c, pos[i]);
                        float reach = d - range[i] - halfDiag;
                        if (reach <= 0f) near.Add(new KeyValuePair<float, int>(d, i));
                        else if (reach < MaxReach) far.Add(new KeyValuePair<float, int>(-range[i] / (reach + 4f), i));
                    }
                    near.Sort((a, b) => a.Key.CompareTo(b.Key));
                    far.Sort((a, b) => a.Key.CompareTo(b.Key));
                    int n = 0;
                    foreach (var kv in near.Concat(far)) { if (n == Budget) break; flat.Add(kv.Value); ++n; }
                    if (near.Count > most) most = near.Count;
                    if (n == Budget) ++full;
                }
            start.Add(flat.Count);
            var go = new GameObject(StreamerName);
            go.transform.SetParent(envRoot.transform, false);
            Component s = SceneSetup.AddUdonAware(go, st);
            SceneSetup.SetField(s, "items", items.ToArray());
            SceneSetup.SetField(s, "origin", min);
            SceneSetup.SetField(s, "cellSize", Cell);
            SceneSetup.SetField(s, "cellsX", nx);
            SceneSetup.SetField(s, "cellsZ", nz);
            SceneSetup.SetField(s, "cellStart", start.ToArray());
            SceneSetup.SetField(s, "cellItems", flat.ToArray());
            SceneSetup.CopyProxyToUdon(s);
            return "streamer " + nx + "x" + nz + " cells of " + Cell + " m, at most " + Budget + " lights on (" + most +
                   " reach the busiest cell; " + full + " cell(s) at the budget)";
        }

        public const int CoarseTiles = 12;           // + ImmersiveLighting's 20 detail volumes = the package's 32
        public const long CoarseVoxels = 1200000;

        [MenuItem("Castlemist/Merge Coarse Light Volumes (32-volume limit)")]
        public static void MergeMenu() { Debug.Log("CASTLEMIST " + MergeCoarse(CoarseTiles, CoarseVoxels) + ": re-bake the Light Volumes"); }

        /// The per-cell (coarse) Light Volumes replaced by at most @p tiles volumes on an XZ grid over
        /// them, each the cells' height range inside its tile, @p voxels in all at one density: the
        /// package draws 32 volumes, chosen by weight, never by distance (96 cells + 20 detail volumes
        /// in Lion's Arch: 84 cells were never drawn). Detail volumes stay. Bake afterwards.
        public static string MergeCoarse(int tiles, long voxels)
        {
            Type lv = SceneSetup.FindType("VRCLightVolumes.LightVolumeInstance");
            if (lv == null) return "merge: no Light Volumes package";
            var cells = Object.FindObjectsByType(lv, FindObjectsInactive.Include, FindObjectsSortMode.None).Cast<Component>()
                .Where(v => v.transform.parent == null || v.transform.parent.name != "Detail").ToList();
            if (cells.Count <= tiles) return "merge: " + cells.Count + " coarse volume(s) already";
            Transform parent = cells[0].transform.parent;
            var boxes = cells.Select(v => new Bounds(v.transform.position, Abs(v.transform.lossyScale))).ToList();
            Bounds all = boxes[0];
            foreach (Bounds b in boxes) all.Encapsulate(b);
            // The grid: nx x nz <= tiles, tile shape closest to square.
            int nx = 1, nz = tiles;
            float best = float.MaxValue;
            for (int a = 1; a <= tiles; ++a)
            {
                int c = tiles / a;
                float sx = all.size.x / a, sz = all.size.z / c, ratio = Mathf.Max(sx, sz) / Mathf.Max(Mathf.Min(sx, sz), 1e-3f);
                if (ratio < best) { best = ratio; nx = a; nz = c; }
            }
            var made = new List<Bounds>();
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x)
                {
                    float x0 = all.min.x + all.size.x * x / nx, x1 = all.min.x + all.size.x * (x + 1) / nx;
                    float z0 = all.min.z + all.size.z * z / nz, z1 = all.min.z + all.size.z * (z + 1) / nz;
                    float y0 = float.MaxValue, y1 = float.MinValue;
                    foreach (Bounds b in boxes)
                        if (b.max.x > x0 && b.min.x < x1 && b.max.z > z0 && b.min.z < z1) { y0 = Mathf.Min(y0, b.min.y); y1 = Mathf.Max(y1, b.max.y); }
                    if (y0 > y1) continue;   // no cell here
                    var t = new Bounds();
                    t.SetMinMax(new Vector3(x0, y0, z0), new Vector3(x1, y1, z1));
                    made.Add(t);
                }
            double total = made.Sum(b => (double)b.size.x * b.size.y * b.size.z);
            float vpu = (float)Math.Pow(voxels / Math.Max(total, 1.0), 1.0 / 3.0);
            foreach (Component v in cells) Undo.DestroyObjectImmediate(v.gameObject);
            var group = new GameObject("Coarse");
            Undo.RegisterCreatedObjectUndo(group, "Merge coarse Light Volumes");
            group.transform.SetParent(parent, false);
            long count = 0;
            for (int k = 0; k < made.Count; ++k)
            {
                Bounds b = made[k];
                var go = new GameObject("LightVolume_coarse_" + k);
                go.transform.SetParent(group.transform, false);
                go.transform.position = b.center;
                go.transform.localScale = b.size;
                Component comp = SceneSetup.AddUdonAware(go, lv);
                SceneSetup.SetField(comp, "AdaptiveResolution", true);
                SceneSetup.SetField(comp, "VoxelsPerUnit", vpu);
                SceneSetup.SetField(comp, "Bake", true);
                SceneSetup.SetField(comp, "IsDynamic", false);
                SceneSetup.SetField(comp, "SmoothBlending", 0.25f);
                MethodInfo rec = SceneSetup.FindType("VRCLightVolumes.LightVolumeTools")?.GetMethod("Recalculate", BindingFlags.Public | BindingFlags.Static, null, new[] { lv }, null);
                try { rec?.Invoke(null, new object[] { comp }); }
                catch (Exception e) { Debug.LogWarning("CASTLEMIST Light Volumes Recalculate: " + (e.InnerException ?? e).Message); }
                SceneSetup.CopyProxyToUdon(comp);
                count += (long)(Math.Ceiling(b.size.x * vpu) * Math.Ceiling(b.size.y * vpu) * Math.Ceiling(b.size.z * vpu));
            }
            ImmersiveLighting.Register(group);
            ImmersiveLighting.SortVolumes();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "merge: " + cells.Count + " coarse volume(s) -> " + made.Count + " (" + nx + "x" + nz + " grid, " + (1f / vpu).ToString("F1") +
                   " m voxels, ~" + count + " voxels)";
        }

        static Vector3 Abs(Vector3 v) { return new Vector3(Mathf.Abs(v.x), Mathf.Abs(v.y), Mathf.Abs(v.z)); }

        /// _AmbientFloor on every material of the open scene's export folder that has it.
        public static string SetAmbientFloor(float floor)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            int n = 0;
            foreach (string g in AssetDatabase.FindAssets("t:Material", new[] { root }))
            {
                var m = AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(g));
                if (m == null || !m.HasProperty("_AmbientFloor")) continue;
                m.SetFloat("_AmbientFloor", floor);
                EditorUtility.SetDirty(m);
                ++n;
            }
            AssetDatabase.SaveAssets();
            return "ambient floor " + floor + " on " + n + " material(s)";
        }

        /// The point lights and the streamer off again, the Lights and Bakery lights back on.
        public static string Revert()
        {
            Type pt = PointType(), bpl = SceneSetup.FindType("BakeryPointLight");
            int n = 0;
            if (pt != null)
                foreach (Component p in Object.FindObjectsByType(pt, FindObjectsInactive.Include, FindObjectsSortMode.None).Cast<Component>())
                {
                    GameObject go = p.gameObject;
                    Light l = go.GetComponent<Light>();
                    if (l == null) continue;   // not one this made
                    go.SetActive(true);
                    l.enabled = true;
                    if (bpl != null && go.GetComponent(bpl) is Behaviour b) b.enabled = true;
                    Type ub = SceneSetup.FindType("VRC.Udon.UdonBehaviour");
                    Component backing = ub != null ? go.GetComponent(ub) : null;
                    Object.DestroyImmediate(p);
                    if (backing != null) Object.DestroyImmediate(backing);
                    ++n;
                }
            foreach (Light l in Object.FindObjectsByType<Light>(FindObjectsInactive.Include, FindObjectsSortMode.None).Where(Glow))
            {
                l.enabled = true;
                if (bpl != null && l.GetComponent(bpl) is Behaviour gb) gb.enabled = true;
            }
            GameObject envRoot = ImmersiveLighting.EnvRoot();
            Transform s = envRoot != null ? envRoot.transform.Find(StreamerName) : null;
            if (s != null) Object.DestroyImmediate(s.gameObject);
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "baked lights: " + n + " light(s) back to Light + Bakery, streamer removed: re-bake";
        }

        /// The falloff LUT at <root>/environment/GW2_PointFalloff.exr (made once): rows v = distance /
        /// range, Unity's built-in point light attenuation; columns u = sqrt(spot cone), 1 at the axis
        /// (u = 0, what point lights read) to 0 at the cone's edge.
        /// The map lights' near / far ratio by world position (x 4, rounded), from the export's manifest.
        static Dictionary<Vector3Int, float> MapLightNear(string root)
        {
            var d = new Dictionary<Vector3Int, float>();
            string man = root + "/manifest.json";
            if (!File.Exists(man)) return d;
            WorldManifest m = WorldManifest.Parse(File.ReadAllText(man));
            GameObject mapRoot = EditorSceneManager.GetActiveScene().GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + m.MapFileId);
            if (mapRoot == null) return d;
            foreach (WorldManifest.MapLight l in m.MapLights)
            {
                if (l.Unity == null || !(l.Far > 0)) continue;
                Vector3 w = mapRoot.transform.TransformPoint(new Vector3((float)l.Unity[0], (float)l.Unity[1], (float)l.Unity[2]));
                d[Vector3Int.RoundToInt(w * 4f)] = Mathf.Clamp01((float)(l.Near / l.Far));
            }
            return d;
        }

        /// GW2's falloff for near / far ratio @p near (see the header) by r = distance / range, x the spot
        /// cone across the other axis (the package's layout).
        static Texture2D FalloffLut(string root, float near)
        {
            int k = Mathf.RoundToInt(near * 100f);
            string path = root + "/environment/GW2_PointFalloff_n" + k + ".exr";
            var have = AssetDatabase.LoadAssetAtPath<Texture2D>(path);
            if (have != null) return have;
            var t = new Texture2D(LutSize, LutSize, TextureFormat.RGBAHalf, false, true);
            var px = new Color[LutSize * LutSize];
            for (int y = 0; y < LutSize; ++y)
            {
                float r = y / (LutSize - 1f);
                float a = r <= near ? 1f : Mathf.Clamp01((1f - r) / Mathf.Max(1f - near, 1e-3f));
                for (int x = 0; x < LutSize; ++x)
                {
                    float u = x / (LutSize - 1f), cone = u * u;      // the shader samples sqrt(cone)
                    float s = 1f - Mathf.SmoothStep(0f, 1f, cone);
                    float v = a * s;
                    px[y * LutSize + x] = new Color(v, v, v, 1f);
                }
            }
            t.SetPixels(px);
            t.Apply();
            File.WriteAllBytes(path, t.EncodeToEXR(Texture2D.EXRFlags.None));
            Object.DestroyImmediate(t);
            AssetDatabase.ImportAsset(path);
            if (AssetImporter.GetAtPath(path) is TextureImporter ti)
            {
                ti.textureType = TextureImporterType.Default;
                ti.sRGBTexture = false;
                ti.mipmapEnabled = false;
                ti.wrapMode = TextureWrapMode.Clamp;
                ti.filterMode = FilterMode.Bilinear;
                ti.textureCompression = TextureImporterCompression.Uncompressed;
                ti.npotScale = TextureImporterNPOTScale.None;
                ti.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
        }
    }
}
