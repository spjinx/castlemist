// castlemist world export: the map's layers as toggles (2026-10-25). The laye chunk puts props on
// numbered layers (manifest instances[].layers; ids below 1024 match manifest layers[], larger values
// with bit 31 set are not layer ids and are skipped). Each layer with placed props gets
// "GW2 Map Layers/<name>" with Runtime/GW2MapLayer listing them, on by default (as exported).
// Names: the chunk stores none, only a GUID; Names holds what castlemist has identified by the props
// (Lion's Arch: layers 0 and 2 carry the Halloween dressing, the dark doors 499261 / 648540 and the
// cauldron 1869318); the rest are "Layer <id>". Rerunning replaces it.
// 2026-10-26, festivals: Festivals groups layers into named festivals; their layers are driven by one
// synced Runtime/GW2Festival ("GW2 Map Layers/GW2 Festivals": Normal = none of them, the default) and
// start off. The map lights on layers move under "GW2 Map Lights/Layer <ids>" (GW2Lights.LayerParent),
// which the layers switch too; the layered props stop being occluders (hidden, they would still cull
// what is behind them: re-bake occlusion) and stay out of the cell proxies (CellProxies).

using System;
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public static class MapLayers
    {
        public const string GroupName = "GW2 Map Layers";

        /// Layer names by (map fileId, layer id), identified from their props.
        static readonly Dictionary<long, Dictionary<long, string>> Names = new Dictionary<long, Dictionary<long, string>>
        {
            { 191000, new Dictionary<long, string> { { 2, "Halloween" }, { 0, "Halloween (second set)" } } },
        };

        /// Layers known not to be festival dressing (they stay on, part of the map as it is), by map
        /// fileId. 2026-10-26, Lion's Arch: only Halloween is on the map this year; 1, 3, 4, 8, 12, 16 and 32
        /// (ribbons, balloons, crates, market goods...) are kept as exported. A layer in neither this nor
        /// Festivals is new: Setup names its props in a warning, for a festival a patch may have added.
        static readonly Dictionary<long, long[]> KnownOther = new Dictionary<long, long[]>
        {
            { 191000, new long[] { 1, 3, 4, 8, 12, 16, 32 } },
        };

        /// Festivals by map fileId: a name and the layers that are its dressing.
        static readonly Dictionary<long, (string name, long[] layers)[]> Festivals = new Dictionary<long, (string, long[])[]>
        {
            { 191000, new[] { ("Halloween", new long[] { 0, 2 }) } },
        };

        /// Models that stay shown whatever the festival, by map fileId, though GW2 puts placements of them
        /// on a festival's layers: the user's call, not the data's (2026-10-26, Lion's Arch: the rug 73346
        /// and 1869316 are part of the normal city).
        static readonly Dictionary<long, HashSet<long>> AlwaysOn = new Dictionary<long, HashSet<long>>
        {
            { 191000, new HashSet<long> { 73346, 1869316 } },
        };

        public const string FestivalsName = "GW2 Festivals";

        public static int Setup(GameObject mapRoot, WorldManifest m, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            Type type = SceneSetup.FindType("Castlemist.GW2MapLayer");
            if (type == null) { warnings.Add("layers: Castlemist.GW2MapLayer is not compiled yet: refresh again once UdonSharp has compiled"); return 0; }
            var objects = new SortedDictionary<long, List<GameObject>>();
            AlwaysOn.TryGetValue(m.MapFileId, out HashSet<long> alwaysOn);
            int kept = 0;
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                Transform props = mapRoot.transform.Find("Cell_" + c.Id + "/Props");
                if (props == null) continue;
                foreach (WorldManifest.InstanceEntry i in c.Instances)
                {
                    if (i.Layers == null || i.Model < 0 || i.Model >= m.Models.Count) continue;
                    Transform inst = props.Find(m.Models[i.Model].FileId + "_" + i.Prop);
                    if (inst == null) continue;
                    if (alwaysOn != null && alwaysOn.Contains(m.Models[i.Model].FileId)) { inst.gameObject.SetActive(true); ++kept; continue; }
                    foreach (long id in i.Layers)
                    {
                        if (id < 0 || id >= 1024) continue;
                        if (!objects.TryGetValue(id, out var list)) objects[id] = list = new List<GameObject>();
                        list.Add(inst.gameObject);
                    }
                }
            }
            // The occlusion bake must not count them: hidden, they would still hide what is behind them.
            int unoccluding = 0;
            foreach (var list in objects.Values)
                foreach (GameObject o in list)
                    foreach (Transform t in o.GetComponentsInChildren<Transform>(true))
                    {
                        StaticEditorFlags f = GameObjectUtility.GetStaticEditorFlags(t.gameObject);
                        if ((f & StaticEditorFlags.OccluderStatic) == 0) continue;
                        GameObjectUtility.SetStaticEditorFlags(t.gameObject, f & ~StaticEditorFlags.OccluderStatic);
                        ++unoccluding;
                    }
            int lights = GroupMapLights(mapRoot, m, objects);
            if (objects.Count == 0) return 0;
            Names.TryGetValue(m.MapFileId, out var names);
            var group = new GameObject(GroupName);
            group.transform.SetParent(mapRoot.transform, false);
            var comps = new List<Component>();
            var byId = new Dictionary<long, Component>();
            Festivals.TryGetValue(m.MapFileId, out var festivals);
            foreach (var kv in objects)
            {
                string name = names != null && names.TryGetValue(kv.Key, out string n) ? n + " (layer " + kv.Key + ")" : "Layer " + kv.Key;
                var go = new GameObject(name);
                go.transform.SetParent(group.transform, false);
                var comp = SceneSetup.AddUdonAware(go, type);
                SceneSetup.SetField(comp, "objects", kv.Value.ToArray());
                // A festival's layer starts off (Normal); the rest as exported.
                bool festival = festivals != null && Array.Exists(festivals, f => Array.IndexOf(f.layers, kv.Key) >= 0);
                SceneSetup.SetField(comp, "on", !festival);
                comps.Add(comp);
                byId[kv.Key] = comp;
            }
            Array others = Array.CreateInstance(type, comps.Count);
            for (int k = 0; k < comps.Count; ++k) others.SetValue(comps[k], k);
            foreach (Component c in comps)
            {
                SceneSetup.SetField(c, "others", others);
                SceneSetup.CopyProxyToUdon(c);
            }
            string fest = festivals != null ? FestivalSwitch(group, festivals, byId) : "no festivals named";
            KnownOther.TryGetValue(m.MapFileId, out long[] other);
            foreach (var kv in objects)
            {
                bool known = (festivals != null && Array.Exists(festivals, f => Array.IndexOf(f.layers, kv.Key) >= 0)) || (other != null && Array.IndexOf(other, kv.Key) >= 0);
                if (known) continue;
                var top = new Dictionary<string, int>();
                foreach (GameObject o in kv.Value) { string mdl = o.name.Split('_')[0]; top[mdl] = top.TryGetValue(mdl, out int c) ? c + 1 : 1; }
                var list = new List<KeyValuePair<string, int>>(top);
                list.Sort((a, b) => b.Value.CompareTo(a.Value));
                warnings.Add("layers: NEW layer " + kv.Key + " (" + kv.Value.Count + " object(s); models " +
                             string.Join(", ", list.GetRange(0, Math.Min(6, list.Count)).ConvertAll(x => x.Key + " x" + x.Value)) +
                             "): a festival? add it to MapLayers.Festivals, else to KnownOther");
            }
            warnings.Add("layers: " + objects.Count + " map layer toggle(s) under " + GroupName + " (ToggleLayer / LayerOn / LayerOff for GMMenu buttons), " +
                         lights + " layered map light(s) switched with them, " + kept + " kept always on (AlwaysOn), " + unoccluding + " layered object(s) no longer occluders (re-bake occlusion); " + fest);
            return objects.Count;
        }

        /// "GW2 Festivals" under @p group: a GW2Festival over the festivals' layers (Normal first).
        static string FestivalSwitch(GameObject group, (string name, long[] layers)[] festivals, Dictionary<long, Component> byId)
        {
            Type ft = SceneSetup.FindType("Castlemist.GW2Festival"), lt = SceneSetup.FindType("Castlemist.GW2MapLayer");
            if (ft == null || lt == null) return "GW2Festival is not compiled yet (refresh again)";
            var names = new List<string> { "Normal" };
            var layers = new List<Component>();
            var of = new List<int>();
            for (int f = 0; f < festivals.Length; ++f)
            {
                names.Add(festivals[f].name);
                foreach (long id in festivals[f].layers)
                    if (byId.TryGetValue(id, out Component c)) { layers.Add(c); of.Add(f + 1); }
            }
            var go = new GameObject(FestivalsName);
            go.transform.SetParent(group.transform, false);
            Component comp = SceneSetup.AddUdonAware(go, ft);
            Array arr = Array.CreateInstance(lt, layers.Count);
            for (int k = 0; k < layers.Count; ++k) arr.SetValue(layers[k], k);
            SceneSetup.SetField(comp, "names", names.ToArray());
            SceneSetup.SetField(comp, "layers", arr);
            SceneSetup.SetField(comp, "festivalOf", of.ToArray());
            SceneSetup.CopyProxyToUdon(comp);
            return "festivals: " + string.Join(", ", names) + " (GW2 Festivals: _F0.._F" + (names.Count - 1) + ")";
        }

        /// The map lights on layers moved under "GW2 Map Lights/Layer <ids>" (GW2Lights' names, found by
        /// replaying its numbering), each container listed with its layers in @p objects; returns how many.
        static int GroupMapLights(GameObject mapRoot, WorldManifest m, SortedDictionary<long, List<GameObject>> objects)
        {
            Transform group = mapRoot.transform.Find(GW2Lights.MapLightsName);
            if (group == null) return 0;
            var byName = new Dictionary<string, Transform>();
            foreach (Transform t in group.GetComponentsInChildren<Transform>(true)) if (t != group && !byName.ContainsKey(t.name)) byName[t.name] = t;
            int made = 0, moved = 0;
            var containers = new HashSet<Transform>();
            foreach (WorldManifest.MapLight l in m.MapLights)
            {
                if (!(l.Intensity > 0) || l.Far <= 0) continue;
                string name = GW2Lights.LightName(l, made++);
                long[] ids = GW2Lights.RealLayers(l.Layers);
                if (ids.Length == 0 || !byName.TryGetValue(name, out Transform t)) continue;
                Transform parent = GW2Lights.LayerParent(group, ids);
                if (t.parent != parent) t.SetParent(parent, true);
                containers.Add(parent);
                ++moved;
            }
            foreach (Transform c in containers)
                foreach (string part in c.name.Substring("Layer ".Length).Split('+'))
                {
                    long id = long.Parse(part);
                    if (!objects.TryGetValue(id, out var list)) objects[id] = list = new List<GameObject>();
                    list.Add(c.gameObject);
                }
            return moved;
        }
    }
}
