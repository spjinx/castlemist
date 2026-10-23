// castlemist world export: GW2 particle clouds as Unity particle systems (2026-10-22).
//
// A model's <fileId>_particles.json (src/exportgltf/particle_export.cpp) lists its particle clouds,
// each with emitters; AddTo builds one Shuriken ParticleSystem per emitter under a placed instance,
// at the cloud's (or emitter's) bone so it follows the model's animation. castlemist's own reading
// of the emitter fields, not the game's renderer:
//   * every float2 range is {min, max} (lifetimes, sizes and radii are stored as [b, b] for a
//     constant, directions as [-1, 1] for a symmetric spray); the RE note's {base, span} is taken
//     as the client's form after load (UNPROVEN either way)
//   * velocity / acceleration rows are (dirX, dirY, dirZ, magnitude) in model space (GW2 Z down)
//   * scaleInitial[1] is the height over the width (most emitters store 1), except on a streak
//   * alignmentType 0 camera-facing, 1 a streak along the velocity (scaleInitial[0] its length,
//     [1] its width), 3 horizontal (UNPROVEN)
//   * an opacity curve with no keys gets the castlemist viewer's envelope (15% fade in, fade out)
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
        const float MaxRate = 80f;
        const int MaxAlive = 400;

        /// The <fileId>_particles.json beside a model's materials.json, or null.
        public static string SidecarFor(string modelFolder, long fileId)
        {
            string p = modelFolder + "/" + fileId + "_particles.json";
            return File.Exists(p) ? p : null;
        }

        /// Builds every emitter of @p sidecar under @p instance (replacing an earlier build); returns
        /// the particle systems made. @p matFolder holds the particle .mat files.
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
            foreach (Transform t in instance.GetComponentsInChildren<Transform>(true))
                if (t != null && t.name.StartsWith(ObjectPrefix, StringComparison.Ordinal) && t.GetComponent<ParticleSystem>() != null)
                    UnityEngine.Object.DestroyImmediate(t.gameObject);

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
                Vector3 cloudVel = Conv(Vec(cloud, "velocity")), cloudAcc = Conv(Vec(cloud, "acceleration"));
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
                    Build(instance.transform, at ?? instance.transform, e, mat, cloudVel, cloudAcc,
                          ObjectPrefix + (bone ?? "cloud") + " " + ci + "." + ei);
                    ++built;
                }
            }
            if (distortion > 0) warnings.Add(tag + ": " + distortion + " distortion emitter(s) skipped (heat haze / refraction)");
            return built;
        }

        static void Build(Transform model, Transform bone, Dictionary<string, object> e, Material mat,
                          Vector3 cloudVel, Vector3 cloudAcc, string name)
        {
            var go = new GameObject(name);
            // The emitter sits at the bone's position, in the model's axes (GW2 moves particles in
            // model space), at the model's scale: sizes and speeds are model units.
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
            // Local: the placement's scale applies to sizes and speeds too (GW2 model units); in World
            // space Unity leaves particle sizes unscaled, ~40x too big on the Mists vault (2026-10-22).
            main.simulationSpace = ParticleSystemSimulationSpace.Local;
            main.scalingMode = ParticleSystemScalingMode.Hierarchy;
            main.startLifetime = new ParticleSystem.MinMaxCurve(life.x, life.y);
            main.startSpeed = 0f;
            Vector2 size = Range(e, "scaleInitial", 0, 1);
            Vector2 aspect = Range(e, "scaleInitial", 1, 1);
            int align = (int)Num(e, "alignmentType", 0);
            float asp = (aspect.x + aspect.y) * 0.5f;
            if (asp <= 0f) asp = 1f;
            asp = Mathf.Clamp(asp, 0.05f, 20f);
            float streakLength = 1f;
            if (align == 1 && aspect.y > 0f)
            {
                // a streak: scaleInitial[0] its length, [1] its width, both absolute (StarsCL 600 x 60,
                // RaysCL 110 x 1.5-3, SpikeCL 65-195 x 6.5-97); the renderer stretches width to length.
                streakLength = Mathf.Clamp((size.x + size.y) / Mathf.Max(aspect.x + aspect.y, 1e-3f), 0.05f, 100f);
                main.startSize = new ParticleSystem.MinMaxCurve(Mathf.Max(aspect.x, 0.01f), Mathf.Max(aspect.y, 0.01f));
            }
            else if (Mathf.Abs(asp - 1f) > 1e-3f)
            {
                main.startSize3D = true;
                main.startSizeX = new ParticleSystem.MinMaxCurve(size.x, size.y);
                main.startSizeY = new ParticleSystem.MinMaxCurve(size.x * asp, size.y * asp);
                main.startSizeZ = new ParticleSystem.MinMaxCurve(size.x, size.y);
            }
            else main.startSize = new ParticleSystem.MinMaxCurve(Mathf.Max(size.x, 0.01f), Mathf.Max(size.y, 0.01f));
            Vector2 rot0 = Range(e, "rotationInitial", 0);
            main.startRotation = new ParticleSystem.MinMaxCurve(rot0.x * Mathf.Deg2Rad, rot0.y * Mathf.Deg2Rad);
            main.startColor = Color.white;

            // Emission: a group every spawnPeriod, kept with spawnProbability (percent when > 1).
            float period = (float)Num(e, "spawnPeriodSec", 1);
            Vector2 group = Range(e, "spawnGroupSize", 1);
            double prob = Num(e, "spawnProbability", 100);
            float keep = Mathf.Clamp01((float)(prob > 1 ? prob / 100.0 : prob));
            // At most MaxRate a second and MaxAlive at once: GW2 gates many emitters by animation
            // (an icicle burst, a portal opening), which a looping system cannot know, so they
            // would run at their burst rate (1500 a second on 3124820) all the time.
            float rate = Mathf.Min(keep * Mathf.Max((group.x + group.y) * 0.5f, 1f) / period, MaxRate);
            var em = ps.emission;
            em.rateOverTime = rate;
            main.maxParticles = Mathf.Clamp(Mathf.CeilToInt(rate * life.y * 1.5f) + 4, 8, MaxAlive);

            Shape(ps, e);
            Motion(ps, e, cloudVel, cloudAcc);
            ColourAndSize(ps, e, life);
            Sheet(ps, e);

            var r = go.GetComponent<ParticleSystemRenderer>();
            r.sharedMaterial = mat;
            r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.lightProbeUsage = UnityEngine.Rendering.LightProbeUsage.Off;
            r.reflectionProbeUsage = UnityEngine.Rendering.ReflectionProbeUsage.Off;
            r.sortMode = ParticleSystemSortMode.Distance;
            r.maxParticleSize = 0.5f;   // of the screen: a mist sprite at the camera does not fill the view
            switch (align)
            {
                case 1:
                    r.renderMode = ParticleSystemRenderMode.Stretch;
                    r.velocityScale = 0f;
                    r.lengthScale = streakLength;
                    break;
                case 3: r.renderMode = ParticleSystemRenderMode.HorizontalBillboard; break;
                default: r.renderMode = ParticleSystemRenderMode.Billboard; break;
            }
            ps.Play(true);
        }

        static void Shape(ParticleSystem ps, Dictionary<string, object> e)
        {
            var sh = ps.shape;
            Vector2 rad = Range(e, "spawnRadius", 0);
            float outer = Mathf.Max(rad.x, rad.y);
            sh.enabled = true;
            sh.radiusThickness = outer > 0 ? Mathf.Clamp01((outer - Mathf.Min(rad.x, rad.y)) / outer) : 1f;
            sh.randomDirectionAmount = 0f;
            // GW2 shapes stand on the model's up axis (-Z there, +Y here): Unity's circle lies in XY.
            sh.rotation = new Vector3(90f, 0f, 0f);
            switch ((int)Num(e, "spawnShape", 6))
            {
                case 0:   // cylinder: a ring, extended downward by its radius
                    sh.shapeType = ParticleSystemShapeType.Circle;
                    sh.radius = Mathf.Max(outer, 0.001f);
                    break;
                case 1:
                    sh.shapeType = ParticleSystemShapeType.Circle;
                    sh.radius = Mathf.Max(outer, 0.001f);
                    break;
                case 2:
                    sh.shapeType = ParticleSystemShapeType.SingleSidedEdge;
                    sh.rotation = Vector3.zero;
                    sh.radius = Mathf.Max(outer, 0.001f);
                    break;
                case 3:
                    sh.shapeType = ParticleSystemShapeType.Box;
                    sh.rotation = Vector3.zero;
                    sh.scale = Vector3.one * Mathf.Max(outer * 2f, 0.001f);
                    break;
                case 4:
                case 5:   // the model's surface: a sphere of the radius (no mesh to sample here)
                    sh.shapeType = ParticleSystemShapeType.Sphere;
                    sh.radius = Mathf.Max(outer, 0.001f);
                    break;
                default:
                    sh.enabled = false;
                    break;
            }
        }

        static void Motion(ParticleSystem ps, Dictionary<string, object> e, Vector3 cloudVel, Vector3 cloudAcc)
        {
            // direction ranges scaled to the mean speed (the game normalises each sample).
            Vector3 vMin, vMax;
            DirRange(e, "velocity", out vMin, out vMax);
            Vector3 aMin, aMax;
            DirRange(e, "acceleration", out aMin, out aMax);
            var v = ps.velocityOverLifetime;
            v.enabled = true;
            v.space = ParticleSystemSimulationSpace.Local;
            v.x = new ParticleSystem.MinMaxCurve(vMin.x + cloudVel.x, vMax.x + cloudVel.x);
            v.y = new ParticleSystem.MinMaxCurve(vMin.y + cloudVel.y, vMax.y + cloudVel.y);
            v.z = new ParticleSystem.MinMaxCurve(vMin.z + cloudVel.z, vMax.z + cloudVel.z);
            Vector3 a = (aMin + aMax) * 0.5f + cloudAcc;
            if (a.sqrMagnitude > 1e-8f)
            {
                var f = ps.forceOverLifetime;
                f.enabled = true;
                f.space = ParticleSystemSimulationSpace.Local;
                f.x = a.x; f.y = a.y; f.z = a.z;
            }
            float drag = (float)Num(e, "drag", 0);
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
                ro.z = new ParticleSystem.MinMaxCurve(spin.x * Mathf.Deg2Rad, spin.y * Mathf.Deg2Rad);
            }
        }

        static void DirRange(Dictionary<string, object> e, string key, out Vector3 min, out Vector3 max)
        {
            min = max = Vector3.zero;
            var rows = List(e, key);
            if (rows.Count < 4) return;
            var lo = new float[3];
            var hi = new float[3];
            for (int k = 0; k < 3; ++k)
            {
                double[] r = Pair(rows[k]);
                lo[k] = (float)Math.Min(r[0], r[1]);
                hi[k] = (float)Math.Max(r[0], r[1]);
            }
            double[] m = Pair(rows[3]);
            float speed = (float)(m[0] + m[1]) * 0.5f;
            var mid = new Vector3((lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f);
            var ext = new Vector3(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]) * 0.5f;
            // the typical sample's length: the mid point, or the spread when the range is centred.
            float len = Mathf.Max(mid.magnitude, ext.magnitude * 0.577f, 1e-4f);
            Vector3 a = Conv(new Vector3(lo[0], lo[1], lo[2])) * (speed / len);
            Vector3 b = Conv(new Vector3(hi[0], hi[1], hi[2])) * (speed / len);
            min = Vector3.Min(a, b);
            max = Vector3.Max(a, b);
        }

        static void ColourAndSize(ParticleSystem ps, Dictionary<string, object> e, Vector2 life)
        {
            Color c0 = Col(e, "colorBegin"), c1 = Col(e, "colorEnd");
            List<Vector2> op = Curve(e, "opacityCurve");
            if (op.Count == 0) op = new List<Vector2> { new Vector2(0, 0), new Vector2(0.15f, 1), new Vector2(0.6f, 1), new Vector2(1, 0) };
            var g = new Gradient();
            var ck = new GradientColorKey[] { new GradientColorKey(new Color(c0.r, c0.g, c0.b), 0f), new GradientColorKey(new Color(c1.r, c1.g, c1.b), 1f) };
            var ak = new List<GradientAlphaKey>();
            foreach (Vector2 k in Thin(op, 8))
                ak.Add(new GradientAlphaKey(Mathf.Clamp01(Mathf.Lerp(c0.a, c1.a, k.x) * k.y), Mathf.Clamp01(k.x)));
            g.SetKeys(ck, ak.ToArray());
            var col = ps.colorOverLifetime;
            col.enabled = true;
            col.color = new ParticleSystem.MinMaxGradient(g);

            // size: scaleCurve x (1 + growth / start size), growth = scaleChange per second.
            Vector2 s0 = Range(e, "scaleInitial", 0, 1);
            Vector2 ds = Range(e, "scaleChange", 0, 0);
            float start = Mathf.Max((s0.x + s0.y) * 0.5f, 1e-3f);
            float grow = (ds.x + ds.y) * 0.5f * (life.x + life.y) * 0.5f / start;
            List<Vector2> sc = Curve(e, "scaleCurve");
            if (sc.Count == 0 && Mathf.Abs(grow) < 1e-3f) return;
            if (sc.Count == 0) sc = new List<Vector2> { new Vector2(0, 1), new Vector2(1, 1) };
            var curve = new AnimationCurve();
            foreach (Vector2 k in sc) curve.AddKey(Mathf.Clamp01(k.x), Mathf.Max(0f, k.y * (1f + grow * Mathf.Clamp01(k.x))));
            if (curve.length == 1) curve.AddKey(1f, curve.keys[0].value);
            var so = ps.sizeOverLifetime;
            so.enabled = true;
            so.size = new ParticleSystem.MinMaxCurve(1f, curve);
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
                // a loop of count frames at fps, from a random frame (the viewer's frameSeed)
                t.timeMode = ParticleSystemAnimationTimeMode.FPS;
                t.fps = fps;
                t.startFrame = new ParticleSystem.MinMaxCurve(first, first + count - 1);
                t.frameOverTime = new ParticleSystem.MinMaxCurve(1f, AnimationCurve.Linear(0, a, 1, b));
            }
            else t.frameOverTime = new ParticleSystem.MinMaxCurve(1f, AnimationCurve.Linear(0, a, 1, b - 1e-4f));
        }

        /// The cloud's material on Castlemist/GW2 Particle, one .mat per atlas rect.
        static Material MaterialFor(MaterialContext ctx, MaterialsFile.MaterialEntry me, string modelFolder, string matFolder, Vector4 rect)
        {
            bool full = rect.x <= 0 && rect.y <= 0 && rect.z >= 1 && rect.w >= 1 || rect.z <= rect.x || rect.w <= rect.y;
            string suffix = full ? "" : "_" + string.Format(CultureInfo.InvariantCulture, "{0:0.###}_{1:0.###}_{2:0.###}_{3:0.###}", rect.x, rect.y, rect.z, rect.w).Replace('.', 'p');
            string path = matFolder + "/" + me.FileName + "_fx" + suffix + ".mat";
            Material mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat != null && mat.shader == ctx.ParticleShader) return mat;
            mat = CastlemistMaterials.BuildParticleMaterial(ctx, me, modelFolder, false, null, path);
            if (!full)
            {
                // texCoordRect (u0, v0, u1, v1), v from the top: Unity's v runs from the bottom.
                mat.SetTextureScale("_MainTex", new Vector2(rect.z - rect.x, rect.w - rect.y));
                mat.SetTextureOffset("_MainTex", new Vector2(rect.x, 1f - rect.w));
                EditorUtility.SetDirty(mat);
            }
            return mat;
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
            Vector3 ux = cx, uy = -cz, uz = cy;
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
        /// colorBegin / colorEnd: [base RGBA, ...]; the first.
        static Color Col(Dictionary<string, object> d, string k)
        {
            var l = List(d, k);
            if (l.Count == 0 || !(l[0] is List<object> c) || c.Count < 4) return Color.white;
            float F(int i) { return c[i] is double x ? Mathf.Clamp01((float)x) : 1f; }
            return new Color(F(0), F(1), F(2), F(3));
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
        static List<Vector2> Thin(List<Vector2> keys, int n)
        {
            if (keys.Count <= n) return keys;
            var r = new List<Vector2>();
            for (int i = 0; i < n; ++i) r.Add(keys[(int)Math.Round(i * (keys.Count - 1) / (double)(n - 1))]);
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
