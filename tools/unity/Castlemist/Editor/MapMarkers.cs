// castlemist world import: the live markers on the GMMenu's GW2 map (2026-10-27, the user). On the
// "GW2 Waypoint Map" panel (WaypointMap.cs: one pixel per continent unit, anchored bottom-left), under
// "GW2 Markers":
//   haunted doors   GW2's event icon (map_special_event, 502087) in an event circle, shown while the door
//                   stands (GW2HauntedDoor.mapMarker; the doors are synced, so everyone sees them)
//   pumpkins        the Shadow of the Mad King jack-o'-lantern (1862652), shown once you carve that
//                   pumpkin (GW2CarvingPumpkin.mapMarker; local, as the carving)
//   candy corn      GW2's mining-node icon (map_node_mining, 157334), hidden while that node is spent
//                   (GW2GatherNode.mapMarker; local)
// The icons are the game's (environment/map_icons/<name>.png, the export's). The event circle is
// castlemist's: GW2 draws its event areas in the UI (an orange fill with a bright rim), with no texture
// found for it in the dat's UI art (2026-10-27 survey of 2320 UI textures); this one copies that look.
// World positions reach the map through the map's map_rect / continent_rect (WikiMaps.Maps). Rerunning
// replaces the markers; HauntedDoors, Pumpkins and GatherNodes run it after placing theirs.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.UI;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class MapMarkers
    {
        public const string GroupName = "GW2 Markers";
        public const float EventCircle = 70f, EventIcon = 30f, PumpkinIcon = 30f, NodeIcon = 26f;   // continent units (map pixels)
        static readonly Color EventOrange = new Color(1f, 0.62f, 0.16f);

        [MenuItem("Castlemist/Map Markers (doors, carved pumpkins, candy corn)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Setup()); }

        public static string Setup()
        {
            var scene = EditorSceneManager.GetActiveScene();
            string root = Path.GetDirectoryName(scene.path).Replace('\\', '/');
            GameObject mapRoot = scene.GetRootGameObjects().FirstOrDefault(g => g.name.StartsWith("GW2_", StringComparison.Ordinal) && !g.name.EndsWith("_Environment", StringComparison.Ordinal));
            if (mapRoot == null || !long.TryParse(mapRoot.name.Substring(4), out long mapId)) return "map markers: no GW2_<id> root";
            if (!WikiMaps.Maps.TryGetValue(mapId, out WikiMaps.MapInfo info)) return "map markers: map " + mapId + " has no map / continent rect (WikiMaps.Maps)";
            Transform panel = Object.FindObjectsOfType<Transform>(true).FirstOrDefault(t => t.name == WaypointMap.PanelName);
            if (panel == null) return "map markers: no " + WaypointMap.PanelName + " (Waypoints first)";

            Transform old = panel.Find(GroupName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            var group = new GameObject(GroupName, typeof(RectTransform));
            group.transform.SetParent(panel, false);
            var grt = (RectTransform)group.transform;
            grt.anchorMin = Vector2.zero; grt.anchorMax = Vector2.one; grt.offsetMin = grt.offsetMax = Vector2.zero;
            group.transform.SetSiblingIndex(1);   // over the map image, under the waypoint buttons

            double[] cr = info.ContinentRect, mr = info.MapRect;
            int x0 = (int)Math.Floor(cr[0]), y0 = (int)Math.Floor(cr[1]);
            int H = (int)Math.Ceiling(cr[3]) - y0;
            Vector2 At(Vector3 world)
            {
                Vector3 l = mapRoot.transform.InverseTransformPoint(world);
                double cx = cr[0] + (l.x - mr[0]) / (mr[2] - mr[0]) * (cr[2] - cr[0]);
                double cy = cr[1] + (mr[3] - l.z) / (mr[3] - mr[1]) * (cr[3] - cr[1]);
                return new Vector2((float)(cx - x0), (float)(H - (cy - y0)));
            }

            Sprite ev = Icon(root, "event"), node = Icon(root, "node_mining"), pumpkin = Icon(root, "pumpkin_carved"), circle = Circle(root);
            int doors = 0, pumpkins = 0, nodes = 0;

            foreach (var door in Object.FindObjectsOfType<MonoBehaviour>(true).Where(m => m != null && m.GetType().Name == "GW2HauntedDoor"))
            {
                GameObject mk = Marker(group.transform, "Door " + (++doors), At(door.transform.position), circle, EventCircle, new Color(EventOrange.r, EventOrange.g, EventOrange.b, 1f));
                Image(mk.transform, "Icon", ev, EventIcon, Color.white);
                mk.SetActive(false);
                SceneSetup.SetField(door, "mapMarker", mk);
                SceneSetup.CopyProxyToUdon(door);
            }
            foreach (var p in Object.FindObjectsOfType<MonoBehaviour>(true).Where(m => m != null && m.GetType().Name == "GW2CarvingPumpkin"))
            {
                GameObject mk = Marker(group.transform, "Pumpkin " + (++pumpkins), At(p.transform.position), pumpkin, PumpkinIcon, Color.white);
                mk.SetActive(false);
                SceneSetup.SetField(p, "mapMarker", mk);
                SceneSetup.CopyProxyToUdon(p);
            }
            foreach (var n in Object.FindObjectsOfType<MonoBehaviour>(true).Where(m => m != null && m.GetType().Name == "GW2GatherNode"))
            {
                GameObject mk = Marker(group.transform, "Candy Corn " + (++nodes), At(n.transform.position), node, NodeIcon, Color.white);
                SceneSetup.SetField(n, "mapMarker", mk);
                SceneSetup.CopyProxyToUdon(n);
            }
            EditorSceneManager.MarkSceneDirty(scene);
            return "map markers: " + doors + " haunted door(s) (event circle while standing), " + pumpkins + " carving pumpkin(s) (shown once carved), " +
                   nodes + " candy corn node(s) (ore icon, hidden while spent)";
        }

        static GameObject Marker(Transform parent, string name, Vector2 pos, Sprite sprite, float size, Color col)
        {
            var go = new GameObject(name, typeof(RectTransform));
            go.transform.SetParent(parent, false);
            var rt = (RectTransform)go.transform;
            rt.anchorMin = rt.anchorMax = Vector2.zero;
            rt.pivot = new Vector2(0.5f, 0.5f);
            rt.sizeDelta = new Vector2(size, size);
            rt.anchoredPosition = pos;
            var img = go.AddComponent<Image>();
            img.sprite = sprite;
            img.color = col;
            img.preserveAspect = true;
            img.raycastTarget = false;   // the waypoint buttons stay clickable under them
            return go;
        }

        static void Image(Transform parent, string name, Sprite sprite, float size, Color col)
        {
            GameObject go = Marker(parent, name, Vector2.zero, sprite, size, col);
            var rt = (RectTransform)go.transform;
            rt.anchorMin = rt.anchorMax = new Vector2(0.5f, 0.5f);
            rt.anchoredPosition = Vector2.zero;
        }

        /// environment/map_icons/<name>.png as a sprite (the export's GW2 icon); null without it.
        static Sprite Icon(string root, string name)
        {
            string ip = root + "/environment/map_icons/" + name + ".png";
            if (AssetImporter.GetAtPath(ip) is TextureImporter it && it.textureType != TextureImporterType.Sprite)
            {
                it.textureType = TextureImporterType.Sprite;
                it.alphaIsTransparency = true;
                it.mipmapEnabled = false;
                it.SaveAndReimport();
            }
            return AssetDatabase.LoadAssetAtPath<Sprite>(ip);
        }

        /// The event circle (see the header): white, tinted by the Image; made once.
        static Sprite Circle(string root)
        {
            string path = root + "/environment/map_icons/event_circle.png";
            if (!File.Exists(path))
            {
                const int S = 256;
                var t = new Texture2D(S, S, TextureFormat.RGBA32, false);
                var px = new Color[S * S];
                for (int y = 0; y < S; ++y)
                    for (int x = 0; x < S; ++x)
                    {
                        float r = Mathf.Sqrt((x - S / 2f + 0.5f) * (x - S / 2f + 0.5f) + (y - S / 2f + 0.5f) * (y - S / 2f + 0.5f)) / (S / 2f);
                        float rim = Mathf.Clamp01(1f - Mathf.Abs(r - 0.93f) / 0.05f);      // the bright edge
                        float glow = Mathf.Clamp01(1f - Mathf.Abs(r - 0.9f) / 0.14f) * 0.35f;
                        float fill = r < 0.93f ? 0.16f : 0f;                                 // the faint area
                        float a = Mathf.Clamp01(Mathf.Max(rim, glow) + fill);
                        px[y * S + x] = new Color(1f, 1f, 1f, r > 1f ? 0f : a);
                    }
                t.SetPixels(px);
                t.Apply();
                File.WriteAllBytes(path, t.EncodeToPNG());
                Object.DestroyImmediate(t);
                AssetDatabase.ImportAsset(path);
            }
            return Icon(root, "event_circle");
        }
    }
}
