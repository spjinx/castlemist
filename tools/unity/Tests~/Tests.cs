// Unity-free tests of MiniJson, WorldManifest, MaterialsFile and ImportSummary.
//   dotnet run --project tools/unity/Tests~ -- [<export dir>]
// With an export dir (the cm_world_export_fixture output), the fixture tests
// run too. Prints "ok <name>" per test and "<n> passed, <m> failed".

using System;
using System.Collections.Generic;
using System.IO;

namespace Castlemist.Tests
{
    static class Program
    {
        static int passed, failed;

        static void Check(bool cond, string what)
        {
            if (!cond) throw new Exception("check failed: " + what);
        }

        static void Throws<T>(Action a, string contains) where T : Exception
        {
            try { a(); }
            catch (T e)
            {
                if (contains != null && e.Message.IndexOf(contains, StringComparison.Ordinal) < 0)
                    throw new Exception("message '" + e.Message + "' lacks '" + contains + "'");
                return;
            }
            throw new Exception("expected " + typeof(T).Name);
        }

        static void Run(string name, Action t)
        {
            try { t(); ++passed; Console.WriteLine("ok " + name); }
            catch (Exception e) { ++failed; Console.WriteLine("FAIL " + name + ": " + e.Message); }
        }

        // A minimal valid manifest; `patch` edits the parsed text.
        const string Minimal = @"{""format"":""castlemist-world"",""version"":1,
 ""map"":{""fileId"":7,""bounds"":null},
 ""models"":[],""cells"":[],
 ""light"":null,""water"":null,""frameProbe"":null,""sky"":null,""spawn"":null,
 ""warnings"":[]}";

        const string MeshPlan = "PLAN materials=4 prefabs=3 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0 foliageTrees=2 foliagePatches=2 terrains=0 detailPrototypes=0 detailCells=0 treeInstances=0 lodPrefabs=3 lodTreePrefabs=1 lodOverrides=1";

        static int Main(string[] args)
        {
            Run("minijson.values", () =>
            {
                var d = (Dictionary<string, object>)MiniJson.Parse(
                    "{\"a\":[[1,2],[3,-4.5e1]],\"s\":\"x\\\"\\u00e9\\n\",\"t\":true,\"f\":false,\"n\":null,\"e\":{}}");
                var a = (List<object>)d["a"];
                Check(((List<object>)a[1]).Count == 2, "nested");
                Check((double)((List<object>)a[1])[1] == -45.0, "exponent");
                Check((string)d["s"] == "x\"é\n", "escapes");
                Check((bool)d["t"] && !(bool)d["f"] && d["n"] == null, "literals");
                Check(((Dictionary<string, object>)d["e"]).Count == 0, "empty object");
                Check(((List<object>)MiniJson.Parse(" [ ] ")).Count == 0, "empty array");
                Check((double)MiniJson.Parse("﻿0.5") == 0.5, "BOM");
            });
            Run("minijson.errors", () =>
            {
                Throws<FormatException>(() => MiniJson.Parse("NaN"), "unexpected");
                Throws<FormatException>(() => MiniJson.Parse("[1] x"), "trailing");
                Throws<FormatException>(() => MiniJson.Parse("\"abc"), "unterminated");
                Throws<FormatException>(() => MiniJson.Parse("[1,]"), "unexpected");
                Throws<FormatException>(() => MiniJson.Parse("{\"a\" 1}"), "':'");
                Throws<FormatException>(() => MiniJson.Parse("01"), "trailing");
                Throws<FormatException>(() => MiniJson.Parse("1e999"), "range");
                Throws<FormatException>(() => MiniJson.Parse(""), "end");
            });
            Run("manifest.nulls", () =>
            {
                WorldManifest m = WorldManifest.Parse(Minimal);
                Check(m.MapFileId == 7, "fileId");
                Check(m.Light == null && m.Water == null && m.FrameProbe == null && m.Sky == null && m.SpawnUnity == null, "nulls");
                Check(Math.Abs(m.UnityScale - 0.0254) < 1e-12, "default scale");
                double[] u = m.MapToUnity(new double[] { 1, 2, 3 });
                Check(u[0] == 1 && u[1] == -3 && u[2] == 2, "toUnity default");
            });
            Run("manifest.version_refused", () =>
            {
                Throws<ManifestException>(() => WorldManifest.Parse(Minimal.Replace("\"version\":1", "\"version\":2")), "major version 2");
                Throws<ManifestException>(() => WorldManifest.Parse(Minimal.Replace("castlemist-world", "other")), "manifest.format");
                Throws<ManifestException>(() => WorldManifest.Parse(Minimal.Replace("\"fileId\":7", "\"fileId\":\"7\"")), "manifest.map.fileId");
                Throws<ManifestException>(() => WorldManifest.Parse("[1"), "manifest.json");
            });
            Run("manifest.model_position", () =>
            {
                // A region export keeps scene.models indices: instances and records name the position instead.
                WorldManifest m = WorldManifest.Parse(Minimal.Replace("\"models\":[]",
                    "\"models\":[{\"index\":5,\"fileId\":11,\"status\":\"ok\"},{\"index\":9,\"fileId\":12,\"status\":\"ok\"}]"));
                Check(m.Models[1].Index == 9 && m.Models[1].Position == 1 && m.Models[0].Position == 0, "position");
            });
            Run("summary.plan_line", () =>
            {
                var s = new ImportSummary { materials = 4, prefabs = 3, controllers = 1, cells = 1, instances = 4, colliders = 1, terrainColliders = 1, water = 1, lights = 1, skybox = 0, foliageTrees = 2, foliagePatches = 2, lodPrefabs = 3, lodTreePrefabs = 1, lodOverrides = 1 };
                // The world_manifest_plan ctest's expected line for the fixture, verbatim.
                Check(s.PlanLine() == MeshPlan, s.PlanLine());
            });
            Run("lods.screen_height", () =>
            {
                // h(d) = size / (2 d tan 30): 2 tan 30 = 1.1547; scale cancels (Task 15b).
                double k = 2 * Math.Tan(Math.PI / 6);
                Check(Math.Abs(ModelLods.ScreenHeight(100, 3500) - 100 / (3500 * k)) < 1e-12, "formula");
                Check(double.IsPositiveInfinity(ModelLods.ScreenHeight(100, 0)) && ModelLods.ScreenHeight(0, 10) == 0, "edges");
                double[] h = ModelLods.TransitionHeights(100, new[] { 1500.0, 7500.0 }, 3, ModelLods.PropCullHeight);
                Check(h.Length == 3 && Math.Abs(h[0] - 100 / (1500 * k)) < 1e-12 && Math.Abs(h[1] - 100 / (7500 * k)) < 1e-12 && h[2] == ModelLods.PropCullHeight, "props");
                Check(ModelLods.PropCull(2) == ModelLods.PropCullHeight / 2 && ModelLods.PropCull(0) == ModelLods.PropCullHeight, "prop cull x Distance");
                // A tree culled (fadeFar 5000) before its LOD2 switch (7500): LOD2 dropped.
                double cull = FoliageRecords.CullHeight(100, 5000);
                h = ModelLods.TransitionHeights(100, new[] { 1500.0, 7500.0 }, 3, cull);
                Check(h.Length == 2 && h[1] == cull && h[0] > h[1], "tree drop");
                // Culled before LOD1 too: LOD0 only.
                h = ModelLods.TransitionHeights(100, new[] { 1500.0, 7500.0 }, 3, FoliageRecords.CullHeight(100, 1000));
                Check(h.Length == 1, "LOD0 only");
                // d1 = d2 and a 0 distance: strictly decreasing, clamped to 1.
                h = ModelLods.TransitionHeights(100, new[] { 0.0, 0.0 }, 3, 0);
                Check(h.Length == 3 && h[0] == 1.0 && h[1] < h[0] && h[1] > 0.99 && h[2] == 0, "clamp");
                h = ModelLods.TransitionHeights(100, new[] { 2000.0, 2000.0 }, 3, 0);
                Check(h[1] < h[0], "equal distances");
                Check(ModelLods.TransitionHeights(100, null, 1, 0.01).Length == 1, "one level");
            });
            Run("lods.assign", () =>
            {
                string b;
                Check(ModelLods.LevelOf("101_mesh0_LOD2", out b) == 2 && b == "101_mesh0", "suffix");
                Check(ModelLods.LevelOf("101_mesh0", out b) == 0 && b == "101_mesh0", "plain");
                Check(ModelLods.LevelOf("x_LOD0", out b) == 0, "LOD0 suffix is a name");
                var orphans = new List<string>();
                List<int>[] a = ModelLods.Assign(new[] { "a_0", "b_1", "a_0_LOD1", "a_0_LOD2", "b_1_LOD2", "c_2" }, 3, orphans);
                Check(string.Join(",", a[0]) == "0,1,5", "LOD0");
                Check(string.Join(",", a[1]) == "2,1,5", "LOD1: b repeats LOD0, c repeats LOD0");
                Check(string.Join(",", a[2]) == "3,4,5", "LOD2");
                Check(orphans.Count == 0, "no orphans");
                ModelLods.Assign(new[] { "z_LOD1", "y_0_LOD3" }, 2, orphans);
                Check(orphans.Count == 3, "orphans: z_LOD1 has no LOD0; y_0_LOD3 has none and is past the levels");
            });
            Run("lods.far", () =>
            {
                var far = new HashSet<string> { "lod_1" };
                bool hasFar;
                // One level: the stand-in is LOD1.
                List<int>[] a = ModelLods.Assign(new[] { "buttress_0", "lod_1" }, 1, far, out hasFar);
                Check(hasFar && a.Length == 2 && string.Join(",", a[0]) == "0" && string.Join(",", a[1]) == "1", "1 level");
                Check(ModelLods.LevelCount(1, true) == 2 && ModelLods.LevelCount(2, true) == 3 && ModelLods.LevelCount(3, true) == 3 &&
                      ModelLods.LevelCount(3, false) == 3, "level counts");
                // Two levels: LOD0, LOD1, then the stand-in.
                a = ModelLods.Assign(new[] { "a_0", "a_0_LOD1", "lod_1" }, 2, far, out hasFar);
                Check(a.Length == 3 && string.Join(",", a[1]) == "1" && string.Join(",", a[2]) == "2", "2 levels");
                // Three levels: the stand-in takes LOD2's place; its own _LOD1 is never drawn.
                a = ModelLods.Assign(new[] { "a_0", "a_0_LOD1", "a_0_LOD2", "lod_1", "lod_1_LOD1" }, 3, far, out hasFar);
                Check(a.Length == 3 && string.Join(",", a[1]) == "1" && string.Join(",", a[2]) == "3", "3 levels");
                Check(ModelLods.IsFar("lod_1_LOD1", far) && !ModelLods.IsFar("a_0", far) && !ModelLods.IsFar("lod_1", null), "IsFar");
                // Only stand-ins: drawn as ordinary meshes.
                a = ModelLods.Assign(new[] { "lod_1" }, 1, far, out hasFar);
                Check(!hasFar && a.Length == 1 && string.Join(",", a[0]) == "0", "stand-ins only");
                // No stand-in present: Assign's own result.
                a = ModelLods.Assign(new[] { "a_0" }, 1, far, out hasFar);
                Check(!hasFar && a.Length == 1, "none present");
            });
            Run("manifest.lods", () =>
            {
                WorldManifest m = WorldManifest.Parse(Minimal.Replace("\"models\":[]",
                    "\"models\":[{\"index\":0,\"fileId\":11,\"status\":\"ok\",\"lods\":{\"levels\":3,\"distances\":[1500,7500]," +
                    "\"distanceSource\":[\"lodOverride\",\"default\"],\"lodOverride\":[1500,0],\"triangles\":[12,8,4]," +
                    "\"farMeshes\":[\"lod_1\"],\"farTriangles\":2}}," +
                    "{\"index\":1,\"fileId\":12,\"status\":\"ok\"}]"));
                Check(m.Models[0].LodLevels == 3 && m.Models[0].LodDistances[1] == 7500, "lods");
                Check(m.Models[0].FarMeshes.Count == 1 && m.Models[0].FarMeshes[0] == "lod_1", "farMeshes");
                Check(m.Models[1].LodLevels == 1 && m.Models[1].LodDistances == null && m.Models[1].FarMeshes.Count == 0, "absent");
                Throws<ManifestException>(() => WorldManifest.Parse(Minimal.Replace("\"models\":[]",
                    "\"models\":[{\"index\":0,\"fileId\":11,\"status\":\"ok\",\"lods\":{\"levels\":4,\"distances\":[1,2]}}]")), "levels");
            });
            Run("foliage.fmix32_keep", () =>
            {
                Check(FoliageRecords.Fmix32(0) == 0 && FoliageRecords.Fmix32(1) == 0x514E28B7u && FoliageRecords.Fmix32(3) == 0x85F0B427u, "vectors");
                Check(!FoliageRecords.Keep(3, 0, 0.5f) && FoliageRecords.Keep(3, 1, 0.5f) && FoliageRecords.Keep(1, 0, 0.5f), "half");
                Check(FoliageRecords.Keep(0xFFFFFFFFu, 0, 0f) && FoliageRecords.Keep(3, 1, 0f) && !FoliageRecords.Keep(0, 0, 0f), "zero");
                Check(FoliageRecords.Keep(0xFFFFFFFEu, 0, 1f), "one");
                Check(FoliageRecords.DensityThreshold(0.3f) == 0.3 * 4294967296.0, "decimal density");
                int kept = 0;
                for (uint i = 0; i < 20000; ++i) if (FoliageRecords.Keep(i, 0, 0.5f)) ++kept;
                Check(kept > 9600 && kept < 10400, "kept " + kept);
            });
            Run("options.quest_and_names", () =>
            {
                ImportOptions q = ImportOptions.Quest();
                Check(q.terrain == TerrainMode.Mesh && q.foliage == FoliageMode.ColliderTrees && q.distance == 0.5f && q.density == 1f, "quest");
                Check(ImportOptions.ParseTerrain("terrain") == TerrainMode.UnityTerrain && ImportOptions.ParseFoliage("colliders") == FoliageMode.ColliderTrees, "parse");
                Throws<ArgumentException>(() => ImportOptions.ParseFoliage("some"), "foliage must be");
                Check(ImportOptions.ParseShader("castlemist") == ShaderMode.Castlemist && new ImportOptions().shader == ShaderMode.Poiyomi, "shader");
                Check(!new ImportOptions().ToString().Contains("shader") &&
                      new ImportOptions { shader = ShaderMode.Castlemist }.ToString().EndsWith(" shader=castlemist"), "shader in the options line");
                Throws<ArgumentException>(() => ImportOptions.ParseShader("toon"), "shader must be");
                var skip = new ImportOptions { skipModels = " 3132765, 12;7\t" }.SkipModelIds();
                Check(skip.SetEquals(new long[] { 3132765, 12, 7 }), "skip models parse");
                Check(new ImportOptions { skipModels = "12,7" }.ToString().EndsWith(" skip=7,12"), "skip in the options line");
                Throws<ArgumentException>(() => new ImportOptions { skipModels = "12,abc" }.Validate(), "skip models");
                Throws<ArgumentException>(() => new ImportOptions { distance = 3f }.Validate(), "distance");
                Throws<ArgumentException>(() => new ImportOptions { density = -0.1f }.Validate(), "density");
                Check(FoliagePlan.PatchCapacity(12) == 1820 && FoliagePlan.PatchCapacity(30000) == 1 && FoliagePlan.PatchCapacity(0) == 21845, "capacity");
            });
            Run("options.environment", () =>
            {
                // 2026-10-22: SceneSetup's toggles; all on is the options line as before.
                var o = new ImportOptions();
                Check(o.fog && o.ambient && o.post && o.lightVolumes && o.reflectionProbes && o.occlusion && !o.ToString().Contains("noEnv"), "defaults");
                ImportOptions q = ImportOptions.Quest();
                Check(!q.post && q.fog && q.lightVolumes && q.reflectionResolution == 128 && q.lightVolumeBudget < o.lightVolumeBudget, "quest");
                Check(!q.haze && q.ToString().EndsWith(" noEnv=post,haze"), "quest line " + q);
                o.DisableEnv("Fog, lightvolumes");
                Check(!o.fog && !o.lightVolumes && o.post && o.ToString().EndsWith(" noEnv=fog,lightvolumes"), "disable " + o);
                ImportOptions c = o.Clone();
                c.post = false;
                Check(o.post && !c.post && !c.fog, "clone");
                o.DisableEnv("all");
                Check(!o.ambient && !o.occlusion && !o.reflectionProbes, "all");
                Throws<ArgumentException>(() => new ImportOptions().DisableEnv("dof"), "environment option");
            });
            Run("manifest.environment", () =>
            {
                WorldManifest none = WorldManifest.Parse(Minimal);
                Check(none.Environment == null, "absent");
                Check(WorldManifest.Parse(Minimal.Replace("\"warnings\":[]", "\"environment\":null,\"warnings\":[]")).Environment == null, "null");
                WorldManifest m = WorldManifest.Parse(Minimal.Replace("\"warnings\":[]",
                    "\"environment\":{\"preset\":1,\"lights\":[{\"color\":[1,0.9,0.8],\"intensity\":1.3,\"unityForward\":[0.7071,-0.7071,0]}," +
                    "{\"color\":[0.2,0.4,0.8],\"intensity\":0.5,\"unityForward\":[0,-1,0]}],\"shadowInfluence\":0.5,\"backlight\":null," +
                    "\"haze\":{\"near\":[0.5,0.6,0.7,0],\"far\":[0.7,0.8,0.9,0.5],\"height\":[0,0,0,0],\"distRange\":[1000,21000]," +
                    "\"heightRange\":[0,-500],\"heightRangeUnity\":[0,500],\"sunDirRange\":[0,1],\"depthCue\":0}," +
                    "\"effect\":{\"glow\":[1,0.5,0.5,1],\"glowLevel\":[0.8,0.6,0.7,1],\"glowAmplify\":1.5,\"saturation\":1.2," +
                    "\"tintColor\":[1,0.5,0,1],\"tintAmount\":0.5,\"ssaoAmount\":0.7,\"flags\":3,\"clutFileId\":null,\"clut\":null,\"clutLayout\":null}," +
                    "\"zones\":[{\"name\":\"z\",\"haze\":null,\"boundsUnity\":[0,0,1,1]}]},\"warnings\":[]"));
                WorldManifest.EnvironmentEntry e = m.Environment;
                Check(e != null && e.Preset == 1 && e.Lights.Count == 2 && e.Lights[0].Intensity == 1.3 && e.Backlight == null, "lights");
                Check(e.Haze.DistRange[1] == 21000 && e.Haze.Far[3] == 0.5 && e.Haze.HeightRangeUnity[1] == 500, "haze");
                Check(e.Effect.GlowAmplify == 1.5 && e.Effect.Saturation == 1.2 && e.Effect.TintFocus == null && e.Effect.Flags == 3 &&
                      e.Effect.ClutFileId == null && e.Effect.Clut == null && e.Effect.ClutLayout == null, "effect, absent fields null");
                Check(e.Zones.Count == 1 && e.Zones[0].Haze == null && e.Zones[0].BoundsUnity[2] == 1, "zones");

                // Fog: alphas 0 -> 0.5 over 1000..21000: the 0 crossing at 1000, the 1 crossing at 41000; colour 0 / 0.5 weighted: far.
                EnvironmentMath.Fog fog = EnvironmentMath.FogFromHaze(e.Haze, 0.0254);
                Check(Math.Abs(fog.Start - 1000 * 0.0254) < 1e-9 && Math.Abs(fog.End - 41000 * 0.0254) < 1e-9, "fog range " + fog.Start + ".." + fog.End);
                Check(Math.Abs(fog.Color[0] - 0.7) < 1e-12 && Math.Abs(fog.Color[2] - 0.9) < 1e-12, "fog colour");
                var flat = new WorldManifest.HazeEntry { Near = new double[] { 0, 0, 0, 1 }, Far = new double[] { 1, 1, 1, 1 }, DistRange = new double[] { 10, 20 } };
                fog = EnvironmentMath.FogFromHaze(flat, 1);
                Check(fog.Start == 10 && fog.End == 20 && Math.Abs(fog.Color[0] - 0.5) < 1e-12, "equal alphas: distRange itself, blended colour");
                Check(EnvironmentMath.FogFromHaze(new WorldManifest.HazeEntry { DistRange = new double[] { 5, 5 } }, 1) == null &&
                      EnvironmentMath.FogFromHaze(null, 1) == null, "no range");

                // Trilight: the fill points straight down (L = up): sky = colour x intensity, equator and ground 0.
                double[][] tri = EnvironmentMath.TrilightFromFills(e.Lights);
                Check(Math.Abs(tri[0][2] - 0.4) < 1e-12 && Math.Abs(tri[1][2]) < 1e-12 && tri[2][2] == 0, "trilight down");
                var side = new List<WorldManifest.EnvLight> { null, new WorldManifest.EnvLight { Color = new double[] { 1, 1, 1 }, Intensity = 1, UnityForward = new double[] { 2, 0, 0 } } };
                tri = EnvironmentMath.TrilightFromFills(side);
                Check(tri[0][0] == 0 && tri[2][0] == 0 && Math.Abs(tri[1][0] - 1 / Math.PI) < 1e-12, "trilight horizontal: ring mean 1/pi");
                Check(EnvironmentMath.TrilightFromFills(e.Lights.GetRange(0, 1)) == null, "sun only: no trilight");

                // The haze volume's heightRange: raw as stored, else from heightRangeUnity (lo, hi) as (-hi, -lo).
                bool fromUnity;
                double[] hr = EnvironmentMath.HazeHeightRange(e.Haze, out fromUnity);
                Check(hr[0] == 0 && hr[1] == -500 && !fromUnity, "raw heightRange");
                hr = EnvironmentMath.HazeHeightRange(new WorldManifest.HazeEntry { HeightRangeUnity = new double[] { 100, 900 } }, out fromUnity);
                Check(hr[0] == -900 && hr[1] == -100 && fromUnity, "from heightRangeUnity");
                Check(EnvironmentMath.HazeHeightRange(new WorldManifest.HazeEntry(), out fromUnity) == null, "none");

                EnvironmentMath.Bloom b = EnvironmentMath.BloomFromEffect(e.Effect);
                Check(b.Intensity == 1.5 && b.Threshold == 1 && b.Color[0] == 1 && b.Color[1] == 0.5, "bloom");
                Check(EnvironmentMath.BloomFromEffect(new WorldManifest.EffectEntry { GlowAmplify = 0 }) == null, "no bloom");
                Check(Math.Abs(EnvironmentMath.Saturation(1.2) - 20) < 1e-9 && EnvironmentMath.Saturation(1) == 0 && EnvironmentMath.Saturation(-5) == -100, "saturation");
                double[] cf = EnvironmentMath.ColorFilter(e.Effect.TintColor, e.Effect.TintAmount);
                Check(cf[0] == 1 && cf[1] == 0.75 && cf[2] == 0.5 && EnvironmentMath.ColorFilter(null, 1)[1] == 1, "colour filter");
                Check(Math.Abs(EnvironmentMath.AoIntensity(2) - 0.3) < 1e-9 && EnvironmentMath.AoIntensity(9) == 0.6 && EnvironmentMath.AoIntensity(null) == 0, "ao");
                Check(EnvironmentMath.IsLutStrip(null, 256, 16) && !EnvironmentMath.IsLutStrip(null, 256, 256) &&
                      !EnvironmentMath.IsLutStrip(new WorldManifest.ClutLayout { Kind = "volume" }, 256, 16), "lut strip");
                double v = EnvironmentMath.VoxelsPerUnit(100, 10, 100, 100000, 1);
                Check(Math.Abs(v - 1) < 1e-12 && Math.Abs(EnvironmentMath.VoxelsPerUnit(300, 50, 300, 131072, 1) - Math.Pow(131072 / 4.5e6, 1 / 3.0)) < 1e-12, "voxels");
            });
            Run("foliage.records", () =>
            {
                var d = new byte[2 * FoliageRecords.RecordBytes];
                BitConverter.GetBytes(7u).CopyTo(d, 100);
                BitConverter.GetBytes((ushort)0xFFFF).CopyTo(d, 104);
                BitConverter.GetBytes((ushort)5).CopyTo(d, 106);
                BitConverter.GetBytes(0xFFFFFFFFu).CopyTo(d, 108);
                BitConverter.GetBytes(-2212f).CopyTo(d, 160);
                BitConverter.GetBytes(0.9486833f).CopyTo(d, 184);
                BitConverter.GetBytes(1.2f).CopyTo(d, 188);
                BitConverter.GetBytes(10f).CopyTo(d, 196);
                var r = FoliageRecords.Parse(d, "x.bin");
                Check(r.Count == 2 && r[1].Model == 7 && r[1].Layer == 0xFFFF && r[1].Collider && r[1].Placement == 0xFFFFFFFF, "header");
                Check(r[1].T[0] == -2212f && r[1].R[3] == 0.9486833f && r[1].S == 1.2f && r[1].TiltDeg == 10f, "unity");
                Check(FoliageRecords.IsTree(r[1], null) && !FoliageRecords.IsTree(r[0], null), "tree rule");
                Throws<ManifestException>(() => FoliageRecords.Parse(new byte[99], "y.bin"), "y.bin: foliage size 99");
                Throws<ManifestException>(() => FoliageRecords.Read("no/such/foliage.bin"), "missing file no/such/foliage.bin");
            });
            Run("foliage.cull_height", () =>
            {
                // size / (2 fadeFar tan 30): 2 tan 30 = 1.1547.
                Check(Math.Abs(FoliageRecords.CullHeight(1000, 7500) - 1000 / (7500 * 2 * Math.Tan(Math.PI / 6))) < 1e-12, "formula");
                Check(FoliageRecords.CullHeight(1e-3, 7500) == 1e-4 && FoliageRecords.CullHeight(1e6, 10) == 1.0, "clamp");
                Check(FoliageRecords.CullHeight(10, 0) == 1e-4, "no fadeFar");
            });
            Run("materials.blend_and_maps", () =>
            {
                MaterialsFile f = MaterialsFile.Parse(@"{""poiyomi"":""10"",""materials"":[{""name"":""M"",""mode"":5,""exact"":false,
                    ""blend"":{""srcRgb"":4,""dstRgb"":2,""srcA"":2,""dstA"":2,""op"":0,""eqRgb"":0,""eqA"":0},
                    ""maps"":{""baseColor"":{""file"":""Textures/M - BaseColor.png"",""uv"":1},""normal"":null,""extras"":[]},
                    ""emission"":{""color"":[1,0.5,0],""colorSpace"":""sRGB"",""strength"":2},
                    ""specularTint"":null,""rim"":null,""warnings"":[""w""]}]}", "materials.json");
                MaterialsFile.MaterialEntry e = f.Materials[0];
                Check(f.Poiyomi == "10" && e.Mode == 5 && !e.Exact && e.Blend.SrcRgb == 4 && e.Blend.DstRgb == 2, "blend");
                Check(e.Map("baseColor").Uv == 1 && e.Map("normal") == null, "maps");
                Check(e.HasEmission && e.EmissionStrength == 2 && e.EmissionColor[1] == 0.5, "emission");
                Check(e.SpecularTint == null && !e.HasRim && e.Warnings.Count == 1 && e.UsedByMeshes, "rest");
            });

            Run("poiyomi.version", () =>
            {
                Check(PoiyomiShaders.VersionOf("<color=#E75898ff>Poiyomi 11.0.22</color>") == "11.0.22", "11");
                Check(PoiyomiShaders.VersionOf("<color=#E75898ff>Poiyomi 10.0.23</color>") == "10.0.23", "10");
                Check(PoiyomiShaders.VersionOf("Poiyomi 9.0.α18") == "9.0.α18", "9 alpha");
                Check(PoiyomiShaders.VersionOf("Rendering Preset") == null && PoiyomiShaders.VersionOf(null) == null, "none");
                Check(PoiyomiShaders.Major("11.0.22") == 11 && PoiyomiShaders.Major("9.0.α18") == 9 && PoiyomiShaders.Major(null) == -1, "major");
                Check(PoiyomiShaders.IsPro(".poiyomi/Old Versions/10.0/Poiyomi Pro") && !PoiyomiShaders.IsPro(".poiyomi/Poiyomi Toon"), "family");
            });
            Run("poiyomi.choose", () =>
            {
                Func<string, string, bool, PoiyomiShaders.Found> F = (n, v, err) => new PoiyomiShaders.Found { Name = n, Version = v, HasError = err };
                const string Pro = ".poiyomi/Poiyomi Pro", Toon = ".poiyomi/Poiyomi Toon";
                const string Pro10 = ".poiyomi/Old Versions/10.0/Poiyomi Pro", Toon10 = ".poiyomi/Old Versions/10.0/Poiyomi Toon";
                const string Pro9 = ".poiyomi/Old Versions/9.3/Poiyomi Pro", Toon9 = ".poiyomi/Old Versions/9.3/Poiyomi Toon";
                // The names are tried in this order; Choose gets them in it.
                Check(string.Join("|", PoiyomiShaders.Names) == string.Join("|", new[] { Pro, Toon, Pro10, Toon10, Pro9, Toon9 }), "names");
                // Poiyomi Pro 11.0.22: 11 at the plain names, 10.0.23 and 9.3 under Old Versions.
                var p11 = new List<PoiyomiShaders.Found> { F(Pro, "11.0.22", false), F(Toon, "11.0.22", false), F(Pro10, "10.0.23", false),
                                                           F(Toon10, "10.0.23", false), F(Pro9, "9.3.67", false), F(Toon9, "9.3.67", false) };
                Check(PoiyomiShaders.Choose(p11) == 0, "11 Pro first");
                // The same package without Shaders/11.0: the old 10.0 Pro.
                Check(PoiyomiShaders.Choose(p11.GetRange(2, 4)) == 0, "10 Pro when 11 is absent");
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Toon10, "10.0.23", false), F(Toon9, "9.3.67", false) }) == 0, "10 Toon");
                // Poiyomi Pro 10.0.9: 10 at the plain names.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Pro, "10.0.9", false), F(Toon, "10.0.9", false), F(Pro9, "9.3.67", false) }) == 0, "10.0.9 Pro");
                // Free Poiyomi Toon: Toon only.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Toon, "10.0.24", false) }) == 0, "Toon only");
                // A newer major beats Pro: 11 Toon over 10 Pro.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Toon, "11.0.22", false), F(Pro10, "10.0.23", false) }) == 0, "11 Toon over 10 Pro");
                // Pro over Toon of one major, whatever the order given.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Toon10, "10.0.23", false), F(Pro10, "10.0.23", false) }) == 1, "Pro over Toon");
                // 9 only when nothing newer; a shader with errors never.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Pro9, "9.3.67", false), F(Toon9, "9.3.67", false) }) == 0, "9");
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Pro, "11.0.22", true), F(Toon, "11.0.22", false) }) == 1, "error skipped");
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Pro, "11.0.22", true) }) == -1, "only errors");
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found>()) == -1, "none");
                // An unreadable version ranks below every known one.
                Check(PoiyomiShaders.Choose(new List<PoiyomiShaders.Found> { F(Pro, null, false), F(Toon9, "9.3.67", false) }) == 1, "unknown version");
                Check(PoiyomiShaders.Checked(11) && PoiyomiShaders.Checked(10) && !PoiyomiShaders.Checked(9) && !PoiyomiShaders.Checked(12), "checked majors");
            });
            Run("poiyomi.presets", () =>
            {
                // Rows 0 and 3 of _Mode's description, as Poiyomi 10.0.23 and 11.0.22 declare it (identical in both).
                string desc = "Rendering Preset--{on_value_actions:[\n\t\t" +
                    "{value:0,actions:[{type:SET_PROPERTY,data:render_queue=2000},{type:SET_PROPERTY,data:_AlphaForceOpaque=1}, {type:SET_PROPERTY,data:render_type=Opaque},            {type:SET_PROPERTY,data:_BlendOp=0}, {type:SET_PROPERTY,data:_BlendOpAlpha=4}, {type:SET_PROPERTY,data:_Cutoff=0},  {type:SET_PROPERTY,data:_SrcBlend=1}, {type:SET_PROPERTY,data:_DstBlend=0},  {type:SET_PROPERTY,data:_SrcBlendAlpha=1}, {type:SET_PROPERTY,data:_DstBlendAlpha=1},  {type:SET_PROPERTY,data:_AddSrcBlend=1}, {type:SET_PROPERTY,data:_AddDstBlend=1}, {type:SET_PROPERTY,data:_AddSrcBlendAlpha=0}, {type:SET_PROPERTY,data:_AddDstBlendAlpha=1}, {type:SET_PROPERTY,data:_AlphaToCoverage=0},  {type:SET_PROPERTY,data:_ZWrite=1}, {type:SET_PROPERTY,data:_ZTest=4},   {type:SET_PROPERTY,data:_AlphaPremultiply=0}, {type:SET_PROPERTY,data:_OutlineSrcBlend=1}, {type:SET_PROPERTY,data:_OutlineDstBlend=0},  {type:SET_PROPERTY,data:_OutlineSrcBlendAlpha=1}, {type:SET_PROPERTY,data:_OutlineDstBlendAlpha=0}, {type:SET_PROPERTY,data:_OutlineBlendOp=0}, {type:SET_PROPERTY,data:_OutlineBlendOpAlpha=4}]},\n\t\t" +
                    "{value:3,actions:[{type:SET_PROPERTY,data:render_queue=3000},{type:SET_PROPERTY,data:_AlphaForceOpaque=0}, {type:SET_PROPERTY,data:render_type=Transparent},       {type:SET_PROPERTY,data:_BlendOp=0}, {type:SET_PROPERTY,data:_BlendOpAlpha=4}, {type:SET_PROPERTY,data:_Cutoff=.5},  {type:SET_PROPERTY,data:_SrcBlend=1}, {type:SET_PROPERTY,data:_DstBlend=10}, {type:SET_PROPERTY,data:_ZWrite=0}, {type:SET_PROPERTY,data:_AlphaPremultiply=1}]},\n\t\t]}";
                Dictionary<int, PoiyomiShaders.PresetActions> p = PoiyomiShaders.ParsePresets(desc);
                Check(p.Count == 2 && p.ContainsKey(0) && p.ContainsKey(3), "rows " + p.Count);
                Check(p[0].Queue == 2000 && p[0].RenderType == "Opaque" && p[0].Floats.Count == 22, "row 0 " + p[0].Floats.Count);
                Check(p[0].Floats["_OutlineDstBlendAlpha"] == 0f && p[0].Floats["_OutlineBlendOpAlpha"] == 4f && p[0].Floats["_ZWrite"] == 1f, "row 0 floats");
                Check(p[3].Queue == 3000 && p[3].RenderType == "Transparent" && p[3].Floats["_Cutoff"] == 0.5f && p[3].Floats["_AlphaPremultiply"] == 1f, "row 3");
                // The keys stay in the shader's order (the importer applies them in it).
                Check(string.Join(",", p[3].Order) == "_AlphaForceOpaque,_BlendOp,_BlendOpAlpha,_Cutoff,_SrcBlend,_DstBlend,_ZWrite,_AlphaPremultiply", "order");
                Check(PoiyomiShaders.ParsePresets("Rendering Preset").Count == 0 && PoiyomiShaders.ParsePresets(null).Count == 0, "no actions");
                // A row without a queue is not a preset the importer can use.
                Check(PoiyomiShaders.ParsePresets("x--{on_value_actions:[{value:1,actions:[{type:SET_PROPERTY,data:_Cutoff=.5}]}]}").Count == 0, "no queue");
            });

            if (args.Length > 0)
            {
                string dir = args[0];
                Run("fixture.manifest", () =>
                {
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    Check(m.Models.Count == 3 && m.Models[1].Clips[0] == "TestClip", "models");
                    Check(m.Cells.Count == 1 && m.Cells[0].Instances.Count == 4 && m.Cells[0].HasTerrain, "cells");
                    Check(m.Cells[0].ColliderParts == 1 && m.Cells[0].WaterTexture != null, "collider/water");
                    WorldManifest.InstanceEntry i = m.Cells[0].Instances[3];
                    Check(i.Model == 1 && i.Clip == "TestClip" && i.S == 3.0 && i.T[0] == 2288.0 && i.R.Length == 4, "instance");
                    Check(m.Models[0].LodLevels == 3 && m.Models[0].LodDistances[0] == 1500 && m.Cells[0].Instances[1].Lod[1] == 2000 &&
                          m.Cells[0].Instances[2].Lod == null, "lods");
                    // probe.fbx is recorded once world-fbx (Blender) has run on the export; the ctest fixture has none.
                    Check(m.Light != null && m.Light.Ambient.Length == 3 && m.Sky == null &&
                          (m.FrameProbe.Fbx == null || m.FrameProbe.Fbx == "probe.fbx"), "light/sky/probe");
                    double[] o = m.MapToUnity(m.Cells[0].Origin);
                    Check(o[0] == -12288.0 && o[2] == 36864.0, "origin");
                });
                Run("fixture.foliage", () =>
                {
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    Check(m.Foliage != null && m.Foliage.Layers.Count == 2 && m.Foliage.Layers[0].Type == 3 && m.Foliage.Layers[1].Type == 1, "layers");
                    Check(m.Foliage.FadeFar(0) == 7500 && m.Foliage.FadeFar(2) == 3000, "fadeFar");
                    WorldManifest.CellEntry c = m.Cells[0];
                    Check(c.FoliageFile == "foliage/c0_0_2x2.bin" && c.FoliageRecords == 4 && c.FoliageColliderTrees == 2, "cell");
                    var recs = FoliageRecords.Read(Path.Combine(dir, c.FoliageFile));
                    int trees = 0;
                    foreach (FoliageRecord r in recs) if (FoliageRecords.IsTree(r, m.Foliage)) ++trees;
                    Check(recs.Count == 4 && trees == 2, "trees " + trees);
                });
                Run("fixture.unity_terrain", () =>
                {
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    WorldManifest.UnityTerrainBlock ut = m.UnityTerrain;
                    Check(ut != null && ut.Tiles.Count == 1 && ut.HeightmapResolution == 129 && ut.DetailResolution == 256, "block");
                    Check(ut.Check.Count == 32 && ut.FadeQualityScale == 1.5 && m.FadeQualityScale == 1.5, "check / fade");
                    WorldManifest.TerrainTile t = ut.Tiles[0];
                    Check(t.Id == "t0_0" && t.Holes != null && t.TreesRecords == 1 && t.Details.Count == 1, "tile");
                    double[] real = t.Real(ut.TileChunks);
                    Check(real[0] == -15360 && real[2] == -15360 + 6144 && real[3] == 39936 && real[1] == 39936 - 6144, "real");
                    Check(ut.TileOfChunk(1, 1) == t && ut.DetailPrototypeOf(2) == 0 && ut.TreePrototypeOf(0) == 0, "lookups");
                    Check(m.Cells[0].Chunks[2] == 1 && m.Cells[0].Rect[0] == -15360 && m.Models[2].Triangles == 12, "cell / model");
                    Check(m.Foliage.Models.Exists(fm => fm.Model == 2 && fm.TerrainDetail), "terrainDetail");
                });
                Run("fixture.plan_counts", () =>
                {
                    // The foliage counts of the world_manifest_plan / _plan_terrain ctests (validate_manifest.py plan_foliage).
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    Func<WorldManifest.CellEntry, List<FoliageRecord>> read = c => FoliageRecords.Read(Path.Combine(dir, c.FoliageFile));
                    Func<ImportOptions, string> counts = o =>
                    {
                        var errs = new List<string>();
                        FoliagePlan p = FoliagePlan.Build(m, read, o, errs);
                        Check(errs.Count == 0, string.Join("; ", errs));
                        return p.TreeCount + " " + p.PatchCount + " " + p.Tiles.Count + " " + p.DetailPrototypeCount + " " +
                               p.DetailCellCount + " " + p.TreeInstanceCount;
                    };
                    string got;
                    Check((got = counts(new ImportOptions())) == "2 2 0 0 0 0", "mesh " + got);
                    Check((got = counts(new ImportOptions { terrain = TerrainMode.UnityTerrain })) == "1 0 1 1 2 1", "terrain " + got);
                    Check((got = counts(new ImportOptions { density = 0.5f })) == "2 1 0 0 0 0", "mesh 0.5 " + got);
                    Check((got = counts(new ImportOptions { terrain = TerrainMode.UnityTerrain, density = 0.5f })) == "1 0 1 1 1 1", "terrain 0.5 " + got);
                    Check((got = counts(new ImportOptions { terrain = TerrainMode.UnityTerrain, foliage = FoliageMode.ColliderTrees })) == "1 0 1 0 0 1", "terrain colliders " + got);
                    Check((got = counts(ImportOptions.Quest())) == "2 0 0 0 0 0", "quest " + got);
                    Check((got = counts(new ImportOptions { foliage = FoliageMode.Off })) == "0 0 0 0 0 0", "off " + got);
                    Check((got = counts(new ImportOptions { density = 0f })) == "2 0 0 0 0 0", "density 0 keeps colliders " + got);
                    // The detail cells, row 0 north: (38, 82) and (101, 92).
                    FoliagePlan tp = FoliagePlan.Build(m, read, new ImportOptions { terrain = TerrainMode.UnityTerrain }, new List<string>());
                    int[] d0 = tp.Tiles[0].Details[0];
                    Check(d0[82 * 256 + 38] == 1 && d0[92 * 256 + 101] == 1, "detail cells");
                    Check(tp.Tiles[0].Trees.Count == 1 && tp.Tiles[0].Trees[0].Placement == 0, "terrain tree");
                });
                Run("fixture.materials", () =>
                {
                    // Task 15d: the maps are in the shared pool, named by content and role.
                    MaterialsFile f = MaterialsFile.Parse(File.ReadAllText(Path.Combine(dir, "models/102/materials.json")), "102");
                    string bc = f.Materials.Count == 1 ? f.Materials[0].Map("baseColor").File : "";
                    Check(bc.StartsWith("../../textures/gen/") && bc.EndsWith("_BaseColor.png") && bc.Length == "../../textures/gen/".Length + 40 + "_BaseColor.png".Length, "baseColor " + bc);
                    Check(File.Exists(Path.Combine(dir, "models/102", bc)), "pooled file");
                    Check(f.Materials[0].Map("packed") != null && f.Materials[0].Blend == null, "packed/blend");
                    Check(f.Materials[0].Key != null && f.Materials[0].Key.Length == 40, "key");
                });
                Run("fixture.shared_materials", () =>
                {
                    // Models 102 and 103 have equal materials: one key, one table entry.
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    Check(m.SharedMaterials.Count == 2, "entries " + m.SharedMaterials.Count);
                    Check(m.Models[1].MaterialKeys.Count == 1 && m.Models[1].MaterialKeys[0] == m.Models[2].MaterialKeys[0], "102 = 103");
                    Check(m.Models[0].MaterialKeys[0] != m.Models[1].MaterialKeys[0], "101 differs");
                    WorldManifest.SharedMaterial e = m.SharedMaterials[1];
                    Check(e.Key == m.Models[1].MaterialKeys[0] && e.Model == 1 && e.Material == 0 && e.Slots == 2 && e.Name == "Mat_58247", "entry");
                    MaterialsFile f = MaterialsFile.Parse(File.ReadAllText(Path.Combine(dir, "models/103/materials.json")), "103");
                    Check(f.Materials[0].Key == e.Key, "materials.json key");
                });
            }
            Console.WriteLine(passed + " passed, " + failed + " failed");
            return failed == 0 ? 0 : 1;
        }
    }
}
