// castlemist world export: frame-time settings on an imported world without re-importing it
// (2026-10-26). Castlemist > Performance Pass. What new imports already get:
//   particles   every ParticleSystem (model prefabs and the scene) culls by Pause: off screen it
//               stops (ParticleEffects; Automatic kept 5272 non-procedural systems simulating unseen)
//   animators   placed props' Animators cull completely off screen (no state, no events); doors keep
//               CullUpdateTransforms (ProximityDoors), demolishables AlwaysAnimate (Demolishables)
//   particle LOD a model's particle renderers join every level of its LODGroup: they cull with the
//               prop (a lantern's flame drew on after the lantern culled; ~7 ms of a 46 ms editor frame
//               went to particle renderers)
//   shadows     placed props under SmallShadow metres across cast no shadow (the sun's shadow maps drew
//               8370 such casters in Lion's Arch; at that size their shadows are a few pixels)
//   terrain     heightmap pixel error PixelError, one-sided shadows, grass (details) to DetailDistance
// ScaleCullDistances (not in the pass: it trades distance for frame time) culls props nearer.
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Castlemist
{
    public static class Performance
    {
        [MenuItem("Castlemist/Performance Pass (particles, animators, shadows, terrain)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply()); }

        public const float SmallShadow = 3f;
        public const float PixelError = 10f;
        public const float DetailDistance = 75f;

        public static string Apply()
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "performance: the open scene is not in a castlemist export folder";
            int prefabs = 0, systems = 0, lods = 0;
            foreach (string g in AssetDatabase.FindAssets("t:Prefab", new[] { root + "/Prefabs" }))
            {
                string path = AssetDatabase.GUIDToAssetPath(g);
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
                if (asset == null || !asset.GetComponentsInChildren<ParticleSystem>(true).Any()) continue;
                GameObject contents = PrefabUtility.LoadPrefabContents(path);
                try
                {
                    lods += ParticlesInLods(contents);
                    foreach (ParticleSystem p in contents.GetComponentsInChildren<ParticleSystem>(true))
                    {
                        var main = p.main;
                        if (main.cullingMode == ParticleSystemCullingMode.Pause) continue;
                        main.cullingMode = ParticleSystemCullingMode.Pause;
                        ++systems;
                    }
                    PrefabUtility.SaveAsPrefabAsset(contents, path);
                    ++prefabs;
                }
                finally { PrefabUtility.UnloadPrefabContents(contents); }
            }
            int scene = 0;
            foreach (ParticleSystem p in Object.FindObjectsByType<ParticleSystem>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                var main = p.main;
                if (main.cullingMode == ParticleSystemCullingMode.Pause) continue;
                main.cullingMode = ParticleSystemCullingMode.Pause;
                ++scene;
            }
            System.Type demolishable = SceneSetup.FindType("Castlemist.GW2Demolishable");
            int animators = 0;
            System.Type door = SceneSetup.FindType("Castlemist.GW2ProximityDoor");
            foreach (Animator a in Object.FindObjectsByType<Animator>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (a.cullingMode == AnimatorCullingMode.CullCompletely) continue;
                if (demolishable != null && a.GetComponent(demolishable) != null) continue;
                if (door != null && a.GetComponentInChildren(door, true) != null) continue;
                if (!a.transform.IsChildOf(MapRoot(a.transform))) continue;
                // 2026-10-26: CullCompletely, not CullUpdateTransforms: that still ran every state machine
                // and fired every sound event off screen (4.9 ms in Lion's Arch).
                a.cullingMode = AnimatorCullingMode.CullCompletely;
                ++animators;
            }
            int shadows = 0;
            foreach (MeshRenderer r in Object.FindObjectsByType<MeshRenderer>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (r.shadowCastingMode == UnityEngine.Rendering.ShadowCastingMode.Off || r.bounds.size.magnitude >= SmallShadow) continue;
                Transform t = r.transform;
                while (t.parent != null && t.parent.name != "Props") t = t.parent;
                if (t.parent == null) continue;   // only placed props
                r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
                ++shadows;
            }
            int terrains = 0;
            foreach (Terrain t in Object.FindObjectsByType<Terrain>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                t.heightmapPixelError = Mathf.Max(t.heightmapPixelError, PixelError);
                t.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.On;
                t.detailObjectDistance = Mathf.Min(t.detailObjectDistance, DetailDistance);
                ++terrains;
            }
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "performance: " + shadows + " small prop(s) cast no shadow, " + terrains + " terrain(s) lighter; " + systems + " particle system(s) in " + prefabs + " prefab(s) + " + scene + " in the scene pause off screen, " +
                   lods + " particle renderer(s) put in their prop's LODGroup; " +
                   animators + " animator(s) cull off screen";
        }

        /// The importer's remembered Distance (the window's slider; the cull scale divides it), which
        /// Refresh rebuilds the LOD culls with.
        public const string DistancePref = "Castlemist.ImportWorld.Distance";
        public static float Distance()
        {
            return Mathf.Clamp(EditorPrefs.GetFloat(DistancePref, 1f), ImportOptions.MinDistance, ImportOptions.MaxDistance);
        }

        /// Every LODGroup's transition heights (model prefabs, foliage prefabs, and placements that
        /// override theirs) x @p factor, capped below 1: factor 2 culls and switches LODs at half the
        /// distance, as a lodBias of half would (Lion's Arch, editor render CPU: 44.5 ms -> 33.7 ms at 2,
        /// 22.5 ms at 4). 1 / factor undoes it.
        public static string ScaleCullDistances(float factor)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json") || !(factor > 0f)) return "cull: the open scene is not in a castlemist export folder";
            bool Scale(LODGroup g)
            {
                LOD[] ls = g.GetLODs();
                if (ls.Length == 0) return false;
                float cap = 0.999f;
                for (int i = 0; i < ls.Length; ++i)
                {
                    ls[i].screenRelativeTransitionHeight = Mathf.Min(ls[i].screenRelativeTransitionHeight * factor, cap);
                    cap = ls[i].screenRelativeTransitionHeight * 0.999f;   // strictly decreasing
                }
                g.SetLODs(ls);
                return true;
            }
            int prefabs = 0, overrides = 0;
            foreach (string g in AssetDatabase.FindAssets("t:Prefab", new[] { root + "/Prefabs" }))
            {
                string path = AssetDatabase.GUIDToAssetPath(g);
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
                if (asset == null || asset.GetComponentInChildren<LODGroup>(true) == null) continue;
                GameObject contents = PrefabUtility.LoadPrefabContents(path);
                try
                {
                    foreach (LODGroup lg in contents.GetComponentsInChildren<LODGroup>(true)) Scale(lg);
                    PrefabUtility.SaveAsPrefabAsset(contents, path);
                    ++prefabs;
                }
                finally { PrefabUtility.UnloadPrefabContents(contents); }
            }
            // Placements whose LODGroup overrides the prefab's (OVERRIDE_LOD) or that are no prefab.
            foreach (LODGroup lg in Object.FindObjectsByType<LODGroup>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                bool own = !PrefabUtility.IsPartOfPrefabInstance(lg) ||
                           PrefabUtility.GetPropertyModifications(lg)?.Any(m => m.target is LODGroup && m.propertyPath.Contains("m_LODs")) == true;
                if (own && Scale(lg)) ++overrides;
            }
            // What a Refresh rebuilds the culls with from now on.
            EditorPrefs.SetFloat(DistancePref, Mathf.Clamp(Distance() / factor, ImportOptions.MinDistance, ImportOptions.MaxDistance));
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "cull: LOD heights x" + factor + " in " + prefabs + " prefab(s) and " + overrides + " placement(s)";
        }

        /// Metres within which every LODGroup still draws, however small (2026-10-26: at a quarter of the
        /// cull distances, small props vanished 5-15 m away).
        public const float KeepNearMetres = 40f;

        [MenuItem("Castlemist/Keep Small Props Visible Nearby (40 m)")]
        public static void KeepNearMenu() { Debug.Log("CASTLEMIST " + KeepNearAll(KeepNearMetres)); }

        /// @p g's cull height capped so it draws within @p metres: a LODGroup culls below
        /// screenRelativeTransitionHeight = size / (2 d tan(fov / 2)) x lodBias; taken at a 90 degree
        /// field of view (VR's is about that or wider) and lodBias 1. Returns whether it changed.
        public static bool KeepNear(LODGroup g, float metres)
        {
            LOD[] ls = g.GetLODs();
            if (ls.Length == 0) return false;
            Vector3 s = g.transform.lossyScale;
            float size = g.size * Mathf.Max(Mathf.Abs(s.x), Mathf.Max(Mathf.Abs(s.y), Mathf.Abs(s.z)));
            float cap = size / (2f * Mathf.Max(metres, 0.01f));
            int last = ls.Length - 1;
            if (!(ls[last].screenRelativeTransitionHeight > cap)) return false;
            // Only the cull height, through its serialized field: on a prefab instance that overrides that
            // one value, not the LOD renderer lists (which a prefab rebuild would leave dangling).
            var so = new SerializedObject(g);
            SerializedProperty h = so.FindProperty("m_LODs.Array.data[" + last + "].screenRelativeHeight");
            if (h == null) { ls[last].screenRelativeTransitionHeight = cap; g.SetLODs(ls); return true; }
            h.floatValue = cap;
            so.ApplyModifiedPropertiesWithoutUndo();
            return true;
        }

        /// KeepNear over every LODGroup of the export's prefabs (models, foliage) and the open scene's own.
        public static string KeepNearAll(float metres)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "keep near: the open scene is not in a castlemist export folder";
            int prefabs = 0, groups = 0;
            foreach (string g in AssetDatabase.FindAssets("t:Prefab", new[] { root + "/Prefabs" }))
            {
                string path = AssetDatabase.GUIDToAssetPath(g);
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(path);
                if (asset == null || asset.GetComponentInChildren<LODGroup>(true) == null) continue;
                GameObject contents = PrefabUtility.LoadPrefabContents(path);
                try
                {
                    int n = 0;
                    foreach (LODGroup lg in contents.GetComponentsInChildren<LODGroup>(true)) if (KeepNear(lg, metres)) ++n;
                    if (n > 0) { PrefabUtility.SaveAsPrefabAsset(contents, path); ++prefabs; groups += n; }
                }
                finally { PrefabUtility.UnloadPrefabContents(contents); }
            }
            // Every placement at its placed scale (props are placed smaller than their model: the prefab's
            // size missed 6457 of them), foliage patches and OVERRIDE_LOD placements included.
            int own = 0;
            foreach (LODGroup lg in Object.FindObjectsByType<LODGroup>(FindObjectsInactive.Include, FindObjectsSortMode.None))
                if (KeepNear(lg, metres)) ++own;
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "keep near: " + groups + " LODGroup(s) in " + prefabs + " prefab(s) and " + own + " in the scene now draw within " + metres + " m";
        }

        /// @p root's particle renderers added to every level of its LODGroup (none: nothing); returns how
        /// many were not in it yet.
        public static int ParticlesInLods(GameObject root)
        {
            LODGroup g = root.GetComponent<LODGroup>();
            if (g == null) return 0;
            LOD[] ls = g.GetLODs();
            if (ls.Length == 0) return 0;
            var ps = root.GetComponentsInChildren<ParticleSystemRenderer>(true);
            int added = 0;
            for (int i = 0; i < ls.Length; ++i)
            {
                var missing = ps.Where(r => !ls[i].renderers.Contains(r)).Cast<Renderer>().ToArray();
                if (missing.Length == 0) continue;
                if (i == 0) added = missing.Length;
                ls[i].renderers = ls[i].renderers.Concat(missing).ToArray();
            }
            if (added > 0) g.SetLODs(ls);
            return added;
        }

        /// The GW2_<fileId> root @p t sits under (only the map's props are touched, not the world's own rigs).
        static Transform MapRoot(Transform t)
        {
            Transform r = t.root;
            return r.name.StartsWith("GW2_") ? r : t;
        }
    }
}
