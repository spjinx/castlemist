// castlemist character export: the VRChat side of the GW2 locomotion (2026-11-02). Compiled only
// where the VRChat Avatars SDK is installed (CASTLEMIST_VRC_AVATARS, set by the asmdef).
//
// Castlemist > Avatar > Make GW2 Parameters and Menu (selected avatar .fbx)
//   beside the controller GW2Animator builds: "<name> GW2 Parameters" (GW2Stance Int saved,
//   GW2Move and GW2Emote Ints, GW2Swim and GW2Dive Bools, all synced: 26 bits) and "<name> GW2 Menu": Stance
//   (a toggle per stance), Moves and Combat (GW2Animator.Moves: buttons for one-shot moves,
//   toggles for held ones; deaths in their own submenu), Swim, Dive. Rebuilt in place, so an
//   avatar already pointing at them keeps working. Nothing on any avatar changes.
// Castlemist > Avatar > Add GW2 to Selected Avatar
//   with an avatar (VRCAvatarDescriptor) selected in the scene and the export's .fbx in the Project
//   window: asks first, then puts the locomotion controller in the Base layer, appends the three
//   parameters to the avatar's own parameter list (left alone if already there) and adds a "GW2"
//   submenu to its root menu. Undo reverts the descriptor; the parameter and menu assets are
//   recorded for undo too.
#if CASTLEMIST_VRC_AVATARS
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Animations;
using UnityEngine;
using VRC.SDK3.Avatars.Components;
using VRC.SDK3.Avatars.ScriptableObjects;

namespace Castlemist.Avatar
{
    public static class GW2AvatarSetup
    {
        const int ParameterBudget = 256;  // VRChat's synced-bit limit

        [MenuItem("Castlemist/Avatar/Make GW2 Parameters and Menu (selected avatar .fbx)")]
        public static void MakeMenu()
        {
            string fbx = Selection.activeObject != null ? AssetDatabase.GetAssetPath(Selection.activeObject) : null;
            Debug.Log("CASTLEMIST " + Make(fbx, out _, out _));
        }

        /// The GW2 parameters and menu for an export; returns a one-line report.
        public static string Make(string assetPath, out VRCExpressionParameters parameters, out VRCExpressionsMenu menu)
        {
            parameters = null;
            menu = null;
            string[] stances = GW2Animator.Stances(assetPath);
            if (stances.Length == 0) return "gw2 menu: select an avatar .fbx exported with character-vrchat --animator";
            string dir = Path.GetDirectoryName(assetPath).Replace('\\', '/');
            string stem = Path.GetFileNameWithoutExtension(assetPath);
            if (stem.EndsWith(".animator")) stem = stem.Substring(0, stem.Length - ".animator".Length);
            string outDir = dir + "/" + stem + " GW2 Animator";
            if (!AssetDatabase.IsValidFolder(outDir)) AssetDatabase.CreateFolder(dir, stem + " GW2 Animator");

            // Merged, not overwritten: an avatar may use this list as its own and add to it.
            parameters = Asset<VRCExpressionParameters>(outDir + "/" + stem + " GW2 Parameters.asset");
            var ours = Ours().ToList();
            parameters.parameters = ours.Concat((parameters.parameters ?? new VRCExpressionParameters.Parameter[0])
                .Where(x => x != null && !ours.Any(o => o.name == x.name))).ToArray();
            EditorUtility.SetDirty(parameters);

            VRCExpressionsMenu Menu(string name)
            {
                var m = Asset<VRCExpressionsMenu>(outDir + "/" + stem + " GW2 " + name + " Menu.asset");
                m.controls.Clear();
                EditorUtility.SetDirty(m);
                return m;
            }
            VRCExpressionsMenu.Control Move(GW2Animator.MoveDef d) => new VRCExpressionsMenu.Control
            {
                name = d.label, value = d.value,
                type = d.hold ? VRCExpressionsMenu.Control.ControlType.Toggle : VRCExpressionsMenu.Control.ControlType.Button,
                parameter = new VRCExpressionsMenu.Control.Parameter { name = GW2Animator.MoveParam }
            };
            VRCExpressionsMenu.Control Sub(string name, VRCExpressionsMenu sub) =>
                new VRCExpressionsMenu.Control { name = name, type = VRCExpressionsMenu.Control.ControlType.SubMenu, subMenu = sub };

            var stanceMenu = Menu("Stance");
            for (int i = 0; i < stances.Length; i++)
                stanceMenu.controls.Add(Toggle(GW2Animator.StanceLabel(stances[i]), GW2Animator.StanceParam, i));
            var moves = Menu("Moves");
            foreach (var d in GW2Animator.Moves.Where(x => x.menu == "Moves")) moves.controls.Add(Move(d));
            var deaths = Menu("Deaths");
            foreach (var d in GW2Animator.Moves.Where(x => x.menu == "Deaths")) deaths.controls.Add(Move(d));
            // Emotes: pages of seven and a "More" to the next page (VRChat menus hold eight).
            GW2Animator.EmoteInfo[] emotes = GW2Animator.Emotes(assetPath);
            var emotePages = new List<VRCExpressionsMenu>();
            for (int i = 0; i < emotes.Length; i += 7) emotePages.Add(Menu("Emotes " + (emotePages.Count + 1)));
            // As in GW2: a looping emote lasts until stopped (a toggle), a one-shot plays once (a button).
            for (int i = 0; i < emotes.Length; i++)
            {
                var c = Toggle(emotes[i].label, GW2Actions.EmoteParam, i + 1);
                if (!GW2Animator.Loops(emotes[i])) c.type = VRCExpressionsMenu.Control.ControlType.Button;
                emotePages[i / 7].controls.Add(c);
            }
            for (int pg = 0; pg + 1 < emotePages.Count; pg++) emotePages[pg].controls.Add(Sub("More", emotePages[pg + 1]));
            var combat = Menu("Combat");
            foreach (var d in GW2Animator.Moves.Where(x => x.menu == "Combat")) combat.controls.Add(Move(d));
            combat.controls.Add(Sub("Deaths", deaths));

            // The root keeps whatever else was put in it; only the GW2 entries are replaced
            // (including the stance toggles the first version put at the top).
            menu = Asset<VRCExpressionsMenu>(outDir + "/" + stem + " GW2 Menu.asset");
            var gw2Names = new HashSet<string>(new[] { "Stance", "Moves", "Combat", "Emotes", "Swim", "Dive" }.Concat(stances.Select(GW2Animator.StanceLabel)));
            var others = menu.controls.Where(c => !gw2Names.Contains(c.name)).ToList();
            menu.controls.Clear();
            menu.controls.Add(Sub("Stance", stanceMenu));
            menu.controls.Add(Sub("Moves", moves));
            menu.controls.Add(Sub("Combat", combat));
            if (emotePages.Count > 0) menu.controls.Add(Sub("Emotes", emotePages[0]));
            menu.controls.Add(Toggle("Swim", GW2Animator.SwimParam, 1));
            menu.controls.Add(Toggle("Dive", GW2Animator.DiveParam, 1));
            foreach (var c in others.Take(VRCExpressionsMenu.MAX_CONTROLS - menu.controls.Count)) menu.controls.Add(c);
            EditorUtility.SetDirty(menu);
            AssetDatabase.SaveAssets();
            string pPath = AssetDatabase.GetAssetPath(parameters), mPath = AssetDatabase.GetAssetPath(menu);
            return "gw2 menu: " + pPath + " and " + mPath + " (" + stances.Length + " stances)";
        }

        [MenuItem("Castlemist/Avatar/Add GW2 to Selected Avatar")]
        public static void AddToAvatar()
        {
            VRCAvatarDescriptor avatar = Selection.gameObjects.Select(g => g.GetComponent<VRCAvatarDescriptor>()).FirstOrDefault(d => d != null);
            string fbx = Selection.assetGUIDs.Select(AssetDatabase.GUIDToAssetPath).FirstOrDefault(p => GW2Animator.Stances(p).Length > 0);
            if (avatar == null || fbx == null)
            {
                EditorUtility.DisplayDialog("GW2 locomotion", "Select the avatar in the scene AND the exported .fbx in the Project window (ctrl-click), then run this again.", "OK");
                return;
            }
            if (!EditorUtility.DisplayDialog("GW2 locomotion",
                    "On " + avatar.name + ":\n- Base layer: the GW2 locomotion controller\n- Action layer: the GW2 emotes\n- adds GW2Stance, GW2Move, GW2Emote, GW2Swim, GW2Dive to its parameters (26 bits)\n- adds a GW2 submenu to its menu\n\nUndo reverts it.",
                    "Add", "Cancel"))
                return;
            Debug.Log("CASTLEMIST " + Add(avatar, fbx));
        }

        /// Wires the GW2 controller, parameters and menu into `avatar`; returns a one-line report.
        public static string Add(VRCAvatarDescriptor avatar, string fbx)
        {
            string report = GW2Animator.Build(fbx, out AnimatorController controller);
            if (controller == null) return report;
            Make(fbx, out _, out VRCExpressionsMenu gw2Menu);
            AnimatorController actions = GW2Actions.Build(fbx, out string actionReport);
            report += " | " + actionReport;

            Undo.RecordObject(avatar, "Add GW2 locomotion");
            avatar.customizeAnimationLayers = true;
            var layers = avatar.baseAnimationLayers;
            for (int i = 0; i < layers.Length; i++)
                if (layers[i].type == VRCAvatarDescriptor.AnimLayerType.Base ||
                    (layers[i].type == VRCAvatarDescriptor.AnimLayerType.Action && actions != null))
                {
                    layers[i].isDefault = false;
                    layers[i].isEnabled = true;
                    layers[i].animatorController = layers[i].type == VRCAvatarDescriptor.AnimLayerType.Base ? controller : actions;
                }
            avatar.baseAnimationLayers = layers;

            avatar.customExpressions = true;
            string notes = "";
            if (avatar.expressionParameters == null) notes += " (no parameter list on the avatar: assign one)";
            else
            {
                Undo.RecordObject(avatar.expressionParameters, "Add GW2 parameters");
                var list = avatar.expressionParameters.parameters?.ToList() ?? new List<VRCExpressionParameters.Parameter>();
                foreach (var p in Ours())
                    if (!list.Any(x => x.name == p.name)) list.Add(p);
                avatar.expressionParameters.parameters = list.ToArray();
                int cost = avatar.expressionParameters.CalcTotalCost();
                if (cost > ParameterBudget) notes += " (parameters now cost " + cost + " bits, over VRChat's " + ParameterBudget + ")";
                EditorUtility.SetDirty(avatar.expressionParameters);
            }
            if (avatar.expressionsMenu == null) notes += " (no menu on the avatar: assign one)";
            else if (avatar.expressionsMenu != gw2Menu && !avatar.expressionsMenu.controls.Any(c => c.subMenu == gw2Menu || c.name == "GW2"))
            {
                Undo.RecordObject(avatar.expressionsMenu, "Add GW2 menu");
                if (avatar.expressionsMenu.controls.Count >= VRCExpressionsMenu.MAX_CONTROLS)
                    notes += " (its root menu is full: add the GW2 menu as a submenu by hand)";
                else
                {
                    avatar.expressionsMenu.controls.Add(new VRCExpressionsMenu.Control
                    {
                        name = "GW2", type = VRCExpressionsMenu.Control.ControlType.SubMenu, subMenu = gw2Menu
                    });
                    EditorUtility.SetDirty(avatar.expressionsMenu);
                }
            }
            EditorUtility.SetDirty(avatar);
            AssetDatabase.SaveAssets();
            return report + " -- added to " + avatar.name + notes;
        }

        // The asset at `path`, or a new one there. Kept, not re-created: avatars hold it by GUID.
        static T Asset<T>(string path) where T : ScriptableObject
        {
            var a = AssetDatabase.LoadAssetAtPath<T>(path);
            if (a != null) return a;
            a = ScriptableObject.CreateInstance<T>();
            AssetDatabase.CreateAsset(a, path);
            return a;
        }

        static IEnumerable<VRCExpressionParameters.Parameter> Ours()
        {
            yield return new VRCExpressionParameters.Parameter { name = GW2Animator.StanceParam, valueType = VRCExpressionParameters.ValueType.Int, saved = true, networkSynced = true };
            yield return new VRCExpressionParameters.Parameter { name = GW2Animator.MoveParam, valueType = VRCExpressionParameters.ValueType.Int, saved = false, networkSynced = true };
            yield return new VRCExpressionParameters.Parameter { name = GW2Actions.EmoteParam, valueType = VRCExpressionParameters.ValueType.Int, saved = false, networkSynced = true };
            yield return new VRCExpressionParameters.Parameter { name = GW2Animator.SwimParam, valueType = VRCExpressionParameters.ValueType.Bool, saved = false, networkSynced = true };
            yield return new VRCExpressionParameters.Parameter { name = GW2Animator.DiveParam, valueType = VRCExpressionParameters.ValueType.Bool, saved = false, networkSynced = true };
        }

        static VRCExpressionsMenu.Control Toggle(string name, string parameter, float value) => new VRCExpressionsMenu.Control
        {
            name = name, type = VRCExpressionsMenu.Control.ControlType.Toggle, value = value,
            parameter = new VRCExpressionsMenu.Control.Parameter { name = parameter }
        };
    }
}
#endif
