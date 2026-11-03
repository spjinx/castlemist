// castlemist world export: doors that open while a player is near (2026-10-25).
//
// A model with both an "Open" and a "Close" clip (Lion's Arch's 92042) gets one controller,
// Controllers/<fileId>_Door.controller: "Closed" (Close held at its end) -> "Opening" (Open) when
// the bool "Open" turns on -> "Opened" (Open held at its end) -> "Closing" (Close) when it turns off
// -> "Closed"; Opening and Closing turn round at once when the bool flips. The clips are the
// importer's copies (WithConstants: mesh visibility, material constants, sound events). Each placed
// instance then gets that controller, skinned bounds fitted to it, and a "GW2 Door Trigger" child:
// a trigger sphere of TriggerRadius metres about the renderers' centre and Runtime/GW2ProximityDoor.
// The door's collision stays as the export placed it. 2026-10-26: the cell collision meshes (the map's
// havok, Cell_<id>/Colliders) also hold every door shut; CutCellColliders takes out their walls (normal
// within WallNormalY of horizontal) inside each opening door's closed bounds (+ Margin), so the doorway
// is open while the door is; floors and steps stay. The cut meshes go to Meshes/Colliders/.

using System;
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;

namespace Castlemist
{
    public static class ProximityDoors
    {
        public const string OpenClip = "Open", CloseClip = "Close";
        public const string TriggerName = "GW2 Door Trigger";
        /// Metres from the door's centre at which it opens (castlemist's choice).
        public const float TriggerRadius = 4f;
        public const float Margin = 0.15f;       // metres around the door's closed bounds
        public const float WallNormalY = 0.7f;   // |normal.y| below this is wall, not floor
        /// Metres across, at most, at a door model's median placement, for its doorways to be cut: the larger
        /// "doors" (gates and whole structures with Open / Close clips, 17-25 m: 226415, 1226119, 499261...)
        /// would take walls with them.
        public const float MaxDoorSize = 12f;   // doors placed scaled up reach 8 m; the gates are 17 m and more

        [MenuItem("Castlemist/Open Doorways in Map Collision")]
        public static void CutMenu() { Debug.Log("CASTLEMIST " + CutCellColliders()); }

        /// The open scene's opening doors (GW2 Door Trigger + GW2ProximityDoor): the cell collision walls
        /// inside each door's closed bounds removed; returns a one-line report (doorways still blocked by a
        /// ray across them are counted).
        /// @p root is the world export folder; null takes the open scene's (an import calls this before
        /// its new scene is saved, when the scene has no path).
        public static string CutCellColliders(string root = null)
        {
            var scene = UnityEditor.SceneManagement.EditorSceneManager.GetActiveScene();
            if (root == null && !string.IsNullOrEmpty(scene.path)) root = System.IO.Path.GetDirectoryName(scene.path).Replace('\\', '/');
            Type doorType = SceneSetup.FindType("Castlemist.GW2ProximityDoor");
            if (string.IsNullOrEmpty(root) || doorType == null) return "doorways: not a castlemist world scene, or GW2ProximityDoor not compiled";
            // By model: a door whose placements are, at the median, at most MaxDoorSize across (one placed
            // scaled down does not make a gate a door).
            var byModel = new Dictionary<string, List<Bounds>>();
            foreach (Component d in UnityEngine.Object.FindObjectsByType(doorType, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                Transform inst = d.transform.parent;
                if (inst == null) continue;
                bool any = false;
                Bounds b = default;
                foreach (Renderer r in inst.GetComponentsInChildren<Renderer>(true))
                {
                    if (r is ParticleSystemRenderer) continue;
                    if (!any) { b = r.bounds; any = true; } else b.Encapsulate(r.bounds);
                }
                if (!any) continue;
                b.Expand(Margin * 2f);
                string model = inst.name.Split('_')[0];
                if (!byModel.TryGetValue(model, out var l)) byModel[model] = l = new List<Bounds>();
                l.Add(b);
            }
            var boxes = new List<Bounds>();
            foreach (var kv in byModel)
            {
                var sizes = kv.Value.Select(x => x.size.magnitude).OrderBy(x => x).ToList();
                if (sizes[sizes.Count / 2] <= MaxDoorSize) boxes.AddRange(kv.Value);
            }
            if (boxes.Count == 0) return "doorways: no opening doors in the scene";
            string dir = root + "/Meshes/Colliders";
            if (!AssetDatabase.IsValidFolder(root + "/Meshes")) AssetDatabase.CreateFolder(root, "Meshes");
            if (!AssetDatabase.IsValidFolder(dir)) AssetDatabase.CreateFolder(root + "/Meshes", "Colliders");
            int meshes = 0, removed = 0;
            foreach (MeshCollider mc in UnityEngine.Object.FindObjectsByType<MeshCollider>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (mc.sharedMesh == null || mc.transform.parent == null || mc.transform.parent.name != "Colliders") continue;
                Bounds cb = mc.bounds;
                var near = boxes.Where(x => x.Intersects(cb)).ToList();
                if (near.Count == 0) continue;
                Mesh src = mc.sharedMesh;
                Vector3[] v = src.vertices;
                Matrix4x4 w = mc.transform.localToWorldMatrix;
                var wv = new Vector3[v.Length];
                for (int i = 0; i < v.Length; ++i) wv[i] = w.MultiplyPoint3x4(v[i]);
                var keepSubs = new List<int[]>();
                int cut = 0;
                for (int sub = 0; sub < src.subMeshCount; ++sub)
                {
                    int[] t = src.GetTriangles(sub);
                    var keep = new List<int>(t.Length);
                    for (int k = 0; k + 2 < t.Length; k += 3)
                    {
                        Vector3 a = wv[t[k]], b = wv[t[k + 1]], c = wv[t[k + 2]];
                        Vector3 n = Vector3.Cross(b - a, c - a);
                        bool wall = n.sqrMagnitude > 1e-12f && Mathf.Abs(n.normalized.y) < WallNormalY;
                        Vector3 centre = (a + b + c) / 3f;
                        if (wall && near.Any(x => x.Contains(centre))) { ++cut; continue; }
                        keep.Add(t[k]); keep.Add(t[k + 1]); keep.Add(t[k + 2]);
                    }
                    keepSubs.Add(keep.ToArray());
                }
                if (cut == 0) continue;
                var mesh = new Mesh { name = src.name + "_doorways", indexFormat = src.indexFormat };
                mesh.vertices = v;
                mesh.subMeshCount = keepSubs.Count;
                for (int sub = 0; sub < keepSubs.Count; ++sub) mesh.SetTriangles(keepSubs[sub], sub);
                mesh.RecalculateBounds();
                string cell = mc.transform.parent.parent != null ? mc.transform.parent.parent.name : "cell";
                string path = dir + "/" + cell + "_" + mc.name + ".asset";
                if (AssetDatabase.LoadMainAssetAtPath(path) != null) AssetDatabase.DeleteAsset(path);
                AssetDatabase.CreateAsset(mesh, path);
                mc.sharedMesh = mesh;
                ++meshes;
                removed += cut;
            }
            AssetDatabase.SaveAssets();
            Physics.SyncTransforms();
            // Rays across each doorway's box along both horizontal axes, at knee height: blocked when both
            // meet cell collision inside the box (a doorway runs along one of them).
            int blocked = 0;
            foreach (Bounds b in boxes)
            {
                bool Crossed(Vector3 axis, float half)
                {
                    Vector3 p = new Vector3(b.center.x, b.min.y + Margin + 0.6f, b.center.z) - axis * half;
                    foreach (RaycastHit h in Physics.RaycastAll(p, axis, half * 2f, ~0, QueryTriggerInteraction.Ignore))
                        if (h.collider is MeshCollider hm && hm.transform.parent != null && hm.transform.parent.name == "Colliders") return true;
                    return false;
                }
                if (Crossed(Vector3.right, b.extents.x) && Crossed(Vector3.forward, b.extents.z)) ++blocked;
            }
            UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(scene);
            return "doorways: " + removed + " collision wall triangle(s) cut from " + meshes + " cell collider(s) at " + boxes.Count + " door(s); " +
                   blocked + " doorway(s) still crossed by cell collision";
        }

        /// True when @p clips (models[].clips) has both door clips, in any case.
        public static bool Has(IEnumerable<string> clips)
        {
            return clips != null && clips.Any(c => string.Equals(c, OpenClip, StringComparison.OrdinalIgnoreCase)) &&
                   clips.Any(c => string.Equals(c, CloseClip, StringComparison.OrdinalIgnoreCase));
        }

        /// The controller at @p path (replaced), see the header.
        public static AnimatorController BuildController(string path, AnimationClip open, AnimationClip close)
        {
            if (AssetDatabase.LoadAssetAtPath<AnimatorController>(path) != null) AssetDatabase.DeleteAsset(path);
            foreach (AnimationClip c in new[] { open, close })
            {
                AnimationClipSettings s = AnimationUtility.GetAnimationClipSettings(c);
                if (s.loopTime) { s.loopTime = false; AnimationUtility.SetAnimationClipSettings(c, s); }
            }
            var ac = AnimatorController.CreateAnimatorControllerAtPath(path);
            ac.AddParameter("Open", AnimatorControllerParameterType.Bool);
            ac.AddParameter(new AnimatorControllerParameter { name = "EndTime", type = AnimatorControllerParameterType.Float, defaultFloat = 0.999f });
            AnimatorStateMachine sm = ac.layers[0].stateMachine;
            AnimatorState closed = sm.AddState("Closed"), opening = sm.AddState("Opening"),
                          opened = sm.AddState("Opened"), closing = sm.AddState("Closing");
            closed.motion = close; closed.timeParameterActive = true; closed.timeParameter = "EndTime";
            opened.motion = open; opened.timeParameterActive = true; opened.timeParameter = "EndTime";
            opening.motion = open;
            closing.motion = close;
            sm.defaultState = closed;
            void On(AnimatorState from, AnimatorState to, bool open_)
            {
                AnimatorStateTransition t = from.AddTransition(to);
                t.AddCondition(open_ ? AnimatorConditionMode.If : AnimatorConditionMode.IfNot, 0, "Open");
                t.hasExitTime = false; t.duration = 0f;
            }
            void End(AnimatorState from, AnimatorState to)
            {
                AnimatorStateTransition t = from.AddTransition(to);
                t.hasExitTime = true; t.exitTime = 1f; t.duration = 0f;
            }
            On(closed, opening, true);
            End(opening, opened);
            On(opening, closing, false);
            On(opened, closing, false);
            End(closing, closed);
            On(closing, opening, true);
            EditorUtility.SetDirty(ac);
            return ac;
        }

        /// Sets placed instance @p inst up (see the header). False when GW2ProximityDoor is not compiled.
        public static bool SetupInstance(GameObject inst, AnimatorController ac)
        {
            Type door = SceneSetup.FindType("Castlemist.GW2ProximityDoor");
            if (door == null) return false;
            Animator an = inst.GetComponent<Animator>();
            if (an == null) an = inst.AddComponent<Animator>();
            an.runtimeAnimatorController = ac;
            an.cullingMode = AnimatorCullingMode.CullUpdateTransforms;
            foreach (Transform t in inst.GetComponentsInChildren<Transform>(true))
                GameObjectUtility.SetStaticEditorFlags(t.gameObject, 0);
            WorldImporter.FitSkinnedBounds(inst, ac);

            Transform old = inst.transform.Find(TriggerName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            Bounds wb = default;
            bool any = false;
            foreach (Renderer r in inst.GetComponentsInChildren<Renderer>(true))
            {
                if (r is ParticleSystemRenderer) continue;
                Bounds b = r is SkinnedMeshRenderer s && s.sharedMesh != null
                    ? new Bounds(s.transform.TransformPoint(s.sharedMesh.bounds.center), Vector3.zero) : r.bounds;
                if (!any) { wb = b; any = true; } else wb.Encapsulate(b);
            }
            var go = new GameObject(TriggerName);
            go.transform.SetParent(inst.transform, false);
            go.transform.position = any ? wb.center : inst.transform.position;
            var sphere = go.AddComponent<SphereCollider>();
            sphere.isTrigger = true;
            float s0 = Mathf.Max(go.transform.lossyScale.x, 1e-6f);
            sphere.radius = TriggerRadius / s0;
            var comp = SceneSetup.AddUdonAware(go, door);
            SceneSetup.SetField(comp, "animator", an);
            SceneSetup.CopyProxyToUdon(comp);
            return true;
        }
    }
}
