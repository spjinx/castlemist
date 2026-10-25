// castlemist world export: GW2 particle clouds as Unity particle systems (2026-10-22).
//
// A model's <fileId>_particles.json (src/exportgltf/particle_export.cpp) lists its particle clouds
// and emitters; AddTo builds one Shuriken ParticleSystem per emitter into the model (its prefab), at
// the emitter's (or cloud's) bone, and Fit sets each placed instance up for the clip it plays.
// The emitter semantics are the client's, read in Gw2-64.exe (GrCloud.cpp; docs/research/
// gw2-particle-system.md, amendment 2026-10-22):
//   * every float2 is {min, max}; the client turns it into {min, max - min} at load and samples
//     min + rand * span
//   * spawnProbability is a percent; rotations are degrees
//   * spawnShape (file value): 0 disc (radius min..max), 1 line, 2 / 6 mesh surface, 3 flat
//     rectangle (half-sizes max x min), 4 sphere (radius min..max), 5 cylinder (ring at min, up to
//     max high)
//   * velocity / acceleration rows are (dirX, dirY, dirZ, magnitude) in model space (GW2 Z down);
//     a particle gets normalize(direction sample) * magnitude sample, none when every direction
//     range is 0
//   * sizes are half-sizes: scaleInitial[0] the X, [1] the Y half-size, Y = X when flags bit 0x10;
//     scaleChange adds to them per second
//   * flags bit 0x4: the particles ignore the emitter transform's scale (their sizes are absolute
//     world units); without it every size is multiplied by it (Fit divides it out per instance)
//   * flags bit 0x1000: the spin takes a random sign
//   * emitterFlags bit 0x8: a burst, one group each time the emitter's bone turns visible;
//     bits 0x4 / 0x2: the rate scales with the wind (spawnWindEmit over spawnWindSpeed): taken at the
//     middle of spawnWindEmit, Unity having no GW2 wind
//   * an emitter spawns only while its bone is visible: the sidecar's clips[].vis (visible at the
//     start of a clip, each key toggles: "on" clips carry no keys and "off" clips one key at 0;
//     castlemist's reading of the keys)
// Not transcribed (UNPROVEN, castlemist's stand-ins): the opacity / scale curve presets (the
// viewer's envelope: fade in over 15%, out from 60%), colorPeriod / colorFalloff (begin -> end over
// the life), alignmentType 1 as a streak along the velocity (length scaleInitial[0], width [1]) and
// every other alignment as camera-facing, wind and turbulence, the distance-tier flags.
// Distortion emitters (heat haze, refraction: a normal stored as their colour) are skipped.
// Materials: the cloud's materials.json entry on Castlemist/GW2 Particle, one .mat per atlas
// rect (texCoordRect as the material's tiling / offset; a flipbook animates inside it).

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public static class ParticleEffects
    {
        public const string ObjectPrefix = "GW2 FX ";
        /// Marks an emitter whose particle sizes ignore the transform's scale (flags bit 0x4).
        const string AbsoluteTag = " abs";
        const int MaxAlive = 2000;

        /// The <fileId>_particles.json beside a model's materials.json, or null.
        public static string SidecarFor(string modelFolder, long fileId)
        {
            string p = modelFolder + "/" + fileId + "_particles.json";
            return File.Exists(p) ? p : null;
        }

        /// Builds every emitter of @p sidecar under @p instance (replacing an earlier build); returns
        /// the particle systems made. @p matFolder holds the particle .mat files. Each system runs as
        /// if its bone were always visible until Fit times it to a clip.
        public static int AddTo(GameObject instance, string sidecar, MaterialsFile mats, string modelFolder,
                                MaterialContext ctx, string matFolder, List<string> warnings)
        {
            if (ctx.ParticleShader == null)
            {
                warnings.Add(Path.GetFileName(modelFolder) + ": particles need the Castlemist shaders (Castlemist/GW2 Particle): not built");
                return 0;
            }
            var doc = MiniJson.Parse(File.ReadAllText(sidecar)) as Dictionary<string, object>;
            if (doc == null) return 0;
            foreach (ParticleSystem old in instance.GetComponentsInChildren<ParticleSystem>(true))
                if (old != null && old.name.StartsWith(ObjectPrefix, StringComparison.Ordinal))
                    UnityEngine.Object.DestroyImmediate(old.gameObject);

            string tag = Path.GetFileName(modelFolder);
            int built = 0, distortion = 0, ci = -1;
            foreach (object co in List(doc, "clouds"))
            {
                ++ci;
                var cloud = co as Dictionary<string, object>;
                if (cloud == null) continue;
                int matIndex = (int)Num(cloud, "materialIndex", -1);
                MaterialsFile.MaterialEntry me = matIndex >= 0 && matIndex < mats.Materials.Count ? mats.Materials[matIndex] : null;
                if (me == null) { warnings.Add(tag + ": cloud " + ci + " has no materials.json entry: skipped"); continue; }
                var cl = new CloudInfo
                {
                    Velocity = Conv(Vec(cloud, "velocity")),
                    Acceleration = Conv(Vec(cloud, "acceleration")),
                    Drag = (float)Num(cloud, "drag", 0),
                };
                string cloudBone = Str(cloud, "bone");
                int ei = -1;
                foreach (object eo in List(cloud, "emitters"))
                {
                    ++ei;
                    var e = eo as Dictionary<string, object>;
                    if (e == null) continue;
                    if (Bool(e, "isDistortion")) { ++distortion; continue; }
                    if (Num(e, "spawnPeriodSec", 0) <= 1e-5) continue;
                    string bone = Str(e, "bone") ?? cloudBone;
                    Transform at = bone != null ? FindDeep(instance.transform, bone) : null;
                    if (bone != null && at == null) warnings.Add(tag + ": bone '" + bone + "' not in the model: cloud " + ci + " at its origin");
                    Material mat = MaterialFor(ctx, me, modelFolder, matFolder, Rect4(e));
                    string name = ObjectPrefix + (bone ?? "cloud") + " " + ci + "." + ei + (Flag(e, "flags", 0x4) ? AbsoluteTag : "");
                    Build(instance.transform, at ?? instance.transform, e, mat, cl, name);
                    ++built;
                }
            }
            if (distortion > 0) warnings.Add(tag + ": " + distortion + " distortion emitter(s) skipped (heat haze / refraction)");
            return built;
        }

        /// Sets the particle systems of one placed instance up for its clip (null: every bone
        /// visible; zeropose, an edit pose, hides most effect bones) and its scale: an emitter spawns
        /// while its bone is visible in the clip, a burst fires at each hidden -> visible change, both repeating with
        /// the clip; sizes that ignore the transform's scale are divided by it. @p mapScale is the
        /// import's unityScale (metres per GW2 unit). @p onDemand (2026-10-25, GW2Demolishable): one
        /// pass of the clip's timing that does not loop or play on awake, for a script to Play().
        /// Returns the systems set.
        public static int Fit(GameObject instance, string sidecar, string clip, float mapScale, bool onDemand = false)
        {
            var systems = new List<ParticleSystem>();
            foreach (ParticleSystem ps in instance.GetComponentsInChildren<ParticleSystem>(true))
                if (ps.name.StartsWith(ObjectPrefix, StringComparison.Ordinal)) systems.Add(ps);
            if (systems.Count == 0 || sidecar == null) return 0;
            var doc = MiniJson.Parse(File.ReadAllText(sidecar)) as Dictionary<string, object>;
            if (doc == null) return 0;
            // "a+b": the clips played one after another (an Animator cycling them); each starts with
            // every bone visible again
            Dictionary<string, object> vis = null;
            float duration = 0f;
            foreach (string part in clip != null ? clip.Split('+') : new string[0])
            {
                Dictionary<string, object> clipEntry = null;
                foreach (object o in List(doc, "clips"))
                    if (o is Dictionary<string, object> c && Str(c, "name") == part) clipEntry = c;
                if (clipEntry == null) continue;
                vis = AppendVis(vis, Obj(clipEntry, "vis"), duration);
                duration += (float)Num(clipEntry, "duration", 0);
            }
            bool loops = clip != null && duration > 0.05f;   // an instance without a clip does not animate

            var clouds = List(doc, "clouds");
            foreach (ParticleSystem ps in systems)
            {
                int ci, ei;
                string bone;
                if (!ParseName(ps.name, out bone, out ci, out ei) || ci >= clouds.Count) continue;
                var cloud = clouds[ci] as Dictionary<string, object>;
                var emitters = cloud != null ? List(cloud, "emitters") : null;
                var e = emitters != null && ei < emitters.Count ? emitters[ei] as Dictionary<string, object> : null;
                if (e == null) continue;
                // world size = startSize x lossyScale: an "abs" emitter keeps the GW2 size in metres
                float k = 1f;
                if (ps.name.EndsWith(AbsoluteTag, StringComparison.Ordinal))
                {
                    float s = ps.transform.lossyScale.x / Mathf.Max(mapScale, 1e-6f);
                    k = s > 1e-6f ? 1f / s : 1f;
                }
                Vector2 sx, sy;
                StartSize(ps, e, k, out sx, out sy);
                // offset (0x140ad11b0): the sprite moved along the camera ray by offset world units
                // (< 0: toward the camera); the renderer's pivot z is in particle sizes, + toward it
                float offset = (float)Num(e, "offset", 0);
                var rend = ps.GetComponent<ParticleSystemRenderer>();
                if (rend != null)
                {
                    bool streak = (int)Num(e, "alignmentType", 0) == 1;
                    Vector2 w = streak || Flag(e, "flags", 0x10) ? (streak ? sy : sx) : (sx + sy) * 0.5f;
                    float size = (w.x + w.y) * 0.5f * k * ps.transform.lossyScale.x;
                    rend.pivot = Mathf.Abs(offset) > 1e-6f && size > 1e-6f ? new Vector3(0f, 0f, -offset * mapScale / size) : Vector3.zero;
                }
                Timing(ps, e, vis != null && bone != null && vis.ContainsKey(bone) ? List(vis, bone) : null, duration, loops);
                if (onDemand)
                {
                    var main = ps.main;
                    main.loop = false;
                    main.prewarm = false;
                    main.playOnAwake = false;
                    if (loops) main.duration = duration;
                    ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
                }
            }
            return systems.Count;
        }

        /// The visibility keys of @p next (starting at @p offset) after those of @p acc: a bone left
        /// hidden by the earlier clips toggles back on at @p offset, as each clip starts visible.
        static Dictionary<string, object> AppendVis(Dictionary<string, object> acc, Dictionary<string, object> next, float offset)
        {
            if (acc == null)
                return next ?? new Dictionary<string, object>();
            var bones = new HashSet<string>(acc.Keys);
            if (next != null) bones.UnionWith(next.Keys);
            var r = new Dictionary<string, object>();
            foreach (string b in bones)
            {
                var keys = new List<object>(acc.ContainsKey(b) ? List(acc, b) : new List<object>());
                if (keys.Count % 2 == 1) keys.Add((double)offset);
                if (next != null && next.ContainsKey(b))
                    foreach (object k in List(next, b)) keys.Add((k is double d ? d : 0.0) + offset);
                r[b] = keys;
            }
            return r;
        }

        sealed class CloudInfo
        {
            public Vector3 Velocity, Acceleration;
            public float Drag;
        }

        static void Build(Transform model, Transform bone, Dictionary<string, object> e, Material mat, CloudInfo cl, string name)
        {
            var go = new GameObject(name);
            // The emitter sits at the bone's position, in the model's axes (GW2 moves particles in
            // model space), at the model's scale times the bone's (flags 0x4: the model's alone):
            // sizes and speeds are model units.
            Vector3 local = model.InverseTransformPoint(bone.position);
            Quaternion rot = Quaternion.identity;
            if (Has(e, "transform"))
            {
                double[] t = Arr(e, "transform");
                local += Conv(new Vector3((float)t[3], (float)t[7], (float)t[11]));
                rot = ConvRot(t);
            }
            go.transform.SetParent(model, false);
            go.transform.localPosition = local;
            go.transform.localRotation = rot;
            go.transform.localScale = Vector3.one;
            go.transform.SetParent(bone, true);
            // Without file flags 0x4 GW2 multiplies every size (and the spawn shape and speeds) by the
            // emitter transform's scale, the bone's included (2026-10-25: 84108's FallingDust bone is
            // scaled x7.91 and its dust was 1/8 size): keep the bone's scale instead of cancelling it.
            if (!Flag(e, "flags", 0x4)) go.transform.localScale = Vector3.one;
            GameObjectUtility.SetStaticEditorFlags(go, 0);

            var ps = go.AddComponent<ParticleSystem>();
            ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);
            var main = ps.main;
            Vector2 life = Range(e, "lifetimeSec", 1);
            life.x = Mathf.Max(life.x, 0.05f); life.y = Mathf.Max(life.y, life.x);
            main.duration = Mathf.Max(life.y, 1f);
            main.loop = true;
            main.prewarm = true;
            main.playOnAwake = true;
            // Local: the placement's scale applies to sizes and speeds (GW2 model units); in World
            // space Unity leaves particle sizes unscaled.
            main.simulationSpace = ParticleSystemSimulationSpace.Local;
            main.scalingMode = ParticleSystemScalingMode.Hierarchy;
            main.startLifetime = new ParticleSystem.MinMaxCurve(life.x, life.y);
            main.startSpeed = 0f;
            main.maxParticles = MaxAlive;

            Vector2 sx, sy;
            float streakLength = StartSize(ps, e, 1f, out sx, out sy);
            bool square = Flag(e, "flags", 0x10);
            int align = (int)Num(e, "alignmentType", 0);
            Vector2 rot0 = Range(e, "rotationInitial", 0);
            main.startRotation = new ParticleSystem.MinMaxCurve(rot0.x * Mathf.Deg2Rad, rot0.y * Mathf.Deg2Rad);
            main.startColor = Color.white;

            var em = ps.emission;
            em.rateOverTime = Rate(e);

            Shape(ps, e);
            Motion(ps, e, cl);
            ColourAndSize(ps, e, life, sx, sy, align == 1 || square);
            Sheet(ps, e);

            var r = go.GetComponent<ParticleSystemRenderer>();
            r.sharedMaterial = mat;
            // Castlemist/GW2 Particle reads the custom vertex streams (frame blend, per-particle
            // randoms); without them the next-frame UV is one texel, blended over the whole quad.
            if (mat != null && mat.shader != null && mat.shader.name == MaterialContext.ParticleShaderName)
                CastlemistMaterials.ApplyParticleStreams(r);
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
            r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
            r.sortMode = ParticleSystemSortMode.Distance;
            r.maxParticleSize = 0.5f;   // of the screen: a sprite at the camera does not fill the view
            if (align == 1)
            {
                r.renderMode = ParticleSystemRenderMode.Stretch;
                r.velocityScale = 0f;
                r.lengthScale = streakLength;
            }
            else r.renderMode = ParticleSystemRenderMode.Billboard;
            // Until Fit: a burst fires once, a continuous emitter runs.
            Timing(ps, e, null, 0f, false);
            ps.Play(true);
        }

        /// Groups a second: spawnProbability (percent) x the mean group / spawnPeriod, x the middle
        /// of spawnWindEmit for a wind-driven emitter.
        static float Rate(Dictionary<string, object> e)
        {
            float period = (float)Num(e, "spawnPeriodSec", 1);
            Vector2 group = Range(e, "spawnGroupSize", 1);
            float keep = Mathf.Clamp01((float)Num(e, "spawnProbability", 100) * 0.01f);
            float rate = keep * Mathf.Max((group.x + group.y) * 0.5f, 0f) / Mathf.Max(period, 1e-4f);
            if (Flag(e, "emitterFlags", 0x2) || Flag(e, "emitterFlags", 0x4))
            {
                Vector2 wind = Range(e, "spawnWindEmit", 1);
                rate *= Mathf.Max((wind.x + wind.y) * 0.5f, 0f);
            }
            return rate;
        }

        /// Emission over one loop of the clip from the bone's visibility keys (null: visible
        /// throughout). Bursts: one group at each hidden -> visible change; otherwise the rate while
        /// visible. @p loops repeats with the clip; else the system plays once (a burst) or runs.
        static void Timing(ParticleSystem ps, Dictionary<string, object> e, List<object> keys, float duration, bool loops)
        {
            var main = ps.main;
            var em = ps.emission;
            bool burst = Flag(e, "emitterFlags", 0x8);
            var rises = new List<float>();
            var spans = new List<Vector2>();   // visible intervals within [0, duration]
            if (keys == null) { rises.Add(0f); spans.Add(new Vector2(0f, float.MaxValue)); }
            else
            {
                bool visible = true;
                float start = 0f;
                rises.Add(0f);
                foreach (object k in keys)
                {
                    float t = k is double d ? (float)d : 0f;
                    visible = !visible;
                    if (visible) { rises.Add(t); start = t; }
                    else
                    {
                        // a key at the rise's own time hides the bone before anything spawns
                        if (t <= start) rises.RemoveAt(rises.Count - 1);
                        else spans.Add(new Vector2(start, t));
                    }
                }
                if (visible) spans.Add(new Vector2(start, float.MaxValue));
            }
            float rate = Rate(e);
            if (burst)
            {
                Vector2 group = Range(e, "spawnGroupSize", 1);
                float keep = Mathf.Clamp01((float)Num(e, "spawnProbability", 100) * 0.01f);
                var bursts = new List<ParticleSystem.Burst>();
                foreach (float t in rises)
                    if (!loops || t < duration)
                        bursts.Add(new ParticleSystem.Burst(t, (short)Mathf.Max(0, Mathf.RoundToInt(group.x)),
                                                            (short)Mathf.Max(0, Mathf.RoundToInt(group.y)), 1, 0.01f) { probability = keep });
                em.rateOverTime = 0f;
                em.SetBursts(bursts.ToArray());
                main.loop = loops;
                main.prewarm = false;
                main.duration = loops ? duration : Mathf.Max(main.startLifetime.constantMax, 0.05f);
                return;
            }
            em.SetBursts(new ParticleSystem.Burst[0]);
            if (!loops)
            {
                bool on = spans.Count > 0;
                em.rateOverTime = on ? rate : 0f;
                main.loop = true;
                main.prewarm = on;
                main.duration = Mathf.Max(main.startLifetime.constantMax, 1f);
                return;
            }
            // the rate as a step curve over the clip (normalised time)
            var curve = new AnimationCurve();
            float prev = -1f;
            for (int i = 0; i <= 64; ++i)
            {
                float t = duration * i / 64f, v = 0f;
                foreach (Vector2 s in spans) if (t >= s.x && t < s.y) v = 1f;
                if (v != prev) { var kf = new Keyframe(i / 64f, v, 0f, 0f) { weightedMode = WeightedMode.None }; curve.AddKey(kf); prev = v; }
            }
            for (int i = 0; i < curve.length; ++i) AnimationUtility.SetKeyLeftTangentMode(curve, i, AnimationUtility.TangentMode.Constant);
            for (int i = 0; i < curve.length; ++i) AnimationUtility.SetKeyRightTangentMode(curve, i, AnimationUtility.TangentMode.Constant);
            em.rateOverTime = new ParticleSystem.MinMaxCurve(rate, curve);
            main.loop = true;
            main.prewarm = false;
            main.duration = duration;
        }

        static void Shape(ParticleSystem ps, Dictionary<string, object> e)
        {
            var sh = ps.shape;
            Vector2 rad = Range(e, "spawnRadius", 0);
            float min = Mathf.Max(rad.x, 0f), max = Mathf.Max(rad.y, 0f);
            sh.enabled = true;
            sh.randomDirectionAmount = 0f;
            sh.alignToDirection = false;
            // GW2's shapes lie in the model's XY plane (Z is up / down there, Y here): Unity's circle
            // and edge lie in XY, turned flat.
            sh.rotation = new Vector3(90f, 0f, 0f);
            switch ((int)Num(e, "spawnShape", 6))
            {
                case 0:   // disc: radius min..max
                    sh.shapeType = ParticleSystemShapeType.Circle;
                    sh.radius = Mathf.Max(max, 0.001f);
                    sh.radiusThickness = max > 0 ? Mathf.Clamp01((max - min) / max) : 1f;
                    break;
                case 1:   // line along X, max long, min thick
                    sh.shapeType = ParticleSystemShapeType.Box;
                    sh.rotation = Vector3.zero;
                    sh.scale = new Vector3(Mathf.Max(max * 2f, 0.001f), Mathf.Max(min * 2f, 0.001f), Mathf.Max(min * 2f, 0.001f));
                    break;
                case 3:   // flat rectangle: half-sizes max (X) by min (Y)
                    sh.shapeType = ParticleSystemShapeType.Rectangle;
                    sh.scale = new Vector3(Mathf.Max(max * 2f, 0.001f), Mathf.Max(min * 2f, 0.001f), 1f);
                    break;
                case 4:   // sphere: radius min..max
                    sh.shapeType = ParticleSystemShapeType.Sphere;
                    sh.rotation = Vector3.zero;
                    sh.radius = Mathf.Max(max, 0.001f);
                    sh.radiusThickness = max > 0 ? Mathf.Clamp01((max - min) / max) : 1f;
                    break;
                case 5:   // cylinder: a ring at min, risen up to max
                    sh.shapeType = ParticleSystemShapeType.Box;
                    sh.rotation = Vector3.zero;
                    sh.position = new Vector3(0f, max * 0.5f, 0f);
                    sh.scale = new Vector3(Mathf.Max(min * 2f, 0.001f), Mathf.Max(max, 0.001f), Mathf.Max(min * 2f, 0.001f));
                    break;
                case 2:
                case 6:   // the model's surface: no mesh here, a sphere of the radius
                    sh.shapeType = ParticleSystemShapeType.Sphere;
                    sh.rotation = Vector3.zero;
                    sh.radius = Mathf.Max(max, 0.001f);
                    break;
                default:
                    sh.enabled = false;
                    break;
            }
        }

        static void Motion(ParticleSystem ps, Dictionary<string, object> e, CloudInfo cl)
        {
            Vector3 vMin, vMax, aMin, aMax;
            DirRange(e, "velocity", out vMin, out vMax);
            DirRange(e, "acceleration", out aMin, out aMax);
            var v = ps.velocityOverLifetime;
            v.enabled = true;
            v.space = ParticleSystemSimulationSpace.Local;
            v.x = new ParticleSystem.MinMaxCurve(vMin.x + cl.Velocity.x, vMax.x + cl.Velocity.x);
            v.y = new ParticleSystem.MinMaxCurve(vMin.y + cl.Velocity.y, vMax.y + cl.Velocity.y);
            v.z = new ParticleSystem.MinMaxCurve(vMin.z + cl.Velocity.z, vMax.z + cl.Velocity.z);
            Vector3 a = (aMin + aMax) * 0.5f + cl.Acceleration;
            if (a.sqrMagnitude > 1e-8f)
            {
                var f = ps.forceOverLifetime;
                f.enabled = true;
                f.space = ParticleSystemSimulationSpace.Local;
                f.x = a.x; f.y = a.y; f.z = a.z;
            }
            // GrCloud: v *= 1 - (emitter drag + cloud drag) * dt
            float drag = (float)Num(e, "drag", 0) + cl.Drag;
            if (drag > 0f)
            {
                var l = ps.limitVelocityOverLifetime;
                l.enabled = true;
                l.limit = 1e6f;
                l.drag = drag;
                l.multiplyDragByParticleSize = false;
                l.multiplyDragByParticleVelocity = false;
            }
            Vector2 spin = Range(e, "rotationChange", 0);
            if (Mathf.Abs(spin.x) + Mathf.Abs(spin.y) > 1e-4f)
            {
                var ro = ps.rotationOverLifetime;
                ro.enabled = true;
                if (Flag(e, "flags", 0x1000))
                {
                    float m = Mathf.Max(Mathf.Abs(spin.x), Mathf.Abs(spin.y));
                    ro.z = new ParticleSystem.MinMaxCurve(-m * Mathf.Deg2Rad, m * Mathf.Deg2Rad);
                }
                else ro.z = new ParticleSystem.MinMaxCurve(spin.x * Mathf.Deg2Rad, spin.y * Mathf.Deg2Rad);
            }
        }

        /// Velocity (or acceleration) as Unity ranges: each direction axis {min, max}, scaled so a
        /// typical sample has the mean magnitude (the client normalises each sample); zero when
        /// every direction range is zero (the client then gives no velocity at all).
        static void DirRange(Dictionary<string, object> e, string key, out Vector3 min, out Vector3 max)
        {
            min = max = Vector3.zero;
            var rows = List(e, key);
            if (rows.Count < 4) return;
            var lo = new float[3];
            var hi = new float[3];
            bool any = false;
            for (int k = 0; k < 3; ++k)
            {
                double[] r = Pair(rows[k]);
                lo[k] = (float)Math.Min(r[0], r[1]);
                hi[k] = (float)Math.Max(r[0], r[1]);
                if (lo[k] != 0f || hi[k] != 0f) any = true;
            }
            if (!any) return;
            double[] m = Pair(rows[3]);
            float speed = (float)(m[0] + m[1]) * 0.5f;
            var mid = new Vector3((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f);
            var ext = new Vector3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) * 0.5f;
            float len = Mathf.Max(mid.magnitude, ext.magnitude * 0.577f, 1e-4f);
            Vector3 a = Conv(new Vector3(lo[0], lo[1], lo[2])) * (speed / len);
            Vector3 b = Conv(new Vector3(hi[0], hi[1], hi[2])) * (speed / len);
            min = Vector3.Min(a, b);
            max = Vector3.Max(a, b);
        }

        /// The start size from the sidecar, x @p k (Fit's scale compensation; Build passes 1). Half-
        /// sizes: X from scaleInitial[0], Y from [1] or X (flags 0x10); a streak (alignment 1) takes
        /// Y as its width and returns length / width for the renderer, else 1. Set whole, never
        /// through startSizeMultiplier: on a two-constant size that setter replaces the maximum.
        static float StartSize(ParticleSystem ps, Dictionary<string, object> e, float k, out Vector2 sx, out Vector2 sy)
        {
            var main = ps.main;
            bool square = Flag(e, "flags", 0x10);
            sx = Range(e, "scaleInitial", 0, 1) * 2f;
            sy = square ? sx : Range(e, "scaleInitial", 1, 1) * 2f;
            ParticleSystem.MinMaxCurve C(Vector2 r) { return new ParticleSystem.MinMaxCurve(Mathf.Max(r.x, 0.01f) * k, Mathf.Max(r.y, 0.01f) * k); }
            float streakLength = 1f;
            if ((int)Num(e, "alignmentType", 0) == 1)
            {
                // a streak: X along the velocity, Y across; the renderer stretches width to length
                float w = Mathf.Max((sy.x + sy.y) * 0.5f, 1e-3f);
                streakLength = Mathf.Clamp((sx.x + sx.y) * 0.5f / w, 0.05f, 200f);
                main.startSize3D = false;
                main.startSize = C(sy);
            }
            else if (square)
            {
                main.startSize3D = false;
                main.startSize = C(sx);
            }
            else
            {
                main.startSize3D = true;
                main.startSizeX = C(sx);
                main.startSizeY = C(sy);
                main.startSizeZ = C(sx);
            }
            return streakLength;
        }

        static void ColourAndSize(ParticleSystem ps, Dictionary<string, object> e, Vector2 life, Vector2 sx, Vector2 sy, bool uniform)
        {
            // colorBegin / colorEnd are {min, max} colours: their middles, blended by the client's
            // weight w(t) (ColourWeight), alpha too, then alpha x the opacity curve
            Color c0 = Mid(e, "colorBegin"), c1 = Mid(e, "colorEnd");
            List<Vector2> op = Curve(e, "opacityCurve");
            if (op.Count == 0) op = new List<Vector2> { new Vector2(0, 0), new Vector2(0.15f, 1), new Vector2(0.6f, 1), new Vector2(1, 0) };
            float period = (float)Num(e, "colorPeriod", Num(e, "colorPeriodSec", 1));
            double[] fo = Arr(e, "colorFalloff");   // as stored: the order matters
            Vector2 falloff = fo.Length >= 2 ? new Vector2((float)fo[0], (float)fo[1]) : new Vector2(0f, 1f);
            const int samples = 128;
            var ws = new List<Vector2>();
            var alphas = new List<Vector2>();
            float wMean = 0f;
            for (int i = 0; i <= samples; ++i)
            {
                float t = i / (float)samples;
                float w = ColourWeight(t, period, falloff);
                ws.Add(new Vector2(t, w));
                wMean += w / (samples + 1);
            }
            // A colour bouncing more than 3 times a life is a flicker the 8 gradient keys cannot
            // follow: its mean colour instead.
            bool flicker = period > 0f && period < 1f / 6f;
            for (int i = 0; i <= samples; ++i)
            {
                float t = i / (float)samples, w = flicker ? wMean : ws[i].y;
                alphas.Add(new Vector2(t, Mathf.Clamp01(Mathf.Lerp(c0.a, c1.a, w) * Mathf.Clamp01(CurveAt(op, t)))));
            }
            var g = new Gradient();
            var ck = new List<GradientColorKey>();
            foreach (Vector2 k in flicker ? new List<Vector2> { new Vector2(0, wMean), new Vector2(1, wMean) } : Reduce(ws, 8))
            {
                Color c = Color.Lerp(c0, c1, k.y);
                ck.Add(new GradientColorKey(new Color(c.r, c.g, c.b), k.x));
            }
            var ak = new List<GradientAlphaKey>();
            foreach (Vector2 k in Reduce(alphas, 8)) ak.Add(new GradientAlphaKey(k.y, k.x));
            g.SetKeys(ck.ToArray(), ak.ToArray());
            var col = ps.colorOverLifetime;
            col.enabled = true;
            col.color = new ParticleSystem.MinMaxGradient(g);

            // size: (start + scaleChange x age) x scaleCurve (0..1 in the client), per axis
            float meanLife = (life.x + life.y) * 0.5f;
            Vector2 dx = Range(e, "scaleChange", 0, 0) * 2f, dy = Range(e, "scaleChange", 1, 0) * 2f;
            if (Flag(e, "flags", 0x10)) dy = dx;
            float gx = (dx.x + dx.y) * 0.5f * meanLife / Mathf.Max((sx.x + sx.y) * 0.5f, 1e-3f);
            float gy = (dy.x + dy.y) * 0.5f * meanLife / Mathf.Max((sy.x + sy.y) * 0.5f, 1e-3f);
            List<Vector2> sc = Curve(e, "scaleCurve");
            if (sc.Count == 0 && Mathf.Abs(gx) < 1e-3f && Mathf.Abs(gy) < 1e-3f) return;
            if (sc.Count == 0) sc = new List<Vector2> { new Vector2(0, 1), new Vector2(1, 1) };
            AnimationCurve Grow(float k)
            {
                var c = new AnimationCurve();
                foreach (Vector2 p in sc) c.AddKey(Mathf.Clamp01(p.x), Mathf.Max(0f, Mathf.Clamp01(p.y) * (1f + k * Mathf.Clamp01(p.x))));
                if (c.length == 1) c.AddKey(1f, c.keys[0].value);
                return c;
            }
            var so = ps.sizeOverLifetime;
            so.enabled = true;
            if (uniform || Mathf.Abs(gx - gy) < 1e-3f) so.size = new ParticleSystem.MinMaxCurve(1f, Grow(gx));
            else
            {
                so.separateAxes = true;
                so.x = new ParticleSystem.MinMaxCurve(1f, Grow(gx));
                so.y = new ParticleSystem.MinMaxCurve(1f, Grow(gy));
                so.z = new ParticleSystem.MinMaxCurve(1f, Grow(gx));
            }
        }

        static void Sheet(ParticleSystem ps, Dictionary<string, object> e)
        {
            var fb = Obj(e, "flipbook");
            if (fb == null) return;
            int cols = (int)Num(fb, "columns", 1), rows = (int)Num(fb, "rows", 1), count = (int)Num(fb, "count", 1), first = (int)Num(fb, "start", 0);
            float fps = (float)Num(fb, "fps", 0);
            if (cols < 1 || rows < 1 || count < 1 || cols * rows < 2) return;
            int total = cols * rows;
            var t = ps.textureSheetAnimation;
            t.enabled = true;
            t.mode = ParticleSystemAnimationMode.Grid;
            t.numTilesX = cols;
            t.numTilesY = rows;
            t.animation = ParticleSystemAnimationType.WholeSheet;
            float a = (float)first / total, b = (float)Math.Min(first + count, total) / total;
            if (count == 1)
            {
                t.frameOverTime = new ParticleSystem.MinMaxCurve(a);   // one fixed cell
                return;
            }
            if (fps > 0.01f)
            {
                t.timeMode = ParticleSystemAnimationTimeMode.FPS;
                t.fps = fps;
                t.startFrame = new ParticleSystem.MinMaxCurve(first, first + count - 1);
                t.frameOverTime = new ParticleSystem.MinMaxCurve(1f, AnimationCurve.Linear(0, a, 1, b));
            }
            else t.frameOverTime = new ParticleSystem.MinMaxCurve(1f, AnimationCurve.Linear(0, a, 1, b - 1e-4f));
        }

        static MaterialContext builtCtx;
        static readonly HashSet<string> built = new HashSet<string>();

        /// The cloud's material on Castlemist/GW2 Particle, one .mat per atlas rect.
        static Material MaterialFor(MaterialContext ctx, MaterialsFile.MaterialEntry me, string modelFolder, string matFolder, Vector4 rect)
        {
            bool full = rect.x <= 0 && rect.y <= 0 && rect.z >= 1 && rect.w >= 1 || rect.z <= rect.x || rect.w <= rect.y;
            string suffix = full ? "" : "_" + string.Format(CultureInfo.InvariantCulture, "{0:0.###}_{1:0.###}_{2:0.###}_{3:0.###}", rect.x, rect.y, rect.z, rect.w).Replace('.', 'p');
            string path = matFolder + "/" + me.FileName + "_fx" + suffix + ".mat";
            // Rebuilt in place (same GUID) once per import, so a shader or builder change reaches
            // existing .mats; later instances of the model reuse it.
            if (builtCtx != ctx) { builtCtx = ctx; built.Clear(); }
            Material mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat != null && built.Contains(path)) return mat;
            built.Add(path);
            mat = CastlemistMaterials.BuildParticleMaterial(ctx, me, modelFolder, false, null, path);
            // Interim until the particle shader has a nebula family: AMATs such as 728777 have no
            // diffuse, their shape is the mask (a cloud flipbook) over a tiled background and a
            // perturb flow. The mask alone draws the shape instead of a solid white quad.
            MaterialsFile.Extra mask = me.Map("baseColor") == null && !mat.GetTexture("_MainTex") ? me.ExtraOf("mask") : null;
            if (mask != null)
            {
                PoiyomiMaterials.SetTex(ctx, mat, "_MainTex", modelFolder, mask.Map, Path.GetFileName(modelFolder) + "/" + me.Name);
                EditorUtility.SetDirty(mat);
            }
            if (!full)
            {
                // texCoordRect (u0, v0, u1, v1), v from the image bottom as in Unity
                // (the Tome atlas 1653632: SpawnFlare 0.37..0.5 is its flare strip only so).
                mat.SetTextureScale("_MainTex", new Vector2(rect.z - rect.x, rect.w - rect.y));
                mat.SetTextureOffset("_MainTex", new Vector2(rect.x, rect.y));
                EditorUtility.SetDirty(mat);
            }
            return mat;
        }

        /// "GW2 FX <bone> <cloud>.<emitter>[ abs]" -> its parts.
        static bool ParseName(string name, out string bone, out int cloud, out int emitter)
        {
            bone = null; cloud = emitter = -1;
            string s = name.Substring(ObjectPrefix.Length);
            if (s.EndsWith(AbsoluteTag, StringComparison.Ordinal)) s = s.Substring(0, s.Length - AbsoluteTag.Length);
            int sp = s.LastIndexOf(' ');
            if (sp < 0) return false;
            string[] ix = s.Substring(sp + 1).Split('.');
            if (ix.Length != 2 || !int.TryParse(ix[0], out cloud) || !int.TryParse(ix[1], out emitter)) return false;
            bone = s.Substring(0, sp);
            if (bone == "cloud") bone = null;
            return true;
        }

        // ---- GW2 model space (X east, Y north, Z down) -> Unity (map_to_unity: x, -z, y) ----
        static Vector3 Conv(Vector3 g) { return new Vector3(g.x, -g.z, g.y); }

        static Quaternion ConvRot(double[] t)
        {
            // rows of the 3x4 emitter matrix; columns are its axes in model space
            var cx = Conv(new Vector3((float)t[0], (float)t[4], (float)t[8]));
            var cy = Conv(new Vector3((float)t[1], (float)t[5], (float)t[9]));
            var cz = Conv(new Vector3((float)t[2], (float)t[6], (float)t[10]));
            // Unity axes x, y, z come from GW2 x, -z, y
            Vector3 uy = -cz, uz = cy;
            if (uy.sqrMagnitude < 1e-8f || uz.sqrMagnitude < 1e-8f) return Quaternion.identity;
            return Quaternion.LookRotation(uz.normalized, uy.normalized);
        }

        // ---- JSON helpers ----
        static List<object> List(Dictionary<string, object> d, string k)
        {
            object v; return d.TryGetValue(k, out v) && v is List<object> l ? l : new List<object>();
        }
        static Dictionary<string, object> Obj(Dictionary<string, object> d, string k)
        {
            object v; return d.TryGetValue(k, out v) ? v as Dictionary<string, object> : null;
        }
        static bool Has(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v != null; }
        static double Num(Dictionary<string, object> d, string k, double def)
        {
            object v; return d.TryGetValue(k, out v) && v is double x ? x : def;
        }
        static bool Flag(Dictionary<string, object> d, string k, uint bit) { return ((uint)Num(d, k, 0) & bit) != 0; }
        static bool Bool(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) && v is bool b && b; }
        static string Str(Dictionary<string, object> d, string k) { object v; return d.TryGetValue(k, out v) ? v as string : null; }
        static double[] Arr(Dictionary<string, object> d, string k)
        {
            var l = List(d, k);
            var r = new double[l.Count];
            for (int i = 0; i < l.Count; ++i) r[i] = l[i] is double x ? x : 0;
            return r;
        }
        static double[] Pair(object o)
        {
            var l = o as List<object>;
            if (l == null || l.Count < 2) return new double[] { 0, 0 };
            return new[] { l[0] is double a ? a : 0, l[1] is double b ? b : 0 };
        }
        static Vector3 Vec(Dictionary<string, object> d, string k)
        {
            double[] a = Arr(d, k);
            return a.Length >= 3 ? new Vector3((float)a[0], (float)a[1], (float)a[2]) : Vector3.zero;
        }
        /// A {min, max} pair, sorted; @p def for both when absent.
        static Vector2 Range(Dictionary<string, object> d, string k, float def)
        {
            if (!Has(d, k)) return new Vector2(def, def);
            double[] p = Pair(d[k]);
            return new Vector2((float)Math.Min(p[0], p[1]), (float)Math.Max(p[0], p[1]));
        }
        /// Row @p row of an array of {min, max} pairs (scaleInitial, scaleChange).
        static Vector2 Range(Dictionary<string, object> d, string k, int row, float def)
        {
            var l = List(d, k);
            if (row >= l.Count) return new Vector2(def, def);
            double[] p = Pair(l[row]);
            return new Vector2((float)Math.Min(p[0], p[1]), (float)Math.Max(p[0], p[1]));
        }
        static Vector4 Rect4(Dictionary<string, object> d)
        {
            double[] a = Arr(d, "texCoordRect");
            return a.Length >= 4 ? new Vector4((float)a[0], (float)a[1], (float)a[2], (float)a[3]) : new Vector4(0, 0, 1, 1);
        }
        /// colorBegin / colorEnd: [min RGBA, max RGBA]; their middle.
        static Color Mid(Dictionary<string, object> d, string k)
        {
            var l = List(d, k);
            Color C(int i)
            {
                if (i >= l.Count || !(l[i] is List<object> c) || c.Count < 4) return i == 0 ? Color.white : C(0);
                float F(int j) { return c[j] is double x ? Mathf.Clamp01((float)x) : 1f; }
                return new Color(F(0), F(1), F(2), F(3));
            }
            return Color.Lerp(C(0), C(1), 0.5f);
        }
        static List<Vector2> Curve(Dictionary<string, object> d, string k)
        {
            var r = new List<Vector2>();
            foreach (object o in List(d, k))
                if (o is Dictionary<string, object> key) r.Add(new Vector2((float)Num(key, "t", 0), (float)Num(key, "value", 1)));
            r.Sort((a, b) => a.x.CompareTo(b.x));
            return r;
        }
        /// At most @p n keys (a gradient takes 8), keeping the first and last.
        /// The client's colour weight at life fraction @p t (0x140ad3a30): with m = t mod period and
        /// n = floor(t / period), x = m / period on even n and 1 - m / period on odd n (a ping-pong;
        /// period 0: x = 0), remapped from [falloff.x, falloff.y] to [0, 1] and clamped
        /// (0x140acf2e0). colour = begin x (1 - w) + end x w, alpha included.
        static float ColourWeight(float t, float period, Vector2 falloff)
        {
            float x = 0f;
            if (period > 0f)
            {
                float m = t % period;
                int n = (int)Mathf.Floor(t / period);
                x = (n & 1) == 0 ? m / period : 1f - m / period;
            }
            if (falloff.x == falloff.y) return x > falloff.y ? 1f : 0f;
            return Mathf.Clamp01((x - falloff.x) / (falloff.y - falloff.x));
        }

        /// A piecewise-linear curve through @p keys sampled at @p t.
        static float CurveAt(List<Vector2> keys, float t)
        {
            if (keys.Count == 0) return 1f;
            if (t <= keys[0].x) return keys[0].y;
            for (int i = 1; i < keys.Count; ++i)
                if (t <= keys[i].x)
                {
                    float span = keys[i].x - keys[i - 1].x;
                    return span > 1e-6f ? Mathf.Lerp(keys[i - 1].y, keys[i].y, (t - keys[i - 1].x) / span) : keys[i].y;
                }
            return keys[keys.Count - 1].y;
        }

        /// At most @p n of the dense samples @p s: the ends, then repeatedly the sample farthest
        /// from the line the kept ones draw (Unity's gradients take 8 keys).
        static List<Vector2> Reduce(List<Vector2> s, int n)
        {
            if (s.Count <= n) return s;
            var keep = new List<int> { 0, s.Count - 1 };
            while (keep.Count < n)
            {
                keep.Sort();
                int best = -1;
                float err = 1e-4f;
                for (int k = 1; k < keep.Count; ++k)
                    for (int i = keep[k - 1] + 1; i < keep[k]; ++i)
                    {
                        Vector2 a = s[keep[k - 1]], b = s[keep[k]];
                        float f = (s[i].x - a.x) / Mathf.Max(b.x - a.x, 1e-6f);
                        float d = Mathf.Abs(s[i].y - Mathf.Lerp(a.y, b.y, f));
                        if (d > err) { err = d; best = i; }
                    }
                if (best < 0) break;
                keep.Add(best);
            }
            keep.Sort();
            var r = new List<Vector2>();
            foreach (int i in keep) r.Add(s[i]);
            return r;
        }

        static Transform FindDeep(Transform t, string name)
        {
            if (t.name == name) return t;
            foreach (Transform c in t)
            {
                Transform f = FindDeep(c, name);
                if (f != null) return f;
            }
            return null;
        }
    }
}
