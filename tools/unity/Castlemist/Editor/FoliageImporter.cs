// castlemist world export: foliage tree prefabs, their instances and the patches.
//
// World-foliage spec D8 ("Mesh terrain: instances and patches", "Unity mapping
// additions") with the D9 knobs (ImportOptions; what goes where is FoliagePlan):
//   Prefabs/Foliage/<fileId>.prefab   a variant of Prefabs/<fileId>.prefab with a
//                                     LODGroup holding LOD0 only (a model with
//                                     models[].lods.levels > 1: its levels, Task 15b,
//                                     LodGroups.Build with this cull of the LODGroup's
//                                     own size), culled at
//                                     cullHeight = clamp(size / (2 fadeFar D tan(fov/2)), 1e-4, 1)
//                                     (size: the prefab's bounds diagonal; fadeFar:
//                                     the model's stored fadeFar x fadeQualityScale
//                                     (ruling S5); D: the Distance knob; fov =
//                                     lodCullFovDeg, 60); materials with
//                                     enableInstancing, BatchingStatic off
//                                     (instancing needs it), OccludeeStatic on.
//                                     Also the Terrain tree prototype (TerrainBuilder).
//   Cell_<id>/Foliage/Trees/<fileId>_<placement>
//                                     one prefab instance per tree record the plan
//                                     places (layer type 3, or 0xFFFF: a geometry-only
//                                     collider; in Terrain mode not the bit-3 ones),
//                                     unity.t / r / s as given
//   Cell_<id>/Foliage/Patches/<fileId>_<k>
//                                     type 1-2 records (not made details), FoliagePatches
// The wind flag (foliage.models[].wind) stays in the manifest: no sway is added
// (spec D10, the user's default). A missing or malformed bin is an error naming
// it; a record whose model is failed (or has no prefab) is skipped and counted.

using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public sealed class FoliageImporter
    {
        /// Flags of a foliage tree (spec D8): no BatchingStatic, so GPU instancing applies.
        public const StaticEditorFlags TreeStaticFlags = StaticEditorFlags.OccludeeStatic;

        readonly string root;
        readonly WorldManifest m;
        readonly ImportSummary sum;
        readonly ImportOptions opts;
        readonly IDictionary<int, GameObject> modelPrefabs;
        readonly Dictionary<string, List<FoliageRecord>> cellRecords = new Dictionary<string, List<FoliageRecord>>();
        readonly Dictionary<int, GameObject> treePrefabs = new Dictionary<int, GameObject>();
        readonly Dictionary<string, FoliageCellPlan> cellPlans = new Dictionary<string, FoliageCellPlan>();
        FoliagePatches patches;

        public FoliagePlan Plan { get; private set; }
        public IDictionary<int, GameObject> TreePrefabs { get { return treePrefabs; } }
        public IDictionary<int, GameObject> ModelPrefabs { get { return modelPrefabs; } }

        public FoliageImporter(string root, WorldManifest m, ImportSummary sum, IDictionary<int, GameObject> modelPrefabs, ImportOptions opts)
        {
            this.root = root;
            this.m = m;
            this.sum = sum;
            this.modelPrefabs = modelPrefabs;
            this.opts = opts;
        }

        bool ModelOk(uint model) { return model < (uint)m.Models.Count && m.Models[(int)model].Ok; }

        /// The fadeFar every cull uses: the stored value x fadeQualityScale (ruling S5); D is applied by CullHeight.
        public double FadeFar(int model) { return (m.Foliage != null ? m.Foliage.FadeFar(model) : 0) * m.FadeQualityScale; }

        /// Reads every cell's bin, plans what goes where, then saves one tree prefab per
        /// model that has a placed tree (prefab instance or Terrain tree). Call after the model prefabs exist.
        public void Prefabs()
        {
            if (opts.foliage != FoliageMode.Off)
                foreach (WorldManifest.CellEntry c in m.Cells)
                {
                    if (c.FoliageFile == null) continue;
                    string path = root + "/" + c.FoliageFile.Replace('\\', '/');
                    List<FoliageRecord> recs;
                    try { recs = FoliageRecords.Read(path); }
                    catch (ManifestException e) { sum.errors.Add("cell " + c.Id + " " + e.Message); continue; }
                    catch (IOException e) { sum.errors.Add("cell " + c.Id + " foliage: " + path + ": " + e.Message); continue; }
                    if (recs.Count != c.FoliageRecords)
                        sum.errors.Add("cell " + c.Id + " foliage: " + path + " holds " + recs.Count + " records, the manifest says " + c.FoliageRecords);
                    cellRecords[c.Id] = recs;
                }
            Plan = FoliagePlan.Build(m, c => { List<FoliageRecord> r; return cellRecords.TryGetValue(c.Id, out r) ? r : null; }, opts, sum.errors);
            foreach (FoliageCellPlan cp in Plan.Cells) cellPlans[cp.Cell.Id] = cp;

            var treeModels = new SortedSet<int>();
            foreach (FoliageCellPlan cp in Plan.Cells) foreach (FoliageRecord r in cp.Trees) treeModels.Add((int)r.Model);
            foreach (FoliageTilePlan tp in Plan.Tiles) foreach (FoliageRecord r in tp.Trees) treeModels.Add((int)r.Model);
            patches = new FoliagePatches(root, m, sum, modelPrefabs, this, opts.distance);
            patches.ClearAssets();
            if (treeModels.Count == 0) return;

            string folder = root + "/Prefabs/Foliage";
            if (!AssetDatabase.IsValidFolder(folder)) AssetDatabase.CreateFolder(root + "/Prefabs", "Foliage");
            foreach (int model in treeModels)
            {
                GameObject src;
                if (!modelPrefabs.TryGetValue(model, out src)) continue;  // counted when placed
                WorldManifest.ModelEntry e = m.Models[model];
                var inst = (GameObject)PrefabUtility.InstantiatePrefab(src);
                try
                {
                    inst.name = e.FileId.ToString();
                    inst.transform.localPosition = Vector3.zero;
                    inst.transform.localRotation = Quaternion.identity;
                    inst.transform.localScale = Vector3.one;
                    foreach (Transform t in inst.GetComponentsInChildren<Transform>(true))
                        GameObjectUtility.SetStaticEditorFlags(t.gameObject, TreeStaticFlags);
                    Renderer[] rs = inst.GetComponentsInChildren<Renderer>(true);
                    Bounds b = new Bounds(inst.transform.position, Vector3.zero);
                    bool any = false;
                    foreach (Renderer r in rs)
                    {
                        if (!any) { b = r.bounds; any = true; }
                        else b.Encapsulate(r.bounds);
                        foreach (Material mat in r.sharedMaterials)
                            if (mat != null && !mat.enableInstancing) { mat.enableInstancing = true; EditorUtility.SetDirty(mat); }
                    }
                    double fadeFar = FadeFar(model);
                    if (!(fadeFar > 0)) sum.warnings.Add("foliage model " + e.FileId + ": no fadeFar in the manifest: its LODGroup culls at 1e-4");
                    if (!any) sum.warnings.Add("foliage model " + e.FileId + ": no renderer: its LODGroup is empty");
                    if (e.LodLevels > 1 || LodGroups.EffectiveFar(inst, e.FarMeshes).Count > 0)
                    {
                        // Task 15b: the model's levels (and its stand-ins), the last ending at the cull (of the LODGroup's own size).
                        LodGroups.Build(inst, e.LodLevels, e.FarMeshes, e.LodDistances,
                                        size => FoliageRecords.CullHeight(size, fadeFar, opts.distance), sum.warnings, "foliage model " + e.FileId,
                                        e.HiddenMeshes);
                        sum.lodTreePrefabs++;
                    }
                    else
                    {
                        float cull = (float)FoliageRecords.CullHeight(b.size.magnitude, fadeFar, opts.distance);
                        LODGroup lg = inst.GetComponent<LODGroup>();
                        if (lg == null) lg = inst.AddComponent<LODGroup>();
                        lg.SetLODs(new[] { new LOD(cull, rs.Where(r => r.gameObject.activeSelf).ToArray()) });
                        lg.RecalculateBounds();
                    }
                    string path = folder + "/" + e.FileId + ".prefab";
                    bool ok;
                    GameObject saved = PrefabUtility.SaveAsPrefabAsset(inst, path, out ok);
                    if (!ok || saved == null) { sum.errors.Add(path + ": prefab not saved"); continue; }
                    treePrefabs[model] = saved;
                }
                finally { Object.DestroyImmediate(inst); }
            }
        }

        /// Cell_<id>/Foliage/Trees (one instance per planned tree, in record order) and Foliage/Patches.
        public void PlaceCell(WorldManifest.CellEntry c, Transform cell)
        {
            FoliageCellPlan cp;
            if (!cellPlans.TryGetValue(c.Id, out cp)) return;
            Transform fol = null, trees = null;
            int treeIndex = -1;
            foreach (FoliageRecord r in cp.Trees)
            {
                treeIndex++;
                GameObject prefab;
                if (!ModelOk(r.Model) || !treePrefabs.TryGetValue((int)r.Model, out prefab)) { sum.foliageSkipped++; continue; }
                if (trees == null)
                {
                    fol = Child(cell, "Foliage");
                    trees = Child(fol, "Trees");
                }
                var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, trees);
                go.name = TreeName(m.Models[(int)r.Model].FileId, r, treeIndex);
                go.transform.localPosition = new Vector3(r.T[0], r.T[1], r.T[2]);
                go.transform.localRotation = new Quaternion(r.R[0], r.R[1], r.R[2], r.R[3]);
                go.transform.localScale = Vector3.one * r.S;
                sum.foliageTrees++;
            }
            if (cp.Patches.Count > 0)
            {
                if (fol == null) fol = Child(cell, "Foliage");
                patches.Place(cp, Child(fol, "Patches"));
            }
        }

        /// A tree's GameObject name: <fileId>_<placement>. A collider linked by geometry only has
        /// no placement (UINT32_MAX), so it is named <fileId>_g<index in the cell's planned trees>
        /// instead, which keeps the names unique (final review M3).
        public static string TreeName(long fileId, FoliageRecord r, int treeIndex)
        {
            return fileId + "_" + (r.Placement == uint.MaxValue ? "g" + treeIndex : r.Placement.ToString());
        }

        static Transform Child(Transform parent, string name)
        {
            Transform t = parent.Find(name);
            if (t != null) return t;
            var go = new GameObject(name);
            go.transform.SetParent(parent, false);
            return go.transform;
        }

        /// The summary's foliage warnings, after every cell is placed.
        public void Finish()
        {
            if (patches != null) patches.Finish();
            if (Plan != null)
            {
                sum.warnings.Add("foliage: " + opts + " (fadeQualityScale " + m.FadeQualityScale.ToString("R") + "): " +
                                 Plan.Hidden + " records hidden by Foliage, " + Plan.Thinned + " thinned by Density, " +
                                 Plan.FailedModel + " of failed models, " + Plan.Unplaced + " of other layer types");
            }
            if (sum.foliageSkipped > 0)
                sum.warnings.Add("foliage: " + sum.foliageSkipped + " records skipped (their model failed or has no prefab or mesh)");
        }
    }
}
