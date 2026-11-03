// castlemist world export: GW2's day / night cycle (2026-10-25). Fills one Runtime/GW2DayNight on
// "GW2 Day Night" under the map root. Day = what the scene has now (the importer's day: the Sun, the
// Trilight, every sky material); night = the export's environment.night (lighting and haze slot 1)
// and the night sets the clouds (attributes[1]) and sky cards ("night") already carry:
//   sun        colour / direction of night.lights[0]; intensity x the scene's day calibration
//              (scene sun intensity / day lights[0].intensity)
//   ambient    EnvironmentMath.TrilightFromFills(night.lights), as SceneSetup does for the day
//   sky        Materials/SkyBlend.mat (Castlemist/GW2 Sky Blend: sky/day and sky/night
//              baked/equirect.png) becomes RenderSettings.skybox, the Skybox.mat exposure kept
//   clouds     _Attr from attributes[1] (lightIntensity x skyLightIntensity[1]), _SunColor /
//              _Ambient, _FogFar from the night haze
//   cards      _Card, _Attr, _Haze from the night set (skyHaze [1]); _NightTex = the night texture
//   haze       the GW2 Haze volume's _HazeNear / _HazeFar / _HazeHeight from the night haze
// Colours go in as linear vectors (what Material.SetColor stores in a linear project). Without
// environment.night (an older export) nothing is made (one warning). Rerunning replaces it.
// 2026-10-27: a night that is the day (Mad King's Labyrinth: night.lights repeat the day's warm sun at
// 1.3) gets castlemist's moonlight instead: a cool white-blue moon (MoonColor) as strong as the day's
// sun, MoonElevation above the horizon on the sun's bearing (long shadows, the game's night shots), and
// the day's Trilight cooled and dimmed (NightAmbient). 2026-10-28: when the sky has a moon card
// (MoonCard) the moonlight comes from it instead, in its texture's colour. Maps in NightByDefault start held at night
// (cycleOn off, fixedWeight 0): the GMMenu's time-of-day slider shows it and moves it.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class DayNight
    {
        public const string Name = "GW2 Day Night";
        /// Maps that start at night (the cycle held there, the GMMenu slider at 0).
        public static readonly HashSet<long> NightByDefault = new HashSet<long> { 2499169 };   // Mad King's Labyrinth
        static readonly Color MoonColor = new Color(0.70f, 0.80f, 1.0f);
        public const float MoonElevation = 22f;   // degrees
        static readonly Color NightAmbient = new Color(0.55f, 0.65f, 0.85f);   // x the day Trilight

        /// 2026-10-28: the moon of a map whose sky shows one (Mad King's Labyrinth's Mad King moon, card 504870):
        /// the first sky card that does not turn (speed 0) and sits above the horizon. @p toMoon is the
        /// world direction to it (the card shader's c, map axes -> the map root's frame), @p color its
        /// texture's alpha-weighted mean, brightest channel 1. The moonlight then comes from the moon
        /// in the sky, and a sun flare (Silent's Sunshine Flare reads the main light) sits on it.
        public static bool MoonCard(GameObject mapRoot, out Vector3 toMoon, out Color color)
        {
            toMoon = Vector3.zero; color = MoonColor;
            Transform group = mapRoot.transform.Find(GW2SkyCards.GroupName);
            if (group == null) return false;
            foreach (Renderer r in group.GetComponentsInChildren<Renderer>(true))
            {
                Material m = r.sharedMaterial;
                if (m == null || !m.HasProperty("_Card")) continue;
                Vector4 card = m.GetVector("_Card");
                if (Mathf.Abs(card.z) > 1e-6f || card.y <= 0f) continue;
                float ca = Mathf.Cos(card.x), sa = Mathf.Sin(card.x), cl = Mathf.Cos(card.y), sl = Mathf.Sin(card.y);
                var c = new Vector3(cl * ca, cl * sa, -sl);
                toMoon = group.TransformDirection(new Vector3(c.x, -c.z, c.y)).normalized;   // MapToLocal
                Texture tex = m.GetTexture("_MainTex");
                if (tex != null)
                {
                    Color a = ImmersiveLighting.AverageColour(tex);
                    float peak = Mathf.Max(a.r, Mathf.Max(a.g, a.b));
                    if (peak > 1e-3f) color = new Color(a.r / peak, a.g / peak, a.b / peak, 1f);
                }
                return true;
            }
            return false;
        }

        /// The UdonBehaviours behind the scene's Light Volumes (GW2DayNight tints them), empty without the package.
        public static Component[] LightVolumeBehaviours()
        {
            Type lv = SceneSetup.FindType("VRCLightVolumes.LightVolumeInstance"), ub = SceneSetup.FindType("VRC.Udon.UdonBehaviour");
            if (lv == null || ub == null) return Array.CreateInstance(ub ?? typeof(Component), 0) as Component[];
            var list = new List<Component>();
            foreach (Component v in Object.FindObjectsByType(lv, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                Component b = v.GetComponent(ub);
                if (b != null) list.Add(b);
            }
            Array a = Array.CreateInstance(ub, list.Count);
            for (int i = 0; i < list.Count; ++i) a.SetValue(list[i], i);
            return (Component[])a;
        }

        /// GW2DayNight's lightVolumes refilled from the scene (after the volumes change).
        public static string LinkLightVolumes()
        {
            Type dn = SceneSetup.FindType("Castlemist.GW2DayNight");
            if (dn == null) return "day/night: no GW2DayNight";
            int n = 0;
            foreach (Component c in Object.FindObjectsByType(dn, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                Component[] vs = LightVolumeBehaviours();
                SceneSetup.SetField(c, "lightVolumes", vs);
                SceneSetup.CopyProxyToUdon(c);
                n = vs.Length;
            }
            UnityEditor.SceneManagement.EditorSceneManager.MarkSceneDirty(UnityEditor.SceneManagement.EditorSceneManager.GetActiveScene());
            return "day/night: " + n + " Light Volume(s) tinted by the cycle";
        }

        public static bool Setup(GameObject mapRoot, string root, long mapFileId, float unityScale, List<string> warnings)
        {
            Transform old = mapRoot.transform.Find(Name);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            Type type = SceneSetup.FindType("Castlemist.GW2DayNight");
            if (type == null) { warnings.Add("day/night: Castlemist.GW2DayNight is not compiled yet: refresh again once UdonSharp has compiled"); return false; }
            var doc = MiniJson.Parse(File.ReadAllText(root + "/manifest.json")) as Dictionary<string, object>;
            var env = doc != null && doc.TryGetValue("environment", out object eo) ? eo as Dictionary<string, object> : null;
            var night = env != null && env.TryGetValue("night", out object no) ? no as Dictionary<string, object> : null;
            if (night == null) { warnings.Add("day/night: no environment.night in the export (re-export): no cycle"); return false; }

            var vecMats = new List<Material>();
            var vecNames = new List<string>();
            var vecDay = new List<Vector4>();
            var vecNight = new List<Vector4>();
            var weightMats = new List<Material>();
            void Vec(Material m, string name, Vector4 nightValue)
            {
                if (m == null || !m.HasProperty(name)) return;
                vecMats.Add(m); vecNames.Add(name); vecDay.Add(m.GetVector(name)); vecNight.Add(nightValue);
            }
            Vector4 Lin(double[] c) => c != null && c.Length >= 3
                ? (Vector4)new Color((float)c[0], (float)c[1], (float)c[2], c.Length >= 4 ? (float)c[3] : 1f).linear : Vector4.zero;

            // Night lights and haze.
            var nightLights = new List<WorldManifest.EnvLight>();
            if (night.TryGetValue("lights", out object nlo) && nlo is List<object> nl)
                foreach (object o in nl)
                    if (o is Dictionary<string, object> l)
                        nightLights.Add(new WorldManifest.EnvLight
                        {
                            Color = GW2Clouds.Arr(l, "color"), Intensity = GW2Clouds.Num(l, "intensity", 0),
                            UnityForward = GW2Clouds.Arr(l, "unityForward"),
                        });
            var nh = night.TryGetValue("haze", out object nho) ? nho as Dictionary<string, object> : null;
            double[] nNear = nh != null ? GW2Clouds.Arr(nh, "near") : null, nFar = nh != null ? GW2Clouds.Arr(nh, "far") : null,
                     nHeight = nh != null ? GW2Clouds.Arr(nh, "height") : null;
            Color nSun = Color.black;
            if (nightLights.Count > 0 && nightLights[0].Color.Length >= 3)
            {
                var c = nightLights[0].Color;
                float k = (float)nightLights[0].Intensity;
                nSun = new Color((float)c[0] * k, (float)c[1] * k, (float)c[2] * k, 1f);
            }
            var li = env.ContainsKey("skyLightIntensity") ? GW2Clouds.Arr(env, "skyLightIntensity") : new double[0];
            float skyLINight = li.Length > 1 ? (float)li[1] : li.Length > 0 ? (float)li[0] : 1f;

            var go = new GameObject(Name);
            go.transform.SetParent(mapRoot.transform, false);
            var comp = SceneSetup.AddUdonAware(go, type);

            // Sun.
            Transform sunT = mapRoot.transform.Find("Sun");
            Light sun = sunT != null ? sunT.GetComponent<Light>() : null;
            if (sun != null && nightLights.Count > 0)
            {
                var dayLights = env.TryGetValue("lights", out object dlo) ? dlo as List<object> : null;
                double dayI = dayLights != null && dayLights.Count > 0 && dayLights[0] is Dictionary<string, object> d0 ? GW2Clouds.Num(d0, "intensity", 1) : 1;
                float calib = dayI > 1e-4 ? sun.intensity / (float)dayI : 1f;
                var nc = nightLights[0].Color;
                var nf = nightLights[0].UnityForward;
                SceneSetup.SetField(comp, "sun", sun);
                Transform av = sunT.Find(ImmersiveLighting.AvatarSunName);
                if (av != null) SceneSetup.SetField(comp, "avatarSun", av.GetComponent<Light>());
                SceneSetup.SetField(comp, "sunDayColor", sun.color);
                SceneSetup.SetField(comp, "sunDayIntensity", sun.intensity);
                SceneSetup.SetField(comp, "sunDayRotation", sun.transform.rotation);
                SceneSetup.SetField(comp, "sunNightColor", nc.Length >= 3 ? new Color((float)nc[0], (float)nc[1], (float)nc[2]) : Color.white);
                SceneSetup.SetField(comp, "sunNightIntensity", (float)nightLights[0].Intensity * calib);
                var fw = nf.Length >= 3 ? new Vector3((float)nf[0], (float)nf[1], (float)nf[2]) : Vector3.down;
                SceneSetup.SetField(comp, "sunNightRotation", fw.sqrMagnitude > 1e-8f ? Quaternion.LookRotation(fw.normalized) : sun.transform.rotation);
            }
            else warnings.Add("day/night: no Sun light or no night light: the sun is not cycled");

            // Ambient.
            double[][] tri = EnvironmentMath.TrilightFromFills(nightLights);
            bool trilight = tri != null && RenderSettings.ambientMode == UnityEngine.Rendering.AmbientMode.Trilight;
            SceneSetup.SetField(comp, "trilight", trilight);
            if (trilight)
            {
                Func<double[], Color> C = v => new Color((float)v[0], (float)v[1], (float)v[2]);
                SceneSetup.SetField(comp, "skyDay", RenderSettings.ambientSkyColor);
                SceneSetup.SetField(comp, "equatorDay", RenderSettings.ambientEquatorColor);
                SceneSetup.SetField(comp, "groundDay", RenderSettings.ambientGroundColor);
                SceneSetup.SetField(comp, "skyNight", C(tri[0]));
                SceneSetup.SetField(comp, "equatorNight", C(tri[1]));
                SceneSetup.SetField(comp, "groundNight", C(tri[2]));
            }

            // 2026-10-27: a night that repeats the day gets the moonlight (see the header).
            var dayL = env.TryGetValue("lights", out object dl2) ? dl2 as List<object> : null;
            bool sameAsDay = nightLights.Count > 0 && dayL != null && dayL.Count > 0 && dayL[0] is Dictionary<string, object> dd &&
                             Math.Abs(GW2Clouds.Num(dd, "intensity", 1) - nightLights[0].Intensity) < 1e-4 &&
                             GW2Clouds.Arr(dd, "color").Length >= 3 && nightLights[0].Color.Length >= 3 &&
                             System.Linq.Enumerable.All(System.Linq.Enumerable.Range(0, 3), i => Math.Abs(GW2Clouds.Arr(dd, "color")[i] - nightLights[0].Color[i]) < 1e-4);
            if (sameAsDay && sun != null)
            {
                Vector3 f = sun.transform.forward;
                Vector3 flat = new Vector3(f.x, 0f, f.z);
                if (flat.sqrMagnitude < 1e-6f) flat = Vector3.forward;
                Quaternion moon = Quaternion.LookRotation(Quaternion.AngleAxis(MoonElevation, Vector3.Cross(Vector3.up, flat.normalized)) * flat.normalized);
                Color moonColor = MoonColor;
                if (MoonCard(mapRoot, out Vector3 toMoon, out Color cardColor)) { moon = Quaternion.LookRotation(-toMoon); moonColor = cardColor; }
                SceneSetup.SetField(comp, "sunNightColor", moonColor);
                SceneSetup.SetField(comp, "sunNightIntensity", sun.intensity);
                SceneSetup.SetField(comp, "sunNightRotation", moon);
                if (trilight)
                {
                    Color Cool(Color c) => new Color(c.r * NightAmbient.r, c.g * NightAmbient.g, c.b * NightAmbient.b, 1f);
                    SceneSetup.SetField(comp, "skyNight", Cool(RenderSettings.ambientSkyColor));
                    SceneSetup.SetField(comp, "equatorNight", Cool(RenderSettings.ambientEquatorColor));
                    SceneSetup.SetField(comp, "groundNight", Cool(RenderSettings.ambientGroundColor));
                }
                warnings.Add("day/night: the export's night repeats the day: castlemist's moonlight for the night");
            }
            if (NightByDefault.Contains(mapFileId))
            {
                SceneSetup.SetField(comp, "cycleOn", false);
                SceneSetup.SetField(comp, "fixedWeight", 0f);
            }

            // Sky.
            Shader blend = Shader.Find("Castlemist/GW2 Sky Blend");
            var dayTex = AssetDatabase.LoadAssetAtPath<Texture2D>(root + "/sky/day/baked/equirect.png");
            var nightTex = AssetDatabase.LoadAssetAtPath<Texture2D>(root + "/sky/night/baked/equirect.png");
            if (blend != null && dayTex != null && nightTex != null)
            {
                string path = root + "/Materials/SkyBlend.mat";
                var sky = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (sky == null) { sky = new Material(blend) { name = "SkyBlend" }; AssetDatabase.CreateAsset(sky, path); }
                sky.shader = blend;
                sky.SetTexture("_DayTex", dayTex);
                sky.SetTexture("_NightTex", nightTex);
                var pano = AssetDatabase.LoadAssetAtPath<Material>(root + "/Materials/Skybox.mat");
                if (pano != null && pano.HasProperty("_Exposure")) sky.SetFloat("_Exposure", pano.GetFloat("_Exposure"));
                if (pano != null && pano.HasProperty("_Rotation")) sky.SetFloat("_Rotation", pano.GetFloat("_Rotation"));
                EditorUtility.SetDirty(sky);
                RenderSettings.skybox = sky;
                weightMats.Add(sky);
            }
            else warnings.Add("day/night: no sky/day or sky/night bake (or the GW2 Sky Blend shader): the sky stays day");

            // Clouds.
            var layers = env.TryGetValue("clouds", out object co) ? co as List<object> : null;
            for (int i = 0; layers != null && i < layers.Count; ++i)
            {
                var mat = AssetDatabase.LoadAssetAtPath<Material>(root + "/Materials/Clouds/Layer " + i + ".mat");
                var L = layers[i] as Dictionary<string, object>;
                var attrs = L != null && L.TryGetValue("attributes", out object ao) ? ao as List<object> : null;
                var A = attrs != null && attrs.Count > 1 ? attrs[1] as Dictionary<string, object> : null;
                if (mat == null || A == null) continue;
                Vec(mat, "_Attr", new Vector4((float)GW2Clouds.Num(A, "brightness", 1), (float)GW2Clouds.Num(A, "density", 1),
                                              (float)GW2Clouds.Num(A, "haze", 0), (float)GW2Clouds.Num(A, "lightIntensity", 0) * skyLINight));
                Vec(mat, "_SunColor", (Vector4)nSun.linear);
                Vec(mat, "_Ambient", (Vector4)nSun.linear);
                if (nFar != null && nFar.Length >= 4) Vec(mat, "_FogFar", Lin(nFar));
            }

            // Cards.
            var cards = env.TryGetValue("skyCards", out object ko) ? ko as List<object> : null;
            var skyHaze = env.TryGetValue("skyHaze", out object sho) ? sho as Dictionary<string, object> : null;
            double At(string k, int i, double def) { var a = skyHaze != null ? GW2Clouds.Arr(skyHaze, k) : new double[0]; return a.Length > i ? a[i] : a.Length > 0 ? a[0] : def; }
            for (int i = 0; cards != null && i < cards.Count; ++i)
            {
                var mat = AssetDatabase.LoadAssetAtPath<Material>(root + "/Materials/SkyCards/Card " + i + ".mat");
                var K = cards[i] as Dictionary<string, object>;
                var N = K != null && K.TryGetValue("night", out object nso) ? nso as Dictionary<string, object> : null;
                if (mat == null || N == null) continue;
                Vec(mat, "_Card", new Vector4((float)GW2Clouds.Num(N, "azimuth", 0), (float)GW2Clouds.Num(N, "latitude", 0) * Mathf.PI / 2,
                                              (float)GW2Clouds.Num(N, "speed", 0), 0));
                Vec(mat, "_Attr", new Vector4((float)GW2Clouds.Num(N, "brightness", 1), (float)GW2Clouds.Num(N, "density", 1),
                                              (float)GW2Clouds.Num(N, "minHaze", 0), (float)GW2Clouds.Num(N, "lightIntensity", 0) * skyLINight));
                Vec(mat, "_Haze", new Vector4((float)At("bottom", 1, 0), (float)At("falloff", 1, 1),
                                              (float)GW2Clouds.Num(N, "hazeDensity", 0) * (float)At("density", 1, 0), 0));
                Vec(mat, "_SunColor", (Vector4)nSun.linear);
                Vec(mat, "_Ambient", (Vector4)nSun.linear);
                if (nFar != null && nFar.Length >= 4) Vec(mat, "_FogFar", Lin(nFar));
                string nt = GW2Clouds.Str(N, "texture");
                Texture2D ntex = nt != null ? GW2Clouds.RawTexture(root + "/" + nt, TextureWrapMode.Clamp) : null;
                mat.SetTexture("_NightTex", ntex != null ? ntex : mat.GetTexture("_MainTex"));
                EditorUtility.SetDirty(mat);
                weightMats.Add(mat);
            }

            // Haze volume.
            var haze = AssetDatabase.LoadAssetAtPath<Material>(root + "/GW2_" + mapFileId + "_Haze.mat");
            // ... and the water's copy of it (GW2 Water fogs itself with the same values).
            // the self-fogging materials (water, refracting glass) get the haze values first
            var fogged = HazeCopy.Apply(mapRoot, haze);
            fogged.Insert(0, haze);
            foreach (Material hm in fogged)
            {
                if (hm == null || nh == null) continue;
                if (nNear != null && nNear.Length >= 4) Vec(hm, "_HazeNear", Lin(nNear));
                if (nFar != null && nFar.Length >= 4) Vec(hm, "_HazeFar", Lin(nFar));
                if (nHeight != null && nHeight.Length >= 4) Vec(hm, "_HazeHeight", Lin(nHeight));
            }

            SceneSetup.SetField(comp, "vecMats", vecMats.ToArray());
            SceneSetup.SetField(comp, "vecNames", vecNames.ToArray());
            SceneSetup.SetField(comp, "vecDay", vecDay.ToArray());
            SceneSetup.SetField(comp, "vecNight", vecNight.ToArray());
            SceneSetup.SetField(comp, "weightMats", weightMats.ToArray());
            SceneSetup.SetField(comp, "lightVolumes", LightVolumeBehaviours());
            SceneSetup.CopyProxyToUdon(comp);
            warnings.Add("day/night: GW2 Day Night cycles " + vecMats.Count + " material value(s), " + weightMats.Count +
                         " blend(s); GW2's 2-hour cycle from UTC (anchor UNPROVEN); ToggleCycle / CycleOn / CycleOff for a GMMenu button");
            return true;
        }
    }
}
