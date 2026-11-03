// castlemist world import: talkable NPCs with GW2's dialogue (2026-10-26). The game's NPCs and their
// dialogue trees are server-side (not in the dat), so each map's table here holds the GW2 wiki's
// transcription (archived pages, wiki.guildwars2.com) and where the wiki's location map puts each NPC:
// pixels on that map, turned into world metres through its two waypoints (WikiMaps: the GW2 API's continent
// coordinates; ~5-10 m) and dropped onto the collision below.
//   Mad King's Labyrinth (2499169): the eight Mad Spirits (wiki "Mad Spirit"; the north-centre one's
//   Mad Maleficence branch is a collection state the server tracks: its "otherwise" line is used).
// Each NPC gets Runtime/GW2Dialogue: Interact opens its floating GW2-styled panel (name, line, a
// scrolling list of replies). The Mad Spirits look like GW2's challenge motes: a blue fire orb (particles
// on the GW2 fire shader, recoloured; the spirit's own effect is not identified).

using System;
using System.Collections.Generic;
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
    public static class Dialogues
    {
        public const string RootName = "GW2 NPCs";
        const string TexDir = "Assets/Castlemist/UI/Textures";
        const int PanelSprite = 19238, StrokeSprite = 22059, CloseSprite = 63375;
        const int MaxRows = 8;
        static readonly Color Cream = new Color(0.92f, 0.87f, 0.76f, 1f);
        static readonly Color Gold = new Color(0.88f, 0.72f, 0.42f, 1f);
        static readonly Color Slate = new Color(0.26f, 0.28f, 0.31f, 0.92f);

        sealed class Reply { public string Text; public int Next = -1; public bool End = true; }
        sealed class Node { public string Line; public Reply[] Replies; }
        sealed class Npc { public string Name, Where; public Vector2 MapPx; public Node[] Nodes; }

        static Node Says(string line, string reply) { return new Node { Line = line, Replies = new[] { new Reply { Text = reply } } }; }

        const string CantHear = "It doesn't seem to be able to hear me...";

        // The spirits' red circles on "Mad Spirits Labyrinth map.jpg" (WikiMaps.MadSpirits).
        static readonly Dictionary<long, (WikiMaps.Image img, Npc[] npcs)> Maps = new Dictionary<long, (WikiMaps.Image, Npc[])>
        {
            [2499169] = (WikiMaps.MadSpirits, new[]
            {
                new Npc { Name = "Mad Spirit", Where = "Northeast corner behind the outer wall", MapPx = new Vector2(165f, 31f),
                          Nodes = new[] { Says("The history that is recorded is written by the lost and befuddled.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "Northeast corner, to the right of the rock formation", MapPx = new Vector2(182f, 73f),
                          Nodes = new[] { Says("It was the water, the still and pure and deep water.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "North center, on top of the cliffs", MapPx = new Vector2(121f, 18f),
                          Nodes = new[] { Says("Something important... Where did I put it...", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "Northwest corner, on top of the rock formation", MapPx = new Vector2(61f, 16f),
                          Nodes = new[] { Says("If you run out of milk, try separating extra buttermilk back into butter and milk.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "Northwest corner, on top of the floating cliffs by the western waypoint", MapPx = new Vector2(18f, 60f),
                          Nodes = new[] { Says("The gods left, but the king remained. The King will protect us from the barbarians.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "West, outside of the walls of the labyrinth", MapPx = new Vector2(24f, 90f),
                          Nodes = new[] { Says("I see the shimmer glinting in the rocks. A mark is on us all.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "Southwest corner, on top of the rocks", MapPx = new Vector2(28f, 148f),
                          Nodes = new[] { Says("This labyrinth is only a maze if your thoughts are not maze shaped.", CantHear) } },
                new Npc { Name = "Mad Spirit", Where = "South, on top a hill of bones", MapPx = new Vector2(94f, 177f),
                          Nodes = new[] { Says("The cloth is full of holes. I have to find a tailor. I have to.", CantHear) } },
            }),
        };

        [MenuItem("Castlemist/Talkable NPCs (GW2 dialogue)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        /// The open map scene's NPCs (its table above) rebuilt under "<env root>/GW2 NPCs"; a one-line report.
        public static string Setup()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "npcs: no GW2_<id>_Environment root in the open scene";
            long mapId;
            if (!long.TryParse(env.name.Replace("GW2_", "").Replace("_Environment", ""), out mapId)) return "npcs: cannot read the map id from " + env.name;
            if (!Maps.TryGetValue(mapId, out var table)) return "npcs: map " + mapId + " has no dialogue table";
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "npcs: UdonSharp is not in this project";
            if (made.Count > 0) return "npcs: UdonSharp program assets made (" + string.Join(", ", made) + "): run again once UdonSharp has compiled";
            Type dlgType = SceneSetup.FindType("Castlemist.GW2Dialogue");
            if (dlgType == null) return "npcs: Castlemist.GW2Dialogue is not compiled yet";

            Transform old = env.transform.Find(RootName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var root = new GameObject(RootName);
            root.transform.SetParent(env.transform, false);
            root.transform.localScale = new Vector3(1f / env.transform.lossyScale.x, 1f / env.transform.lossyScale.y, 1f / env.transform.lossyScale.z);   // metres
            Physics.SyncTransforms();

            // The wiki map's spots on the collision; a spot whose ground is far below the others' (a few
            // metres off past a cliff edge) walks toward the map centre until it is not.
            GameObject mapRoot = env.scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + mapId);
            if (mapRoot == null) return "npcs: no GW2_" + mapId + " root in the open scene";
            var spots = new Vector3[table.npcs.Length];
            for (int i = 0; i < spots.Length; ++i)
            {
                WikiMaps.ToWorld(mapId, table.img, table.npcs[i].MapPx, mapRoot.transform, out spots[i]);
                spots[i].y = WikiMaps.Ground(spots[i].x, spots[i].z);
            }
            Vector3 centre = mapRoot.transform.position;
            Material flame = MoteMaterial();
            float[] ys = spots.Select(v => v.y).Where(y => !float.IsNaN(y)).OrderBy(y => y).ToArray();
            float floor = ys.Length > 0 ? ys[ys.Length / 2] - MaxBelowMedian : float.NegativeInfinity;
            int placed = 0, unsnapped = 0, moved = 0;
            for (int i = 0; i < spots.Length; ++i)
            {
                Vector3 at = spots[i];
                if (float.IsNaN(at.y) || at.y < floor)
                {
                    Vector2 toCentre = new Vector2(centre.x - at.x, centre.z - at.z).normalized;
                    for (float d = 2f; d <= 40f; d += 2f)
                    {
                        float y = WikiMaps.Ground(at.x + toCentre.x * d, at.z + toCentre.y * d);
                        if (float.IsNaN(y) || y < floor) continue;
                        at = new Vector3(at.x + toCentre.x * d, y, at.z + toCentre.y * d);
                        ++moved;
                        break;
                    }
                }
                if (float.IsNaN(at.y)) { at.y = 0f; ++unsnapped; }
                Build(root.transform, table.npcs[i], at, dlgType, flame);
                ++placed;
            }
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "npcs: " + placed + " talkable NPC(s) with GW2 dialogue (positions from the wiki's map, ~5-10 m; " + moved +
                   " moved in from a cliff edge, " + unsnapped + " not over collision)";
        }

        const float MaxBelowMedian = 60f;   // metres under the NPCs' median ground: past an edge

        static void Build(Transform root, Npc npc, Vector3 at, Type dlgType, Material flame)
        {
            var go = new GameObject(npc.Name + " (" + npc.Where + ")");
            go.transform.SetParent(root, false);
            go.transform.position = at;
            var col = go.AddComponent<CapsuleCollider>();
            col.center = new Vector3(0f, 1f, 0f); col.height = 2f; col.radius = 0.45f;
            if (flame != null) MoteOrb(go.transform, flame, MotePlume, MoteCore);

            // The panel: 600 x 440 px at 2.5 mm a pixel (1.5 m wide), above the head.
            var canvasGo = new GameObject("Dialogue", typeof(RectTransform));
            canvasGo.transform.SetParent(go.transform, false);
            canvasGo.transform.localPosition = new Vector3(0f, 2.6f, 0f);
            canvasGo.transform.localScale = Vector3.one * 0.0025f;
            var canvas = canvasGo.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.WorldSpace;
            canvasGo.AddComponent<GraphicRaycaster>();
            Type shape = SceneSetup.FindType("VRC.SDK3.Components.VRCUiShape");
            if (shape != null) canvasGo.AddComponent(shape);
            var crt = (RectTransform)canvasGo.transform;
            crt.sizeDelta = new Vector2(600f, 440f);

            Image bg = Img("Panel", crt, Spr(PanelSprite), new Color(1f, 1f, 1f, 0.96f));
            if (bg.sprite == null) bg.color = new Color(0.1f, 0.11f, 0.13f, 0.92f);
            Place(bg.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            Text speaker = Txt("Name", crt, npc.Name, 34, Gold, TextAnchor.UpperLeft);
            Place(speaker.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -38f), new Vector2(-80f, 50f));
            Text line = Txt("Line", crt, "", 26, Cream, TextAnchor.UpperLeft);
            Place(line.rectTransform, new Vector2(0f, 1f), new Vector2(1f, 1f), new Vector2(0f, -120f), new Vector2(-80f, 110f));
            Button close = MakeButton("Close", crt, "", 20);
            Place((RectTransform)close.transform, new Vector2(1f, 1f), new Vector2(1f, 1f), new Vector2(-36f, -36f), new Vector2(44f, 44f));
            Sprite x = Spr(CloseSprite);
            if (x != null) { var fill = close.targetGraphic as Image; fill.sprite = x; fill.color = Color.white; }
            else close.GetComponentInChildren<Text>().text = "X";

            // The replies: a scroll view (VR: drag; desktop: wheel) of up to MaxRows buttons.
            RectTransform view = Rect("Replies", crt);
            Place(view, new Vector2(0f, 0f), new Vector2(1f, 0f), new Vector2(0f, 105f), new Vector2(-60f, 190f));
            view.gameObject.AddComponent<Image>().color = new Color(0f, 0f, 0f, 0.01f);   // raycast target for dragging
            view.gameObject.AddComponent<RectMask2D>();
            RectTransform content = Rect("Content", view);
            content.anchorMin = new Vector2(0f, 1f); content.anchorMax = new Vector2(1f, 1f); content.pivot = new Vector2(0.5f, 1f);
            content.anchoredPosition = Vector2.zero; content.sizeDelta = Vector2.zero;
            var vl = content.gameObject.AddComponent<VerticalLayoutGroup>();
            vl.spacing = 8f; vl.childControlHeight = true; vl.childControlWidth = true; vl.childForceExpandHeight = false;
            content.gameObject.AddComponent<ContentSizeFitter>().verticalFit = ContentSizeFitter.FitMode.PreferredSize;
            var scroll = view.gameObject.AddComponent<ScrollRect>();
            scroll.content = content; scroll.viewport = view; scroll.horizontal = false; scroll.movementType = ScrollRect.MovementType.Clamped;
            scroll.scrollSensitivity = 20f;

            var rows = new GameObject[MaxRows];
            var texts = new Text[MaxRows];
            var ends = new GameObject[MaxRows];
            var mores = new GameObject[MaxRows];
            for (int i = 0; i < MaxRows; ++i)
            {
                Button b = MakeButton("Reply " + i, content, "", 24);
                b.gameObject.AddComponent<LayoutElement>().preferredHeight = 56f;
                Text t = b.GetComponentInChildren<Text>();
                t.alignment = TextAnchor.MiddleLeft;
                Place(t.rectTransform, Vector2.zero, Vector2.one, new Vector2(24f, 0f), new Vector2(-72f, -6f));
                // GW2's reply icons: the end of the conversation, or more to say.
                Text end = Txt("End", b.transform, "✖", 22, Gold, TextAnchor.MiddleCenter);
                Place(end.rectTransform, new Vector2(0f, 0.5f), new Vector2(0f, 0.5f), new Vector2(26f, 0f), new Vector2(36f, 36f));
                Text more = Txt("More", b.transform, "▶", 22, Gold, TextAnchor.MiddleCenter);
                Place(more.rectTransform, new Vector2(0f, 0.5f), new Vector2(0f, 0.5f), new Vector2(26f, 0f), new Vector2(36f, 36f));
                rows[i] = b.gameObject; texts[i] = t; ends[i] = end.gameObject; mores[i] = more.gameObject;
            }

            // Nodes flattened into the behaviour's arrays.
            var lines = new List<string>(); var starts = new List<int>(); var counts = new List<int>();
            var opts = new List<string>(); var nexts = new List<int>(); var endFlags = new List<bool>();
            foreach (Node n in npc.Nodes)
            {
                lines.Add(n.Line); starts.Add(opts.Count); counts.Add(Math.Min(n.Replies.Length, MaxRows));
                foreach (Reply r in n.Replies.Take(MaxRows)) { opts.Add(r.Text); nexts.Add(r.Next); endFlags.Add(r.End || r.Next < 0); }
            }

            Component dlg = SceneSetup.AddUdonAware(go, dlgType);
            SceneSetup.SetField(dlg, "speaker", npc.Name);
            SceneSetup.SetField(dlg, "lines", lines.ToArray());
            SceneSetup.SetField(dlg, "optionStart", starts.ToArray());
            SceneSetup.SetField(dlg, "optionCount", counts.ToArray());
            SceneSetup.SetField(dlg, "options", opts.ToArray());
            SceneSetup.SetField(dlg, "optionNext", nexts.ToArray());
            SceneSetup.SetField(dlg, "optionEnd", endFlags.ToArray());
            SceneSetup.SetField(dlg, "panel", canvasGo);
            SceneSetup.SetField(dlg, "speakerText", speaker);
            SceneSetup.SetField(dlg, "lineText", line);
            SceneSetup.SetField(dlg, "rows", rows);
            SceneSetup.SetField(dlg, "rowTexts", texts);
            SceneSetup.SetField(dlg, "rowEndIcons", ends);
            SceneSetup.SetField(dlg, "rowMoreIcons", mores);
            SceneSetup.SetField(dlg, "scroll", scroll);
            SceneSetup.CopyProxyToUdon(dlg);
            Component udon = Backing(dlg);
            if (udon != null) SceneSetup.SetField(udon, "interactText", "Talk");
            for (int i = 0; i < MaxRows; ++i) Wire(rows[i].GetComponent<Button>(), udon, "_O" + i);
            Wire(close, udon, "_Close");
            canvasGo.SetActive(false);
        }

        // 2026-10-27 (the user's screenshot of a Mad Spirit in the game: a blue smoke plume rising off a bright
        // orb wrapped in swirling streaks): built from GW2's own wisp texture, 506657's "WispEmt" / "SmokeTop"
        // (Mat_19092: a 2 x 2 sheet, three wisps of drifting strands and one bright orb in swirls), on Unity's
        // legacy additive particle shader with the sheet as a texture-sheet animation:
        //   Plume  the strand frames rising, growing and fading (the smoke)
        //   Swirl  the strand frames turning round the centre
        //   Core   the orb frame, spinning, bright
        // The colours are per spirit (blue for the Mad Spirits, purple for the caged ones).
        static readonly Color MotePlume = new Color(0.12f, 0.32f, 1f), MoteCore = new Color(0.55f, 0.85f, 1f);
        static readonly Color CagePlume = new Color(0.55f, 0.15f, 1f), CageCore = new Color(0.9f, 0.6f, 1f);

        /// Materials/GW2_Mote_Wisp.mat: the wisp sheet on Legacy Shaders/Particles/Additive; null when the
        /// scene has no 19092 particle material to take the texture from.
        static Material MoteMaterial() { return ParticleMaterial("Mat_19092_fx", "GW2_Mote_Wisp"); }

        /// Materials/GW2_Mote_Smoke.mat: GW2's billowing smoke puff (60669's "SmokeG01", Mat_643096), the plume.
        static Material SmokeMaterial() { return ParticleMaterial("Mat_643096_fx", "GW2_Mote_Smoke"); }

        /// Materials/<name>.mat: the texture of the scene's first particle material named @p source, on Legacy
        /// Shaders/Particles/Additive; null without one.
        static Material ParticleMaterial(string source, string name)
        {
            Texture tex = null;
            foreach (ParticleSystemRenderer r in Object.FindObjectsOfType<ParticleSystemRenderer>(true))
                if (r.sharedMaterial != null && r.sharedMaterial.name.StartsWith(source, StringComparison.Ordinal) && r.sharedMaterial.mainTexture != null)
                { tex = r.sharedMaterial.mainTexture; break; }
            Shader sh = Shader.Find("Legacy Shaders/Particles/Additive");
            if (tex == null || sh == null) return null;
            string root = System.IO.Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path).Replace('\\', '/');
            string path = root + "/Materials/" + name + ".mat";
            Material m = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (m == null) { m = new Material(sh); AssetDatabase.CreateAsset(m, path); }
            m.shader = sh;
            m.mainTexture = tex;
            m.SetColor("_TintColor", new Color(0.8f, 0.8f, 0.8f, 0.8f));   // x 2 in the shader: the wisps glow
            m.enableInstancing = true;
            EditorUtility.SetDirty(m);
            return m;
        }

        [MenuItem("Castlemist/Mad Spirit Look (in place)")]
        public static void MoteLookMenu() { Debug.Log("CASTLEMIST " + MoteLook()); }

        /// Every Mad Spirit's orb rebuilt where it stands (see above); returns a one-line report.
        public static string MoteLook()
        {
            Material mat = MoteMaterial();
            if (mat == null) return "mad spirits: no GW2 wisp texture (Mat_19092) in the scene";
            int n = 0;
            foreach (Transform t in Object.FindObjectsOfType<Transform>(true).Where(x => x.name == "Mote Orb").ToList())
            {
                Transform parent = t.parent;
                Object.DestroyImmediate(t.gameObject);
                MoteOrb(parent, mat, MotePlume, MoteCore);
                ++n;
            }
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "mad spirits: " + n + " orb(s) rebuilt from GW2's wisp sheet (plume, swirl, core)";
        }

        // 2026-10-27 (the user, from the game): the hanging cages (86495) each hold a burning spirit, purple
        // in the game's screenshots: the Mad Spirit's orb (MoteOrb) at the cage's centre, no dialogue or
        // interaction, in purple.
        public const long CageModel = 86495;
        public const string CageOrbName = "GW2 Cage Spirit";
        public const float CageScale = 1.0f;   // the orb filling a ~3 m cage

        [MenuItem("Castlemist/Cage Spirits (burning, in the cages)")]
        public static void CageSpiritsMenu() { Debug.Log("CASTLEMIST " + CageSpirits()); }

        /// A burning spirit in every placed 86495 cage (replacing earlier ones); returns a one-line report.
        public static string CageSpirits()
        {
            Material mat = MoteMaterial();
            if (mat == null) return "cage spirits: no GW2 wisp texture (Mat_19092) in the scene";
            int n = 0;
            // the cages first: rebuilding destroys transforms a live enumeration would then reach
            var cages = Object.FindObjectsOfType<Transform>(true)
                .Where(c => c.parent != null && c.parent.name == "Props" && c.name.StartsWith(CageModel + "_", StringComparison.Ordinal)).ToList();
            foreach (Transform cage in cages)
            {
                Transform old = cage.Find(CageOrbName);
                if (old != null) Object.DestroyImmediate(old.gameObject);
                Renderer body = cage.GetComponentsInChildren<Renderer>(true).FirstOrDefault(r => r.name.StartsWith("cell", StringComparison.Ordinal));
                if (body == null) continue;
                var holder = new GameObject(CageOrbName);
                holder.transform.SetParent(cage, false);
                holder.transform.position = body.bounds.center - Vector3.up * 1.3f * CageScale;   // MoteOrb lifts its orb 1.3 m
                Vector3 ls = cage.lossyScale;
                holder.transform.localScale = new Vector3(CageScale / ls.x, CageScale / ls.y, CageScale / ls.z);
                GameObject orb = MoteOrb(holder.transform, mat, CagePlume, CageCore);
                orb.name = "Cage Orb";   // MoteLook rebuilds "Mote Orb"s blue
                foreach (ParticleSystem ps in orb.GetComponentsInChildren<ParticleSystem>(true))
                {
                    ParticleSystem.MainModule main = ps.main;
                    main.scalingMode = ParticleSystemScalingMode.Hierarchy;
                }
                ++n;
            }
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            return "cage spirits: " + n + " burning purple spirit(s) in the " + CageModel + " cages";
        }

        /// The spirit (see above) 1.3 m over @p parent; returns its "Mote Orb".
        static GameObject MoteOrb(Transform parent, Material mat, Color plume, Color core)
        {
            // 2026-10-27, second pass against the user's screenshot: one bright orb wrapped in swirling ribbons
            // at the base, the plume light at the orb and deep blue as it rises (the spinning wisp layer read
            // as a starburst, the orb frame as loose blobs).
            var go = new GameObject("Mote Orb");
            go.transform.SetParent(parent, false);
            go.transform.localPosition = new Vector3(0f, 1.3f, 0f);
            Material smoke = SmokeMaterial();
            if (smoke == null) smoke = mat;
            // the plume: GW2's smoke puff rising off the orb, light at the base, deep blue as it rises
            ParticleSystem pl = Layer(go.transform, "Plume", smoke, Color.white, frames: new Vector2(-1, -1), rate: 22f, life: new Vector2(1.3f, 1.8f),
                                      size: new Vector2(0.55f, 0.85f), grow: new AnimationCurve(new Keyframe(0f, 0.6f), new Keyframe(1f, 2.1f)),
                                      rise: new Vector2(0.75f, 1.1f), spin: 0.8f, radius: 0.12f, alphaPeak: 0.75f, noise: 0.3f);
            ParticleSystem.ColorOverLifetimeModule pc = pl.colorOverLifetime;
            var pg = new Gradient();
            pg.SetKeys(new[] { new GradientColorKey(Color.Lerp(core, Color.white, 0.2f), 0f), new GradientColorKey(plume, 0.45f), new GradientColorKey(plume * 0.7f, 1f) },
                       new[] { new GradientAlphaKey(0f, 0f), new GradientAlphaKey(0.8f, 0.12f), new GradientAlphaKey(0.45f, 0.6f), new GradientAlphaKey(0f, 1f) });
            pc.color = pg;
            // the orb: a steady glow and a white-hot centre (the default soft particle), no flicker
            Glow(go.transform, "Orb", core, 0.62f, 0.9f);
            Glow(go.transform, "Centre", Color.Lerp(core, Color.white, 0.7f), 0.28f, 1f);
            // the ribbons: points circling the orb on every axis, their trails the swirling streaks
            Ribbons(go.transform, Color.Lerp(core, Color.white, 0.5f));
            return go;
        }

        static Material SoftParticle()
        {
            string root = System.IO.Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path).Replace('\\', '/');
            string path = root + "/Materials/GW2_Mote_Glow.mat";
            Material m = AssetDatabase.LoadAssetAtPath<Material>(path);
            Shader sh = Shader.Find("Legacy Shaders/Particles/Additive");
            if (m == null) { m = new Material(sh); AssetDatabase.CreateAsset(m, path); }
            m.shader = sh;
            m.mainTexture = AssetDatabase.GetBuiltinExtraResource<Texture2D>("Default-Particle.psd");
            m.SetColor("_TintColor", new Color(0.6f, 0.6f, 0.6f, 0.6f));
            m.enableInstancing = true;
            EditorUtility.SetDirty(m);
            return m;
        }

        /// One steady soft glow of @p size metres at the orb's centre.
        static void Glow(Transform parent, string name, Color col, float size, float alpha)
        {
            var go = new GameObject(name);
            go.transform.SetParent(parent, false);
            var ps = go.AddComponent<ParticleSystem>();
            ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
            ParticleSystem.MainModule main = ps.main;
            main.startLifetime = 1f; main.startSpeed = 0f; main.startSize = size;
            main.startColor = new Color(col.r, col.g, col.b, alpha);
            main.simulationSpace = ParticleSystemSimulationSpace.Local;
            main.maxParticles = 4; main.cullingMode = ParticleSystemCullingMode.Pause;
            ParticleSystem.EmissionModule em = ps.emission; em.rateOverTime = 3f;
            ParticleSystem.ShapeModule sh = ps.shape; sh.enabled = false;
            ParticleSystem.ColorOverLifetimeModule colm = ps.colorOverLifetime; colm.enabled = true;
            var g = new Gradient();
            g.SetKeys(new[] { new GradientColorKey(Color.white, 0f), new GradientColorKey(Color.white, 1f) },
                      new[] { new GradientAlphaKey(0f, 0f), new GradientAlphaKey(1f, 0.25f), new GradientAlphaKey(1f, 0.75f), new GradientAlphaKey(0f, 1f) });
            colm.color = g;
            var r = go.GetComponent<ParticleSystemRenderer>();
            r.sharedMaterial = SoftParticle();
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off; r.receiveShadows = false;
            ps.Play();
        }

        /// Points orbiting the orb on all three axes; their trails draw the swirling streaks round it.
        static void Ribbons(Transform parent, Color col)
        {
            var go = new GameObject("Ribbons");
            go.transform.SetParent(parent, false);
            var ps = go.AddComponent<ParticleSystem>();
            ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
            ParticleSystem.MainModule main = ps.main;
            main.startLifetime = new ParticleSystem.MinMaxCurve(0.7f, 1.1f); main.startSpeed = 0f;
            main.startSize = 0.035f; main.startColor = col;
            main.simulationSpace = ParticleSystemSimulationSpace.Local;
            main.maxParticles = 24; main.cullingMode = ParticleSystemCullingMode.Pause;
            ParticleSystem.EmissionModule em = ps.emission; em.rateOverTime = 14f;
            ParticleSystem.ShapeModule sh = ps.shape; sh.shapeType = ParticleSystemShapeType.Sphere; sh.radius = 0.24f; sh.radiusThickness = 0f;
            ParticleSystem.VelocityOverLifetimeModule vel = ps.velocityOverLifetime;
            vel.enabled = true; vel.space = ParticleSystemSimulationSpace.Local;
            vel.x = new ParticleSystem.MinMaxCurve(0f, 0f); vel.y = new ParticleSystem.MinMaxCurve(0f, 0f); vel.z = new ParticleSystem.MinMaxCurve(0f, 0f);
            vel.orbitalX = new ParticleSystem.MinMaxCurve(-5f, 5f);
            vel.orbitalY = new ParticleSystem.MinMaxCurve(4f, 7f);
            vel.orbitalZ = new ParticleSystem.MinMaxCurve(-5f, 5f);
            vel.radial = new ParticleSystem.MinMaxCurve(0f, 0f);
            ParticleSystem.ColorOverLifetimeModule colm = ps.colorOverLifetime; colm.enabled = true;
            var g = new Gradient();
            g.SetKeys(new[] { new GradientColorKey(Color.white, 0f), new GradientColorKey(Color.white, 1f) },
                      new[] { new GradientAlphaKey(0f, 0f), new GradientAlphaKey(1f, 0.15f), new GradientAlphaKey(0.8f, 0.7f), new GradientAlphaKey(0f, 1f) });
            colm.color = g;
            ParticleSystem.TrailModule tr = ps.trails;
            tr.enabled = true;
            tr.lifetime = new ParticleSystem.MinMaxCurve(0.28f);
            tr.minVertexDistance = 0.02f;
            tr.widthOverTrail = new ParticleSystem.MinMaxCurve(1f, new AnimationCurve(new Keyframe(0f, 1f), new Keyframe(1f, 0f)));
            tr.inheritParticleColor = true;
            tr.dieWithParticles = true;
            var r = go.GetComponent<ParticleSystemRenderer>();
            Material soft = SoftParticle();
            r.sharedMaterial = soft;
            r.trailMaterial = soft;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off; r.receiveShadows = false;
            ps.Play();
        }

        static ParticleSystem Layer(Transform parent, string name, Material mat, Color col, Vector2 frames, float rate, Vector2 life, Vector2 size,
                          AnimationCurve grow, Vector2 rise, float spin, float radius, float alphaPeak, float noise)
        {
            var go = new GameObject(name);
            go.transform.SetParent(parent, false);
            if (rise.y > 0f) go.transform.localPosition = new Vector3(0f, 0.25f, 0f);   // a rising layer starts over the orb
            var ps = go.AddComponent<ParticleSystem>();
            ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
            ParticleSystem.MainModule main = ps.main;
            main.startLifetime = new ParticleSystem.MinMaxCurve(life.x, life.y);
            main.startSpeed = 0f;
            main.startSize = new ParticleSystem.MinMaxCurve(size.x, size.y);
            main.startRotation = new ParticleSystem.MinMaxCurve(0f, 2f * Mathf.PI);
            main.startColor = col;
            main.simulationSpace = ParticleSystemSimulationSpace.Local;
            main.maxParticles = 48;
            main.cullingMode = ParticleSystemCullingMode.Pause;
            ParticleSystem.EmissionModule em = ps.emission;
            em.rateOverTime = rate;
            ParticleSystem.ShapeModule sh = ps.shape;
            sh.enabled = radius > 0f;
            sh.shapeType = ParticleSystemShapeType.Sphere;
            sh.radius = Mathf.Max(radius, 0.001f);
            if (rise.y > 0f)
            {
                ParticleSystem.VelocityOverLifetimeModule vel = ps.velocityOverLifetime;
                vel.enabled = true;
                vel.space = ParticleSystemSimulationSpace.Local;
                // every axis in one mode (two constants): mixed modes log an error every frame
                vel.x = new ParticleSystem.MinMaxCurve(-0.05f, 0.05f); vel.y = new ParticleSystem.MinMaxCurve(rise.x, rise.y); vel.z = new ParticleSystem.MinMaxCurve(-0.05f, 0.05f);
            }
            ParticleSystem.SizeOverLifetimeModule sol = ps.sizeOverLifetime;
            sol.enabled = true;
            sol.size = new ParticleSystem.MinMaxCurve(1f, grow);
            ParticleSystem.RotationOverLifetimeModule rot = ps.rotationOverLifetime;
            rot.enabled = true;
            rot.z = new ParticleSystem.MinMaxCurve(-spin, spin);   // radians per second, either way
            ParticleSystem.ColorOverLifetimeModule colm = ps.colorOverLifetime;
            colm.enabled = true;
            var g = new Gradient();
            g.SetKeys(new[] { new GradientColorKey(Color.white, 0f), new GradientColorKey(Color.white, 1f) },
                      new[] { new GradientAlphaKey(0f, 0f), new GradientAlphaKey(alphaPeak, 0.18f), new GradientAlphaKey(alphaPeak * 0.6f, 0.6f), new GradientAlphaKey(0f, 1f) });
            colm.color = g;
            if (noise > 0f)
            {
                ParticleSystem.NoiseModule nm = ps.noise;
                nm.enabled = true;
                nm.strength = noise;
                nm.frequency = 0.8f;
                nm.scrollSpeed = 0.4f;
            }
            ParticleSystem.TextureSheetAnimationModule tsa = ps.textureSheetAnimation;
            tsa.enabled = frames.x >= 0f;   // the smoke puff is one image
            tsa.numTilesX = 2; tsa.numTilesY = 2;
            // the frame by frameOverTime (normalised over the 4 tiles, one random constant per particle): 0-2 the wisps, 3 the orb
            tsa.frameOverTime = new ParticleSystem.MinMaxCurve((frames.x + 0.05f) / 4f, (frames.y + 0.95f) / 4f);
            tsa.startFrame = new ParticleSystem.MinMaxCurve(0f);
            tsa.cycleCount = 1;
            var r = go.GetComponent<ParticleSystemRenderer>();
            r.sharedMaterial = mat;
            r.renderMode = ParticleSystemRenderMode.Billboard;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.sortMode = ParticleSystemSortMode.Distance;
            ps.Play();
            return ps;
        }

        // ---- UI helpers (as GW2MenuLook's) -------------------------------------------------------------

        static Sprite Spr(int id) { return AssetDatabase.LoadAssetAtPath<Sprite>(TexDir + "/gw2ui_" + id + ".png"); }

        static RectTransform Rect(string name, Transform parent)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.layer = parent.gameObject.layer;
            var rt = (RectTransform)go.transform;
            rt.SetParent(parent, false);
            return rt;
        }

        static Image Img(string name, Transform parent, Sprite s, Color c)
        {
            var i = Rect(name, parent).gameObject.AddComponent<Image>();
            i.sprite = s; i.color = c; i.raycastTarget = false;
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

        static Button MakeButton(string name, Transform parent, string text, int size)
        {
            RectTransform rt = Rect(name, parent);
            Image fill = Img("Fill", rt, Spr(StrokeSprite), Slate);
            fill.raycastTarget = true;
            Place(fill.rectTransform, Vector2.zero, Vector2.one, Vector2.zero, Vector2.zero);
            Text t = Txt("Text", rt, text, size, Cream, TextAnchor.MiddleCenter);
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

        /// Button click -> SendCustomEvent(@p ev) on @p udon (an UdonBehaviour).
        static void Wire(Button b, Component udon, string ev)
        {
            if (b == null || udon == null) return;
            MethodInfo send = udon.GetType().GetMethod("SendCustomEvent", new[] { typeof(string) });
            if (send == null) return;
            var action = (UnityAction<string>)Delegate.CreateDelegate(typeof(UnityAction<string>), udon, send);
            while (b.onClick.GetPersistentEventCount() > 0) UnityEventTools.RemovePersistentListener(b.onClick, 0);
            UnityEventTools.AddStringPersistentListener(b.onClick, action, ev);
        }

        static Component Backing(Component proxy)
        {
            Type u = SceneSetup.FindType("UdonSharpEditor.UdonSharpEditorUtility");
            MethodInfo m = u?.GetMethods(BindingFlags.Public | BindingFlags.Static).FirstOrDefault(x => x.Name == "GetBackingUdonBehaviour" && x.GetParameters().Length == 1);
            return m != null ? m.Invoke(null, new object[] { proxy }) as Component : null;
        }
    }
}
