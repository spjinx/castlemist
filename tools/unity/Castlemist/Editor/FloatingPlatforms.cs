// castlemist world import: floating platforms that drift up and down (2026-10-27). Mad King's
// Labyrinth's flower platforms (2498697, the chain by the north-west waypoint) rise and sink in the
// game, each out of step (the user's video; the server moves them, so neither the map nor the model
// has the path: the model's own "zeropose08" bobs its Platform bone under a centimetre). Per placed
// platform:
//   collision   its triangles are cut out of the cell's merged collider (Colliders/Collider_<n>: the
//               triangles whose centre is inside the platform's bounds) and given back to it as its
//               own MeshCollider child ("GW2 Platform Collider"), with a kinematic Rigidbody, so the
//               floor moves with it and none is left behind. A rerun keeps the collider it made.
//   carrying    a trigger over its top ("GW2 Platform Stand", Ignore Raycast layer) with
//               Runtime/GW2FloatingPlatform: the height follows the server clock, the local player
//               standing on it moves with it.
// Amplitude and period are a reading of the video (no measurement): adjust them on each platform.
// 2026-10-27: players ride them through one PlatformLink (Runtime/PlatformLink.cs, jetdog8808's, MIT) on
// "GW2 Platform Link": it links to colliders with a Rigidbody on LinkLayer (Environment), which the
// platform colliders move to; the platforms' own carrying is off (it turned players around).

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class FloatingPlatforms
    {
        public const string ColliderName = "GW2 Platform Collider";
        public const string StandName = "GW2 Platform Stand";
        public const float Amplitude = 3f, Period = 12f;
        public const int LinkLayer = 11;   // VRChat's Environment
        public const string LinkName = "GW2 Platform Link";

        /// Map fileId -> the platform models that float there.
        static readonly Dictionary<long, long[]> Models = new Dictionary<long, long[]> { [2499169] = new long[] { 2498697 } };

        [MenuItem("Castlemist/Floating Platforms (drift, carry players)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            UnityEngine.SceneManagement.Scene scene = EditorSceneManager.GetActiveScene();
            string root = Path.GetDirectoryName(scene.path).Replace('\\', '/');
            GameObject mapRoot = scene.GetRootGameObjects().FirstOrDefault(g => g.name.StartsWith("GW2_", StringComparison.Ordinal) && !g.name.EndsWith("_Environment", StringComparison.Ordinal));
            if (mapRoot == null || !long.TryParse(mapRoot.name.Substring(4), out long mapId)) return "floating platforms: no GW2_<id> root in the open scene";
            if (!Models.TryGetValue(mapId, out long[] models)) return "floating platforms: map " + mapId + " has none";
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "floating platforms: UdonSharp is not in this project";
            if (made.Count > 0) return "floating platforms: UdonSharp program assets made: run again once UdonSharp has compiled";
            Type type = SceneSetup.FindType("Castlemist.GW2FloatingPlatform");
            Type linkType = SceneSetup.FindType("Castlemist.PlatformLink");
            if (type == null || linkType == null) return "floating platforms: GW2FloatingPlatform / PlatformLink not compiled yet";

            var platforms = mapRoot.GetComponentsInChildren<Transform>(true)
                .Where(t => t.parent != null && t.parent.name == "Props" && models.Any(m => t.name.StartsWith(m + "_", StringComparison.Ordinal)))
                .OrderBy(t => t.name, StringComparer.Ordinal).ToList();
            if (platforms.Count == 0) return "floating platforms: none placed";
            string dir = root + "/Meshes/Colliders";
            if (!AssetDatabase.IsValidFolder(root + "/Meshes")) AssetDatabase.CreateFolder(root, "Meshes");
            if (!AssetDatabase.IsValidFolder(dir)) AssetDatabase.CreateFolder(root + "/Meshes", "Colliders");
            Physics.SyncTransforms();

            int k = 0, cutTotal = 0;
            foreach (Transform p in platforms)
            {
                Bounds b = RenderBounds(p);
                Transform col = p.Find(ColliderName);
                MeshCollider ownCol = col != null ? col.GetComponent<MeshCollider>() : null;
                if (ownCol == null || ownCol.sharedMesh == null)
                {
                    Mesh own = CutOut(b, p, dir, out int cut);
                    cutTotal += cut;
                    if (own != null)
                    {
                        if (col == null) { col = new GameObject(ColliderName).transform; col.SetParent(p, false); }
                        col.localPosition = Vector3.zero; col.localRotation = Quaternion.identity; col.localScale = Vector3.one;
                        MeshCollider mc = col.GetComponent<MeshCollider>();
                        if (mc == null) mc = col.gameObject.AddComponent<MeshCollider>();
                        mc.sharedMesh = own;
                    }
                }
                if (col != null) col.gameObject.layer = LinkLayer;
                Rigidbody rb = p.GetComponent<Rigidbody>();
                if (rb == null) rb = p.gameObject.AddComponent<Rigidbody>();
                rb.isKinematic = true; rb.useGravity = false;
                foreach (Transform t in p.GetComponentsInChildren<Transform>(true)) GameObjectUtility.SetStaticEditorFlags(t.gameObject, 0);

                Transform old = p.Find(StandName);
                if (old != null) Object.DestroyImmediate(old.gameObject);
                var stand = new GameObject(StandName);
                stand.layer = 2;   // Ignore Raycast: no block on the interact laser
                stand.transform.SetParent(p, false);
                stand.transform.position = new Vector3(b.center.x, b.max.y + 0.75f, b.center.z);
                stand.transform.rotation = Quaternion.identity;
                Vector3 ls = p.lossyScale;
                stand.transform.localScale = new Vector3(1f / ls.x, 1f / ls.y, 1f / ls.z);
                var box = stand.AddComponent<BoxCollider>();
                box.isTrigger = true;
                box.size = new Vector3(b.size.x, 1.5f, b.size.z);
                Component comp = SceneSetup.AddUdonAware(stand, type);
                SceneSetup.SetField(comp, "platform", p);
                SceneSetup.SetField(comp, "amplitude", Amplitude);
                SceneSetup.SetField(comp, "period", Period);
                SceneSetup.SetField(comp, "phase", k * 2.39996f);   // golden angle: neighbours out of step
                SceneSetup.SetField(comp, "carryPlayers", false);
                SceneSetup.CopyProxyToUdon(comp);
                ++k;
            }
            // the one PlatformLink
            GameObject env = ImmersiveLighting.EnvRoot();
            Transform oldLink = env != null ? env.transform.Find(LinkName) : null;
            if (oldLink != null) Object.DestroyImmediate(oldLink.gameObject);
            var linkGo = new GameObject(LinkName);
            if (env != null) linkGo.transform.SetParent(env.transform, false);
            Component link = SceneSetup.AddUdonAware(linkGo, linkType);
            SceneSetup.SetField(link, "linkToLayers", (LayerMask)(1 << LinkLayer));
            SceneSetup.SetField(link, "inheriteVelocity", true);
            SceneSetup.SetField(link, "unLinkDistance", 3f);
            SceneSetup.CopyProxyToUdon(link);
            EditorSceneManager.MarkSceneDirty(scene);
            AssetDatabase.SaveAssets();
            return "floating platforms: " + k + " drifting " + Amplitude + " m over " + Period + " s, out of step (" + cutTotal + " collision triangle(s) moved off the cells onto them)";
        }

        static Bounds RenderBounds(Transform p)
        {
            Bounds b = new Bounds(p.position, Vector3.zero);
            bool any = false;
            foreach (Renderer r in p.GetComponentsInChildren<Renderer>(true))
            {
                if (r is ParticleSystemRenderer) continue;
                if (!any) { b = r.bounds; any = true; } else b.Encapsulate(r.bounds);
            }
            return b;
        }

        /// The cell collider triangles inside @p b, removed from the cells (saved as new meshes) and
        /// returned as one mesh in @p p's local space; null when there were none.
        static Mesh CutOut(Bounds b, Transform p, string dir, out int cut)
        {
            cut = 0;
            var ownV = new List<Vector3>();
            var ownT = new List<int>();
            Matrix4x4 toLocal = p.worldToLocalMatrix;
            foreach (MeshCollider mc in Object.FindObjectsByType<MeshCollider>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (mc.sharedMesh == null || mc.transform.parent == null || mc.transform.parent.name != "Colliders" || !mc.bounds.Intersects(b)) continue;
                Mesh src = mc.sharedMesh;
                Vector3[] v = src.vertices;
                Matrix4x4 w = mc.transform.localToWorldMatrix;
                var wv = new Vector3[v.Length];
                for (int i = 0; i < v.Length; ++i) wv[i] = w.MultiplyPoint3x4(v[i]);
                var keepSubs = new List<int[]>();
                int here = 0;
                for (int sub = 0; sub < src.subMeshCount; ++sub)
                {
                    int[] t = src.GetTriangles(sub);
                    var keep = new List<int>(t.Length);
                    for (int i = 0; i + 2 < t.Length; i += 3)
                    {
                        Vector3 c = (wv[t[i]] + wv[t[i + 1]] + wv[t[i + 2]]) / 3f;
                        if (b.Contains(c))
                        {
                            for (int j = 0; j < 3; ++j) { ownT.Add(ownV.Count); ownV.Add(toLocal.MultiplyPoint3x4(wv[t[i + j]])); }
                            ++here;
                            continue;
                        }
                        keep.Add(t[i]); keep.Add(t[i + 1]); keep.Add(t[i + 2]);
                    }
                    keepSubs.Add(keep.ToArray());
                }
                if (here == 0) continue;
                var mesh = new Mesh { name = src.name + "_platforms", indexFormat = src.indexFormat };
                mesh.vertices = v;
                mesh.subMeshCount = keepSubs.Count;
                for (int sub = 0; sub < keepSubs.Count; ++sub) mesh.SetTriangles(keepSubs[sub], sub);
                mesh.RecalculateBounds();
                string path = AssetDatabase.GenerateUniqueAssetPath(dir + "/" + mc.transform.parent.parent.name + "_" + mc.name + "_platforms.asset");
                AssetDatabase.CreateAsset(mesh, path);
                Undo.RecordObject(mc, "Floating platforms");
                mc.sharedMesh = mesh;
                cut += here;
            }
            if (ownT.Count == 0) return null;
            var own = new Mesh { name = p.name + "_collider" };
            own.indexFormat = ownV.Count > 65000 ? UnityEngine.Rendering.IndexFormat.UInt32 : UnityEngine.Rendering.IndexFormat.UInt16;
            own.SetVertices(ownV);
            own.SetTriangles(ownT, 0);
            own.RecalculateBounds();
            AssetDatabase.CreateAsset(own, AssetDatabase.GenerateUniqueAssetPath(dir + "/" + p.name + "_collider.asset"));
            return own;
        }
    }
}
