// castlemist world export: the map's look after the import (2026-10-22).
//
// WorldImporter calls SceneSetup.Apply after the sky, the sun and the frame
// probe. It reads manifest.environment (the env chunk: lights, haze, effect;
// WorldManifest.EnvironmentEntry; every block may be null) and sets up, each
// behind an ImportOptions toggle (default on; the Quest preset turns post off):
//   haze             the default fog path: "GW2 Haze", a Unity cube under the map root with
//                    Castlemist/GW2 Haze (Shaders/GW2 Haze.shader, after Silent's SkyProbe
//                    Fog): GW2's own distance + height haze per pixel from the camera depth
//                    texture, environment.haze as raw data (_DistRange in GW2 units,
//                    _HeightRange as stored: map z, up = -Z), material
//                    <folder>/GW2_<fileId>_Haze.mat. RenderSettings.fog goes off (no double
//                    fog; the volume fogs avatars too). Needs the depth texture (in VRChat the
//                    sun's shadows give it); transparents drawn after queue Transparent-216
//                    are not fogged by it. No haze block or no shader: the fog fallback
//   fog              RenderSettings Linear fog from environment.haze
//                    (EnvironmentMath.FogFromHaze: start / end from distRange x
//                    unityScale and the near / far alphas, one colour). Unity's
//                    built-in fog has one colour and no height term: haze.height
//                    and heightRange are not mapped, nor environment.zones.
//                    Castlemist/GW2 and Poiyomi apply it (multi_compile_fog); the
//                    Skybox/Panoramic sky is not fogged
//   ambient          RenderSettings Trilight from the fills environment.lights[1..]
//                    (EnvironmentMath.TrilightFromFills). Trilight, not a custom
//                    SphericalHarmonicsL2: Unity re-derives RenderSettings.ambientProbe
//                    from the ambient mode whenever it updates the environment (and
//                    on a bake), so a hand-set probe does not survive; Trilight does
//                    and is baked into the probes and light volumes. No fills (or no
//                    environment): the Flat light.ambient SkyAndLight set, as before
//                    (light.ambient is fill x min(intensity, 1) of the same fill, so
//                    the two are not added)
//   post             Post Processing Stack v2 (com.unity.postprocessing; compiled
//                    only with UNITY_POST_PROCESSING_STACK_V2 and the asmdef's
//                    CASTLEMIST_PPV2 version define): a global PostProcessVolume on
//                    the "PostProcessing" layer (created in the first free user layer,
//                    22.. first: VRChat's own are 0-21) with the profile
//                    <folder>/GW2_<fileId>_PostProcess.asset -- Bloom
//                    (EnvironmentMath.BloomFromEffect), ColorGrading LDR (saturation,
//                    colour filter, and effect.clut as the LDR lookup strip when it is
//                    width = height^2; imported linear, uncompressed, no mips, clamp),
//                    AmbientOcclusion MSVO (ssaoAmount) left off and Bloom fast (VR cost);
//                    no depth of field (VR). A
//                    PostProcessLayer goes on the reference camera: the VRC scene
//                    descriptor's ReferenceCamera (VRChat SDK3; one is made and linked
//                    when the descriptor has none), else Camera.main, else a new
//                    "Main Camera" at the spawn. Every mapping is UNPROVEN
//   lightVolumes     VRC Light Volumes (red.sim.lightvolumes 3.x, found by
//                    reflection: VRCLightVolumes.LightVolumeInstance; 3.x's
//                    LightVolumeSetup / LightVolume are migration-only legacy types):
//                    one volume per cell over the cell's renderer bounds, adaptive
//                    resolution at EnvironmentMath.VoxelsPerUnit (opts.lightVolumeBudget
//                    voxels a cell, <= 1 per unit), registered with the scene's
//                    LightVolumeManager (the package's own onboarding). The sun turns
//                    Mixed (Baked Indirect) so the bake carries its bounce. Needs a bake
//   reflectionProbes a baked, box-projected ReflectionProbe per cell (the cell's
//                    bounds; captured 2 units above the ground at the centre),
//                    opts.reflectionResolution px; the default reflection is the
//                    skybox. Needs a bake
//   occlusion        static flags on the map's renderers (StaticFlagsFor): opaque
//                    (every material's queue < 2450) Occluder + Occludee, cutout and
//                    transparent Occludee only; ContributeGI on queue < 2500 with
//                    MeshRenderer.receiveGI = Light Probes (GW2's UV1 is no lightmap
//                    UV, so nothing is lightmapped); ReflectionProbeStatic with
//                    reflectionProbes. Animated props (an Animator above them: the
//                    instances WorldImporter gave a clip and cleared), foliage
//                    (FoliageImporter's Occludee-only trees and patches), particle and
//                    skinned renderers are left alone. One OcclusionArea over the map
//   (lighting)       with lightVolumes, reflectionProbes or occlusion: the scene's
//                    LightingSettings <folder>/GW2_<fileId>_Lighting.lighting: baked
//                    GI on, realtime GI off, Progressive GPU, Baked Indirect, no
//                    auto-generate
// Everything goes under the unscaled root GW2_<fileId>_Environment (a light
// volume's and a probe's size is the transform's world scale; the map root is
// scaled by unityScale). Menu Castlemist > Bake Lighting and Occlusion (Unity / Bakery) bakes
// lighting (probes, reflection probes, light volumes) with Lightmapping.BakeAsync
// and then computes occlusion with StaticOcclusionCulling.GenerateInBackground.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.Rendering;
#if UNITY_POST_PROCESSING_STACK_V2 && CASTLEMIST_PPV2
using UnityEngine.Rendering.PostProcessing;
#endif
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class SceneSetup
    {
        public const string PostLayerName = "PostProcessing";
        /// The capture point of a cell's reflection probe above the ground under its centre (Unity units).
        public const float ProbeEyeHeight = 2f;

        public sealed class Context
        {
            public string Root;           // the export folder under Assets/
            public WorldManifest M;
            public ImportSummary Sum;
            public ImportOptions Opts;
            public float UnityScale;
            public GameObject MapRoot;    // GW2_<fileId>
        }

        static string F(double v) { return v.ToString("0.###", CultureInfo.InvariantCulture); }
        static Color C(double[] c) { return new Color((float)c[0], (float)c[1], (float)c[2]); }

        /// The static flags for one object of a model prefab or the scene (see the header);
        /// with occlusion off, WorldImporter.StaticFlags as before (+ ReflectionProbeStatic).
        public static StaticEditorFlags StaticFlagsFor(GameObject go, ImportOptions o)
        {
            StaticEditorFlags f = WorldImporter.StaticFlags;
            Renderer r = go.GetComponent<Renderer>();
            if (r != null && o.occlusion)
            {
                int q = MaxQueue(r);
                f = StaticEditorFlags.BatchingStatic | StaticEditorFlags.OccludeeStatic;
                if (q < 2450) f |= StaticEditorFlags.OccluderStatic;
                // Not a skinned mesh: Unity's baker skips them, but Bakery lightmaps a static one through
                // GW2's UV1 (no lightmap UV), which baked 3560849's platform discs dark (2026-10-22).
                if (q < 2500 && !(r is SkinnedMeshRenderer)) f |= StaticEditorFlags.ContributeGI;
            }
            if (r != null && o.reflectionProbes) f |= StaticEditorFlags.ReflectionProbeStatic;
            return f;
        }

        /// Sets StaticFlagsFor on go, and Light Probes GI on a contributing MeshRenderer.
        public static void ApplyStatic(GameObject go, ImportOptions o)
        {
            StaticEditorFlags f = StaticFlagsFor(go, o);
            if (GameObjectUtility.GetStaticEditorFlags(go) != f) GameObjectUtility.SetStaticEditorFlags(go, f);
            var mr = go.GetComponent<MeshRenderer>();
            if (mr != null && (f & StaticEditorFlags.ContributeGI) != 0 && mr.receiveGI != ReceiveGI.LightProbes)
                mr.receiveGI = ReceiveGI.LightProbes;
            var smr = go.GetComponent<SkinnedMeshRenderer>();
            if (smr != null) ProbesOnly(smr);
        }

        /// A renderer that takes its GI from probes / Light Volumes and never gets a lightmap, by its
        /// serialized fields (SkinnedMeshRenderer has no receiveGI API): receive GI Light Probes,
        /// scale in lightmap 0 (Bakery skips a 0-scale renderer), and no lightmap assigned now.
        public static bool ProbesOnly(Renderer r)
        {
            var so = new SerializedObject(r);
            SerializedProperty recv = so.FindProperty("m_ReceiveGI"), scale = so.FindProperty("m_ScaleInLightmap");
            bool changed = false;
            if (recv != null && recv.intValue != (int)ReceiveGI.LightProbes) { recv.intValue = (int)ReceiveGI.LightProbes; changed = true; }
            if (scale != null && scale.floatValue != 0f) { scale.floatValue = 0f; changed = true; }
            if (changed) so.ApplyModifiedPropertiesWithoutUndo();
            if (r.lightmapIndex >= 0 && r.lightmapIndex < 65534) { r.lightmapIndex = -1; changed = true; }
            return changed;
        }

        static int MaxQueue(Renderer r)
        {
            int q = 2000;
            foreach (Material m in r.sharedMaterials) if (m != null) q = Math.Max(q, m.renderQueue);
            return q;
        }

        public static void Apply(Context c)
        {
            WorldManifest.EnvironmentEntry env = c.M.Environment;
            if (ImportOptions.EnvNames.All(n => !c.Opts.Env(n))) { c.Sum.envLine = "ENV off"; return; }   // the scene as before 2026-10-22
            var line = new List<string>();
            if (env == null)
                c.Sum.warnings.Add("environment: null in the manifest (an export before 2026-10-22, or a map without an env chunk): " +
                                   "no fog, Flat ambient, post-processing without the GW2 effect values");
            string envName = "GW2_" + c.M.MapFileId + "_Environment";
            GameObject old = GameObject.Find(envName);
            if (old != null) Object.DestroyImmediate(old);
            var envRoot = new GameObject(envName);

            Bounds mapBounds;
            List<KeyValuePair<string, Bounds>> cells = CellBounds(c, out mapBounds);
            bool any = mapBounds.size.sqrMagnitude > 0;
            if (!any) c.Sum.warnings.Add("environment: the scene has no renderers: no probes, light volumes or occlusion area");

            if (env != null && c.M.Light == null && env.Lights.Count > 0 && RenderSettings.sun != null && env.Lights[0].UnityForward != null)
            {
                // The manifest's light block is null but the env chunk has a sun: use it (same frame, raw intensity).
                Light sun = RenderSettings.sun;
                Vector3 fwd = new Vector3((float)env.Lights[0].UnityForward[0], (float)env.Lights[0].UnityForward[1], (float)env.Lights[0].UnityForward[2]);
                if (fwd.sqrMagnitude > 1e-12f) sun.transform.rotation = Quaternion.LookRotation(fwd.normalized);
                sun.color = C(env.Lights[0].Color);
                sun.intensity = (float)env.Lights[0].Intensity;
                c.Sum.warnings.Add("light: null in the manifest: the sun is environment.lights[0]");
            }

            string haze = c.Opts.haze ? HazeVolume(c, env, mapBounds, any) : null;
            if (haze != null) line.Add("haze=" + haze);
            else if (c.Opts.fog) line.Add("fog=" + Fog(c, env));
            if (c.Opts.ambient) line.Add("ambient=" + Ambient(c, env));
            if (c.Opts.occlusion) line.Add("static=" + Occlusion(c, envRoot, mapBounds, any));
            else if (c.Opts.reflectionProbes) Flags(c);
            if (c.Opts.reflectionProbes) line.Add("reflectionProbes=" + Reflections(c, envRoot, cells, mapBounds));
            if (c.Opts.lightVolumes) line.Add("lightVolumes=" + LightVolumes(c, envRoot, cells));
            if (c.Opts.lightVolumes || c.Opts.reflectionProbes || c.Opts.occlusion) Lighting(c);
            if (c.Opts.post) line.Add("post=" + Post(c, env, envRoot, mapBounds));
            if (env != null && env.Zones.Count > 0)
                c.Sum.warnings.Add("environment: " + env.Zones.Count + " zone haze override(s) not mapped (Unity fog is one global setting)");
            if (env != null && env.Backlight != null && env.Backlight.Intensity > 0)
                c.Sum.warnings.Add("environment: backlight (intensity " + F(env.Backlight.Intensity) + ") not mapped");
            c.Sum.envLine = "ENV " + (line.Count == 0 ? "off" : string.Join(" ", line));
        }

        // ---- bounds ----------------------------------------------------------------------------

        static bool Skip(Renderer r)
        {
            if (r is ParticleSystemRenderer || r is TrailRenderer || r is LineRenderer) return true;
            if (!r.enabled || !r.gameObject.activeInHierarchy) return true;
            return false;
        }

        /// Each Cell_<id>'s renderer bounds (Unity Terrain tiles added where they overlap the
        /// cell's XZ), world space; mapBounds their union (size 0 when there is nothing).
        static List<KeyValuePair<string, Bounds>> CellBounds(Context c, out Bounds mapBounds)
        {
            var res = new List<KeyValuePair<string, Bounds>>();
            mapBounds = new Bounds();
            bool anyMap = false;
            var terrains = Object.FindObjectsOfType<Terrain>().Where(t => t.terrainData != null)
                .Select(t => new Bounds(t.transform.position + t.terrainData.size * 0.5f, t.terrainData.size)).ToList();
            foreach (Transform cell in c.MapRoot.transform)
            {
                if (!cell.name.StartsWith("Cell_", StringComparison.Ordinal)) continue;
                Bounds b = new Bounds();
                bool anyCell = false;
                foreach (Renderer r in cell.GetComponentsInChildren<Renderer>())
                {
                    if (Skip(r)) continue;
                    if (!anyCell) { b = r.bounds; anyCell = true; } else b.Encapsulate(r.bounds);
                }
                if (anyCell)
                    foreach (Bounds t in terrains)
                    {
                        if (t.max.x < b.min.x || t.min.x > b.max.x || t.max.z < b.min.z || t.min.z > b.max.z) continue;
                        b.Encapsulate(new Vector3(b.center.x, t.min.y, b.center.z));
                        b.Encapsulate(new Vector3(b.center.x, t.max.y, b.center.z));
                    }
                if (!anyCell) continue;
                res.Add(new KeyValuePair<string, Bounds>(cell.name.Substring(5), b));
                if (!anyMap) { mapBounds = b; anyMap = true; } else mapBounds.Encapsulate(b);
            }
            foreach (Bounds t in terrains)
            {
                if (!anyMap) { mapBounds = t; anyMap = true; } else mapBounds.Encapsulate(t);
            }
            return res;
        }

        /// The haze volume, or null (then Unity fog is the fallback).
        static string HazeVolume(Context c, WorldManifest.EnvironmentEntry env, Bounds map, bool any)
        {
            WorldManifest.HazeEntry h = env != null ? env.Haze : null;
            if (h == null || h.DistRange == null || !any) return null;
            Shader sh = Shader.Find("Castlemist/GW2 Haze");
            if (sh == null)
            {
                c.Sum.warnings.Add("haze: shader Castlemist/GW2 Haze not found (copy tools/unity/Castlemist/Shaders): Unity fog instead");
                return null;
            }
            string path = c.Root + "/GW2_" + c.M.MapFileId + "_Haze.mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
            if (mat == null) { mat = new Material(sh) { name = "GW2_" + c.M.MapFileId + "_Haze" }; AssetDatabase.CreateAsset(mat, path); }
            else mat.shader = sh;
            double[] near = h.Near ?? h.Far, far = h.Far ?? h.Near;
            Func<double[], Color> C4 = v => v == null ? new Color(0, 0, 0, 0) : new Color((float)v[0], (float)v[1], (float)v[2], (float)v[3]);
            mat.SetColor("_HazeNear", C4(near));
            mat.SetColor("_HazeFar", C4(far));
            mat.SetColor("_HazeHeight", C4(h.Height));
            mat.SetVector("_DistRange", new Vector4((float)h.DistRange[0], (float)h.DistRange[1], 0, 0));
            bool fromUnity;
            double[] hr = EnvironmentMath.HazeHeightRange(h, out fromUnity);
            if (hr == null) { hr = new double[] { 0, -1 }; mat.SetColor("_HazeHeight", new Color(0, 0, 0, 0)); c.Sum.warnings.Add("haze: no heightRange: height haze off"); }
            else if (fromUnity) c.Sum.warnings.Add("haze: heightRange (raw) absent: (-hi, -lo) of heightRangeUnity used (UNPROVEN order)");
            mat.SetVector("_HeightRange", new Vector4((float)hr[0], (float)hr[1], 0, 0));
            mat.SetFloat("_DepthCue", (float)h.DepthCue);
            mat.SetFloat("_UnityScale", c.UnityScale);
            mat.SetFloat("_MapOriginY", c.MapRoot.transform.position.y);
            mat.SetFloat("_ApplySkybox", 1f);
            EditorUtility.SetDirty(mat);

            // A cube well beyond the map (the camera must stay inside it: the shader draws its back
            // faces, ZTest Always), under the map root: world size -> local size / unityScale.
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = "GW2 Haze";
            Object.DestroyImmediate(go.GetComponent<Collider>());
            go.transform.SetParent(c.MapRoot.transform, false);
            go.transform.position = map.center;
            float side = Mathf.Max(map.size.x, Mathf.Max(map.size.y, map.size.z)) * 2f + 200f;
            go.transform.localScale = Vector3.one * (side / Mathf.Max(c.UnityScale, 1e-6f));
            go.layer = 0;
            var r = go.GetComponent<MeshRenderer>();
            r.sharedMaterial = mat;
            r.shadowCastingMode = ShadowCastingMode.Off;
            r.receiveShadows = false;
            r.lightProbeUsage = LightProbeUsage.Off;
            r.reflectionProbeUsage = ReflectionProbeUsage.Off;
            r.allowOcclusionWhenDynamic = false;
            GameObjectUtility.SetStaticEditorFlags(go, 0);
            RenderSettings.fog = false;
            c.Sum.warnings.Add("haze: GW2 Haze volume (" + F(side) + " units; distRange " + F(h.DistRange[0]) + ".." + F(h.DistRange[1]) +
                               " GW2 units): Unity fog off; needs the camera depth texture (the sun's shadows in VRChat); " +
                               "transparents after queue Transparent-216 are not fogged by it");
            return "volume";
        }

        // ---- fog and ambient ---------------------------------------------------------------------

        static string Fog(Context c, WorldManifest.EnvironmentEntry env)
        {
            EnvironmentMath.Fog fog = env != null ? EnvironmentMath.FogFromHaze(env.Haze, c.UnityScale) : null;
            if (fog == null)
            {
                RenderSettings.fog = false;
                if (env != null) c.Sum.warnings.Add("fog: environment.haze has no usable distRange: no fog");
                return "off";
            }
            RenderSettings.fog = true;
            RenderSettings.fogMode = FogMode.Linear;
            RenderSettings.fogColor = C(fog.Color);
            RenderSettings.fogStartDistance = (float)fog.Start;
            RenderSettings.fogEndDistance = (float)fog.End;
            c.Sum.warnings.Add("fog: Linear " + F(fog.Start) + ".." + F(fog.End) + " (UNPROVEN reading of haze distRange and alphas; " +
                               "height haze not mapped)");
            return "linear:" + F(fog.Start) + ".." + F(fog.End);
        }

        static string Ambient(Context c, WorldManifest.EnvironmentEntry env)
        {
            // The fills as GW2 shades them, each by N.L (the summed flat ambient lit every surface by
            // all of them: ~3x the game's level on the Mists vault, 2026-10-22).
            double[][] tri = env != null ? EnvironmentMath.TrilightFromFills(env.Lights) : null;
            if (tri == null)
            {
                if (env != null) c.Sum.warnings.Add("ambient: environment.lights has no fill with a direction: Flat light.ambient as before");
                return RenderSettings.ambientMode == AmbientMode.Flat ? "flat" : RenderSettings.ambientMode.ToString().ToLowerInvariant();
            }
            RenderSettings.ambientMode = AmbientMode.Trilight;
            RenderSettings.ambientSkyColor = C(tri[0]);
            RenderSettings.ambientEquatorColor = C(tri[1]);
            RenderSettings.ambientGroundColor = C(tri[2]);
            DynamicGI.UpdateEnvironment();   // the ambient probe from the Trilight now, not at the next bake
            c.Sum.warnings.Add("ambient: Trilight from " + (env.Lights.Count - 1) + " fill(s) (UNPROVEN: Lambert of the fills as unshadowed lights)");
            return "trilight";
        }

        // ---- static flags and occlusion -----------------------------------------------------------

        static bool Animated(Transform t) { return t.GetComponentInParent<Animator>(true) != null; }

        static bool UnderNamed(Transform t, Transform stop, string name)
        {
            for (Transform p = t; p != null && p != stop; p = p.parent) if (p.name == name) return true;
            return false;
        }

        /// The static flags pass over the map's renderers; returns "opaque/other/left".
        static string Flags(Context c)
        {
            int opaque = 0, other = 0, left = 0;
            var roots = new List<Transform> { c.MapRoot.transform };
            foreach (Terrain t in Object.FindObjectsOfType<Terrain>()) if (!roots.Contains(t.transform.root)) roots.Add(t.transform.root);
            foreach (Transform root in roots)
                foreach (Renderer r in root.GetComponentsInChildren<Renderer>(true))
                {
                    Transform t = r.transform;
                    if (r is ParticleSystemRenderer || r is SkinnedMeshRenderer || r is TrailRenderer || r is LineRenderer ||
                        Animated(t) || UnderNamed(t, root, "Foliage") || UnderNamed(t, root, "FrameProbe")) { ++left; continue; }
                    ApplyStatic(r.gameObject, c.Opts);
                    if ((GameObjectUtility.GetStaticEditorFlags(r.gameObject) & StaticEditorFlags.OccluderStatic) != 0) ++opaque; else ++other;
                }
            foreach (Terrain t in Object.FindObjectsOfType<Terrain>())
            {
                // Unity Terrain tiles: occluder and occludee; no ContributeGI (a Terrain would be lightmapped).
                StaticEditorFlags f = WorldImporter.StaticFlags | (c.Opts.reflectionProbes ? StaticEditorFlags.ReflectionProbeStatic : 0);
                if (GameObjectUtility.GetStaticEditorFlags(t.gameObject) != f) GameObjectUtility.SetStaticEditorFlags(t.gameObject, f);
            }
            return opaque + "/" + other + "/" + left;
        }

        static string Occlusion(Context c, GameObject envRoot, Bounds map, bool any)
        {
            string s = Flags(c);
            if (any)
            {
                var go = new GameObject("OcclusionArea");
                go.transform.SetParent(envRoot.transform, false);
                go.transform.position = map.center;
                var oa = go.AddComponent<OcclusionArea>();
                oa.center = Vector3.zero;
                oa.size = map.size + Vector3.one * 2f;
            }
            c.Sum.warnings.Add("occlusion: static flags set (opaque/other/left alone " + s + "); occlusion data needs " +
                               "Castlemist > Bake Lighting and Occlusion (Unity / Bakery)");
            return s;
        }

        // ---- reflection probes ---------------------------------------------------------------------

        static string Reflections(Context c, GameObject envRoot, List<KeyValuePair<string, Bounds>> cells, Bounds map)
        {
            RenderSettings.defaultReflectionMode = DefaultReflectionMode.Skybox;
            RenderSettings.defaultReflectionResolution = c.Opts.reflectionResolution;
            if (cells.Count == 0) return "0";
            var parent = new GameObject("ReflectionProbes");
            parent.transform.SetParent(envRoot.transform, false);
            Physics.SyncTransforms();
            float far = Mathf.Max(100f, map.size.magnitude * 1.1f);
            foreach (KeyValuePair<string, Bounds> kv in cells)
            {
                Bounds b = kv.Value;
                var go = new GameObject("ReflectionProbe_" + kv.Key);
                go.transform.SetParent(parent.transform, false);
                // Capture from just above the ground under the cell's centre (a raycast on the
                // terrain / prop colliders), not from inside a hill at the bounds' centre.
                Vector3 eye = b.center;
                RaycastHit hit;
                if (Physics.Raycast(new Vector3(b.center.x, b.max.y + 1f, b.center.z), Vector3.down, out hit, b.size.y + 2f))
                    eye = hit.point + Vector3.up * ProbeEyeHeight;
                go.transform.position = eye;
                var p = go.AddComponent<ReflectionProbe>();
                p.mode = ReflectionProbeMode.Baked;
                p.boxProjection = true;
                p.center = b.center - eye;
                p.size = b.size;
                p.resolution = c.Opts.reflectionResolution;
                p.hdr = true;
                p.blendDistance = 4f;
                p.nearClipPlane = 0.1f;
                p.farClipPlane = far;
                p.clearFlags = ReflectionProbeClearFlags.Skybox;
                GameObjectUtility.SetStaticEditorFlags(go, StaticEditorFlags.ReflectionProbeStatic);
            }
            c.Sum.warnings.Add("reflection probes: " + cells.Count + " baked box probe(s) at " + c.Opts.reflectionResolution +
                               " px: they render black / the sky until Castlemist > Bake Lighting and Occlusion (Unity / Bakery)");
            return cells.Count.ToString(CultureInfo.InvariantCulture);
        }

        // ---- lighting settings -----------------------------------------------------------------------

        static void Lighting(Context c)
        {
            string path = c.Root + "/GW2_" + c.M.MapFileId + "_Lighting.lighting";
            var ls = AssetDatabase.LoadAssetAtPath<LightingSettings>(path);
            if (ls == null)
            {
                ls = new LightingSettings { name = "GW2_" + c.M.MapFileId + "_Lighting" };
                AssetDatabase.CreateAsset(ls, path);
            }
            ls.bakedGI = true;
            ls.realtimeGI = false;
            ls.autoGenerate = false;
            ls.lightmapper = LightingSettings.Lightmapper.ProgressiveGPU;
            ls.mixedBakeMode = MixedLightingMode.IndirectOnly;
            ls.lightmapResolution = 2f;   // nothing is lightmapped (Light Probes GI); kept small
            ls.lightmapPadding = 2;
            EditorUtility.SetDirty(ls);
            Lightmapping.lightingSettings = ls;
            if (c.Opts.lightVolumes && RenderSettings.sun != null) RenderSettings.sun.lightmapBakeType = LightmapBakeType.Mixed;
        }

        // ---- VRC Light Volumes (reflection: the package is optional) ----------------------------------

        internal static Type FindType(string fullName)
        {
            foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
            {
                Type t = a.GetType(fullName, false);
                if (t != null) return t;
            }
            return null;
        }

        internal static void SetField(object o, string name, object v)
        {
            FieldInfo f = o.GetType().GetField(name, BindingFlags.Public | BindingFlags.Instance);
            if (f != null) f.SetValue(o, v);
        }

        internal static Component AddUdonAware(GameObject go, Type t)
        {
            // In a VRChat world (UdonSharp) LightVolumeInstance is an UdonSharpBehaviour: add it the
            // UdonSharp way (a proxy and its backing UdonBehaviour), as the package does.
            Type undo = FindType("UdonSharpEditor.UdonSharpUndo");
            Type usb = FindType("UdonSharp.UdonSharpBehaviour");
            if (undo != null && usb != null && usb.IsAssignableFrom(t))
            {
                MethodInfo add = undo.GetMethod("AddComponent", BindingFlags.Public | BindingFlags.Static, null, new[] { typeof(GameObject), typeof(Type) }, null);
                if (add != null) return (Component)add.Invoke(null, new object[] { go, t });
            }
            return go.AddComponent(t);
        }

        internal static void CopyProxyToUdon(Component c)
        {
            Type u = FindType("UdonSharpEditor.UdonSharpEditorUtility");
            if (u == null) return;
            MethodInfo m = u.GetMethods(BindingFlags.Public | BindingFlags.Static)
                .FirstOrDefault(x => x.Name == "CopyProxyToUdon" && x.GetParameters().Length == 1);
            if (m != null) m.Invoke(null, new object[] { c });
        }

        static string LightVolumes(Context c, GameObject envRoot, List<KeyValuePair<string, Bounds>> cells)
        {
            Type lv = FindType("VRCLightVolumes.LightVolumeInstance");
            if (lv == null)
            {
                c.Sum.warnings.Add("light volumes: VRC Light Volumes (red.sim.lightvolumes) is not in the project: none placed");
                return "absent";
            }
            if (cells.Count == 0) return "0";
            var parent = new GameObject("LightVolumes");
            parent.transform.SetParent(envRoot.transform, false);
            Type tools = FindType("VRCLightVolumes.LightVolumeTools");
            long voxels = 0;
            foreach (KeyValuePair<string, Bounds> kv in cells)
            {
                Bounds b = kv.Value;
                var go = new GameObject("LightVolume_" + kv.Key);
                go.transform.SetParent(parent.transform, false);
                go.transform.position = b.center;
                go.transform.localScale = b.size;
                double vpu = EnvironmentMath.VoxelsPerUnit(b.size.x, b.size.y, b.size.z, c.Opts.lightVolumeBudget, 1.0);
                Component comp = AddUdonAware(go, lv);
                SetField(comp, "AdaptiveResolution", true);
                SetField(comp, "VoxelsPerUnit", (float)vpu);
                SetField(comp, "Bake", true);
                SetField(comp, "IsDynamic", false);
                SetField(comp, "SmoothBlending", 0.25f);
                voxels += (long)(Math.Ceiling(b.size.x * vpu) * Math.Ceiling(b.size.y * vpu) * Math.Ceiling(b.size.z * vpu));
                try
                {
                    MethodInfo rec = tools != null ? tools.GetMethod("Recalculate", BindingFlags.Public | BindingFlags.Static, null, new[] { lv }, null) : null;
                    if (rec != null) rec.Invoke(null, new object[] { comp });
                }
                catch (Exception e) { c.Sum.warnings.Add("light volumes: LightVolumeTools.Recalculate failed: " + (e.InnerException ?? e).Message); }
                CopyProxyToUdon(comp);
            }
            // The package's own onboarding (internal; what its editor updater runs when a volume
            // appears): migrate, create the scene's LightVolumeManager, register the volumes.
            string manager = "pending";
            try
            {
                Type setup = FindType("VRCLightVolumes.LightVolumeSceneSetup");
                MethodInfo onboard = setup != null ? setup.GetMethods(BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Static)
                    .FirstOrDefault(x => x.Name == "OnboardHierarchy" && x.GetParameters().Length == 3) : null;
                if (onboard != null)
                {
                    var args = new object[] { parent, null, false };
                    onboard.Invoke(null, args);
                    manager = args[1] != null ? "registered" : "pending";
                }
            }
            catch (Exception e) { c.Sum.warnings.Add("light volumes: onboarding failed: " + (e.InnerException ?? e).Message); }
            if (manager == "pending")
                c.Sum.warnings.Add("light volumes: no LightVolumeManager yet: the package adds one when the scene is next edited or opened");
            c.Sum.warnings.Add("light volumes: " + cells.Count + " volume(s), ~" + voxels + " voxels (manager " + manager +
                               "): unlit until Castlemist > Bake Lighting and Occlusion (Unity / Bakery)");
            return cells.Count + ":" + manager;
        }

        // ---- reference camera ---------------------------------------------------------------------------

        /// The VRC scene descriptor (VRChat SDK3, by reflection) or null.
        static Component Descriptor()
        {
#if VRC_SDK_VRCSDK3
            Type t = FindType("VRC.SDKBase.VRC_SceneDescriptor");
            if (t != null) return Object.FindObjectOfType(t) as Component;
#endif
            return null;
        }

        static Camera ReferenceCamera(Context c, GameObject envRoot, Bounds map)
        {
            Component desc = Descriptor();
            FieldInfo refField = desc != null ? desc.GetType().GetField("ReferenceCamera") : null;
            if (refField != null)
            {
                var go = refField.GetValue(desc) as GameObject;
                if (go != null && go.GetComponent<Camera>() != null) return go.GetComponent<Camera>();
            }
            bool vrc = false;
#if VRC_SDK_VRCSDK3
            vrc = true;
#endif
            if (!vrc && Camera.main != null) return Camera.main;
            var camGo = new GameObject(vrc ? "ReferenceCamera" : "Main Camera");
            camGo.transform.SetParent(envRoot.transform, false);
            if (c.M.SpawnUnity != null)
                camGo.transform.position = c.MapRoot.transform.TransformPoint(new Vector3((float)c.M.SpawnUnity[0], (float)c.M.SpawnUnity[1], (float)c.M.SpawnUnity[2])) + Vector3.up * 1.7f;
            Camera cam = camGo.AddComponent<Camera>();
            cam.nearClipPlane = 0.05f;
            // Far enough for the haze volume's back faces (2x the map's largest side + 200, seen from the map).
            cam.farClipPlane = Mathf.Max(1000f, Mathf.Max(map.size.x, Mathf.Max(map.size.y, map.size.z)) * 3f + 400f);
            cam.allowHDR = true;
            if (vrc)
            {
                cam.enabled = false;   // VRChat copies its settings and components; it renders nothing itself
                if (refField != null) { refField.SetValue(desc, camGo); EditorUtility.SetDirty(desc); }
            }
            else camGo.tag = "MainCamera";
            return cam;
        }

        /// Links GW2_<id>_Environment/ReferenceCamera to a VRC scene descriptor that has no reference
        /// camera (the descriptor is added by hand after the import; CHECKLIST 11).
        public static void LinkReferenceCamera()
        {
            Component desc = Descriptor();
            FieldInfo refField = desc != null ? desc.GetType().GetField("ReferenceCamera") : null;
            if (refField == null || refField.GetValue(desc) as GameObject != null) return;
            Camera cam = Object.FindObjectsOfType<Camera>(true).FirstOrDefault(x => x.name == "ReferenceCamera");
            if (cam == null) return;
            refField.SetValue(desc, cam.gameObject);
            EditorUtility.SetDirty(desc);
            Debug.Log("CASTLEMIST the VRC scene descriptor's Reference Camera is now " + cam.name);
        }

        // ---- post-processing ----------------------------------------------------------------------------

        /// The "PostProcessing" layer, made in the first free user layer (22..31, then 8..21 in a
        /// project without VRChat's layers); -1 when none is free.
        public static int PostLayer()
        {
            int l = LayerMask.NameToLayer(PostLayerName);
            if (l >= 0) return l;
            Object[] tmAssets = AssetDatabase.LoadAllAssetsAtPath("ProjectSettings/TagManager.asset");
            if (tmAssets == null || tmAssets.Length == 0) return -1;
            var tm = new SerializedObject(tmAssets[0]);
            SerializedProperty layers = tm.FindProperty("layers");
            if (layers == null) return -1;
            bool vrcLayers = LayerMask.NameToLayer("PlayerLocal") >= 0 || LayerMask.NameToLayer("MirrorReflection") >= 0;
            var order = Enumerable.Range(22, 10).Concat(vrcLayers ? Enumerable.Empty<int>() : Enumerable.Range(8, 14));
            foreach (int i in order)
            {
                if (i >= layers.arraySize) continue;
                SerializedProperty p = layers.GetArrayElementAtIndex(i);
                if (!string.IsNullOrEmpty(p.stringValue)) continue;
                p.stringValue = PostLayerName;
                tm.ApplyModifiedPropertiesWithoutUndo();
                return i;
            }
            return -1;
        }

        static string Post(Context c, WorldManifest.EnvironmentEntry env, GameObject envRoot, Bounds map)
        {
#if UNITY_POST_PROCESSING_STACK_V2 && CASTLEMIST_PPV2
            WorldManifest.EffectEntry fx = env != null ? env.Effect : null;
            string path = c.Root + "/GW2_" + c.M.MapFileId + "_PostProcess.asset";
            var profile = AssetDatabase.LoadAssetAtPath<PostProcessProfile>(path);
            if (profile == null)
            {
                profile = ScriptableObject.CreateInstance<PostProcessProfile>();
                AssetDatabase.CreateAsset(profile, path);
            }
            var parts = new List<string>();

            // Bloom (UNPROVEN: EnvironmentMath.BloomFromEffect).
            EnvironmentMath.Bloom bl = EnvironmentMath.BloomFromEffect(fx);
            Bloom bloom = Effect<Bloom>(profile);
            bloom.enabled.Override(bl != null);
            bloom.fastMode.Override(true);   // VR: the 4-tap filter, half the bloom passes' cost
            if (bl != null)
            {
                bloom.intensity.Override((float)bl.Intensity);
                bloom.threshold.Override((float)bl.Threshold);
                bloom.color.Override(C(bl.Color));
                parts.Add("bloom:" + F(bl.Intensity) + "@" + F(bl.Threshold));
            }

            // Colour grading, LDR (GW2's grading is a LDR strip; UNPROVEN): saturation, colour filter, LUT.
            ColorGrading cg = Effect<ColorGrading>(profile);
            cg.enabled.Override(true);
            cg.gradingMode.Override(GradingMode.LowDefinitionRange);
            double sat = fx != null && fx.Saturation != null ? EnvironmentMath.Saturation(fx.Saturation.Value) : 0;
            cg.saturation.Override((float)sat);
            double[] filt = EnvironmentMath.ColorFilter(fx != null ? fx.TintColor : null, fx != null ? fx.TintAmount : null);
            cg.colorFilter.Override(C(filt));
            Texture2D lut = fx != null && fx.Clut != null ? Lut(c, fx) : null;
            cg.ldrLut.Override(lut);
            cg.ldrLutContribution.Override(1f);
            parts.Add("grading:sat" + F(sat) + (lut != null ? "+lut" : ""));

            // Ambient occlusion, MSVO (UNPROVEN: ssaoAmount as the intensity), left off: full-resolution
            // MSVO per eye cost the Wizard Vault most of its VR frame (2026-10-23). The intensity is
            // kept for a desktop world to turn on.
            double ao = EnvironmentMath.AoIntensity(fx != null ? fx.SsaoAmount : null);
            AmbientOcclusion aoS = Effect<AmbientOcclusion>(profile);
            aoS.enabled.Override(false);
            aoS.mode.Override(AmbientOcclusionMode.MultiScaleVolumetricObscurance);
            aoS.intensity.Override((float)ao);
            if (ao > 0) parts.Add("ao:" + F(ao) + " off (VR)");

            // No depth of field in VR (an earlier profile's is turned off).
            DepthOfField dof;
            if (profile.TryGetSettings(out dof)) dof.enabled.Override(false);
            EditorUtility.SetDirty(profile);

            int layer = PostLayer();
            if (layer < 0) c.Sum.warnings.Add("post: no free layer for '" + PostLayerName + "': the volume is on Default");
            var vol = new GameObject("PostProcessVolume");
            vol.transform.SetParent(envRoot.transform, false);
            vol.layer = Math.Max(0, layer);
            var v = vol.AddComponent<PostProcessVolume>();
            v.isGlobal = true;
            v.priority = 0f;
            v.weight = 1f;
            v.sharedProfile = profile;

            Camera cam = ReferenceCamera(c, envRoot, map);
            var ppl = cam.GetComponent<PostProcessLayer>();
            if (ppl == null) ppl = cam.gameObject.AddComponent<PostProcessLayer>();
            var res = AssetDatabase.LoadAssetAtPath<PostProcessResources>("Packages/com.unity.postprocessing/PostProcessing/PostProcessResources.asset");
            if (res == null)
            {
                string g = AssetDatabase.FindAssets("t:PostProcessResources").FirstOrDefault();
                if (g != null) res = AssetDatabase.LoadAssetAtPath<PostProcessResources>(AssetDatabase.GUIDToAssetPath(g));
            }
            if (res == null) c.Sum.errors.Add("post: PostProcessResources asset not found: the PostProcessLayer will not render");
            ppl.Init(res);
            ppl.volumeTrigger = cam.transform;
            ppl.volumeLayer = 1 << Math.Max(0, layer);
            ppl.antialiasingMode = PostProcessLayer.Antialiasing.None;
            EditorUtility.SetDirty(ppl);
            c.Sum.warnings.Add("post: " + (parts.Count > 0 ? string.Join(", ", parts) : "neutral") + " on " + cam.name +
                               " (UNPROVEN mappings of environment.effect; no depth of field)" + (fx == null ? "; environment.effect is null: neutral values" : ""));
            return string.Join(",", parts);
#else
            c.Sum.warnings.Add("post: Post Processing Stack v2 (com.unity.postprocessing) is not in the project: no post-processing");
            return "absent";
#endif
        }

#if UNITY_POST_PROCESSING_STACK_V2 && CASTLEMIST_PPV2
        static T Effect<T>(PostProcessProfile p) where T : PostProcessEffectSettings
        {
            T s;
            if (p.TryGetSettings(out s)) return s;
            s = p.AddSettings<T>();
            AssetDatabase.AddObjectToAsset(s, p);
            return s;
        }

        /// effect.clut as a PPv2 LDR lookup strip: imported linear, uncompressed, no mips, clamp,
        /// bilinear (PPv2's own LUT check); null with a warning when it is not width = height^2.
        static Texture2D Lut(Context c, WorldManifest.EffectEntry fx)
        {
            string p = c.Root + "/" + fx.Clut.Replace('\\', '/');
            var ti = AssetImporter.GetAtPath(p) as TextureImporter;
            if (ti == null) { c.Sum.warnings.Add("post: effect.clut " + p + " is not an imported texture: no LUT"); return null; }
            if (CastlemistPostprocessor.ApplyLutSettings(ti)) ti.SaveAndReimport();
            var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(p);
            if (tex == null || !EnvironmentMath.IsLutStrip(fx.ClutLayout, tex.width, tex.height))
            {
                c.Sum.warnings.Add("post: effect.clut " + p + " is not a PPv2 lookup strip (width = height^2" +
                                   (fx.ClutLayout != null ? ", layout " + fx.ClutLayout.Kind : "") + "): no LUT");
                return null;
            }
            c.Sum.warnings.Add("post: LUT " + p + " (" + tex.width + "x" + tex.height + "; UNPROVEN: GW2's strip read with PPv2's slice order and v up)");
            return tex;
        }
#endif

        // ---- bake ----------------------------------------------------------------------------------------

        [MenuItem("Castlemist/Bake Lighting and Occlusion (Unity)")]
        public static void BakeMenu()
        {
            if (Lightmapping.isRunning || StaticOcclusionCulling.isRunning) { Debug.LogWarning("CASTLEMIST a bake is already running"); return; }
            LinkReferenceCamera();
            SetLightVolumeBakingMode(0);
            Lightmapping.bakeCompleted -= AfterLighting;
            Lightmapping.bakeCompleted += AfterLighting;
            if (!Lightmapping.BakeAsync())
            {
                Lightmapping.bakeCompleted -= AfterLighting;
                Debug.LogError("CASTLEMIST Lightmapping.BakeAsync did not start (save the scene first)");
                return;
            }
            Debug.Log("CASTLEMIST baking lighting (probes, reflection probes, light volumes); occlusion follows");
        }

        static void AfterLighting()
        {
            Lightmapping.bakeCompleted -= AfterLighting;
            // Occlusion after lighting: both are long editor jobs, and occlusion only needs the static flags.
            if (StaticOcclusionCulling.GenerateInBackground()) Debug.Log("CASTLEMIST lighting baked; computing occlusion");
            else Debug.LogError("CASTLEMIST StaticOcclusionCulling.GenerateInBackground did not start");
        }

        // ---- light volume fog (2026-10-22) -------------------------------------------------------------------
        // BakedVolumetrics' VRC Light Volumes fog (Assets/BakedVolumetrics/VRC/VolumetricFog.prefab:
        // BakedVolumetrics/SceneVolumetricFog_VRCLV, a ray-marched box lit by the Light Volumes, sized by
        // its VRCFogVolume from the transform's scale). One box per Light Volume of the open scene,
        // limited to GW2's height haze: below the haze's top (heightRange's smaller map z, h0: the
        // haze grows with z, i.e. downward), read from the scene's GW2 Haze material, so the lit fog
        // pools where GW2 draws its height haze (the Wizard Vault: the void under the walkways). Without
        // a GW2 Haze, the lower third of each volume. Rerunning replaces the boxes.

        const string LightVolumeFogName = "LightVolumeFog";

        [MenuItem("Castlemist/Add Light Volume Fog")]
        public static void AddLightVolumeFogMenu()
        {
            string msg = AddLightVolumeFog();
            Debug.Log("CASTLEMIST " + msg);
        }

        /// The fog shader's sample count (its KeywordEnum _Samples: 0 = 8, 1 = 16, 2 = 24 ...). Every box
        /// the view crosses marches this many samples per pixel; the boxes span hundreds of metres and
        /// overlap, so 8 (jittered, as the shader animates) keeps it affordable in VR (2026-10-23).
        static void SetFogSamples(Material m, int index)
        {
            if (!m.HasProperty("_Samples")) return;
            string[] keys = { "_SAMPLES__8", "_SAMPLES__16", "_SAMPLES__24", "_SAMPLES__32", "_SAMPLES__48", "_SAMPLES__64", "_SAMPLES__128" };
            m.SetFloat("_Samples", index);
            for (int i = 0; i < keys.Length; ++i)
                if (i == index) m.EnableKeyword(keys[i]); else m.DisableKeyword(keys[i]);
        }

        public static string AddLightVolumeFog()
        {
            GameObject prefab = AssetDatabase.FindAssets("VolumetricFog t:Prefab")
                .Select(g => AssetDatabase.LoadAssetAtPath<GameObject>(AssetDatabase.GUIDToAssetPath(g)))
                .FirstOrDefault(p => p != null && p.GetComponents<Component>().Any(c => c != null && c.GetType().Name == "VRCFogVolume"));
            if (prefab == null) return "light volume fog: no VolumetricFog prefab with a VRCFogVolume (import BakedVolumetrics' VRC folder)";
            Type lvt = FindType("VRCLightVolumes.LightVolumeInstance");
            if (lvt == null) return "light volume fog: VRC Light Volumes is not in the project";
            var volumes = Object.FindObjectsByType(lvt, FindObjectsInactive.Exclude, FindObjectsSortMode.None)
                                .Cast<Component>().OrderBy(v => v.name).ToList();
            if (volumes.Count == 0) return "light volume fog: no Light Volumes in the open scene (import with Light Volumes on)";

            // GW2's height haze top, in world Y.
            float? top = null;
            var haze = Object.FindObjectsByType<Renderer>(FindObjectsInactive.Include, FindObjectsSortMode.None)
                             .FirstOrDefault(r => r.sharedMaterial != null && r.sharedMaterial.shader != null &&
                                                  r.sharedMaterial.shader.name == "Castlemist/GW2 Haze");
            if (haze != null)
            {
                Material hm = haze.sharedMaterial;
                Vector4 hr = hm.GetVector("_HeightRange");
                float s = hm.GetFloat("_UnityScale"), oy = hm.GetFloat("_MapOriginY");
                if (s > 0 && hm.GetColor("_HazeHeight").a > 0) top = oy - Mathf.Min(hr.x, hr.y) * s;
            }

            // Under the scene's environment root (else the first Light Volume's parent).
            GameObject envRoot = UnityEngine.SceneManagement.SceneManager.GetActiveScene().GetRootGameObjects()
                                     .FirstOrDefault(g => g.name.EndsWith("_Environment"));
            Transform parentOf = envRoot != null ? envRoot.transform : volumes[0].transform.parent;
            Transform old = parentOf != null ? parentOf.Find(LightVolumeFogName) : null;
            if (old != null) Undo.DestroyObjectImmediate(old.gameObject);
            var group = new GameObject(LightVolumeFogName);
            Undo.RegisterCreatedObjectUndo(group, "Add Light Volume Fog");
            if (parentOf != null) group.transform.SetParent(parentOf, false);

            // The fog's own material, per scene: the shader is additive and runs every ray to full
            // opacity, so it adds ~ L0 x _LightIntensity - _LightThreshold to the screen. The sample
            // material's 3.76 / 0.79 on the Mists vaults' bright warm volumes (L0 ~1) was an orange
            // glare (2026-10-22); 0.3 / 0 is a soft lit mist. Made once, then left to the user's tuning.
            Material fogMat = null;
            var srcRenderer = prefab.GetComponent<Renderer>();
            string scenePath = UnityEngine.SceneManagement.SceneManager.GetActiveScene().path;
            if (srcRenderer != null && srcRenderer.sharedMaterial != null && !string.IsNullOrEmpty(scenePath))
            {
                string matPath = System.IO.Path.ChangeExtension(scenePath, null) + "_LightVolumeFog.mat";
                fogMat = AssetDatabase.LoadAssetAtPath<Material>(matPath);
                if (fogMat == null)
                {
                    fogMat = new Material(srcRenderer.sharedMaterial);
                    if (fogMat.HasProperty("_LightIntensity")) fogMat.SetFloat("_LightIntensity", 0.3f);
                    if (fogMat.HasProperty("_LightThreshold")) fogMat.SetFloat("_LightThreshold", 0f);
                    SetFogSamples(fogMat, 0);
                    AssetDatabase.CreateAsset(fogMat, matPath);
                }
            }

            int made = 0, skipped = 0;
            foreach (Component v in volumes)
            {
                if (v.transform.parent != null && v.transform.parent.name == "Detail") continue;   // ImmersiveLighting's fine volumes
                Transform t = v.transform;
                Vector3 c = t.position, size = t.lossyScale;   // a Light Volume is a unit box scaled by its transform
                float lo = c.y - Mathf.Abs(size.y) * 0.5f, hi = c.y + Mathf.Abs(size.y) * 0.5f;
                float cap = top ?? lo + (hi - lo) / 3f;
                if (cap <= lo + 0.5f) { ++skipped; continue; }   // the volume is above the haze
                hi = Mathf.Min(hi, cap);
                var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
                go.name = "Fog_" + v.name;
                go.transform.SetParent(group.transform, false);
                go.transform.position = new Vector3(c.x, (lo + hi) * 0.5f, c.z);
                go.transform.rotation = Quaternion.identity;   // VRCFogVolume keeps it axis-aligned
                go.transform.localScale = new Vector3(Mathf.Abs(size.x), hi - lo, Mathf.Abs(size.z));
                if (fogMat != null) go.GetComponent<Renderer>().sharedMaterial = fogMat;
                foreach (MonoBehaviour b in go.GetComponents<MonoBehaviour>())
                {
                    MethodInfo upd = b != null ? b.GetType().GetMethod("UpdateVolumeSize") : null;
                    if (upd != null) upd.Invoke(b, null);
                }
                ++made;
            }
            EditorSceneManager.MarkSceneDirty(group.scene);
            return "light volume fog: " + made + " box(es) from " + volumes.Count + " Light Volume(s), " + skipped + " above the haze; " +
                   (top.HasValue ? "below the GW2 height haze top y = " + top.Value.ToString("F1") : "no GW2 Haze: the lower third of each volume") +
                   " (lit by the Light Volumes: bake them)";
        }

        // ---- bake with Bakery (2026-10-22) -----------------------------------------------------------------
        // Bakery is an Asset Store package, so it is found by reflection, as red.sim.lightvolumes'
        // BakeryEditorBridge does (ftRenderLightmap in BakeryEditorAssembly or Assembly-CSharp-Editor).
        // The scene's LightVolumeManager goes to BakingMode 1 through the package's own
        // HandleBakingModeChanged (it adds each volume's BakeryVolume); then Bakery's full render
        // (ftRenderLightmap.instance.RenderButton, Bakery's scripting entry), and on its
        // OnFinishedFullRender the reflection probes (Unity's bake, now lit by Bakery's probes) and
        // occlusion. The map's renderers receive GI from probes / Light Volumes, not lightmaps, so
        // no lightmap UVs are needed (GW2's UV1 stays in UV4 for the castlemist shaders).

        static Type BakeryRenderer()
        {
            return FindType("ftRenderLightmap");
        }

        [MenuItem("Castlemist/Bake Lighting and Occlusion (Bakery)")]
        public static void BakeBakeryMenu()
        {
            Type rt = BakeryRenderer();
            if (rt == null)
            {
                EditorUtility.DisplayDialog("Castlemist", "Bakery is not in this project (no ftRenderLightmap). Install Bakery, " +
                                            "or use Castlemist/Bake Lighting and Occlusion (Unity).", "OK");
                return;
            }
            if (Lightmapping.isRunning || StaticOcclusionCulling.isRunning) { Debug.LogWarning("CASTLEMIST a bake is already running"); return; }
            LinkReferenceCamera();
            SetLightVolumeBakingMode(1);

            const BindingFlags S = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
            const BindingFlags I = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            // Bakery's renderer is ftRenderLightmap.instance (not "main", which no Bakery version has:
            // the 2026-10-22 error). It is its EditorWindow; LoadRenderSettings sets the static.
            Func<object> current = () =>
            {
                FieldInfo f = rt.GetField("instance", S) ?? rt.GetField("main", S);
                return f != null ? f.GetValue(null) : null;
            };
            object bakery = current();
            if (bakery == null && typeof(EditorWindow).IsAssignableFrom(rt)) bakery = EditorWindow.GetWindow(rt);
            if (bakery == null) { Debug.LogError("CASTLEMIST Bakery: could not get its ftRenderLightmap window"); return; }

            EventInfo finished = rt.GetEvent("OnFinishedFullRender", S);
            if (finished != null && finished.EventHandlerType == typeof(EventHandler))
            {
                finished.RemoveEventHandler(null, s_afterBakery);
                finished.AddEventHandler(null, s_afterBakery);
            }
            else Debug.LogWarning("CASTLEMIST Bakery has no OnFinishedFullRender: run Castlemist/Bake Reflection Probes and Occlusion after it");

            MethodInfo load = rt.GetMethod("LoadRenderSettings", I, null, Type.EmptyTypes, null);
            if (load != null) load.Invoke(bakery, null);
            MethodInfo render = rt.GetMethod("RenderButton", I, null, new[] { typeof(bool) }, null);
            if (render != null) render.Invoke(bakery, new object[] { false });
            else
            {
                render = rt.GetMethod("RenderButton", I, null, Type.EmptyTypes, null);
                if (render == null) { Debug.LogError("CASTLEMIST Bakery: no ftRenderLightmap.RenderButton in this version"); return; }
                render.Invoke(bakery, null);
            }
            Debug.Log("CASTLEMIST baking with Bakery (light volumes, probes); reflection probes and occlusion follow");
        }

        static readonly EventHandler s_afterBakery = AfterBakery;

        static void AfterBakery(object sender, EventArgs args)
        {
            EventInfo finished = BakeryRenderer()?.GetEvent("OnFinishedFullRender", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
            if (finished != null) finished.RemoveEventHandler(null, s_afterBakery);
            // Bakery's completion runs inside its own update: start the Unity jobs on the next one.
            EditorApplication.delayCall += BakeReflectionsAndOcclusion;
        }

        [MenuItem("Castlemist/Bake Reflection Probes and Occlusion")]
        public static void BakeReflectionsAndOcclusion()
        {
            int n = BakeReflectionProbes();
            if (n > 0) Debug.Log("CASTLEMIST " + n + " reflection probe(s) baked");
            else Debug.LogWarning("CASTLEMIST reflection probes: nothing baked (no probes, or the scene is unsaved)");
            if (StaticOcclusionCulling.GenerateInBackground()) Debug.Log("CASTLEMIST computing occlusion");
            else Debug.LogError("CASTLEMIST StaticOcclusionCulling.GenerateInBackground did not start");
        }

        /// Every reflection probe of the open scene baked into its own cubemap asset beside the scene
        /// (<scene>/ReflectionProbe-<name>.exr) and set as the probe's Custom texture. Baked-mode probes
        /// keep their result in Unity's lighting data, which a Bakery bake never writes: after one,
        /// every probe had no texture and metal reflected the black Mists sky (2026-10-22).
        public static int BakeReflectionProbes()
        {
            string scene = UnityEngine.SceneManagement.SceneManager.GetActiveScene().path;
            if (string.IsNullOrEmpty(scene)) return 0;
            string dir = System.IO.Path.ChangeExtension(scene, null);
            System.IO.Directory.CreateDirectory(dir);
            int n = 0;
            foreach (ReflectionProbe p in Object.FindObjectsByType<ReflectionProbe>(FindObjectsSortMode.None))
            {
                if (p.mode == ReflectionProbeMode.Realtime) continue;
                string path = dir + "/ReflectionProbe-" + p.name + ".exr";
                if (!Lightmapping.BakeReflectionProbe(p, path)) continue;
                AssetDatabase.ImportAsset(path);
                var cube = AssetDatabase.LoadAssetAtPath<Texture>(path);
                if (cube == null) continue;
                Undo.RecordObject(p, "Bake reflection probe");
                p.mode = ReflectionProbeMode.Custom;
                p.customBakedTexture = cube;
                EditorUtility.SetDirty(p);
                ++n;
            }
            if (n > 0) EditorSceneManager.MarkSceneDirty(UnityEngine.SceneManagement.SceneManager.GetActiveScene());
            return n;
        }

        /// The scene's LightVolumeManager to @p mode (0 Unity, 1 Bakery) through the package's own
        /// HandleBakingModeChanged, so its Bakery helpers are added or removed as when edited by hand.
        static void SetLightVolumeBakingMode(int mode)
        {
            Type mt = FindType("VRCLightVolumes.LightVolumeManager");
            if (mt == null) return;
            FieldInfo bm = mt.GetField("BakingMode", BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic);
            if (bm == null) return;
            Type backend = FindType("VRCLightVolumes.LightVolumeManagerEditorBackend");
            MethodInfo changed = backend != null
                ? backend.GetMethod("HandleBakingModeChanged", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
                : null;
            foreach (UnityEngine.Object o in UnityEngine.Object.FindObjectsByType(mt, FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                int prev = (int)bm.GetValue(o);
                if (prev == mode) continue;
                Undo.RecordObject(o, "Light Volumes baking mode");
                bm.SetValue(o, mode);
                if (changed != null)
                {
                    try { changed.Invoke(null, new object[] { o, prev }); }
                    catch (Exception e) { Debug.LogWarning("CASTLEMIST Light Volumes: HandleBakingModeChanged failed: " + (e.InnerException ?? e).Message); }
                }
                EditorUtility.SetDirty(o);
                Debug.Log("CASTLEMIST Light Volumes baking mode " + prev + " -> " + mode);
            }
        }
    }
}
