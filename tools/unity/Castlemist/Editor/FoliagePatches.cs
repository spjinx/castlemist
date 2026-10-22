// castlemist world export: foliage patches (world-foliage spec D8, "Types 1-2: patches").
//
// One patch per FoliagePatchPlan: the records of one model in one 1536-unit
// square (foliagePatchUnits) of a cell, split in record order every
// FoliagePlan.PatchCapacity(models[].triangles) records, so a patch has at most
// 65 535 vertices (3 a triangle; a single record above that gets 32-bit indices).
//   FoliagePatches/<cellId>/<fileId>_<k>.asset   the combined mesh
//   Cell_<id>/Foliage/Patches/<fileId>_<k>       MeshFilter + MeshRenderer (the model's
//                                                materials, one sub-mesh each) + a
//                                                LODGroup culled at cullHeight for the
//                                                patch's bounds diagonal
// The patch sits at its first record's unity.t (cell-local); each record's
// unity TRS is taken relative to it and Mesh.CombineMeshes bakes it in,
// sub-meshes kept (one per distinct material, in first-use order).
// The asset folder is FoliagePatches/, not the spec's Foliage/: on Windows
// that name is the export's own foliage/ folder (the .bin files), and the
// importer clears its patch folder on every import.

using System.Collections.Generic;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public sealed class FoliagePatches
    {
        public const string AssetFolder = "FoliagePatches";

        readonly string root;
        readonly WorldManifest m;
        readonly ImportSummary sum;
        readonly IDictionary<int, GameObject> modelPrefabs;
        readonly FoliageImporter owner;
        readonly float distance;
        readonly Dictionary<int, Part[]> parts = new Dictionary<int, Part[]>();
        int bigPatches;

        struct Part
        {
            public Mesh Mesh;
            public Matrix4x4 ToRoot;
            public Material[] Materials;
        }

        public FoliagePatches(string root, WorldManifest m, ImportSummary sum, IDictionary<int, GameObject> modelPrefabs,
                              FoliageImporter owner, float distance)
        {
            this.root = root;
            this.m = m;
            this.sum = sum;
            this.modelPrefabs = modelPrefabs;
            this.owner = owner;
            this.distance = distance;
        }

        /// Removes the previous import's patch meshes (they depend on Density and the mode).
        public void ClearAssets()
        {
            string dir = root + "/" + AssetFolder;
            if (AssetDatabase.IsValidFolder(dir)) AssetDatabase.DeleteAsset(dir);
        }

        /// The model prefab's static LOD0 meshes (not its "<name>_LOD<k>" objects, Task 15b), each with its
        /// transform to the prefab root; null when it has none.
        Part[] PartsOf(int model)
        {
            Part[] ps;
            if (parts.TryGetValue(model, out ps)) return ps;
            GameObject prefab;
            var list = new List<Part>();
            if (modelPrefabs.TryGetValue(model, out prefab))
            {
                Matrix4x4 toRoot = prefab.transform.worldToLocalMatrix;
                ICollection<string> far = LodGroups.EffectiveFar(prefab, m.Models[model].FarMeshes);
                foreach (MeshFilter mf in prefab.GetComponentsInChildren<MeshFilter>(true))
                {
                    var mr = mf.GetComponent<MeshRenderer>();
                    if (mf.sharedMesh == null || mr == null) continue;
                    if (LodGroups.IsLodObject(mf.gameObject, far)) continue;   // Task 15b: patches keep LOD0 only, no stand-ins
                    list.Add(new Part { Mesh = mf.sharedMesh, ToRoot = toRoot * mf.transform.localToWorldMatrix, Materials = mr.sharedMaterials });
                }
            }
            ps = list.Count > 0 ? list.ToArray() : null;
            parts[model] = ps;
            return ps;
        }

        /// Builds and places every patch of a cell under `parent` (Cell_<id>/Foliage/Patches).
        public void Place(FoliageCellPlan cp, Transform parent)
        {
            string folder = root + "/" + AssetFolder + "/" + cp.Cell.Id;
            EnsureFolder(folder);
            var built = new List<KeyValuePair<FoliagePatchPlan, Mesh>>();
            AssetDatabase.StartAssetEditing();
            try
            {
                foreach (FoliagePatchPlan pp in cp.Patches)
                {
                    Part[] ps = PartsOf(pp.Model);
                    if (ps == null) { sum.foliageSkipped += pp.Records.Count; continue; }
                    Mesh mesh = Combine(pp, ps, out Material[] mats);
                    if (mesh == null) { sum.foliageSkipped += pp.Records.Count; continue; }
                    long fileId = m.Models[pp.Model].FileId;
                    mesh.name = fileId + "_" + pp.K;
                    AssetDatabase.CreateAsset(mesh, folder + "/" + mesh.name + ".asset");
                    built.Add(new KeyValuePair<FoliagePatchPlan, Mesh>(pp, mesh));
                    pending[mesh] = mats;
                }
            }
            finally { AssetDatabase.StopAssetEditing(); }
            foreach (KeyValuePair<FoliagePatchPlan, Mesh> kv in built)
            {
                FoliagePatchPlan pp = kv.Key;
                Mesh mesh = kv.Value;
                FoliageRecord first = pp.Records[0];
                var go = new GameObject(mesh.name);
                go.transform.SetParent(parent, false);
                go.transform.localPosition = new Vector3(first.T[0], first.T[1], first.T[2]);
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                var mr = go.AddComponent<MeshRenderer>();
                mr.sharedMaterials = pending[mesh];
                GameObjectUtility.SetStaticEditorFlags(go, FoliageImporter.TreeStaticFlags);
                double fadeFar = owner.FadeFar(pp.Model);
                float cull = (float)FoliageRecords.CullHeight(mesh.bounds.size.magnitude, fadeFar, distance);
                LODGroup lg = go.AddComponent<LODGroup>();
                lg.SetLODs(new[] { new LOD(cull, new Renderer[] { mr }) });
                lg.RecalculateBounds();
                sum.foliagePatches++;
            }
            pending.Clear();
        }

        readonly Dictionary<Mesh, Material[]> pending = new Dictionary<Mesh, Material[]>();

        /// The records' copies of the model's parts in one mesh, one sub-mesh per distinct material.
        Mesh Combine(FoliagePatchPlan pp, Part[] ps, out Material[] mats)
        {
            FoliageRecord first = pp.Records[0];
            var origin = new Vector3(first.T[0], first.T[1], first.T[2]);
            var byMat = new List<KeyValuePair<Material, List<CombineInstance>>>();
            var index = new Dictionary<Material, int>();
            Material nullKey = null;
            foreach (FoliageRecord r in pp.Records)
            {
                Matrix4x4 rec = Matrix4x4.TRS(new Vector3(r.T[0], r.T[1], r.T[2]) - origin,
                                              new Quaternion(r.R[0], r.R[1], r.R[2], r.R[3]), Vector3.one * r.S);
                foreach (Part p in ps)
                {
                    for (int s = 0; s < p.Mesh.subMeshCount; ++s)
                    {
                        Material mat = p.Materials.Length > 0 ? p.Materials[Mathf.Min(s, p.Materials.Length - 1)] : nullKey;
                        int k;
                        if (mat == null)
                        {
                            sum.warnings.Add("foliage patch " + m.Models[pp.Model].FileId + ": a sub-mesh has no material: skipped");
                            continue;
                        }
                        if (!index.TryGetValue(mat, out k))
                        {
                            k = byMat.Count;
                            index[mat] = k;
                            byMat.Add(new KeyValuePair<Material, List<CombineInstance>>(mat, new List<CombineInstance>()));
                        }
                        byMat[k].Value.Add(new CombineInstance { mesh = p.Mesh, subMeshIndex = s, transform = rec * p.ToRoot });
                    }
                }
            }
            mats = new Material[byMat.Count];
            if (byMat.Count == 0) return null;
            long total = 0;
            var subs = new CombineInstance[byMat.Count];
            var temps = new List<Mesh>();
            try
            {
                for (int k = 0; k < byMat.Count; ++k)
                {
                    mats[k] = byMat[k].Key;
                    var t = new Mesh { indexFormat = IndexFormat.UInt32 };
                    t.CombineMeshes(byMat[k].Value.ToArray(), true, true);
                    total += t.vertexCount;
                    temps.Add(t);
                    subs[k] = new CombineInstance { mesh = t, subMeshIndex = 0, transform = Matrix4x4.identity };
                }
                var mesh = new Mesh { indexFormat = total > FoliagePlan.PatchMaxVertices ? IndexFormat.UInt32 : IndexFormat.UInt16 };
                mesh.CombineMeshes(subs, false, false);
                mesh.RecalculateBounds();
                if (total > FoliagePlan.PatchMaxVertices) ++bigPatches;
                return mesh;
            }
            finally { foreach (Mesh t in temps) Object.DestroyImmediate(t); }
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }

        public void Finish()
        {
            if (bigPatches > 0)
                sum.warnings.Add("foliage: " + bigPatches + " patches hold more than 65 535 vertices (a single record of a large model, or " +
                                 "fewer triangles than vertices / 3): 32-bit indices");
        }
    }
}
