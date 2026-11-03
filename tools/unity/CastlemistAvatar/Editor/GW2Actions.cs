// castlemist character export: GW2 emotes as the VRChat Action layer (2026-11-02). Built by
// Castlemist > Avatar > Make GW2 Parameters and Menu (and Add GW2 to Selected Avatar), from the
// emotes the export's .animator.json lists (the emoc file's player emotes for the race, with
// GW2's blend times and intro / loop / outro lengths; src/ripper/animator.cpp). Needs the VRChat
// Avatars SDK (the Action layer is blended in and out by its behaviours).
//
// "<name> GW2 Actions.controller", Int GW2Emote (1..n in menu order):
//   Wait -> Begin    an emote is picked: the Action layer blends up and the body follows the
//                    animation instead of tracking (VRChat's emote hand-off)
//   a looping emote: Intro, then Loop while GW2Emote holds it, then Outro (GW2's three clips,
//                    variants 04 / 08 / 12 of the emote's name)
//   a one-shot:      its clip once, to the end (a menu button; letting go does not cut it)
//   any emote ends early when the avatar starts moving, as GW2 cancels one
//   End              the layer blends back down (GW2's blend out) and tracking returns; back to
//                    Wait once GW2Emote is cleared, or straight into the next emote picked
#if CASTLEMIST_VRC_AVATARS
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;
using VRC.SDK3.Avatars.Components;
using VRC.SDKBase;

namespace Castlemist.Avatar
{
    public static class GW2Actions
    {
        public const string EmoteParam = "GW2Emote";
        const float MovingCancels = 0.6f;  // m/s: moving this fast ends an emote

        /// Builds (in place) the Action controller of an export; null when it has no emotes.
        public static AnimatorController Build(string assetPath, out string report)
        {
            GW2Animator.EmoteInfo[] emotes = GW2Animator.Emotes(assetPath);
            report = "gw2 actions: no emotes in the plan (re-export with character-vrchat --animator)";
            if (emotes.Length == 0) return null;
            string dir = Path.GetDirectoryName(assetPath).Replace('\\', '/');
            string stem = Path.GetFileNameWithoutExtension(assetPath);
            if (stem.EndsWith(".animator")) stem = stem.Substring(0, stem.Length - ".animator".Length);
            string fbx = dir + "/" + stem + ".fbx";
            string outDir = dir + "/" + stem + " GW2 Animator";
            if (!AssetDatabase.IsValidFolder(outDir)) AssetDatabase.CreateFolder(dir, stem + " GW2 Animator");

            var clips = AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<AnimationClip>()
                .Where(c => !c.name.StartsWith("__preview__"))
                .GroupBy(c => GW2Animator.TakeName(c.name)).ToDictionary(g => g.Key, g => g.First());

            string path = outDir + "/" + stem + " GW2 Actions.controller";
            AnimatorController ac = AssetDatabase.LoadAssetAtPath<AnimatorController>(path);
            if (ac == null) ac = AnimatorController.CreateAnimatorControllerAtPath(path);
            else
            {
                foreach (Object sub in AssetDatabase.LoadAllAssetsAtPath(path))
                    if (sub != null && sub != ac) AssetDatabase.RemoveObjectFromAsset(sub);
                ac.layers = new AnimatorControllerLayer[0];
                ac.parameters = new AnimatorControllerParameter[0];
                ac.AddLayer("Action");
            }
            ac.AddParameter(EmoteParam, AnimatorControllerParameterType.Int);
            ac.AddParameter("VelocityMagnitude", AnimatorControllerParameterType.Float);
            AnimatorStateMachine sm = ac.layers[0].stateMachine;

            AnimatorState wait = sm.AddState("Wait", new Vector3(0, 0));
            sm.defaultState = wait;
            AnimatorState begin = sm.AddState("Begin", new Vector3(250, 0));
            AnimatorState end = sm.AddState("End", new Vector3(250, 200));
            foreach (AnimatorState s in new[] { wait, begin, end }) s.writeDefaultValues = false;
            Hand(begin, toAnimation: true, blend: 0.25f);
            Hand(end, toAnimation: false, blend: 0.25f);
            var go = T(wait, begin, 0); go.AddCondition(AnimatorConditionMode.NotEqual, 0, EmoteParam);
            var rest = T(end, wait, 0); rest.AddCondition(AnimatorConditionMode.Equals, 0, EmoteParam);
            rest.hasExitTime = true; rest.exitTime = 0.5f;

            int made = 0;
            var missing = new List<string>();
            for (int i = 0; i < emotes.Length; i++)
            {
                GW2Animator.EmoteInfo e = emotes[i];
                int value = i + 1;
                Vector3 at = new Vector3(550 + 300 * (i % 4), -400 + 100 * (i / 4));
                var chain = new List<AnimatorState>();
                if (GW2Animator.Loops(e))
                {
                    AnimationClip intro = Get(clips, e.intro, missing), loop = Get(clips, e.loop, missing), outro = Get(clips, e.outro, missing);
                    if (loop == null) continue;
                    AnimatorState si = intro != null ? Add(sm, e.label + " Intro", intro, at) : null;
                    AnimatorState sl = Add(sm, e.label, loop, at + new Vector3(0, 30));
                    AnimatorState so = outro != null ? Add(sm, e.label + " Outro", outro, at + new Vector3(0, 60)) : null;
                    var into = T(begin, si ?? sl, e.blendIn); into.AddCondition(AnimatorConditionMode.Equals, value, EmoteParam);
                    if (si != null) { var on = T(si, sl, 0.1f); on.hasExitTime = true; on.exitTime = 1f; }
                    var stop = T(sl, so ?? end, so != null ? 0.1f : e.blendOut); stop.AddCondition(AnimatorConditionMode.NotEqual, value, EmoteParam);
                    if (so != null) { var off = T(so, end, e.blendOut); off.hasExitTime = true; off.exitTime = 1f; }
                    chain.AddRange(new[] { si, sl }.Where(x => x != null));
                }
                else
                {
                    AnimationClip c = Get(clips, e.intro, missing);
                    if (c == null) continue;
                    AnimatorState s = Add(sm, e.label, c, at);
                    var into = T(begin, s, e.blendIn); into.AddCondition(AnimatorConditionMode.Equals, value, EmoteParam);
                    // A one-shot (a menu button) plays to its end: letting go of the button must not cut it.
                    var done = T(s, end, e.blendOut); done.hasExitTime = true; done.exitTime = 1f;
                    chain.Add(s);
                }
                foreach (AnimatorState s in chain)
                {
                    var moving = T(s, end, 0.2f);
                    moving.AddCondition(AnimatorConditionMode.Greater, MovingCancels, "VelocityMagnitude");
                }
                made++;
            }
            // Picking another emote from the end: straight into it (not while still moving).
            var next = T(end, begin, 0); next.AddCondition(AnimatorConditionMode.NotEqual, 0, EmoteParam);
            next.AddCondition(AnimatorConditionMode.Less, MovingCancels, "VelocityMagnitude");
            // Begin with nothing to play (GW2Emote names no emote): hand back.
            var none = T(begin, end, 0.1f); none.hasExitTime = true; none.exitTime = 0.5f;
            GW2Face.AttachLayers(ac, outDir);

            EditorUtility.SetDirty(ac);
            AssetDatabase.SaveAssets();
            report = "gw2 actions: " + path + " -- " + made + " emotes" + (missing.Count > 0 ? ", missing clips: " + string.Join(", ", missing) : "");
            return ac;
        }

        // VRChat's hand-off: the Action layer's weight and which body parts follow the animation.
        static void Hand(AnimatorState s, bool toAnimation, float blend)
        {
            var layer = s.AddStateMachineBehaviour<VRCPlayableLayerControl>();
            layer.layer = VRC_PlayableLayerControl.BlendableLayer.Action;
            layer.goalWeight = toAnimation ? 1f : 0f;
            layer.blendDuration = blend;
            var track = s.AddStateMachineBehaviour<VRCAnimatorTrackingControl>();
            var mode = toAnimation ? VRC_AnimatorTrackingControl.TrackingType.Animation : VRC_AnimatorTrackingControl.TrackingType.Tracking;
            track.trackingHead = track.trackingLeftHand = track.trackingRightHand = track.trackingHip = mode;
            track.trackingLeftFoot = track.trackingRightFoot = track.trackingLeftFingers = track.trackingRightFingers = mode;
            track.trackingEyes = track.trackingMouth = VRC_AnimatorTrackingControl.TrackingType.NoChange;
        }

        static AnimationClip Get(Dictionary<string, AnimationClip> clips, string name, List<string> missing)
        {
            if (string.IsNullOrEmpty(name)) return null;
            if (clips.TryGetValue(name, out AnimationClip c)) return c;
            missing.Add(name);
            return null;
        }

        static AnimatorState Add(AnimatorStateMachine sm, string name, Motion m, Vector3 at)
        {
            AnimatorState s = sm.AddState(name, at);
            s.motion = m;
            s.writeDefaultValues = false;
            return s;
        }

        static AnimatorStateTransition T(AnimatorState from, AnimatorState to, float seconds)
        {
            AnimatorStateTransition t = from.AddTransition(to);
            t.hasExitTime = false;
            t.hasFixedDuration = true;
            t.duration = Mathf.Max(0f, seconds);
            return t;
        }
    }
}
#endif
