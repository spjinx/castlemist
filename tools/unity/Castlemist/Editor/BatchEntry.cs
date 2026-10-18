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
//     [-castlemistScale <s>] [-castlemistTerrain mesh|terrain]
//     [-castlemistFoliage off|colliders|trees|all] [-castlemistDensity f]
//     [-castlemistDistance f] [-castlemistQuest]
//     [-castlemistExpectShader "<Poiyomi shader name>"|Standard] [-castlemistScreenshot <file.png>]
//
// ImportWorld runs WorldImporter.Import on the folder (scale: the manifest's
// unityScale unless given; options: ImportOptions, -castlemistQuest the Quest
// preset, then the other flags on top), logs the warnings, the PROBE line, the
// TERRAIN_CHECK line (Terrain mode) and the PLAN line (validate_manifest.py
// --plan --mode --foliage --foliage-density --foliage-distance's format), then
// CELL lines checking that each cell's terrain lies under its Cell_<id> origin,
// INSTANCES checking the prop matrices, FOLIAGE_TREES the foliage tree
// instances (count, order, LODGroup and matrix against each record's m, as
// FoliagePlan places them), FOLIAGE_PATCHES the patches (count, LODGroup,
// at most 65 535 vertices unless one record, every record's position inside
// its patch's bounds) and, in Terrain mode, TERRAIN_TREES (each TreeInstance's
// position against its record's m), then LODS (Task 15b: every model prefab with
// lods.levels > 1 has a LODGroup of those levels with strictly decreasing heights equal
// to ModelLods.TransitionHeights, tree prefabs the levels their cull leaves, and each
// instances[].lod instance its own heights; CheckLods), then MATERIALS (Task 15d: one Shared .mat
// per key and variant, never two; every prefab renderer draws its model's shared .mat; CheckMaterials), then
// POIYOMI_MATERIALS (Task 15c: the shader chosen, -castlemistExpectShader's when given, on every .mat, without
// compile errors, and each shared .mat's main texture, colour, cutoff, cull, queue, preset, pan and keywords as
// set; CheckShader), and with -castlemistScreenshot a SCREENSHOT line (a Camera.Render of a few props; skipped
// under -nographics). Exit 0 when there are no errors, the probe
// and the terrain frame check passed (or did not run) and those checks passed.
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
                ImportOptions opts = Options();
                Debug.Log("CASTLEMIST unity=" + Application.unityVersion + " world=" + folder + " unityScale=" +
                          scale.ToString("R", CultureInfo.InvariantCulture) + " " + opts);
                ImportSummary sum = WorldImporter.Import(folder, scale, opts);
                WorldImporter.Log(sum);
                bool cellsOk = CheckCells(folder, opts);
                bool shaderOk = CheckShader(folder);
                string shot = Arg("-castlemistScreenshot");
                bool shotOk = shot == null || Screenshot(folder, shot);
                code = sum.errors.Count == 0 && (sum.probeLine == null || sum.probeOk) && sum.terrainCheckOk && cellsOk && shaderOk && shotOk ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("CASTLEMIST IMPORT ERROR " + e);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        /// The import options from the command line (ImportOptions defaults; -castlemistQuest first).
        static ImportOptions Options()
        {
            ImportOptions o = HasArg("-castlemistQuest") ? ImportOptions.Quest() : new ImportOptions();
            string a;
            if ((a = Arg("-castlemistTerrain")) != null) o.terrain = ImportOptions.ParseTerrain(a);
            if ((a = Arg("-castlemistFoliage")) != null) o.foliage = ImportOptions.ParseFoliage(a);
            if ((a = Arg("-castlemistDensity")) != null) o.density = float.Parse(a, CultureInfo.InvariantCulture);
            if ((a = Arg("-castlemistDistance")) != null) o.distance = float.Parse(a, CultureInfo.InvariantCulture);
            o.Validate();
            return o;
        }

        /// Each Cell_<id>'s terrain must be centred under the cell's own
        /// position (the cell origin is its rect centre): a check that the
        /// -origin offset of the cell .fbx content is right.
        static bool CheckCells(string folder, ImportOptions opts)
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
            if (m.UnityTerrain == null) opts = new ImportOptions { terrain = TerrainMode.Mesh, foliage = opts.foliage, density = opts.density, distance = opts.distance };
            var errs = new List<string>();
            FoliagePlan plan = FoliagePlan.Build(m, c => FoliageRecords.Read(folder + "/" + c.FoliageFile), opts, errs);
            bool foliageOk = CheckFoliage(m, root, plan, folder, opts);
            bool patchesOk = CheckPatches(m, root, plan);
            bool terrainOk = plan.Mode != TerrainMode.UnityTerrain || CheckTerrainTrees(m, plan);
            bool lodsOk = CheckLods(folder, m, root, opts);
            bool materialsOk = CheckMaterials(folder, m);
            return ok && instOk && foliageOk && patchesOk && terrainOk && lodsOk && materialsOk;
        }

        /// MATERIALS (Task 15d): one .mat per (key, dark decal, uvScroll) in Materials/Shared,
        /// never two for one: per key, the .mat files named for it ("<key>.mat",
        /// "<key>_<8 hex>.mat") are exactly its variants the models use; and every renderer of
        /// every model prefab and foliage tree prefab (LOD levels included) draws a Shared .mat
        /// whose key is one of that model's materialKeys. Skipped (with the line) for an export
        /// without keys.
        static bool CheckMaterials(string folder, WorldManifest m)
        {
            var fails = new List<string>();
            var variantsOfKey = new Dictionary<string, HashSet<string>>(StringComparer.Ordinal);
            int keyed = 0, renderers = 0;
            var drawn = new HashSet<Material>();
            string sharedDir = folder + "/Materials/Shared/";
            foreach (WorldManifest.ModelEntry e in m.Models)
            {
                if (!e.Ok || e.Materials == null || e.MaterialKeys.Count == 0) continue;
                ++keyed;
                MaterialsFile f = MaterialsFile.Parse(System.IO.File.ReadAllText(folder + "/" + e.Materials), e.Materials);
                if (f.Materials.Count != e.MaterialKeys.Count) fails.Add(e.FileId + ": " + f.Materials.Count + " materials, " + e.MaterialKeys.Count + " keys");
                foreach (MaterialsFile.MaterialEntry me in f.Materials)
                {
                    if (!me.UsedByMeshes || me.Key == null) continue;
                    bool dark = m.DarkDecalRule && e.DarkDecalMaterials.Contains(me.Name);
                    WorldManifest.UvScroll sc = e.UvScroll.FirstOrDefault(u => u.Material == me.Name);
                    HashSet<string> vs;
                    if (!variantsOfKey.TryGetValue(me.Key, out vs)) variantsOfKey[me.Key] = vs = new HashSet<string>(StringComparer.Ordinal);
                    vs.Add(WorldImporter.SharedMaterialFile(me.Key, WorldImporter.SharedMaterialVariant(me.Key, dark, sc)));
                }
                var keys = new HashSet<string>(e.MaterialKeys, StringComparer.Ordinal);
                foreach (string pp in new[] { folder + "/Prefabs/" + e.FileId + ".prefab", folder + "/Prefabs/Foliage/" + e.FileId + ".prefab" })
                {
                    var p = AssetDatabase.LoadAssetAtPath<GameObject>(pp);
                    if (p == null) continue;
                    foreach (Renderer r in p.GetComponentsInChildren<Renderer>(true))
                        foreach (Material sm in r.sharedMaterials)
                        {
                            if (sm == null || sm.name == "Default-Material") continue;   // warned at import
                            ++renderers;
                            string path = AssetDatabase.GetAssetPath(sm);
                            string stem = System.IO.Path.GetFileNameWithoutExtension(path);
                            if (!path.StartsWith(sharedDir, StringComparison.Ordinal) || stem.Length < 40 || !keys.Contains(stem.Substring(0, 40)))
                                fails.Add(r.gameObject.name + " in " + pp + ": " + path + " is not one of the model's shared materials");
                            else drawn.Add(sm);
                        }
                }
            }
            if (keyed == 0)
            {
                Debug.Log("MATERIALS keyed=0 (an export without material keys): skipped");
                return true;
            }
            var onDisk = new Dictionary<string, List<string>>(StringComparer.Ordinal);   // key -> .mat stems
            foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { folder + "/Materials/Shared" }))
            {
                string stem = System.IO.Path.GetFileNameWithoutExtension(AssetDatabase.GUIDToAssetPath(guid));
                if (stem.Length < 40) { fails.Add("Materials/Shared/" + stem + ".mat: not named by a key"); continue; }
                string key = stem.Substring(0, 40);
                List<string> l;
                if (!onDisk.TryGetValue(key, out l)) onDisk[key] = l = new List<string>();
                l.Add(stem);
            }
            int variants = 0;
            foreach (KeyValuePair<string, HashSet<string>> kv in variantsOfKey)
            {
                variants += kv.Value.Count;
                List<string> have;
                onDisk.TryGetValue(kv.Key, out have);
                var got = new HashSet<string>(have ?? new List<string>(), StringComparer.Ordinal);
                if ((have == null ? 0 : have.Count) != kv.Value.Count || !got.SetEquals(kv.Value))
                    fails.Add("key " + kv.Key + ": " + (have == null ? 0 : have.Count) + " .mat files, " + kv.Value.Count + " variants used");
            }
            bool ok = fails.Count == 0;
            Debug.Log("MATERIALS keyed=" + keyed + " keys=" + variantsOfKey.Count + " shared=" + variants + " drawn=" + drawn.Count +
                      " renderers=" + renderers + (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(8))));
            return ok;
        }

        /// SHADER / POIYOMI_MATERIALS (Task 15c): the shader the import chose (MaterialContext.Create, run again:
        /// it is deterministic) is -castlemistExpectShader's ("Standard" for none) when given; every .mat under
        /// Materials/ except Skybox.mat uses it; no shader the materials use has compile errors; and every used keyed materials.json
        /// entry's shared .mat reads back what PoiyomiMaterials set from it: _MainTex (the pooled file), _Color,
        /// _Cutoff (Cutout and TransClipping), _Cull, the render queue, and with Poiyomi _Mode, the RenderType tag of
        /// the shader's own preset row, _MainTexPan (uvScroll on the base colour's UV), MOCHIE_PBR for a packed map
        /// and _EMISSION for emission.
        static bool CheckShader(string folder)
        {
            WorldManifest m = WorldManifest.Parse(System.IO.File.ReadAllText(folder + "/manifest.json"));
            MaterialContext ctx = MaterialContext.Create(new List<string>());
            var fails = new List<string>();
            string got = ctx.Poiyomi ? ctx.ShaderName : "Standard";
            string expect = Arg("-castlemistExpectShader");
            if (expect != null && expect != got) fails.Add("shader " + got + ", expected " + expect);
            var all = new HashSet<Material>();
            foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { folder + "/Materials" }))
            {
                string mp = AssetDatabase.GUIDToAssetPath(guid);
                if (mp == folder + "/Materials/Skybox.mat") continue;   // Skybox/Panoramic by design
                var mat = AssetDatabase.LoadAssetAtPath<Material>(mp);
                if (mat == null) continue;
                all.Add(mat);
                if (mat.shader != ctx.Shader) fails.Add(mat.name + ": shader " + mat.shader.name);
            }
            var shaders = new HashSet<Shader>(all.Select(x => x.shader));
            foreach (Shader s in shaders)
                if (PoiyomiMaterials.HasErrors(s)) fails.Add("shader " + s.name + " has compile errors");

            int n = 0, tex = 0, cut = 0, cullOff = 0, pan = 0, packed = 0, emis = 0;
            Func<float, double, bool> near = (a, b) => Math.Abs(a - b) <= 1e-5 * Math.Max(1.0, Math.Abs(b));
            foreach (WorldManifest.ModelEntry e in m.Models)
            {
                if (!e.Ok || e.Materials == null || e.MaterialKeys.Count == 0) continue;
                string rel = folder + "/" + e.Materials;
                MaterialsFile f = MaterialsFile.Parse(System.IO.File.ReadAllText(rel), e.Materials);
                string modelFolder = System.IO.Path.GetDirectoryName(rel).Replace('\\', '/');
                foreach (MaterialsFile.MaterialEntry me in f.Materials)
                {
                    if (!me.UsedByMeshes || me.Key == null) continue;
                    bool dark = m.DarkDecalRule && e.DarkDecalMaterials.Contains(me.Name);
                    WorldManifest.UvScroll sc = e.UvScroll.FirstOrDefault(u => u.Material == me.Name);
                    string path = folder + "/Materials/Shared/" + WorldImporter.SharedMaterialFile(me.Key, WorldImporter.SharedMaterialVariant(me.Key, dark, sc)) + ".mat";
                    var mat = AssetDatabase.LoadAssetAtPath<Material>(path);
                    string tag = e.FileId + "/" + me.Name;
                    if (mat == null) { fails.Add(tag + ": no " + path); continue; }
                    ++n;
                    int mode = dark ? PoiyomiMaterials.Fade : me.Mode;
                    var bad = new List<string>();
                    if (mat.shader != ctx.Shader) bad.Add("shader " + mat.shader.name);
                    MaterialsFile.MapRef bc = me.Map("baseColor");
                    if (bc != null)
                    {
                        string want = PoiyomiMaterials.AssetPath(modelFolder + "/" + bc.File.Replace('\\', '/'));
                        Texture t = mat.HasProperty("_MainTex") ? mat.GetTexture("_MainTex") : null;
                        string have = t != null ? AssetDatabase.GetAssetPath(t) : "none";
                        if (have != want) bad.Add("_MainTex " + have + ", want " + want);
                        else ++tex;
                    }
                    if (mat.GetColor("_Color") != Color.white) bad.Add("_Color " + mat.GetColor("_Color"));
                    if (mode == PoiyomiMaterials.Cutout || mode == PoiyomiMaterials.TransClipping)
                    {
                        if (!near(mat.GetFloat("_Cutoff"), me.AlphaCutoff)) bad.Add("_Cutoff " + mat.GetFloat("_Cutoff") + ", want " + me.AlphaCutoff);
                        ++cut;
                    }
                    float cull = me.Cull == "Off" ? 0f : 2f;
                    if (mat.HasProperty("_Cull") && mat.GetFloat("_Cull") != cull) bad.Add("_Cull " + mat.GetFloat("_Cull"));   // Standard has none
                    if (cull == 0f) ++cullOff;
                    if (mat.renderQueue != ctx.QueueFor(mode) + me.RenderQueueOffset) bad.Add("queue " + mat.renderQueue);
                    if (ctx.Poiyomi)
                    {
                        if (mat.GetFloat("_Mode") != mode) bad.Add("_Mode " + mat.GetFloat("_Mode") + ", want " + mode);
                        PoiyomiShaders.PresetActions pa;
                        if (ctx.Presets.TryGetValue(mode, out pa) && pa.RenderType != null && mat.GetTag("RenderType", false) != pa.RenderType)
                            bad.Add("RenderType " + mat.GetTag("RenderType", false) + ", want " + pa.RenderType);
                        if (sc != null && bc != null && bc.Uv == sc.UvIndex)
                        {
                            Vector4 v = mat.GetVector("_MainTexPan");
                            if (!near(v.x, 20.0 * sc.U) || !near(v.y, -20.0 * sc.V)) bad.Add("_MainTexPan " + v);
                            ++pan;
                        }
                        if (me.Map("packed") != null)
                        {
                            if (!mat.IsKeywordEnabled("MOCHIE_PBR") || mat.GetFloat("_MochieBRDF") != 1f) bad.Add("packed: MOCHIE_PBR off");
                            ++packed;
                        }
                        if (me.HasEmission || me.Map("emissionMap") != null || me.Map("emissionMask") != null || me.Map("emissionBaked") != null)
                        {
                            if (!mat.IsKeywordEnabled("_EMISSION") || mat.GetFloat("_EnableEmission") != 1f) bad.Add("emission: _EMISSION off");
                            ++emis;
                        }
                    }
                    if (bad.Count > 0) fails.Add(tag + " (" + path + "): " + string.Join(", ", bad));
                }
            }
            bool ok = fails.Count == 0;
            Debug.Log("POIYOMI_MATERIALS shader=" + got + " version=" + (ctx.Version ?? "-") + " checked=" + n + " mats=" + all.Count +
                      " shaders=" + shaders.Count + " mainTex=" + tex + " cutout=" + cut + " cullOff=" + cullOff + " panned=" + pan +
                      " packed=" + packed + " emission=" + emis + (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(8))));
            return ok;
        }

        /// SCREENSHOT (Task 15c): up to 8 model prefabs, picked to cover opaque, cutout, blended, panned, packed and
        /// emissive materials, each scaled to a 1-unit box, in a 4 x 2 grid under a sun and a grey flat ambient,
        /// rendered by Camera.Render to `png` (1600 x 800). Fails on a shader error after the render, or when over
        /// 1% of the pixels are Unity's magenta error colour. Needs a graphics device (no -nographics).
        static bool Screenshot(string folder, string png)
        {
            if (SystemInfo.graphicsDeviceType == UnityEngine.Rendering.GraphicsDeviceType.Null)
            {
                Debug.Log("SCREENSHOT skipped: no graphics device (-nographics)");
                return true;
            }
            ShaderUtil.allowAsyncCompilation = false;   // no cyan placeholder shaders in the render
            WorldManifest m = WorldManifest.Parse(System.IO.File.ReadAllText(folder + "/manifest.json"));
            var covered = new HashSet<string>();
            var picks = new List<GameObject>();
            var pickNames = new List<string>();
            for (int pass = 0; pass < 2 && picks.Count < 8; ++pass)
                foreach (WorldManifest.ModelEntry e in m.Models)
                {
                    if (picks.Count >= 8) break;
                    if (!e.Ok || e.Materials == null) continue;
                    var p = AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Prefabs/" + e.FileId + ".prefab");
                    if (p == null || picks.Contains(p) || p.GetComponentInChildren<Renderer>() == null) continue;
                    MaterialsFile f = MaterialsFile.Parse(System.IO.File.ReadAllText(folder + "/" + e.Materials), e.Materials);
                    var feats = new HashSet<string>();
                    foreach (MaterialsFile.MaterialEntry me in f.Materials)
                    {
                        if (!me.UsedByMeshes) continue;
                        feats.Add(me.Mode == PoiyomiMaterials.Opaque ? "opaque" : me.Mode == PoiyomiMaterials.Cutout || me.Mode == PoiyomiMaterials.TransClipping ? "cutout" : "blend");
                        if (e.UvScroll.Any(u => u.Material == me.Name)) feats.Add("pan");
                        if (me.Map("packed") != null) feats.Add("packed");
                        if (me.HasEmission) feats.Add("emission");
                    }
                    if (pass == 0 && feats.All(covered.Contains)) continue;   // first pass: something new each
                    covered.UnionWith(feats);
                    picks.Add(p);
                    pickNames.Add(e.FileId + "[" + string.Join("+", feats.OrderBy(x => x)) + "]");
                }
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Flat;
            RenderSettings.ambientLight = new Color(0.45f, 0.45f, 0.45f);
            var sun = new GameObject("Sun").AddComponent<Light>();
            sun.type = LightType.Directional;
            sun.intensity = 1.1f;
            sun.transform.rotation = Quaternion.Euler(40f, -30f, 0f);
            for (int i = 0; i < picks.Count; ++i)
            {
                var go = (GameObject)UnityEngine.Object.Instantiate(picks[i]);
                Bounds b = Bounds(go);
                float s = 1f / Mathf.Max(1e-6f, Mathf.Max(b.size.x, Mathf.Max(b.size.y, b.size.z)));
                go.transform.localScale *= s;
                b = Bounds(go);
                Vector3 cell = new Vector3((i % 4 - 1.5f) * 1.3f, (i < 4 ? 0.65f : -0.65f), 0f);
                go.transform.position += cell - b.center;
            }
            var cam = new GameObject("Camera").AddComponent<Camera>();
            cam.transform.position = new Vector3(0f, 0.8f, -5.6f);
            cam.transform.LookAt(new Vector3(0f, 0f, 0f));
            cam.fieldOfView = 40f;
            cam.clearFlags = CameraClearFlags.SolidColor;
            cam.backgroundColor = new Color(0.2f, 0.22f, 0.25f);
            var rt = new RenderTexture(1600, 800, 24);
            cam.targetTexture = rt;
            cam.Render();
            RenderTexture.active = rt;
            var img = new Texture2D(rt.width, rt.height, TextureFormat.RGB24, false);
            img.ReadPixels(new Rect(0, 0, rt.width, rt.height), 0, 0);
            img.Apply();
            RenderTexture.active = null;
            cam.targetTexture = null;
            System.IO.File.WriteAllBytes(png, img.EncodeToPNG());
            int magenta = 0;
            Color32[] px = img.GetPixels32();
            foreach (Color32 c in px) if (c.r > 230 && c.g < 25 && c.b > 230) ++magenta;
            var errs = new List<string>();
            foreach (Renderer r in UnityEngine.Object.FindObjectsOfType<Renderer>())
                foreach (Material mt in r.sharedMaterials)
                    if (mt != null && PoiyomiMaterials.HasErrors(mt.shader) && !errs.Contains(mt.shader.name)) errs.Add(mt.shader.name);
            bool ok = errs.Count == 0 && magenta * 100 <= px.Length;
            Debug.Log("SCREENSHOT file=" + png + " props=" + string.Join(",", pickNames) + " magentaPixels=" + magenta +
                      " shaderErrors=" + errs.Count + (ok ? " OK" : " FAIL " + string.Join(", ", errs)));
            return ok;
        }

        /// LOCK_PROBE (Task 15c): an imported world's materials can be locked by ThryEditor, as its VRChat upload
        /// hook does. Without a compile-time reference: ShaderOptimizer.LockMaterials / IsMaterialLocked /
        /// UnlockMaterials by reflection on up to -castlemistLockProbe (default 6) Materials/Shared .mat files,
        /// one per _Mode first. Each must come out locked on a "Hidden/Locked/..." shader without compile errors,
        /// then unlocked on its original shader again (the import leaves materials unlocked).
        ///   ... -executeMethod Castlemist.BatchEntry.LockProbe -castlemistWorld Assets/<export> [-castlemistLockProbe n]
        public static void LockProbe()
        {
            int code = 1;
            try
            {
                string folder = Arg("-castlemistWorld").Replace('\\', '/').TrimEnd('/');
                string na = Arg("-castlemistLockProbe");
                int n = na != null ? int.Parse(na, CultureInfo.InvariantCulture) : 6;
                Type t = AppDomain.CurrentDomain.GetAssemblies().Select(a => a.GetType("Thry.ThryEditor.ShaderOptimizer", false)).FirstOrDefault(x => x != null);
                if (t == null) { Debug.Log("LOCK_PROBE skipped: ThryEditor is not in this project"); EditorApplication.Exit(0); return; }
                var flags = System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static;
                System.Reflection.MethodInfo lockM = t.GetMethod("LockMaterials", flags), unlockM = t.GetMethod("UnlockMaterials", flags),
                                             isLocked = t.GetMethod("IsMaterialLocked", flags);
                var all = AssetDatabase.FindAssets("t:Material", new[] { folder + "/Materials/Shared" })
                    .Select(g => AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(g))).Where(x => x != null)
                    .OrderBy(x => x.name, StringComparer.Ordinal).ToList();
                var mats = all.GroupBy(x => x.HasProperty("_Mode") ? x.GetFloat("_Mode") : 0f).Select(g => g.First()).ToList();
                mats.AddRange(all.Where(x => !mats.Contains(x)));
                mats = mats.Take(n).ToList();
                var original = mats.ToDictionary(x => x, x => x.shader);
                object none = Enum.ToObject(lockM.GetParameters()[1].ParameterType, 0);
                var fails = new List<string>();
                var sw = System.Diagnostics.Stopwatch.StartNew();
                bool lr = (bool)lockM.Invoke(null, new object[] { mats, none });
                double lockSeconds = sw.Elapsed.TotalSeconds;
                var locked = new List<string>();
                foreach (Material x in mats)
                {
                    bool il = (bool)isLocked.Invoke(null, new object[] { x });
                    if (!il || !x.shader.name.StartsWith("Hidden/Locked/", StringComparison.Ordinal)) fails.Add(x.name + ": not locked (" + x.shader.name + ")");
                    else if (PoiyomiMaterials.HasErrors(x.shader)) fails.Add(x.name + ": locked shader " + x.shader.name + " has errors");
                    else locked.Add("_Mode " + x.GetFloat("_Mode") + " -> " + x.shader.name);
                }
                bool ur = (bool)unlockM.Invoke(null, new object[] { mats, none });
                foreach (Material x in mats)
                    if (x.shader != original[x]) fails.Add(x.name + ": unlocked to " + x.shader.name);
                foreach (string l in locked) Debug.Log("LOCKED " + l);
                bool ok = lr && ur && fails.Count == 0;
                Debug.Log("LOCK_PROBE materials=" + mats.Count + " locked=" + locked.Count + " lockSeconds=" +
                          lockSeconds.ToString("F1", CultureInfo.InvariantCulture) + " unlocked=" + (ur ? "yes" : "no") +
                          (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(8))));
                code = ok ? 0 : 1;
            }
            catch (Exception e)
            {
                Debug.LogError("LOCK_PROBE ERROR " + e);
                code = 1;
            }
            EditorApplication.Exit(code);
        }

        static Bounds Bounds(GameObject go)
        {
            Renderer[] rs = go.GetComponentsInChildren<Renderer>();
            Bounds b = rs[0].bounds;
            foreach (Renderer r in rs) b.Encapsulate(r.bounds);
            return b;
        }

        static double FadeFar(WorldManifest m, int model)
        {
            return (m.Foliage != null ? m.Foliage.FadeFar(model) : 0) * m.FadeQualityScale;
        }

        /// The tree prefab's expected level count: the model's levels less those its cull drops, or 1.
        static int TreeLevels(WorldManifest m, int model, LODGroup lg, ImportOptions opts, out double[] want)
        {
            WorldManifest.ModelEntry e = m.Models[model];
            want = null;
            if (e.LodLevels <= 1 || lg == null) return 1;
            want = ModelLods.TransitionHeights(lg.size, e.LodDistances, e.LodLevels,
                                              FoliageRecords.CullHeight(lg.size, FadeFar(m, model), opts.distance));
            return want.Length;
        }

        static string Heights(LODGroup lg, double[] want)
        {
            LOD[] lods = lg.GetLODs();
            for (int k = 0; k < want.Length && k < lods.Length; ++k)
                if (Math.Abs(lods[k].screenRelativeTransitionHeight - want[k]) > 1e-6 + 1e-5 * want[k])
                    return "LOD" + k + " height " + lods[k].screenRelativeTransitionHeight.ToString("R", CultureInfo.InvariantCulture) +
                           ", want " + want[k].ToString("R", CultureInfo.InvariantCulture);
            return null;
        }

        /// Every "<b>_LOD<k>" renderer under `root` has exactly its LOD0 "<b>" renderer's sharedMaterials
        /// (the same .mat assets, never a copy per level); null when they all do.
        static string SameMaterials(GameObject root)
        {
            Renderer[] rs = root.GetComponentsInChildren<Renderer>(true);
            var lod0 = new Dictionary<string, Renderer>(StringComparer.Ordinal);
            foreach (Renderer r in rs)
            {
                string b;
                if (ModelLods.LevelOf(r.gameObject.name, out b) == 0 && !lod0.ContainsKey(b)) lod0[b] = r;
            }
            foreach (Renderer r in rs)
            {
                string b;
                if (ModelLods.LevelOf(r.gameObject.name, out b) == 0) continue;
                Renderer z;
                if (!lod0.TryGetValue(b, out z)) continue;   // an orphan: warned at import
                Material[] a = r.sharedMaterials, c = z.sharedMaterials;
                if (a.Length != c.Length || a.Where((mat, i) => !ReferenceEquals(mat, c[i])).Any())
                    return r.gameObject.name + " materials differ from " + z.gameObject.name + "'s";
            }
            return null;
        }

        /// LODS (Task 15b): every model prefab whose model has lods.levels > 1 has a LODGroup with that
        /// many levels, strictly decreasing heights equal to ModelLods.TransitionHeights of its size and
        /// the model's distances, every LOD with renderers, each level drawing as many renderers as LOD0,
        /// every active renderer in some level and every LOD renderer with its LOD0's materials; a model without LODs has no LODGroup. Each tree
        /// prefab of such a model has the levels its cull leaves (TreeLevels); each instance with
        /// instances[].lod has the heights of its own distances.
        static bool CheckLods(string folder, WorldManifest m, GameObject root, ImportOptions opts)
        {
            var fails = new List<string>();
            int prefabs = 0, plain = 0, trees = 0, overrides = 0, levelsSum = 0;
            foreach (WorldManifest.ModelEntry e in m.Models)
            {
                if (!e.Ok) continue;
                var p = AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Prefabs/" + e.FileId + ".prefab");
                if (p == null) continue;
                LODGroup lg = p.GetComponent<LODGroup>();
                if (e.LodLevels <= 1)
                {
                    if (lg != null) fails.Add(e.FileId + ": a LODGroup on a model without LODs");
                    ++plain;
                }
                else
                {
                    string why = LodGroups.Problem(lg, e.LodLevels);
                    if (why == null) why = Heights(lg, ModelLods.TransitionHeights(lg.size, e.LodDistances, e.LodLevels, ModelLods.PropCullHeight));
                    if (why == null)
                    {
                        LOD[] lods = lg.GetLODs();
                        var inLod = new HashSet<Renderer>(lods.SelectMany(l => l.renderers));
                        if (lods.Any(l => l.renderers.Length != lods[0].renderers.Length)) why = "levels draw different renderer counts";
                        else if (p.GetComponentsInChildren<Renderer>(false).Any(r => !inLod.Contains(r))) why = "an active renderer in no level";
                        else why = SameMaterials(p);
                    }
                    if (why != null) fails.Add(e.FileId + ": " + why);
                    ++prefabs;
                    levelsSum += e.LodLevels;
                }
                var tp = AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Prefabs/Foliage/" + e.FileId + ".prefab");
                if (tp != null && e.LodLevels > 1)
                {
                    LODGroup tl = tp.GetComponent<LODGroup>();
                    double[] want;
                    int n = TreeLevels(m, e.Position, tl, opts, out want);
                    string why = LodGroups.Problem(tl, n);
                    if (why == null && want != null) why = Heights(tl, want);
                    if (why == null)
                    {
                        var inLod = new HashSet<Renderer>(tl.GetLODs().SelectMany(l => l.renderers));
                        if (tp.GetComponentsInChildren<Renderer>(false).Any(r => !inLod.Contains(r))) why = "an active renderer in no level";
                        else why = SameMaterials(tp);
                    }
                    if (why != null) fails.Add("tree " + e.FileId + ": " + why);
                    ++trees;
                }
            }
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                Transform props = root.transform.Find("Cell_" + c.Id + "/Props");
                if (props == null) continue;
                foreach (WorldManifest.InstanceEntry i in c.Instances)
                {
                    if (i.Lod == null || i.Model < 0 || i.Model >= m.Models.Count || m.Models[i.Model].LodLevels <= 1) continue;
                    Transform t = props.Find(m.Models[i.Model].FileId + "_" + i.Prop);
                    if (t == null) continue;
                    LODGroup lg = t.GetComponent<LODGroup>();
                    string why = LodGroups.Problem(lg, m.Models[i.Model].LodLevels);
                    if (why == null) why = Heights(lg, ModelLods.TransitionHeights(lg.size, i.Lod, m.Models[i.Model].LodLevels, ModelLods.PropCullHeight));
                    if (why != null) fails.Add("instance " + t.name + ": " + why);
                    ++overrides;
                }
            }
            bool ok = fails.Count == 0;
            Debug.Log("LODS prefabs=" + prefabs + " levels=" + levelsSum + " withoutLods=" + plain + " trees=" + trees +
                      " overrides=" + overrides + (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(8))));
            return ok;
        }

        /// The map-space translation of a record in Unity world space: root * C * t.
        static Vector3 WorldOf(WorldManifest m, GameObject root, FoliageRecord r)
        {
            var u = new Vector3();
            for (int i = 0; i < 3; ++i)
                u[i] = (float)(m.ToUnity[i, 0] * r.M[9] + m.ToUnity[i, 1] * r.M[10] + m.ToUnity[i, 2] * r.M[11]);
            return root.transform.TransformPoint(u);
        }

        /// FOLIAGE_PATCHES: Cell_<id>/Foliage/Patches holds the plan's patches, each with a LOD0-only LODGroup,
        /// at most 65 535 vertices unless it holds one record, and every record's position inside its bounds.
        static bool CheckPatches(WorldManifest m, GameObject root, FoliagePlan plan)
        {
            var fails = new List<string>();
            int patches = 0, want = 0, recs = 0;
            long maxVerts = 0;
            foreach (FoliageCellPlan cp in plan.Cells)
            {
                want += cp.Patches.Count;
                Transform parent = root.transform.Find("Cell_" + cp.Cell.Id + "/Foliage/Patches");
                int have = parent != null ? parent.childCount : 0;
                if (have != cp.Patches.Count) { fails.Add(cp.Cell.Id + ": " + have + " patches, plan " + cp.Patches.Count); continue; }
                for (int k = 0; k < have; ++k)
                {
                    Transform t = parent.GetChild(k);
                    FoliagePatchPlan pp = cp.Patches[k];
                    string name = m.Models[pp.Model].FileId + "_" + pp.K;
                    if (t.name != name) fails.Add(cp.Cell.Id + ": patch " + k + " is " + t.name + ", plan " + name);
                    LODGroup lg = t.GetComponent<LODGroup>();
                    if (lg == null || lg.lodCount != 1) fails.Add(t.name + ": no LOD0-only LODGroup");
                    MeshFilter mf = t.GetComponent<MeshFilter>();
                    Renderer rd = t.GetComponent<Renderer>();
                    if (mf == null || mf.sharedMesh == null || rd == null) { fails.Add(t.name + ": no mesh"); continue; }
                    long v = mf.sharedMesh.vertexCount;
                    maxVerts = Math.Max(maxVerts, v);
                    if (v > FoliagePlan.PatchMaxVertices && pp.Records.Count > 1) fails.Add(t.name + ": " + v + " vertices");
                    Bounds b = rd.bounds;
                    b.Expand(1e-3f);
                    foreach (FoliageRecord r in pp.Records)
                    {
                        Vector3 w = WorldOf(m, root, r);
                        // A model's origin may sit outside its own mesh (sideways): test against the bounds grown by one patch square.
                        Bounds g = b;
                        g.Expand((float)(2 * FoliagePlan.PatchUnits) * root.transform.localScale.x);
                        if (!g.Contains(w)) fails.Add(t.name + ": record " + r.Placement + " outside its patch");
                        ++recs;
                    }
                    ++patches;
                }
            }
            bool ok = fails.Count == 0;
            Debug.Log("FOLIAGE_PATCHES checked=" + patches + " plan=" + want + " records=" + recs + " maxVertices=" + maxVerts +
                      (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(5))));
            return ok;
        }

        /// TERRAIN_TREES: each Terrain's TreeInstances, in order, at its record's world position within 1e-3 of a tile.
        static bool CheckTerrainTrees(WorldManifest m, FoliagePlan plan)
        {
            GameObject root = GameObject.Find("GW2_" + m.MapFileId);
            GameObject troot = GameObject.Find("GW2_" + m.MapFileId + "_Terrain");
            if (root == null || troot == null) { Debug.LogError("TERRAIN_TREES FAIL no root"); return false; }
            var fails = new List<string>();
            int n = 0, clampedY = 0;
            double maxErr = 0;
            foreach (FoliageTilePlan tp in plan.Tiles)
            {
                Transform t = troot.transform.Find("Tile_" + tp.Tile.Id);
                Terrain terrain = t != null ? t.GetComponent<Terrain>() : null;
                if (terrain == null) { fails.Add(tp.Tile.Id + ": no Terrain"); continue; }
                TreeInstance[] ti = terrain.terrainData.treeInstances;
                if (ti.Length != tp.Trees.Count) { fails.Add(tp.Tile.Id + ": " + ti.Length + " trees, plan " + tp.Trees.Count); continue; }
                Vector3 size = terrain.terrainData.size, pos = terrain.transform.position;
                for (int i = 0; i < ti.Length; ++i)
                {
                    Vector3 got = pos + Vector3.Scale(ti[i].position, size);
                    Vector3 want = WorldOf(m, root, tp.Trees[i]);
                    // Unity clamps TreeInstance.position.y to 0..1: a tree outside the terrain's height range
                    // (TerrainBuilder warns) is checked in x and z only.
                    if (want.y < pos.y || want.y > pos.y + size.y) { want.y = got.y; ++clampedY; }
                    double err = (got - want).magnitude / Math.Max(1.0, size.x);
                    maxErr = Math.Max(maxErr, err);
                    ++n;
                }
            }
            bool ok = fails.Count == 0 && maxErr < 1e-5;
            Debug.Log("TERRAIN_TREES checked=" + n + " yClamped=" + clampedY + " maxRelErr=" + maxErr.ToString("E2", CultureInfo.InvariantCulture) +
                      (ok ? " OK" : " FAIL " + string.Join("; ", fails.Take(5))));
            return ok;
        }

        /// World-foliage D8: Cell_<id>/Foliage/Trees holds one instance per tree
        /// record the plan places, in record order, each with a LODGroup (LOD0 only, or
        /// its model's levels less those its cull drops: Task 15b), and its
        /// world matrix equals root * C * m * C^-1 from the record's own m.
        static bool CheckFoliage(WorldManifest m, GameObject root, FoliagePlan plan, string folder, ImportOptions opts)
        {
            var treeLevels = new Dictionary<int, int>();   // model -> its tree prefab's levels (Task 15b)
            Func<int, int> levelsOf = model =>
            {
                int n;
                if (treeLevels.TryGetValue(model, out n)) return n;
                var tp = AssetDatabase.LoadAssetAtPath<GameObject>(folder + "/Prefabs/Foliage/" + m.Models[model].FileId + ".prefab");
                double[] want;
                n = TreeLevels(m, model, tp != null ? tp.GetComponent<LODGroup>() : null, opts, out want);
                treeLevels[model] = n;
                return n;
            };
            double maxErr = 0;
            int checkedTrees = 0, want = 0;
            var fails = new List<string>();
            foreach (FoliageCellPlan cp in plan.Cells)
            {
                WorldManifest.CellEntry c = cp.Cell;
                List<FoliageRecord> trees = cp.Trees;
                want += trees.Count;
                Transform parent = root.transform.Find("Cell_" + c.Id + "/Foliage/Trees");
                int have = parent != null ? parent.childCount : 0;
                if (have != trees.Count) { fails.Add(c.Id + ": " + have + " trees, " + trees.Count + " records"); continue; }
                if (have == 0) continue;
                for (int k = 0; k < have; ++k)
                {
                    Transform t = parent.GetChild(k);
                    FoliageRecord r = trees[k];
                    if (t.name != m.Models[(int)r.Model].FileId + "_" + r.Placement) fails.Add(c.Id + ": child " + k + " is " + t.name);
                    LODGroup lg = t.GetComponent<LODGroup>();
                    int wantLevels = levelsOf((int)r.Model);
                    if (lg == null || lg.lodCount != wantLevels) fails.Add(t.name + ": no LODGroup of " + wantLevels + " level(s)");
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
