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
                // The root's own rotation and scale kept, as WorldImporter.Place keeps them for an instance.
                Matrix4x4 toRoot = Matrix4x4.TRS(Vector3.zero, prefab.transform.localRotation, prefab.transform.localScale) *
                                   prefab.transform.worldToLocalMatrix;
                ICollection<string> far = LodGroups.EffectiveFar(prefab, m.Models[model].FarMeshes);
                foreach (MeshFilter mf in prefab.GetComponentsInChildren<MeshFilter>(true))
                {
                    var mr = mf.GetComponent<MeshRenderer>();
                    if (mf.sharedMesh == null || mr == null) continue;
                    if (LodGroups.IsLodObject(mf.gameObject, far)) continue;   // Task 15b: patches keep LOD0 only, no stand-ins
                    list.Add(new Part { Mesh = mf.sharedMesh, ToRoot = toRoot * mf.transform.localToWorldMatrix, Materials = mr.sharedMaterials });
                }
                // A static model kept rigged (WorldImporter NeedsRig: a joint scale the FBX cannot
                // bake) has a SkinnedMeshRenderer: its posed mesh, baked from an instance at the origin.
                if (prefab.GetComponentsInChildren<SkinnedMeshRenderer>(true).Length > 0)
                {
                    var inst = (GameObject)PrefabUtility.InstantiatePrefab(prefab);
                    try
                    {
                        inst.transform.position = Vector3.zero;   // its own rotation and scale kept (WorldImporter.Place)
                        foreach (SkinnedMeshRenderer smr in inst.GetComponentsInChildren<SkinnedMeshRenderer>(true))
                        {
                            if (smr.sharedMesh == null || LodGroups.IsLodObject(smr.gameObject, far)) continue;
                            var baked = new Mesh { name = smr.sharedMesh.name + "_baked" };
                            smr.BakeMesh(baked, false);   // the renderer's space without its scale
                            bakedMeshes.Add(baked);
                            Transform t = smr.transform;
                            list.Add(new Part { Mesh = baked, ToRoot = Matrix4x4.TRS(t.position, t.rotation, Vector3.one), Materials = smr.sharedMaterials });
                        }
                    }
                    finally { Object.DestroyImmediate(inst); }
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
            var recs = new List<List<Matrix4x4>>();   // per byMat entry, each part's record (its plant)
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
                            recs.Add(new List<Matrix4x4>());
                        }
                        byMat[k].Value.Add(new CombineInstance { mesh = p.Mesh, subMeshIndex = s, transform = rec * p.ToRoot });
                        recs[k].Add(rec);
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
                    t.SetUVs(7, PlantChannel(byMat[k].Value, recs[k], t.vertexCount));
                    // No vertex colours (map 506592's pebbles 189628 / 189629): Unity would feed white, full
                    // bend weight to the vegetation wind; zero holds them still.
                    if (!t.HasVertexAttribute(VertexAttribute.Color)) t.colors32 = new Color32[t.vertexCount];
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

        /// UV7 per vertex, in CombineMeshes' order: its plant's origin (patch space, the record's unity.t
        /// less the patch's) and scale (w), for the vegetation wind (GW2Core.cginc), which needs each plant.
        /// CombineMeshes copies a part's whole vertex buffer or its sub-mesh's range; `total` tells which.
        static List<Vector4> PlantChannel(List<CombineInstance> parts, List<Matrix4x4> recs, int total)
        {
            int whole = 0;
            foreach (CombineInstance c in parts) whole += c.mesh.vertexCount;
            var uv = new List<Vector4>(total);
            for (int i = 0; i < parts.Count; ++i)
            {
                CombineInstance c = parts[i];
                Vector3 o = recs[i].MultiplyPoint3x4(Vector3.zero);
                float s = recs[i].MultiplyVector(Vector3.right).magnitude;
                int n = whole == total ? c.mesh.vertexCount : c.mesh.GetSubMesh(c.subMeshIndex).vertexCount;
                for (int v = 0; v < n; ++v) uv.Add(new Vector4(o.x, o.y, o.z, s));
            }
            while (uv.Count < total) uv.Add(Vector4.zero);   // never expected: no plant, no wind
            if (uv.Count > total) uv.RemoveRange(total, uv.Count - total);
            return uv;
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }

        readonly List<Mesh> bakedMeshes = new List<Mesh>();

        public void Finish()
        {
            foreach (Mesh b in bakedMeshes) Object.DestroyImmediate(b);
            bakedMeshes.Clear();
            parts.Clear();
            if (bigPatches > 0)
                sum.warnings.Add("foliage: " + bigPatches + " patches hold more than 65 535 vertices (a single record of a large model, or " +
                                 "fewer triangles than vertices / 3): 32-bit indices");
        }
    }
}
