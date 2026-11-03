// castlemist world import: Halloween Haunted Doors at their spawn points (2026-10-26). The game spawns them
// (server-side), so the points come from the GW2 wiki's door map ("Haunted Door (Mad King's Labyrinth).jpg",
// WikiMaps.HauntedDoors; part of that map is fogged, so only the doors it shows): green Mad King doors
// and the purple boss doors. Each is a copy of the map's own placed door of that kind (its model, LODs,
// sounds), with Runtime/GW2HauntedDoor and a controller of the model's Lurk / Unlurk / Open / Close clips:
//   Hidden (Lurk held) -Present-> Unlurking -> Closed (Close held) -Open-> Opening -> Opened (Open held)
//   -!Open-> Closing -> Closed -!Present-> Lurking -> Hidden
// The knock is 648540's "latch" sound (no knock in either door's sound script). Each door faces the map
// centre (the wiki map has no facing). 2026-10-27 (the user, from the game): only ~2 doors stand at once;
// when one is sent away another rises elsewhere ~5 min later: Runtime/GW2HauntedDoors on the group picks.
// The click box is door-sized (the model's bounds take in its 20 m dark aura).
// 2026-10-27 (the user): the door takes knocks (everyone hears them) and opens on its handle, for GMs
// first: a GW2DoorHandle on the ring knocker of each face (front: door-local x +1.3 m, 2.3 m up, read
// off a face-on render of 499261; the back mirrored), clickable from GM up (GMMenu's PlayerPermissions;
// HandleMinPermission). Its box stands proud of the knock box on its face, so it is the one a ray hits.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class HauntedDoors
    {
        public const string RootName = "GW2 Haunted Doors";

        sealed class Kind { public long Model; public string Name; public Vector2[] Px; }

        // Icons on the 227 x 240 px door map: green (Mad King) and purple (boss) doors.
        static readonly Dictionary<long, Kind[]> Maps = new Dictionary<long, Kind[]>
        {
            [2499169] = new[]
            {
                new Kind { Model = 499261, Name = "Haunted Door",
                           Px = new[] { new Vector2(124.0f, 70.7f), new Vector2(164.3f, 72.0f), new Vector2(163.3f, 114.3f), new Vector2(88.0f, 142.7f), new Vector2(57.3f, 74.7f) } },
                new Kind { Model = 648540, Name = "Haunted Door (boss)",
                           Px = new[] { new Vector2(169.0f, 180.3f), new Vector2(56.7f, 196.0f), new Vector2(162.7f, 205.3f), new Vector2(117.0f, 213.0f) } },
            },
        };

        [MenuItem("Castlemist/Haunted Doors (knock, open, close, respawn)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "haunted doors: no GW2_<id>_Environment root in the open scene";
            if (!long.TryParse(env.name.Replace("GW2_", "").Replace("_Environment", ""), out long mapId)) return "haunted doors: cannot read the map id";
            if (!Maps.TryGetValue(mapId, out Kind[] kinds)) return "haunted doors: map " + mapId + " has no door spawn points";
            GameObject mapRoot = env.scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + mapId);
            string root = Path.GetDirectoryName(env.scene.path).Replace('\\', '/');
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "haunted doors: UdonSharp is not in this project";
            if (made.Count > 0) return "haunted doors: UdonSharp program assets made: run again once UdonSharp has compiled";
            Type doorType = SceneSetup.FindType("Castlemist.GW2HauntedDoor");
            Type managerType = SceneSetup.FindType("Castlemist.GW2HauntedDoors");
            Type handleType = SceneSetup.FindType("Castlemist.GW2DoorHandle");
            Type permType = SceneSetup.FindType("Sylan.GMMenu.PlayerPermissions");
            Component permissions = permType != null ? Object.FindObjectsOfType(permType, true).Cast<Component>().FirstOrDefault() : null;
            if (doorType == null || managerType == null || handleType == null || mapRoot == null) return "haunted doors: GW2HauntedDoor(s) not compiled, or no GW2_" + mapId + " root";

            Transform old = env.transform.Find(RootName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var group = new GameObject(RootName);
            group.transform.SetParent(env.transform, false);
            Physics.SyncTransforms();
            AudioClip knock = AssetDatabase.LoadAssetAtPath<AudioClip>(root + "/models/648540/sounds/565150.mp3");

            int placed = 0;
            var notes = new List<string>();
            var doors = new List<Component>();
            Component manager = SceneSetup.AddUdonAware(group, managerType);
            foreach (Kind k in kinds)
            {
                GameObject template = mapRoot.GetComponentsInChildren<Transform>(true)
                    .Where(t => t.name.StartsWith(k.Model + "_", StringComparison.Ordinal) && t.GetComponent<Animator>() != null)
                    .Select(t => t.gameObject).FirstOrDefault();
                if (template == null) { notes.Add("no placed " + k.Model + " to copy"); continue; }
                AnimatorController ac = Controller(root, k.Model);
                if (ac == null) { notes.Add(k.Model + ": no Lurk / Unlurk / Open / Close clips"); continue; }
                int n = 0;
                foreach (Vector2 px in k.Px)
                {
                    if (!WikiMaps.ToWorld(mapId, WikiMaps.HauntedDoors, px, mapRoot.transform, out Vector3 at)) continue;
                    float y = WikiMaps.Ground(at.x, at.z);
                    if (!float.IsNaN(y)) at.y = y;
                    // Copied beside the template (under the map root's 0.0254 scale), then moved keeping its
                    // world scale: a copy made at the scene root lost that scale (doors 59x too big).
                    GameObject door = Object.Instantiate(template, template.transform.parent);
                    door.name = k.Name + " " + (++n);
                    door.transform.SetParent(group.transform, true);
                    door.transform.position = at;
                    Vector3 toCentre = mapRoot.transform.position - at; toCentre.y = 0f;
                    if (toCentre.sqrMagnitude > 1e-4f) door.transform.rotation = Quaternion.LookRotation(toCentre.normalized, Vector3.up);
                    Transform prox = door.transform.Find("GW2 Door Trigger");
                    if (prox != null) Object.DestroyImmediate(prox.gameObject);
                    door.GetComponent<Animator>().runtimeAnimatorController = ac;
                    doors.Add(Haunt(door, doorType, handleType, permissions, knock, manager, doors.Count));
                    ++placed;
                }
            }
            Array typed = Array.CreateInstance(doorType, doors.Count);
            for (int i = 0; i < doors.Count; ++i) typed.SetValue(doors[i], i);
            SceneSetup.SetField(manager, "doors", typed);
            SceneSetup.CopyProxyToUdon(manager);
            MapMarkers.Setup();   // 2026-10-27: their event circles on the GMMenu map
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "haunted doors: " + placed + " spawn point(s) from the wiki's door map, 2 standing at once (knock on the door; its handles open and close it, " +
                   (permissions != null ? "GMs only" : "everyone: no GMMenu PlayerPermissions in the scene") + "; another rises 4-6 min later)" +
                   (notes.Count > 0 ? "; " + string.Join("; ", notes) : "");
        }

        static readonly Vector3 ClickBox = new Vector3(4.5f, 6f, 2f);   // metres: the door frame, not its aura
        public const int HandleMinPermission = 2;   // GMMenu: 2 GM, 1 facilitator, 0 everyone
        // door-local (metres; +z the side the door faces): the ring knocker on each face
        static readonly Vector3[] HandleAt = { new Vector3(1.3f, 2.3f, 0.8f), new Vector3(-1.3f, 2.3f, -0.8f) };
        static readonly Vector3 HandleBox = new Vector3(0.8f, 0.9f, 1.0f);   // to 1.3 m out: past the knock box's 1 m

        static Component Haunt(GameObject door, Type doorType, Type handleType, Component permissions, AudioClip knockClip, Component manager, int index)
        {
            var go = new GameObject("GW2 Haunted Door");
            go.transform.SetParent(door.transform, false);
            go.transform.position = door.transform.position + Vector3.up * (ClickBox.y * 0.5f);
            go.transform.rotation = door.transform.rotation;
            go.transform.localScale = new Vector3(1f / door.transform.lossyScale.x, 1f / door.transform.lossyScale.y, 1f / door.transform.lossyScale.z);
            var box = go.AddComponent<BoxCollider>();
            box.isTrigger = true;
            box.size = ClickBox;
            var src = go.AddComponent<AudioSource>();
            src.clip = knockClip; src.playOnAwake = false; src.spatialBlend = 1f; src.minDistance = 2f; src.maxDistance = 30f;
            src.rolloffMode = AudioRolloffMode.Linear;

            Collider[] blockers = door.GetComponentsInChildren<Collider>(true).Where(c => c != box && !c.isTrigger).ToArray();
            Component comp = SceneSetup.AddUdonAware(go, doorType);
            var handles = new List<Component>();
            var handleBoxes = new List<Collider>();
            for (int h = 0; h < HandleAt.Length; ++h)
            {
                var hg = new GameObject("GW2 Door Handle " + (h == 0 ? "(front)" : "(back)"));
                hg.transform.SetParent(go.transform, false);
                hg.transform.localPosition = HandleAt[h] - Vector3.up * (ClickBox.y * 0.5f);   // go sits at the box centre
                var hb = hg.AddComponent<BoxCollider>();
                hb.isTrigger = true;
                hb.size = HandleBox;
                Component hc = SceneSetup.AddUdonAware(hg, handleType);
                SceneSetup.SetField(hc, "door", comp);
                SceneSetup.SetField(hc, "minPermission", HandleMinPermission);
                if (permissions != null) SceneSetup.SetField(hc, "permissions", permissions);
                SceneSetup.CopyProxyToUdon(hc);
                handles.Add(hc);
                handleBoxes.Add(hb);
            }
            Array typedHandles = Array.CreateInstance(handleType, handles.Count);
            for (int h = 0; h < handles.Count; ++h) typedHandles.SetValue(handles[h], h);
            SceneSetup.SetField(comp, "handles", typedHandles);
            SceneSetup.SetField(comp, "handleColliders", handleBoxes.ToArray());
            SceneSetup.SetField(comp, "manager", manager);
            SceneSetup.SetField(comp, "index", index);
            SceneSetup.SetField(comp, "animator", door.GetComponent<Animator>());
            SceneSetup.SetField(comp, "knock", src);
            SceneSetup.SetField(comp, "blockers", blockers);
            SceneSetup.SetField(comp, "clickCollider", box);
            SceneSetup.SetField(comp, "visuals", door.GetComponentsInChildren<Renderer>(true));
            AnimationClip lurk = door.GetComponent<Animator>().runtimeAnimatorController.animationClips.FirstOrDefault(c => c.name.EndsWith("Lurk", StringComparison.Ordinal) && !c.name.EndsWith("Unlurk", StringComparison.Ordinal));
            if (lurk != null) SceneSetup.SetField(comp, "lurkSeconds", lurk.length);
            SceneSetup.CopyProxyToUdon(comp);
            return comp;
        }

        /// Controllers/<model>_Haunted.controller from the model's FBX clips (see the header); null without them.
        static AnimatorController Controller(string root, long model)
        {
            var clips = AssetDatabase.LoadAllAssetsAtPath(root + "/models/" + model + "/" + model + ".fbx").OfType<AnimationClip>()
                .Where(c => !c.name.StartsWith("__preview__", StringComparison.Ordinal)).ToList();
            // the importer's copy with the material and UV curves (WorldImporter.WithConstants) when it made one
            AnimationClip Clip(string n) { return AssetDatabase.LoadAssetAtPath<AnimationClip>(root + "/Controllers/" + model + "_" + n + ".anim") ?? clips.FirstOrDefault(c => c.name == n) ?? clips.FirstOrDefault(c => c.name.EndsWith("|" + n, StringComparison.Ordinal) || c.name.EndsWith(n, StringComparison.Ordinal)); }
            AnimationClip lurk = Clip("Lurk"), unlurk = Clip("Unlurk"), open = Clip("Open"), close = Clip("Close");
            if (lurk == null || unlurk == null || open == null || close == null) return null;
            string path = root + "/Controllers/" + model + "_Haunted.controller";
            if (AssetDatabase.LoadAssetAtPath<AnimatorController>(path) != null) AssetDatabase.DeleteAsset(path);
            var ac = AnimatorController.CreateAnimatorControllerAtPath(path);
            ac.AddParameter(new AnimatorControllerParameter { name = "Present", type = AnimatorControllerParameterType.Bool, defaultBool = true });
            ac.AddParameter("Open", AnimatorControllerParameterType.Bool);
            ac.AddParameter(new AnimatorControllerParameter { name = "EndTime", type = AnimatorControllerParameterType.Float, defaultFloat = 0.999f });
            AnimatorStateMachine sm = ac.layers[0].stateMachine;
            AnimatorState Held(string n, AnimationClip c) { var s = sm.AddState(n); s.motion = c; s.timeParameterActive = true; s.timeParameter = "EndTime"; return s; }
            AnimatorState Play(string n, AnimationClip c) { var s = sm.AddState(n); s.motion = c; return s; }
            AnimatorState hidden = Held("Hidden", lurk), unlurking = Play("Unlurking", unlurk), closed = Held("Closed", close),
                          opening = Play("Opening", open), opened = Held("Opened", open), closing = Play("Closing", close), lurking = Play("Lurking", lurk);
            sm.defaultState = closed;
            void When(AnimatorState a, AnimatorState b, string p, bool v)
            {
                AnimatorStateTransition t = a.AddTransition(b);
                t.AddCondition(v ? AnimatorConditionMode.If : AnimatorConditionMode.IfNot, 0, p);
                t.hasExitTime = false; t.duration = 0f;
            }
            void End(AnimatorState a, AnimatorState b) { AnimatorStateTransition t = a.AddTransition(b); t.hasExitTime = true; t.exitTime = 1f; t.duration = 0f; }
            When(hidden, unlurking, "Present", true);
            End(unlurking, closed);
            When(closed, opening, "Open", true);
            End(opening, opened);
            When(opened, closing, "Open", false);
            When(opening, closing, "Open", false);
            End(closing, closed);
            When(closed, lurking, "Present", false);
            End(lurking, hidden);
            EditorUtility.SetDirty(ac);
            AssetDatabase.SaveAssets();
            return ac;
        }
    }
}
