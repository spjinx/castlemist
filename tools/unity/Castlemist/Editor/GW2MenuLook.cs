// castlemist: the GMMenu (Sylan's com.sylan.gmmenu) in a Guild Wars 2 look, for any world (2026-10-26).
// Castlemist > GMMenu: Add or Restyle adds the menu when the scene has none, then, in place:
//   - restyles it with GW2 UI art: a painted panel with GW2's ragged window mask, overlapping watercolour
//     washes behind the content, brush-stroke buttons (a dark stroke under a faint gold one), ink icons on
//     the tabs, a gold stroke under the selected tab (Runtime/GW2MenuTabs) and cream text. The package's
//     scripts colour some buttons pink at runtime; those use the "GW2 UI Remap" material, whose shader
//     turns the pink into the GW2 palette (Shaders/GW2 UI.shader), so the package stays unedited;
//   - adds a first tab, "Guild", that the menu opens on: the GW2VRC.2917 group with a button that opens
//     its VRChat group page (Runtime/GW2GroupLink);
//   - moves the waypoint map (Editor/WaypointMap.cs), when the world has one, into a "Map" tab;
//   - adds a "Performance" tab after it: view distance, music and sound effect volumes, and local on /
//     off switches for the map's costly parts (fog with everything that reads depth, sun, shadows, point
//     lights, post FX, clouds, particles, foliage; Runtime/GW2Performance), only those the scene has,
//     with "Lowest" and "Default" buttons. Rerun after adding fog, particles or sounds;
//   - adds "Time of day" for GMs to the options: a synced day / night slider (Runtime/GW2DayNightSlider);
//   - adds "View distance" (since 2026-10-29 in the Performance tab): a local camera far clip slider
//     (Runtime/GW2ViewDistance);
//   - adds "Festival" for GMs to the options: Normal / Halloween / ... buttons (Runtime/GW2Festival);
//   - adds "GW2 Menu Hint", the first-join "Tab / right stick" token (Runtime/GW2MenuHint).
// Rerunning rebuilds everything it added ("GW2 ..." objects) and restyles again. The art is GW2's own
// (dat base ids below), exported once per project into Assets/Castlemist/UI/Textures by
//   gw2dat_cli texbatch --dat <Gw2.dat> --base-ids <ids.txt> --mode png --out <folder>
// and renamed gw2ui_<id>.png.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.Events;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class GW2MenuLook
    {
        public const string GroupId = "grp_c5aadfcd-e567-4018-a36a-8f32916a285e";
        public const string GroupName = "GW2VRC";
        public const string GroupCode = "GW2VRC.2917";
        const string UiDir = "Assets/Castlemist/UI";
        const string TexDir = UiDir + "/Textures";
        const string MenuPrefab = "Packages/com.sylan.gmmenu/Samples/Prefab/GMMenu.prefab";
        const string Prefix = "GW2 ";
        const string HintName = "GW2 Menu Hint";

        // GW2 UI art (dat base ids)
        const int Panel = 19238;          // dark painted window panel
        const int PanelMask = 20097;      // ragged window mask
        const int WashBlue = 19299, WashBrown = 19358, WashBlot = 19314, WashMixed = 19294;
        const int Stroke = 22059, Stroke2 = 22073, StrokeSel = 22065, StrokeLine = 22054;   // brush strokes (white on black)
        const int Frame = 21936, DarkFrame = 21944;  // gold / dark square frames
        const int Close = 63375;
        const int IconGuild = 487853, IconAlerts = 487844, IconActions = 487840, IconPlayers = 487838, IconOptions = 487835, IconMap = 487882;
        public static readonly int[] Ids =
        {
            Panel, PanelMask, WashBlue, WashBrown, WashBlot, WashMixed, Stroke, Stroke2, StrokeSel, StrokeLine, Frame, DarkFrame, Close,
            IconGuild, IconAlerts, IconActions, IconPlayers, IconOptions, IconMap,
        };

        static readonly Color Cream = new Color(0.92f, 0.87f, 0.76f, 1f);
        static readonly Color Gold = new Color(0.88f, 0.72f, 0.42f, 1f);
        static readonly Color Dark = new Color(0.1f, 0.11f, 0.13f, 0.88f);
        static readonly Color Slate = new Color(0.26f, 0.28f, 0.31f, 0.92f);    // button strokes on the dark panel
        static readonly Color Ember = new Color(0.55f, 0.38f, 0.18f, 0.95f);    // the guild's join button
        static readonly Color GoldActive = new Color(0.79f, 0.61f, 0.32f, 0.95f);
        static readonly Color GoldWash = new Color(0.88f, 0.72f, 0.42f, 0.3f);

        static Material plain, stroke, panelMat, remap, overlay, overlayStroke;

        [MenuItem("Castlemist/GMMenu: Add or Restyle (GW2 look, guild tab, join hint)")]
        public static void Menu()
        {
            var notes = new List<string>();
            string err = Apply(notes);
            if (err != null) { EditorUtility.DisplayDialog("GW2 GMMenu", err, "OK"); return; }
            Debug.Log("CASTLEMIST GMMenu GW2 look:\n" + string.Join("\n", notes));
        }

        /// Restyles (adding first when missing) the open scene's GMMenu; null, or why it could not.
        public static string Apply(List<string> notes)
        {
            Type menuType = SceneSetup.FindType("Sylan.GMMenu.GMMenu");
            Type tabType = SceneSetup.FindType("Sylan.GMMenu.MenuTab");
            Type toggleType = SceneSetup.FindType("Sylan.GMMenu.GMMenuToggle");
            Type hintType = SceneSetup.FindType("Castlemist.GW2MenuHint");
            if (menuType == null || tabType == null || toggleType == null) return "The GMMenu package (com.sylan.gmmenu) is not in this project.";
            if (hintType == null) return "Castlemist's runtime scripts are not compiled yet (UdonSharp): try again once they have.";
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "UdonSharp is not in this project.";
            if (made.Count > 0) return "UdonSharp program assets were made for Castlemist's runtime scripts: run this again once UdonSharp has compiled.";
            string missing = string.Join(", ", Ids.Where(id => !File.Exists(TexPath(id))).Select(id => id.ToString()));
            if (missing.Length > 0)
                return "GW2 UI art missing in " + TexDir + ": " + missing + ".\nExport it with gw2dat_cli texbatch --mode png and name the files gw2ui_<id>.png.";
            ImportTextures();
            MakeMaterials();

            Component menu = Object.FindObjectsOfType(menuType, true).Cast<Component>().FirstOrDefault();
            if (menu == null)
            {
                var prefab = AssetDatabase.LoadAssetAtPath<GameObject>(MenuPrefab);
                if (prefab == null) return "No GMMenu in the scene and no prefab at " + MenuPrefab + ".";
                var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
                Undo.RegisterCreatedObjectUndo(go, "Add GMMenu");
                menu = go.GetComponent(menuType);
                notes.Add("added the GMMenu prefab");
            }
            GameObject root = menu.gameObject;
            if (PrefabUtility.IsPartOfPrefabInstance(root))
            {
                PrefabUtility.UnpackPrefabInstance(PrefabUtility.GetOutermostPrefabInstanceRoot(root), PrefabUnpackMode.Completely, InteractionMode.AutomatedAction);
                notes.Add("unpacked the GMMenu prefab instance (the new tab goes first, which a prefab instance does not allow)");
            }
            Undo.RegisterFullObjectHierarchyUndo(root, "GW2 GMMenu look");

            Transform canvas = root.transform.Find("GMCanvas");
            Transform tabs = canvas != null ? canvas.Find("MenuTabs") : null;
            Transform panels = canvas != null ? canvas.Find("Panels") : null;
            if (tabs == null || panels == null) return "This GMMenu has no GMCanvas/MenuTabs or GMCanvas/Panels.";
            Transform map = ReclaimMap(root);
            foreach (Transform t in root.GetComponentsInChildren<Transform>(true).Where(t => t != null && t.name.StartsWith(Prefix)).ToList())
                if (t != null) Undo.DestroyObjectImmediate(t.gameObject);

            Restyle(root, canvas, notes);
            Washes(canvas);
            AddGuildTab(tabs, panels, tabType, notes);
            AddMapTab(tabs, panels, tabType, map, notes);
            AddPerformanceTab(root, tabs, panels, tabType, notes);
            TabLook(root, canvas.GetComponent<Canvas>(), tabs, tabType);
            AddDayNight(root, canvas, notes);
            AddFestival(root, canvas, notes);
            AddHint(root, toggleType, hintType, notes);

            EditorSceneManager.MarkSceneDirty(root.scene);
            return null;
        }

        // ---- art -------------------------------------------------------------------------------------

        static string TexPath(int id) { return TexDir + "/gw2ui_" + id + ".png"; }
        static Sprite Spr(int id) { return AssetDatabase.LoadAssetAtPath<Sprite>(TexPath(id)); }

        static void ImportTextures()
        {
            foreach (int id in Ids)
            {
                var ti = AssetImporter.GetAtPath(TexPath(id)) as TextureImporter;
                if (ti == null) continue;
                Vector4 border = id == Frame || id == DarkFrame ? new Vector4(18, 18, 18, 18) : Vector4.zero;
                bool mask = id == PanelMask || id == Stroke || id == Stroke2 || id == StrokeSel || id == StrokeLine;   // read as alpha: linear
                if (ti.textureType == TextureImporterType.Sprite && ti.spriteBorder == border && ti.wrapMode == TextureWrapMode.Clamp && ti.sRGBTexture != mask) continue;
                ti.sRGBTexture = !mask;
                ti.textureType = TextureImporterType.Sprite;
                ti.spriteImportMode = SpriteImportMode.Single;
                ti.alphaIsTransparency = true;
                ti.mipmapEnabled = true;
                ti.wrapMode = TextureWrapMode.Clamp;
                ti.maxTextureSize = 1024;
                ti.spriteBorder = border;
                ti.SaveAndReimport();
            }
        }

        static Material Mat(string name, Action<Material> setup)
        {
            string path = UiDir + "/" + name + ".mat";
            var m = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (m == null)
            {
                m = new Material(Shader.Find("Castlemist/GW2 UI"));
                AssetDatabase.CreateAsset(m, path);
            }
            setup(m);
            EditorUtility.SetDirty(m);
            return m;
        }

        static void MakeMaterials()
        {
            plain = Mat("GW2 UI", m => { });
            stroke = Mat("GW2 UI Stroke", m => m.SetFloat("_Stroke", 1f));
            panelMat = Mat("GW2 UI Panel", m => m.SetTexture("_MaskTex", AssetDatabase.LoadAssetAtPath<Texture2D>(TexPath(PanelMask))));
            remap = Mat("GW2 UI Remap", m =>
            {
                m.SetFloat("_Remap", 1f);
                m.SetFloat("_Stroke", 1f);
                m.SetColor("_InactiveColor", Slate);
                m.SetColor("_ActiveColor", GoldActive);
                m.SetColor("_SelectedColor", new Color(0.95f, 0.84f, 0.58f, 0.9f));
            });
            overlay = Mat("GW2 UI Overlay", m => m.SetFloat("_ZTest", (float)UnityEngine.Rendering.CompareFunction.Always));
            overlayStroke = Mat("GW2 UI Overlay Stroke", m =>
            {
                m.SetFloat("_ZTest", (float)UnityEngine.Rendering.CompareFunction.Always);
                m.SetFloat("_Stroke", 1f);
            });
            AssetDatabase.SaveAssets();
        }

        // ---- restyle the stock menu ------------------------------------------------------------------

        static void Restyle(GameObject root, Transform canvas, List<string> notes)
        {
            // images the package recolours at runtime: every Image field of these behaviours
            var scripted = new HashSet<Image>();
            var names = new HashSet<string> { "MenuOpenSettings", "PermissionOptionButtons", "NoclipToggle", "PanelTemplate", "MessagePanel", "PlayerPanel" };
            foreach (MonoBehaviour b in root.GetComponentsInChildren<MonoBehaviour>(true))
            {
                if (b == null || !names.Contains(b.GetType().Name)) continue;
                var so = new SerializedObject(b);
                SerializedProperty it = so.GetIterator();
                while (it.Next(true))
                    if (it.propertyType == SerializedPropertyType.ObjectReference && it.objectReferenceValue is Image img) scripted.Add(img);
            }

            var bg = canvas.GetComponent<Image>();
            if (bg != null) bg.color = new Color(1f, 1f, 1f, 0f);   // still blocks raycasts; the painted panel is "GW2 Washes/Panel"

            int styled = 0;
            foreach (Image img in canvas.GetComponentsInChildren<Image>(true))
            {
                if (img == bg || img.GetComponent<Mask>() != null && img.sprite == null) continue;
                if (scripted.Contains(img))
                {
                    SetLook(img, Spr(Stroke), img.color, remap);
                    ++styled;
                    continue;
                }
                string role = Role(img);
                if (role == "fill") SetLook(img, Spr(Stroke), new Color(Slate.r, Slate.g, Slate.b, Mathf.Min(Slate.a, Slate.a * img.color.a / 0.5f)), stroke);
                else if (role == "outline") SetLook(img, Spr(Stroke2), GoldWash, stroke);
                else if (role == "on") SetLook(img, Spr(Stroke), GoldActive, stroke);
                else if (role == "track") { img.color = new Color(0f, 0f, 0f, 0.3f); img.material = plain; }
                else if (role == "handle") { img.color = new Color(Gold.r, Gold.g, Gold.b, 0.65f); img.material = plain; }
                else continue;
                ++styled;
            }

            foreach (Text t in canvas.GetComponentsInChildren<Text>(true))
            {
                t.color = new Color(Cream.r, Cream.g, Cream.b, t.color.a);
                if (t.GetComponent<Shadow>() == null) Undo.AddComponent<Shadow>(t.gameObject);
                var sh = t.GetComponent<Shadow>();
                sh.effectColor = new Color(0f, 0f, 0f, 0.6f);
                sh.effectDistance = new Vector2(2f, -2f);
            }

            // the close button: GW2's X
            Transform x = canvas.Find("MenuTabs/XButton");
            if (x != null)
            {
                Transform label = x.Find("Text");
                if (label != null) label.gameObject.SetActive(false);
                Image icon = Img(Prefix + "Close", x, Spr(Close), Cream, plain);
                Place(icon.rectTransform, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f), Vector2.zero, new Vector2(34f, 34f));
            }
            notes.Add("restyled " + styled + " image(s); " + scripted.Count + " recoloured by the package use the remap material");
        }

        /// What a stock GMMenu image is, by its colour: pink / light pink first time, dark / gold on a rerun.
        static string Role(Image img)
        {
            Color c = img.color;
            if (img.name == "ImageOn") return "on";                                       // a toggle's on graphic
            if (img.sprite != null && img.sprite.name.StartsWith("gw2ui_")) return c.r > 0.5f ? "outline" : "fill";
            if (img.GetComponentInParent<Scrollbar>(true) != null)
                return img.GetComponent<Scrollbar>() != null ? "track" : "handle";
            if (c.r > 0.55f && c.b > 0.5f && c.g < 0.5f) return "fill";                   // 994C8C, BA33FF, EB58FF
            if (c.r > 0.95f && c.g > 0.75f && c.g < 0.85f && c.b > 0.9f) return "outline"; // FFCCF7
            if (c.r < 0.3f && c.g < 0.3f && c.b < 0.33f && c.a > 0f) return "fill";         // restyled, art gone
            if (c.r > 0.8f && c.g > 0.6f && c.b < 0.5f && c.a < 0.5f) return "outline";
            return null;
        }

        static void SetLook(Image img, Sprite s, Color c, Material m)
        {
            img.sprite = s;
            img.type = Image.Type.Simple;
            img.preserveAspect = false;
            img.color = c;
            img.material = m;
        }

        static void Washes(Transform canvas)
        {
            RectTransform w = Rect(Prefix + "Washes", canvas);
            w.SetSiblingIndex(0);
            Place(w, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            // the painted panel, larger than the canvas so GW2's ragged edge falls outside the content
            Image panel = Img("Panel", w, Spr(Panel), new Color(1f, 1f, 1f, 0.97f), panelMat);
            Place(panel.rectTransform, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f), new Vector2(0f, 6f), new Vector2(1000f, 900f));
            // overlapping washes, bleeding past the panel edge as GW2's painted windows do
            Wash(w, WashBlue, new Vector2(0.5f, 0.5f), new Vector2(0f, 0f), new Vector2(980f, 900f), 0f, 0.6f);
            Wash(w, WashBrown, new Vector2(0f, 1f), new Vector2(160f, -150f), new Vector2(620f, 560f), 0f, 0.5f);
            Wash(w, WashBlot, new Vector2(1f, 0f), new Vector2(-170f, 170f), new Vector2(640f, 600f), 180f, 0.55f);
            Wash(w, WashMixed, new Vector2(0.5f, 1f), new Vector2(0f, -40f), new Vector2(900f, 260f), 0f, 0.4f);
        }

        static void Wash(Transform parent, int id, Vector2 anchor, Vector2 pos, Vector2 size, float rot, float alpha)
        {
            Image i = Img(Prefix + "Wash " + id, parent, Spr(id), new Color(1f, 1f, 1f, alpha), plain);
            Place(i.rectTransform, anchor, anchor, pos, size);
            i.rectTransform.localEulerAngles = new Vector3(0f, 0f, rot);
        }

        // ---- tabs ------------------------------------------------------------------------------------

        static void AddGuildTab(Transform tabs, Transform panels, Type tabType, List<string> notes)
        {
            RectTransform panel = AddTab(tabs, panels, tabType, "Guild", 0);
            if (panel == null) { notes.Add("guild tab: no MenuTab to copy"); return; }
            BuildGuildPanel(panel);
            notes.Add("guild tab: first tab, opens on " + GroupCode + " (" + GroupId + ")");
        }

        /// A new tab "GW2 <name>" at @p index with its empty panel "GW2 <name>Panel" (returned; null
        /// without a tab to copy). The tab is a copy of the last one (Options: no permission filter).
        static RectTransform AddTab(Transform tabs, Transform panels, Type tabType, string name, int index)
        {
            Transform options = tabs.Cast<Transform>().LastOrDefault(t => t.GetComponent(tabType) != null);
            if (options == null) return null;
            RectTransform panel = Rect(Prefix + name + "Panel", panels);
            Place(panel, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            var group = panel.gameObject.AddComponent<CanvasGroup>();
            group.alpha = 0f; group.interactable = false; group.blocksRaycasts = false;

            var tab = Object.Instantiate(options.gameObject, tabs);
            tab.name = Prefix + name;
            tab.transform.SetSiblingIndex(index);
            Undo.RegisterCreatedObjectUndo(tab, "GW2 tab");
            foreach (Component c in tab.GetComponents<Component>().Where(c => c != null && c.GetType().Name == "ToggleByPermission").ToList())
                Object.DestroyImmediate(c);
            Component mt = tab.GetComponent(tabType);
            var so = new SerializedObject(mt);
            so.FindProperty("panel").objectReferenceValue = group;
            so.ApplyModifiedPropertiesWithoutUndo();
            SceneSetup.CopyProxyToUdon(mt);
            Text label = tab.GetComponentsInChildren<Text>(true).FirstOrDefault();
            if (label != null) label.text = name;
            return panel;
        }

        // ---- the waypoint map as the "Map" tab -------------------------------------------------------

        const float MarkerScale = 1.8f;   // the map's waypoint buttons, enlarged inside the menu

        /// Hosts WaypointMap's "GW2 Waypoint Map" (Editor/WaypointMap.cs) in a "Map" tab after Guild:
        /// its own canvas, laser shape and collider go (the menu's do that job), it is scaled to fit and
        /// its waypoint buttons are enlarged. Its buttons keep calling GW2Waypoints._TP<k>.
        static void AddMapTab(Transform tabs, Transform panels, Type tabType, Transform map, List<string> notes)
        {
            if (map == null) return;
            RectTransform panel = AddTab(tabs, panels, tabType, "Map", 1);
            if (panel == null) return;
            var mrt = (RectTransform)map;
            Vector2 size = mrt.sizeDelta;
            float s = Mathf.Min(740f / size.x, 560f / size.y);
            Image frame = Img("Frame", panel, Spr(Frame), Gold, plain);
            frame.type = Image.Type.Sliced;
            Place(frame.rectTransform, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f), new Vector2(0f, 20f), size * s + new Vector2(16f, 16f));
            foreach (Component c in map.GetComponents<Component>().Where(c => c != null && !(c is Transform)).Reverse().ToList())
                Undo.DestroyObjectImmediate(c);
            Undo.SetTransformParent(map, panel, "GW2 map tab");
            Place(mrt, new Vector2(0.5f, 0.5f), new Vector2(0.5f, 0.5f), new Vector2(0f, 20f), size);
            mrt.anchoredPosition3D = new Vector3(0f, 20f, 0f);   // reparenting kept its world depth
            map.localRotation = Quaternion.identity;
            map.localScale = Vector3.one * s;
            foreach (Transform b in Icons(map)) b.localScale = Vector3.one * MarkerScale;
            Text hint = Txt("Hint", panel, "Click a waypoint to travel there.", 24, new Color(Cream.r, Cream.g, Cream.b, 0.7f), TextAnchor.MiddleCenter);
            Place(hint.rectTransform, new Vector2(0f, 0f), new Vector2(1f, 0f), new Vector2(0f, 30f), new Vector2(-40f, 30f));
            notes.Add("map tab: " + Icons(map).Count() + " marker(s) from " + WaypointMap.PanelName);
        }

        // ---- the "Performance" tab ---------------------------------------------------------------------

        /// Shaders that read the camera depth texture with no fallback when it is missing: switched with the
        /// fog, so turning Fog off leaves nothing that needs a depth pass.
        static readonly string[] DepthShaders = { "Castlemist/GW2 Haze", "Castlemist/GW2 Distort" };

        /// The scene's objects for each GW2Performance switch (its Fog .. Foliage order); Sun and Shadows
        /// act on the lights, so they list none. The cell switcher's particles are its own (SetParticles),
        /// as are the light streamer's point lights (SetItems). @p renderers gets the depth readers' renderers.
        static List<GameObject>[] PerformanceTargets(UnityEngine.SceneManagement.Scene scene, Component switcher, Component streamer,
                                                     List<Renderer> renderers)
        {
            var lists = new List<GameObject>[10];
            for (int i = 0; i < lists.Length; ++i) lists[i] = new List<GameObject>();
            var owned = new HashSet<GameObject>();
            if (switcher != null && switcher.GetType().GetField("particles")?.GetValue(switcher) is GameObject[] ps)
                foreach (GameObject g in ps) if (g != null) owned.Add(g);
            var streamed = new HashSet<GameObject>();
            if (streamer != null && streamer.GetType().GetField("items")?.GetValue(streamer) is GameObject[] items)
                foreach (GameObject g in items) if (g != null) streamed.Add(g);
            Type ppv = SceneSetup.FindType("UnityEngine.Rendering.PostProcessing.PostProcessVolume");
            GameObject fields = null;
            foreach (Transform t in scene.GetRootGameObjects().SelectMany(g => g.GetComponentsInChildren<Transform>(true)))
            {
                GameObject g = t.gameObject;
                if (t.name == "LightVolumeFog") lists[0].Add(g);
                if (!streamed.Contains(g) && t.GetComponents<Component>().Any(c => c != null && c.GetType().Name == "PointLightVolumeInstance")) lists[3].Add(g);
                if (ppv != null && t.GetComponent(ppv) != null) lists[4].Add(g);
                Renderer r = t.GetComponent<Renderer>();
                if (r != null && r.sharedMaterials.Any(m => m != null && m.shader != null && DepthShaders.Contains(m.shader.name))) renderers.Add(r);
                if (t.name == GW2Clouds.GroupName) lists[6].Add(g);
                if (t.name == "GW2 Particle Fields") { lists[7].Add(g); fields = g; }
                else if (t.GetComponent<ParticleSystem>() != null && !owned.Contains(g) &&
                         (t.parent == null || t.parent.GetComponentInParent<ParticleSystem>(true) == null) &&
                         t.GetComponentInParent<Canvas>(true) == null)
                    lists[7].Add(g);
                if (t.name == "Foliage" && t.parent != null && t.parent.name.StartsWith("Cell_")) lists[8].Add(g);
                if (t.name == SunGlow.ObjectName ||
                    (r != null && r.sharedMaterial != null && r.sharedMaterial.shader != null && r.sharedMaterial.shader.name == SunGlow.SilentShader))
                    lists[9].Add(g);
            }
            // A particle system under the particle fields group goes with the group.
            if (fields != null) lists[7] = lists[7].Where(g => g == fields || !g.transform.IsChildOf(fields.transform)).ToList();
            return lists;
        }

        /// A "Performance" tab after Guild / Map, scrolling: View Distance, Music and Sound Effects sliders,
        /// then one on / off button per switch the scene can use (Runtime/GW2Performance), and "Lowest" /
        /// "Default" below. Local, saved per player.
        static void AddPerformanceTab(GameObject root, Transform tabs, Transform panels, Type tabType, List<string> notes)
        {
            Type pt = SceneSetup.FindType("Castlemist.GW2Performance");
            if (pt == null) { notes.Add("performance tab: GW2Performance is not compiled yet"); return; }
            Transform mapTab = tabs.Find(Prefix + "Map");
            RectTransform panel = AddTab(tabs, panels, tabType, "Performance", mapTab != null ? mapTab.GetSiblingIndex() + 1 : 1);
            if (panel == null) { notes.Add("performance tab: no MenuTab to copy"); return; }

            Type swType = SceneSetup.FindType("Castlemist.GW2CellSwitcher");
            Component switcher = swType != null ? Object.FindObjectsOfType(swType, true).Cast<Component>().FirstOrDefault(c => c.gameObject.scene == root.scene) : null;
            Type stType = SceneSetup.FindType("Castlemist.GW2Streamer");
            Component streamer = stType != null ? Object.FindObjectsOfType(stType, true).Cast<Component>().FirstOrDefault(c => c.gameObject.scene == root.scene) : null;
            var depthRenderers = new List<Renderer>();
            List<GameObject>[] lists = PerformanceTargets(root.scene, switcher, streamer, depthRenderers);
            Light sun = RenderSettings.sun;
            Transform av = sun != null ? sun.transform.Find(ImmersiveLighting.AvatarSunName) : null;
            string[] names = { "Fog", "Sun", "Sun Shadows", "Point Lights", "Post FX", "Haze", "Clouds", "Particles", "Foliage", "Sun Flare" };
            bool[] has = new bool[names.Length];
            for (int i = 0; i < names.Length; ++i) has[i] = lists[i].Count > 0;
            has[0] |= depthRenderers.Count > 0;
            has[1] = sun != null;
            has[2] = sun != null && sun.shadows != LightShadows.None;
            has[3] |= streamer != null;
            has[5] = false;   // folded into Fog
            has[7] |= switcher != null;

            Text title = Txt("Title", panel, "Performance", 48, Gold, TextAnchor.MiddleCenter);
            Place(title.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -44f), new Vector2(-40f, 56f));
            Text sub = Txt("Subtitle", panel, "Only changes your own view and sound; remembered next visit. Scroll for more.",
                           20, new Color(Cream.r, Cream.g, Cream.b, 0.7f), TextAnchor.MiddleCenter);
            Place(sub.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -86f), new Vector2(-60f, 28f));

            // A scrolling list: the sliders, then the switches.
            RectTransform viewport = Rect("Viewport", panel);
            Place(viewport, Vector2.zero, Vector2.one, new Vector2(0f, 5f), new Vector2(-40f, -210f));
            viewport.gameObject.AddComponent<RectMask2D>();
            Image hit = Img("Hit", viewport, null, new Color(0f, 0f, 0f, 0f), plain);   // drag anywhere to scroll
            hit.raycastTarget = true;
            Place(hit.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            RectTransform content = Rect("Content", viewport);
            content.anchorMin = new Vector2(0f, 1f); content.anchorMax = new Vector2(1f, 1f); content.pivot = new Vector2(0.5f, 1f);
            content.anchoredPosition = Vector2.zero; content.sizeDelta = Vector2.zero;
            var vl = content.gameObject.AddComponent<VerticalLayoutGroup>();
            vl.spacing = 6f;
            vl.padding = new RectOffset(0, 0, 4, 10);
            vl.childControlWidth = vl.childControlHeight = true;
            vl.childForceExpandWidth = true; vl.childForceExpandHeight = false;
            content.gameObject.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            var scroll = viewport.gameObject.AddComponent<ScrollRect>();
            scroll.content = content; scroll.viewport = viewport;
            scroll.horizontal = false; scroll.vertical = true;
            scroll.movementType = ScrollRect.MovementType.Clamped;
            scroll.scrollSensitivity = 30f;

            AddViewDistance(content, notes);
            Component comp = SceneSetup.AddUdonAware(panel.gameObject, pt);

            // Music and sound effects.
            Type mpType = SceneSetup.FindType("Castlemist.GW2MusicPlayer");
            Type spType = SceneSetup.FindType("Castlemist.GW2SoundPlayer");
            Component[] musics = mpType != null ? Object.FindObjectsOfType(mpType, true).Cast<Component>().Where(c => c.gameObject.scene == root.scene).ToArray() : new Component[0];
            Component[] players = spType != null ? Object.FindObjectsOfType(spType, true).Cast<Component>().Where(c => c.gameObject.scene == root.scene).ToArray() : new Component[0];
            var driven = new HashSet<AudioSource>();
            foreach (Component m in musics) if (mpType.GetField("source")?.GetValue(m) is AudioSource a) driven.Add(a);
            foreach (Component p in players)
                if (spType.GetField("sources")?.GetValue(p) is AudioSource[] ss) foreach (AudioSource a in ss) if (a != null) driven.Add(a);
            AudioSource[] sfx = root.scene.GetRootGameObjects().SelectMany(g => g.GetComponentsInChildren<AudioSource>(true))
                                    .Where(a => !driven.Contains(a)).ToArray();
            if (musics.Length > 0)
            {
                SliderRow(content, "Music", content.childCount, out Text ml, out Slider ms, out RectTransform _);
                ms.value = 1f;
                SceneSetup.SetField(comp, "musicSlider", ms);
                SceneSetup.SetField(comp, "musicLabel", ml);
                Wire(ms.onValueChanged, comp, "_OnMusic");
            }
            if (players.Length > 0 || sfx.Length > 0)
            {
                SliderRow(content, "Sound Effects", content.childCount, out Text sl, out Slider ss2, out RectTransform _);
                ss2.value = 1f;
                SceneSetup.SetField(comp, "sfxSlider", ss2);
                SceneSetup.SetField(comp, "sfxLabel", sl);
                Wire(ss2.onValueChanged, comp, "_OnSfx");
            }
            SceneSetup.SetField(comp, "music", ToTyped(musics, mpType));
            SceneSetup.SetField(comp, "sfxSources", sfx);

            // The switches, two to a row.
            RectTransform grid = Rect("Switches", content);
            var gl = grid.gameObject.AddComponent<GridLayoutGroup>();
            gl.cellSize = new Vector2(330f, 60f);
            gl.spacing = new Vector2(24f, 10f);
            gl.padding = new RectOffset(0, 0, 10, 0);
            gl.childAlignment = TextAnchor.UpperCenter;
            gl.constraint = GridLayoutGroup.Constraint.FixedColumnCount;
            gl.constraintCount = 2;
            var labels = new Text[names.Length];
            var shown = new List<string>();
            for (int i = 0; i < names.Length; ++i)
            {
                if (!has[i]) continue;
                Button b = MakeButton(names[i], grid, names[i] + ": On", 28, Cream);
                labels[i] = b.transform.Find("Text").GetComponent<Text>();
                Wire(b, comp, "_T" + i);
                shown.Add(names[i]);
            }
            int rows = (shown.Count + 1) / 2;
            grid.gameObject.AddComponent<LayoutElement>().preferredHeight = 10f + rows * 60f + Mathf.Max(rows - 1, 0) * 10f;

            RectTransform presets = Rect("Presets", panel);
            Place(presets, new Vector2(0.5f, 0f), new Vector2(0.5f, 0f), new Vector2(0f, 55f), new Vector2(700f, 70f));
            var ph = presets.gameObject.AddComponent<HorizontalLayoutGroup>();
            ph.spacing = 30f;
            ph.childControlWidth = ph.childControlHeight = true;
            ph.childForceExpandWidth = ph.childForceExpandHeight = true;
            Button low = MakeButton("Lowest", presets, "Lowest (all off)", 28, Cream);
            low.transform.Find("Fill").GetComponent<Image>().color = Ember;
            Wire(low, comp, "_Lowest");
            Button def = MakeButton("Default", presets, "Default (all on)", 28, Gold);
            Wire(def, comp, "_Default");

            var targets = new List<GameObject>();
            var which = new List<int>();
            for (int i = 0; i < names.Length; ++i)
                foreach (GameObject g in lists[i]) { targets.Add(g); which.Add(i == 5 ? 0 : i); }
            SceneSetup.SetField(comp, "names", names);
            SceneSetup.SetField(comp, "targets", targets.ToArray());
            SceneSetup.SetField(comp, "targetSwitch", which.ToArray());
            SceneSetup.SetField(comp, "flareNeedsDepth", lists[9].Any(g => g.activeSelf && g.name != SunGlow.ObjectName));
            SceneSetup.SetField(comp, "renderTargets", depthRenderers.ToArray());
            SceneSetup.SetField(comp, "renderSwitch", new int[depthRenderers.Count]);   // all Fog (0)
            SceneSetup.SetField(comp, "sun", sun);
            SceneSetup.SetField(comp, "avatarSun", av != null ? av.GetComponent<Light>() : null);
            if (switcher != null) SceneSetup.SetField(comp, "cellSwitcher", switcher);
            if (streamer != null) SceneSetup.SetField(comp, "lightStreamer", streamer);
            SceneSetup.SetField(comp, "labels", labels);
            SceneSetup.CopyProxyToUdon(comp);
            foreach (Component p in players)
            {
                SceneSetup.SetField(p, "settings", comp);
                SceneSetup.CopyProxyToUdon(p);
            }
            notes.Add("performance tab: view distance, " + (musics.Length > 0 ? "music, " : "") + "sound effects (" + players.Length + " sound player(s), " +
                      sfx.Length + " other source(s)); " + string.Join(", ", shown) + " (" + targets.Count + " object(s), " + depthRenderers.Count + " depth renderer(s) with Fog)");
        }

        /// @p items as an array of @p t (a field of type GW2MusicPlayer[] takes no Component[]).
        static Array ToTyped(Component[] items, Type t)
        {
            if (t == null) return null;
            Array a = Array.CreateInstance(t, items.Length);
            for (int i = 0; i < items.Length; ++i) a.SetValue(items[i], i);
            return a;
        }

        /// The map's icons to enlarge: the waypoint buttons and each marker inside a group (MapMarkers'
        /// "GW2 Markers"). Not the groups themselves: a full-size group scaled about the map's centre threw
        /// its markers out toward the corners (2026-10-29).
        static IEnumerable<Transform> Icons(Transform map)
        {
            foreach (Transform b in map)
            {
                if (b.name == "Map") continue;
                if (b.name == MapMarkers.GroupName) { foreach (Transform m in b) yield return m; }
                else yield return b;
            }
        }

        /// Takes the waypoint map out of the menu, back to its GW2Waypoints group as WaypointMap built
        /// it (a world canvas), before the menu's "GW2 ..." objects are rebuilt; returns the map to
        /// host, or null. A newer map already in the group (waypoints re-imported) wins.
        static Transform ReclaimMap(GameObject menuRoot)
        {
            Type wpType = SceneSetup.FindType("Castlemist.GW2Waypoints");
            Component wp = wpType != null ? Object.FindObjectsOfType(wpType, true).Cast<Component>().FirstOrDefault(c => c.gameObject.scene == menuRoot.scene) : null;
            Transform hosted = menuRoot.GetComponentsInChildren<Transform>(true).FirstOrDefault(t => t.name == WaypointMap.PanelName);
            Transform fresh = wp != null ? wp.transform.Find(WaypointMap.PanelName) : null;
            if (hosted == null) return fresh;
            if (fresh != null || wp == null)
            {
                if (fresh != null) { Undo.DestroyObjectImmediate(hosted.gameObject); return fresh; }
                Undo.SetTransformParent(hosted, null, "GW2 map tab");   // no group to return to: keep it
            }
            else Undo.SetTransformParent(hosted, wp.transform, "GW2 map tab");
            hosted.gameObject.AddComponent<Canvas>().renderMode = RenderMode.WorldSpace;
            hosted.gameObject.AddComponent<GraphicRaycaster>();
            Type shape = SceneSetup.FindType("VRC.SDK3.Components.VRCUiShape");
            if (shape != null) hosted.gameObject.AddComponent(shape);
            foreach (Transform b in hosted) b.localScale = Vector3.one;   // also undoes the old enlarged GW2 Markers group
            foreach (Transform b in Icons(hosted)) b.localScale = Vector3.one;
            return hosted;
        }

        static void BuildGuildPanel(RectTransform panel)
        {
            Image frame = Img("Emblem Frame", panel, Spr(Frame), Gold, plain);
            frame.type = Image.Type.Sliced;
            Place(frame.rectTransform, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f), new Vector2(0f, -120f), new Vector2(170f, 170f));
            Image wash = Img("Emblem Wash", panel, Spr(WashBrown), new Color(1f, 1f, 1f, 0.55f), plain);
            Place(wash.rectTransform, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f), new Vector2(0f, -120f), new Vector2(150f, 150f));
            Image icon = Img("Emblem", panel, Spr(IconGuild), Cream, plain);
            Place(icon.rectTransform, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f), new Vector2(0f, -120f), new Vector2(130f, 130f));

            Text title = Txt("Title", panel, GroupName, 72, Gold, TextAnchor.MiddleCenter);
            Place(title.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -258f), new Vector2(-40f, 84f));
            Text sub = Txt("Subtitle", panel, "Guild Wars 2 in VRChat  ·  " + GroupCode, 30, Cream, TextAnchor.MiddleCenter);
            Place(sub.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -318f), new Vector2(-40f, 40f));
            Image rule = Img("Rule", panel, Spr(StrokeLine), new Color(Gold.r, Gold.g, Gold.b, 0.6f), stroke);
            Place(rule.rectTransform, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f), new Vector2(0f, -350f), new Vector2(520f, 14f));
            Text body = Txt("Body", panel, "Join the group to hear about new Tyria maps and events.", 30, Cream, TextAnchor.MiddleCenter);
            Place(body.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -392f), new Vector2(-80f, 60f));

            Button join = MakeButton("Join Button", panel, "Join the Guild", 40, Gold);
            join.transform.Find("Fill").GetComponent<Image>().color = Ember;
            join.transform.Find("Text").GetComponent<Text>().color = Cream;
            Place((RectTransform)join.transform, new Vector2(0.5f, 1f), new Vector2(0.5f, 1f), new Vector2(0f, -480f), new Vector2(440f, 92f));
            Type linkType = SceneSetup.FindType("Castlemist.GW2GroupLink");
            if (linkType != null)
            {
                Component link = SceneSetup.AddUdonAware(join.gameObject, linkType);
                SceneSetup.SetField(link, "groupId", GroupId);
                SceneSetup.CopyProxyToUdon(link);
                Wire(join, link, "OpenGroup");
            }
            Text note = Txt("Link", panel, "Opens the group page in VRChat  ·  vrc.group/" + GroupCode, 22, new Color(Cream.r, Cream.g, Cream.b, 0.75f), TextAnchor.MiddleCenter);
            Place(note.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -548f), new Vector2(-40f, 30f));
            Text next = Txt("Next", panel, "Use the tabs above for the rest of the menu.", 24, new Color(Cream.r, Cream.g, Cream.b, 0.6f), TextAnchor.MiddleCenter);
            Place(next.rectTransform, new Vector2(0f, 0f), new Vector2(1f, 0f), new Vector2(0f, 34f), new Vector2(-40f, 30f));
        }

        static void TabLook(GameObject root, Canvas canvas, Transform tabs, Type tabType)
        {
            var icons = new Dictionary<string, int>
            {
                { Prefix + "Guild", IconGuild }, { Prefix + "Map", IconMap }, { Prefix + "Performance", IconOptions }, { "Alerts", IconAlerts }, { "Player Actions", IconActions },
                { "Player List Button", IconPlayers }, { "Options", IconOptions },
            };
            var panels = new List<CanvasGroup>();
            var lights = new List<GameObject>();
            foreach (Transform tab in tabs)
            {
                Component mt = tab.GetComponent(tabType);
                if (mt == null) continue;
                Button b = tab.GetComponentInChildren<Button>(true);
                if (b == null) continue;
                Transform outline = b.transform.Find("Outline");
                if (outline != null) outline.GetComponent<Image>().color = new Color(Gold.r, Gold.g, Gold.b, 0.12f);
                Image light = Img(Prefix + "Selected", b.transform, Spr(StrokeSel), new Color(Gold.r, Gold.g, Gold.b, 0.7f), stroke);
                Place(light.rectTransform, Vector2.zero, Vector2.one, new Vector2(0f, -4f), new Vector2(-6f, -18f));
                light.transform.SetSiblingIndex(outline != null ? outline.GetSiblingIndex() + 1 : 1);
                light.gameObject.SetActive(false);
                if (icons.TryGetValue(tab.name, out int id))
                {
                    Image icon = Img(Prefix + "Icon", b.transform, Spr(id), Cream, plain);
                    Place(icon.rectTransform, new Vector2(0f, 0.5f), new Vector2(0f, 0.5f), new Vector2(30f, 0f), new Vector2(46f, 46f));
                    Text label = b.GetComponentInChildren<Text>(true);
                    if (label != null)
                    {
                        Place(label.rectTransform, Vector2.zero, Vector2.one, new Vector2(30f, 0f), new Vector2(-72f, -16f));
                        label.alignment = TextAnchor.MiddleCenter;
                    }
                }
                var so = new SerializedObject(mt);
                panels.Add(so.FindProperty("panel").objectReferenceValue as CanvasGroup);
                lights.Add(light.gameObject);
            }
            Type t = SceneSetup.FindType("Castlemist.GW2MenuTabs");
            if (t == null) return;
            var go = new GameObject(Prefix + "Menu Tabs");
            go.transform.SetParent(root.transform, false);
            Component comp = SceneSetup.AddUdonAware(go, t);
            SceneSetup.SetField(comp, "canvas", canvas);
            SceneSetup.SetField(comp, "panels", panels.ToArray());
            SceneSetup.SetField(comp, "highlights", lights.ToArray());
            SceneSetup.CopyProxyToUdon(comp);
        }

        // ---- options: view distance ------------------------------------------------------------------

        static Transform OptionList(Transform canvas)
        {
            return canvas.Find("Panels/OptionsPanel/OptionListViewport/OptionListContent");
        }

        /// The view distance slider, first in @p content (the Performance tab's list, since 2026-10-29).
        static void AddViewDistance(Transform content, List<string> notes)
        {
            Type t = SceneSetup.FindType("Castlemist.GW2ViewDistance");
            if (content == null || t == null) { notes.Add("view distance: no GW2ViewDistance"); return; }
            RectTransform row = SliderRow(content, "View Distance", 0, out Text label, out Slider slider, out RectTransform _);

            Component comp = SceneSetup.AddUdonAware(row.gameObject, t);
            SceneSetup.SetField(comp, "slider", slider);
            SceneSetup.SetField(comp, "label", label);
            SceneSetup.CopyProxyToUdon(comp);
            Wire(slider.onValueChanged, comp, "_OnSlider");
            notes.Add("view distance: far clip slider at the top of Performance");
        }

        /// GMs only: the time of day (GW2DayNight's day weight) as a slider, with a "GW2 clock" button
        /// that hands it back to the clock. Shown by the GMMenu's own ToggleByPermission.
        static void AddDayNight(GameObject root, Transform canvas, List<string> notes)
        {
            Transform content = OptionList(canvas);
            Type dnType = SceneSetup.FindType("Castlemist.GW2DayNight");
            Type sliderType = SceneSetup.FindType("Castlemist.GW2DayNightSlider");
            Type permType = SceneSetup.FindType("Sylan.GMMenu.ToggleByPermission");
            Component dn = dnType != null ? Object.FindObjectsOfType(dnType, true).Cast<Component>().FirstOrDefault(c => c.gameObject.scene == root.scene) : null;
            if (content == null || sliderType == null || permType == null) { notes.Add("time of day: no OptionListContent, GW2DayNightSlider or ToggleByPermission"); return; }
            if (dn == null) { notes.Add("time of day: no GW2 Day Night in the scene (Import World makes it): no slider"); return; }

            RectTransform row = SliderRow(content, "Time of Day", 0, out Text label, out Slider slider, out RectTransform line);
            Button clock = MakeButton("Clock", line, "GW2 clock", 24, Cream);
            var cle = clock.gameObject.AddComponent<LayoutElement>();
            cle.minWidth = cle.preferredWidth = 150f;
            Wire(clock, dn, "CycleOn");

            Component comp = SceneSetup.AddUdonAware(row.gameObject, sliderType);
            SceneSetup.SetField(comp, "dayNight", dn);
            SceneSetup.SetField(comp, "slider", slider);
            SceneSetup.SetField(comp, "label", label);
            SceneSetup.CopyProxyToUdon(comp);
            Wire(slider.onValueChanged, comp, "_OnSlider");

            var gate = new GameObject(Prefix + "Time of Day Permission");
            gate.transform.SetParent(root.transform, false);
            Component perm = SceneSetup.AddUdonAware(gate, permType);
            var so = new SerializedObject(perm);
            so.FindProperty("permissionGM").boolValue = true;
            so.FindProperty("permissionFacilitator").boolValue = false;
            so.FindProperty("permissionPlayer").boolValue = false;
            so.FindProperty("useBasePermissionLevel").boolValue = false;
            SerializedProperty objs = so.FindProperty("gameObjects");
            objs.arraySize = 1;
            objs.GetArrayElementAtIndex(0).objectReferenceValue = row.gameObject;
            so.ApplyModifiedPropertiesWithoutUndo();
            SceneSetup.CopyProxyToUdon(perm);
            row.gameObject.SetActive(false);   // until the permission check shows it to a GM
            notes.Add("time of day: GM-only slider in Options, driving " + dn.name);
        }

        /// GMs only (2026-10-26): the festival dressing (GW2Festival: Normal, Halloween, ...), one button per
        /// festival, the current one named above them. Shown by the GMMenu's own ToggleByPermission.
        static void AddFestival(GameObject root, Transform canvas, List<string> notes)
        {
            Transform content = OptionList(canvas);
            Type ft = SceneSetup.FindType("Castlemist.GW2Festival");
            Type permType = SceneSetup.FindType("Sylan.GMMenu.ToggleByPermission");
            Component fest = ft != null ? Object.FindObjectsOfType(ft, true).Cast<Component>().FirstOrDefault(c => c.gameObject.scene == root.scene) : null;
            if (content == null || permType == null) { notes.Add("festival: no OptionListContent or ToggleByPermission"); return; }
            if (fest == null) { notes.Add("festival: no GW2 Festivals in the scene (MapLayers makes it): no buttons"); return; }
            string[] names = ft.GetField("names")?.GetValue(fest) as string[] ?? new string[0];

            RectTransform row = SliderRow(content, "Festival", 1, out Text label, out Slider slider, out RectTransform line);
            Object.DestroyImmediate(slider.gameObject);   // buttons, not a slider
            label.text = names.Length > 0 ? names[0] : "Festival";
            for (int k = 0; k < names.Length && k < 8; ++k)
            {
                Button b = MakeButton(names[k], line, names[k], 24, Cream);
                b.gameObject.AddComponent<LayoutElement>().flexibleWidth = 1f;
                Wire(b, fest, "_F" + k);
            }
            SceneSetup.SetField(fest, "label", label);
            SceneSetup.CopyProxyToUdon(fest);

            var gate = new GameObject(Prefix + "Festival Permission");
            gate.transform.SetParent(root.transform, false);
            Component perm = SceneSetup.AddUdonAware(gate, permType);
            var so = new SerializedObject(perm);
            so.FindProperty("permissionGM").boolValue = true;
            so.FindProperty("permissionFacilitator").boolValue = false;
            so.FindProperty("permissionPlayer").boolValue = false;
            so.FindProperty("useBasePermissionLevel").boolValue = false;
            SerializedProperty objs = so.FindProperty("gameObjects");
            objs.arraySize = 1;
            objs.GetArrayElementAtIndex(0).objectReferenceValue = row.gameObject;
            so.ApplyModifiedPropertiesWithoutUndo();
            SceneSetup.CopyProxyToUdon(perm);
            row.gameObject.SetActive(false);   // until the permission check shows it to a GM
            notes.Add("festival: GM-only buttons in Options (" + string.Join(", ", names) + ")");
        }

        /// An options row: a title on the left, then a label above a slider (and room on the slider's
        /// line, @p line, for a button). Inserted at @p index; the option list grows to fit.
        static RectTransform SliderRow(Transform content, string title, int index, out Text label, out Slider slider, out RectTransform line)
        {
            FitOptionList(content);
            RectTransform row = Rect(Prefix + title, content);
            row.SetSiblingIndex(index);
            row.sizeDelta = new Vector2(row.sizeDelta.x, 100f);
            var h = row.gameObject.AddComponent<HorizontalLayoutGroup>();
            h.padding = new RectOffset(20, 0, 0, 0);
            h.childControlWidth = h.childControlHeight = true;
            h.childForceExpandWidth = false;
            Text t = Txt("Text", row, title + ":", 14, Cream, TextAnchor.MiddleLeft);
            t.resizeTextForBestFit = true; t.resizeTextMaxSize = 30;
            var le = t.gameObject.AddComponent<LayoutElement>();
            le.minWidth = le.preferredWidth = 115f;
            RectTransform col = Rect("Column", row);
            var v = col.gameObject.AddComponent<VerticalLayoutGroup>();
            v.spacing = 4f;
            v.childControlWidth = v.childControlHeight = true;
            v.childForceExpandWidth = true; v.childForceExpandHeight = false;
            col.gameObject.AddComponent<LayoutElement>().flexibleWidth = 1f;
            label = Txt("Label", col, title, 26, Cream, TextAnchor.MiddleCenter);
            label.gameObject.AddComponent<LayoutElement>().preferredHeight = 40f;
            line = Rect("Line", col);
            var lh = line.gameObject.AddComponent<HorizontalLayoutGroup>();
            lh.spacing = 10f;
            lh.childControlWidth = lh.childControlHeight = true;
            lh.childForceExpandWidth = false; lh.childForceExpandHeight = true;
            line.gameObject.AddComponent<LayoutElement>().preferredHeight = 52f;
            slider = MakeSlider("Slider", line);
            slider.gameObject.AddComponent<LayoutElement>().flexibleWidth = 1f;
            return row;
        }

        /// The stock option list has a fixed height; with more rows it must grow (top-anchored) to scroll.
        static void FitOptionList(Transform content)
        {
            var rt = (RectTransform)content;
            if (rt.GetComponent<ContentSizeFitter>() != null) return;
            rt.anchorMin = new Vector2(rt.anchorMin.x, 1f);
            rt.anchorMax = new Vector2(rt.anchorMax.x, 1f);
            rt.pivot = new Vector2(rt.pivot.x, 1f);
            rt.anchoredPosition = new Vector2(rt.anchoredPosition.x, 0f);
            Undo.AddComponent<ContentSizeFitter>(rt.gameObject).verticalFit = ContentSizeFitter.FitMode.PreferredSize;
        }

        /// A GW2 brush-stroke slider: slate track, gold fill, framed gold handle.
        static Slider MakeSlider(string name, Transform parent)
        {
            RectTransform rt = Rect(name, parent);
            Image track = Img("Background", rt, Spr(Stroke), Slate, stroke);
            Place(track.rectTransform, new Vector2(0f, 0.5f), new Vector2(1f, 0.5f), Vector2.zero, new Vector2(0f, 40f));
            RectTransform fillArea = Rect("Fill Area", rt);
            Place(fillArea, new Vector2(0f, 0.5f), new Vector2(1f, 0.5f), Vector2.zero, new Vector2(-24f, 26f));
            Image fill = Img("Fill", fillArea, Spr(Stroke), GoldActive, stroke);
            Place(fill.rectTransform, Vector2.zero, new Vector2(0f, 1f), Vector2.zero, new Vector2(12f, 0f));
            RectTransform handleArea = Rect("Handle Slide Area", rt);
            Place(handleArea, Vector2.zero, Vector2.one, Vector2.zero, new Vector2(-24f, 0f));
            Image handle = Img("Handle", handleArea, Spr(Frame), Gold, plain);
            handle.raycastTarget = true;
            Place(handle.rectTransform, Vector2.zero, new Vector2(0f, 1f), Vector2.zero, new Vector2(46f, -6f));
            Image knob = Img("Knob", handle.transform, Spr(Stroke), GoldActive, stroke);
            Place(knob.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, new Vector2(-14f, -14f));
            var s = rt.gameObject.AddComponent<Slider>();
            s.fillRect = fill.rectTransform;
            s.handleRect = handle.rectTransform;
            s.targetGraphic = handle;
            s.direction = Slider.Direction.LeftToRight;
            s.minValue = 0f; s.maxValue = 1f;
            s.value = 0.75f;              // preview; the runtime puts it at the world's far clip
            track.raycastTarget = true;   // click anywhere on the track
            return s;
        }

        // ---- first-join hint -------------------------------------------------------------------------

        static void AddHint(GameObject menuRoot, Type toggleType, Type hintType, List<string> notes)
        {
            foreach (GameObject old in menuRoot.scene.GetRootGameObjects().Where(g => g.name == HintName).ToList())
                Undo.DestroyObjectImmediate(old);
            var root = new GameObject(HintName);
            SceneManagerMove(root, menuRoot);
            Undo.RegisterCreatedObjectUndo(root, "GW2 menu hint");

            var tokenGo = new GameObject("Token", typeof(RectTransform));
            var token = (RectTransform)tokenGo.transform;
            token.SetParent(root.transform, false);
            token.sizeDelta = new Vector2(370f, 100f);
            token.localScale = Vector3.one * 0.001f;
            var c = tokenGo.AddComponent<Canvas>();
            c.renderMode = RenderMode.WorldSpace;
            c.sortingOrder = 32000;
            var group = tokenGo.AddComponent<CanvasGroup>();
            group.interactable = false; group.blocksRaycasts = false;

            Image back = Img("Back", token, Spr(Stroke), new Color(Dark.r, Dark.g, Dark.b, 1f), overlayStroke);
            Place(back.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, new Vector2(30f, 10f));
            Image gold = Img("Gold", token, Spr(Stroke2), new Color(Gold.r, Gold.g, Gold.b, 0.14f), overlayStroke);
            Place(gold.rectTransform, Vector2.zero, Vector2.one, new Vector2(6f, -4f), new Vector2(10f, -8f));

            GameObject desk = HintLabel(token, "Desktop", DarkFrame, "Tab", 26, "Open the menu");
            GameObject vr = HintLabel(token, "VR", Frame, "↓↑", 34, "Right stick down, then up\nto open the menu");

            Component hint = SceneSetup.AddUdonAware(root, hintType);
            Component toggle = Object.FindObjectsOfType(toggleType, true).Cast<Component>().FirstOrDefault(x => x.transform.IsChildOf(menuRoot.transform));
            if (toggle != null) SceneSetup.SetField(hint, "menuToggle", toggle);
            SceneSetup.SetField(hint, "token", token);
            SceneSetup.SetField(hint, "group", group);
            SceneSetup.SetField(hint, "desktopLabel", desk);
            SceneSetup.SetField(hint, "vrLabel", vr);
            SceneSetup.CopyProxyToUdon(hint);
            notes.Add("hint: " + HintName + " (gone once the menu is opened" + (toggle != null ? "" : "; no GMMenuToggle found, so on Tab / stick") + ")");
        }

        static void SceneManagerMove(GameObject go, GameObject sameSceneAs)
        {
            UnityEngine.SceneManagement.SceneManager.MoveGameObjectToScene(go, sameSceneAs.scene);
        }

        static GameObject HintLabel(Transform token, string name, int keySprite, string key, int keySize, string text)
        {
            RectTransform g = Rect(name, token);
            Place(g, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            Image cap = Img("Key", g, Spr(keySprite), name == "VR" ? Gold : Cream, overlay);
            cap.type = Image.Type.Sliced;
            Place(cap.rectTransform, new Vector2(0f, 0.5f), new Vector2(0f, 0.5f), new Vector2(58f, 0f), new Vector2(76f, 64f));
            Text k = Txt("KeyText", cap.transform, key, keySize, Gold, TextAnchor.MiddleCenter);
            k.material = overlay;
            Place(k.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            Text t = Txt("Text", g, text, 19, Cream, TextAnchor.MiddleLeft);
            t.material = overlay;
            Place(t.rectTransform, Vector2.zero, Vector2.one, new Vector2(55f, 0f), new Vector2(-120f, -10f));
            foreach (Shadow s in g.GetComponentsInChildren<Shadow>(true)) Object.DestroyImmediate(s);   // shadows draw without the overlay material
            return g.gameObject;
        }

        // ---- UI helpers ------------------------------------------------------------------------------

        static RectTransform Rect(string name, Transform parent)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.layer = parent.gameObject.layer;
            var rt = (RectTransform)go.transform;
            rt.SetParent(parent, false);
            return rt;
        }

        static Image Img(string name, Transform parent, Sprite s, Color c, Material m)
        {
            RectTransform rt = Rect(name, parent);
            var i = rt.gameObject.AddComponent<Image>();
            i.sprite = s; i.color = c; i.material = m; i.raycastTarget = false;
            return i;
        }

        static Text Txt(string name, Transform parent, string text, int size, Color c, TextAnchor a)
        {
            RectTransform rt = Rect(name, parent);
            var t = rt.gameObject.AddComponent<Text>();
            t.font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            t.text = text; t.fontSize = size; t.color = c; t.alignment = a; t.raycastTarget = false;
            t.horizontalOverflow = HorizontalWrapMode.Wrap; t.verticalOverflow = VerticalWrapMode.Overflow;
            var sh = rt.gameObject.AddComponent<Shadow>();
            sh.effectColor = new Color(0f, 0f, 0f, 0.6f);
            sh.effectDistance = new Vector2(2f, -2f);
            return t;
        }

        /// A GW2 brush-stroke button: "Fill" (dark stroke), "Outline" (faint gold stroke), "Text".
        static Button MakeButton(string name, Transform parent, string text, int size, Color textColor)
        {
            RectTransform rt = Rect(name, parent);
            Image fill = Img("Fill", rt, Spr(Stroke), Slate, stroke);
            fill.raycastTarget = true;
            Place(fill.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            Image outline = Img("Outline", rt, Spr(Stroke2), GoldWash, stroke);
            Place(outline.rectTransform, Vector2.zero, Vector2.one, new Vector2(4f, -3f), new Vector2(-6f, -10f));
            Text t = Txt("Text", rt, text, size, textColor, TextAnchor.MiddleCenter);
            Place(t.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, new Vector2(-12f, -6f));
            var b = rt.gameObject.AddComponent<Button>();
            b.targetGraphic = fill;
            ColorBlock cb = b.colors;
            cb.highlightedColor = new Color(1.25f, 1.15f, 0.95f, 1f);
            cb.pressedColor = new Color(0.8f, 0.75f, 0.65f, 1f);
            cb.colorMultiplier = 1.2f;
            b.colors = cb;
            return b;
        }

        static void Place(RectTransform rt, Vector2 anchorMin, Vector2 anchorMax, Vector2 pos, Vector2 size)
        {
            rt.anchorMin = anchorMin; rt.anchorMax = anchorMax;
            rt.pivot = new Vector2(0.5f, 0.5f);
            rt.anchoredPosition = pos;
            rt.sizeDelta = size;
        }

        /// Button click -> SendCustomEvent(@p ev) on @p proxy's UdonBehaviour (as the GMMenu's own buttons do).
        static void Wire(Button b, Component proxy, string ev) { Wire(b.onClick, proxy, ev); }

        static void Wire(UnityEventBase e, Component proxy, string ev)
        {
            Component udon = Backing(proxy);
            if (udon == null) return;
            MethodInfo send = udon.GetType().GetMethod("SendCustomEvent", new[] { typeof(string) });
            if (send == null) return;
            var action = (UnityAction<string>)Delegate.CreateDelegate(typeof(UnityAction<string>), udon, send);
            while (e.GetPersistentEventCount() > 0) UnityEventTools.RemovePersistentListener(e, 0);
            UnityEventTools.AddStringPersistentListener(e, action, ev);
        }

        static Component Backing(Component proxy)
        {
            Type u = SceneSetup.FindType("UdonSharpEditor.UdonSharpEditorUtility");
            MethodInfo m = u?.GetMethods(BindingFlags.Public | BindingFlags.Static).FirstOrDefault(x => x.Name == "GetBackingUdonBehaviour" && x.GetParameters().Length == 1);
            return m != null ? m.Invoke(null, new object[] { proxy }) as Component : null;
        }
    }
}
