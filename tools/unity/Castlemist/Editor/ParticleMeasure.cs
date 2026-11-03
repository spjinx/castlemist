// Castlemist > Particles > Measure Selection: runs every GW2 FX particle system under the selection
// in edit mode (seed 1, 1/60 s steps, 4 s, as castlemist's reference run) and compares what it does
// with <fileId>_fxstats.json, the reference statistics the model export writes beside
// <fileId>_particles.json (docs/superpowers/specs/2026-10-29-unity-translation-design.md).
//
// Per emitter: live particles at the end, the median and 95th-percentile distance from the
// emitter, mean speed, alpha and half-size at mid-life; Unity / GW2 ratios, red past +-15 %.
// Distances and sizes are in the particle system's local units, which the importer keeps in the
// export's units (GW2 inches) under the model's scale.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public class ParticleMeasure : EditorWindow
    {
        const float Dt = 1f / 60f;
        const int Steps = 240;
        const float Tolerance = 0.15f;

        struct Row
        {
            public string name;
            public float[] unity, gw2;   // live, spread50, spread95, speed, alpha@.5, size@.5
        }

        static readonly string[] Columns = { "live", "spread 50%", "spread 95%", "speed", "alpha @.5", "size @.5" };
        List<Row> rows = new List<Row>();
        string message = "";
        string reference = "";
        Vector2 scroll;

        [MenuItem("Castlemist/Particles/Measure Selection")]
        static void Open()
        {
            var w = GetWindow<ParticleMeasure>("GW2 Particle Measure");
            w.Measure();
        }

        void OnGUI()
        {
            EditorGUILayout.BeginHorizontal();
            if (GUILayout.Button("Measure selection", GUILayout.Width(140))) Measure();
            if (GUILayout.Button("Load reference...", GUILayout.Width(130)))
            {
                string p = EditorUtility.OpenFilePanel("castlemist _fxstats.json", Path.GetDirectoryName(reference) ?? "", "json");
                if (!string.IsNullOrEmpty(p)) { reference = p; Measure(); }
            }
            if (GUILayout.Button("Copy report", GUILayout.Width(100))) EditorGUIUtility.systemCopyBuffer = Report();
            EditorGUILayout.EndHorizontal();
            EditorGUILayout.LabelField(reference.Length > 0 ? "Reference: " + reference : "No reference loaded");
            if (message.Length > 0) EditorGUILayout.HelpBox(message, MessageType.Info);
            scroll = EditorGUILayout.BeginScrollView(scroll);
            EditorGUILayout.BeginHorizontal();
            GUILayout.Label("emitter", EditorStyles.boldLabel, GUILayout.Width(220));
            foreach (string c in Columns) GUILayout.Label(c, EditorStyles.boldLabel, GUILayout.Width(120));
            EditorGUILayout.EndHorizontal();
            var red = new GUIStyle(EditorStyles.label) { normal = { textColor = new Color(1f, 0.35f, 0.3f) } };
            foreach (Row r in rows)
            {
                EditorGUILayout.BeginHorizontal();
                GUILayout.Label(r.name, GUILayout.Width(220));
                for (int k = 0; k < Columns.Length; ++k)
                {
                    float g = r.gw2 != null ? r.gw2[k] : float.NaN;
                    string text = Cell(r.unity[k], g, out bool off);
                    GUILayout.Label(text, off ? red : EditorStyles.label, GUILayout.Width(120));
                }
                EditorGUILayout.EndHorizontal();
            }
            EditorGUILayout.EndScrollView();
        }

        static string Cell(float u, float g, out bool off)
        {
            off = false;
            if (float.IsNaN(g)) return u.ToString("0.##");
            if (Mathf.Abs(g) < 1e-4f) { off = Mathf.Abs(u) > 1e-3f; return u.ToString("0.##") + " / 0"; }
            float ratio = u / g;
            off = Mathf.Abs(ratio - 1f) > Tolerance;
            return u.ToString("0.##") + " / " + g.ToString("0.##") + "  (" + ratio.ToString("0.00") + ")";
        }

        string Report()
        {
            var sb = new StringBuilder();
            sb.Append("emitter\t").Append(string.Join("\t", Columns)).Append('\n');
            foreach (Row r in rows)
            {
                sb.Append(r.name);
                for (int k = 0; k < Columns.Length; ++k)
                    sb.Append('\t').Append(Cell(r.unity[k], r.gw2 != null ? r.gw2[k] : float.NaN, out bool _));
                sb.Append('\n');
            }
            return sb.ToString();
        }

        /// The Measure table for @p go as text (no window): for automation and the console.
        public static string ReportFor(GameObject go, string referencePath = "")
        {
            var w = CreateInstance<ParticleMeasure>();
            try
            {
                w.reference = referencePath ?? "";
                w.Measure(new[] { go });
                return (w.message.Length > 0 ? w.message + "\n" : "") + "reference: " + w.reference + "\n" + w.Report();
            }
            finally { DestroyImmediate(w); }
        }

        void Measure() { Measure(Selection.gameObjects); }

        void Measure(GameObject[] selection)
        {
            rows.Clear();
            message = "";
            var systems = new List<ParticleSystem>();
            foreach (GameObject go in selection)
                systems.AddRange(go.GetComponentsInChildren<ParticleSystem>(true)
                                   .Where(p => p.name.StartsWith(ParticleEffects.ObjectPrefix, StringComparison.Ordinal)));
            if (systems.Count == 0) { message = "Select a model instance that has GW2 FX particle systems."; return; }
            if (reference.Length == 0) reference = GuessReference(selection[0]);

            List<object> refEmitters = null;
            List<int> cloudStart = null;
            if (reference.Length > 0 && File.Exists(reference))
            {
                var stats = MiniJson.Parse(File.ReadAllText(reference)) as Dictionary<string, object>;
                refEmitters = stats != null && stats.TryGetValue("emitters", out object eo) ? eo as List<object> : null;
                cloudStart = CloudStarts(reference.Replace("_fxstats.json", "_particles.json"));
            }
            else message = "No _fxstats.json found beside the model: re-export it from castlemist, or Load reference.";

            int idle = 0;
            foreach (ParticleSystem ps in systems)
            {
                var row = new Row { name = ps.name.Substring(ParticleEffects.ObjectPrefix.Length), unity = Simulate(ps) };
                if (row.unity[0] == 0f && row.unity[1] == 0f) { row.name += "  (not playing: on demand or never visible)"; ++idle; }
                if (refEmitters != null && cloudStart != null &&
                    ParticleEffects.ParseName(ps.name, out string _, out int ci, out int ei) && ci >= 0 && ci < cloudStart.Count)
                {
                    int index = cloudStart[ci] + ei;
                    if (index >= 0 && index < refEmitters.Count) row.gw2 = Reference(refEmitters[index] as Dictionary<string, object>);
                }
                rows.Add(row);
            }
            if (idle > 0) message += (message.Length > 0 ? " " : "") + idle + " system(s) did not play in the 4 s run.";
            if (this != null) Repaint();
        }

        /// The first stats index of each cloud: the reference lists emitters cloud by cloud, each
        /// cloud's emitters in sidecar order.
        static List<int> CloudStarts(string sidecar)
        {
            if (!File.Exists(sidecar)) return null;
            var doc = MiniJson.Parse(File.ReadAllText(sidecar)) as Dictionary<string, object>;
            if (doc == null || !doc.TryGetValue("clouds", out object co) || !(co is List<object> clouds)) return null;
            var starts = new List<int>();
            int n = 0;
            foreach (object c in clouds)
            {
                starts.Add(n);
                if (c is Dictionary<string, object> cd && cd.TryGetValue("emitters", out object eo) && eo is List<object> em) n += em.Count;
            }
            return starts;
        }

        static float[] Reference(Dictionary<string, object> e)
        {
            if (e == null) return null;
            float Num(object o) { return o is double d ? (float)d : o is long l ? l : 0f; }
            List<object> live = e.TryGetValue("live", out object lo) ? lo as List<object> : null;
            List<object> spread = e.TryGetValue("spread", out object so) ? so as List<object> : null;
            List<object> alpha = e.TryGetValue("alphaAt", out object ao) ? ao as List<object> : null;
            List<object> size = e.TryGetValue("sizeAt", out object zo) ? zo as List<object> : null;
            return new[]
            {
                live != null && live.Count > 0 ? Num(live[live.Count - 1]) : float.NaN,
                spread != null && spread.Count > 2 ? Num(spread[1]) : float.NaN,
                spread != null && spread.Count > 2 ? Num(spread[2]) : float.NaN,
                e.TryGetValue("meanSpeed", out object mo) ? Num(mo) : float.NaN,
                alpha != null && alpha.Count > 2 ? Num(alpha[2]) : float.NaN,
                size != null && size.Count > 2 ? Num(size[2]) : float.NaN,
            };
        }

        /// Runs @p ps for 4 s in edit mode with seed 1 and gathers the same numbers as fxsim.
        static float[] Simulate(ParticleSystem ps)
        {
            bool autoSeed = ps.useAutoRandomSeed;
            uint seed = ps.randomSeed;
            ps.Stop(false, ParticleSystemStopBehavior.StopEmittingAndClear);
            ps.useAutoRandomSeed = false;
            ps.randomSeed = 1;
            var dist = new List<float>();
            double speedSum = 0, alphaSum = 0, sizeSum = 0;
            long speedN = 0, midN = 0;
            var buf = new ParticleSystem.Particle[ps.main.maxParticles];
            int alive = 0;
            // A stretched system keeps its length (GW2's X half-size) on the 3D size's Y
            // (ParticleEffects.StartSize); the speed is measured from each particle's movement, which
            // includes the velocity module's speed modifier (the drag decay) that totalVelocity leaves out.
            var renderer = ps.GetComponent<ParticleSystemRenderer>();
            bool stretched = renderer != null && renderer.renderMode == ParticleSystemRenderMode.Stretch;
            var last = new Dictionary<uint, Vector3>();
            var seen = new Dictionary<uint, Vector3>();
            for (int i = 0; i < Steps; ++i)
            {
                ps.Simulate(Dt, false, i == 0, true);
                alive = ps.GetParticles(buf);
                seen.Clear();
                for (int k = 0; k < alive; ++k)
                {
                    ParticleSystem.Particle p = buf[k];
                    dist.Add(p.position.magnitude);
                    if (last.TryGetValue(p.randomSeed, out Vector3 before))
                    {
                        speedSum += (p.position - before).magnitude / Dt;
                        ++speedN;
                    }
                    seen[p.randomSeed] = p.position;
                    float t = p.startLifetime > 0f ? 1f - p.remainingLifetime / p.startLifetime : 0f;
                    if (Mathf.Abs(t - 0.5f) <= 0.125f)
                    {
                        alphaSum += p.GetCurrentColor(ps).a / 255f;
                        sizeSum += (stretched ? p.GetCurrentSize3D(ps).y : p.GetCurrentSize(ps)) * 0.5f;   // GW2 sizes are half-sizes
                        ++midN;
                    }
                }
                var swap = last; last = seen; seen = swap;
            }
            ps.Stop(false, ParticleSystemStopBehavior.StopEmittingAndClear);
            ps.useAutoRandomSeed = autoSeed;
            ps.randomSeed = seed;
            dist.Sort();
            float Pct(float q) { return dist.Count == 0 ? 0f : dist[(int)(q * (dist.Count - 1))]; }
            return new[]
            {
                alive, Pct(0.5f), Pct(0.95f), speedN > 0 ? (float)(speedSum / speedN) : 0f,
                midN > 0 ? (float)(alphaSum / midN) : 0f, midN > 0 ? (float)(sizeSum / midN) : 0f,
            };
        }

        /// <fileId>_fxstats.json beside the selected model's prefab or asset, if there is one.
        static string GuessReference(GameObject go)
        {
            UnityEngine.Object src = PrefabUtility.GetCorrespondingObjectFromOriginalSource(go);
            string path = AssetDatabase.GetAssetPath(src != null ? src : go);
            if (string.IsNullOrEmpty(path)) return "";
            string dir = Path.GetDirectoryName(path);
            for (int up = 0; up < 3 && !string.IsNullOrEmpty(dir); ++up, dir = Path.GetDirectoryName(dir))
            {
                string hit = Directory.GetFiles(dir, "*_fxstats.json").FirstOrDefault();
                if (hit != null) return hit.Replace('\\', '/');
            }
            return "";
        }
    }
}
