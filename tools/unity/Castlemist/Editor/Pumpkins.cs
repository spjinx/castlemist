// castlemist world import: Halloween Carving Pumpkins (2026-10-27). The game spawns them (server-side); the
// spots are the GW2 wiki's "Carving Pumpkin locations (Mad King's Labyrinth).jpg" (WikiMaps.CarvingPumpkins,
// its 28 red dots) dropped onto the collision. The models are the game's carving pumpkins, Lion's Arch's
// Halloween props 499277 / 499278 / 499279 / 499280 (added to this export with gw2dat_cli model-export):
// one model each, its "On" clip hiding the uncarved shell (the carved pumpkin and candle stay) and "Off"
// the candle and the carved pumpkin (the export's clips[].boneVis with meshVis). Which variant stands
// where is a fixed-seed pick (the wiki's map does not say). Runtime/GW2CarvingPumpkin: local carving.
// 2026-10-27: the carve sound: a quick slash (GW2's foley "stab", 1341156-1341160, ~1 s) over a splat (its prop
// "splat", 56905-56909, 0.2-0.8 s), one of each at random (the user: quick, "a slash or sploosh"; the prop
// sound "carve", 2247097, was 9 s and wrong). Found with gw2dat_cli soundscan; sounds/pumpkin/.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class Pumpkins
    {
        public const string RootName = "GW2 Carving Pumpkins";
        static readonly long[] Models = { 499277, 499278, 499279, 499280 };

        // The red dots (image pixels) of the 300 px wiki map.
        static readonly Vector2[] Spots =
        {
            new Vector2(134.8f, 33.1f), new Vector2(189.3f, 34.5f), new Vector2(73.7f, 37.3f), new Vector2(260.6f, 73.6f),
            new Vector2(73.5f, 106.8f), new Vector2(158.6f, 117.4f), new Vector2(178.5f, 119.7f), new Vector2(49.3f, 124.9f),
            new Vector2(156.7f, 128.0f), new Vector2(259.4f, 129.6f), new Vector2(222.9f, 135.6f), new Vector2(57.3f, 137.4f),
            new Vector2(215.1f, 159.3f), new Vector2(270.6f, 172.5f), new Vector2(122.2f, 173.3f), new Vector2(51.4f, 175.3f),
            new Vector2(177.5f, 181.5f), new Vector2(158.8f, 205.6f), new Vector2(179.5f, 212.1f), new Vector2(230.3f, 220.6f),
            new Vector2(193.0f, 223.2f), new Vector2(69.6f, 237.7f), new Vector2(133.9f, 238.8f), new Vector2(111.5f, 241.5f),
            new Vector2(238.9f, 242.3f), new Vector2(54.4f, 248.2f), new Vector2(118.9f, 253.3f), new Vector2(206.4f, 265.0f),
        };

        [MenuItem("Castlemist/Carving Pumpkins")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            GameObject env = ImmersiveLighting.EnvRoot();
            if (env == null) return "pumpkins: no GW2_<id>_Environment root in the open scene";
            if (!long.TryParse(env.name.Replace("GW2_", "").Replace("_Environment", ""), out long mapId)) return "pumpkins: cannot read the map id";
            if (mapId != 2499169) return "pumpkins: only Mad King's Labyrinth has its spots";
            GameObject mapRoot = env.scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + mapId);
            string root = Path.GetDirectoryName(env.scene.path).Replace('\\', '/');
            var made = new List<string>();
            if (!Demolishables.EnsurePrograms(made)) return "pumpkins: UdonSharp is not in this project";
            if (made.Count > 0) return "pumpkins: UdonSharp program assets made: run again once UdonSharp has compiled";
            Type type = SceneSetup.FindType("Castlemist.GW2CarvingPumpkin");
            if (type == null || mapRoot == null) return "pumpkins: GW2CarvingPumpkin not compiled, or no GW2_" + mapId + " root";
            var prefabs = Models.Select(id => AssetDatabase.LoadAssetAtPath<GameObject>(root + "/Prefabs/" + id + ".prefab")).ToArray();
            if (prefabs.Any(p => p == null)) return "pumpkins: refresh models " + string.Join(", ", Models) + " first";
            var hiddenOn = Models.Select(id => HiddenIn(root, id, "On")).ToArray();
            AudioClip[] Clips(params int[] ids) => ids.Select(id => AssetDatabase.LoadAssetAtPath<AudioClip>(root + "/sounds/pumpkin/" + id + ".mp3")).Where(c => c != null).ToArray();
            AudioClip[] slash = Clips(1341156, 1341157, 1341158, 1341159, 1341160), splat = Clips(56905, 56906, 56907, 56908, 56909);

            Transform old = env.transform.Find(RootName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var group = new GameObject(RootName);
            group.transform.SetParent(env.transform, false);
            Physics.SyncTransforms();
            float scale = mapRoot.transform.lossyScale.x;
            var rng = new System.Random(2499169 + 7);
            int placed = 0;
            foreach (Vector2 px in Spots)
            {
                if (!WikiMaps.ToWorld(mapId, WikiMaps.CarvingPumpkins, px, mapRoot.transform, out Vector3 at)) continue;
                float y = WikiMaps.Ground(at.x, at.z);
                if (!float.IsNaN(y)) at.y = y;
                int v = rng.Next(Models.Length);
                var node = new GameObject("Carving Pumpkin " + (placed + 1));
                node.transform.SetParent(group.transform, false);
                node.transform.position = at;
                node.transform.rotation = Quaternion.Euler(0f, (float)rng.NextDouble() * 360f, 0f);
                var model = (GameObject)PrefabUtility.InstantiatePrefab(prefabs[v]);
                model.transform.SetParent(node.transform, false);
                // model-export's glb roots are Z-up to Y-up by a -90 degree turn about X ((x, y, z) -> (x, z, -y)),
                // the world export's by (x, -z, -y): the same but mirrored in Y. Upside down without the flip.
                model.transform.localScale = new Vector3(scale, -scale, scale);
                Animator an = model.GetComponent<Animator>();
                if (an != null) an.enabled = false;   // the behaviour shows the states by renderer

                var uncarved = new List<Renderer>();
                var carved = new List<Renderer>();
                foreach (Renderer r in model.GetComponentsInChildren<Renderer>(true))
                {
                    if (r is ParticleSystemRenderer) continue;
                    if (hiddenOn[v].Any(h => r.name.StartsWith(h, StringComparison.Ordinal))) uncarved.Add(r);
                    else carved.Add(r);
                }

                var click = new GameObject("GW2 Carving Pumpkin");
                click.transform.SetParent(node.transform, false);
                click.transform.localPosition = new Vector3(0f, 0.5f, 0f);
                var box = click.AddComponent<BoxCollider>();
                box.isTrigger = true;
                box.size = new Vector3(1.4f, 1.2f, 1.4f);
                Component comp = SceneSetup.AddUdonAware(click, type);
                SceneSetup.SetField(comp, "uncarved", uncarved.ToArray());
                SceneSetup.SetField(comp, "carved", carved.ToArray());
                if (slash.Length + splat.Length > 0)
                {
                    var src = click.AddComponent<AudioSource>();
                    src.playOnAwake = false; src.loop = false; src.spatialBlend = 1f;
                    src.rolloffMode = AudioRolloffMode.Logarithmic; src.minDistance = 2f; src.maxDistance = 20f;
                    SceneSetup.SetField(comp, "carveSound", src);
                    SceneSetup.SetField(comp, "slashClips", slash);
                    SceneSetup.SetField(comp, "splatClips", splat);
                }
                SceneSetup.CopyProxyToUdon(comp);
                ++placed;
            }
            MapMarkers.Setup();   // 2026-10-27: carved pumpkins on the GMMenu map
            EditorSceneManager.MarkSceneDirty(env.scene);
            return "pumpkins: " + placed + " carving pumpkin(s) at the wiki map's spots";
        }

        /// The renderer names (meshVis keys) a clip hides: its boneVis bones that toggle off.
        static List<string> HiddenIn(string root, long id, string clip)
        {
            var hidden = new List<string>();
            string path = root + "/models/" + id + "/" + id + "_particles.json";
            if (!File.Exists(path)) return hidden;
            var doc = MiniJson.Parse(File.ReadAllText(path)) as Dictionary<string, object>;
            var meshVis = doc != null && doc.TryGetValue("meshVis", out object mv) ? mv as Dictionary<string, object> : null;
            var clips = doc != null && doc.TryGetValue("clips", out object cl) ? cl as List<object> : null;
            if (meshVis == null || clips == null) return hidden;
            foreach (object o in clips)
            {
                if (!(o is Dictionary<string, object> c) || !(c.TryGetValue("name", out object n) && (string)n == clip)) continue;
                if (!(c.TryGetValue("boneVis", out object bv) && bv is Dictionary<string, object> bones)) continue;
                foreach (var kv in meshVis)
                    if (bones.ContainsKey(kv.Value as string ?? "")) hidden.Add(kv.Key);
            }
            return hidden;
        }
    }
}
