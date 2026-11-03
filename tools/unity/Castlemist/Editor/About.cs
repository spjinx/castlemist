// castlemist: Castlemist > About and Help (2026-10-26). One window over every Castlemist menu command:
// grouped by job, a line or two on what each does and when, the usual order after an import, and a Run
// button (EditorApplication.ExecuteMenuItem). The commands are found from their [MenuItem] attributes
// (TypeCache), so a new one shows under "Other" until Groups describes it.
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public class About : EditorWindow
    {
        struct Entry
        {
            public string Item, Text;
            public Entry(string item, string text) { Item = item; Text = text; }
        }

        static readonly (string group, Entry[] entries)[] Groups =
        {
            ("Import", new[]
            {
                new Entry("Import World...", "Imports a castlemist world export folder (manifest.json) into a new scene: terrain, props, materials, lights, water, sky, sounds."),
                new Entry("Refresh Open World Scene In Place", "Rebuilds the open world scene's materials and model prefabs from its export and applies them to the props already placed (terrain, lights, volumes and probes are left). It converts the unanimated skinned meshes again by itself and keeps your cull distance (the Import window's remembered Distance)."),
            }),
            ("Lighting", new[]
            {
                new Entry("GW2 Lighting Look (realtime sun, no bounce)", "Sets the scene up the way GW2 lights a map: realtime sun with shadows, no bounce light, unshadowed map lights. Run before a bake; safe to run again."),
                new Entry("GW2 Realtime Lights (Light Volumes point lights)", "Turns the map's and props' lights into realtime Light Volumes point lights (no bake needed for them), streamed: up to 64 near each player."),
                new Entry("GW2 Baked Lights (undo realtime lights)", "Undoes GW2 Realtime Lights: the lights go back to being baked by Bakery."),
                new Entry("Setup Immersive Lighting", "Adds the bake helpers: detail Light Volumes over walkable floors, glow lights, the Bakery sun and sky. Bake afterwards."),
                new Entry("Merge Coarse Light Volumes (32-volume limit)", "Light Volumes draws only 32 volumes: merges the per-cell volumes into 12 large ones so every area is lit. Re-bake afterwards."),
                new Entry("Coarse Light Volumes (smaller atlas)", "Lowers the coarse volumes' resolution (a smaller download). Re-bake afterwards."),
                new Entry("Add Light Volume Fog", "Puts the lit volumetric fog boxes (BakedVolumetrics) under GW2's height haze, one per coarse volume."),
                new Entry("Bake Lighting and Occlusion (Bakery)", "Bakes with Bakery: the Light Volumes, reflection probes and occlusion culling. The usual bake."),
                new Entry("Bake Lighting and Occlusion (Unity)", "The same bake with Unity's own lightmapper (no Bakery)."),
                new Entry("Bake Reflection Probes and Occlusion", "Only the reflection probes and occlusion culling (after moving things, without a full light bake)."),
                new Entry("Fit Reflection Probes to Light Volumes", "Rebuilds one reflection probe per Light Volume and bakes them."),
                new Entry("Smaller Reflection Probes (re-bake)", "Every reflection probe to 128 px and re-baked (a smaller download)."),
            }),
            ("Performance", new[]
            {
                new Entry("Static Skinned Meshes to Mesh Renderers", "Props that never animate but were imported skinned become plain meshes (the biggest single frame-time win, no visual change)."),
                new Entry("Performance Pass (particles, animators, shadows, terrain)", "Particles pause off screen and cull with their prop; ambient animators stop entirely off screen (doors and demolishables keep theirs); props under 3 m cast no shadow; terrain grass to 75 m, coarser terrain mesh, one-sided terrain shadows."),
                new Entry("Build Cell Proxies (far cells as one mesh)", "The map is cut into 64 m tiles; beyond 110 m each tile is drawn as one simplified mesh coloured from its textures, and its animations and particles stop, switched by distance at runtime. ToggleProxies on GW2 Cell Switcher turns them off to compare."),
                new Entry("Keep Small Props Visible Nearby (40 m)", "However short the cull distances, every prop (each placement at its placed scale) still draws within 40 m. Imports and refreshes apply it by themselves."),
                new Entry("Remove Cell Proxies", "Deletes the cell proxies and their switcher: real props at every distance."),
                new Entry("Lower LODs (simplified extra level, in place)", "Props whose lowest level is still heavy get one more level, simplified to 30% of its triangles, from a third of the distance they cull at; they draw exactly as far as before. Imports and refreshes add it by themselves."),
                new Entry("Optimization Pass (lower LODs, proxies, compression)", "Lower LODs, the cell proxies rebuilt, Optimize Build Size, prop textures crunched at 1024 px and sound effects at Vorbis 50%, in one go. Slow: it re-imports the textures."),
            }),
            ("Collision", new[]
            {
                new Entry("Open Doorways in Map Collision", "The map's collision holds every opening door shut: cuts the walls out of it inside each door that opens (door models up to 12 m; gates are left). Imports and refreshes do it by themselves."),
            }),
            ("Build size", new[]
            {
                new Entry("Optimize Build Size", "Textures capped at 1024 px (terrain, sky and environment textures kept) and foliage patches at Medium mesh compression. Prop meshes stay uncompressed: compression snaps their UVs into blocks."),
                new Entry("Smaller Prop Textures (512 px, crunched)", "Prop textures (textures/, the ones the last build packed) at 512 px with crunch compression: the biggest share of the download. Slow: it re-imports thousands of textures."),
                new Entry("Smaller Sound Effects (Vorbis 50%)", "Every sound effect at Vorbis quality 50% (streamed music kept)."),
                new Entry("Clean Materials (strip unused properties)", "Removes properties left over from other shaders in every material (they were most of the materials' size)."),
            }),
            ("World menu", new[]
            {
                new Entry("GMMenu: Add or Restyle (GW2 look, guild tab, join hint)", "Adds the GMMenu to the world or restyles it in the GW2 look, with the guild tab and join hint."),
                new Entry("World Music (the map's GW2 playlist)", "Plays the map's GW2 music playlist (Mad King's Labyrinth: madkingmaze) as world music, with the game's silences between tracks."),
                new Entry("Carving Pumpkins", "Places the game's carving pumpkins at the wiki map's spots: Carve turns one into a lit jack-o-lantern for a while (each player on their own)."),
                new Entry("Candy Corn Nodes (gatherable)", "Places Raw Candy Corn and Rich Candy Corn Veins on open maze floor: each player gathers them (3 / 10 strikes), then they respawn."),
                new Entry("Candy Corn Node Sounds (in place)", "Gives every candy corn node GW2's mining sounds (pick hit, ore hit, the node running dry), where it stands."),
                new Entry("Haunted Doors (knock, open, close, respawn)", "Places Halloween Haunted Doors at the wiki map's spawn points: anyone knocks on the door (everyone hears it); its ring-knocker handles open and close it, GMs only; a closed door comes back elsewhere 4-6 minutes later."),
                new Entry("Drop GW2 Waypoint Model (at the Scene view)", "Puts GW2's waypoint (model 106188: the hovering shard, its ground rune, light and hum) at the Scene view's centre, in any map; the world import places one at each API waypoint."),
                new Entry("Talkable NPCs (GW2 dialogue)", "Places the map's talkable NPCs (Mad King's Labyrinth: the Mad Spirits) with GW2's dialogue from the wiki: Talk opens a floating panel with the line and a scrolling list of replies."),
            }),
        };

        static readonly string[] Order =
        {
            "Import World...",
            "Static Skinned Meshes to Mesh Renderers",
            "Performance Pass (particles, animators, shadows, terrain)",
            "GW2 Lighting Look (realtime sun, no bounce)",
            "GW2 Realtime Lights (Light Volumes point lights)",
            "Merge Coarse Light Volumes (32-volume limit)",
            "Bake Lighting and Occlusion (Bakery)",
            "Build Cell Proxies (far cells as one mesh)",
            "Optimize Build Size",
        };

        Vector2 scroll;
        List<string> others;

        [MenuItem("Castlemist/About and Help...", false, 0)]
        public static void Open()
        {
            var w = GetWindow<About>(true, "Castlemist", true);
            w.minSize = new Vector2(520, 420);
        }

        void OnEnable()
        {
            var known = new HashSet<string>(Groups.SelectMany(g => g.entries).Select(e => e.Item)) { "About and Help..." };
            others = TypeCache.GetMethodsWithAttribute<MenuItem>()
                .SelectMany(m => m.GetCustomAttributes(typeof(MenuItem), false).Cast<MenuItem>())
                .Where(a => !a.validate && a.menuItem.StartsWith("Castlemist/"))
                .Select(a => a.menuItem.Substring("Castlemist/".Length))
                .Where(n => !known.Contains(n)).Distinct().OrderBy(n => n).ToList();
        }

        void OnGUI()
        {
            var wrap = new GUIStyle(EditorStyles.wordWrappedLabel);
            scroll = EditorGUILayout.BeginScrollView(scroll);
            EditorGUILayout.LabelField("Castlemist for Unity", EditorStyles.boldLabel);
            EditorGUILayout.LabelField("Guild Wars 2 maps and models from castlemist exports, as VRChat worlds. Every command below is also under the Castlemist menu.", wrap);
            EditorGUILayout.Space();
            EditorGUILayout.LabelField("Usual order after an import", EditorStyles.boldLabel);
            for (int i = 0; i < Order.Length; ++i) EditorGUILayout.LabelField((i + 1) + ". " + Order[i], wrap);
            foreach (var (group, entries) in Groups)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField(group, EditorStyles.boldLabel);
                foreach (Entry e in entries) Row(e.Item, e.Text, wrap);
            }
            if (others != null && others.Count > 0)
            {
                EditorGUILayout.Space();
                EditorGUILayout.LabelField("Other", EditorStyles.boldLabel);
                foreach (string n in others) Row(n, "", wrap);
            }
            EditorGUILayout.EndScrollView();
        }

        static void Row(string item, string text, GUIStyle wrap)
        {
            using (new EditorGUILayout.HorizontalScope(EditorStyles.helpBox))
            {
                using (new EditorGUILayout.VerticalScope())
                {
                    EditorGUILayout.LabelField(item, EditorStyles.miniBoldLabel);
                    if (text.Length > 0) EditorGUILayout.LabelField(text, wrap);
                }
                if (GUILayout.Button("Run", GUILayout.Width(48), GUILayout.Height(32)))
                    EditorApplication.ExecuteMenuItem("Castlemist/" + item);
            }
        }
    }
}
