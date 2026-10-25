// castlemist world export: the Unity world importer.
//
// Spec docs/superpowers/specs/2026-10-15-world-export-design.md, D6 "Unity"
// steps 1-10 and "Unity mapping". Menu Castlemist > Import World... picks an
// export folder that has been copied under Assets/. Batchmode:
// BatchEntry.ImportWorld.
//
// What it builds, all under <folder>/ (re-import updates in place: .mat,
// .prefab and .controller assets keep their GUIDs; the scene is rebuilt):
//   Materials/Shared/<fileId>_<name>.mat  one per material key (Task 15d: sharedMaterials; every model's
//                                    slots of that key, LOD renderers included, remap to it), built
//                                    from and named after the key's first used materials.json entry
//                                    (its model's fileId and the entry's fileName; SharedMaterialStems);
//                                    a slot the manifest gives another dark decal or uvScroll gets its
//                                    own .mat, named after its own first entry. Shared .mat files no
//                                    variant names (an earlier import's) are deleted
//   Materials/<fileId>/<Mat>.mat     from models/<fileId>/materials.json, for an export without keys
//   Materials/terrain/<id>.mat       Poiyomi Opaque, _MainTex = cells/<id>_albedo.png
//   Materials/water/<id>.mat         Poiyomi Fade, _MainTex = cells/<id>_water.png
//   Materials/Skybox.mat             Skybox/Panoramic from sky.equirect
//   Prefabs/<fileId>.prefab          a variant of models/<fileId>/<fileId>.fbx, materials
//                                    remapped with ModelImporter.AddRemap
//   Controllers/<fileId>_<clip>.controller   one looping state, per (model, clip) used
//                                    with a LODGroup LOD0..LODn when models[].lods.levels > 1
//                                    (Task 15b, LodGroups / ModelLods: heights from the switch distances)
//   Prefabs/Foliage/<fileId>.prefab  the model prefab + a LODGroup culled at fadeFar: LOD0 only,
//                                    or the model's levels (FoliageImporter)
//   GW2_<fileId>.unity               the scene:
//     GW2_<fileId>    localScale = unityScale (the only scale; ruling W7)
//       Cell_<id>     localPosition = frame.toUnity * cell.origin
//         Terrain     MeshRenderer + MeshCollider
//         Water       MeshRenderer
//         Colliders/Collider_k   MeshCollider only (no renderer)
//         Props/<fileId>_<prop>  prefab instance, unity.t / r / s as given;
//                                Animator + controller when the instance has a clip;
//                                instances[].lod: its own LODGroup heights (Task 15b)
//         Foliage/Trees/<fileId>_<placement>  tree prefab instance per foliage tree
//                                record (FoliageImporter; world-foliage spec D8)
//         Foliage/Patches/<fileId>_<k>  type 1-2 records combined (FoliagePatches)
//       Sun           Directional Light (light.sun raw), ambient Flat
//       Spawn         at spawn.unity (when present)
//     GW2_<fileId>_Environment   SceneSetup (2026-10-22), unscaled: the post-processing
//                    volume and camera, a light volume and a reflection probe per cell,
//                    the OcclusionArea; fog / ambient in RenderSettings; the profile
//                    GW2_<fileId>_PostProcess.asset and GW2_<fileId>_Lighting.lighting
//       FrameProbe    probe.fbx at frameProbe.unity, checked by FrameProbe, then disabled
//     GW2_<fileId>_Terrain   Unity Terrain mode only (TerrainBuilder): the tiles, with
//                    the terrain frame check; the cells' mesh terrain and its
//                    MeshCollider are then not instantiated (the .fbx keeps them)
// Options (world-foliage D8, D9; ImportOptions): Terrain Mesh / Unity Terrain,
// Foliage Off / Collider trees / Trees / All, Density 0..1, Distance x0.1..2
// (on top of the manifest's fadeQualityScale, ruling S5), and the Quest preset
// (Mesh, Collider trees, Distance 0.5). No wind sway is added (D10).
// The importer does no frame maths beyond frame.toUnity on the cell origin.
// Cell .fbx files hold absolute map-space vertices (castlemist_world.py
// --to-fbx bakes every node transform), so their content is placed at
// -origin inside the cell.
// Static flags (BatchingStatic | OccluderStatic | OccludeeStatic) go on
// terrain, colliders and the prop prefabs (with Occlusion on, per renderer:
// SceneSetup.StaticFlagsFor); an animated instance has them
// cleared (a moving object must not be static). Prop materials get
// enableInstancing. Which of static batching and GPU instancing wins in
// VRChat is UNPROVEN (spec D2): CHECKLIST.md reads the Frame Debugger.
// Never locks Poiyomi materials, never adds a VRChat scene descriptor.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEditor.Animations;
using UnityEditor.SceneManagement;
using UnityEngine;
using Debug = UnityEngine.Debug;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class WorldImporter
    {
        public const StaticEditorFlags StaticFlags =
            StaticEditorFlags.BatchingStatic | StaticEditorFlags.OccluderStatic | StaticEditorFlags.OccludeeStatic;

        [MenuItem("Castlemist/Import World...")]
        static void Menu()
        {
            ImportWorldWindow.Open();
        }

        /// Reads <assetFolder>/manifest.json, builds the assets and a new scene,
        /// saves it as <assetFolder>/GW2_<fileId>.unity and returns what was
        /// built. Throws ManifestException for a missing or unknown manifest;
        /// everything else is a warning or an error in the summary.
        public static ImportSummary Import(string assetFolder, float unityScale)
        {
            return Import(assetFolder, unityScale, new ImportOptions());
        }

        /// Import with the window's options (world-foliage D8, D9).
        public static ImportSummary Import(string assetFolder, float unityScale, ImportOptions opts)
        {
            if (opts == null) opts = new ImportOptions();
            opts.Validate();
            var sw = Stopwatch.StartNew();
            var sum = new ImportSummary();
            string root = assetFolder.Replace('\\', '/').TrimEnd('/');
            string manPath = root + "/manifest.json";
            if (!File.Exists(manPath)) throw new ManifestException(manPath + ": missing");
            WorldManifest m = WorldManifest.Parse(File.ReadAllText(manPath));
            if (!(unityScale > 0f) || float.IsInfinity(unityScale))
                throw new ArgumentException("unityScale must be a positive number, not " + unityScale);
            sum.warnings.Add("unityScale = " + unityScale.ToString("R") + " (UNPROVEN: 'GW2 inches', spec D7)");
            CastlemistPostprocessor.ClearCache();
            // The mode in force: Terrain mode needs the export's unityTerrain block.
            ImportOptions eff = opts.Clone();
            if (eff.terrain == TerrainMode.UnityTerrain && m.UnityTerrain == null)
            {
                sum.errors.Add("unityTerrain: missing: Terrain mode needs the export's unityTerrain block " +
                               "(world-export without --no-unity-terrain); mesh terrain is built");
                eff.terrain = TerrainMode.Mesh;
            }
            sum.warnings.Add("options: " + eff);

            var b = new Build(root, m, sum, eff);
            EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            b.Timed("fbxSettings", b.FbxBaseSettings);
            b.UnityScale = unityScale;
            b.Timed("materials", b.Materials);
            b.Timed("modelImport", b.ModelImport);
            b.Timed("prefabs", b.Prefabs);
            b.Timed("controllers", b.Controllers);
            b.Timed("foliagePrefabs", b.FoliagePrefabs);
            b.Timed("scene", () => b.Scene(unityScale));
            b.Timed("terrain", () => b.UnityTerrain(unityScale));
            b.Timed("skyLight", b.SkyAndLight);
            b.Timed("probe", b.Probe);
            b.Timed("environment", () => b.Environment(unityScale));
            b.Timed("demolish", () => b.Demolish(b.MapRootObject, unityScale));

            string scenePath = root + "/GW2_" + m.MapFileId + ".unity";
            EditorSceneManager.SaveScene(EditorSceneManager.GetActiveScene(), scenePath);
            AssetDatabase.SaveAssets();
            sum.seconds = sw.Elapsed.TotalSeconds;
            Debug.Log("CASTLEMIST import " + root + " -> " + scenePath + " in " + sum.seconds.ToString("F1") + " s (" +
                      string.Join(", ", b.Times) + ")");
            return sum;
        }

        /// The open world scene's assets rebuilt from its export folder (the scene's own folder) and
        /// applied to the instances already in it, without a new scene (2026-10-23: the user edits
        /// the imported scenes by hand). Re-runs the asset steps: FBX settings, materials, model
        /// import (rigs, clips), prefabs (their particle systems and LOD groups; the scene's instances
        /// follow them as prefab instances), particle materials and controllers; then gives each
        /// placed instance (found by its "<fileId>_<prop>" name) its clip's controller and particle
        /// timing again. Terrain, environment, lights, light volumes and probes are left as they are.
        /// The scene is marked dirty, not saved.
        [MenuItem("Castlemist/Refresh Open World Scene In Place")]
        public static void RefreshOpenSceneMenu()
        {
            ImportSummary sum = RefreshOpenScene(null);
            Log(sum);
        }

        public static ImportSummary RefreshOpenScene(ImportOptions opts)
        {
            var sw = Stopwatch.StartNew();
            var sum = new ImportSummary();
            var scene = EditorSceneManager.GetActiveScene();
            string root = Path.GetDirectoryName(scene.path).Replace('\\', '/');
            string manPath = root + "/manifest.json";
            if (string.IsNullOrEmpty(scene.path) || !File.Exists(manPath))
            {
                sum.errors.Add("refresh: the open scene (" + scene.path + ") is not in a castlemist world export folder");
                return sum;
            }
            WorldManifest m = WorldManifest.Parse(File.ReadAllText(manPath));
            GameObject mapRoot = scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + m.MapFileId);
            if (mapRoot == null)
            {
                sum.errors.Add("refresh: no GW2_" + m.MapFileId + " root in the open scene");
                return sum;
            }
            float unityScale = mapRoot.transform.localScale.x;
            CastlemistPostprocessor.ClearCache();
            if (opts == null)
            {
                // As imported: the shader mode the scene's materials use (the default is Poiyomi),
                // the window's skipped models.
                opts = new ImportOptions { skipModels = EditorPrefs.GetString("Castlemist.ImportWorld.SkipModels", "") };
                bool castlemist = mapRoot.GetComponentsInChildren<Renderer>(true)
                    .SelectMany(r => r.sharedMaterials).Any(x => x != null && x.shader != null && x.shader.name.StartsWith("Castlemist/GW2", StringComparison.Ordinal));
                opts.shader = castlemist ? ShaderMode.Castlemist : ShaderMode.Poiyomi;
                sum.warnings.Add("refresh: shader mode " + opts.shader + " (from the scene's materials)");
            }
            ImportOptions eff = opts.Clone();
            eff.Validate();
            var b = new Build(root, m, sum, eff) { UnityScale = unityScale };
            b.Timed("fbxSettings", b.FbxBaseSettings);
            b.Timed("materials", b.Materials);
            b.Timed("modelImport", b.ModelImport);
            b.Timed("prefabs", b.Prefabs);
            b.Timed("controllers", b.Controllers);
            b.Timed("instances", () => b.RefreshInstances(mapRoot, unityScale));
            b.Timed("foliage", () => b.RefreshFoliage(mapRoot));
            b.Timed("clouds", () => b.Clouds(mapRoot, unityScale));
            b.Timed("cellColliders", () => b.RefreshCellColliders(mapRoot));
            b.Timed("demolish", () => b.Demolish(mapRoot, unityScale));
            EditorSceneManager.MarkSceneDirty(scene);
            AssetDatabase.SaveAssets();
            sum.seconds = sw.Elapsed.TotalSeconds;
            Debug.Log("CASTLEMIST refresh " + root + " in " + sum.seconds.ToString("F1") + " s (" + string.Join(", ", b.Times) + ")");
            return sum;
        }

        /// Task 15d: what one shared .mat is built for -- the key, and the dark
        /// decal and uvScroll the manifest gives the slot (validate_manifest.py
        /// material_variant counts the same).
        public static string SharedMaterialVariant(string key, bool dark, WorldManifest.UvScroll sc)
        {
            string v = key;
            if (dark) v += "|dark";
            if (sc != null)
                v += "|scroll:" + sc.UvIndex + ":" + sc.U.ToString("R", System.Globalization.CultureInfo.InvariantCulture) +
                     ":" + sc.V.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
            return v;
        }

        /// The Materials/Shared .mat file names: variant (SharedMaterialVariant) -> "<fileId>_<fileName>"
        /// of the variant's first used, keyed materials.json entry, models in manifest order (the
        /// entry its .mat is built from). One entry has one variant, so the names are unique. A
        /// model whose materials.json is missing or unreadable is skipped (the import reports it).
        public static Dictionary<string, string> SharedMaterialStems(string root, WorldManifest m)
        {
            var stems = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (WorldManifest.ModelEntry e in m.Models)
            {
                if (!e.Ok || e.Materials == null) continue;
                string path = root + "/" + e.Materials.Replace('\\', '/');
                if (!File.Exists(path)) continue;
                MaterialsFile f;
                try { f = MaterialsFile.Parse(File.ReadAllText(path), path); }
                catch (ManifestException) { continue; }
                foreach (MaterialsFile.MaterialEntry me in f.Materials)
                {
                    if (!me.UsedByMeshes || me.Key == null) continue;
                    bool dark = m.DarkDecalRule && e.DarkDecalMaterials.Contains(me.Name);
                    WorldManifest.UvScroll sc = e.UvScroll.FirstOrDefault(u => u.Material == me.Name);
                    string variant = SharedMaterialVariant(me.Key, dark, sc);
                    if (!stems.ContainsKey(variant)) stems[variant] = Build.Safe(e.FileId + "_" + me.FileName);
                }
            }
            return stems;
        }

        /// Writes the summary to the log: warnings, errors, PLAN and PROBE lines.
        public static void Log(ImportSummary sum)
        {
            foreach (string w in sum.warnings) Debug.Log("CASTLEMIST warning: " + w);
            foreach (string e in sum.errors) Debug.LogError("CASTLEMIST error: " + e);
            Debug.Log("CASTLEMIST colliderParts=" + sum.colliderParts + " warnings=" + sum.warnings.Count +
                      " errors=" + sum.errors.Count + " seconds=" + sum.seconds.ToString("F1"));
            if (sum.shaderLine != null) Debug.Log(sum.shaderLine);
            if (sum.probeLine != null) Debug.Log(sum.probeLine);
            if (sum.terrainCheckLine != null) Debug.Log(sum.terrainCheckLine);
            if (sum.envLine != null) Debug.Log(sum.envLine);
            if (sum.particleSystems > 0) Debug.Log("CASTLEMIST particleSystems=" + sum.particleSystems);
            if (sum.particleFields > 0) Debug.Log("CASTLEMIST particleFields=" + sum.particleFields);
            if (sum.clouds > 0) Debug.Log("CASTLEMIST clouds=" + sum.clouds);
            if (sum.skyCards > 0) Debug.Log("CASTLEMIST skyCards=" + sum.skyCards);
            if (sum.demolishables > 0) Debug.Log("CASTLEMIST demolishables=" + sum.demolishables);
            Debug.Log(sum.PlanLine());
        }

        /// An instance's rotation and scale, on top of its prefab root's own: Unity leaves the
        /// axis conversion (90 degrees about X) on the root of some .fbx files instead of baking
        /// it (map 506592's cloth ropes 71804 / 71749, rooted at an armature that deforms no mesh),
        /// and setting the rotation alone laid them flat. Identity for every other model there.
        public static void Place(GameObject go, GameObject prefab, Quaternion r, float s)
        {
            go.transform.localRotation = r * prefab.transform.localRotation;
            go.transform.localScale = prefab.transform.localScale * s;
        }

        /// Each SkinnedMeshRenderer's localBounds set to what @p ac's clips sweep, sampled on a
        /// hidden copy: the bind-pose box frustum-culled a mesh its clip carries far from it
        /// (506586_158's "On" swings ~21500 units around a ~1900 unit box, 2026-10-25).
        public static void FitSkinnedBounds(GameObject go, RuntimeAnimatorController ac)
        {
            SkinnedMeshRenderer[] orig = go.GetComponentsInChildren<SkinnedMeshRenderer>(true);
            if (orig.Length == 0 || ac == null || ac.animationClips.Length == 0) return;
            GameObject copy = Object.Instantiate(go);
            copy.hideFlags = HideFlags.HideAndDontSave;
            try
            {
                SkinnedMeshRenderer[] smrs = copy.GetComponentsInChildren<SkinnedMeshRenderer>(true);
                var box = new Bounds[smrs.Length];
                var seen = new bool[smrs.Length];
                var baked = new Mesh();
                foreach (AnimationClip clip in ac.animationClips.Distinct())
                {
                    int steps = Mathf.Clamp(Mathf.CeilToInt(clip.length * 10f), 30, 600);
                    for (int k = 0; k <= steps; ++k)
                    {
                        clip.SampleAnimation(copy, clip.length * k / steps);
                        for (int j = 0; j < smrs.Length; ++j)
                        {
                            SkinnedMeshRenderer s = smrs[j];
                            if (s.sharedMesh == null) continue;
                            s.BakeMesh(baked, true);
                            // localBounds is in the root bone's space (the renderer's own without one).
                            Matrix4x4 m = (s.rootBone != null ? s.rootBone : s.transform).worldToLocalMatrix * s.transform.localToWorldMatrix;
                            foreach (Vector3 v in baked.vertices)
                            {
                                Vector3 p = m.MultiplyPoint3x4(v);
                                if (!seen[j]) { box[j] = new Bounds(p, Vector3.zero); seen[j] = true; }
                                else box[j].Encapsulate(p);
                            }
                        }
                    }
                }
                Object.DestroyImmediate(baked);
                for (int j = 0; j < orig.Length && j < smrs.Length; ++j)
                {
                    if (!seen[j]) continue;
                    box[j].Encapsulate(orig[j].localBounds);
                    box[j].Expand(box[j].size * 0.02f);
                    if (orig[j].localBounds != box[j]) { Undo.RecordObject(orig[j], "Fit skinned bounds"); orig[j].localBounds = box[j]; }
                }
            }
            finally { Object.DestroyImmediate(copy); }
        }

        sealed class Build
        {
            readonly string root;
            readonly WorldManifest m;
            readonly ImportSummary sum;
            public readonly List<string> Times = new List<string>();
            public float UnityScale = 0.0254f;   // the import's unityScale (materials: depth distances)
            MaterialContext ctx;
            // Per model position in models[] (what instances and records name): name -> material, the prefab.
            readonly Dictionary<int, Dictionary<string, Material>> modelMats = new Dictionary<int, Dictionary<string, Material>>();
            // Per model: each built material's materials.json entry (its constAnims, for the clips).
            readonly Dictionary<int, Dictionary<Material, MaterialsFile.MaterialEntry>> modelEntries =
                new Dictionary<int, Dictionary<Material, MaterialsFile.MaterialEntry>>();
            readonly Dictionary<int, GameObject> prefabs = new Dictionary<int, GameObject>();
            readonly Dictionary<string, RuntimeAnimatorController> controllers = new Dictionary<string, RuntimeAnimatorController>();
            readonly Dictionary<string, Material> terrainMats = new Dictionary<string, Material>();
            readonly Dictionary<string, Material> waterMats = new Dictionary<string, Material>();
            GameObject mapRoot;
            FoliageImporter foliage;
            readonly ImportOptions opts;
            bool UnityTerrainMode { get { return opts.terrain == TerrainMode.UnityTerrain; } }

            public Build(string root, WorldManifest m, ImportSummary sum, ImportOptions opts)
            {
                this.root = root; this.m = m; this.sum = sum; this.opts = opts;
            }

            // World-foliage D8: the bins read and planned, the tree prefabs saved.
            public void FoliagePrefabs()
            {
                foliage = new FoliageImporter(root, m, sum, prefabs, opts);
                foliage.Prefabs();
            }

            // World-foliage D8, Unity Terrain mode: the tiles and the terrain frame check.
            public void UnityTerrain(float unityScale)
            {
                if (!UnityTerrainMode) return;
                TerrainBuilder.Result r = TerrainBuilder.Build(root, m, unityScale, opts, foliage.Plan, foliage.TreePrefabs, prefabs, sum);
                Debug.Log(r.FilesLine);
                string line;
                sum.terrainCheckOk = TerrainBuilder.CheckFrame(r.Tiles, m.UnityTerrain.Check, unityScale, out line);
                sum.terrainCheckLine = line;
                if (!sum.terrainCheckOk) sum.errors.Add("terrain frame check failed: " + line);
            }

            public void Timed(string name, Action a)
            {
                var sw = Stopwatch.StartNew();
                a();
                Times.Add(name + "=" + sw.Elapsed.TotalSeconds.ToString("F1") + "s");
            }

            string P(string rel) { return root + "/" + rel.Replace('\\', '/'); }

            bool Exists(string rel, string what)
            {
                if (rel != null && File.Exists(P(rel))) return true;
                sum.errors.Add(what + ": missing file " + (rel == null ? "(none named)" : P(rel)));
                return false;
            }

            static void EnsureFolder(string path)
            {
                if (AssetDatabase.IsValidFolder(path)) return;
                int slash = path.LastIndexOf('/');
                EnsureFolder(path.Substring(0, slash));
                AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
            }

            /// True when a skin joint of the model's .glb has a scale other than 1 (TRS or matrix):
            /// GW2 binds many static props to one scaled joint (map 506592's rock 90608: 3280.66)
            /// that the inverse bind matrix cancels. Blender cannot keep that scale in the bone's
            /// rest, so the .fbx mesh sits at 1 / scale and only the rig restores it; imported
            /// without the rig (None) such a model is that small. False when the .glb is unreadable.
            bool NeedsRig(WorldManifest.ModelEntry e)
            {
                if (e.Glb == null || !File.Exists(P(e.Glb))) return false;
                try
                {
                    using (var br = new BinaryReader(File.OpenRead(P(e.Glb))))
                    {
                        if (br.ReadUInt32() != 0x46546C67) return false;   // "glTF"
                        br.ReadUInt32(); br.ReadUInt32();
                        int len = (int)br.ReadUInt32();
                        if (br.ReadUInt32() != 0x4E4F534A) return false;   // "JSON"
                        var gltf = MiniJson.Parse(System.Text.Encoding.UTF8.GetString(br.ReadBytes(len))) as Dictionary<string, object>;
                        var nodes = gltf != null && gltf.TryGetValue("nodes", out object n) ? n as List<object> : null;
                        var skins = gltf != null && gltf.TryGetValue("skins", out object s) ? s as List<object> : null;
                        if (nodes == null || skins == null) return false;
                        foreach (object sk in skins)
                        {
                            if (!(sk is Dictionary<string, object> skin) || !skin.TryGetValue("joints", out object j) || !(j is List<object> joints))
                                continue;
                            foreach (object ji in joints)
                            {
                                int k = Convert.ToInt32(ji);
                                if (k < 0 || k >= nodes.Count || !(nodes[k] is Dictionary<string, object> node)) continue;
                                // A placeholder joint ("Invalid": scale 0, rotation (0, 0, 0, 0); map 506592's
                                // ground covers 1391982-1391988) is no rig: skinned through it every vertex collapses.
                                if (node.TryGetValue("rotation", out object ro) && ro is List<object> rv && rv.Count == 4 &&
                                    rv.All(c => Math.Abs(Convert.ToDouble(c)) < 1e-9)) continue;
                                if (node.TryGetValue("scale", out object sc) && sc is List<object> sv)
                                {
                                    if (sv.Any(c => Math.Abs(Convert.ToDouble(c)) < 1e-6)) continue;
                                    foreach (object c in sv) if (Math.Abs(Convert.ToDouble(c) - 1.0) > 1e-3) return true;
                                }
                                if (node.TryGetValue("matrix", out object mx) && mx is List<object> mv && mv.Count == 16)
                                    for (int col = 0; col < 3; ++col)
                                    {
                                        double x = Convert.ToDouble(mv[col * 4]), y = Convert.ToDouble(mv[col * 4 + 1]), z = Convert.ToDouble(mv[col * 4 + 2]);
                                        if (Math.Abs(Math.Sqrt(x * x + y * y + z * z) - 1.0) > 1e-3) return true;
                                    }
                            }
                        }
                    }
                }
                catch (Exception ex) when (ex is IOException || ex is FormatException || ex is InvalidCastException || ex is OverflowException)
                {
                    sum.warnings.Add("model " + e.FileId + ": .glb not read for its rig (" + ex.Message + "): imported without it");
                }
                return false;
            }

            public static string Safe(string s)
            {
                char[] bad = Path.GetInvalidFileNameChars();
                var c = s.Select(ch => bad.Contains(ch) || ch == '|' ? '_' : ch).ToArray();
                return new string(c);
            }

            IEnumerable<string> AllFbx()
            {
                foreach (WorldManifest.ModelEntry e in m.Models) if (e.Ok && e.Fbx != null) yield return P(e.Fbx);
                foreach (WorldManifest.CellEntry c in m.Cells) if (c.Fbx != null) yield return P(c.Fbx);
                if (m.FrameProbe != null && m.FrameProbe.Fbx != null) yield return P(m.FrameProbe.Fbx);
            }

            // Step 0: every world .fbx imported with bakeAxisConversion (the
            // postprocessor sets it on first import; this catches .fbx files
            // imported before the importer's scripts were in the project).
            public void FbxBaseSettings()
            {
                int fixedCount = 0;
                AssetDatabase.StartAssetEditing();
                try
                {
                    foreach (string path in AllFbx())
                    {
                        var mi = AssetImporter.GetAtPath(path) as ModelImporter;
                        if (mi == null) continue;
                        if (CastlemistPostprocessor.ApplyModelSettings(mi, path.Substring(root.Length + 1)))
                        {
                            mi.SaveAndReimport();
                            ++fixedCount;
                        }
                    }
                }
                finally { AssetDatabase.StopAssetEditing(); }
                if (fixedCount > 0)
                    sum.warnings.Add("fbx: " + fixedCount + " file(s) were imported without the castlemist settings " +
                                     "(bakeAxisConversion and the rest; copy tools/unity/Castlemist in first): re-imported");
            }

            // Step 3.
            public void Materials()
            {
                ctx = MaterialContext.Create(sum.warnings, opts.shader);
                ctx.Uv1InUv4 = m.Uv1InUv4;
                ctx.UnityScale = UnityScale;
                ctx.TessellateCloth = opts.tessellateCloth;
                CastlemistMaterials.VegWindFrom(m.Environment != null ? m.Environment.Wind : null, out ctx.VegWind, out ctx.VegWindGust);
                sum.shaderLine = ctx.Line;
                EnsureFolder(root + "/Materials/terrain");
                EnsureFolder(root + "/Materials/water");
                bool anyKeys = false;
                foreach (WorldManifest.ModelEntry e in m.Models)
                    if (e.Ok && e.Materials != null)
                    {
                        if (e.MaterialKeys.Count > 0) anyKeys = true;
                        else EnsureFolder(root + "/Materials/" + e.FileId);
                    }
                if (anyKeys) EnsureFolder(root + "/Materials/Shared");
                var shared = new Dictionary<string, Material>(StringComparer.Ordinal);   // variant -> .mat (Task 15d)
                Dictionary<string, string> stems = SharedMaterialStems(root, m);
                if (anyKeys)
                {
                    // An earlier import's .mat files (key-named before 2026-10-21, or a variant gone).
                    var keep = new HashSet<string>(stems.Values, StringComparer.Ordinal);
                    int stale = 0;
                    foreach (string guid in AssetDatabase.FindAssets("t:Material", new[] { root + "/Materials/Shared" }))
                    {
                        string mp = AssetDatabase.GUIDToAssetPath(guid);
                        if (keep.Contains(Path.GetFileNameWithoutExtension(mp))) continue;
                        if (AssetDatabase.DeleteAsset(mp)) ++stale;
                    }
                    if (stale > 0) sum.warnings.Add("materials: " + stale + " Materials/Shared .mat file(s) no material names any more: deleted");
                }

                AssetDatabase.StartAssetEditing();
                try
                {
                    foreach (WorldManifest.ModelEntry e in m.Models)
                    {
                        if (!e.Ok)
                        {
                            // "empty": no drawable meshes, nothing to place; not an error (final review M1).
                            if (e.Status == "empty") continue;
                            sum.warnings.Add("model " + e.FileId + ": status " + e.Status + (e.Error != null ? " (" + e.Error + ")" : "") + ": skipped");
                            continue;
                        }
                        if (e.Materials == null || !Exists(e.Materials, "model " + e.FileId + " materials")) continue;
                        MaterialsFile f;
                        try { f = MaterialsFile.Parse(File.ReadAllText(P(e.Materials)), P(e.Materials)); }
                        catch (ManifestException ex) { sum.errors.Add(ex.Message); continue; }
                        if (f.Poiyomi != "10") sum.warnings.Add(P(e.Materials) + ": poiyomi '" + f.Poiyomi + "', expected '10'");
                        string folder = Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/');
                        var mats = new Dictionary<string, Material>(StringComparer.Ordinal);
                        var entries = new Dictionary<Material, MaterialsFile.MaterialEntry>();
                        var seen = new Dictionary<string, int>(StringComparer.Ordinal);
                        foreach (MaterialsFile.MaterialEntry me in f.Materials)
                        {
                            int k;
                            seen.TryGetValue(me.Name, out k);
                            seen[me.Name] = k + 1;
                            if (!me.UsedByMeshes) continue;  // particle-only materials (VRChat export §1)
                            bool dark = m.DarkDecalRule && e.DarkDecalMaterials.Contains(me.Name);
                            WorldManifest.UvScroll sc = e.UvScroll.FirstOrDefault(u => u.Material == me.Name);
                            Material mat;
                            if (me.Key != null)
                            {
                                // Task 15d: one .mat per key, built from its first used entry; the manifest's
                                // per-model dark decal and uvScroll make a variant of it when they differ.
                                string variant = SharedMaterialVariant(me.Key, dark, sc);
                                if (!shared.TryGetValue(variant, out mat))
                                {
                                    mat = PoiyomiMaterials.BuildModelMaterial(ctx, me, folder, dark, sc,
                                        root + "/Materials/Shared/" + stems[variant] + ".mat");
                                    shared[variant] = mat;
                                }
                            }
                            else
                                mat = PoiyomiMaterials.BuildModelMaterial(ctx, me, folder, dark, sc,
                                    root + "/Materials/" + e.FileId + "/" + Safe(me.FileName) + ".mat");
                            foreach (string key in FbxMaterialNames(me, k)) mats[key] = mat;
                            if (!entries.ContainsKey(mat)) entries[mat] = me;
                        }
                        modelMats[e.Position] = mats;
                        modelEntries[e.Position] = entries;
                    }
                    foreach (WorldManifest.CellEntry c in m.Cells)
                    {
                        if (!UnityTerrainMode && c.HasTerrain && c.Albedo != null && Exists(c.Albedo, "cell " + c.Id + " albedo"))
                            terrainMats[c.Id] = PoiyomiMaterials.BuildSimple(ctx, root + "/Materials/terrain/" + Safe(c.Id) + ".mat",
                                AssetDatabase.LoadAssetAtPath<Texture2D>(P(c.Albedo)), PoiyomiMaterials.Opaque);
                        if (c.WaterTexture != null && Exists(c.WaterTexture, "cell " + c.Id + " water"))
                            waterMats[c.Id] = PoiyomiMaterials.BuildSimple(ctx, root + "/Materials/water/" + Safe(c.Id) + ".mat",
                                AssetDatabase.LoadAssetAtPath<Texture2D>(P(c.WaterTexture)), PoiyomiMaterials.Fade);
                    }
                }
                finally { AssetDatabase.StopAssetEditing(); }
                sum.materials = ctx.Built;
            }

            /// The names the .fbx may give material `me`, the k-th (from 0) of
            /// that name in materials.json. Blender names glTF materials that
            /// share a name "<name>", "<name>.001", ... in file order, and cuts a
            /// name to 63 bytes (suffix included); Unity's FBX importer turns ':'
            /// into '_'. materials.json's fileName is the export's own unique,
            /// file-safe form. Measured on Spirit Vale (Task 12).
            static IEnumerable<string> FbxMaterialNames(MaterialsFile.MaterialEntry me, int k)
            {
                string suffix = k > 0 ? "." + k.ToString("D3") : "";
                string b = CutUtf8(me.Name, 63 - suffix.Length) + suffix;
                if (k == 0) yield return me.Name;
                yield return me.FileName;
                yield return b;
                yield return b.Replace(':', '_');
            }

            static string CutUtf8(string s, int maxBytes)
            {
                if (System.Text.Encoding.UTF8.GetByteCount(s) <= maxBytes) return s;
                int n = s.Length;
                while (n > 0 && System.Text.Encoding.UTF8.GetByteCount(s.Substring(0, n)) > maxBytes) --n;
                if (n > 0 && char.IsHighSurrogate(s[n - 1])) --n;
                return s.Substring(0, n);
            }

            // Step 4a: remap, rig and clip settings, re-imported in one batch.
            public void ModelImport()
            {
                AssetDatabase.StartAssetEditing();
                try
                {
                    foreach (WorldManifest.ModelEntry e in m.Models)
                    {
                        if (!e.Ok) continue;
                        if (e.Fbx == null)
                        {
                            sum.warnings.Add("model " + e.FileId + ": no .fbx (run gw2dat_cli world-fbx --dir <folder>, or castlemist_world.py --to-fbx <folder>): no prefab");
                            continue;
                        }
                        if (!Exists(e.Fbx, "model " + e.FileId + " fbx")) continue;
                        var mi = AssetImporter.GetAtPath(P(e.Fbx)) as ModelImporter;
                        if (mi == null) { sum.errors.Add(P(e.Fbx) + ": not imported as a model"); continue; }
                        mi.bakeAxisConversion = FrameProbe.BakeAxisConversion;
                        mi.importCameras = false;
                        mi.importLights = false;
                        Dictionary<string, Material> mats;
                        if (modelMats.TryGetValue(e.Position, out mats))
                            foreach (KeyValuePair<string, Material> kv in mats)
                                mi.AddRemap(new AssetImporter.SourceAssetIdentifier(typeof(Material), kv.Key), kv.Value);
                        if (e.Clips.Count == 0 && NeedsRig(e))
                        {
                            // Skinned to a scaled joint (map 506592's rock 90608: 3280.66): only the rig
                            // puts the mesh at its size, so it keeps the rig and plays nothing.
                            mi.animationType = ModelImporterAnimationType.Generic;
                            mi.importAnimation = false;
                            sum.riggedStatic++;
                        }
                        else if (e.Clips.Count == 0)
                        {
                            mi.animationType = ModelImporterAnimationType.None;
                            mi.importAnimation = false;
                        }
                        else
                        {
                            mi.animationType = ModelImporterAnimationType.Generic;
                            mi.importAnimation = true;
                            ModelImporterClipAnimation[] clips = mi.clipAnimations;
                            if (clips == null || clips.Length == 0) clips = mi.defaultClipAnimations;
                            foreach (ModelImporterClipAnimation ca in clips) ca.loopTime = true;  // clipPhase: looping (UNPROVEN)
                            mi.clipAnimations = clips;
                        }
                        mi.SaveAndReimport();
                    }
                }
                finally { AssetDatabase.StopAssetEditing(); }
            }

            // Step 4b.
            public void Prefabs()
            {
                EnsureFolder(root + "/Prefabs");
                foreach (WorldManifest.ModelEntry e in m.Models)
                {
                    if (!e.Ok || e.Fbx == null || !File.Exists(P(e.Fbx))) continue;
                    var asset = AssetDatabase.LoadAssetAtPath<GameObject>(P(e.Fbx));
                    if (asset == null) { sum.errors.Add(P(e.Fbx) + ": no GameObject"); continue; }
                    Dictionary<string, Material> mats;
                    modelMats.TryGetValue(e.Position, out mats);
                    var ours = mats != null ? new HashSet<Material>(mats.Values) : new HashSet<Material>();
                    var notRemapped = new SortedSet<string>();
                    bool noMaterial = false;
                    foreach (Renderer r in asset.GetComponentsInChildren<Renderer>(true))
                        foreach (Material sm in r.sharedMaterials)
                        {
                            // A glTF primitive with no material (seen in the export's
                            // own .glb) imports with Unity's built-in Default-Material.
                            if (sm == null || sm.name == "Default-Material") noMaterial = true;
                            else if (!ours.Contains(sm)) notRemapped.Add(sm.name);
                        }
                    if (noMaterial)
                        sum.warnings.Add("model " + e.FileId + ": a mesh has no material in the export: Unity's Default-Material is left on it");
                    if (notRemapped.Count > 0)
                        sum.warnings.Add("model " + e.FileId + ": FBX materials not remapped from materials.json: " + string.Join(", ", notRemapped));

                    var inst = (GameObject)PrefabUtility.InstantiatePrefab(asset);
                    try
                    {
                        inst.name = e.FileId.ToString();
                        // 2026-10-22: per renderer (SceneSetup.StaticFlagsFor): occluder only when opaque,
                        // ContributeGI with Light Probes GI, ReflectionProbeStatic; StaticFlags with Occlusion off.
                        foreach (Transform t in inst.GetComponentsInChildren<Transform>(true))
                            SceneSetup.ApplyStatic(t.gameObject, opts);
                        // Task 15b: LOD0..LODn from the model's LOD objects and its "lod" stand-ins
                        // (LodGroups, ModelLods); every model culls at ModelLods.PropCull.
                        int built = LodGroups.Build(inst, e.LodLevels, e.FarMeshes, e.LodDistances,
                                                    size => ModelLods.PropCull(opts.distance), sum.warnings, "model " + e.FileId,
                                                    e.HiddenMeshes);
                        if (built > 1) sum.lodPrefabs++;
                        // 2026-10-22: the model's particle clouds as particle systems (ParticleEffects).
                        string modelFolder = e.Materials != null ? Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/') : null;
                        string sidecar = modelFolder != null ? ParticleEffects.SidecarFor(modelFolder, e.FileId) : null;
                        if (sidecar != null)
                        {
                            try
                            {
                                MaterialsFile mf = MaterialsFile.Parse(File.ReadAllText(P(e.Materials)), P(e.Materials));
                                // Per model: material names repeat across models for different textures.
                                string pmat = root + "/Materials/Particles/" + e.FileId;
                                EnsureFolder(pmat);
                                sum.particleSystems += ParticleEffects.AddTo(inst, sidecar, mf, modelFolder, ctx,
                                                                             pmat, sum.warnings);
                            }
                            catch (ManifestException ex) { sum.errors.Add(ex.Message); }
                        }
                        string path = root + "/Prefabs/" + e.FileId + ".prefab";
                        bool ok;
                        GameObject saved = PrefabUtility.SaveAsPrefabAsset(inst, path, out ok);
                        if (!ok || saved == null) { sum.errors.Add(path + ": prefab not saved"); continue; }
                        prefabs[e.Position] = saved;
                        sum.prefabs++;
                    }
                    finally { Object.DestroyImmediate(inst); }
                }
            }

            static AnimationClip FindClip(string fbx, string clip)
            {
                foreach (AnimationClip c in AssetDatabase.LoadAllAssetsAtPath(fbx).OfType<AnimationClip>())
                {
                    if (c.name.StartsWith("__preview__", StringComparison.Ordinal)) continue;
                    if (c.name == clip || c.name.EndsWith("|" + clip, StringComparison.Ordinal)) return c;
                }
                return null;
            }

            // Step 5.
            public void Controllers()
            {
                var pairs = new SortedSet<string>(StringComparer.Ordinal);
                foreach (WorldManifest.CellEntry c in m.Cells)
                    foreach (WorldManifest.InstanceEntry i in c.Instances)
                        if (i.Clip != null && i.Model >= 0 && i.Model < m.Models.Count) pairs.Add(i.Model + "\n" + i.Clip);
                if (pairs.Count > 0) EnsureFolder(root + "/Controllers");
                foreach (string key in pairs)
                {
                    int model = int.Parse(key.Substring(0, key.IndexOf('\n')));
                    string clipName = key.Substring(key.IndexOf('\n') + 1);
                    WorldManifest.ModelEntry e = m.Models[model];
                    if (!prefabs.ContainsKey(model)) continue;
                    // "a+b": a sequence (world/anim.h resolve_prop_sequence: intro, then loop).
                    var clips = new List<AnimationClip>();
                    foreach (string part in clipName.Split('+'))
                    {
                        AnimationClip c = FindClip(P(e.Fbx), part);
                        if (c == null) { clips = null; break; }
                        clips.Add(WithConstants(model, e, c, part));
                    }
                    if (clips == null || clips.Count == 0) { sum.warnings.Add("model " + e.FileId + ": clip '" + clipName + "' not in its FBX: instances stay still"); continue; }
                    string path = root + "/Controllers/" + e.FileId + "_" + Safe(clipName) + ".controller";
                    if (AssetDatabase.LoadAssetAtPath<AnimatorController>(path) != null) AssetDatabase.DeleteAsset(path);
                    var ac = AnimatorController.CreateAnimatorControllerAtPathWithClip(path, clips[0]);
                    AnimatorStateMachine sm = ac.layers[0].stateMachine;
                    AnimatorState prev = sm.defaultState;
                    for (int k = 1; k < clips.Count; ++k)
                    {
                        AnimatorState st = sm.AddState(clips[k].name);
                        st.motion = clips[k];
                        AnimatorStateTransition tr = prev.AddTransition(st);
                        tr.hasExitTime = true;
                        tr.exitTime = 1f;
                        tr.duration = 0f;
                        prev = st;
                    }
                    EditorUtility.SetDirty(ac);
                    controllers[key] = ac;
                    sum.controllers++;
                }
            }

            /// @p src with the material constants model @p model animates over clip @p part
            /// (materials.json constAnims, CastlemistMaterials.AnimatedProperties) added as material
            /// curves on the renderers using each material; @p src itself when there are none.
            /// Saved as Controllers/<fileId>_<part>.anim.
            AnimationClip WithConstants(int model, WorldManifest.ModelEntry e, AnimationClip src, string part)
            {
                Dictionary<Material, MaterialsFile.MaterialEntry> entries;
                GameObject prefab;
                if (!modelEntries.TryGetValue(model, out entries) || !prefabs.TryGetValue(model, out prefab)) return src;
                var curves = new List<KeyValuePair<EditorCurveBinding, AnimationCurve>>();
                foreach (Renderer r in prefab.GetComponentsInChildren<Renderer>(true))
                {
                    string rpath = AnimationUtility.CalculateTransformPath(r.transform, prefab.transform);
                    var done = new HashSet<string>(StringComparer.Ordinal);
                    foreach (Material sm in r.sharedMaterials)
                    {
                        MaterialsFile.MaterialEntry me;
                        if (sm == null || !entries.TryGetValue(sm, out me)) continue;
                        foreach (MaterialsFile.ConstAnim ca in me.ConstAnims)
                        {
                            if (ca.Clip != part || ca.Times.Length == 0) continue;
                            foreach (KeyValuePair<string, int> prop in CastlemistMaterials.AnimatedProperties(me, ca.Constant))
                            {
                                if (!done.Add(prop.Key)) continue;   // the renderer's first material with it
                                var keys = new List<Keyframe>();
                                for (int i = 0; i < ca.Times.Length; ++i)
                                    keys.Add(new Keyframe((float)ca.Times[i], (float)ca.Values[i][prop.Value]));
                                if (keys.Count == 1) keys.Add(new Keyframe(Mathf.Max(src.length, 1f / 30f), keys[0].value));   // held
                                var curve = new AnimationCurve(keys.ToArray());
                                for (int i = 0; i < curve.length; ++i)
                                {
                                    AnimationUtility.SetKeyLeftTangentMode(curve, i, AnimationUtility.TangentMode.Linear);
                                    AnimationUtility.SetKeyRightTangentMode(curve, i, AnimationUtility.TangentMode.Linear);
                                }
                                curves.Add(new KeyValuePair<EditorCurveBinding, AnimationCurve>(
                                    EditorCurveBinding.FloatCurve(rpath, r.GetType(), "material." + prop.Key), curve));
                            }
                        }
                    }
                }
                // 2026-10-25: mesh visibility (the sidecar's meshVis and clips[].boneVis): a mesh is
                // visible at the start of each clip and toggles at each key of its visBone (84108's
                // zeropose hides its "demolish" pieces, Demolish its "good" ones). Every clip writes
                // every such mesh, a constant "on" when its bone has no keys there: a renderer saved
                // off (the idle pose) would otherwise come back off through write-defaults, and
                // demolished buildings vanished.
                Dictionary<string, string> meshVis;
                Dictionary<string, List<float>> boneKeys;
                if (MeshVisibility(e, part, out meshVis, out boneKeys))
                    foreach (Renderer r in prefab.GetComponentsInChildren<Renderer>(true))
                    {
                        string bone;
                        List<float> keys;
                        if (r is ParticleSystemRenderer || !meshVis.TryGetValue(Regex.Replace(r.name, "_LOD[0-9]+$", ""), out bone)) continue;
                        if (!boneKeys.TryGetValue(bone, out keys)) keys = new List<float>();
                        var steps = new List<Keyframe> { new Keyframe(0f, 1f) };
                        float v = 1f;
                        foreach (float t in keys)
                        {
                            v = 1f - v;
                            if (steps[steps.Count - 1].time >= t - 1e-5f) steps[steps.Count - 1] = new Keyframe(steps[steps.Count - 1].time, v);
                            else steps.Add(new Keyframe(t, v));
                        }
                        float end = Mathf.Max(src.length, steps[steps.Count - 1].time + 1f / 30f);
                        steps.Add(new Keyframe(end, v));
                        var curve = new AnimationCurve(steps.ToArray());
                        for (int i = 0; i < curve.length; ++i)
                        {
                            AnimationUtility.SetKeyLeftTangentMode(curve, i, AnimationUtility.TangentMode.Constant);
                            AnimationUtility.SetKeyRightTangentMode(curve, i, AnimationUtility.TangentMode.Constant);
                        }
                        curves.Add(new KeyValuePair<EditorCurveBinding, AnimationCurve>(
                            EditorCurveBinding.FloatCurve(AnimationUtility.CalculateTransformPath(r.transform, prefab.transform), r.GetType(), "m_Enabled"), curve));
                    }
                if (curves.Count == 0) return src;
                var clip = new AnimationClip { name = src.name, frameRate = src.frameRate };
                foreach (EditorCurveBinding b in AnimationUtility.GetCurveBindings(src))
                    AnimationUtility.SetEditorCurve(clip, b, AnimationUtility.GetEditorCurve(src, b));
                foreach (KeyValuePair<EditorCurveBinding, AnimationCurve> kv in curves)
                    AnimationUtility.SetEditorCurve(clip, kv.Key, kv.Value);
                AnimationUtility.SetAnimationClipSettings(clip, AnimationUtility.GetAnimationClipSettings(src));
                string path = root + "/Controllers/" + e.FileId + "_" + Safe(part) + ".anim";
                if (AssetDatabase.LoadAssetAtPath<AnimationClip>(path) != null) AssetDatabase.DeleteAsset(path);
                AssetDatabase.CreateAsset(clip, path);
                sum.constantClips++;
                return clip;
            }

            /// The model's sidecar meshVis (mesh node -> visBone) and clip @p part's boneVis keys; false
            /// without either.
            bool MeshVisibility(WorldManifest.ModelEntry e, string part, out Dictionary<string, string> meshVis,
                                out Dictionary<string, List<float>> boneKeys)
            {
                meshVis = new Dictionary<string, string>(StringComparer.Ordinal);
                boneKeys = new Dictionary<string, List<float>>(StringComparer.Ordinal);
                if (e.Materials == null) return false;
                string sidecar = ParticleEffects.SidecarFor(Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/'), e.FileId);
                if (sidecar == null) return false;
                var doc = MiniJson.Parse(File.ReadAllText(sidecar)) as Dictionary<string, object>;
                object mv, cl;
                if (doc == null || !doc.TryGetValue("meshVis", out mv) || !(mv is Dictionary<string, object> mvd) || mvd.Count == 0) return false;
                foreach (KeyValuePair<string, object> kv in mvd) if (kv.Value is string b) meshVis[kv.Key] = b;
                if (!doc.TryGetValue("clips", out cl) || !(cl is List<object> clips)) return false;
                foreach (object o in clips)
                {
                    if (!(o is Dictionary<string, object> c) || !(c.TryGetValue("name", out object n) && (n as string) == part)) continue;
                    if (c.TryGetValue("boneVis", out object bv) && bv is Dictionary<string, object> bvd)
                        foreach (KeyValuePair<string, object> kv in bvd)
                            if (kv.Value is List<object> ks) boneKeys[kv.Key] = ks.OfType<double>().Select(x => (float)x).ToList();
                }
                return meshVis.Count > 0;
            }

            static Transform Child(Transform parent, string name)
            {
                var go = new GameObject(name);
                go.transform.SetParent(parent, false);
                return go.transform;
            }

            /// @p parent's child @p name, made when it has none.
            static Transform FindOrChild(Transform parent, string name)
            {
                Transform t = parent.Find(name);
                return t != null ? t : Child(parent, name);
            }

            static string BaseName(string n)
            {
                int dot = n.LastIndexOf('.');
                return dot > 0 && n.Substring(dot + 1).All(char.IsDigit) ? n.Substring(0, dot) : n;
            }

            static Vector3 V(double[] a) { return new Vector3((float)a[0], (float)a[1], (float)a[2]); }

            // Step 6-7.
            public void Scene(float unityScale)
            {
                mapRoot = new GameObject("GW2_" + m.MapFileId);
                mapRoot.transform.localScale = Vector3.one * unityScale;
                int skipped = 0, skippedByOption = 0;
                HashSet<long> skip = opts.SkipModelIds();
                foreach (WorldManifest.CellEntry c in m.Cells)
                {
                    Vector3 origin = V(m.MapToUnity(c.Origin));
                    Transform cell = Child(mapRoot.transform, "Cell_" + c.Id);
                    cell.localPosition = origin;
                    sum.cells++;
                    CellContent(c, cell, origin);

                    Transform props = Child(cell, "Props");
                    foreach (WorldManifest.InstanceEntry i in c.Instances)
                    {
                        GameObject prefab;
                        if (i.Model < 0 || i.Model >= m.Models.Count || !prefabs.TryGetValue(i.Model, out prefab)) { ++skipped; continue; }
                        WorldManifest.ModelEntry e = m.Models[i.Model];
                        if (skip.Contains(e.FileId)) { ++skippedByOption; continue; }
                        var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, props);
                        go.name = e.FileId + "_" + i.Prop;
                        go.transform.localPosition = V(i.T);
                        Place(go, prefab, new Quaternion((float)i.R[0], (float)i.R[1], (float)i.R[2], (float)i.R[3]), (float)i.S);
                        // Task 15b: OVERRIDE_LOD placements get their own heights on the instance.
                        if (i.Lod != null && LodGroups.Override(go, i.Lod)) sum.lodOverrides++;
                        RuntimeAnimatorController ac;
                        if (i.Clip != null && controllers.TryGetValue(i.Model + "\n" + i.Clip, out ac))
                        {
                            Animator an = go.GetComponent<Animator>();
                            if (an == null) an = go.AddComponent<Animator>();
                            an.runtimeAnimatorController = ac;
                            foreach (Transform t in go.GetComponentsInChildren<Transform>(true))
                                GameObjectUtility.SetStaticEditorFlags(t.gameObject, 0);
                            FitSkinnedBounds(go, ac);
                        }
                        // 2026-10-22: its particle systems timed to its clip, at its scale (ParticleEffects.Fit).
                        if (e.Materials != null)
                        {
                            string sidecar = ParticleEffects.SidecarFor(Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/'), e.FileId);
                            if (sidecar != null) ParticleEffects.Fit(go, sidecar, i.Clip, unityScale);
                        }
                        sum.instances++;
                    }
                    if (foliage != null) foliage.PlaceCell(c, cell);
                }
                if (foliage != null) foliage.Finish();
                if (skipped > 0) sum.warnings.Add("instances: " + skipped + " skipped (their model has no prefab)");
                if (skippedByOption > 0) sum.warnings.Add("instances: " + skippedByOption + " not placed (Skip models: " + opts.skipModels + ")");
            }

            /// RefreshOpenScene, after the instances: each Cell_<id>/Foliage is built again from the
            /// bins (the importer's own node, so it is replaced). Mesh mode when the scene holds no
            /// Terrain (deleted, or a Mesh import), so no record is left to a missing detail layer;
            /// with a Terrain, its details and trees stay as they are and the rest becomes patches.
            public void RefreshFoliage(GameObject mapRoot)
            {
                if (m.Foliage == null) return;
                if (mapRoot.GetComponentsInChildren<Terrain>(true).Length == 0) opts.terrain = TerrainMode.Mesh;
                else if (m.UnityTerrain != null) opts.terrain = TerrainMode.UnityTerrain;
                foliage = new FoliageImporter(root, m, sum, prefabs, opts);
                foliage.Prefabs();
                foreach (WorldManifest.CellEntry c in m.Cells)
                {
                    Transform cell = mapRoot.transform.Find("Cell_" + c.Id);
                    if (cell == null) continue;
                    Transform old = cell.Find("Foliage");
                    if (old != null) UnityEngine.Object.DestroyImmediate(old.gameObject);
                    foliage.PlaceCell(c, cell);
                }
                foliage.Finish();
            }

            public GameObject MapRootObject { get { return mapRoot; } }

            /// 2026-10-25: the props of every model with a Demolish clip made demolishable
            /// (Demolishables; after the instances, their controllers and the cell colliders).
            public void Demolish(GameObject mapRoot, float unityScale)
            {
                var models = new Dictionary<int, AnimatorController>();
                for (int k = 0; k < m.Models.Count; ++k)
                {
                    WorldManifest.ModelEntry e = m.Models[k];
                    if (!Demolishables.Has(e.Clips) || e.Fbx == null) continue;
                    AnimationClip dem = FindClip(P(e.Fbx), Demolishables.ClipName);
                    if (dem == null) { sum.warnings.Add("demolish: model " + e.FileId + ": no Demolish clip in its FBX"); continue; }
                    AnimationClip idle = FindClip(P(e.Fbx), "zeropose");
                    EnsureFolder(root + "/Controllers");
                    models[k] = Demolishables.BuildController(root + "/Controllers/" + e.FileId + "_Demolish.controller",
                                                              idle != null ? WithConstants(k, e, idle, "zeropose") : null,
                                                              WithConstants(k, e, dem, Demolishables.ClipName));
                }
                if (models.Count == 0) return;
                if (!Demolishables.EnsurePrograms(sum.warnings)) return;
                Component permissions = Demolishables.FindPermissions();
                int done = 0;
                foreach (WorldManifest.CellEntry c in m.Cells)
                {
                    Transform cell = mapRoot.transform.Find("Cell_" + c.Id);
                    if (cell == null) continue;
                    Transform props = cell.Find("Props");
                    Transform own = cell.Find(PropCollidersName);
                    foreach (WorldManifest.InstanceEntry i in c.Instances)
                    {
                        AnimatorController ac;
                        if (!models.TryGetValue(i.Model, out ac) || props == null) continue;
                        WorldManifest.ModelEntry e = m.Models[i.Model];
                        Transform inst = props.Find(e.FileId + "_" + i.Prop);
                        if (inst == null) continue;
                        Transform col = own != null ? own.Find("PropCollider_" + i.Prop) : null;
                        if (col == null) col = inst.Find(Demolishables.ColliderName);
                        string sidecar = e.Materials != null
                            ? ParticleEffects.SidecarFor(Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/'), e.FileId) : null;
                        if (!Demolishables.SetupInstance(inst.gameObject, ac, sidecar, unityScale, col, permissions, sum.warnings))
                        {
                            sum.warnings.Add("demolish: Castlemist.GW2Demolishable is not compiled yet: refresh again once UdonSharp has compiled");
                            return;
                        }
                        ++done;
                    }
                }
                Demolishables.StartAsPlayer(mapRoot, permissions, sum.warnings);
                sum.demolishables = done;
                if (permissions == null) sum.warnings.Add("demolish: no GMMenu in the scene: anyone can click the " + done + " demolishable prop(s)");
            }

            /// RefreshOpenScene's last step: every placed instance under @p mapRoot (named
            /// "<fileId>_<prop>" by Scene) gets its clip's controller and particle timing again.
            public void RefreshInstances(GameObject mapRoot, float unityScale)
            {
                var byName = new Dictionary<string, KeyValuePair<int, WorldManifest.InstanceEntry>>(StringComparer.Ordinal);
                foreach (WorldManifest.CellEntry c in m.Cells)
                    foreach (WorldManifest.InstanceEntry i in c.Instances)
                        if (i.Model >= 0 && i.Model < m.Models.Count)
                            byName[m.Models[i.Model].FileId + "_" + i.Prop] = new KeyValuePair<int, WorldManifest.InstanceEntry>(i.Model, i);
                int done = 0;
                foreach (Transform t in mapRoot.GetComponentsInChildren<Transform>(true))
                {
                    KeyValuePair<int, WorldManifest.InstanceEntry> hit;
                    if (t.parent == null || t.parent.name != "Props" || !byName.TryGetValue(t.name, out hit)) continue;
                    WorldManifest.InstanceEntry i = hit.Value;
                    WorldManifest.ModelEntry e = m.Models[hit.Key];
                    // Place() for a model whose prefab root is turned or scaled (only then: other
                    // instances keep any hand edit).
                    GameObject prefab;
                    if (prefabs.TryGetValue(hit.Key, out prefab) &&
                        (prefab.transform.localRotation != Quaternion.identity || prefab.transform.localScale != Vector3.one))
                        Place(t.gameObject, prefab, new Quaternion((float)i.R[0], (float)i.R[1], (float)i.R[2], (float)i.R[3]), (float)i.S);
                    RuntimeAnimatorController ac;
                    if (i.Clip != null && controllers.TryGetValue(hit.Key + "\n" + i.Clip, out ac))
                    {
                        Animator an = t.GetComponent<Animator>();
                        if (an == null) an = t.gameObject.AddComponent<Animator>();
                        an.runtimeAnimatorController = ac;
                        foreach (Transform c in t.GetComponentsInChildren<Transform>(true))
                            GameObjectUtility.SetStaticEditorFlags(c.gameObject, 0);
                        FitSkinnedBounds(t.gameObject, ac);
                    }
                    if (e.Materials != null)
                    {
                        string sidecar = ParticleEffects.SidecarFor(Path.GetDirectoryName(P(e.Materials)).Replace('\\', '/'), e.FileId);
                        if (sidecar != null) ParticleEffects.Fit(t.gameObject, sidecar, i.Clip, unityScale);
                    }
                    ++done;
                }
                sum.warnings.Add("refresh: " + done + " placed instance(s) given their controllers and particle timing");
            }

            const string PropCollidersName = "PropColliders";

            /// Turns cell node @p t into a MeshCollider named @p n under @p parent (its renderer dropped).
            static void MakeCollider(Transform t, Transform parent, string n, bool isRoot, StaticEditorFlags flags)
            {
                if (isRoot) { var keep = t.localPosition; t.SetParent(parent, false); t.localPosition = keep; }
                else t.SetParent(parent, true);
                t.name = n;
                var mf = t.GetComponent<MeshFilter>();
                var mr = t.GetComponent<MeshRenderer>();
                Mesh mesh = mf.sharedMesh;
                if (mr != null) Object.DestroyImmediate(mr);
                Object.DestroyImmediate(mf);
                var mc = t.gameObject.AddComponent<MeshCollider>();
                mc.convex = false;
                mc.sharedMesh = mesh;
                GameObjectUtility.SetStaticEditorFlags(t.gameObject, flags);
            }

            /// RefreshOpenScene (2026-10-25): every cell's Colliders and PropColliders built again from
            /// its .fbx (an export that split props' collision out), the rest of the cell untouched. A
            /// prop collider Demolishables had moved under its prop is replaced too.
            public void RefreshCellColliders(GameObject mapRoot)
            {
                int cellsDone = 0;
                foreach (WorldManifest.CellEntry c in m.Cells)
                {
                    Transform cell = mapRoot.transform.Find("Cell_" + c.Id);
                    if (cell == null || c.Fbx == null || !File.Exists(P(c.Fbx))) continue;
                    var asset = AssetDatabase.LoadAssetAtPath<GameObject>(P(c.Fbx));
                    if (asset == null) continue;
                    for (int k = cell.childCount - 1; k >= 0; --k)
                    {
                        Transform o = cell.GetChild(k);
                        if (o.name == "Colliders" || o.name == PropCollidersName) Object.DestroyImmediate(o.gameObject);
                    }
                    foreach (Transform t in cell.GetComponentsInChildren<Transform>(true).ToArray())
                        if (t != null && t.name == Demolishables.ColliderName) Object.DestroyImmediate(t.gameObject);
                    Vector3 origin = V(m.MapToUnity(c.Origin));
                    var tmp = (GameObject)Object.Instantiate(asset);
                    tmp.transform.SetParent(cell, false);
                    tmp.transform.localPosition = -origin;  // absolute map-space vertices, as CellContent
                    Transform colliders = null;
                    int parts = 0;
                    bool keepRoot = false;
                    foreach (Transform t in tmp.GetComponentsInChildren<Transform>(true).ToArray())
                    {
                        var mf = t.GetComponent<MeshFilter>();
                        if (mf == null || mf.sharedMesh == null) continue;
                        bool isRoot = t == tmp.transform;
                        string n = BaseName(isRoot ? mf.sharedMesh.name : t.name);
                        if (n.StartsWith("Collider_", StringComparison.Ordinal))
                        {
                            if (colliders == null) colliders = Child(cell, "Colliders");
                            MakeCollider(t, colliders, n, isRoot, StaticFlags);
                            keepRoot |= isRoot;
                            ++parts;
                        }
                        else if (n.StartsWith("PropCollider_", StringComparison.Ordinal))
                        {
                            MakeCollider(t, FindOrChild(cell, PropCollidersName), n, isRoot, 0);
                            keepRoot |= isRoot;
                        }
                    }
                    if (!keepRoot) Object.DestroyImmediate(tmp);
                    else
                        for (int k = tmp.transform.childCount - 1; k >= 0; --k)  // leftover nodes of a single-mesh file
                            Object.DestroyImmediate(tmp.transform.GetChild(k).gameObject);
                    if (parts != c.ColliderParts) sum.warnings.Add("cell " + c.Id + ": " + parts + " collider parts, manifest says " + c.ColliderParts);
                    ++cellsDone;
                }
                sum.warnings.Add("refresh: " + cellsDone + " cell(s) given their colliders again");
            }

            void CellContent(WorldManifest.CellEntry c, Transform cell, Vector3 origin)
            {
                string what = "cell " + c.Id;
                if (c.Fbx == null)
                {
                    if (c.Glb != null) sum.warnings.Add(what + ": no .fbx (run gw2dat_cli world-fbx --dir <folder>, or castlemist_world.py --to-fbx <folder>): no terrain, water or colliders");
                    return;
                }
                if (!Exists(c.Fbx, what + " fbx")) return;
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(P(c.Fbx));
                if (asset == null) { sum.errors.Add(P(c.Fbx) + ": no GameObject"); return; }
                var tmp = (GameObject)Object.Instantiate(asset);
                tmp.transform.SetParent(cell, false);
                tmp.transform.localPosition = -origin;  // absolute map-space vertices
                Transform colliders = null;
                int parts = 0;
                bool terrain = false, water = false;
                bool keepRoot = false;
                foreach (Transform t in tmp.GetComponentsInChildren<Transform>(true).ToArray())
                {
                    if (t == null) continue;  // under a node destroyed above (Terrain mode)
                    var mf = t.GetComponent<MeshFilter>();
                    var mr = t.GetComponent<MeshRenderer>();
                    if (mf == null || mf.sharedMesh == null) continue;
                    // An .fbx with a single mesh imports as one object named after
                    // the file (a cell with terrain only): its role is the mesh's name.
                    bool isRoot = t == tmp.transform;
                    string n = BaseName(isRoot ? mf.sharedMesh.name : t.name);
                    if (isRoot) keepRoot = true;
                    if (n == "Terrain" && UnityTerrainMode)
                    {
                        // Unity Terrain mode: the tile replaces the mesh terrain (and its MeshCollider).
                        if (isRoot) { Object.DestroyImmediate(mr); Object.DestroyImmediate(mf); keepRoot = false; }
                        else Object.DestroyImmediate(t.gameObject);
                        terrain = true;
                        continue;
                    }
                    if (n == "Terrain")
                    {
                        if (!isRoot) t.SetParent(cell, true);
                        t.name = "Terrain";
                        Material mat;
                        if (mr != null && terrainMats.TryGetValue(c.Id, out mat)) mr.sharedMaterial = mat;
                        var mc = t.gameObject.AddComponent<MeshCollider>();
                        mc.sharedMesh = mf.sharedMesh;
                        GameObjectUtility.SetStaticEditorFlags(t.gameObject, StaticFlags);
                        terrain = true;
                        sum.terrainColliders++;
                    }
                    else if (n == "Water")
                    {
                        if (!isRoot) t.SetParent(cell, true);
                        t.name = "Water";
                        Material mat;
                        if (mr != null && waterMats.TryGetValue(c.Id, out mat)) mr.sharedMaterial = mat;
                        water = true;
                        sum.water++;
                    }
                    else if (n.StartsWith("Collider_", StringComparison.Ordinal))
                    {
                        if (colliders == null) colliders = Child(cell, "Colliders");
                        MakeCollider(t, colliders, n, isRoot, StaticFlags);
                        ++parts;
                    }
                    else if (n.StartsWith("PropCollider_", StringComparison.Ordinal))
                    {
                        // 2026-10-25: a demolishable prop's own collision; Demolishables moves it under the prop
                        MakeCollider(t, FindOrChild(cell, PropCollidersName), n, isRoot, 0);
                    }
                }
                if (!keepRoot) Object.DestroyImmediate(tmp);
                else
                    for (int k = tmp.transform.childCount - 1; k >= 0; --k)  // leftover nodes of a single-mesh file
                        Object.DestroyImmediate(tmp.transform.GetChild(k).gameObject);
                if (c.HasTerrain && !terrain) sum.warnings.Add(what + ": no Terrain node in " + P(c.Fbx));
                if (c.WaterTexture != null && !water) sum.warnings.Add(what + ": no Water node in " + P(c.Fbx));
                if (parts != c.ColliderParts) sum.warnings.Add(what + ": " + parts + " collider parts, manifest says " + c.ColliderParts);
                if (parts > 0) sum.colliders++;
                sum.colliderParts += parts;
            }

            // Step 8.
            public void SkyAndLight()
            {
                if (m.Sky != null)
                {
                    string eq = m.Sky.Equirect;
                    if (eq == null)
                    {
                        string dir = Path.GetDirectoryName(m.Sky.Json.Replace('\\', '/')).Replace('\\', '/');
                        eq = (dir.Length > 0 ? dir + "/" : "") + m.Sky.Default + "/baked/equirect.png";
                    }
                    Shader pano = Shader.Find("Skybox/Panoramic");
                    if (pano == null) sum.errors.Add("sky: shader Skybox/Panoramic not found");
                    else if (Exists(eq, "sky equirect"))
                    {
                        EnsureFolder(root + "/Materials");
                        string path = root + "/Materials/Skybox.mat";
                        var skyMat = AssetDatabase.LoadAssetAtPath<Material>(path);
                        if (skyMat == null) { skyMat = new Material(pano) { name = "Skybox" }; AssetDatabase.CreateAsset(skyMat, path); }
                        else skyMat.shader = pano;
                        skyMat.SetTexture("_MainTex", AssetDatabase.LoadAssetAtPath<Texture2D>(P(eq)));
                        if (skyMat.HasProperty("_Mapping")) skyMat.SetFloat("_Mapping", 1f);      // Latitude Longitude
                        if (skyMat.HasProperty("_ImageType")) skyMat.SetFloat("_ImageType", 0f);  // 360 degrees
                        EditorUtility.SetDirty(skyMat);
                        RenderSettings.skybox = skyMat;
                        sum.skybox = 1;
                    }
                }
                else sum.warnings.Add("sky: null in the manifest: no skybox");

                var sunGo = new GameObject("Sun");
                sunGo.transform.SetParent(mapRoot.transform, false);
                Light sun = sunGo.AddComponent<Light>();
                sun.type = LightType.Directional;
                sun.shadows = LightShadows.Soft;
                if (m.Light != null)
                {
                    Vector3 fwd = V(m.Light.SunUnityForward);
                    if (fwd.sqrMagnitude > 1e-12f) sunGo.transform.rotation = Quaternion.LookRotation(fwd.normalized);
                    else sum.warnings.Add("light: sun direction is zero: the light points along +Z");
                    if (fwd.y > 0f) sum.warnings.Add("light: sun.unityForward points up (y = " + fwd.y.ToString("F3") + "): used as given");
                    sun.color = new Color((float)m.Light.SunColor[0], (float)m.Light.SunColor[1], (float)m.Light.SunColor[2]);
                    sun.intensity = (float)m.Light.SunIntensity;  // raw (castlemist's choice, spec D5)
                    RenderSettings.ambientMode = UnityEngine.Rendering.AmbientMode.Flat;
                    RenderSettings.ambientLight = new Color((float)m.Light.Ambient[0], (float)m.Light.Ambient[1], (float)m.Light.Ambient[2]);
                    if (!m.Light.Present) sum.warnings.Add("light: the map has no lighting preset: the export's default light is used");
                }
                else
                {
                    sunGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);
                    sun.color = Color.white;
                    sun.intensity = 1f;
                    sum.warnings.Add("light: null in the manifest: a default white sun is used");
                }
                RenderSettings.sun = sun;
                sum.lights = 1;

                if (m.SpawnUnity != null)
                {
                    var sp = new GameObject("Spawn");
                    sp.transform.SetParent(mapRoot.transform, false);
                    sp.transform.localPosition = V(m.SpawnUnity);
                }
                else sum.warnings.Add("spawn: null in the manifest: no Spawn object");
            }

            // 2026-10-22: fog, ambient, post-processing, light volumes, reflection probes and
            // occlusion from manifest.environment (SceneSetup; each behind its ImportOptions toggle).
            public void Environment(float unityScale)
            {
                SceneSetup.Apply(new SceneSetup.Context { Root = root, M = m, Sum = sum, Opts = opts, UnityScale = unityScale, MapRoot = mapRoot });
                // 2026-10-23: the ambient particle fields (ParticleFields), from the raw manifest.
                try
                {
                    var doc = MiniJson.Parse(File.ReadAllText(root + "/manifest.json")) as Dictionary<string, object>;
                    object env, fields;
                    if (doc != null && doc.TryGetValue("environment", out env) && env is Dictionary<string, object> e &&
                        e.TryGetValue("particleFields", out fields) && fields is List<object> list && list.Count > 0)
                        sum.particleFields = ParticleFields.Build(mapRoot, list,
                            f => f.TryGetValue("texture", out object t) && t is string s ? P(s) : null,
                            root + "/Materials/ParticleFields", unityScale, sum.warnings);
                }
                catch (Exception ex) { sum.warnings.Add("particle fields: " + ex.Message); }
                Clouds(mapRoot, unityScale);
            }

            /// 2026-10-25: the cloud layers (GW2Clouds) and the texture sky cards (GW2SkyCards), from the
            /// raw manifest; also a refresh step.
            public void Clouds(GameObject mapRoot, float unityScale)
            {
                try
                {
                    var doc = MiniJson.Parse(File.ReadAllText(root + "/manifest.json")) as Dictionary<string, object>;
                    object env;
                    if (doc != null && doc.TryGetValue("environment", out env) && env is Dictionary<string, object> e)
                    {
                        sum.clouds = GW2Clouds.Build(mapRoot, e, P, root + "/Materials/Clouds", unityScale,
                                                     m.SpawnUnity != null ? m.SpawnUnity[1] : 0, sum.warnings);
                        sum.skyCards = GW2SkyCards.Build(mapRoot, e, P, root + "/Materials/SkyCards", unityScale, sum.warnings);
                    }
                }
                catch (Exception ex) { sum.warnings.Add("clouds / sky cards: " + ex.Message); }
            }

            // Step 9.
            public void Probe()
            {
                if (m.FrameProbe == null) { sum.warnings.Add("frameProbe: null in the manifest: frame not checked"); return; }
                if (m.FrameProbe.Fbx == null) { sum.warnings.Add("frameProbe: no .fbx (run gw2dat_cli world-fbx --dir <folder>, or castlemist_world.py --to-fbx <folder>): frame not checked"); return; }
                if (!Exists(m.FrameProbe.Fbx, "frame probe")) return;
                var asset = AssetDatabase.LoadAssetAtPath<GameObject>(P(m.FrameProbe.Fbx));
                if (asset == null) { sum.errors.Add(P(m.FrameProbe.Fbx) + ": no GameObject"); return; }
                var probe = (GameObject)Object.Instantiate(asset);
                probe.name = "FrameProbe";
                probe.transform.SetParent(mapRoot.transform, false);
                probe.transform.localPosition = V(m.FrameProbe.Unity);
                string line;
                sum.probeOk = FrameProbe.Check(probe, out line);
                sum.probeLine = line;
                if (!sum.probeOk) sum.errors.Add("frame probe failed (fix castlemist_world.py's FBX axes or bakeAxisConversion): " + line.Split('\n')[0]);
                probe.tag = "EditorOnly";
                probe.SetActive(false);
            }
        }
    }

    public sealed class ImportWorldWindow : EditorWindow
    {
        DefaultAsset folder;
        float unityScale = (float)WorldManifest.DefaultUnityScale;
        const string SkipModelsPref = "Castlemist.ImportWorld.SkipModels";
        ImportOptions opts = new ImportOptions { skipModels = EditorPrefs.GetString(SkipModelsPref, "") };
        string loadedFor;
        string status = "";
        Vector2 scroll;

        // A normal (dockable, resizable) window with its options in a scroll view: as a fixed-size
        // utility window the options ran past its bottom edge and the Import button could not be
        // reached on a small screen (2026-10-23). It opens inside the main editor window.
        public static void Open()
        {
            var w = GetWindow<ImportWorldWindow>(false, "Castlemist: Import World", true);
            w.minSize = new Vector2(360, 240);
            w.maxSize = new Vector2(4000, 4000);   // a stored 0 x 0 maximum pinned it to its minimum, unresizable
            if (w.docked) return;
            Place(w);
            EditorApplication.delayCall += () => { if (w != null && !w.docked) Place(w); };   // a new window's first layout resets it
        }

        /// At least 460 x 520, inside the main editor window.
        static void Place(EditorWindow w)
        {
            Rect main = EditorGUIUtility.GetMainWindowPosition();
            Rect r = w.position;
            r.width = Mathf.Min(Mathf.Max(r.width, 460f), main.width - 40f);
            r.height = Mathf.Min(Mathf.Max(r.height, 520f), main.height - 80f);
            r.x = Mathf.Clamp(r.x, main.x + 20f, main.xMax - r.width - 20f);
            r.y = Mathf.Clamp(r.y, main.y + 40f, main.yMax - r.height - 20f);
            w.position = r;
        }

        // A window left open across a script reload can come back without its options
        // (not serialized): make them again rather than throw on every repaint.
        void OnEnable()
        {
            if (opts == null) opts = new ImportOptions { skipModels = EditorPrefs.GetString(SkipModelsPref, "") };
            if (status == null) status = "";
        }

        void OnGUI()
        {
            OnEnable();
            scroll = EditorGUILayout.BeginScrollView(scroll);
            Body();
            EditorGUILayout.EndScrollView();
        }

        void Body()
        {
            EditorGUILayout.HelpBox("Pick a castlemist world export folder (it must hold manifest.json) that has been copied under Assets/.", MessageType.None);
            folder = (DefaultAsset)EditorGUILayout.ObjectField("Export folder", folder, typeof(DefaultAsset), false);
            string path = folder != null ? AssetDatabase.GetAssetPath(folder) : null;
            if (path != loadedFor)
            {
                loadedFor = path;
                status = "";
                if (path != null && File.Exists(path + "/manifest.json"))
                {
                    try { unityScale = (float)WorldManifest.Parse(File.ReadAllText(path + "/manifest.json")).UnityScale; }
                    catch (ManifestException e) { status = e.Message; }
                }
            }
            unityScale = EditorGUILayout.FloatField(new GUIContent("Unity scale",
                "Unity units per map unit, on the GW2_<fileId> root. Default from the manifest: 0.0254, the 'GW2 inches' reading; UNPROVEN (spec D7)."), unityScale);
            opts.terrain = (TerrainMode)EditorGUILayout.Popup(new GUIContent("Terrain",
                "Mesh: the cells' terrain meshes and MeshColliders. Unity Terrain: one Terrain per 4 x 4-chunk tile with the baked " +
                "albedo as its one layer, Unity details and Terrain trees (world-foliage D8). Quest stays on Mesh."),
                (int)opts.terrain, new[] { "Mesh", "Unity Terrain (baked layer)" });
            opts.foliage = (FoliageMode)EditorGUILayout.Popup(new GUIContent("Foliage",
                "Off; Collider trees (the zone trees that have a havk collider); Trees (every tree); All (trees, grass and plants)."),
                (int)opts.foliage, new[] { "Off", "Collider trees", "Trees", "All" });
            opts.density = EditorGUILayout.Slider(new GUIContent("Density",
                "Keeps a placement when fmix32(placement) < density x 2^32; trees with a collider are always kept (D9)."), opts.density, 0f, 1f);
            opts.distance = EditorGUILayout.Slider(new GUIContent("Distance",
                "Multiplies every fade distance (fadeFar x the manifest's fadeQualityScale), for the LODGroup culls and the Terrain distances (D9)."),
                opts.distance, ImportOptions.MinDistance, ImportOptions.MaxDistance);
            EditorGUI.BeginChangeCheck();
            opts.skipModels = EditorGUILayout.TextField(new GUIContent("Skip models",
                "Model fileIds not to place, comma separated (e.g. 3132765: the Mists vault's slabs over the walkway). Remembered."),
                opts.skipModels ?? "");
            if (EditorGUI.EndChangeCheck()) EditorPrefs.SetString(SkipModelsPref, opts.skipModels);
            opts.shader = (ShaderMode)EditorGUILayout.Popup(new GUIContent("Shader",
                "Poiyomi: Poiyomi Pro / Toon (Realistic lighting; Flat for particle-like materials). " +
                "Castlemist GW2: castlemist's own Castlemist/GW2 (one shader, no keywords) and GW2 Particle (VRC Light Volumes; tools/unity/Castlemist/Shaders)."),
                (int)opts.shader, new[] { "Poiyomi", "Castlemist GW2" });
            EditorGUILayout.LabelField("Scene look (manifest.environment; UNPROVEN readings)", EditorStyles.boldLabel);
            opts.tessellateCloth = EditorGUILayout.Toggle(new GUIContent("Tessellate cloth (PC)",
                "GW2 cloth (banners) on Castlemist/GW2 Tessellated: the wave subdivided on the GPU, smoother. Not on Quest."), opts.tessellateCloth);
            opts.haze = EditorGUILayout.Toggle(new GUIContent("Haze volume (GW2 height fog)",
                "The Castlemist/GW2 Haze cube: GW2's distance + height haze per pixel (needs the camera depth texture; Unity fog off). Not on Quest."), opts.haze);
            opts.fog = EditorGUILayout.Toggle(new GUIContent("Fog (fallback)", "Unity Linear fog from environment.haze (distRange x Unity scale, one colour; no height fog), " +
                "when the haze volume is off or cannot be made."), opts.fog);
            opts.ambient = EditorGUILayout.Toggle(new GUIContent("Ambient", "Trilight sky / equator / ground from the fill lights environment.lights[1..]."), opts.ambient);
            opts.post = EditorGUILayout.Toggle(new GUIContent("Post-processing",
                "Post Processing Stack v2 (com.unity.postprocessing): global volume on the PostProcessing layer, Bloom, Color Grading " +
                "(saturation, tint, the clut LUT), MSVO ambient occlusion; a PostProcessLayer on the reference camera. Not on Quest."), opts.post);
            opts.lightVolumes = EditorGUILayout.Toggle(new GUIContent("Light Volumes",
                "VRC Light Volumes (red.sim.lightvolumes): one volume per cell; needs Castlemist > Bake Lighting and Occlusion (Unity / Bakery)."), opts.lightVolumes);
            opts.reflectionProbes = EditorGUILayout.Toggle(new GUIContent("Reflection probes",
                "A baked box-projected reflection probe per cell (" + opts.reflectionResolution + " px); default reflection from the sky."), opts.reflectionProbes);
            opts.occlusion = EditorGUILayout.Toggle(new GUIContent("Occlusion and GI flags",
                "Occluder on opaque, occludee on everything static, ContributeGI with Light Probes GI on opaque and cutout; an OcclusionArea over the map."), opts.occlusion);
            if (GUILayout.Button(new GUIContent("Quest preset", "Terrain Mesh, Foliage Collider trees, Distance 0.5 (D9); no post-processing, " +
                                                                "128 px reflection probes, a smaller light volume budget; keeps the Shader.")))
            {
                ShaderMode keep = opts.shader;
                string skipKeep = opts.skipModels;
                opts = ImportOptions.Quest();
                opts.shader = keep;
                opts.skipModels = skipKeep;
            }
            using (new EditorGUI.DisabledScope(path == null))
            {
                if (GUILayout.Button("Import"))
                {
                    if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
                    try
                    {
                        ImportSummary s = WorldImporter.Import(path, unityScale, opts);
                        WorldImporter.Log(s);
                        status = s.PlanLine() + "\n" + (s.probeLine ?? "no probe") + "\n" + (s.terrainCheckLine ?? "") + "\n" + (s.envLine ?? "") + "\n" + s.warnings.Count + " warnings, " + s.errors.Count + " errors (see the Console)";
                    }
                    catch (Exception e)
                    {
                        status = "Import failed: " + e.Message;
                        Debug.LogException(e);
                    }
                }
            }
            if (status.Length > 0) EditorGUILayout.HelpBox(status, MessageType.Info);
        }
    }
}
