// castlemist world export: unanimated skinned meshes as plain meshes (2026-10-26). Castlemist >
// Static Skinned Meshes to Mesh Renderers. The export skins every mesh of a rigged model, animated or
// not: Lion's Arch had 12031 SkinnedMeshRenderers, 10473 under no Animator, ~10 ms of a 40 ms editor
// frame (no instancing, no static batching, skinning every frame). Each becomes a MeshRenderer of
// its mesh baked at the pose it shows (BakeMesh, checked against the skinned vertex 0), with its
// materials, shadow mode and enabled state; LODGroups and particle shapes that pointed at it point
// at the MeshRenderer. The baked meshes go to Meshes/Static/<prefab>.asset.
//   prefabs     a model with no animation clips: in its prefab (every placement follows)
//   placements  an instance of a model with clips but no Animator: on that instance
// Renderers under an Animator / Animation are left, and (2026-10-27) any a Castlemist behaviour points
// at: the carving pumpkins show / hide theirs by reference, and converting them left those null.
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
    public static class StaticSkinned
    {
        [MenuItem("Castlemist/Static Skinned Meshes to Mesh Renderers")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Convert()); }

        /// The conversion above on the open scene; returns a one-line report.
        public static string Convert()
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "static skinned: the open scene is not in a castlemist export folder";
            string dir = root + "/Meshes/Static";
            if (!AssetDatabase.IsValidFolder(root + "/Meshes")) AssetDatabase.CreateFolder(root, "Meshes");
            if (!AssetDatabase.IsValidFolder(dir)) AssetDatabase.CreateFolder(root + "/Meshes", "Static");

            HashSet<Object> held = HeldByBehaviours();
            var targets = Object.FindObjectsByType<SkinnedMeshRenderer>(FindObjectsInactive.Include, FindObjectsSortMode.None)
                .Where(r => r.sharedMesh != null && !Animated(r.transform) && !held.Contains(r)).ToList();
            var clipless = new Dictionary<string, bool>();
            var byPrefab = new Dictionary<string, List<SkinnedMeshRenderer>>();
            var onInstances = new List<SkinnedMeshRenderer>();
            foreach (SkinnedMeshRenderer r in targets)
            {
                string pf = PrefabUtility.IsPartOfPrefabInstance(r) ? PrefabUtility.GetPrefabAssetPathOfNearestInstanceRoot(r) : null;
                if (pf != null && !clipless.TryGetValue(pf, out bool none))
                    clipless[pf] = none = !AssetDatabase.LoadAllAssetsAtPath(AssetDatabase.GetAssetPath(r.sharedMesh)).OfType<AnimationClip>()
                                              .Any(c => !c.name.StartsWith("__preview__"));
                if (pf != null && clipless[pf])
                {
                    if (!byPrefab.TryGetValue(pf, out var l)) byPrefab[pf] = l = new List<SkinnedMeshRenderer>();
                    l.Add(r);
                }
                else onInstances.Add(r);
            }

            // What each scene placement shows (instance overrides included), to restore after the prefab changes.
            var keep = new List<(GameObject go, Material[] mats, UnityEngine.Rendering.ShadowCastingMode shadow, bool on)>();
            foreach (var l in byPrefab.Values)
                foreach (SkinnedMeshRenderer r in l) keep.Add((r.gameObject, r.sharedMaterials, r.shadowCastingMode, r.enabled));

            int inPrefabs = 0, failed = 0;
            foreach (var kv in byPrefab)
            {
                GameObject contents = PrefabUtility.LoadPrefabContents(kv.Key);
                try
                {
                    var meshes = new List<Mesh>();
                    foreach (SkinnedMeshRenderer s in contents.GetComponentsInChildren<SkinnedMeshRenderer>(true))
                    {
                        Mesh baked = ToMeshRenderer(s, out MeshRenderer _);
                        if (baked == null) { ++failed; continue; }
                        meshes.Add(baked);
                        ++inPrefabs;
                    }
                    SaveMeshes(meshes, dir + "/" + Path.GetFileNameWithoutExtension(kv.Key) + ".asset");
                    PrefabUtility.SaveAsPrefabAsset(contents, kv.Key);
                }
                finally { PrefabUtility.UnloadPrefabContents(contents); }
            }
            int restored = 0;
            foreach (var k in keep)
            {
                if (k.go == null) continue;
                var mr = k.go.GetComponent<MeshRenderer>();
                if (mr == null) continue;
                bool changed = false;
                if (!mr.sharedMaterials.SequenceEqual(k.mats)) { mr.sharedMaterials = k.mats; changed = true; }
                if (mr.shadowCastingMode != k.shadow) { mr.shadowCastingMode = k.shadow; changed = true; }
                if (mr.enabled != k.on) { mr.enabled = k.on; changed = true; }
                if (changed) ++restored;
            }

            // Placements of animated models that are not animated: on the instance, one baked mesh per
            // prefab renderer path (every placement of a model rests in the same pose).
            int onInst = 0;
            var cache = new Dictionary<string, Mesh>();
            var instMeshes = new List<Mesh>();
            foreach (SkinnedMeshRenderer s in onInstances)
            {
                if (s == null) continue;
                GameObject instRoot = PrefabUtility.GetNearestPrefabInstanceRoot(s);
                string key = (instRoot != null ? PrefabUtility.GetPrefabAssetPathOfNearestInstanceRoot(s) + ":" + PathIn(s.transform, instRoot.transform) : null);
                Mesh shared = key != null && cache.TryGetValue(key, out Mesh m) ? m : null;
                Mesh baked = ToMeshRenderer(s, out MeshRenderer _, shared);
                if (baked == null) { ++failed; continue; }
                if (baked != shared)
                {
                    if (key != null && shared == null) cache[key] = baked;
                    instMeshes.Add(baked);
                }
                ++onInst;
            }
            // A new file each run: earlier runs' placements still use theirs.
            SaveMeshes(instMeshes, AssetDatabase.GenerateUniqueAssetPath(dir + "/_placements.asset"));
            AssetDatabase.SaveAssets();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "static skinned: " + inPrefabs + " renderer(s) in " + byPrefab.Count + " prefab(s), " + onInst + " on placements, " +
                   restored + " placement override(s) kept" + (failed > 0 ? ", " + failed + " left skinned (bake did not match)" : "");
        }

        /// Every renderer a Castlemist behaviour in the open scene references (its serialized fields).
        static HashSet<Object> HeldByBehaviours()
        {
            var held = new HashSet<Object>();
            foreach (MonoBehaviour b in Object.FindObjectsByType<MonoBehaviour>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (b == null || b.GetType().Namespace != "Castlemist") continue;
                SerializedProperty it = new SerializedObject(b).GetIterator();
                while (it.Next(true))
                    if (it.propertyType == SerializedPropertyType.ObjectReference && it.objectReferenceValue is Renderer r) held.Add(r);
            }
            return held;
        }

        static bool Animated(Transform t)
        {
            return t.GetComponentInParent<Animator>(true) != null || t.GetComponentInParent<Animation>(true) != null;
        }

        static string PathIn(Transform t, Transform root)
        {
            string p = t.name;
            while (t.parent != null && t.parent != root) { t = t.parent; p = t.name + "/" + p; }
            return p;
        }

        /// @p s replaced by a MeshFilter + MeshRenderer of its mesh baked at its current pose (or
        /// @p shared, already baked for the same renderer); returns the mesh, null when the bake does
        /// not reproduce the skinned vertex 0 (left skinned).
        static Mesh ToMeshRenderer(SkinnedMeshRenderer s, out MeshRenderer made, Mesh shared = null)
        {
            made = null;
            Vector3 want = Skinned0(s);
            bool Matches(Mesh b) => b.vertexCount > 0 && (s.transform.TransformPoint(b.vertices[0]) - want).magnitude <= 1e-3f * (1f + want.magnitude);
            Mesh mesh = shared != null && Matches(shared) ? shared : null;
            if (mesh == null)
            {
                foreach (bool useScale in new[] { false, true })
                {
                    var b = new Mesh();
                    s.BakeMesh(b, useScale);
                    if (Matches(b)) { mesh = b; break; }
                    Object.DestroyImmediate(b);
                }
                if (mesh == null) return null;
                mesh.name = s.sharedMesh.name;
                mesh.RecalculateBounds();
            }
            GameObject go = s.gameObject;
            Material[] mats = s.sharedMaterials;
            var shadow = s.shadowCastingMode;
            bool receive = s.receiveShadows, on = s.enabled;
            var probes = s.lightProbeUsage;
            var refl = s.reflectionProbeUsage;
            // The LOD slots and particle shapes that name this renderer, to point at the new one.
            var slots = new List<(LODGroup g, int lod, int k)>();
            foreach (LODGroup g in go.GetComponentsInParent<LODGroup>(true))
            {
                LOD[] ls = g.GetLODs();
                for (int i = 0; i < ls.Length; ++i)
                    for (int k = 0; k < ls[i].renderers.Length; ++k)
                        if (ls[i].renderers[k] == s) slots.Add((g, i, k));
            }
            GameObject top = PrefabUtility.GetNearestPrefabInstanceRoot(go);
            if (top == null) top = go.transform.root.gameObject;
            var shapes = top.GetComponentsInChildren<ParticleSystem>(true).Where(p => p.shape.skinnedMeshRenderer == s).ToList();
            Object.DestroyImmediate(s, true);
            var mf = go.GetComponent<MeshFilter>();
            if (mf == null) mf = go.AddComponent<MeshFilter>();
            mf.sharedMesh = mesh;
            made = go.AddComponent<MeshRenderer>();
            made.sharedMaterials = mats;
            made.shadowCastingMode = shadow;
            made.receiveShadows = receive;
            made.enabled = on;
            made.lightProbeUsage = probes;
            made.reflectionProbeUsage = refl;
            foreach (var g in slots.Select(x => x.g).Distinct())
            {
                LOD[] ls = g.GetLODs();
                foreach (var x in slots.Where(x => x.g == g)) ls[x.lod].renderers[x.k] = made;
                g.SetLODs(ls);
            }
            foreach (ParticleSystem p in shapes)
            {
                var sh = p.shape;
                sh.shapeType = ParticleSystemShapeType.MeshRenderer;
                sh.meshRenderer = made;
            }
            return mesh;
        }

        /// The skinned position of vertex 0 in world space (the bone matrices x bind poses, by weight).
        static Vector3 Skinned0(SkinnedMeshRenderer s)
        {
            Mesh m = s.sharedMesh;
            var bw = m.boneWeights;
            Transform[] bones = s.bones;
            Matrix4x4[] bp = m.bindposes;
            Vector3 v = m.vertices[0];
            if (bw.Length == 0 || bones.Length == 0) return s.transform.TransformPoint(v);
            BoneWeight w = bw[0];
            Vector3 P(int i) => i < bones.Length && bones[i] != null ? (bones[i].localToWorldMatrix * bp[i]).MultiplyPoint3x4(v) : Vector3.zero;
            return P(w.boneIndex0) * w.weight0 + P(w.boneIndex1) * w.weight1 + P(w.boneIndex2) * w.weight2 + P(w.boneIndex3) * w.weight3;
        }

        static void SaveMeshes(List<Mesh> meshes, string path)
        {
            if (meshes.Count == 0) return;
            if (AssetDatabase.LoadMainAssetAtPath(path) != null) AssetDatabase.DeleteAsset(path);
            AssetDatabase.CreateAsset(meshes[0], path);
            for (int i = 1; i < meshes.Count; ++i) AssetDatabase.AddObjectToAsset(meshes[i], path);
        }
    }
}
