// castlemist world export: a GW2 model's sounds in Unity (2026-10-25).
//
// The export writes models/<fileId>/<fileId>_sounds.json from the model's AMSP sound script
// (src/exportgltf/sound_export.cpp): sounds (files by weight, linear volume, pitch ratio, loop mode,
// the attenuation envelope) and trigger keys named after the model's clips, each with markers that
// play a sound at a time into the clip. Here:
//   * AddClipEvents gives a clip copy Animation Events: SendCustomEvent "_T<k>" at 0 (trigger k's
//     clip started) and "_S<n>" at each marker (sound n), received by Runtime/GW2SoundPlayer on the
//     Animator's object (WorldImporter.WithConstants calls it for every clip it copies);
//   * SetupInstance puts one AudioSource per sound under "GW2 Sounds" and the player on the
//     instance root; a prop without an Animator plays its idle trigger's sounds at Start.
// UNPROVEN, castlemist's stand-ins: the envelope as a custom rolloff over (inputOffset + inputScale)
// metres x DistanceScale with its points' values as the gain, ending silent, at most MaxDistance;
// sounds with no envelope roll off linearly DefaultMinDistance..DefaultMaxDistance (2026-10-25: the
// logarithmic rolloff never went silent, so every loop in Lion's Arch stayed audible and they stole
// each other's voices); a sound loops when its loopMode is not 0, at a lower priority than one-shots.
// The player switches its sources off beyond the farthest one's range (GW2SoundPlayer.cullDistance).

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public static class GW2Sounds
    {
        public const string GroupName = "GW2 Sounds";
        /// GW2 audio distance units to metres (UNPROVEN: GW2's AudioSettings distanceScale not read).
        public const float DistanceScale = 1f;
        public const float DefaultMinDistance = 3f, DefaultMaxDistance = 25f, MaxDistance = 60f;
        const int MaxSounds = 64, MaxTriggers = 16;   // GW2SoundPlayer's numbered entry points

        public sealed class Sound
        {
            public string Id, Name;
            public readonly List<string> Paths = new List<string>();
            public readonly List<float> Weights = new List<float>(), Volumes = new List<float>();
            public float VolMin = 1, VolMax = 1, PitchMin = 1, PitchMax = 1;
            public bool Loop;
            public Dictionary<string, object> Rolloff;   // null: none
        }

        public sealed class Trigger
        {
            public string Id, Name, Clip;
            public readonly List<KeyValuePair<int, float>> Markers = new List<KeyValuePair<int, float>>();   // sound index, time
        }

        public sealed class Sidecar
        {
            public float Volume = 1;
            public readonly List<Sound> Sounds = new List<Sound>();
            public readonly List<Trigger> Triggers = new List<Trigger>();
        }

        static readonly Dictionary<string, Sidecar> cache = new Dictionary<string, Sidecar>(StringComparer.Ordinal);

        /// The sidecar at asset path @p path (cached per import), or null.
        public static Sidecar Load(string path)
        {
            if (path == null || !File.Exists(path)) return null;
            Sidecar sc;
            if (cache.TryGetValue(path, out sc)) return sc;
            var d = MiniJson.Parse(File.ReadAllText(path)) as Dictionary<string, object>;
            if (d == null) return null;
            sc = new Sidecar { Volume = (float)Num(d, "volume", 1) };
            var index = new Dictionary<string, int>(StringComparer.Ordinal);
            foreach (object o in List(d, "sounds"))
            {
                var s = o as Dictionary<string, object>;
                if (s == null) continue;
                var snd = new Sound { Id = Str(s, "id"), Name = Str(s, "name") };
                foreach (object fo in List(s, "files"))
                {
                    var f = fo as Dictionary<string, object>;
                    string p = f != null ? Str(f, "path") : null;
                    if (p == null) continue;
                    snd.Paths.Add(p);
                    snd.Weights.Add((float)Num(f, "weight", 1));
                    snd.Volumes.Add((float)Num(f, "volume", 1));
                }
                Range(s, "volume", out snd.VolMin, out snd.VolMax);
                Range(s, "pitch", out snd.PitchMin, out snd.PitchMax);
                snd.Loop = Num(s, "loopMode", 0) != 0;
                snd.Rolloff = s.TryGetValue("rolloff", out object ro) ? ro as Dictionary<string, object> : null;
                if (snd.Id != null) index[snd.Id] = sc.Sounds.Count;
                sc.Sounds.Add(snd);
            }
            foreach (object o in List(d, "triggers"))
            {
                var t = o as Dictionary<string, object>;
                if (t == null) continue;
                var tr = new Trigger { Id = Str(t, "id"), Name = Str(t, "name"), Clip = Str(t, "clip") };
                foreach (object mo in List(t, "markers"))
                {
                    var m = mo as Dictionary<string, object>;
                    int si;
                    if (m != null && Str(m, "sound") is string sid && index.TryGetValue(sid, out si))
                        tr.Markers.Add(new KeyValuePair<int, float>(si, (float)Num(m, "time", 0)));
                }
                sc.Triggers.Add(tr);
            }
            cache[path] = sc;
            return sc;
        }

        /// Forgets the cached sidecars (each import / refresh reads them afresh).
        public static void ClearCache() { cache.Clear(); }

        /// The events clip @p clipName gets from @p sc: "_T<k>" at 0, then "_S<n>" at each marker.
        public static List<AnimationEvent> ClipEvents(Sidecar sc, string clipName)
        {
            var ev = new List<AnimationEvent>();
            if (sc == null) return ev;
            for (int k = 0; k < sc.Triggers.Count && k < MaxTriggers; ++k)
            {
                Trigger t = sc.Triggers[k];
                if (t.Clip != clipName) continue;
                ev.Add(new AnimationEvent { time = 0f, functionName = "SendCustomEvent", stringParameter = "_T" + k });
                foreach (KeyValuePair<int, float> m in t.Markers)
                    if (m.Key < MaxSounds)
                        ev.Add(new AnimationEvent { time = Mathf.Max(0f, m.Value), functionName = "SendCustomEvent", stringParameter = "_S" + m.Key });
            }
            return ev;
        }

        /// Sets placed instance @p inst up: AudioSources under "GW2 Sounds" and GW2SoundPlayer on the
        /// root. @p modelFolder is the model's asset folder (sidecar paths are relative to it);
        /// @p animated: its clips drive the sounds; else @p idleClip's trigger sounds play at Start.
        public static bool SetupInstance(GameObject inst, Sidecar sc, string modelFolder, bool animated, string idleClip,
                                         List<string> warnings)
        {
            Type player = SceneSetup.FindType("Castlemist.GW2SoundPlayer");
            if (player == null || sc == null || sc.Sounds.Count == 0) return false;
            Transform old = inst.transform.Find(GroupName);
            if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
            var group = new GameObject(GroupName);
            group.transform.SetParent(inst.transform, false);
            GameObjectUtility.SetStaticEditorFlags(group, 0);

            int n = Mathf.Min(sc.Sounds.Count, MaxSounds);
            if (sc.Sounds.Count > MaxSounds) warnings.Add("sounds: " + inst.name + ": " + sc.Sounds.Count + " sounds, the first " + MaxSounds + " kept");
            var sources = new AudioSource[n];
            var clips = new List<AudioClip>();
            var weight = new List<float>();
            var baseVolume = new List<float>();
            var first = new int[n];
            var count = new int[n];
            var volMin = new float[n]; var volMax = new float[n];
            var pitchMin = new float[n]; var pitchMax = new float[n];
            var loop = new bool[n];
            for (int i = 0; i < n; ++i)
            {
                Sound s = sc.Sounds[i];
                var go = new GameObject("Sound " + i + " " + s.Name);
                go.transform.SetParent(group.transform, false);
                GameObjectUtility.SetStaticEditorFlags(go, 0);
                var src = go.AddComponent<AudioSource>();
                src.playOnAwake = false;
                src.spatialBlend = 1f;
                src.dopplerLevel = 0f;
                src.priority = s.Loop ? 192 : 128;
                Rolloff(src, s.Rolloff);
                sources[i] = src;
                first[i] = clips.Count;
                for (int k = 0; k < s.Paths.Count; ++k)
                {
                    var clip = AssetDatabase.LoadAssetAtPath<AudioClip>(modelFolder + "/" + s.Paths[k]);
                    if (clip == null) { warnings.Add("sounds: " + modelFolder + "/" + s.Paths[k] + " did not import"); continue; }
                    clips.Add(clip);
                    weight.Add(Mathf.Max(s.Weights[k], 0f));
                    baseVolume.Add(Mathf.Clamp01(s.Volumes[k] * sc.Volume));
                }
                count[i] = clips.Count - first[i];
                volMin[i] = s.VolMin; volMax[i] = s.VolMax;
                pitchMin[i] = s.PitchMin; pitchMax[i] = s.PitchMax;
                loop[i] = s.Loop;
            }
            var start = new List<int>();
            if (!animated)
                foreach (Trigger t in sc.Triggers)
                    if (t.Clip == (idleClip ?? "zeropose") || (idleClip == null && t.Name == "zeropose"))
                        foreach (KeyValuePair<int, float> m in t.Markers)
                            if (m.Key < n && !start.Contains(m.Key)) start.Add(m.Key);

            Component comp = inst.GetComponent(player) ?? SceneSetup.AddUdonAware(inst, player);
            SceneSetup.SetField(comp, "sources", sources);
            SceneSetup.SetField(comp, "clips", clips.ToArray());
            SceneSetup.SetField(comp, "first", first);
            SceneSetup.SetField(comp, "count", count);
            SceneSetup.SetField(comp, "weight", weight.ToArray());
            SceneSetup.SetField(comp, "baseVolume", baseVolume.ToArray());
            SceneSetup.SetField(comp, "volMin", volMin);
            SceneSetup.SetField(comp, "volMax", volMax);
            SceneSetup.SetField(comp, "pitchMin", pitchMin);
            SceneSetup.SetField(comp, "pitchMax", pitchMax);
            SceneSetup.SetField(comp, "loop", loop);
            SceneSetup.SetField(comp, "startSounds", start.ToArray());
            float reach = 0f;
            foreach (AudioSource a in sources) reach = Mathf.Max(reach, a.maxDistance);
            SceneSetup.SetField(comp, "cullDistance", reach + 5f);
            SceneSetup.CopyProxyToUdon(comp);
            return true;
        }

        /// GW2's attenuation envelope as a custom rolloff (see the header), else linear.
        static void Rolloff(AudioSource src, Dictionary<string, object> r)
        {
            var pts = r != null ? List(r, "points") : new List<object>();
            var keys = new List<Keyframe>();
            foreach (object p in pts)
                if (p is List<object> xy && xy.Count >= 2 && xy[0] is double x && xy[1] is double y)
                    keys.Add(new Keyframe(Mathf.Clamp01((float)x), Mathf.Clamp01((float)y)));
            if (keys.Count < 2)
            {
                src.rolloffMode = AudioRolloffMode.Linear;
                src.minDistance = DefaultMinDistance;
                src.maxDistance = DefaultMaxDistance;
                return;
            }
            float max = Mathf.Clamp(((float)Num(r, "inputOffset", 0) + (float)Num(r, "inputScale", 1)) * DistanceScale, 1f, MaxDistance);
            keys = keys.OrderBy(k => k.time).ToList();
            if (keys[keys.Count - 1].time < 1f || keys[keys.Count - 1].value > 0f) keys.Add(new Keyframe(1f, 0f));   // silent at the range
            src.rolloffMode = AudioRolloffMode.Custom;
            src.maxDistance = max;
            src.SetCustomCurve(AudioSourceCurveType.CustomRolloff, new AnimationCurve(keys.ToArray()));
        }

        // a {value, type, random} param as a {min, max} factor: type 1 its random range, else its
        // value; 0 (GW2's "unset") is 1
        static void Range(Dictionary<string, object> s, string key, out float min, out float max)
        {
            min = max = 1f;
            var p = s.TryGetValue(key, out object po) ? po as Dictionary<string, object> : null;
            if (p == null) return;
            float v = (float)Num(p, "value", 0);
            var rnd = Arr(p, "random");
            if (Num(p, "type", 0) == 1 && rnd.Length >= 2 && (rnd[0] != 0 || rnd[1] != 0))
            {
                min = (float)Math.Min(rnd[0], rnd[1]);
                max = (float)Math.Max(rnd[0], rnd[1]);
            }
            else if (v != 0) min = max = v;
        }

        static string Str(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) ? v as string : null; }
        static double Num(Dictionary<string, object> d, string k, double def) { object v; return d.TryGetValue(k, out v) && v is double x ? x : def; }
        static List<object> List(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v is List<object> l ? l : new List<object>(); }
        static double[] Arr(Dictionary<string, object> d, string k)
        {
            var l = List(d, k);
            var r = new double[l.Count];
            for (int i = 0; i < l.Count; ++i) r[i] = l[i] is double x ? x : 0;
            return r;
        }
    }
}
