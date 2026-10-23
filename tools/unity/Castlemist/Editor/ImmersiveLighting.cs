// Castlemist > Setup Immersive Lighting (2026-10-22): turns an imported GW2 map scene into one that
// bakes well with VRC Light Volumes, with Unity's lightmapper or with Bakery. On the open scene:
//
//   GI materials     every castlemist material gets Double Sided GI: GW2 meshes are mostly one-sided,
//                    and the bakers read a back face as black (the black voxels under the vault's
//                    platforms, 59-66 % of three volumes, 2026-10-22).
//   helpers          renderers without a material (Unity's Default-Material: GW2 editor helpers such
//                    as 3124756's "mounta"/"mountb"/"scaleref", exported before the 2026-10-22 fix)
//                    are switched off.
//   coarse volumes   each per-cell Light Volume's resolution goes to CoarseBudget voxels (~58^3).
//   detail volumes   fine Light Volumes (DetailVoxelsPerUnit) over where players walk: floors found by
//                    downward raycasts through the colliders, binned on a DetailCell grid, merged into boxes from just below
//                    the floor to DetailHeight above it; weight 1, so they win over the coarse ones.
//                    At most MaxDetailVolumes, the largest first.
//   probes / areas   a LightProbeGroup over every walkable floor (ProbeSpacing apart, ProbeHeights
//                    above it, none inside geometry): avatars and shaders without Light Volumes; and
//                    one OcclusionArea per walkable region (where the camera can be) in place of the
//                    import's one map-wide area: tighter culling, a faster occlusion bake.
//   glow lights      a baked point light at each glowing source -- the orbs (castlemist interior
//                    block) and the additive unlit glows of a sensible size -- coloured by the
//                    average of its glow texture x its colour, merged when close. Each gets a Unity
//                    Light (Point, Baked) and, with Bakery, a BakeryPointLight (direct + indirect).
//   sun / sky        with Bakery: a BakeryDirectLight on the Sun (indirect only: the Sun stays a
//                    realtime light for avatars, as its Unity Mixed / Indirect Only mode), a
//                    BakerySkyLight from the GW2 ambient, and Bakery's render mode set to Indirect.
//
// Rerunning replaces what it made (GW2Lights, LightVolumes/Detail, the Bakery sky). Everything is a
// reading of GW2 data for looks, not a measurement: intensities and ranges are castlemist's choices.

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class ImmersiveLighting
    {
        public const int CoarseBudget = 196608;
        public const float DetailVoxelsPerUnit = 0.5f;
        public const float DetailCell = 24f;
        public const float DetailHeight = 10f;
        public const int MaxDetailVolumes = 20;
        public const int MaxLights = 64;

        [MenuItem("Castlemist/Setup Immersive Lighting")]
        public static void Menu()
        {
            Debug.Log("CASTLEMIST " + Apply());
        }

        public static string Apply()
        {
            var lines = new List<string>();
            GameObject envRoot = EnvRoot();
            if (envRoot == null) return "immersive lighting: no GW2_<id>_Environment root in the open scene (import a map first)";
            lines.Add(DoubleSidedGI());
            lines.Add(HideHelpers());
            lines.Add(SkinnedProbesOnly());
            lines.Add(CoarseVolumes());
            lines.Add(DetailVolumes(envRoot));
            lines.Add(ProbesAndOcclusion(envRoot));
            lines.Add(GlowLights(envRoot));
            lines.Add(BakerySunSky(envRoot));
            EditorSceneManager.MarkSceneDirty(envRoot.scene);
            return "immersive lighting: " + string.Join("; ", lines) + ". Bake with Castlemist > Bake Lighting and Occlusion (Unity / Bakery)";
        }

        static GameObject EnvRoot()
        {
            return UnityEngine.SceneManagement.SceneManager.GetActiveScene().GetRootGameObjects()
                .FirstOrDefault(g => g.name.StartsWith("GW2_") && g.name.EndsWith("_Environment"));
        }

        static bool IsCastlemist(Material m)
        {
            return m != null && m.shader != null && m.shader.name.StartsWith("Castlemist/");
        }

        // ---- materials and helpers ------------------------------------------------------------------------

        static string DoubleSidedGI()
        {
            var mats = new HashSet<Material>();
            foreach (Renderer r in Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None))
                foreach (Material m in r.sharedMaterials)
                    if (IsCastlemist(m)) mats.Add(m);
            int n = 0;
            foreach (Material m in mats)
            {
                if (m.doubleSidedGI) continue;
                Undo.RecordObject(m, "Double Sided GI");
                m.doubleSidedGI = true;
                EditorUtility.SetDirty(m);
                ++n;
            }
            return "double-sided GI on " + n + " of " + mats.Count + " materials";
        }

        static string HideHelpers()
        {
            int n = 0;
            foreach (MeshRenderer r in Object.FindObjectsByType<MeshRenderer>(FindObjectsSortMode.None))
            {
                Material m = r.sharedMaterial;
                if (m == null || m.name != "Default-Material" || !r.enabled) continue;
                if (r.GetComponentsInParent<Transform>(true).All(t => !t.name.StartsWith("GW2_"))) continue;
                Undo.RecordObject(r, "Hide GW2 helper");
                r.enabled = false;
                ++n;
            }
            return n + " material-less helper mesh(es) hidden";
        }

        /// Skinned meshes off the bake: no Contribute GI, probes / Light Volumes only, no lightmap
        /// (Bakery had lightmapped 3560849_179's discs through GW2's UV1: dark and scrambled).
        static string SkinnedProbesOnly()
        {
            int n = 0;
            foreach (SkinnedMeshRenderer r in Object.FindObjectsByType<SkinnedMeshRenderer>(FindObjectsSortMode.None))
            {
                if (r.GetComponentsInParent<Transform>(true).All(t => !t.name.StartsWith("GW2_"))) continue;
                StaticEditorFlags f = GameObjectUtility.GetStaticEditorFlags(r.gameObject);
                bool changed = false;
                if ((f & StaticEditorFlags.ContributeGI) != 0)
                {
                    Undo.RecordObject(r.gameObject, "Skinned off the bake");
                    GameObjectUtility.SetStaticEditorFlags(r.gameObject, f & ~StaticEditorFlags.ContributeGI);
                    changed = true;
                }
                Undo.RecordObject(r, "Skinned off the bake");
                if (SceneSetup.ProbesOnly(r)) changed = true;
                if (changed) ++n;
            }
            return n + " skinned renderer(s) off the bake (probes / Light Volumes only)";
        }

        // ---- Light Volumes ----------------------------------------------------------------------------------

        static Type VolumeType() { return SceneSetup.FindType("VRCLightVolumes.LightVolumeInstance"); }

        static string CoarseVolumes()
        {
            Type lv = VolumeType();
            if (lv == null) return "no Light Volumes package";
            int n = 0;
            foreach (Component v in Object.FindObjectsByType(lv, FindObjectsInactive.Include, FindObjectsSortMode.None).Cast<Component>())
            {
                if (v.transform.parent != null && v.transform.parent.name == "Detail") continue;
                Vector3 s = v.transform.lossyScale;
                double vpu = EnvironmentMath.VoxelsPerUnit(Mathf.Abs(s.x), Mathf.Abs(s.y), Mathf.Abs(s.z), CoarseBudget, 1.0);
                Undo.RecordObject(v, "Light Volume resolution");
                SceneSetup.SetField(v, "VoxelsPerUnit", (float)vpu);
                Recalculate(v);
                SceneSetup.CopyProxyToUdon(v);
                ++n;
            }
            return n + " coarse volume(s) at ~" + CoarseBudget + " voxels";
        }

        static void Recalculate(Component v)
        {
            Type tools = SceneSetup.FindType("VRCLightVolumes.LightVolumeTools");
            MethodInfo rec = tools != null ? tools.GetMethod("Recalculate", BindingFlags.Public | BindingFlags.Static, null, new[] { v.GetType() }, null) : null;
            try { if (rec != null) rec.Invoke(null, new object[] { v }); }
            catch (Exception e) { Debug.LogWarning("CASTLEMIST Light Volumes Recalculate: " + (e.InnerException ?? e).Message); }
        }

        /// Walkable boxes: floors found by casting rays straight down through the colliders on a grid
        /// (cell / 4 apart; works on unreadable collision meshes, which the import leaves), binned on
        /// an XZ grid of @p cell; adjacent bins in a row with overlapping floor heights merge; each box
        /// spans the floor to @p height above it.
        public static List<Bounds> WalkableBoxes(IEnumerable<Collider> colliders, float cell, float height)
        {
            var set = new HashSet<Collider>(colliders);
            var bins = new Dictionary<Vector3Int, Vector2>();   // (x, storey, z) bin -> (min floor y, max floor y)
            bool backfaces = Physics.queriesHitBackfaces;
            Physics.queriesHitBackfaces = true;   // either winding counts (castlemist reversed colliders 2026-10-22)
            Physics.SyncTransforms();
            float step = cell / 4f;
            try
            {
                foreach (Collider col in set)
                {
                    Bounds cb = col.bounds;
                    for (float x = cb.min.x + step * 0.5f; x < cb.max.x; x += step)
                        for (float z = cb.min.z + step * 0.5f; z < cb.max.z; z += step)
                        {
                            var ray = new Ray(new Vector3(x, cb.max.y + 1f, z), Vector3.down);
                            foreach (RaycastHit h in Physics.RaycastAll(ray, cb.size.y + 2f, ~0, QueryTriggerInteraction.Ignore))
                            {
                                if (h.collider != col || Mathf.Abs(h.normal.y) < 0.7f) continue;
                                // Each storey of a column of floors is its own bin (a bridge over a hall).
                                var key = new Vector3Int(Mathf.FloorToInt(h.point.x / cell), Mathf.FloorToInt(h.point.y / (height * 2f)),
                                                         Mathf.FloorToInt(h.point.z / cell));
                                Vector2 r;
                                bins[key] = bins.TryGetValue(key, out r) ? new Vector2(Mathf.Min(r.x, h.point.y), Mathf.Max(r.y, h.point.y))
                                                                        : new Vector2(h.point.y, h.point.y);
                            }
                        }
                }
            }
            finally { Physics.queriesHitBackfaces = backfaces; }
            // Group the bins into height bands (one storey each), take each band's connected areas
            // (4-neighbour), and box them: just above the floor (voxels inside the slab bake black and
            // darken what stands on it, 2026-10-22) to the highest floor + height, split into tiles
            // of at most MaxBox. Sorted by walkable area, largest first.
            var boxes = new List<KeyValuePair<int, Bounds>>();
            foreach (var band in bins.GroupBy(k => k.Key.y))
            {
                var left = new Dictionary<Vector2Int, Vector2>();
                foreach (var kv in band) left[new Vector2Int(kv.Key.x, kv.Key.z)] = kv.Value;
                while (left.Count > 0)
                {
                    var seed = left.Keys.First();
                    var comp = new List<KeyValuePair<Vector2Int, Vector2>>();
                    var stack = new Stack<Vector2Int>();
                    stack.Push(seed);
                    while (stack.Count > 0)
                    {
                        Vector2Int k = stack.Pop();
                        Vector2 v;
                        if (!left.TryGetValue(k, out v)) continue;
                        left.Remove(k);
                        comp.Add(new KeyValuePair<Vector2Int, Vector2>(k, v));
                        stack.Push(k + Vector2Int.right); stack.Push(k + Vector2Int.left);
                        stack.Push(k + Vector2Int.up); stack.Push(k + Vector2Int.down);
                    }
                    // Tiles of at most MaxBox over the component's bins.
                    int tile = Mathf.Max(1, Mathf.FloorToInt(MaxBox / cell));
                    foreach (var t in comp.GroupBy(c => new Vector2Int(FloorDiv(c.Key.x, tile), FloorDiv(c.Key.y, tile))))
                    {
                        int x0 = t.Min(c => c.Key.x), x1 = t.Max(c => c.Key.x), z0 = t.Min(c => c.Key.y), z1 = t.Max(c => c.Key.y);
                        float y0 = t.Min(c => c.Value.x), y1 = t.Max(c => c.Value.y);
                        var bx = new Bounds();
                        bx.SetMinMax(new Vector3(x0 * cell, y0 + 0.25f, z0 * cell), new Vector3((x1 + 1) * cell, y1 + height, (z1 + 1) * cell));
                        boxes.Add(new KeyValuePair<int, Bounds>(t.Count(), bx));
                    }
                }
            }
            return boxes.OrderByDescending(b => b.Key).Select(b => b.Value).ToList();
        }

        public const float MaxBox = 128f;

        static int FloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

        static string DetailVolumes(GameObject envRoot)
        {
            Type lv = VolumeType();
            if (lv == null) return "no detail volumes (no Light Volumes package)";
            Transform parent = envRoot.transform.Find("LightVolumes");
            if (parent == null) { var p = new GameObject("LightVolumes"); p.transform.SetParent(envRoot.transform, false); parent = p.transform; }
            Transform old = parent.Find("Detail");
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var group = new GameObject("Detail");
            Undo.RegisterCreatedObjectUndo(group, "Detail Light Volumes");
            group.transform.SetParent(parent, false);

            var colliders = Object.FindObjectsByType<Collider>(FindObjectsSortMode.None)
                .Where(c => c.enabled && !c.isTrigger && !PlaceholderTerrain(c) && c.GetComponentsInParent<Transform>(true).Any(t => t.name.StartsWith("GW2_")));
            // Prop and building colliders first; terrain (a cell's "Terrain") only fills what is left:
            // the Mists vault's terrain is a flat placeholder under everything, open-world terrain is ground.
            var all = colliders.ToList();
            List<Bounds> boxes = WalkableBoxes(all.Where(c => c.name != "Terrain"), DetailCell, DetailHeight)
                .Concat(WalkableBoxes(all.Where(c => c.name == "Terrain"), DetailCell, DetailHeight))
                .Take(MaxDetailVolumes).ToList();
            long voxels = 0;
            int k = 0;
            foreach (Bounds b in boxes)
            {
                var go = new GameObject("LightVolume_detail_" + (k++));
                go.transform.SetParent(group.transform, false);
                go.transform.position = b.center;
                go.transform.localScale = b.size;
                Component comp = SceneSetup.AddUdonAware(go, lv);
                SceneSetup.SetField(comp, "AdaptiveResolution", true);
                SceneSetup.SetField(comp, "VoxelsPerUnit", DetailVoxelsPerUnit);
                SceneSetup.SetField(comp, "Bake", true);
                SceneSetup.SetField(comp, "IsDynamic", false);
                SceneSetup.SetField(comp, "SmoothBlending", 0.5f);
                MethodInfo w = comp.GetType().GetMethod("SetWeight", new[] { typeof(float) });
                if (w != null) w.Invoke(comp, new object[] { 1f });
                else SceneSetup.SetField(comp, "RegistryWeight", 1f);
                Recalculate(comp);
                SceneSetup.CopyProxyToUdon(comp);
                voxels += (long)(Math.Ceiling(b.size.x * DetailVoxelsPerUnit) * Math.Ceiling(b.size.y * DetailVoxelsPerUnit) * Math.Ceiling(b.size.z * DetailVoxelsPerUnit));
            }
            Register(group);
            SortVolumes();
            return boxes.Count + " detail volume(s) over walkable floors (~" + voxels + " voxels)";
        }

        /// The manager's volumes ordered by weight (then resolution), the package's own explicit sort.
        /// The shader takes the first volume that holds a point, and the package never reorders on its
        /// own: without this the coarse cell volumes, registered first, win everywhere and the detail
        /// volumes only blend at their edges (the Wizard Vault's hub disc lit from ~9 m voxels,
        /// 2026-10-23).
        public static bool SortVolumes()
        {
            try
            {
                Type backend = SceneSetup.FindType("VRCLightVolumes.LightVolumeManagerEditorBackend");
                Type mgrType = SceneSetup.FindType("VRCLightVolumes.LightVolumeManager");
                MethodInfo sort = backend != null ? backend.GetMethod("SortLightVolumesByVoxelsPerUnit", BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static) : null;
                if (sort == null || mgrType == null) { Debug.LogWarning("CASTLEMIST Light Volumes: no SortLightVolumesByVoxelsPerUnit in this package version"); return false; }
                foreach (Object m in Object.FindObjectsByType(mgrType, FindObjectsInactive.Include, FindObjectsSortMode.None)) sort.Invoke(null, new object[] { m });
                return true;
            }
            catch (Exception e) { Debug.LogWarning("CASTLEMIST Light Volumes sort: " + (e.InnerException ?? e).Message); return false; }
        }

        /// One Light Volume over @p area (a world box) at @p voxelsPerUnit and @p weight, under
        /// LightVolumes/Detail, registered and sorted in: for a surface no detail volume boundary may
        /// cross (a box edge through a floor shows as a straight light seam). Replaces one of the same
        /// @p name. Bake afterwards.
        public static string AddFocusVolume(string name, Bounds area, float voxelsPerUnit, float weight)
        {
            Type lv = VolumeType();
            if (lv == null) return "no Light Volumes package";
            GameObject envRoot = UnityEngine.SceneManagement.SceneManager.GetActiveScene().GetRootGameObjects()
                                     .FirstOrDefault(g => g.name.EndsWith("_Environment"));
            if (envRoot == null) return "no *_Environment root";
            Transform parent = envRoot.transform.Find("LightVolumes");
            Transform detail = parent != null ? parent.Find("Detail") : null;
            if (detail == null) return "no LightVolumes/Detail group (run Setup Immersive Lighting first)";
            Transform old = detail.Find(name);
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var go = new GameObject(name);
            Undo.RegisterCreatedObjectUndo(go, "Focus Light Volume");
            go.transform.SetParent(detail, false);
            go.transform.position = area.center;
            go.transform.localScale = area.size;
            Component comp = SceneSetup.AddUdonAware(go, lv);
            SceneSetup.SetField(comp, "AdaptiveResolution", true);
            SceneSetup.SetField(comp, "VoxelsPerUnit", voxelsPerUnit);
            SceneSetup.SetField(comp, "Bake", true);
            SceneSetup.SetField(comp, "IsDynamic", false);
            SceneSetup.SetField(comp, "SmoothBlending", 0.5f);
            MethodInfo w = comp.GetType().GetMethod("SetWeight", new[] { typeof(float) });
            if (w != null) w.Invoke(comp, new object[] { weight });
            else SceneSetup.SetField(comp, "RegistryWeight", weight);
            Recalculate(comp);
            SceneSetup.CopyProxyToUdon(comp);
            Register(go);
            bool sorted = SortVolumes();
            return name + ": " + area.size.ToString("0") + " at " + voxelsPerUnit + " voxels/unit, weight " + weight +
                   (sorted ? ", sorted" : ", NOT sorted") + " (bake to fill it)";
        }

        /// The package's onboarding: registers new volumes with the scene's LightVolumeManager.
        static void Register(GameObject group)
        {
            try
            {
                Type setup = SceneSetup.FindType("VRCLightVolumes.LightVolumeSceneSetup");
                MethodInfo onboard = setup != null ? setup.GetMethods(BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static)
                    .FirstOrDefault(x => x.Name == "OnboardHierarchy" && x.GetParameters().Length == 3) : null;
                if (onboard != null) onboard.Invoke(null, new object[] { group, null, false });
            }
            catch (Exception e) { Debug.LogWarning("CASTLEMIST Light Volumes onboarding: " + (e.InnerException ?? e).Message); }
        }

        /// A cell's terrain collider that is perfectly flat: a placeholder under the map (the Mists
        /// vault's, at y 3 under everything), not ground anyone walks on. Real terrain has relief.
        static bool PlaceholderTerrain(Collider c)
        {
            return c.name == "Terrain" && c.bounds.size.y < 0.5f;
        }

        public const float ProbeSpacing = 8f;
        static readonly float[] ProbeHeights = { 1.5f, 6f };
        public const float OcclusionHeight = 14f;

        static string ProbesAndOcclusion(GameObject envRoot)
        {
            var colliders = Object.FindObjectsByType<Collider>(FindObjectsSortMode.None)
                .Where(c => c.enabled && !c.isTrigger && !PlaceholderTerrain(c) && c.GetComponentsInParent<Transform>(true).Any(t => t.name.StartsWith("GW2_"))).ToList();
            List<Bounds> boxes = WalkableBoxes(colliders, DetailCell, DetailHeight);

            // Light probes: a grid over each walkable box at the probe heights, skipping points in geometry.
            Transform oldP = envRoot.transform.Find("LightProbes");
            if (oldP != null) Undo.DestroyObjectImmediate(oldP.gameObject);
            var pgo = new GameObject("LightProbes");
            Undo.RegisterCreatedObjectUndo(pgo, "GW2 light probes");
            pgo.transform.SetParent(envRoot.transform, false);
            var positions = new List<Vector3>();
            var seen = new HashSet<Vector3Int>();
            foreach (Bounds b in boxes)
                for (float x = b.min.x + ProbeSpacing * 0.5f; x < b.max.x; x += ProbeSpacing)
                    for (float z = b.min.z + ProbeSpacing * 0.5f; z < b.max.z; z += ProbeSpacing)
                        foreach (float h in ProbeHeights)
                        {
                            var p = new Vector3(x, b.min.y + h, z);
                            if (p.y > b.max.y + ProbeHeights[ProbeHeights.Length - 1]) continue;
                            var key = Vector3Int.RoundToInt(p / 2f);
                            if (!seen.Add(key)) continue;
                            if (Physics.CheckSphere(p, 0.3f, ~0, QueryTriggerInteraction.Ignore)) continue;
                            positions.Add(p);
                        }
            var group = pgo.AddComponent<LightProbeGroup>();
            group.probePositions = positions.Select(p => pgo.transform.InverseTransformPoint(p)).ToArray();

            // Occlusion areas: one per walkable box, as tall as a player can look from it.
            Transform oldA = envRoot.transform.Find("OcclusionAreas");
            if (oldA != null) Undo.DestroyObjectImmediate(oldA.gameObject);
            Transform mapWide = envRoot.transform.Find("OcclusionArea");
            if (mapWide != null) Undo.DestroyObjectImmediate(mapWide.gameObject);
            var agroup = new GameObject("OcclusionAreas");
            Undo.RegisterCreatedObjectUndo(agroup, "GW2 occlusion areas");
            agroup.transform.SetParent(envRoot.transform, false);
            int k = 0;
            foreach (Bounds b in boxes)
            {
                var go = new GameObject("OcclusionArea_" + (k++));
                go.transform.SetParent(agroup.transform, false);
                go.transform.position = new Vector3(b.center.x, b.min.y + OcclusionHeight * 0.5f, b.center.z);
                OcclusionArea a = go.AddComponent<OcclusionArea>();
                a.center = Vector3.zero;
                a.size = new Vector3(b.size.x, Mathf.Max(b.size.y, OcclusionHeight), b.size.z);
            }
            return positions.Count + " light probe(s) and " + boxes.Count + " occlusion area(s) over walkable floors";
        }

        // ---- glow lights ------------------------------------------------------------------------------------

        struct Glow { public Vector3 Pos; public float Size; public Color Col; public float Weight; }

        /// The light a glowing renderer gives off, or null when it is not a light source.
        static Glow? GlowOf(Renderer r)
        {
            Material m = r.sharedMaterial;
            if (!IsCastlemist(m) || m.shader.name != "Castlemist/GW2" || !r.enabled || !r.gameObject.activeInHierarchy) return null;
            bool orb = m.HasProperty("_UseInterior") && m.GetFloat("_UseInterior") > 0.5f;
            bool additive = m.HasProperty("_Unlit") && m.GetFloat("_Unlit") > 0.5f && m.HasProperty("_DstBlend") &&
                            (m.GetFloat("_DstBlend") == (float)BlendMode.One || m.GetFloat("_DstBlend") == (float)BlendMode.OneMinusSrcColor);
            if (!orb && !additive) return null;
            float size = r.bounds.size.magnitude;
            if (size < 0.5f || size > 120f) return null;   // specks, and the sky / void backdrops
            Texture tex = orb && m.HasProperty("_InteriorTex") && m.GetTexture("_InteriorTex") != null ? m.GetTexture("_InteriorTex") : m.mainTexture;
            Color c = AverageColour(tex);
            if (m.HasProperty("_Color")) c *= m.GetColor("_Color");
            float peak = Mathf.Max(c.r, Mathf.Max(c.g, c.b));
            if (peak < 0.05f) return null;
            return new Glow { Pos = r.bounds.center, Size = size, Col = new Color(c.r / peak, c.g / peak, c.b / peak), Weight = peak };
        }

        static readonly Dictionary<Texture, Color> s_avg = new Dictionary<Texture, Color>();

        /// The mean colour of a texture (a 1 x 1 mip via the GPU; textures need not be readable).
        static Color AverageColour(Texture t)
        {
            if (t == null) return Color.white;
            Color c;
            if (s_avg.TryGetValue(t, out c)) return c;
            var rt = RenderTexture.GetTemporary(16, 16, 0, RenderTextureFormat.ARGB32, RenderTextureReadWrite.sRGB);
            Graphics.Blit(t, rt);
            var prev = RenderTexture.active;
            RenderTexture.active = rt;
            var px = new Texture2D(16, 16, TextureFormat.RGBA32, false);
            px.ReadPixels(new Rect(0, 0, 16, 16), 0, 0);
            px.Apply();
            RenderTexture.active = prev;
            RenderTexture.ReleaseTemporary(rt);
            Color sum = Color.clear;
            foreach (Color p in px.GetPixels()) sum += p * p.a;
            Object.DestroyImmediate(px);
            c = sum / 256f;
            c.a = 1;
            s_avg[t] = c;
            return c;
        }

        static string GlowLights(GameObject envRoot)
        {
            Transform old = envRoot.transform.Find("GW2Lights");
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var glows = Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None)
                .Where(r => r.GetComponentsInParent<Transform>(true).Any(t => t.name.StartsWith("GW2_") && !t.name.EndsWith("_Environment")))
                .Select(GlowOf).Where(g => g.HasValue).Select(g => g.Value).ToList();
            // Merge sources closer than half their size: one light per orb cluster, not per mesh part.
            var merged = new List<Glow>();
            foreach (Glow g in glows.OrderByDescending(x => x.Size))
            {
                int i = merged.FindIndex(o => Vector3.Distance(o.Pos, g.Pos) < Mathf.Max(3f, 0.5f * Mathf.Max(o.Size, g.Size)));
                if (i < 0) { merged.Add(g); continue; }
                Glow o2 = merged[i];
                float w = o2.Weight + g.Weight;
                o2.Col = (o2.Col * o2.Weight + g.Col * g.Weight) / Mathf.Max(w, 1e-4f);
                o2.Weight = Mathf.Max(o2.Weight, g.Weight);
                merged[i] = o2;
            }
            merged = merged.OrderByDescending(g => g.Size * g.Weight).Take(MaxLights).ToList();

            var group = new GameObject("GW2Lights");
            Undo.RegisterCreatedObjectUndo(group, "GW2 glow lights");
            group.transform.SetParent(envRoot.transform, false);
            Type bpl = SceneSetup.FindType("BakeryPointLight");
            int k = 0;
            foreach (Glow g in merged)
            {
                float range = Mathf.Clamp(g.Size * 1.5f, 4f, 60f);
                float intensity = Mathf.Clamp(0.6f + g.Weight, 0.6f, 2.5f);
                var go = new GameObject("GlowLight_" + (k++));
                go.transform.SetParent(group.transform, false);
                go.transform.position = g.Pos;
                Light l = go.AddComponent<Light>();
                l.type = LightType.Point;
                l.lightmapBakeType = LightmapBakeType.Baked;
                l.color = g.Col;
                l.intensity = intensity;
                l.range = range;
                l.shadows = LightShadows.Soft;
                l.shadowRadius = Mathf.Clamp(g.Size * 0.2f, 0.1f, 5f);
                if (bpl != null)
                {
                    Component b = go.AddComponent(bpl);
                    SceneSetup.SetField(b, "color", g.Col);
                    SceneSetup.SetField(b, "intensity", intensity);
                    SceneSetup.SetField(b, "cutoff", range);
                    SceneSetup.SetField(b, "shadowSpread", Mathf.Clamp(g.Size * 0.2f, 0.1f, 5f));
                    SceneSetup.SetField(b, "samples", 16);
                    SceneSetup.SetField(b, "bakeToIndirect", true);   // direct + indirect: nothing realtime lights from it
                    SceneSetup.SetField(b, "realisticFalloff", false);
                }
            }
            return merged.Count + " glow light(s) from " + glows.Count + " glowing renderer(s)" + (bpl != null ? " (+ Bakery)" : "");
        }

        // ---- Bakery sun / sky --------------------------------------------------------------------------------

        static string BakerySunSky(GameObject envRoot)
        {
            Type dl = SceneSetup.FindType("BakeryDirectLight"), sl = SceneSetup.FindType("BakerySkyLight");
            if (dl == null || sl == null) return "no Bakery: Unity's sun and ambient only";
            Light sun = RenderSettings.sun != null ? RenderSettings.sun
                : Object.FindObjectsByType<Light>(FindObjectsSortMode.None).FirstOrDefault(x => x.type == LightType.Directional);
            if (sun != null)
            {
                Component d = sun.GetComponent(dl) ?? sun.gameObject.AddComponent(dl);
                SceneSetup.SetField(d, "color", sun.color);
                SceneSetup.SetField(d, "intensity", sun.intensity);
                SceneSetup.SetField(d, "shadowSpread", 0.02f);
                SceneSetup.SetField(d, "samples", 32);
                SceneSetup.SetField(d, "bakeToIndirect", false);   // indirect only: the Sun stays realtime
                SceneSetup.SetField(d, "shadowmask", false);
            }
            Transform old = envRoot.transform.Find("BakerySky");
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var go = new GameObject("BakerySky");
            Undo.RegisterCreatedObjectUndo(go, "Bakery sky");
            go.transform.SetParent(envRoot.transform, false);
            Component s = go.AddComponent(sl);
            // GW2's ambient: the Trilight's mean colour (SceneSetup: the fills, each by N.L).
            Color amb = RenderSettings.ambientMode == AmbientMode.Trilight
                ? (RenderSettings.ambientSkyColor + RenderSettings.ambientEquatorColor + RenderSettings.ambientGroundColor) / 3f
                : RenderSettings.ambientLight;
            float peak = Mathf.Max(amb.r, Mathf.Max(amb.g, amb.b), 1e-4f);
            SceneSetup.SetField(s, "color", new Color(amb.r / peak, amb.g / peak, amb.b / peak));
            SceneSetup.SetField(s, "intensity", peak);
            SceneSetup.SetField(s, "samples", 32);
            SceneSetup.SetField(s, "hemispherical", false);
            SceneSetup.SetField(s, "bakeToIndirect", true);
            // Bakery's render mode: Indirect (1), so the realtime Sun's direct light is not baked in twice.
            try
            {
                Type rl = SceneSetup.FindType("ftRenderLightmap");
                MethodInfo find = rl != null ? rl.GetMethod("FindRenderSettingsStorage", BindingFlags.Public | BindingFlags.Static) : null;
                object storage = find != null ? find.Invoke(null, new object[] { null }) : null;
                if (storage != null)
                {
                    SceneSetup.SetField(storage, "renderSettingsUserRenderMode", 1);
                    if (storage is Object so) EditorUtility.SetDirty(so);
                }
            }
            catch (Exception e) { Debug.LogWarning("CASTLEMIST Bakery render mode: " + (e.InnerException ?? e).Message); }
            return "Bakery sun (indirect only) and sky light, render mode Indirect";
        }
    }
}
