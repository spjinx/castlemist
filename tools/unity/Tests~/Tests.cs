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
            Run("summary.plan_line", () =>
            {
                var s = new ImportSummary { materials = 4, prefabs = 2, controllers = 1, cells = 1, instances = 4, colliders = 1, terrainColliders = 1, water = 1, lights = 1, skybox = 0 };
                // Task 9's expected PLAN line for the fixture, verbatim.
                Check(s.PlanLine() == "PLAN materials=4 prefabs=2 controllers=1 cells=1 instances=4 colliders=1 terrainColliders=1 water=1 lights=1 skybox=0", s.PlanLine());
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

            if (args.Length > 0)
            {
                string dir = args[0];
                Run("fixture.manifest", () =>
                {
                    WorldManifest m = WorldManifest.Parse(File.ReadAllText(Path.Combine(dir, "manifest.json")));
                    Check(m.Models.Count == 2 && m.Models[1].Clips[0] == "TestClip", "models");
                    Check(m.Cells.Count == 1 && m.Cells[0].Instances.Count == 4 && m.Cells[0].HasTerrain, "cells");
                    Check(m.Cells[0].ColliderParts == 1 && m.Cells[0].WaterTexture != null, "collider/water");
                    WorldManifest.InstanceEntry i = m.Cells[0].Instances[3];
                    Check(i.Model == 1 && i.Clip == "TestClip" && i.S == 3.0 && i.T[0] == 2288.0 && i.R.Length == 4, "instance");
                    Check(m.Light != null && m.Light.Ambient.Length == 3 && m.Sky == null && m.FrameProbe.Fbx == "probe.fbx", "light/sky/probe");
                    double[] o = m.MapToUnity(m.Cells[0].Origin);
                    Check(o[0] == -12288.0 && o[2] == 36864.0, "origin");
                });
                Run("fixture.materials", () =>
                {
                    MaterialsFile f = MaterialsFile.Parse(File.ReadAllText(Path.Combine(dir, "models/102/materials.json")), "102");
                    Check(f.Materials.Count == 1 && f.Materials[0].Map("baseColor").File == "Textures/Mat_58247 - BaseColor.png", "baseColor");
                    Check(f.Materials[0].Map("packed") != null && f.Materials[0].Blend == null, "packed/blend");
                });
            }
            Console.WriteLine(passed + " passed, " + failed + " failed");
            return failed == 0 ? 0 : 1;
        }
    }
}
