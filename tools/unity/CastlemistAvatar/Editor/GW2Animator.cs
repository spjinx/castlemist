// castlemist character export: GW2 locomotion as a VRChat Base-layer controller (2026-11-02).
// Castlemist > Avatar > Build GW2 Locomotion, with the exported avatar .fbx (or its .animator.json)
// selected in the Project window.
//
// `gw2dat_cli character-vrchat ... --animator --stance u,w,g` writes <name>.animator.json beside
// the avatar (version 2): per stance, the race's GW2 animation machine resolved to clips -- states
// and their clip, GW2's crossfade times, transitions with their transition clips (starts, stops,
// landings), the yaw a diagonal state turns its straight clip by, each clip's authored ground speed
// in m/s, and the stance's draw / stow clips (src/ripper/animator.cpp,
// docs/research/gw2-anim-blend-trees.md). The clips are Humanoid, so the controller this builds
// drives any humanoid avatar, not only the exported one.
//
// Layout of "<name> GW2 Locomotion.controller":
//   one sub-state machine per stance, chosen by the Int GW2Stance (0, 1, 2... in --stance order):
//     Draw        a weapon stance's draw clip, played on entering the stance
//     Idle        the stance's idle (a weapon stance idles in GW2's "Alert", weapon ready)
//     Fidget      after a few idle loops
//     Start F/L/R GW2's idle -> run start clips, by the direction you set off in
//     Move F/L/R/B one shared 2D freeform directional tree on (VelocityX, VelocityZ): every walk,
//                 run and sprint clip at its direction x its authored speed, diagonals as the
//                 straight clip turned by GW2's yaw (baked copies). Four states over one tree, so a
//                 stop knows its direction; switching between them crossfades like GW2 does.
//     Stop F/L/R/B GW2's run -> idle stop clips
//     Turn L/R    turning in place
//     Jump, Fall  directional trees of GW2's jump / fall (the straight clip turned, as in GW2)
//     Land        GW2's landing clip, standing or moving
//   Water Surface (Bool GW2Swim) and Water Under (Bool GW2Dive) at the top: GW2's surface
//   breaststroke and underwater swim, directional trees again.
// Every transition takes GW2's own time (the transition's blendIn, else the target state's).
// Loop: off for clips GW2 flags 0x10 and for every transition clip -- inferred from which clips
// carry the flag, not read from the client.
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
    public static class GW2Animator
    {
        public const string StanceParam = "GW2Stance";
        public const string SwimParam = "GW2Swim";
        public const string DiveParam = "GW2Dive";
        public const string MoveParam = "GW2Move";
        const int GlideValue = 18;      // Moves: Glide, the one move that keeps Jump / Fall away

        /// One GW2Move value: what the menu sets, what it plays. `hold`: plays while the value stays
        /// (a menu toggle), then `recover`; otherwise once (a menu button). `dirs`: the GW2 state
        /// prefix of a directional move (picked by the velocity it starts at).
        public class MoveDef
        {
            public int value; public string label, menu, state, recover, follow; public bool hold, dirs, air;
        }
        public static readonly MoveDef[] Moves =
        {
            new MoveDef { value = 1,  label = "Dodge",      menu = "Moves",  state = "Ground Evade", dirs = true },
            new MoveDef { value = 2,  label = "Slide",      menu = "Moves",  state = "Ground Slide", dirs = true, hold = true },
            new MoveDef { value = 3,  label = "Kneel",      menu = "Moves",  state = "Ground Kneel", hold = true },
            new MoveDef { value = 4,  label = "Pick Up",    menu = "Moves",  state = "Ground Pickup Small" },
            new MoveDef { value = 5,  label = "Threaten",   menu = "Moves",  state = "Ground Threaten" },
            new MoveDef { value = 6,  label = "Blowout",    menu = "Moves",  state = "Ground Blowout" },
            new MoveDef { value = 17, label = "Limp",       menu = "Moves",  state = "Ground Limp", hold = true },
            new MoveDef { value = 18, label = "Glide",      menu = "Moves",  state = "Air Glide", hold = true, air = true },
            new MoveDef { value = 7,  label = "Hit",        menu = "Combat", state = "Ground Hit Large" },
            new MoveDef { value = 8,  label = "Knockback",  menu = "Combat", state = "Ground Knockback Back" },
            new MoveDef { value = 9,  label = "Knockdown",  menu = "Combat", state = "Ground Knockdown Back", hold = true, recover = "Ground Recover Back" },
            new MoveDef { value = 10, label = "Stun",       menu = "Combat", state = "Ground Stun", hold = true },
            new MoveDef { value = 11, label = "Downed",     menu = "Combat", state = "Ground Downed", follow = "Ground Downed Alert", hold = true, recover = "Ground Recover Downed" },
            new MoveDef { value = 12, label = "Death",      menu = "Combat", state = "Ground Death Physical", hold = true, recover = "Ground Recover Downed" },
            new MoveDef { value = 13, label = "Death: Fire",      menu = "Deaths", state = "Ground Death Fire", hold = true, recover = "Ground Recover Downed" },
            new MoveDef { value = 14, label = "Death: Ice",       menu = "Deaths", state = "Ground Death Ice", hold = true, recover = "Ground Recover Downed" },
            new MoveDef { value = 15, label = "Death: Lightning", menu = "Deaths", state = "Ground Death Lightning", hold = true, recover = "Ground Recover Downed" },
            new MoveDef { value = 16, label = "Death: Choking",   menu = "Deaths", state = "Ground Death Choking", hold = true, recover = "Ground Recover Downed" },
        };

        const float Still = 0.15f;      // m/s: below this the avatar stands
        const float Go = 1.0f;          // m/s along an axis: a start or a direction state picks it
        const float TurnRate = 0.6f;    // AngularY past this turns in place
        const float JumpUp = 0.6f;      // VelocityY above this, off the ground: a jump
        const float FallDown = -2.0f;   // VelocityY below this, off the ground: falling
        const long OnceFlag = 0x10;     // PlayAnimation.flags bit carried by every one-shot clip

        [Serializable] class Plan { public string format; public int version; public string race; public string units; public StancePlan[] stances; public EmoteInfo[] emotes; }
        /// One player emote of the plan: its clips (GW2 keeps an emote's intro, loop and outro as
        /// three clips; a one-shot has only an intro) and GW2's blend times (seconds).
        [Serializable] public class EmoteInfo { public string label, intro, loop, outro; public float blendIn, blendOut; }

        /// The emotes an export carries, in menu order (empty for a plan without them).
        public static EmoteInfo[] Emotes(string assetPath) => Load(assetPath, out _, out _, out _)?.emotes ?? new EmoteInfo[0];

        public static bool Loops(EmoteInfo e) => !string.IsNullOrEmpty(e.loop);
        [Serializable] class StancePlan { public string stance; public ClipInfo[] clips; public StateInfo[] states; public TransInfo[] transitions; public string[] missing; public Extras extras; }
        [Serializable] class ClipInfo { public string name; public float duration; public float moveSpeed; }
        [Serializable] class StateInfo { public string name; public string clip; public float blendIn; public long flags; public float yawDegrees; }
        [Serializable] class TransInfo { public string from; public string to; public string @event; public string clip; public float blendIn; public float blendOut; }
        [Serializable] class Extras { public string draw; public string stow; }

        /// What a stance letter is, for state machine and menu names.
        public static string StanceLabel(string letter)
        {
            switch (letter)
            {
                case "u": return "Unarmed";
                case "w": return "One-handed";
                case "g": return "Greatsword";
                case "h": return "Hammer";
                case "s": return "Staff";
                default: return "Stance " + letter;
            }
        }

        [MenuItem("Castlemist/Avatar/Build GW2 Locomotion (selected avatar .fbx)")]
        public static void Menu()
        {
            string path = Selection.activeObject != null ? AssetDatabase.GetAssetPath(Selection.activeObject) : null;
            Debug.Log("CASTLEMIST " + Build(path, out _));
        }

        /// The stance letters an export carries, in GW2Stance order.
        public static string[] Stances(string assetPath)
        {
            Plan plan = Load(assetPath, out _, out _, out _);
            return plan?.stances?.Select(s => s.stance).ToArray() ?? new string[0];
        }

        static Plan Load(string assetPath, out string dir, out string stem, out string fbxPath)
        {
            dir = stem = fbxPath = null;
            if (string.IsNullOrEmpty(assetPath)) return null;
            dir = Path.GetDirectoryName(assetPath).Replace('\\', '/');
            stem = Path.GetFileNameWithoutExtension(assetPath);
            if (stem.EndsWith(".animator")) stem = stem.Substring(0, stem.Length - ".animator".Length);
            fbxPath = dir + "/" + stem + ".fbx";
            string jsonPath = dir + "/" + stem + ".animator.json";
            if (!File.Exists(jsonPath)) return null;
            var plan = JsonUtility.FromJson<Plan>(File.ReadAllText(jsonPath));
            return plan != null && plan.format == "castlemist-animator" && plan.version >= 2 ? plan : null;
        }

        /// Builds the controller for an avatar .fbx or its .animator.json; returns a one-line report.
        public static string Build(string assetPath, out AnimatorController built)
        {
            built = null;
            Plan plan = Load(assetPath, out string dir, out string stem, out string fbxPath);
            if (plan == null) return "gw2 locomotion: select an avatar .fbx exported with character-vrchat --animator (version 2 plan beside it)";
            if (!File.Exists(fbxPath)) return "gw2 locomotion: no " + fbxPath;
            if (plan.stances == null || plan.stances.Length == 0) return "gw2 locomotion: the plan has no stances";

            SetImport(fbxPath, plan);
            var clips = AssetDatabase.LoadAllAssetsAtPath(fbxPath).OfType<AnimationClip>()
                .Where(c => !c.name.StartsWith("__preview__"))
                .GroupBy(c => TakeName(c.name)).ToDictionary(g => g.Key, g => g.First());
            if (!clips.Values.Any(c => c.humanMotion))
                return "gw2 locomotion: " + fbxPath + " has no humanoid clips (Rig > Animation Type: Humanoid, then Apply)";

            string outDir = dir + "/" + stem + " GW2 Animator";
            if (!AssetDatabase.IsValidFolder(outDir)) AssetDatabase.CreateFolder(dir, stem + " GW2 Animator");
            string path = outDir + "/" + stem + " GW2 Locomotion.controller";
            var ac = Fresh(path);
            foreach (var p in new[] { "VelocityX", "VelocityY", "VelocityZ", "VelocityMagnitude", "AngularY" })
                ac.AddParameter(p, AnimatorControllerParameterType.Float);
            ac.AddParameter("Grounded", AnimatorControllerParameterType.Bool);
            ac.AddParameter(StanceParam, AnimatorControllerParameterType.Int);
            ac.AddParameter(SwimParam, AnimatorControllerParameterType.Bool);
            ac.AddParameter(DiveParam, AnimatorControllerParameterType.Bool);
            ac.AddParameter(MoveParam, AnimatorControllerParameterType.Int);
            AnimatorStateMachine root = ac.layers[0].stateMachine;

            var b = new Builder { ac = ac, clips = clips, outDir = outDir, model = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath) };
            var stanceMachines = new List<AnimatorStateMachine>();
            for (int i = 0; i < plan.stances.Length; i++)
                stanceMachines.Add(b.Stance(plan.stances[i], i, root, new Vector3(300, 120 * i)));

            // Boot: picks the saved stance on load (entering a sub-machine straight from Entry would
            // need a default inside it).
            AnimatorState boot = root.AddState("Boot", new Vector3(0, 0));
            boot.motion = b.idleOf.FirstOrDefault();
            root.defaultState = boot;
            for (int i = 0; i < stanceMachines.Count; i++)
            {
                var t = boot.AddTransition(stanceMachines[i]);
                t.hasExitTime = false; t.duration = 0;
                t.AddCondition(AnimatorConditionMode.Equals, i, StanceParam);
                for (int j = 0; j < stanceMachines.Count; j++)
                {
                    if (j == i) continue;
                    var sw = root.AddStateMachineTransition(stanceMachines[i], stanceMachines[j]);
                    sw.AddCondition(AnimatorConditionMode.Equals, j, StanceParam);
                }
            }
            b.Water(plan.stances[0], root, stanceMachines);
            GW2Face.AttachLayers(ac, outDir);

            EditorUtility.SetDirty(ac);
            AssetDatabase.SaveAssets();
            built = ac;
            return "gw2 locomotion: " + path + " -- stances " + string.Join("/", plan.stances.Select(s => StanceLabel(s.stance))) +
                   ", " + b.turned + " turned clips" + (b.missing.Count > 0 ? ", missing clips: " + string.Join(", ", b.missing.Distinct()) : "");
        }

        class Builder
        {
            public AnimatorController ac;
            public Dictionary<string, AnimationClip> clips;
            public GameObject model;  // the export, to measure a pose (Levelled)
            readonly Dictionary<string, Motion> levelled = new Dictionary<string, Motion>();  // by asset path: built once
            public string outDir;
            public int turned;
            public readonly List<string> missing = new List<string>();
            public readonly List<Motion> idleOf = new List<Motion>();

            StancePlan sp;
            Dictionary<string, StateInfo> states;
            Dictionary<string, float> speeds;

            void Use(StancePlan plan)
            {
                sp = plan;
                states = (plan.states ?? new StateInfo[0]).GroupBy(s => s.name).ToDictionary(g => g.Key, g => g.First());
                speeds = (plan.clips ?? new ClipInfo[0]).Where(c => !string.IsNullOrEmpty(c.name)).GroupBy(c => c.name).ToDictionary(g => g.Key, g => g.First().moveSpeed);
            }

            AnimationClip Clip(string name)
            {
                if (string.IsNullOrEmpty(name)) return null;
                if (clips.TryGetValue(name, out AnimationClip c)) return c;
                missing.Add(name);
                return null;
            }

            // The motion a GW2 state plays: its clip, or a baked copy turned by its yaw.
            Motion Of(string stateName)
            {
                if (!states.TryGetValue(stateName, out StateInfo s)) return null;
                AnimationClip c = Clip(s.clip);
                if (c == null) return null;
                if (Mathf.Abs(Mathf.DeltaAngle(0, s.yawDegrees)) < 0.01f) return c;
                turned++;
                string folder = outDir + "/" + StanceLabel(sp.stance);
                if (!AssetDatabase.IsValidFolder(folder)) AssetDatabase.CreateFolder(outDir, StanceLabel(sp.stance));
                // GW2's positive yaw turns the body left (Walk Right = walk forward at 270);
                // Unity's +Y turns right, hence the minus.
                return Turned(c, -s.yawDegrees, folder + "/" + stateName + ".anim");
            }

            float BlendIn(string to, string from = null)
            {
                TransInfo t = from == null ? null : Find(from, to, null);
                if (t != null && t.blendIn >= 0) return t.blendIn;
                return states.TryGetValue(to, out StateInfo s) ? s.blendIn : 0.25f;
            }

            TransInfo Find(string from, string to, string evt, bool withClip = false) =>
                (sp.transitions ?? new TransInfo[0]).FirstOrDefault(x => x.from == from && x.to == to &&
                    (evt == null || x.@event == evt) && (!withClip || !string.IsNullOrEmpty(x.clip)));

            // A directional tree: `centre` at (0,0), each named state at its direction x its speed.
            BlendTree Tree(string name, Motion centre, IEnumerable<string> names, float fallbackSpeed)
            {
                var tree = new BlendTree { name = name, blendType = BlendTreeType.FreeformDirectional2D,
                                           blendParameter = "VelocityX", blendParameterY = "VelocityZ", useAutomaticThresholds = false };
                AssetDatabase.AddObjectToAsset(tree, ac);
                if (centre != null) tree.AddChild(centre, Vector2.zero);
                foreach (string n in names)
                {
                    Motion m = Of(n);
                    if (m == null) continue;
                    float speed = speeds.TryGetValue(states[n].clip, out float v) && v > 0.01f ? v : fallbackSpeed;
                    tree.AddChild(m, Direction(n) * speed);
                }
                return tree;
            }

            static readonly string[] Dirs = { "Forward", "Backward", "Left", "Right", "Forward Left", "Forward Right", "Backward Left", "Backward Right" };
            IEnumerable<string> Named(string prefix) => Dirs.Select(d => prefix + " " + d).Where(states.ContainsKey);

            public AnimatorStateMachine Stance(StancePlan plan, int index, AnimatorStateMachine root, Vector3 at)
            {
                Use(plan);
                AnimatorStateMachine sm = root.AddStateMachine(StanceLabel(plan.stance), at);
                bool weapon = plan.stance != "u";
                string idleName = weapon && states.TryGetValue("Ground Alert", out var alert) && clips.ContainsKey(alert.clip ?? "") ? "Ground Alert" : "Ground Idle";
                Motion idleMotion = Of(idleName);
                idleOf.Add(idleMotion);

                AnimatorState idle = sm.AddState("Idle", new Vector3(300, 0));
                idle.motion = idleMotion;
                sm.defaultState = idle;
                if (weapon && Clip(plan.extras?.draw) is AnimationClip draw)
                {
                    AnimatorState drawState = sm.AddState("Draw", new Vector3(0, 0));
                    drawState.motion = draw;
                    sm.defaultState = drawState;
                    var t = T(drawState, idle, BlendIn(idleName)); t.hasExitTime = true; t.exitTime = 0.9f;
                }
                if (Of("Ground Fidget") is Motion fidgetMotion)
                {
                    AnimatorState fidget = sm.AddState("Fidget", new Vector3(300, -240));
                    fidget.motion = fidgetMotion;
                    var go = T(idle, fidget, BlendIn("Ground Fidget")); go.hasExitTime = true; go.exitTime = 3f;
                    var done = T(fidget, idle, BlendIn(idleName)); done.hasExitTime = true; done.exitTime = 0.95f;
                    var leave = T(fidget, idle, 0.15f); leave.AddCondition(AnimatorConditionMode.Greater, Still, "VelocityMagnitude");
                }

                // Moving: four states over one tree, so the stop knows which way you were going.
                var moveNames = states.Keys.Where(n => n.StartsWith("Ground Walk") || n.StartsWith("Ground Run") || n.StartsWith("Ground Sprint"));
                BlendTree tree = Tree(StanceLabel(plan.stance) + " Ground", idleMotion, moveNames,
                                      5f);
                var move = new Dictionary<string, AnimatorState>();
                var spots = new Dictionary<string, Vector3> { { "F", new Vector3(650, -80) }, { "L", new Vector3(650, 40) }, { "R", new Vector3(650, 160) }, { "B", new Vector3(650, 280) } };
                foreach (var kv in spots) { var st = sm.AddState("Move " + kv.Key, kv.Value); st.motion = tree; move[kv.Key] = st; }
                var gw2Name = new Dictionary<string, string> { { "F", "Ground Run Forward" }, { "L", "Ground Run Left" }, { "R", "Ground Run Right" }, { "B", "Ground Walk Backward" } };

                // Starts (idle -> run, by direction), most specific first; then a plain crossfade.
                foreach (string d in new[] { "L", "R", "F" })
                {
                    TransInfo start = Find(idleName, gw2Name[d], null, withClip: true);
                    AnimatorStateTransition into;
                    if (start != null && Clip(start.clip) is AnimationClip sc)
                    {
                        AnimatorState s = sm.AddState("Start " + d, spots[d] + new Vector3(-180, 0));
                        s.motion = sc;
                        into = T(idle, s, Math.Max(0, start.blendIn));
                        var on = T(s, move[d], start.blendOut > 0 ? start.blendOut : BlendIn(gw2Name[d]));
                        on.hasExitTime = true; on.exitTime = 0.85f;
                        var stop = T(s, idle, BlendIn(idleName)); stop.AddCondition(AnimatorConditionMode.Less, Still, "VelocityMagnitude");
                    }
                    else into = T(idle, move[d], BlendIn(gw2Name[d], idleName));
                    DirectionConditions(into, d);
                }
                var back = T(idle, move["B"], BlendIn("Ground Walk Backward", idleName));
                DirectionConditions(back, "B");
                var anyWay = T(idle, move["F"], BlendIn("Ground Run Forward", idleName));
                anyWay.AddCondition(AnimatorConditionMode.Greater, Still, "VelocityMagnitude");

                // Changing direction while moving: GW2's crossfade between the two states.
                foreach (string from in move.Keys)
                    foreach (string to in move.Keys)
                    {
                        if (from == to) continue;
                        DirectionConditions(T(move[from], move[to], BlendIn(gw2Name[to], gw2Name[from])), to);
                    }

                // Stops, by the direction you were moving in.
                foreach (var kv in move)
                {
                    TransInfo stop = Find(gw2Name[kv.Key], idleName, null, withClip: true);
                    AnimatorStateTransition t;
                    if (stop != null && Clip(stop.clip) is AnimationClip stc)
                    {
                        AnimatorState s = sm.AddState("Stop " + kv.Key, spots[kv.Key] + new Vector3(220, 0));
                        s.motion = stc;
                        t = T(kv.Value, s, Math.Max(0, stop.blendIn));
                        var done = T(s, idle, stop.blendOut > 0 ? stop.blendOut : BlendIn(idleName)); done.hasExitTime = true; done.exitTime = 0.9f;
                        var again = T(s, move["F"], BlendIn("Ground Run Forward")); again.AddCondition(AnimatorConditionMode.Greater, Still, "VelocityMagnitude");
                    }
                    else t = T(kv.Value, idle, BlendIn(idleName, gw2Name[kv.Key]));
                    t.AddCondition(AnimatorConditionMode.Less, Still, "VelocityMagnitude");
                }

                // Turning in place.
                foreach (var (name, sign, y) in new[] { ("Ground Turn Left", -1f, -360f), ("Ground Turn Right", 1f, 360f) })
                {
                    if (!(Of(name) is Motion tm)) continue;
                    AnimatorState turn = sm.AddState(name.Substring(7), new Vector3(300, y));
                    turn.motion = tm;
                    var go = T(idle, turn, BlendIn(name, idleName));
                    go.AddCondition(AnimatorConditionMode.Less, Still, "VelocityMagnitude");
                    go.AddCondition(sign < 0 ? AnimatorConditionMode.Less : AnimatorConditionMode.Greater, sign * TurnRate, "AngularY");
                    var stopTurn = T(turn, idle, BlendIn(idleName, name));
                    stopTurn.AddCondition(sign < 0 ? AnimatorConditionMode.Greater : AnimatorConditionMode.Less, sign * TurnRate * 0.5f, "AngularY");
                    var walk = T(turn, move["F"], BlendIn("Ground Run Forward", name));
                    walk.AddCondition(AnimatorConditionMode.Greater, Still, "VelocityMagnitude");
                }

                // Air: directional jump and fall, entered from anywhere in this stance.
                AnimatorState jump = null, fall = null;
                if (Of("Air Jump") is Motion jc)
                {
                    jump = sm.AddState("Jump", new Vector3(1050, -80));
                    jump.motion = Tree(StanceLabel(plan.stance) + " Jump", jc, Named("Air Jump"), 1f);
                }
                if (Of("Air Fall") is Motion fc)
                {
                    fall = sm.AddState("Fall", new Vector3(1050, 80));
                    fall.motion = Tree(StanceLabel(plan.stance) + " Fall", fc, Named("Air Fall"), 1f);
                }
                if (jump != null)
                {
                    var up = Any(root, jump, BlendIn("Air Jump"), index);
                    up.AddCondition(AnimatorConditionMode.Greater, JumpUp, "VelocityY");
                    up.AddCondition(AnimatorConditionMode.NotEqual, GlideValue, MoveParam);
                }
                if (fall != null)
                {
                    var down = Any(root, fall, BlendIn("Air Fall", "Air Jump"), index);
                    down.AddCondition(AnimatorConditionMode.Less, FallDown, "VelocityY");
                    down.AddCondition(AnimatorConditionMode.NotEqual, GlideValue, MoveParam);
                }
                AnimatorState glide = Glide(sm, fall, jump);

                // Landing: GW2's "Land" clips, standing and moving.
                foreach (var (target, moving, y) in new[] { (idleName, false, -60f), ("Ground Run Forward", true, 60f) })
                {
                    TransInfo t = Find("Air Fall", target, "Land", withClip: true);
                    if (t == null || !(Clip(t.clip) is AnimationClip lc)) continue;
                    AnimatorState land = sm.AddState(moving ? "Land Moving" : "Land", new Vector3(1350, y));
                    land.motion = lc;
                    foreach (AnimatorState from in new[] { fall, jump, glide })
                    {
                        if (from == null) continue;
                        var down = T(from, land, Math.Max(0f, t.blendIn));
                        down.AddCondition(AnimatorConditionMode.If, 0, "Grounded");
                        down.AddCondition(moving ? AnimatorConditionMode.Greater : AnimatorConditionMode.Less, Still, "VelocityMagnitude");
                    }
                    AnimatorState after = moving ? move["F"] : idle;
                    var done = T(land, after, t.blendOut > 0 ? t.blendOut : BlendIn(target));
                    done.hasExitTime = true; done.exitTime = moving ? 0.35f : 0.8f;
                    var other = T(land, moving ? idle : move["F"], BlendIn(moving ? idleName : "Ground Run Forward"));
                    other.AddCondition(moving ? AnimatorConditionMode.Less : AnimatorConditionMode.Greater, Still, "VelocityMagnitude");
                }

                // Every state so far is an ordinary one a move can start from (the air ones are
                // guarded by Grounded); move states made below are not, so a held move does not
                // restart itself from its own follow-on state.
                var ordinary = sm.states.Select(c => c.state).Where(x => x != jump && x != fall && x != glide && x.name != "Glide Open").ToList();
                GroundMoves(sm, ordinary, idle, idleName, move["F"]);
                // Leaving the ground with Glide on (a jump or an edge): straight into the glide.
                AnimatorState glideIn = sm.states.Select(c => c.state).FirstOrDefault(x => x.name == "Glide Open") ?? glide;
                if (glideIn != null)
                    foreach (AnimatorState f in ordinary)
                    {
                        var t = T(f, glideIn, BlendIn("Air Glide"));
                        t.AddCondition(AnimatorConditionMode.Equals, GlideValue, MoveParam);
                        t.AddCondition(AnimatorConditionMode.IfNot, 0, "Grounded");
                    }

                // Leaving the stance: every state exits when GW2Stance names another one, or into
                // the water (the top level picks the water state).
                foreach (ChildAnimatorState c in sm.states)
                {
                    var exit = c.state.AddExitTransition();
                    exit.hasExitTime = false; exit.hasFixedDuration = true; exit.duration = 0.25f;
                    exit.AddCondition(AnimatorConditionMode.NotEqual, index, StanceParam);
                    foreach (string wet in new[] { SwimParam, DiveParam })
                    {
                        var w = c.state.AddExitTransition();
                        w.hasExitTime = false; w.hasFixedDuration = true; w.duration = 0.3f;
                        w.AddCondition(AnimatorConditionMode.If, 0, wet);
                    }
                }
                return sm;
            }

            // The GW2Move moves on the ground (every Moves entry but the air ones). From anywhere in
            // the stance on the ground and dry; a button move plays once and returns to idle (or to
            // moving), a toggle holds until GW2Move changes, then plays its recovery if it has one.
            void GroundMoves(AnimatorStateMachine sm, List<AnimatorState> from, AnimatorState idle, string idleName, AnimatorState moving)
            {
                int row = 0;
                foreach (MoveDef m in Moves.Where(x => !x.air))
                {
                    Vector3 at = new Vector3(1700 + 260 * (row / 6), -300 + 110 * (row % 6));
                    row++;
                    var made = new List<AnimatorState>();
                    if (m.state == "Ground Limp")
                    {
                        // Limping is a way of walking: its own directional tree.
                        if (!Named("Ground Limp").Any()) continue;
                        AnimatorState limp = sm.AddState(m.label, at);
                        limp.motion = Tree(StanceLabel(sp.stance) + " Limp", Of(idleName), Named("Ground Limp"), 1.5f);
                        Into(from, limp, BlendIn("Ground Limp Forward"), m.value, null);
                        made.Add(limp);
                    }
                    else if (m.dirs)
                    {
                        // Most specific first: sideways, forward, backward; standing still takes
                        // the backward one, as GW2's standing dodge does.
                        foreach (string d in new[] { "L", "R", "F", "B", "" })
                        {
                            // Standing still: GW2's standing dodge goes backward; a slide goes forward.
                            string still = m.value == 1 ? "Backward" : "Forward";
                            string dirName = m.state + " " + (d == "L" ? "Left" : d == "R" ? "Right" : d == "F" ? "Forward" : d == "B" ? "Backward" : still);
                            if (!(Of(dirName) is Motion dm)) continue;
                            // GW2's slide stands on a steep downhill: levelled, it skids on flat ground
                            // instead of hanging in the air with both feet off it.
                            if (m.state == "Ground Slide" && dm is AnimationClip slideClip)
                            {
                                string lp = outDir + "/" + StanceLabel(sp.stance) + "/" + dirName + ".anim";
                                if (!levelled.TryGetValue(lp, out Motion lm)) levelled[lp] = lm = Levelled(slideClip, model, lp, sp.stance, outDir);
                                dm = lm;
                            }
                            AnimatorState s = made.FirstOrDefault(x => x.motion == dm);
                            if (s == null)
                            {
                                s = sm.AddState(m.label + " " + (d == "" ? still.Substring(0, 1) : d), at + new Vector3(0, 30 * made.Count));
                                s.motion = dm;
                                made.Add(s);
                            }
                            Into(from, s, BlendIn(dirName), m.value, d);
                        }
                    }
                    else if (Of(m.state) is Motion sm1)
                    {
                        AnimatorState s = sm.AddState(m.label, at);
                        s.motion = sm1;
                        made.Add(s);
                        Into(from, s, BlendIn(m.state), m.value, null);
                    }
                    if (made.Count == 0) continue;
                    foreach (AnimatorState s in made)
                    {
                        if (!m.hold)
                        {
                            var done = T(s, idle, BlendIn(idleName)); done.hasExitTime = true; done.exitTime = 0.9f;
                            done.AddCondition(AnimatorConditionMode.Less, Still, "VelocityMagnitude");
                            var go = T(s, moving, BlendIn("Ground Run Forward")); go.hasExitTime = true; go.exitTime = 0.8f;
                            go.AddCondition(AnimatorConditionMode.Greater, Still, "VelocityMagnitude");
                            continue;
                        }
                        AnimatorState last = s;
                        if (m.follow != null && Of(m.follow) is Motion fm)
                        {
                            // A hold that settles into a loop (downed: fall down, then the downed idle).
                            AnimatorState loop = sm.AddState(m.label + " Loop", at + new Vector3(130, 0));
                            loop.motion = fm;
                            var settle = T(s, loop, BlendIn(m.follow)); settle.hasExitTime = true; settle.exitTime = 0.95f;
                            Release(s, m, idle, idleName, at);
                            last = loop;
                        }
                        Release(last, m, idle, idleName, at);
                    }
                }
            }

            // From each ordinary state into a move: GW2Move == value on the ground, plus the
            // direction (`d`: L/R/F/B, "" standing still, null any). Kept ahead of the state's own
            // transitions, so a move wins over carrying on.
            void Into(List<AnimatorState> from, AnimatorState to, float seconds, int value, string d)
            {
                foreach (AnimatorState f in from)
                {
                    var t = T(f, to, seconds);
                    t.AddCondition(AnimatorConditionMode.Equals, value, MoveParam);
                    t.AddCondition(AnimatorConditionMode.If, 0, "Grounded");
                    if (d == "") t.AddCondition(AnimatorConditionMode.Less, Go, "VelocityMagnitude");
                    else if (d != null) DirectionConditions(t, d);
                    var list = f.transitions.ToList();
                    list.Remove(t);
                    int at = list.FindIndex(x => !x.conditions.Any(c => c.parameter == MoveParam));
                    list.Insert(at < 0 ? list.Count : at, t);
                    f.transitions = list.ToArray();
                }
            }

            // Leaving a held move: its recovery clip, then idle; or straight back to idle.
            void Release(AnimatorState s, MoveDef m, AnimatorState idle, string idleName, Vector3 at)
            {
                AnimatorStateTransition t;
                if (m.recover != null && Of(m.recover) is Motion rm)
                {
                    AnimatorState rec = s.transitions.Select(x => x.destinationState).FirstOrDefault(x => x != null && x.motion == rm);
                    if (rec == null)
                    {
                        AnimatorStateMachine sm = FindMachine(s);
                        rec = sm.states.Select(c => c.state).FirstOrDefault(x => x.name == m.label + " Recover");
                        if (rec == null)
                        {
                            rec = sm.AddState(m.label + " Recover", at + new Vector3(260, 0));
                            rec.motion = rm;
                            var up = T(rec, idle, BlendIn(idleName)); up.hasExitTime = true; up.exitTime = 0.9f;
                        }
                    }
                    t = T(s, rec, BlendIn(m.recover));
                }
                else t = T(s, idle, BlendIn(idleName));
                t.AddCondition(AnimatorConditionMode.NotEqual, m.value, MoveParam);
            }

            AnimatorStateMachine FindMachine(AnimatorState s)
            {
                foreach (var layer in ac.layers)
                    foreach (var sub in layer.stateMachine.stateMachines)
                        if (sub.stateMachine.states.Any(c => c.state == s)) return sub.stateMachine;
                return ac.layers[0].stateMachine;
            }

            // Gliding: GW2's tumble-to-glide clip, then the directional glide, while GW2Move holds
            // Glide in the air; letting go falls, the ground lands (the caller wires landing).
            AnimatorState Glide(AnimatorStateMachine sm, AnimatorState fall, AnimatorState jump)
            {
                if (!(Of("Air Glide") is Motion gm)) return null;
                AnimatorState glide = sm.AddState("Glide", new Vector3(1050, 240));
                glide.motion = Tree(StanceLabel(sp.stance) + " Glide", gm, Named("Air Glide"), 3f);
                AnimatorState into = glide;
                TransInfo open = Find("Air Fall", "Air Glide", null, withClip: true);
                if (open != null && Clip(open.clip) is AnimationClip oc)
                {
                    into = sm.AddState("Glide Open", new Vector3(850, 240));
                    into.motion = oc;
                    var on = T(into, glide, open.blendOut > 0 ? open.blendOut : BlendIn("Air Glide")); on.hasExitTime = true; on.exitTime = 0.8f;
                }
                foreach (AnimatorState air in new[] { jump, fall })
                {
                    if (air == null) continue;
                    var t = T(air, into, open != null ? Math.Max(0, open.blendIn) : BlendIn("Air Glide"));
                    t.AddCondition(AnimatorConditionMode.Equals, GlideValue, MoveParam);
                    t.AddCondition(AnimatorConditionMode.IfNot, 0, "Grounded");
                }
                if (fall != null)
                    foreach (AnimatorState g in new[] { into, glide }.Distinct())
                    {
                        var drop = T(g, fall, BlendIn("Air Fall"));
                        drop.AddCondition(AnimatorConditionMode.NotEqual, GlideValue, MoveParam);
                        drop.AddCondition(AnimatorConditionMode.IfNot, 0, "Grounded");
                    }
                return glide;
            }

            public void Water(StancePlan plan, AnimatorStateMachine root, List<AnimatorStateMachine> stances)
            {
                Use(plan);
                var made = new List<AnimatorState>();
                Motion surfaceIdle = Of("Water Surface Idle") ?? Of("Water Idle");
                if (surfaceIdle != null)
                {
                    AnimatorState surface = root.AddState("Water Surface", new Vector3(0, -240));
                    surface.motion = Tree("Water Surface", surfaceIdle, Named("Water Surface Swim"), 1.5f);
                    foreach (AnimatorStateMachine st in stances)
                    {
                        var into = root.AddStateMachineTransition(st, surface);
                        into.AddCondition(AnimatorConditionMode.If, 0, SwimParam);
                        into.AddCondition(AnimatorConditionMode.IfNot, 0, DiveParam);
                    }
                    made.Add(surface);
                }
                if (Of("Water Idle") is Motion underIdle)
                {
                    AnimatorState under = root.AddState("Water Under", new Vector3(0, -360));
                    under.motion = Tree("Water Under", underIdle, Named("Water Swim"), 2f);
                    foreach (AnimatorStateMachine st in stances)
                    {
                        var into = root.AddStateMachineTransition(st, under);
                        into.AddCondition(AnimatorConditionMode.If, 0, DiveParam);
                    }
                    made.Add(under);
                }
                // Surface <-> under, as Dive goes on and off.
                AnimatorState surfaceState = made.FirstOrDefault(x => x.name == "Water Surface");
                AnimatorState underState = made.FirstOrDefault(x => x.name == "Water Under");
                if (surfaceState != null && underState != null)
                {
                    T(surfaceState, underState, BlendIn("Water Idle")).AddCondition(AnimatorConditionMode.If, 0, DiveParam);
                    var up = T(underState, surfaceState, BlendIn("Water Surface Idle"));
                    up.AddCondition(AnimatorConditionMode.IfNot, 0, DiveParam);
                    up.AddCondition(AnimatorConditionMode.If, 0, SwimParam);
                }

                // Dodging in water: GW2's water evades, by direction, back to whichever water state.
                var evades = new List<AnimatorState>();
                foreach (string d in new[] { "L", "R", "F", "B", "" })
                {
                    string dirName = "Water Evade " + (d == "L" ? "Left" : d == "R" ? "Right" : d == "F" ? "Forward" : "Backward");
                    if (!(Of(dirName) is Motion em)) continue;
                    AnimatorState s = evades.FirstOrDefault(x => x.motion == em);
                    if (s == null)
                    {
                        s = root.AddState("Water Dodge " + (d == "" ? "B" : d), new Vector3(-300, -360 + 40 * evades.Count));
                        s.motion = em;
                        evades.Add(s);
                    }
                    foreach (AnimatorState w in new[] { surfaceState, underState })
                    {
                        if (w == null) continue;
                        var into = T(w, s, BlendIn(dirName));
                        into.AddCondition(AnimatorConditionMode.Equals, 1, MoveParam);
                        if (d != "") DirectionConditions(into, d);
                        else into.AddCondition(AnimatorConditionMode.Less, Go, "VelocityMagnitude");
                    }
                }
                foreach (AnimatorState e in evades)
                {
                    if (surfaceState != null) { var t = T(e, surfaceState, 0.25f); t.hasExitTime = true; t.exitTime = 0.9f; t.AddCondition(AnimatorConditionMode.IfNot, 0, DiveParam); }
                    if (underState != null) { var t = T(e, underState, 0.25f); t.hasExitTime = true; t.exitTime = 0.9f; t.AddCondition(AnimatorConditionMode.If, 0, DiveParam); }
                }
                made.AddRange(evades);

                foreach (AnimatorState w in made)
                    for (int i = 0; i < stances.Count; i++)
                    {
                        var dry = w.AddTransition(stances[i]);
                        dry.hasExitTime = false; dry.duration = 0.3f;
                        dry.AddCondition(AnimatorConditionMode.IfNot, 0, SwimParam);
                        dry.AddCondition(AnimatorConditionMode.IfNot, 0, DiveParam);
                        dry.AddCondition(AnimatorConditionMode.Equals, i, StanceParam);
                    }
            }

            // Any State -> an air state of stance `index`: off the ground, dry, that stance.
            AnimatorStateTransition Any(AnimatorStateMachine root, AnimatorState to, float seconds, int index)
            {
                var t = root.AddAnyStateTransition(to);
                Setup(t, seconds);
                t.AddCondition(AnimatorConditionMode.IfNot, 0, "Grounded");
                t.AddCondition(AnimatorConditionMode.IfNot, 0, SwimParam);
                t.AddCondition(AnimatorConditionMode.IfNot, 0, DiveParam);
                t.AddCondition(AnimatorConditionMode.Equals, index, StanceParam);
                return t;
            }
        }

        // The velocity a direction state is entered at. VelocityX: + right, VelocityZ: + forward.
        static void DirectionConditions(AnimatorStateTransition t, string d)
        {
            switch (d)
            {
                case "F": t.AddCondition(AnimatorConditionMode.Greater, Go, "VelocityZ"); break;
                case "B": t.AddCondition(AnimatorConditionMode.Less, -Go * 0.5f, "VelocityZ"); break;
                case "L":
                    t.AddCondition(AnimatorConditionMode.Less, -Go, "VelocityX");
                    t.AddCondition(AnimatorConditionMode.Less, Go, "VelocityZ");
                    t.AddCondition(AnimatorConditionMode.Greater, -Go * 0.5f, "VelocityZ");
                    break;
                case "R":
                    t.AddCondition(AnimatorConditionMode.Greater, Go, "VelocityX");
                    t.AddCondition(AnimatorConditionMode.Less, Go, "VelocityZ");
                    t.AddCondition(AnimatorConditionMode.Greater, -Go * 0.5f, "VelocityZ");
                    break;
            }
        }

        static AnimatorStateTransition T(AnimatorState from, AnimatorState to, float seconds)
        {
            AnimatorStateTransition t = from.AddTransition(to);
            Setup(t, seconds);
            return t;
        }

        static void Setup(AnimatorStateTransition t, float seconds)
        {
            t.hasExitTime = false;
            t.hasFixedDuration = true;
            t.duration = Mathf.Max(0f, seconds);
            t.canTransitionToSelf = false;
            t.interruptionSource = TransitionInterruptionSource.Destination;
        }

        // The controller at `path`, emptied, or a new one. An avatar's Base layer points at it by GUID,
        // so a rebuild must keep the asset (deleting and re-creating it would leave that slot empty).
        static AnimatorController Fresh(string path)
        {
            var ac = AssetDatabase.LoadAssetAtPath<AnimatorController>(path);
            if (ac == null) return AnimatorController.CreateAnimatorControllerAtPath(path);
            foreach (Object sub in AssetDatabase.LoadAllAssetsAtPath(path))
                if (sub != null && sub != ac) AssetDatabase.RemoveObjectFromAsset(sub);
            ac.layers = new AnimatorControllerLayer[0];
            ac.parameters = new AnimatorControllerParameter[0];
            ac.AddLayer("Base Layer");
            return ac;
        }

        // Unity names an FBX take "Armature|name" or "name".
        public static string TakeName(string n) { int bar = n.LastIndexOf('|'); return bar >= 0 ? n.Substring(bar + 1) : n; }

        // The plan's clips: loop or once, root kept still (VRChat moves the avatar).
        static void SetImport(string fbxPath, Plan plan)
        {
            var imp = AssetImporter.GetAtPath(fbxPath) as ModelImporter;
            if (imp == null) return;
            var wanted = new HashSet<string>();
            var once = new HashSet<string>();
            foreach (EmoteInfo e in plan.emotes ?? new EmoteInfo[0])
            {
                foreach (string c in new[] { e.intro, e.loop, e.outro }) if (!string.IsNullOrEmpty(c)) wanted.Add(c);
                if (!string.IsNullOrEmpty(e.intro)) once.Add(e.intro);
                if (!string.IsNullOrEmpty(e.outro)) once.Add(e.outro);
            }
            foreach (StancePlan sp in plan.stances)
            {
                foreach (ClipInfo c in sp.clips ?? new ClipInfo[0]) if (!string.IsNullOrEmpty(c.name)) wanted.Add(c.name);
                foreach (StateInfo s in sp.states ?? new StateInfo[0])
                    if ((s.flags & OnceFlag) != 0 && !string.IsNullOrEmpty(s.clip)) once.Add(s.clip);
                foreach (TransInfo t in sp.transitions ?? new TransInfo[0]) if (!string.IsNullOrEmpty(t.clip)) once.Add(t.clip);
                if (sp.extras != null) { once.Add(sp.extras.draw ?? ""); once.Add(sp.extras.stow ?? ""); }
            }
            // An explicit take list imports only the takes on it: a re-export that carries more clips
            // must add them, or Unity silently leaves them out. Earlier settings are kept per take.
            var list = imp.clipAnimations.ToList();
            bool changed = false;
            foreach (var d in imp.defaultClipAnimations)
                if (!list.Any(c => c.takeName == d.takeName)) { list.Add(d); changed = true; }
            list.RemoveAll(c => !imp.defaultClipAnimations.Any(d => d.takeName == c.takeName) && (changed = true));
            // A take's frame range is stored with it; a re-export at another frame rate (or a clip of
            // another length) must take the file's range, or Unity cuts the clip at the old one.
            foreach (var c in list)
            {
                var d = imp.defaultClipAnimations.FirstOrDefault(x => x.takeName == c.takeName);
                if (d == null || (Mathf.Abs(c.firstFrame - d.firstFrame) < 0.01f && Mathf.Abs(c.lastFrame - d.lastFrame) < 0.01f)) continue;
                c.firstFrame = d.firstFrame;
                c.lastFrame = d.lastFrame;
                changed = true;
            }
            // Every transform in the mask: a humanoid take otherwise drops the bones outside the
            // humanoid set, the face among them (GW2Face reads those curves).
            if (list.Any(c => c.maskType != ClipAnimationMaskType.CreateFromThisModel))
            {
                var model = AssetDatabase.LoadAssetAtPath<GameObject>(fbxPath);
                var all = new AvatarMask();
                if (model != null) all.AddTransformPath(model.transform, true);
                foreach (var c in list) { c.maskType = ClipAnimationMaskType.CreateFromThisModel; c.ConfigureClipFromMask(all); }
                changed = true;
            }
            foreach (var c in list)
            {
                string n = TakeName(c.name);
                if (!wanted.Contains(n)) continue;
                bool loop = !once.Contains(n);
                if (c.loopTime == loop && c.lockRootRotation && c.lockRootHeightY && c.lockRootPositionXZ && c.keepOriginalOrientation) continue;
                c.loopTime = loop;
                c.lockRootRotation = true;     // bake into pose
                c.lockRootHeightY = true;
                c.lockRootPositionXZ = true;
                c.keepOriginalOrientation = true;
                c.keepOriginalPositionY = true;
                c.keepOriginalPositionXZ = true;
                changed = true;
            }
            if (!changed) return;
            imp.clipAnimations = list.ToArray();
            imp.SaveAndReimport();
        }

        // Unit direction of a GW2 state name, Unity axes: +X right, +Z forward.
        static Vector2 Direction(string name)
        {
            float x = 0, z = 0;
            if (name.Contains("Forward")) z += 1;
            if (name.Contains("Backward")) z -= 1;
            if (name.EndsWith(" Left")) x -= 1;
            if (name.EndsWith(" Right")) x += 1;
            var d = new Vector2(x, z);
            return d == Vector2.zero ? new Vector2(0, 1) : d.normalized;
        }

        // A copy of `clip` posed level: the slope its feet stand on (their height difference against
        // their spread, averaged over the clip) rotated away, then lowered until its lower ankle is
        // as high as when standing, so the feet meet the ground. For clips authored on a slope, like
        // GW2's slide (it otherwise hangs in the air on flat ground).
        static Motion Levelled(AnimationClip clip, GameObject model, string path, string stance, string outDir)
        {
            if (model == null) return clip;
            string folder = Path.GetDirectoryName(path).Replace('\\', '/');
            if (!AssetDatabase.IsValidFolder(folder)) AssetDatabase.CreateFolder(outDir, Path.GetFileName(folder));
            var times = new[] { 0f, 0.25f, 0.5f, 0.75f };
            Vector3 spread = Vector3.zero;
            float rest = 0, lowest = 0;
            Motion copy = clip;
            Measure(model, (go, an) =>
            {
                Transform l = an.GetBoneTransform(HumanBodyBones.LeftFoot), r = an.GetBoneTransform(HumanBodyBones.RightFoot);
                rest = Mathf.Min(l.position.y, r.position.y);  // the model as imported: standing
                foreach (float f in times)
                {
                    clip.SampleAnimation(go, clip.length * f);
                    spread += l.position - r.position;
                }
            });
            var flat = new Vector3(spread.x, 0, spread.z);
            if (flat.sqrMagnitude < 1e-6f) return clip;
            copy = Turned(clip, Quaternion.FromToRotation(spread.normalized, flat.normalized), path, 0f);
            var levelled = (AnimationClip)copy;
            Measure(model, (go, an) =>
            {
                Transform l = an.GetBoneTransform(HumanBodyBones.LeftFoot), r = an.GetBoneTransform(HumanBodyBones.RightFoot);
                float sum = 0;
                foreach (float f in times)
                {
                    levelled.SampleAnimation(go, levelled.length * f);
                    sum += Mathf.Min(l.position.y, r.position.y);
                }
                lowest = sum / times.Length;
            });
            return Turned(clip, Quaternion.FromToRotation(spread.normalized, flat.normalized), path, lowest - rest);
        }

        // Runs `body` on an instance of `model` in a preview scene (nothing in the open scenes changes).
        static void Measure(GameObject model, Action<GameObject, Animator> body)
        {
            var scene = UnityEditor.SceneManagement.EditorSceneManager.NewPreviewScene();
            try
            {
                var go = (GameObject)Object.Instantiate(model);
                UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(go, scene);
                var an = go.GetComponent<Animator>();
                if (an != null && an.isHuman) body(go, an);
            }
            finally { UnityEditor.SceneManagement.EditorSceneManager.ClosePreviewScene(scene); }
        }

        // A copy of `clip` with the body turned `degrees` about up (RootQ and RootT rotated), the
        // turn baked into the pose: VRChat drops root motion, and an unbaked root turn with it.
        static Motion Turned(AnimationClip clip, float degrees, string path) => Turned(clip, Quaternion.Euler(0, degrees, 0), path, 0f);

        // `turn` any rotation; `lower`: metres taken off the root height.
        static Motion Turned(AnimationClip clip, Quaternion turn, string path, float lower)
        {
            var bindings = AnimationUtility.GetCurveBindings(clip);
            EditorCurveBinding? B(string p) { foreach (var b in bindings) if (b.path == "" && b.propertyName == p) return b; return null; }
            var qb = new[] { B("RootQ.x"), B("RootQ.y"), B("RootQ.z"), B("RootQ.w") };
            if (qb.Any(b => b == null)) return clip;
            var tb = new[] { B("RootT.x"), B("RootT.y"), B("RootT.z") };

            AnimationClip copy = Object.Instantiate(clip);
            copy.name = Path.GetFileNameWithoutExtension(path);
            AnimationClipSettings st = AnimationUtility.GetAnimationClipSettings(copy);
            st.loopBlendOrientation = true;
            st.loopBlendPositionY = true;
            st.loopBlendPositionXZ = true;
            st.keepOriginalOrientation = true;
            st.keepOriginalPositionY = true;
            st.keepOriginalPositionXZ = true;
            AnimationUtility.SetAnimationClipSettings(copy, st);
            var q = qb.Select(b => AnimationUtility.GetEditorCurve(clip, b.Value)).ToArray();
            var times = q.SelectMany(c => c.keys.Select(k => k.time)).Distinct().OrderBy(t => t).ToArray();
            var outQ = new[] { new AnimationCurve(), new AnimationCurve(), new AnimationCurve(), new AnimationCurve() };
            foreach (float t in times)
            {
                Quaternion r = turn * new Quaternion(q[0].Evaluate(t), q[1].Evaluate(t), q[2].Evaluate(t), q[3].Evaluate(t));
                outQ[0].AddKey(t, r.x); outQ[1].AddKey(t, r.y); outQ[2].AddKey(t, r.z); outQ[3].AddKey(t, r.w);
            }
            for (int i = 0; i < 4; i++) AnimationUtility.SetEditorCurve(copy, qb[i].Value, outQ[i]);
            if (tb.All(b => b != null))
            {
                var c = tb.Select(b => AnimationUtility.GetEditorCurve(clip, b.Value)).ToArray();
                var tt = c.SelectMany(x => x.keys.Select(k => k.time)).Distinct().OrderBy(t => t).ToArray();
                var outT = new[] { new AnimationCurve(), new AnimationCurve(), new AnimationCurve() };
                foreach (float t in tt)
                {
                    Vector3 p = turn * new Vector3(c[0].Evaluate(t), c[1].Evaluate(t), c[2].Evaluate(t)) - new Vector3(0, lower, 0);
                    outT[0].AddKey(t, p.x); outT[1].AddKey(t, p.y); outT[2].AddKey(t, p.z);
                }
                for (int i = 0; i < 3; i++) AnimationUtility.SetEditorCurve(copy, tb[i].Value, outT[i]);
            }
            AssetDatabase.DeleteAsset(path);
            AssetDatabase.CreateAsset(copy, path);
            return copy;
        }
    }
}
