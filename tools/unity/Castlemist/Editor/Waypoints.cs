// castlemist world export: the map's GW2 waypoints, vistas and landmarks (2026-10-25). The map file
// has none (they are game content); the public GW2 API lists them per map:
//   /v2/maps/<id>                                   continent_id, region_id, default_floor,
//                                                   map_rect (map units: Lion's Arch's matches the
//                                                   export's cell rects), continent_rect
//   /v2/continents/<c>/floors/<f>/regions/<r>/maps/<id>   points_of_interest {name, type, coord}
// A coord (continent space, y down) becomes map x = map.x0 + (cx - cont.x0) / cont.w x map.w,
// y = map.y1 - (cy - cont.y0) / cont.h x map.h; the height is a raycast down through the scene's
// colliders (+ 1 m), else the water plane. "GW2 Waypoints" gets one child per point ("<type> -
// <name>") and Runtime/GW2Waypoints (MaxPoints: its numbered teleports), and the clickable map
// (WaypointMap). The API map id comes from
// ApiMapIds (map fileId -> id); a map without one gets nothing (warned). Needs the network once.
// 2026-10-27: the waypoint itself, GW2's model 106188 (found by its sound script 106173, whose one
// sound is "waypoint"; meshes shrinegood / shrinecont: the shrine and its contested look), stands at
// each waypoint in its "on08" loop with its hum (106172), the contested meshes and emitters off; the
// teleport target moves ArriveOffset in front of it. Any export that has Prefabs/106188.prefab serves
// (gw2dat_cli model-export + world-fbx, added to the manifest); "Drop GW2 Waypoint Model" puts one
// at the Scene view's centre in any map.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using UnityEngine.Networking;

namespace Castlemist
{
    public static class Waypoints
    {
        public const string GroupName = "GW2 Waypoints";
        public const int MaxPoints = 64;
        const string Api = "https://api.guildwars2.com/v2/";

        /// Map fileId (mapc) -> GW2 API map id. Add a map here to give it waypoints.
        public static readonly Dictionary<long, int> ApiMapIds = new Dictionary<long, int> { { 191000, 50 }, { 2499169, 866 }, { 506592, 863 } };   // 2026-10-26: Mad King's Labyrinth, Ascent to Madness

        public static int Setup(GameObject mapRoot, WorldManifest m, string root, float unityScale, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            if (!ApiMapIds.TryGetValue(m.MapFileId, out int id)) { warnings.Add("waypoints: map " + m.MapFileId + " has no GW2 API map id (Waypoints.ApiMapIds): none placed"); return 0; }
            Type type = SceneSetup.FindType("Castlemist.GW2Waypoints");
            if (type == null) { warnings.Add("waypoints: Castlemist.GW2Waypoints is not compiled yet: refresh again once UdonSharp has compiled"); return 0; }
            var map = Get(Api + "maps/" + id, warnings);
            if (map == null) return 0;
            double[] mr = Rect(map, "map_rect"), cr = Rect(map, "continent_rect");
            int cont = (int)Num(map, "continent_id"), region = (int)Num(map, "region_id"), floor = (int)Num(map, "default_floor");
            var detail = Get(Api + "continents/" + cont + "/floors/" + floor + "/regions/" + region + "/maps/" + id, warnings);
            var pois = detail != null && detail.TryGetValue("points_of_interest", out object po) ? po as Dictionary<string, object> : null;
            if (mr == null || cr == null || pois == null) { warnings.Add("waypoints: the API answer for map " + id + " has no rects or points"); return 0; }

            var list = new List<Dictionary<string, object>>();
            foreach (var kv in pois) if (kv.Value is Dictionary<string, object> p) list.Add(p);
            // waypoints first, then vistas, landmarks, the rest; by name within a kind
            int Rank(string t) => t == "waypoint" ? 0 : t == "vista" ? 1 : t == "landmark" ? 2 : 3;
            list.Sort((a, b) =>
            {
                int r = Rank(Str(a, "type")).CompareTo(Rank(Str(b, "type")));
                return r != 0 ? r : string.CompareOrdinal(Str(a, "name"), Str(b, "name"));
            });
            if (list.Count > MaxPoints) { warnings.Add("waypoints: " + list.Count + " points, the first " + MaxPoints + " kept"); list.RemoveRange(MaxPoints, list.Count - MaxPoints); }

            var group = new GameObject(GroupName);
            group.transform.SetParent(mapRoot.transform, false);
            float waterY = m.Water != null && m.Water.Plane ? mapRoot.transform.position.y - (float)m.Water.Z * unityScale : mapRoot.transform.position.y;
            var names = new List<string>(); var kinds = new List<string>(); var points = new List<Transform>();
            var coords = new List<Vector2>();
            foreach (var p in list)
            {
                if (!(p.TryGetValue("coord", out object co) && co is List<object> c && c.Count >= 2)) continue;
                double cx = Convert.ToDouble(c[0]), cy = Convert.ToDouble(c[1]);
                double x = mr[0] + (cx - cr[0]) / (cr[2] - cr[0]) * (mr[2] - mr[0]);
                double y = mr[3] - (cy - cr[1]) / (cr[3] - cr[1]) * (mr[3] - mr[1]);
                Vector3 w = mapRoot.transform.TransformPoint(new Vector3((float)x, 0f, (float)y));
                float h = Stand(ref w, waterY) + 1f;
                string kind = Str(p, "type") ?? "point", name = Str(p, "name") ?? "";
                var go = new GameObject(char.ToUpperInvariant(kind[0]) + kind.Substring(1) + " - " + name);
                go.transform.SetParent(group.transform, true);
                go.transform.position = new Vector3(w.x, h, w.z);
                names.Add(name); kinds.Add(kind); points.Add(go.transform); coords.Add(new Vector2((float)cx, (float)cy));
            }
            var comp = SceneSetup.AddUdonAware(group, type);
            int models = 0;
            for (int i = 0; i < points.Count; ++i)
                if (kinds[i] == "waypoint" && DropModel(points[i], mapRoot.transform.position, unityScale, true) != null) ++models;
            if (models == 0 && kinds.Contains("waypoint")) warnings.Add("waypoints: no Prefabs/" + WaypointModel + ".prefab in any export: the points have no waypoint model");
            SceneSetup.SetField(comp, "names", names.ToArray());
            SceneSetup.SetField(comp, "kinds", kinds.ToArray());
            SceneSetup.SetField(comp, "points", points.ToArray());
            AudioClip whoosh = AssetDatabase.LoadAssetAtPath<AudioClip>(root + "/sounds/waypoint/599049.ogg");
            if (whoosh != null)
            {
                var arrive = group.AddComponent<AudioSource>();
                arrive.clip = whoosh; arrive.playOnAwake = false; arrive.spatialBlend = 0f; arrive.volume = 0.7f;
                SceneSetup.SetField(comp, "arriveSound", arrive);
            }
            SceneSetup.CopyProxyToUdon(comp);
            try { WaypointMap.Build(group, comp, root, id, cont, floor, cr, names, kinds, coords, warnings); }
            catch (Exception ex) { warnings.Add("waypoint map: " + (ex.InnerException ?? ex).Message); }
            warnings.Add("waypoints: " + points.Count + " point(s) from the GW2 API (map " + id + ") under " + GroupName + "; _TP<k> teleports for GMMenu buttons");
            return points.Count;
        }

        /// The ground under @p w (x, z), else the water plane. 2026-10-27: a point just off a ledge
        /// (Mad King's Labyrinth's waypoints stand on its corner rock outcrops; the NE one's coordinate
        /// misses its outcrop by a few metres and fell 200 m to the terrain) moves to the nearest probe
        /// (rings of 4..40 m) within 3 m of the highest surface there, when that stands more than LedgeDrop
        /// above the hit. The Labyrinth's two stand on its raised corner plateaus (y 278, with the doors
        /// down into the maze), whose edges their coordinates just miss.
        const float LedgeDrop = 40f;
        static float Ground(float x, float z)
        {
            return Physics.Raycast(new Vector3(x, 3000f, z), Vector3.down, out RaycastHit hit, 6000f, ~0, QueryTriggerInteraction.Ignore) ? hit.point.y : float.NaN;
        }

        static float Stand(ref Vector3 w, float waterY)
        {
            float h = Ground(w.x, w.z);
            float top = float.NaN;
            for (int ring = 1; ring <= 10; ++ring)
                for (int k = 0; k < 32; ++k)
                {
                    float a = k * Mathf.PI / 16f, r = ring * 4f;
                    float g = Ground(w.x + Mathf.Cos(a) * r, w.z + Mathf.Sin(a) * r);
                    if (!float.IsNaN(g) && (float.IsNaN(top) || g > top)) top = g;
                }
            if (!float.IsNaN(top) && (float.IsNaN(h) || top - h > LedgeDrop))
                for (int ring = 1; ring <= 10; ++ring)
                    for (int k = 0; k < 32; ++k)
                    {
                        float a = k * Mathf.PI / 16f, r = ring * 4f;
                        float x = w.x + Mathf.Cos(a) * r, z = w.z + Mathf.Sin(a) * r, g = Ground(x, z);
                        if (float.IsNaN(g) || top - g > 3f) continue;   // on the surface, not its slope
                        w = new Vector3(x, w.y, z);
                        return g;
                    }
            return float.IsNaN(h) ? waterY : h;
        }

        public const long WaypointModel = 106188;
        public const float ArriveOffset = 3.5f;   // metres in front of the shrine

        /// The waypoint model's prefab from any export that has it (null when none does).
        public static GameObject WaypointPrefab()
        {
            foreach (string guid in AssetDatabase.FindAssets(WaypointModel + " t:Prefab"))
            {
                string path = AssetDatabase.GUIDToAssetPath(guid);
                if (path.EndsWith("/Prefabs/" + WaypointModel + ".prefab", StringComparison.Ordinal)) return AssetDatabase.LoadAssetAtPath<GameObject>(path);
            }
            return null;
        }

        /// GW2's waypoint at @p point's ground (its 1 m lift taken off when @p moveArrival), facing
        /// @p centre, under @p point's parent; @p moveArrival puts @p point ArriveOffset in front of it.
        /// Null without the prefab.
        public static GameObject DropModel(Transform point, Vector3 centre, float unityScale, bool moveArrival)
        {
            GameObject prefab = WaypointPrefab();
            if (prefab == null) return null;
            string folder = Path.GetDirectoryName(Path.GetDirectoryName(AssetDatabase.GetAssetPath(prefab))).Replace('\\', '/');
            Vector3 at = point.position - (moveArrival ? Vector3.up : Vector3.zero);
            Vector3 face = centre - at; face.y = 0f;
            Quaternion rot = face.sqrMagnitude > 1e-4f ? Quaternion.LookRotation(face.normalized, Vector3.up) : Quaternion.identity;
            var go = new GameObject("GW2 Waypoint Model");
            go.transform.SetParent(point.parent, true);
            go.transform.SetPositionAndRotation(at, rot);
            Vector3 parentScale = go.transform.parent != null ? go.transform.parent.lossyScale : Vector3.one;
            go.transform.localScale = new Vector3(1f / parentScale.x, 1f / parentScale.y, 1f / parentScale.z);   // metres, under the map root's scale too
            var model = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
            model.transform.SetParent(go.transform, false);
            // model-export's glb roots are mirrored in Y against the world export's (Pumpkins.cs)
            model.transform.localScale = new Vector3(unityScale, -unityScale, unityScale);
            foreach (Renderer r in model.GetComponentsInChildren<Renderer>(true))
                if (r.name.StartsWith("shrinecont", StringComparison.Ordinal)) r.enabled = false;
            foreach (ParticleSystem ps in model.GetComponentsInChildren<ParticleSystem>(true))
                if (ps.name.Contains("Cont")) ps.gameObject.SetActive(false);
            Animator an = model.GetComponent<Animator>();
            if (an == null) an = model.AddComponent<Animator>();
            an.runtimeAnimatorController = LoopController(folder);
            AudioClip hum = AssetDatabase.LoadAssetAtPath<AudioClip>(folder + "/sounds/waypoint/106172.ogg");
            if (hum != null)
            {
                var src = go.AddComponent<AudioSource>();
                src.clip = hum; src.loop = true; src.playOnAwake = true; src.spatialBlend = 1f;
                src.rolloffMode = AudioRolloffMode.Logarithmic; src.minDistance = 2f; src.maxDistance = 25f; src.volume = 0.6f;
            }
            if (moveArrival) point.position = at + rot * Vector3.forward * ArriveOffset + Vector3.up;
            return go;
        }

        /// Controllers/106188_Waypoint.controller: the "on08" loop (the shard turning once and bobbing over
        /// 16.6 s), as a looping copy of the FBX's clip (Controllers/106188_on08_loop.anim): the FBX clip
        /// does not loop, and its __preview__ twin is not the clip.
        static RuntimeAnimatorController LoopController(string folder)
        {
            string path = folder + "/Controllers/" + WaypointModel + "_Waypoint.controller";
            string animPath = folder + "/Controllers/" + WaypointModel + "_on08_loop.anim";
            var existing = AssetDatabase.LoadAssetAtPath<RuntimeAnimatorController>(path);
            if (existing != null && AssetDatabase.LoadAssetAtPath<AnimationClip>(animPath) != null) return existing;
            AnimationClip on = AssetDatabase.LoadAllAssetsAtPath(folder + "/models/" + WaypointModel + "/" + WaypointModel + ".fbx").OfType<AnimationClip>()
                .FirstOrDefault(c => c.name.EndsWith("on08", StringComparison.Ordinal) && !c.name.StartsWith("__preview__", StringComparison.Ordinal));
            if (on == null) return null;
            if (!AssetDatabase.IsValidFolder(folder + "/Controllers")) AssetDatabase.CreateFolder(folder, "Controllers");
            var loop = new AnimationClip { name = WaypointModel + "_on08_loop", frameRate = on.frameRate };
            foreach (EditorCurveBinding b in AnimationUtility.GetCurveBindings(on))
                AnimationUtility.SetEditorCurve(loop, b, AnimationUtility.GetEditorCurve(on, b));
            AnimationClipSettings st = AnimationUtility.GetAnimationClipSettings(on);
            st.loopTime = true;
            AnimationUtility.SetAnimationClipSettings(loop, st);
            if (AssetDatabase.LoadAssetAtPath<AnimationClip>(animPath) != null) AssetDatabase.DeleteAsset(animPath);
            AssetDatabase.CreateAsset(loop, animPath);
            if (existing != null) AssetDatabase.DeleteAsset(path);
            return UnityEditor.Animations.AnimatorController.CreateAnimatorControllerAtPathWithClip(path, loop);
        }

        [MenuItem("Castlemist/Drop GW2 Waypoint Model (at the Scene view)")]
        public static void DropMenu()
        {
            SceneView sv = SceneView.lastActiveSceneView;
            Vector3 c = sv != null ? sv.pivot : Vector3.zero;
            float y = Physics.Raycast(new Vector3(c.x, 3000f, c.z), Vector3.down, out RaycastHit hit, 6000f, ~0, QueryTriggerInteraction.Ignore) ? hit.point.y : c.y;
            var scene = UnityEditor.SceneManagement.EditorSceneManager.GetActiveScene();
            GameObject mapRoot = scene.GetRootGameObjects().FirstOrDefault(g => g.name.StartsWith("GW2_", StringComparison.Ordinal) && !g.name.EndsWith("_Environment", StringComparison.Ordinal));
            float scale = mapRoot != null ? mapRoot.transform.lossyScale.x : 0.0254f;
            var holder = new GameObject("GW2 Waypoint");
            holder.transform.position = new Vector3(c.x, y, c.z);
            GameObject go = DropModel(holder.transform, holder.transform.position + Vector3.forward, scale, false);
            if (go == null)
            {
                UnityEngine.Object.DestroyImmediate(holder);
                EditorUtility.DisplayDialog("GW2 Waypoint", "No export has Prefabs/" + WaypointModel + ".prefab (add model " + WaypointModel + " to one first).", "OK");
                return;
            }
            go.transform.SetParent(null, true);
            UnityEngine.Object.DestroyImmediate(holder);
            Undo.RegisterCreatedObjectUndo(go, "Drop GW2 Waypoint");
            Selection.activeGameObject = go;
            UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(scene);
        }

        static Dictionary<string, object> Get(string url, List<string> warnings)
        {
            using (var r = UnityWebRequest.Get(url))
            {
                var op = r.SendWebRequest();
                var t0 = DateTime.UtcNow;
                while (!op.isDone && (DateTime.UtcNow - t0).TotalSeconds < 20) System.Threading.Thread.Sleep(20);
                if (r.result != UnityWebRequest.Result.Success) { warnings.Add("waypoints: " + url + ": " + r.error); return null; }
                return MiniJson.Parse(r.downloadHandler.text) as Dictionary<string, object>;
            }
        }

        static double[] Rect(Dictionary<string, object> d, string k)
        {
            if (!(d.TryGetValue(k, out object o) && o is List<object> l && l.Count == 2 && l[0] is List<object> a && l[1] is List<object> b)) return null;
            return new[] { Convert.ToDouble(a[0]), Convert.ToDouble(a[1]), Convert.ToDouble(b[0]), Convert.ToDouble(b[1]) };
        }

        static double Num(Dictionary<string, object> d, string k) { return d.TryGetValue(k, out object o) ? Convert.ToDouble(o) : 0; }
        static string Str(Dictionary<string, object> d, string k) { return d.TryGetValue(k, out object o) ? o as string : null; }
    }
}
