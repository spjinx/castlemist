// castlemist world export: the GW2 lighting look for a bake (2026-10-26). Castlemist > GW2 Lighting
// Look, on the open scene. GW2 lights a map with a realtime sun (shadowed), a flat ambient from the
// env fills (SceneSetup's Trilight, Bakery's sky light) and its map / model point lights unshadowed,
// with no bounce light at all; a bake with Bakery's defaults (5 bounces, the sun's bounce, every light
// shadowed and bouncing) lights the map differently. This sets:
//   sun            Realtime, soft shadows, marked realtime again (a Full Lighting bake marks it baked:
//                  no runtime sun, no shadows, no depth texture for the GW2 Haze); its BakeryDirectLight
//                  indirectIntensity 0 (nothing baked); Bakery render mode Indirect
//   bounces        none: Unity's lighting settings 0; Bakery 1 (its Indirect mode refuses 0) with every
//                  Bakery light's indirectIntensity 0, the sky light's too
//   other lights   Unity Light shadows None; BakeryPointLight direct only (bakeToIndirect, the
//                  direct part baked; indirectIntensity 0). Bakery has no per-light shadow switch: its
//                  point lights still shadow, with their small shadowSpread
//   sky light      kept (GW2's ambient)
// Re-bake (Bakery, then the Light Volumes) after.
using System;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class GW2LightingLook
    {
        [MenuItem("Castlemist/GW2 Lighting Look (realtime sun, no bounce)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply() + ": re-bake Bakery and the Light Volumes"); }

        /// The look above on the open scene; returns a one-line report.
        public static string Apply()
        {
            Light sun = RenderSettings.sun;
            Type dl = SceneSetup.FindType("BakeryDirectLight"), pl = SceneSetup.FindType("BakeryPointLight");
            if (sun != null)
            {
                Undo.RecordObject(sun, "GW2 lighting look");
                sun.lightmapBakeType = LightmapBakeType.Realtime;
                sun.shadows = LightShadows.Soft;
                Component d = dl != null ? sun.GetComponent(dl) : null;
                if (d != null)
                {
                    Undo.RecordObject(d, "GW2 lighting look");
                    SceneSetup.SetField(d, "indirectIntensity", 0f);
                    SceneSetup.SetField(d, "bakeToIndirect", false);
                }
                // A Full Lighting bake marks the sun completely baked (ftRenderLightmap.MarkLightAsCompletelyBaked):
                // Unity then drops it at runtime, its shadows and with them the depth texture the GW2 Haze reads.
                // Bakery's own MarkLightAsRealtime output, so it lights again without a re-bake.
                sun.bakingOutput = new LightBakingOutput
                {
                    isBaked = false, lightmapBakeType = LightmapBakeType.Realtime, mixedLightingMode = MixedLightingMode.IndirectOnly,
                    occlusionMaskChannel = -1, probeOcclusionLightIndex = -1,
                };
                ForgetBaked(sun);
            }
            int lights = 0, bakery = 0;
            foreach (Light l in Object.FindObjectsByType<Light>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (l == sun || l.type == LightType.Directional) continue;
                Undo.RecordObject(l, "GW2 lighting look");
                l.shadows = LightShadows.None;
                ++lights;
                Component b = pl != null ? l.GetComponent(pl) : null;
                if (b == null) continue;
                Undo.RecordObject(b, "GW2 lighting look");
                SceneSetup.SetField(b, "bakeToIndirect", true);   // direct baked (Bakery's Indirect render mode skips it otherwise)
                SceneSetup.SetField(b, "indirectIntensity", 0f);
                ++bakery;
            }
            int skies = SkyNoBounce();
            string bounces = Bounces();
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "GW2 lighting look: sun " + (sun != null ? sun.name + " realtime" : "none") + "; " + lights + " light(s) unshadowed, " +
                   bakery + " Bakery point light(s) direct only, " + skies + " sky light(s) direct only; " + bounces;
        }

        /// @p l off every Bakery storage's baked-light list: ftLightmaps re-applies that list (marking the
        /// light completely baked again) whenever the scene loads, until the next bake rewrites it.
        static void ForgetBaked(Light l)
        {
            Type st = SceneSetup.FindType("ftLightmapsStorage");
            if (st == null) return;
            FieldInfo lf = st.GetField("bakedLights", BindingFlags.Public | BindingFlags.Instance);
            PropertyInfo cp = st.GetProperty("bakedLightChannels", BindingFlags.Public | BindingFlags.Instance);
            foreach (Object o in Object.FindObjectsByType(st, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (!(lf?.GetValue(o) is System.Collections.Generic.List<Light> lights)) continue;
                var ch = cp?.GetValue(o) as System.Collections.Generic.List<int>;
                bool changed = false;
                for (int i = lights.Count - 1; i >= 0; --i)
                {
                    if (lights[i] != l) continue;
                    lights.RemoveAt(i);
                    if (ch != null && i < ch.Count) ch.RemoveAt(i);
                    changed = true;
                }
                if (changed) EditorUtility.SetDirty(o);
            }
        }

        /// Every BakerySkyLight's indirectIntensity to 0 (its light lands, never bounces); returns how many.
        static int SkyNoBounce()
        {
            Type sl = SceneSetup.FindType("BakerySkyLight");
            if (sl == null) return 0;
            int n = 0;
            foreach (Component c in Object.FindObjectsByType(sl, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                Undo.RecordObject(c, "GW2 lighting look");
                SceneSetup.SetField(c, "indirectIntensity", 0f);
                ++n;
            }
            return n;
        }

        static string Bounces()
        {
            string unity;
            try
            {
                LightingSettings s = Lightmapping.lightingSettings;
                Undo.RecordObject(s, "GW2 lighting look");
                s.maxBounces = 0;
                EditorUtility.SetDirty(s);
                unity = "Unity bounces " + s.maxBounces;
            }
            catch (Exception e) { unity = "Unity bounces not set (" + e.Message + ")"; }
            Type rl = SceneSetup.FindType("ftRenderLightmap");
            if (rl == null) return unity + "; no Bakery";
            try
            {
                // Bakery's Indirect mode needs a bounce ("Can't render indirect lightmaps, if bounces < 1"):
                // 1, with every Bakery light's indirectIntensity 0 (Apply, SkyNoBounce), so nothing bounces.
                FieldInfo f = rl.GetField("bounces", BindingFlags.Public | BindingFlags.Static);
                if (f != null) f.SetValue(null, 1);
                MethodInfo find = rl.GetMethod("FindRenderSettingsStorage", BindingFlags.Public | BindingFlags.Static);
                object storage = find != null ? find.Invoke(null, new object[] { null }) : null;
                // Render mode Indirect (1): the realtime sun stays realtime. An open Bakery window writes its
                // own fields over the storage when it bakes, so both are set.
                if (storage != null)
                {
                    SceneSetup.SetField(storage, "renderSettingsBounces", 1);
                    SceneSetup.SetField(storage, "renderSettingsUserRenderMode", 1);
                    if (storage is Object so) EditorUtility.SetDirty(so);
                }
                FieldInfo mode = rl.GetField("userRenderMode", BindingFlags.Public | BindingFlags.Instance);
                int windows = 0;
                if (mode != null)
                    foreach (Object w in Resources.FindObjectsOfTypeAll(rl))
                    {
                        mode.SetValue(w, Enum.ToObject(mode.FieldType, 1));
                        ++windows;
                    }
                return unity + ", Bakery bounces 1 (every light's indirect 0), render mode Indirect" + (windows > 0 ? " (open Bakery window too)" : "");
            }
            catch (Exception e) { return unity + "; Bakery bounces not set (" + (e.InnerException ?? e).Message + ")"; }
        }
    }
}
