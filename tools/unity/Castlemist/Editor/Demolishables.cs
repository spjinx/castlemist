// castlemist world export: props that can be demolished in a VRChat world (2026-10-25).
//
// A model with a "Demolish" clip (map 506592's 84108, a Mad King tower block) gets one controller,
// Controllers/<fileId>_Demolish.controller: "Idle" (the instance's own clip, zeropose, or none) ->
// "Demolish" when the bool "Demolished" turns on -> "Rubble" (the clip held on its last frame) at the
// end; either back to "Idle" when it turns off. Each placed instance then gets:
//   * that controller on its Animator (AlwaysAnimate), skinned bounds fitted to both clips;
//   * its dust particle systems timed to Demolish once, on demand (ParticleEffects.Fit onDemand);
//   * its own collider (the cell's PropColliders/PropCollider_<prop>, which the exporter split out of
//     Collider_k) moved under it as "GW2 Prop Collider";
//   * a trigger box over its renderers to click, and Runtime/GW2Demolishable (UdonSharp) wired to all
//     of it and to the GMMenu's PlayerPermissions when the world has one.
// With the GMMenu, one "GW2 Start As Player" object (Runtime/GW2StartAsPlayer) drops everyone to the
// player level at join; the menu's role buttons then switch to facilitator or GM (no whitelist).
// The UdonSharp program assets and the Castlemist.Runtime assembly definition asset are made here
// when missing (EnsurePrograms). Without UdonSharp nothing is added (one warning).

using System;
using System.Collections.Generic;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.Animations;
using AssemblyDefinitionAsset = UnityEditorInternal.AssemblyDefinitionAsset;
using UnityEngine;

namespace Castlemist
{
    public static class Demolishables
    {
        public const string ClipName = "Demolish";
        public const string ColliderName = "GW2 Prop Collider";
        const string ClickName = "GW2 Demolish Click";
        const string StartAsPlayerName = "GW2 Start As Player";

        /// True when @p clips (models[].clips) has the Demolish clip, in any case.
        public static bool Has(IEnumerable<string> clips)
        {
            return clips != null && clips.Any(c => string.Equals(c, ClipName, StringComparison.OrdinalIgnoreCase));
        }

        /// The controller at @p path (replaced): Idle (@p idle, may be null) / Demolish / Rubble.
        public static AnimatorController BuildController(string path, AnimationClip idle, AnimationClip demolish)
        {
            if (AssetDatabase.LoadAssetAtPath<AnimatorController>(path) != null) AssetDatabase.DeleteAsset(path);
            var ac = AnimatorController.CreateAnimatorControllerAtPath(path);
            ac.AddParameter("Demolished", AnimatorControllerParameterType.Bool);
            ac.AddParameter(new AnimatorControllerParameter { name = "RubbleTime", type = AnimatorControllerParameterType.Float, defaultFloat = 0.999f });
            AnimatorStateMachine sm = ac.layers[0].stateMachine;
            AnimatorState idleSt = sm.AddState("Idle");
            idleSt.motion = idle;
            AnimatorState dem = sm.AddState("Demolish");
            dem.motion = demolish;
            AnimatorState rub = sm.AddState("Rubble");
            rub.motion = demolish;
            rub.timeParameterActive = true;   // the clip held at RubbleTime (its end), whether or not it loops
            rub.timeParameter = "RubbleTime";
            sm.defaultState = idleSt;
            AnimatorStateTransition t = idleSt.AddTransition(dem);
            t.AddCondition(AnimatorConditionMode.If, 0, "Demolished");
            t.hasExitTime = false; t.duration = 0f;
            t = dem.AddTransition(rub);
            t.hasExitTime = true; t.exitTime = 1f; t.duration = 0f;
            foreach (AnimatorState from in new[] { dem, rub })
            {
                t = from.AddTransition(idleSt);
                t.AddCondition(AnimatorConditionMode.IfNot, 0, "Demolished");
                t.hasExitTime = false; t.duration = 0f;
            }
            EditorUtility.SetDirty(ac);
            return ac;
        }

        /// Sets placed instance @p inst up (see the header). @p propCollider: its PropCollider_<prop>
        /// (null: none split out); @p permissions: the GMMenu's PlayerPermissions or null. False when
        /// UdonSharp's GW2Demolishable is not available.
        public static bool SetupInstance(GameObject inst, AnimatorController ac, string sidecar, float unityScale,
                                         Transform propCollider, Component permissions, List<string> warnings)
        {
            Type demolishable = SceneSetup.FindType("Castlemist.GW2Demolishable");
            if (demolishable == null) return false;
            Animator an = inst.GetComponent<Animator>();
            if (an == null) an = inst.AddComponent<Animator>();
            an.runtimeAnimatorController = ac;
            an.cullingMode = AnimatorCullingMode.AlwaysAnimate;
            foreach (Transform t in inst.GetComponentsInChildren<Transform>(true))
                GameObjectUtility.SetStaticEditorFlags(t.gameObject, 0);
            WorldImporter.FitSkinnedBounds(inst, ac);
            ApplyIdleVisibility(inst, ac);

            if (sidecar != null) ParticleEffects.Fit(inst, sidecar, ClipName, unityScale, true);
            ParticleSystem[] dust = inst.GetComponentsInChildren<ParticleSystem>(true)
                .Where(p => p.name.StartsWith(ParticleEffects.ObjectPrefix, StringComparison.Ordinal)).ToArray();

            Transform oldCol = inst.transform.Find(ColliderName);
            if (oldCol != null && oldCol != propCollider) UnityEngine.Object.DestroyImmediate(oldCol.gameObject);
            if (propCollider != null)
            {
                propCollider.SetParent(inst.transform, true);
                propCollider.name = ColliderName;
            }

            // the click box: the renderers' bounds at the idle pose, in the instance's space, a trigger
            Transform oldClick = inst.transform.Find(ClickName);
            if (oldClick != null) UnityEngine.Object.DestroyImmediate(oldClick.gameObject);
            var box = inst.GetComponent<BoxCollider>();
            if (box == null) box = inst.AddComponent<BoxCollider>();
            box.isTrigger = true;
            Bounds wb = default;
            bool any = false;
            foreach (Renderer r in inst.GetComponentsInChildren<Renderer>(true))
            {
                if (r is ParticleSystemRenderer) continue;
                Bounds b = r is SkinnedMeshRenderer s && s.sharedMesh != null ? WorldBounds(s) : r.bounds;
                if (!any) { wb = b; any = true; } else wb.Encapsulate(b);
            }
            if (any)
            {
                Matrix4x4 w2l = inst.transform.worldToLocalMatrix;
                Vector3 c = w2l.MultiplyPoint3x4(wb.center);
                Vector3 e = w2l.MultiplyVector(wb.extents);
                box.center = c;
                box.size = new Vector3(Mathf.Abs(e.x), Mathf.Abs(e.y), Mathf.Abs(e.z)) * 2f;
            }

            var comp = inst.GetComponent(demolishable) ?? SceneSetup.AddUdonAware(inst, demolishable);
            SceneSetup.SetField(comp, "animator", an);
            SceneSetup.SetField(comp, "dust", dust);
            SceneSetup.SetField(comp, "propCollider", propCollider != null ? propCollider.gameObject : null);
            if (permissions != null) SceneSetup.SetField(comp, "permissions", permissions);
            SceneSetup.CopyProxyToUdon(comp);
            if (propCollider == null) warnings.Add("demolish: " + inst.name + " has no own collider: it stays solid once demolished");
            return true;
        }

        /// The renderers' saved enabled state set to the Idle clip's mesh visibility at its start, so
        /// the editor and the Animator's write-defaults agree: 84108's intact and broken mesh sets
        /// overlap exactly and z-fought with both on (2026-10-25).
        static void ApplyIdleVisibility(GameObject inst, AnimatorController ac)
        {
            AnimationClip idle = null;
            foreach (ChildAnimatorState s in ac.layers[0].stateMachine.states)
                if (s.state.name == "Idle") idle = s.state.motion as AnimationClip;
            foreach (Renderer r in inst.GetComponentsInChildren<Renderer>(true))
            {
                if (r is ParticleSystemRenderer) continue;
                bool on = true;
                if (idle != null)
                {
                    var b = EditorCurveBinding.FloatCurve(AnimationUtility.CalculateTransformPath(r.transform, inst.transform), r.GetType(), "m_Enabled");
                    AnimationCurve c = AnimationUtility.GetEditorCurve(idle, b);
                    if (c != null) on = c.Evaluate(0f) > 0.5f;
                }
                if (r.enabled != on) { Undo.RecordObject(r, "Idle visibility"); r.enabled = on; }
            }
        }

        /// A skinned renderer's bind-pose mesh bounds in world space (Renderer.bounds of a skinned mesh
        /// covers the fitted animation sweep, which would make the click box huge).
        static Bounds WorldBounds(SkinnedMeshRenderer s)
        {
            Bounds mb = s.sharedMesh.bounds;
            Matrix4x4 m = s.transform.localToWorldMatrix;
            var b = new Bounds(m.MultiplyPoint3x4(mb.center), Vector3.zero);
            for (int i = 0; i < 8; ++i)
            {
                Vector3 corner = mb.center + Vector3.Scale(mb.extents, new Vector3((i & 1) != 0 ? 1 : -1, (i & 2) != 0 ? 1 : -1, (i & 4) != 0 ? 1 : -1));
                b.Encapsulate(m.MultiplyPoint3x4(corner));
            }
            return b;
        }

        /// The GMMenu's PlayerPermissions in the open scene, or null.
        public static Component FindPermissions()
        {
            Type t = SceneSetup.FindType("Sylan.GMMenu.PlayerPermissions");
            return t != null ? UnityEngine.Object.FindObjectsByType(t, FindObjectsInactive.Include, FindObjectsSortMode.None).FirstOrDefault() as Component : null;
        }

        /// One GW2StartAsPlayer under @p mapRoot, wired to @p permissions (replaced).
        public static void StartAsPlayer(GameObject mapRoot, Component permissions, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(StartAsPlayerName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            Type t = SceneSetup.FindType("Castlemist.GW2StartAsPlayer");
            if (t == null || permissions == null) return;
            var go = new GameObject(StartAsPlayerName);
            go.transform.SetParent(mapRoot.transform, false);
            var comp = SceneSetup.AddUdonAware(go, t);
            SceneSetup.SetField(comp, "permissions", permissions);
            SceneSetup.CopyProxyToUdon(comp);
            warnings.Add("demolish: everyone joins as a GMMenu player and can switch to facilitator / GM in its options; " +
                         "facilitators and GMs can click a demolishable prop");
        }

        /// The UdonSharp assembly definition asset of Castlemist.Runtime and a program asset per
        /// runtime script, made when missing. False without UdonSharp (warned).
        public static bool EnsurePrograms(List<string> warnings)
        {
            Type asmDefT = SceneSetup.FindType("UdonSharpEditor.UdonSharpAssemblyDefinition");
            Type progT = SceneSetup.FindType("UdonSharp.UdonSharpProgramAsset");
            if (asmDefT == null || progT == null)
            {
                warnings.Add("demolish: UdonSharp is not in the project: no demolishable props");
                return false;
            }
            bool made = false;
            string asmdefGuid = AssetDatabase.FindAssets("Castlemist.Runtime t:AssemblyDefinitionAsset").FirstOrDefault();
            if (asmdefGuid != null)
            {
                string asmdefPath = AssetDatabase.GUIDToAssetPath(asmdefGuid);
                string usAsm = System.IO.Path.GetDirectoryName(asmdefPath).Replace('\\', '/') + "/Castlemist.Runtime.UdonSharp.asset";
                if (AssetDatabase.LoadAssetAtPath(usAsm, asmDefT) == null)
                {
                    var a = ScriptableObject.CreateInstance(asmDefT);
                    SceneSetup.SetField(a, "sourceAssembly", AssetDatabase.LoadAssetAtPath<AssemblyDefinitionAsset>(asmdefPath));
                    AssetDatabase.CreateAsset(a, usAsm);
                    made = true;
                }
            }
            foreach (string script in new[] { "GW2Demolishable", "GW2StartAsPlayer" })
            {
                string guid = AssetDatabase.FindAssets(script + " t:MonoScript")
                    .FirstOrDefault(g => System.IO.Path.GetFileNameWithoutExtension(AssetDatabase.GUIDToAssetPath(g)) == script);
                if (guid == null) continue;
                string cs = AssetDatabase.GUIDToAssetPath(guid);
                string asset = cs.Substring(0, cs.Length - 3) + ".asset";
                if (AssetDatabase.LoadAssetAtPath(asset, progT) != null) continue;
                var p = ScriptableObject.CreateInstance(progT);
                SceneSetup.SetField(p, "sourceCsScript", AssetDatabase.LoadAssetAtPath<MonoScript>(cs));
                AssetDatabase.CreateAsset(p, asset);
                made = true;
            }
            if (made)
            {
                AssetDatabase.SaveAssets();
                warnings.Add("demolish: UdonSharp program assets made for Castlemist.Runtime; let UdonSharp compile, then refresh again");
            }
            return true;
        }
    }
}
