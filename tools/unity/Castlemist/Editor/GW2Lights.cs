// GW2Lights (2026-10-25): the map's own lights and its models' effect lights as Unity lights.
//
//   map lights     manifest mapLights (the lght chunk's point and spot lights) under the map root's
//                  "GW2 Map Lights": position map units (the root scales them), range = far x
//                  unityScale, intensity as stored, colour as the export reads it (B, G, R like the
//                  env lights; UNPROVEN for lght). Spot: forward = unityDirection, spotAngle = 2 x
//                  outerAngle (radians, read as the half angle; UNPROVEN), innerSpotAngle likewise.
//                  A light with intensity <= 0 is skipped: GW2's subtractive lights have no Unity
//                  counterpart. Each lght group stores two lights at one position; both are kept
//                  (what the second is for is not traced).
//   model lights   each placed prop's sidecar effectLights (ModelLightData) as a point light at its
//                  bone (else the prop's origin), "GW2 Light <k>" (colour as the sidecar reads it, range
//                  far model units x the prop's scale).
//
// Every light is Baked (1742 in Lion's Arch: realtime would not run in VRChat); a bake (Unity,
// Bakery, VRC Light Volumes, Castlemist > Setup Immersive Lighting) turns them into light.
// Intensities and the falloff are castlemist's choices. Rerunning replaces what it made.
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public static class GW2Lights
    {
        public const string MapLightsName = "GW2 Map Lights";
        public const string ModelLightPrefix = "GW2 Light ";

        /// Rebuilds "GW2 Map Lights" under @p mapRoot from @p m; returns the lights made.
        public static int MapLights(GameObject mapRoot, WorldManifest m, float unityScale, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(MapLightsName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            if (m.MapLights.Count == 0) return 0;
            var parent = new GameObject(MapLightsName);
            parent.transform.SetParent(mapRoot.transform, false);
            int made = 0, subtractive = 0, twins = 0;
            // 2026-10-27: Mad King's Labyrinth's lght stores every light twice (each of its 108 groups:
            // two lights identical in every field); one Light each.
            var seen = new HashSet<string>();
            foreach (WorldManifest.MapLight l in m.MapLights)
            {
                if (!(l.Intensity > 0) || l.Far <= 0) { if (l.Intensity < 0) ++subtractive; continue; }
                if (!seen.Add(TwinKey(l))) { ++twins; continue; }
                var go = new GameObject(LightName(l, made));
                go.transform.SetParent(LayerParent(parent.transform, l.Layers), false);
                go.transform.localPosition = new Vector3((float)l.Unity[0], (float)l.Unity[1], (float)l.Unity[2]);
                var light = go.AddComponent<Light>();
                light.type = l.Spot ? LightType.Spot : LightType.Point;
                if (l.Spot && l.UnityDirection != null)
                {
                    var d = new Vector3((float)l.UnityDirection[0], (float)l.UnityDirection[1], (float)l.UnityDirection[2]);
                    if (d.sqrMagnitude > 1e-8f) go.transform.rotation = Quaternion.LookRotation(d.normalized);
                    light.spotAngle = Mathf.Clamp((float)l.OuterAngle * 2f * Mathf.Rad2Deg, 1f, 179f);
                    light.innerSpotAngle = Mathf.Clamp((float)l.InnerAngle * 2f * Mathf.Rad2Deg, 0f, light.spotAngle);
                }
                Setup(light, new Color((float)l.Color[0], (float)l.Color[1], (float)l.Color[2]), (float)l.Intensity, (float)l.Far * unityScale);
                ++made;
            }
            if (subtractive > 0)
                warnings.Add("lights: " + subtractive + " subtractive map light(s) skipped (negative intensity: no Unity counterpart)");
            if (twins > 0) warnings.Add("lights: " + twins + " map light(s) stored twice in the map file: placed once");
            return made;
        }

        static string TwinKey(WorldManifest.MapLight l)
        {
            string V(double[] v) { return v == null ? "-" : string.Join(",", System.Array.ConvertAll(v, x => x.ToString("R"))); }
            return V(l.Unity) + "|" + V(l.Color) + "|" + l.Intensity.ToString("R") + "|" + l.Far.ToString("R") + "|" + l.Spot + "|" + V(l.UnityDirection) +
                   "|" + (l.Layers == null ? "-" : string.Join(",", l.Layers));
        }

        /// Adds @p inst's effect lights from its particle sidecar at their bones (replacing earlier
        /// ones); returns the lights made.
        public static int ModelLights(GameObject inst, string sidecar)
        {
            foreach (Light old in inst.GetComponentsInChildren<Light>(true))
                if (old.name.StartsWith(ModelLightPrefix, System.StringComparison.Ordinal)) Object.DestroyImmediate(old.gameObject);
            if (sidecar == null || !File.Exists(sidecar)) return 0;
            var doc = MiniJson.Parse(File.ReadAllText(sidecar)) as Dictionary<string, object>;
            if (doc == null || !(doc.TryGetValue("effectLights", out object lo) && lo is List<object> lights)) return 0;
            var bones = new Dictionary<string, Transform>();
            foreach (Transform t in inst.GetComponentsInChildren<Transform>(true))
                if (!bones.ContainsKey(t.name)) bones[t.name] = t;
            int made = 0;
            foreach (object o in lights)
            {
                var e = o as Dictionary<string, object>;
                if (e == null) continue;
                Transform at = inst.transform;
                if (e.TryGetValue("bone", out object b) && b is string bn && bones.TryGetValue(bn, out Transform bt)) at = bt;
                var go = new GameObject(ModelLightPrefix + made);
                go.transform.SetParent(at, false);
                var c = e.TryGetValue("color", out object co) && co is List<object> cl && cl.Count == 3 ? new Color(F(cl[0]), F(cl[1]), F(cl[2])) : Color.white;
                float far = e.TryGetValue("farDistance", out object fo) ? F(fo) : 1f;
                float intensity = e.TryGetValue("intensity", out object io) ? F(io) : 1f;
                if (!(intensity > 0)) { Object.DestroyImmediate(go); continue; }
                Setup(go.AddComponent<Light>(), c, intensity, far * inst.transform.lossyScale.x);
                ++made;
            }
            return made;
        }

        /// The name a map light gets: "Point <k>" / "Spot <k>", k counting the lights made (MapLights' order).
        public static string LightName(WorldManifest.MapLight l, int made) { return (l.Spot ? "Spot " : "Point ") + made; }

        /// The layer ids that are layers (below 1024; larger values have bit 31 set or are flags), sorted.
        public static long[] RealLayers(long[] layers)
        {
            if (layers == null) return new long[0];
            var r = new List<long>();
            foreach (long id in layers) if (id >= 0 && id < 1024 && !r.Contains(id)) r.Add(id);
            r.Sort();
            return r.ToArray();
        }

        /// 2026-10-26: a light on map layers goes under "GW2 Map Lights/Layer <a>+<b>" (made on demand), which
        /// MapLayers switches with the layer: its point light volume streams under it, never past it.
        public static Transform LayerParent(Transform group, long[] layers)
        {
            long[] ids = RealLayers(layers);
            if (ids.Length == 0) return group;
            string name = "Layer " + string.Join("+", ids);
            Transform t = group.Find(name);
            if (t != null) return t;
            var go = new GameObject(name);
            go.transform.SetParent(group, false);
            return go.transform;
        }

        static void Setup(Light light, Color c, float intensity, float range)
        {
            light.color = c;
            light.intensity = intensity;
            light.range = Mathf.Max(range, 0.01f);
            light.lightmapBakeType = LightmapBakeType.Baked;
            light.shadows = LightShadows.None;   // GW2's map and model lights cast no shadows (GW2LightingLook)
            light.renderMode = LightRenderMode.ForceVertex;
            GameObjectUtility.SetStaticEditorFlags(light.gameObject, StaticEditorFlags.ContributeGI);
        }

        static float F(object o) { return o is double d ? (float)d : o is long l ? l : 0f; }
    }
}
