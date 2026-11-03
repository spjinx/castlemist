// castlemist world export: the GW2 map of the map, with its waypoints on it as teleport buttons
// (2026-10-25). The image is the official world map: tiles.guildwars2.com/<continent>/<floor>/7/x/y.jpg
// (zoom 7: one continent unit is one pixel, 256 px tiles) over the map's continent_rect, stitched into
// Materials/waypoints/Map_<apiId>.png. "GW2 Waypoint Map" is a world-space canvas (VRChat's UI shape
// for the laser) with that image and one button per GW2Waypoints point at its continent coordinate
// (the game's map icons from environment/map_icons when the export wrote them, else coloured dots;
// the waypoint icon swaps to its hover icon), clicking SendCustomEvent("_TP<k>")
// on the GW2Waypoints behaviour. Built under "GW2 Waypoints", PanelWidth metres wide, by the spawn;
// move it into the GMMenu (or anywhere). Needs the network once.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEditor.Events;
using UnityEngine;
using UnityEngine.Events;
using UnityEngine.Networking;
using UnityEngine.UI;

namespace Castlemist
{
    public static class WaypointMap
    {
        public const string PanelName = "GW2 Waypoint Map";
        public const float PanelWidth = 1.6f;   // metres

        /// Builds the panel for @p points (continent coords @p coords, kinds @p kinds) under @p group.
        public static bool Build(GameObject group, Component waypoints, string root, int apiId, int continent, int floor,
                                 double[] contRect, List<string> names, List<string> kinds, List<Vector2> coords, List<string> warnings)
        {
            Transform old = group.transform.Find(PanelName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            int x0 = (int)Math.Floor(contRect[0]), y0 = (int)Math.Floor(contRect[1]);
            int W = (int)Math.Ceiling(contRect[2]) - x0, H = (int)Math.Ceiling(contRect[3]) - y0;
            if (W <= 0 || H <= 0 || W > 8192 || H > 8192) { warnings.Add("waypoint map: continent rect " + W + "x" + H + " out of range"); return false; }

            // Stitch the tiles; Unity rows run bottom-up, the tiles' top-down.
            var img = new Texture2D(W, H, TextureFormat.RGB24, false);
            var fill = new Color32[W * H];
            for (int i = 0; i < fill.Length; ++i) fill[i] = new Color32(20, 24, 28, 255);
            img.SetPixels32(fill);
            int missing = 0;
            for (int ty = y0 / 256; ty <= (y0 + H - 1) / 256; ++ty)
                for (int tx = x0 / 256; tx <= (x0 + W - 1) / 256; ++tx)
                {
                    byte[] bytes = Fetch("https://tiles.guildwars2.com/" + continent + "/" + floor + "/7/" + tx + "/" + ty + ".jpg");
                    var tile = new Texture2D(2, 2);
                    if (bytes == null || !tile.LoadImage(bytes)) { ++missing; UnityEngine.Object.DestroyImmediate(tile); continue; }
                    Color32[] tp = tile.GetPixels32();
                    int tw = tile.width, th = tile.height;
                    for (int py = 0; py < th; ++py)
                    {
                        int cy = ty * 256 + (th - 1 - py) * 256 / th;   // tile row py from the top
                        int iy = cy - y0;
                        if (iy < 0 || iy >= H) continue;
                        for (int px = 0; px < tw; ++px)
                        {
                            int ix = tx * 256 + px * 256 / tw - x0;
                            if (ix < 0 || ix >= W) continue;
                            img.SetPixel(ix, H - 1 - iy, tp[py * tw + px]);
                        }
                    }
                    UnityEngine.Object.DestroyImmediate(tile);
                }
            if (missing > 0) warnings.Add("waypoint map: " + missing + " map tile(s) did not download");
            img.Apply();
            string dir = root + "/Materials/waypoints";
            if (!AssetDatabase.IsValidFolder(dir)) AssetDatabase.CreateFolder(root + "/Materials", "waypoints");
            string path = dir + "/Map_" + apiId + ".png";
            File.WriteAllBytes(path, img.EncodeToPNG());
            UnityEngine.Object.DestroyImmediate(img);
            AssetDatabase.ImportAsset(path);
            if (AssetImporter.GetAtPath(path) is TextureImporter ti)
            {
                ti.textureType = TextureImporterType.Default;
                ti.mipmapEnabled = true;
                ti.wrapMode = TextureWrapMode.Clamp;
                ti.maxTextureSize = 4096;
                ti.textureCompression = TextureImporterCompression.CompressedHQ;
                ti.SaveAndReimport();
            }
            var mapTex = AssetDatabase.LoadAssetAtPath<Texture2D>(path);

            // The canvas: pixels = continent units; scaled to PanelWidth metres.
            var go = new GameObject(PanelName, typeof(RectTransform));
            go.transform.SetParent(group.transform, false);
            var canvas = go.AddComponent<Canvas>();
            canvas.renderMode = RenderMode.WorldSpace;
            go.AddComponent<GraphicRaycaster>();
            Type shape = SceneSetup.FindType("VRC.SDK3.Components.VRCUiShape");
            if (shape != null) go.AddComponent(shape);
            var rt = (RectTransform)go.transform;
            rt.sizeDelta = new Vector2(W, H);
            go.transform.localScale = Vector3.one * (PanelWidth / W) / Mathf.Max(group.transform.lossyScale.x, 1e-6f);
            Transform spawn = group.transform.parent != null ? group.transform.parent.Find("Spawn") : null;
            if (spawn != null) go.transform.position = spawn.position + Vector3.up * 1.5f + Vector3.forward * 2f;
            go.layer = 0;

            var bg = new GameObject("Map", typeof(RectTransform));
            bg.transform.SetParent(go.transform, false);
            var raw = bg.AddComponent<RawImage>();
            raw.texture = mapTex;
            Stretch((RectTransform)bg.transform);

            Sprite knob = AssetDatabase.GetBuiltinExtraResource<Sprite>("UI/Skin/Knob.psd");
            // The game's own map icons, when the export wrote them (environment/map_icons/<name>.png).
            Sprite Icon(string name)
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
            var icons = new Dictionary<string, Sprite>
            {
                { "waypoint", Icon("waypoint") }, { "vista", Icon("vista") }, { "landmark", Icon("landmark") },
                { "unlock", Icon("landmark") }, { "heropoint", Icon("heropoint") },
            };
            Sprite wpHover = Icon("waypoint_hover");
            Font font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");
            Component udon = Backing(waypoints);
            for (int k = 0; k < coords.Count; ++k)
            {
                string kind = kinds[k];
                Sprite icon = icons.TryGetValue(kind, out Sprite si) ? si : null;
                Color col = icon != null ? Color.white : kind == "waypoint" ? new Color(1f, 0.8f, 0.2f) : kind == "vista" ? new Color(0.75f, 0.45f, 1f)
                          : kind == "landmark" ? Color.white : Color.grey;
                float size = kind == "waypoint" ? 56f : 40f;
                var b = new GameObject(names[k], typeof(RectTransform));
                b.transform.SetParent(go.transform, false);
                var brt = (RectTransform)b.transform;
                brt.anchorMin = brt.anchorMax = Vector2.zero;
                brt.pivot = new Vector2(0.5f, 0.5f);
                brt.sizeDelta = new Vector2(size, size);
                brt.anchoredPosition = new Vector2(coords[k].x - x0, H - (coords[k].y - y0));
                var image = b.AddComponent<Image>();
                image.sprite = icon != null ? icon : knob;
                image.color = col;
                image.preserveAspect = true;
                var button = b.AddComponent<Button>();
                button.targetGraphic = image;
                if (kind == "waypoint" && icon != null && wpHover != null)
                {
                    button.transition = Selectable.Transition.SpriteSwap;
                    button.spriteState = new SpriteState { highlightedSprite = wpHover, pressedSprite = wpHover, selectedSprite = wpHover };
                }
                if (udon != null)
                {
                    var call = (UnityAction<string>)Delegate.CreateDelegate(typeof(UnityAction<string>), udon, "SendCustomEvent");
                    UnityEventTools.AddStringPersistentListener(button.onClick, call, "_TP" + k);
                }
                if (kind != "waypoint") continue;
                var label = new GameObject("Label", typeof(RectTransform));
                label.transform.SetParent(b.transform, false);
                var lrt = (RectTransform)label.transform;
                lrt.anchoredPosition = new Vector2(0, -40);
                lrt.sizeDelta = new Vector2(360, 30);
                var text = label.AddComponent<Text>();
                text.font = font;
                text.fontSize = 22;
                text.alignment = TextAnchor.MiddleCenter;
                text.color = Color.white;
                text.text = names[k].Replace(" Waypoint", "");
                text.raycastTarget = false;
                label.AddComponent<Outline>().effectColor = Color.black;
            }
            if (udon == null) warnings.Add("waypoint map: no UdonBehaviour behind GW2Waypoints: the buttons do nothing yet (refresh again once UdonSharp has compiled)");
            return true;
        }

        static void Stretch(RectTransform r)
        {
            r.anchorMin = Vector2.zero; r.anchorMax = Vector2.one; r.offsetMin = r.offsetMax = Vector2.zero;
        }

        /// The UdonBehaviour an UdonSharp proxy compiles to (its SendCustomEvent is what VRChat UI calls).
        static Component Backing(Component proxy)
        {
            Type util = SceneSetup.FindType("UdonSharpEditor.UdonSharpEditorUtility");
            var m = util != null ? util.GetMethod("GetBackingUdonBehaviour", new[] { proxy.GetType().BaseType ?? proxy.GetType() }) : null;
            if (m == null && util != null)
                foreach (var mi in util.GetMethods()) if (mi.Name == "GetBackingUdonBehaviour" && mi.GetParameters().Length == 1) { m = mi; break; }
            return m != null ? m.Invoke(null, new object[] { proxy }) as Component : null;
        }

        static byte[] Fetch(string url)
        {
            using (var r = UnityWebRequest.Get(url))
            {
                var op = r.SendWebRequest();
                var t0 = DateTime.UtcNow;
                while (!op.isDone && (DateTime.UtcNow - t0).TotalSeconds < 20) System.Threading.Thread.Sleep(10);
                return r.result == UnityWebRequest.Result.Success ? r.downloadHandler.data : null;
            }
        }
    }
}
