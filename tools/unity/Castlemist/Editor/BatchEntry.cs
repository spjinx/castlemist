// castlemist world export: batchmode entry points.
//
//   Unity.exe -batchmode -nographics -quit -projectPath <project>
//     -executeMethod Castlemist.BatchEntry.CheckProbe -castlemistFile Assets/probe.fbx
//     [-castlemistNoBakeAxis] -logFile -
//
// CheckProbe imports the probe .fbx with ModelImporter.bakeAxisConversion =
// FrameProbe.BakeAxisConversion (on; -castlemistNoBakeAxis turns it off to
// reproduce the failing case), instantiates it in a scratch scene, logs
// FrameProbe.Check's PROBE line and exits with 0 (OK) or 1.
//
//   ... -executeMethod Castlemist.BatchEntry.ImportWorld -castlemistWorld Assets/<export>
//     [-castlemistScale <s>]
//
// ImportWorld runs WorldImporter.Import on the folder (scale: the manifest's
// unityScale unless given), logs the warnings, the PROBE line and the PLAN
// line (validate_manifest.py --plan's format), then CELL lines checking that
// each cell's terrain lies under its Cell_<id> origin, INSTANCES checking the
// prop matrices and FOLIAGE_TREES the foliage tree instances (count, order,
// LODGroup and matrix against each record's m). Exit 0 when there are no
// errors, the probe passed (or there was none) and those checks passed.
//
//   ... -executeMethod Castlemist.BatchEntry.CheckSkinnedProbe -castlemistFile Assets/<dir>/skinned_probe.fbx
//     [-castlemistNoBakeAxis]
//
// CheckSkinnedProbe imports the skinned probe (cm_world_export_fixture
// --skinned-probe, then castlemist_world.py --to-fbx) as a world model is
// imported (bakeAxisConversion, Generic rig), measures it as instantiated
// (rest), then samples "ProbeClip" at its first and last frame, and logs
// SKINNED_PROBE with each bar's deformed centre per bone (rest | start | end;
// rest must equal start). Expected, in Unity axes (map east +X, north +Z, up +Y): East
// (150,0,0) -> (250,0,0); North (0,0,100) -> (-100,0,0) (turned 90 degrees
// about map up, so it points west); Up (0,50,0) -> (0,100,0). It also fails
// on inward triangles and mirrored transforms.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Castlemist
{
    public static class BatchEntry
    {
        public static void CheckProbe()
        {
            int code = 1;
            try
            {
                string file = Arg("-castlemistFile");
                if (string.IsNullOrEmpty(file)) throw new ArgumentException("-castlemistFile <Assets/...fbx> is required");
                bool bake = FrameProbe.BakeAxisConversion && !HasArg("-castlemistNoBakeAxis");

                AssetDatabase.ImportAsset(file, ImportAssetOptions.ForceUpdate);
                var importer = AssetImporter.GetAtPath(file) as ModelImporter;
                if (importer == null) throw new ArgumentException("not a model: " + file);
                // Readable, so FrameProbe can walk the triangles and normals.
                if (importer.bakeAxisConversion != bake || !importer.isReadable)
                {
                    importer.bakeAxisConversion = bake;
                    importer.isReadable = true;
                    importer.SaveAndReimport();
                }

                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(file);
                if (asset == null) throw new ArgumentException("no GameObject in " + file);
                EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                var root = (GameObject)UnityEngine.Object.Instantiate(asset);
                root.name = asset.name;

                string line;
                bool ok = FrameProbe.Check(root, out line);
                Debug.Log("CASTLEMIST bakeAxisConversion=" + (bake ? "1" : "0") + " unity=" + Application.unityVersion);
                Debug.Log(line);
                foreach (Transform t in root.GetComponentsInChildren<Transform>(true))
                    Debug.Log("PROBE_NODE " + t.name + " rot=" + t.localEulerAngles.ToString("F1") +
                              " scale=" + t.localScale.ToString("F3"));
                code = ok ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("PROBE ERROR " + e.Message);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        public static void ImportWorld()
        {
            int code = 1;
            try
            {
                string folder = Arg("-castlemistWorld");
                if (string.IsNullOrEmpty(folder)) throw new ArgumentException("-castlemistWorld <Assets/...> is required");
                folder = folder.Replace('\\', '/').TrimEnd('/');
                string scaleArg = Arg("-castlemistScale");
                float scale = scaleArg != null
                    ? float.Parse(scaleArg, CultureInfo.InvariantCulture)
                    : (float)WorldManifest.Parse(System.IO.File.ReadAllText(folder + "/manifest.json")).UnityScale;
                Debug.Log("CASTLEMIST unity=" + Application.unityVersion + " world=" + folder + " unityScale=" +
                          scale.ToString("R", CultureInfo.InvariantCulture) +
                          " poiyomi=" + (Shader.Find(PoiyomiMaterials.ShaderName) != null ? "found" : "absent"));
                ImportSummary sum = WorldImporter.Import(folder, scale);
                WorldImporter.Log(sum);
                bool cellsOk = CheckCells(folder);
                code = sum.errors.Count == 0 && (sum.probeLine == null || sum.probeOk) && cellsOk ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("CASTLEMIST IMPORT ERROR " + e);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        /// Each Cell_<id>'s terrain must be centred under the cell's own
        /// position (the cell origin is its rect centre): a check that the
        /// -origin offset of the cell .fbx content is right.
        static bool CheckCells(string folder)
        {
            WorldManifest m = WorldManifest.Parse(System.IO.File.ReadAllText(folder + "/manifest.json"));
            GameObject root = GameObject.Find("GW2_" + m.MapFileId);
            if (root == null) { Debug.LogError("CELL FAIL no root"); return false; }
            bool ok = true;
            int n = 0;
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                Transform cell = root.transform.Find("Cell_" + c.Id);
                Transform terrain = cell != null ? cell.Find("Terrain") : null;
                if (terrain == null) continue;
                Bounds b = terrain.GetComponent<Renderer>().bounds;
                Vector3 d = b.center - cell.position;
                float off = new Vector2(d.x, d.z).magnitude;
                float tol = 0.01f * Mathf.Max(b.size.x, b.size.z);
                bool good = off <= tol;
                if (!good || n < 3)
                    Debug.Log("CELL " + c.Id + " terrainCentreOffset=" + off.ToString("F4", CultureInfo.InvariantCulture) +
                              " size=" + b.size.ToString("F3") + (good ? " OK" : " FAIL"));
                ok &= good;
                ++n;
            }
            Debug.Log("CELLS checked=" + n + (ok ? " OK" : " FAIL"));

            // Each instance's world matrix against its map-space m, independently
            // of unity.t/r/s: world = root * C * m * C^-1 (C = frame.toUnity).
            double maxErr = 0;
            int checkedInst = 0;
            float scale = root.transform.localScale.x;
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                Transform props = root.transform.Find("Cell_" + c.Id + "/Props");
                if (props == null) continue;
                foreach (WorldManifest.InstanceEntry i in c.Instances)
                {
                    Transform t = props.Find(m.Models[i.Model].FileId + "_" + i.Prop);
                    if (t == null || i.M == null) continue;
                    Matrix4x4 got = root.transform.worldToLocalMatrix * t.localToWorldMatrix;  // map units, Unity axes
                    for (int r = 0; r < 3; ++r)
                        for (int k = 0; k < 4; ++k)
                        {
                            double want = 0;
                            if (k < 3)
                                for (int a = 0; a < 3; ++a)
                                    for (int b = 0; b < 3; ++b)
                                        want += m.ToUnity[r, a] * i.M[b * 4 + a] * m.ToUnity[k, b];  // C^-1 = C^T
                            else
                                for (int a = 0; a < 3; ++a) want += m.ToUnity[r, a] * i.M[12 + a];
                            double err = Math.Abs(got[r, k] - want) / Math.Max(1.0, Math.Abs(want));
                            if (err > maxErr) maxErr = err;
                        }
                    ++checkedInst;
                }
            }
            bool instOk = maxErr < 1e-4;
            Debug.Log("INSTANCES checked=" + checkedInst + " maxRelErr=" + maxErr.ToString("E2", CultureInfo.InvariantCulture) +
                      " rootScale=" + scale.ToString("R", CultureInfo.InvariantCulture) + (instOk ? " OK" : " FAIL"));
            return ok && instOk && CheckFoliage(folder, m, root);
        }

        /// World-foliage D8: Cell_<id>/Foliage/Trees holds one instance per tree
        /// record of an ok model, in record order, each with a LODGroup, and its
        /// world matrix equals root * C * m * C^-1 from the record's own m.
        static bool CheckFoliage(string folder, WorldManifest m, GameObject root)
        {
            double maxErr = 0;
            int checkedTrees = 0, want = 0;
            var fails = new List<string>();
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                if (c.FoliageFile == null) continue;
                List<FoliageRecord> recs = FoliageRecords.Read(folder + "/" + c.FoliageFile);
                var trees = recs.Where(r => FoliageRecords.IsTree(r, m.Foliage) && r.Model < m.Models.Count && m.Models[(int)r.Model].Ok).ToList();
                want += trees.Count;
                Transform parent = root.transform.Find("Cell_" + c.Id + "/Foliage/Trees");
                int have = parent != null ? parent.childCount : 0;
                if (have != trees.Count) { fails.Add(c.Id + ": " + have + " trees, " + trees.Count + " records"); continue; }
                for (int k = 0; k < have; ++k)
                {
                    Transform t = parent.GetChild(k);
                    FoliageRecord r = trees[k];
                    if (t.name != m.Models[(int)r.Model].FileId + "_" + r.Placement) fails.Add(c.Id + ": child " + k + " is " + t.name);
                    LODGroup lg = t.GetComponent<LODGroup>();
                    if (lg == null || lg.lodCount != 1) fails.Add(t.name + ": no LOD0-only LODGroup");
                    Matrix4x4 got = root.transform.worldToLocalMatrix * t.localToWorldMatrix;
                    for (int i = 0; i < 3; ++i)
                        for (int k2 = 0; k2 < 4; ++k2)
                        {
                            double w = 0;
                            if (k2 < 3)
                                for (int a = 0; a < 3; ++a)
                                    for (int b = 0; b < 3; ++b)
                                        w += m.ToUnity[i, a] * r.M[b * 3 + a] * m.ToUnity[k2, b];
                            else
                                for (int a = 0; a < 3; ++a) w += m.ToUnity[i, a] * r.M[9 + a];
                            double err = Math.Abs(got[i, k2] - w) / Math.Max(1.0, Math.Abs(w));
                            if (err > maxErr) maxErr = err;
                        }
                    ++checkedTrees;
                }
            }
            bool ok = fails.Count == 0 && maxErr < 1e-4;
            Debug.Log("FOLIAGE_TREES checked=" + checkedTrees + " records=" + want + " maxRelErr=" +
                      maxErr.ToString("E2", CultureInfo.InvariantCulture) + (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(5))));
            return ok;
        }

        public static void CheckSkinnedProbe()
        {
            int code = 1;
            try
            {
                string file = Arg("-castlemistFile");
                if (string.IsNullOrEmpty(file)) throw new ArgumentException("-castlemistFile <Assets/...fbx> is required");
                AssetDatabase.ImportAsset(file, ImportAssetOptions.ForceUpdate);
                var mi = AssetImporter.GetAtPath(file) as ModelImporter;
                if (mi == null) throw new ArgumentException("not a model: " + file);
                bool bake = FrameProbe.BakeAxisConversion && !HasArg("-castlemistNoBakeAxis");
                mi.bakeAxisConversion = bake;
                mi.isReadable = true;
                mi.animationType = ModelImporterAnimationType.Generic;
                mi.importAnimation = true;
                mi.SaveAndReimport();

                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(file);
                AnimationClip clip = AssetDatabase.LoadAllAssetsAtPath(file).OfType<AnimationClip>()
                    .FirstOrDefault(c => !c.name.StartsWith("__preview__") && c.name.EndsWith("ProbeClip"));
                if (asset == null || clip == null) throw new ArgumentException("no model or no ProbeClip in " + file);
                EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
                var go = (GameObject)UnityEngine.Object.Instantiate(asset);
                var smr = go.GetComponentInChildren<SkinnedMeshRenderer>();
                if (smr == null) throw new ArgumentException("no SkinnedMeshRenderer in " + file);

                var want = new Dictionary<string, Vector3[]>
                {
                    { "East", new[] { new Vector3(150, 0, 0), new Vector3(250, 0, 0) } },
                    { "North", new[] { new Vector3(0, 0, 100), new Vector3(-100, 0, 0) } },
                    { "Up", new[] { new Vector3(0, 50, 0), new Vector3(0, 100, 0) } },
                };
                var fails = new List<string>();
                var parts = new List<string>();
                bool neg = go.GetComponentsInChildren<Transform>(true).Any(t => t.localToWorldMatrix.determinant < 0f);
                Mesh src = smr.sharedMesh;
                BoneWeight[] bw = src.boneWeights;
                int[] tri = src.triangles;
                // k = 0: the pose as instantiated (no clip sampled: what a static
                // instance shows); 1: the clip's first frame; 2: its last frame.
                string[] when = { "@rest", "@start", "@end" };
                for (int k = 0; k < 3; ++k)
                {
                    if (k > 0) clip.SampleAnimation(go, k == 1 ? 0f : clip.length);
                    var baked = new Mesh();
                    smr.BakeMesh(baked, true);
                    Matrix4x4 w = Matrix4x4.TRS(smr.transform.position, smr.transform.rotation, Vector3.one);
                    Vector3[] v = baked.vertices.Select(p => w.MultiplyPoint3x4(p)).ToArray();
                    var sums = new Dictionary<string, Vector3>();
                    var counts = new Dictionary<string, int>();
                    var group = new string[v.Length];
                    for (int i = 0; i < v.Length; ++i)
                    {
                        string bone = smr.bones[bw[i].boneIndex0].name;
                        group[i] = bone;
                        sums[bone] = (sums.ContainsKey(bone) ? sums[bone] : Vector3.zero) + v[i];
                        counts[bone] = (counts.ContainsKey(bone) ? counts[bone] : 0) + 1;
                    }
                    foreach (KeyValuePair<string, Vector3[]> kv in want)
                    {
                        string key = sums.Keys.FirstOrDefault(n => n == kv.Key || n.EndsWith(kv.Key));
                        Vector3? got = key != null ? sums[key] / counts[key] : (Vector3?)null;
                        parts.Add(kv.Key + "=" + (got.HasValue ? Fmt(got.Value) : "none"));
                        if (!got.HasValue || (got.Value - kv.Value[k < 2 ? 0 : 1]).magnitude > 0.5f) fails.Add(kv.Key + when[k]);
                    }
                    int bad = 0;
                    for (int i = 0; i + 2 < tri.Length; i += 3)
                    {
                        Vector3 a = v[tri[i]], b = v[tri[i + 1]], c = v[tri[i + 2]];
                        Vector3 centre = sums[group[tri[i]]] / counts[group[tri[i]]];
                        if (Vector3.Dot(Vector3.Cross(b - a, c - a), (a + b + c) / 3f - centre) <= 0f) ++bad;
                    }
                    if (bad > 0) fails.Add("winding" + when[k] + "=" + bad);
                    if (k < 2) parts.Add("|");
                    UnityEngine.Object.DestroyImmediate(baked);
                }
                Debug.Log("CASTLEMIST unity=" + Application.unityVersion + " bakeAxisConversion=" + (bake ? "1" : "0") + " clip=" + clip.name + " length=" +
                          clip.length.ToString("F3", CultureInfo.InvariantCulture) + " bones=" +
                          string.Join(",", smr.bones.Select(b => b.name)));
                Debug.Log("SKINNED_PROBE " + string.Join(" ", parts) + " negScale=" + (neg ? 1 : 0) +
                          (fails.Count == 0 && !neg ? " OK" : " FAIL " + string.Join(",", fails)));
                code = fails.Count == 0 && !neg ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("SKINNED_PROBE ERROR " + e);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        static string Fmt(Vector3 v)
        {
            return "(" + (v.x + 0f).ToString("F1", CultureInfo.InvariantCulture) + "," +
                   (v.y + 0f).ToString("F1", CultureInfo.InvariantCulture) + "," +
                   (v.z + 0f).ToString("F1", CultureInfo.InvariantCulture) + ")";
        }

        static string Arg(string name)
        {
            string[] a = Environment.GetCommandLineArgs();
            for (int i = 0; i + 1 < a.Length; ++i)
                if (a[i] == name) return a[i + 1];
            return null;
        }

        static bool HasArg(string name)
        {
            return Array.IndexOf(Environment.GetCommandLineArgs(), name) >= 0;
        }
    }
}
