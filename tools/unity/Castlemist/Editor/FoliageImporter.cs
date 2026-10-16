// castlemist world export: foliage tree prefabs and their instances.
//
// World-foliage spec D8 ("Mesh terrain: instances and patches", trees) and
// "Unity mapping additions", at phase-1 level:
//   Prefabs/Foliage/<fileId>.prefab   a variant of Prefabs/<fileId>.prefab with a
//                                     LODGroup holding LOD0 only, culled at
//                                     cullHeight = clamp(size / (2 fadeFar D tan(fov/2)), 1e-4, 1)
//                                     (size: the prefab's bounds diagonal; D = 1;
//                                     fov = lodCullFovDeg, 60); materials with
//                                     enableInstancing, BatchingStatic off
//                                     (instancing needs it), OccludeeStatic on
//   Cell_<id>/Foliage/Trees/<fileId>_<placement>
//                                     one prefab instance per tree record (layer
//                                     type 3, or layer 0xFFFF: a geometry-only
//                                     collider), unity.t / r / s as given
// Records of layer type 1-2 (details) are not placed here: they are counted in
// ImportSummary.foliageDeferred and a warning. A missing or malformed bin is an
// error naming it; a record whose model is failed (or has no prefab) is skipped
// and counted.

using System.Collections.Generic;
using System.IO;
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
        readonly IDictionary<int, GameObject> modelPrefabs;
        readonly Dictionary<string, List<FoliageRecord>> cellRecords = new Dictionary<string, List<FoliageRecord>>();
        readonly Dictionary<int, GameObject> treePrefabs = new Dictionary<int, GameObject>();

        public FoliageImporter(string root, WorldManifest m, ImportSummary sum, IDictionary<int, GameObject> modelPrefabs)
        {
            this.root = root;
            this.m = m;
            this.sum = sum;
            this.modelPrefabs = modelPrefabs;
        }

        bool ModelOk(uint model) { return model < (uint)m.Models.Count && m.Models[(int)model].Ok; }

        /// Reads every cell's bin, then saves one tree prefab per model that has
        /// tree records. Call after the model prefabs exist.
        public void Prefabs()
        {
            var treeModels = new SortedSet<int>();
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
                foreach (FoliageRecord r in recs)
                    if (FoliageRecords.IsTree(r, m.Foliage) && ModelOk(r.Model)) treeModels.Add((int)r.Model);
            }
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
                    double fadeFar = m.Foliage != null ? m.Foliage.FadeFar(model) : 0;
                    if (!(fadeFar > 0)) sum.warnings.Add("foliage model " + e.FileId + ": no fadeFar in the manifest: its LODGroup culls at 1e-4");
                    if (!any) sum.warnings.Add("foliage model " + e.FileId + ": no renderer: its LODGroup is empty");
                    float cull = (float)FoliageRecords.CullHeight(b.size.magnitude, fadeFar);
                    LODGroup lg = inst.GetComponent<LODGroup>();
                    if (lg == null) lg = inst.AddComponent<LODGroup>();
                    lg.SetLODs(new[] { new LOD(cull, rs) });
                    lg.RecalculateBounds();
                    string path = folder + "/" + e.FileId + ".prefab";
                    bool ok;
                    GameObject saved = PrefabUtility.SaveAsPrefabAsset(inst, path, out ok);
                    if (!ok || saved == null) { sum.errors.Add(path + ": prefab not saved"); continue; }
                    treePrefabs[model] = saved;
                }
                finally { Object.DestroyImmediate(inst); }
            }
        }

        /// Cell_<id>/Foliage/Trees: one instance per tree record, in record order.
        public void PlaceCell(WorldManifest.CellEntry c, Transform cell)
        {
            List<FoliageRecord> recs;
            if (!cellRecords.TryGetValue(c.Id, out recs) || recs.Count == 0) return;
            Transform trees = null;
            foreach (FoliageRecord r in recs)
            {
                if (!FoliageRecords.IsTree(r, m.Foliage)) { sum.foliageDeferred++; continue; }
                GameObject prefab;
                if (!ModelOk(r.Model) || !treePrefabs.TryGetValue((int)r.Model, out prefab)) { sum.foliageSkipped++; continue; }
                if (trees == null)
                {
                    var fol = new GameObject("Foliage");
                    fol.transform.SetParent(cell, false);
                    var tr = new GameObject("Trees");
                    tr.transform.SetParent(fol.transform, false);
                    trees = tr.transform;
                }
                var go = (GameObject)PrefabUtility.InstantiatePrefab(prefab, trees);
                go.name = m.Models[(int)r.Model].FileId + "_" + r.Placement;
                go.transform.localPosition = new Vector3(r.T[0], r.T[1], r.T[2]);
                go.transform.localRotation = new Quaternion(r.R[0], r.R[1], r.R[2], r.R[3]);
                go.transform.localScale = Vector3.one * r.S;
                sum.foliageTrees++;
            }
        }

        /// The summary's foliage warnings, after every cell is placed.
        public void Finish()
        {
            if (sum.foliageDeferred > 0)
                sum.warnings.Add("foliage: " + sum.foliageDeferred + " detail records (layer type 1-2) not placed: " +
                                 "this importer places trees only so far");
            if (sum.foliageSkipped > 0)
                sum.warnings.Add("foliage: " + sum.foliageSkipped + " tree records skipped (their model failed or has no prefab)");
        }
    }
}
