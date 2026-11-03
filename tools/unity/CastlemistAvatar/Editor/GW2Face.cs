// castlemist character export: GW2's facial animation on an avatar with its own face rig
// (2026-11-03). Castlemist > Avatar > Retarget GW2 Face (selected avatars + export .fbx).
//
// GW2's clips move ~20 face bones by name (JawBone, MouthTop/Bot, CheekUpper, EyelidUpper/Botm,
// brows, nose...). An avatar edited after export (renamed, re-rigged, re-weighted) keeps its own,
// usually smaller, face rig, so that motion has nowhere to go. This moves it across without
// touching the avatar's model:
//   - FaceMap pairs GW2 bones with the avatar's (a bone the avatar lacks is skipped; the eyes are
//     left to VRChat's eye tracking). Left / right follow which side a bone actually sits on.
//   - per clip, each GW2 bone's motion is taken relative to the head, as a change from its rest
//     pose (rotation, and offset scaled by face width: cheek to cheek), and applied to the
//     avatar's bone from its own rest pose. Bone axes may differ between the rigs; only the change
//     carries over, in the head's frame.
//   - the result: face-only clips per avatar, in "<name> GW2 Animator/Face/<avatar>/", and in the
//     locomotion and actions controllers one synced layer per avatar ("GW2 Face <avatar>") that
//     plays the face clip of whatever state the body layer is in, masked to that avatar's face
//     bones. Avatars with different bone paths can share the controllers.
// The rest poses come from the avatars as they stand in the scene (their bind pose).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist.Avatar
{
    public static class GW2Face
    {
        /// GW2 face bone -> the usual names of the matching bone on an edited rig (first found wins).
        static readonly (string gw2, string[] targets)[] FaceMap =
        {
            ("JawBone",      new[] { "Jaw", "JawBone", "jaw" }),
            ("MouthTop",     new[] { "UpperLip", "MouthTop", "Lip.upper", "UpperLip.c" }),
            ("MouthBot",     new[] { "LowerLip", "MouthBot", "Lip.lower", "LowerLip.c" }),
            ("CheekUpperL",  new[] { "Cheek.l", "CheekUpperL", "Cheek_L", "CheekL" }),
            ("CheekUpperR",  new[] { "Cheek.r", "CheekUpperR", "Cheek_R", "CheekR" }),
            ("EyelidUpperL", new[] { "UpperEyelid.l", "EyelidUpperL", "UpperLid.l" }),
            ("EyelidUpperR", new[] { "UpperEyelid.r", "EyelidUpperR", "UpperLid.r" }),
            ("EyelidBotmL",  new[] { "LowerEyelid.l", "EyelidBotmL", "LowerLid.l" }),
            ("EyelidBotmR",  new[] { "LowerEyelid.r", "EyelidBotmR", "LowerLid.r" }),
        };
        const float Fps = 30f;
        const float Moves = 0.002f;  // a clip whose face never changes more than this (rad / m) gets the rest face

        [MenuItem("Castlemist/Avatar/Retarget GW2 Face (selected avatars + export .fbx)")]
        public static void Menu()
        {
            string fbx = Selection.assetGUIDs.Select(AssetDatabase.GUIDToAssetPath).FirstOrDefault(p => GW2Animator.Stances(p).Length > 0);
            var avatars = Selection.gameObjects.Where(g => g.scene.IsValid()).ToArray();
            if (fbx == null || avatars.Length == 0)
            {
                EditorUtility.DisplayDialog("GW2 face", "Select the avatars in the scene AND the exported .fbx in the Project window (ctrl-click), then run this again.", "OK");
                return;
            }
            Debug.Log("CASTLEMIST " + Retarget(fbx, avatars));
        }

        /// Face clips for each avatar from the export's clips, then the face layers on the GW2
        /// controllers beside the export. Returns a one-line report.
        public static string Retarget(string exportFbx, GameObject[] avatars)
        {
            string dir = Path.GetDirectoryName(exportFbx).Replace('\\', '/');
            string stem = Path.GetFileNameWithoutExtension(exportFbx);
            string outDir = dir + "/" + stem + " GW2 Animator";
            GameObject source = AssetDatabase.LoadAssetAtPath<GameObject>(exportFbx);
            var clips = AssetDatabase.LoadAllAssetsAtPath(exportFbx).OfType<AnimationClip>()
                .Where(c => !c.name.StartsWith("__preview__")).GroupBy(c => GW2Animator.TakeName(c.name)).ToDictionary(g => g.Key, g => g.First());
            if (source == null || clips.Count == 0) return "gw2 face: nothing to read in " + exportFbx;
            if (!clips.Values.Any(c => AnimationUtility.GetCurveBindings(c).Any(b => FaceMap.Any(f => b.path.EndsWith("/" + f.gw2)))))
                return "gw2 face: the export's clips carry no face curves (rebuild the locomotion first: it sets the import mask)";

            var reports = new List<string>();
            foreach (GameObject avatar in avatars)
                reports.Add(ForAvatar(avatar, source, clips, outDir));
            foreach (string ctl in new[] { stem + " GW2 Locomotion.controller", stem + " GW2 Actions.controller" })
            {
                var ac = AssetDatabase.LoadAssetAtPath<AnimatorController>(outDir + "/" + ctl);
                if (ac != null) AttachLayers(ac, outDir);
            }
            AssetDatabase.SaveAssets();
            return "gw2 face: " + string.Join("; ", reports);
        }

        // One avatar: its face bones, rest poses, and a face clip per GW2 clip.
        static string ForAvatar(GameObject avatar, GameObject source, Dictionary<string, AnimationClip> clips, string outDir)
        {
            Transform root = avatar.GetComponent<Animator>() ? avatar.transform : avatar.transform;
            Transform[] all = avatar.GetComponentsInChildren<Transform>(true);

            // Rest poses, measured on copies in a preview scene so nothing in the open scene moves.
            var scene = UnityEditor.SceneManagement.EditorSceneManager.NewPreviewScene();
            try
            {
                GameObject src = (GameObject)Object.Instantiate(source);
                UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(src, scene);
                GameObject dst = (GameObject)Object.Instantiate(avatar);
                UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(dst, scene);
                dst.transform.SetPositionAndRotation(avatar.transform.position, avatar.transform.rotation);
                foreach (var b in dst.GetComponentsInChildren<Behaviour>(true)) if (!(b is Animator)) b.enabled = false;

                Transform Find(GameObject g, string n) => g.GetComponentsInChildren<Transform>(true).FirstOrDefault(t => t.name == n);
                Transform srcHead = src.GetComponent<Animator>()?.GetBoneTransform(HumanBodyBones.Head) ?? Find(src, "Head");
                Transform dstHeadInScene = avatar.GetComponent<Animator>() && avatar.GetComponent<Animator>().isHuman
                    ? avatar.GetComponent<Animator>().GetBoneTransform(HumanBodyBones.Head) : all.FirstOrDefault(t => t.name == "Head");
                if (srcHead == null || dstHeadInScene == null) return avatar.name + ": no head bone";

                // The pairs that exist on both sides, with each side's own copy of the bone.
                var pairs = new List<(Transform s, Transform d, string path)>();
                foreach (var (gw2, targets) in FaceMap)
                {
                    Transform s = Find(src, gw2);
                    Transform dScene = targets.Select(n => all.FirstOrDefault(t => t.name == n && t.IsChildOf(dstHeadInScene))).FirstOrDefault(t => t != null);
                    if (s == null || dScene == null) continue;
                    string path = AnimationUtility.CalculateTransformPath(dScene, root);
                    Transform d = dst.transform.Find(path);
                    if (d != null) pairs.Add((s, d, path));
                }
                if (pairs.Count == 0) return avatar.name + ": no matching face bones";
                Transform dstHead = dst.transform.Find(AnimationUtility.CalculateTransformPath(dstHeadInScene, root));

                // A frame both rigs agree on, whatever their bone axes: the face's forward (head joint
                // toward the face bones, level) and world up.
                Quaternion FaceFrame(Transform head, IEnumerable<Transform> bones)
                {
                    Vector3 mid = Vector3.zero; int c = 0;
                    foreach (var b in bones) { mid += b.position; c++; }
                    Vector3 fwd = mid / Mathf.Max(1, c) - head.position; fwd.y = 0;
                    return Quaternion.LookRotation(fwd.sqrMagnitude > 1e-8f ? fwd.normalized : Vector3.forward, Vector3.up);
                }
                Quaternion srcFrame = FaceFrame(srcHead, pairs.Select(p => p.s)), dstFrame = FaceFrame(dstHead, pairs.Select(p => p.d));

                // Left / right: GW2's "L" is the character's left. If the avatar's matching bone sits
                // on the other side of its face, the pair swaps with its opposite.
                var swapped = new HashSet<string>();
                for (int i = 0; i < pairs.Count; i++)
                {
                    var p = pairs[i];
                    float ss = Vector3.Dot(p.s.position - srcHead.position, srcFrame * Vector3.right);
                    float ds = Vector3.Dot(p.d.position - dstHead.position, dstFrame * Vector3.right);
                    if (Mathf.Abs(ss) < 0.002f || Mathf.Abs(ds) < 0.002f || Mathf.Sign(ss) == Mathf.Sign(ds)) continue;
                    string opposite = p.s.name.EndsWith("L") ? p.s.name.Substring(0, p.s.name.Length - 1) + "R" : p.s.name.EndsWith("R") ? p.s.name.Substring(0, p.s.name.Length - 1) + "L" : null;
                    Transform os = opposite != null ? Find(src, opposite) : null;
                    if (os != null) { pairs[i] = (os, p.d, p.path); swapped.Add(p.path); }
                }

                // Scale: face width, cheek to cheek, on each side.
                float Width(IEnumerable<Transform> bones)
                {
                    var b = bones.ToList();
                    return b.Count < 2 ? 0 : b.Max(x => Vector3.Distance(x.position, b[0].position));
                }
                float sw = Width(pairs.Select(p => p.s)), dw = Width(pairs.Select(p => p.d));
                float scale = sw > 1e-4f && dw > 1e-4f ? dw / sw : 1f;

                // Rest, in each head's frame (source) and in the avatar's own world (target).
                var srcRestRot = pairs.Select(p => Quaternion.Inverse(srcHead.rotation) * p.s.rotation).ToArray();
                var srcRestPos = pairs.Select(p => srcHead.InverseTransformVector(p.s.position - srcHead.position)).ToArray();
                Quaternion srcHeadRest = srcHead.rotation;
                var dstRestRot = pairs.Select(p => p.d.rotation).ToArray();
                var dstRestPos = pairs.Select(p => p.d.position).ToArray();
                var dstRestLocal = pairs.Select(p => (p.d.localPosition, p.d.localRotation)).ToArray();
                Quaternion dstHeadRest = dstHead.rotation;

                string folder = outDir + "/Face";
                if (!AssetDatabase.IsValidFolder(folder)) AssetDatabase.CreateFolder(outDir, "Face");
                string safe = string.Join("_", avatar.name.Split(Path.GetInvalidFileNameChars()));
                string avDir = folder + "/" + safe;
                if (!AssetDatabase.IsValidFolder(avDir)) AssetDatabase.CreateFolder(folder, safe);

                // The rest face (for states whose clip moves no face) and the mask, once per avatar.
                var restClip = new AnimationClip { name = "Rest" };
                for (int i = 0; i < pairs.Count; i++) Keys(restClip, pairs[i].path, new[] { 0f, 1f / Fps }, new[] { dstRestLocal[i].localPosition, dstRestLocal[i].localPosition }, new[] { dstRestLocal[i].localRotation, dstRestLocal[i].localRotation });
                Save(restClip, avDir + "/_Rest.anim");
                var mask = new AvatarMask();
                for (int b = 0; b < (int)AvatarMaskBodyPart.LastBodyPart; b++) mask.SetHumanoidBodyPartActive((AvatarMaskBodyPart)b, false);
                mask.transformCount = pairs.Count;
                for (int i = 0; i < pairs.Count; i++) { mask.SetTransformPath(i, pairs[i].path); mask.SetTransformActive(i, true); }
                Save(mask, avDir + "/_Mask.mask");

                int moving = 0;
                foreach (var kv in clips)
                {
                    AnimationClip clip = kv.Value;
                    int n = Mathf.Max(2, Mathf.CeilToInt(clip.length * Fps) + 1);
                    var times = new float[n];
                    var pos = new Vector3[pairs.Count][];
                    var rot = new Quaternion[pairs.Count][];
                    for (int i = 0; i < pairs.Count; i++) { pos[i] = new Vector3[n]; rot[i] = new Quaternion[n]; }
                    float change = 0;
                    for (int k = 0; k < n; k++)
                    {
                        float t = clip.length * k / (n - 1);
                        times[k] = t;
                        clip.SampleAnimation(src, t);
                        // Put the target at rest, then pose each mapped bone (parents first: FaceMap
                        // lists the jaw before the lip under it) and read back its local values.
                        for (int i = 0; i < pairs.Count; i++) { pairs[i].d.localPosition = dstRestLocal[i].localPosition; pairs[i].d.localRotation = dstRestLocal[i].localRotation; }
                        for (int i = 0; i < pairs.Count; i++)
                        {
                            Quaternion rel = Quaternion.Inverse(srcHead.rotation) * pairs[i].s.rotation;
                            Quaternion deltaHead = rel * Quaternion.Inverse(srcRestRot[i]);              // in the source head's axes
                            Quaternion deltaWorld = srcHeadRest * deltaHead * Quaternion.Inverse(srcHeadRest);  // the same turn, world axes, head at rest
                            Vector3 offHead = srcHead.InverseTransformVector(pairs[i].s.position - srcHead.position) - srcRestPos[i];
                            Vector3 offWorld = srcHead.parent != null ? srcHeadRest * Vector3.Scale(offHead, srcHead.lossyScale) : offHead;
                            change = Mathf.Max(change, Quaternion.Angle(Quaternion.identity, deltaHead) * Mathf.Deg2Rad, offWorld.magnitude * scale);
                            // From the source face frame to the avatar's (the avatar's head is at rest in this copy).
                            Quaternion map = dstFrame * Quaternion.Inverse(srcFrame);
                            pairs[i].d.rotation = (map * deltaWorld * Quaternion.Inverse(map)) * dstRestRot[i];
                            pairs[i].d.position = dstRestPos[i] + map * offWorld * scale;
                            pos[i][k] = pairs[i].d.localPosition;
                            rot[i][k] = pairs[i].d.localRotation;
                        }
                    }
                    string path = avDir + "/" + kv.Key + ".anim";
                    if (change < Moves) { AssetDatabase.DeleteAsset(path); continue; }
                    var face = new AnimationClip { name = kv.Key };
                    for (int i = 0; i < pairs.Count; i++) Keys(face, pairs[i].path, times, pos[i], rot[i]);
                    var st = AnimationUtility.GetAnimationClipSettings(face);
                    st.loopTime = clip.isLooping;
                    AnimationUtility.SetAnimationClipSettings(face, st);
                    Save(face, path);
                    moving++;
                }
                return avatar.name + ": " + pairs.Count + " bones (" + string.Join(",", pairs.Select(p => p.s.name + ">" + p.d.name)) + ")" +
                       (swapped.Count > 0 ? ", sides swapped" : "") + ", face scale " + scale.ToString("F2") + ", " + moving + " clips move the face";
            }
            finally { UnityEditor.SceneManagement.EditorSceneManager.ClosePreviewScene(scene); }
        }

        static void Keys(AnimationClip clip, string path, float[] times, Vector3[] pos, Quaternion[] rot)
        {
            var c = new AnimationCurve[7];
            for (int j = 0; j < 7; j++) c[j] = new AnimationCurve();
            for (int k = 0; k < times.Length; k++)
            {
                if (k > 0 && Quaternion.Dot(rot[k], rot[k - 1]) < 0) rot[k] = new Quaternion(-rot[k].x, -rot[k].y, -rot[k].z, -rot[k].w);  // no flips
                c[0].AddKey(times[k], pos[k].x); c[1].AddKey(times[k], pos[k].y); c[2].AddKey(times[k], pos[k].z);
                c[3].AddKey(times[k], rot[k].x); c[4].AddKey(times[k], rot[k].y); c[5].AddKey(times[k], rot[k].z); c[6].AddKey(times[k], rot[k].w);
            }
            string[] props = { "m_LocalPosition.x", "m_LocalPosition.y", "m_LocalPosition.z", "m_LocalRotation.x", "m_LocalRotation.y", "m_LocalRotation.z", "m_LocalRotation.w" };
            for (int j = 0; j < 7; j++) AnimationUtility.SetEditorCurve(clip, EditorCurveBinding.FloatCurve(path, typeof(Transform), props[j]), c[j]);
        }

        // Kept in place when it exists (controllers point at it by GUID).
        static void Save(Object asset, string path)
        {
            Object old = AssetDatabase.LoadMainAssetAtPath(path);
            if (old != null && old.GetType() == asset.GetType()) { EditorUtility.CopySerialized(asset, old); EditorUtility.SetDirty(old); }
            else { if (old != null) AssetDatabase.DeleteAsset(path); AssetDatabase.CreateAsset(asset, path); }
        }

        /// One synced face layer per avatar under outDir/Face: each state of the body layer plays
        /// its clip's face clip (a blend tree: its first child's; none: the rest face). Called
        /// after every controller build, so a rebuild keeps them.
        public static void AttachLayers(AnimatorController ac, string outDir)
        {
            string folder = outDir + "/Face";
            if (!AssetDatabase.IsValidFolder(folder)) return;
            var states = new List<AnimatorState>();
            void Collect(AnimatorStateMachine sm) { foreach (var c in sm.states) states.Add(c.state); foreach (var s in sm.stateMachines) Collect(s.stateMachine); }
            Collect(ac.layers[0].stateMachine);
            var layers = ac.layers.Where(l => !l.name.StartsWith("GW2 Face ")).ToList();
            foreach (string avDir in AssetDatabase.GetSubFolders(folder))
            {
                var mask = AssetDatabase.LoadAssetAtPath<AvatarMask>(avDir + "/_Mask.mask");
                var rest = AssetDatabase.LoadAssetAtPath<AnimationClip>(avDir + "/_Rest.anim");
                if (mask == null || rest == null) continue;
                var faces = AssetDatabase.FindAssets("t:AnimationClip", new[] { avDir }).Select(AssetDatabase.GUIDToAssetPath)
                    .Select(AssetDatabase.LoadAssetAtPath<AnimationClip>).Where(c => c != null && c != rest).ToDictionary(c => c.name, c => c);
                Motion FaceOf(Motion m)
                {
                    if (m is BlendTree bt) return bt.children.Length > 0 ? FaceOf(bt.children[0].motion) : rest;
                    if (m is AnimationClip c && faces.TryGetValue(GW2Animator.TakeName(c.name), out AnimationClip f)) return f;
                    return rest;
                }
                var sm = new AnimatorStateMachine { name = "GW2 Face " + Path.GetFileName(avDir), hideFlags = HideFlags.HideInHierarchy };
                AssetDatabase.AddObjectToAsset(sm, ac);
                var layer = new AnimatorControllerLayer
                {
                    name = "GW2 Face " + Path.GetFileName(avDir), stateMachine = sm, avatarMask = mask, defaultWeight = 1f,
                    blendingMode = AnimatorLayerBlendingMode.Override, syncedLayerIndex = 0, syncedLayerAffectsTiming = false,
                };
                foreach (AnimatorState s in states) layer.SetOverrideMotion(s, FaceOf(s.motion));
                layers.Add(layer);
            }
            ac.layers = layers.ToArray();
            EditorUtility.SetDirty(ac);
        }
    }
}
