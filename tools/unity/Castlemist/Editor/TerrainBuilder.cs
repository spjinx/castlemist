// castlemist world export: Unity Terrain mode, baked layer (world-foliage spec D8
// "Unity Terrain (baked layer)"; docs/research/gw2-terrain-to-unity.md §5, §8.3).
//
//   GW2_<fileId>_Terrain             unscaled sibling of GW2_<fileId>
//     Tile_<id>                      Terrain + TerrainCollider at position x unityScale
//   TerrainData/<id>.asset           heightmapResolution n, size = size x unityScale,
//                                    SetHeights from terrain/<id>.r16 (row 0 south),
//                                    SetHoles from _holes.png (row 0 north; white = surface),
//                                    SetDetailResolution(detailResolution, 32), instance-count
//                                    detail layers, tree prototypes and TreeInstances
//   TerrainLayers/<id>.terrainlayer  diffuse = terrain/<id>_albedo.png, tileSize = the tile's size
//   TerrainDetails/<fileId>.asset, .prefab
//                                    a detail prototype: the model's one mesh, scaled by
//                                    unityScale (Terrain sizes are in Unity units), its
//                                    material with enableInstancing
//   Materials/terrain/TerrainStandard.mat   the materialTemplate (Nature/Terrain/Standard)
// Details and trees come from the foliage records the plan keeps (FoliagePlan),
// so Density applies; at density 1 with every kind shown they must equal the
// export's _d<k>.png and _trees.bin (TERRAIN_FILES). Distances (ruling S5):
// treeDistance = treeBillboardDistance = max tree fadeFar x fadeQualityScale x
// unityScale x D; detailObjectDistance = max detail fadeFar x ... (Unity clamps
// it to 250). TreeInstance: position (u, h, v) from the record's m, rotation =
// unityYaw (terrainTreeYawSign UNPROVEN), width / height scale = unity.s x
// unityScale (the prefab is in map units). No capsule colliders: the havk hulls
// collide. The terrain frame check: CheckFrame.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEngine;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public static class TerrainBuilder
    {
        public const int DetailResolutionPerPatch = 32;
        public const int DetailMaxCount = 255;
        public const string TemplateShader = "Nature/Terrain/Standard";

        public sealed class Result
        {
            public Terrain[] Tiles = new Terrain[0];
            public string FilesLine;
            public bool FilesOk = true;
        }

        /// Builds every tile of m.UnityTerrain. Errors and warnings go to `sum`.
        public static Result Build(string root, WorldManifest m, float unityScale, ImportOptions opts, FoliagePlan plan,
                                   IDictionary<int, GameObject> treePrefabs, IDictionary<int, GameObject> modelPrefabs,
                                   ImportSummary sum)
        {
            var res = new Result();
            WorldManifest.UnityTerrainBlock ut = m.UnityTerrain;
            if (ut == null) return res;
            string P(string rel) { return root + "/" + rel.Replace('\\', '/'); }
            EnsureFolder(root + "/TerrainData");
            EnsureFolder(root + "/TerrainLayers");
            EnsureFolder(root + "/Materials/terrain");

            Material template = null;
            Shader sh = Shader.Find(TemplateShader);
            if (sh == null) sum.warnings.Add("unityTerrain: shader " + TemplateShader + " not found: Unity's default terrain material is used");
            else
            {
                string path = root + "/Materials/terrain/TerrainStandard.mat";
                template = AssetDatabase.LoadAssetAtPath<Material>(path);
                if (template == null) { template = new Material(sh) { name = "TerrainStandard" }; AssetDatabase.CreateAsset(template, path); }
                else template.shader = sh;
            }
            if (ut.Tiles.Count > 0) sum.materials++;   // the shared template (validate_manifest.py plan)

            var plans = new Dictionary<string, FoliageTilePlan>(StringComparer.Ordinal);
            foreach (FoliageTilePlan tp in plan.Tiles) plans[tp.Tile.Id] = tp;
            var detailPrefabs = new Dictionary<int, GameObject>();
            bool fullSet = opts.density >= 1f && (opts.foliage == FoliageMode.All || opts.foliage == FoliageMode.Trees);
            bool fullDetails = opts.density >= 1f && opts.foliage == FoliageMode.All;
            int fileChecks = 0;
            var fileFails = new List<string>();

            double q = m.FadeQualityScale;
            double treeFar = 0, detailFar = 0;
            foreach (WorldManifest.TreePrototypeEntry t in ut.TreePrototypes) treeFar = Math.Max(treeFar, t.FadeFar);
            foreach (WorldManifest.DetailPrototypeEntry d in ut.DetailPrototypes) detailFar = Math.Max(detailFar, d.FadeFar);
            float treeDistance = (float)(treeFar * q * unityScale * opts.distance);
            float detailDistance = (float)(detailFar * q * unityScale * opts.distance);
            if (detailDistance > 250f)
                sum.warnings.Add("unityTerrain: detailObjectDistance " + detailDistance.ToString("F1", CultureInfo.InvariantCulture) + " clamped by Unity to 250");

            int treesOutsideRange = 0;
            var rootGo = new GameObject("GW2_" + m.MapFileId + "_Terrain");
            var byChunk = new Dictionary<long, Terrain>();
            var tiles = new List<Terrain>();
            int n = ut.HeightmapResolution, dres = ut.DetailResolution;
            double range = ut.HMax - ut.HMin;
            foreach (WorldManifest.TerrainTile tile in ut.Tiles)
            {
                string what = "unityTerrain tile " + tile.Id;
                byte[] r16;
                try { r16 = File.ReadAllBytes(P(tile.Heights)); }
                catch (IOException) { sum.errors.Add(what + ": missing file " + P(tile.Heights)); continue; }
                if (r16.Length != n * n * 2) { sum.errors.Add(what + ": " + P(tile.Heights) + " holds " + r16.Length + " bytes, expected " + n * n * 2); continue; }
                var td = new TerrainData();
                td.heightmapResolution = n;
                td.size = new Vector3((float)(tile.Size[0] * unityScale), (float)(tile.Size[1] * unityScale), (float)(tile.Size[2] * unityScale));
                var h = new float[n, n];
                for (int row = 0; row < n; ++row)
                    for (int col = 0; col < n; ++col)
                        h[row, col] = BitConverter.ToUInt16(r16, 2 * (row * n + col)) / 65535f;   // row 0 south = Unity z 0
                td.SetHeights(0, 0, h);
                if (tile.Holes != null)
                {
                    Color32[] px = LoadPng(P(tile.Holes), n - 1, what, sum);
                    if (px != null)
                    {
                        var holes = new bool[n - 1, n - 1];
                        for (int y = 0; y < n - 1; ++y)   // Texture rows run bottom-up: y 0 = south = Unity z 0
                            for (int x = 0; x < n - 1; ++x) holes[y, x] = px[y * (n - 1) + x].r > 127;
                        td.SetHoles(0, 0, holes);
                    }
                }
                // The baked layer.
                var albedo = AssetDatabase.LoadAssetAtPath<Texture2D>(P(tile.Albedo));
                if (albedo == null) sum.errors.Add(what + ": missing file " + P(tile.Albedo));
                string layerPath = root + "/TerrainLayers/" + tile.Id + ".terrainlayer";
                var layer = AssetDatabase.LoadAssetAtPath<TerrainLayer>(layerPath);
                if (layer == null) { layer = new TerrainLayer(); AssetDatabase.CreateAsset(layer, layerPath); }
                layer.diffuseTexture = albedo;
                layer.tileSize = new Vector2(td.size.x, td.size.z);
                layer.tileOffset = Vector2.zero;
                EditorUtility.SetDirty(layer);
                td.alphamapResolution = 16;   // one layer: weight 1 everywhere
                td.terrainLayers = new[] { layer };
                if (albedo != null) td.baseMapResolution = Mathf.Clamp(Mathf.NextPowerOfTwo(albedo.width), 16, 2048);

                FoliageTilePlan tp;
                plans.TryGetValue(tile.Id, out tp);

                // Details: one layer per prototype the tile's kept records use.
                td.SetDetailResolution(dres, DetailResolutionPerPatch);
                var protos = new List<DetailPrototype>();
                var layers = new List<int[,]>();
                if (tp != null)
                    foreach (KeyValuePair<int, int[]> kv in tp.Details)
                    {
                        WorldManifest.DetailPrototypeEntry de = ut.DetailPrototypes[kv.Key];
                        GameObject dp = DetailPrefab(root, m, de.Model, unityScale, modelPrefabs, detailPrefabs, sum);
                        if (dp == null) { foreach (int v in kv.Value) sum.foliageSkipped += v; continue; }
                        protos.Add(new DetailPrototype
                        {
                            prototype = dp,
                            usePrototypeMesh = true,
                            useInstancing = true,
                            renderMode = DetailRenderMode.VertexLit,
                            minWidth = (float)de.MinScale, maxWidth = (float)de.MaxScale,
                            minHeight = (float)de.MinScale, maxHeight = (float)de.MaxScale,
                            noiseSeed = 0,
                            alignToGround = 0f,
                            healthyColor = Color.white, dryColor = Color.white,
                        });
                        var layerCounts = new int[dres, dres];
                        int clamped = 0;
                        for (int dy = 0; dy < dres; ++dy)
                            for (int dx = 0; dx < dres; ++dx)
                            {
                                int v = kv.Value[dy * dres + dx];
                                if (v > DetailMaxCount) { ++clamped; v = DetailMaxCount; }
                                layerCounts[dres - 1 - dy, dx] = v;   // dy counts from the north; Unity rows from the south
                                if (v > 0) sum.detailCells++;
                            }
                        if (clamped > 0) sum.warnings.Add(what + ": detail prototype " + kv.Key + ": " + clamped + " cells over 255, clamped");
                        layers.Add(layerCounts);
                        if (fullDetails)
                        {
                            WorldManifest.TileDetail file = tile.Details.Find(d => d.Prototype == kv.Key);
                            ++fileChecks;
                            if (file == null) fileFails.Add(tile.Id + " d" + kv.Key + ": not in the manifest");
                            else
                            {
                                Color32[] px = LoadPng(P(file.Counts), dres, what, sum);
                                if (px != null)
                                {
                                    int bad = 0;
                                    for (int y = 0; y < dres; ++y)
                                        for (int x = 0; x < dres; ++x)
                                            if (px[y * dres + x].r != layerCounts[y, x]) ++bad;
                                    if (bad > 0) fileFails.Add(tile.Id + " d" + kv.Key + ": " + bad + " cells differ from " + file.Counts);
                                }
                            }
                        }
                    }
                if (fullDetails && tp != null && tile.Details.Count != tp.Details.Count)
                    fileFails.Add(tile.Id + ": " + tp.Details.Count + " detail layers, the manifest lists " + tile.Details.Count);
                td.detailPrototypes = protos.ToArray();
                td.SetDetailScatterMode(DetailScatterMode.InstanceCountMode);
                for (int k = 0; k < layers.Count; ++k) td.SetDetailLayer(0, 0, k, layers[k]);
                sum.detailPrototypes += protos.Count;

                // Trees.
                var treeProtos = new List<TreePrototype>();
                var protoOf = new Dictionary<int, int>();
                var instances = new List<TreeInstance>();
                double X0 = tile.Position[0], Z0 = tile.Position[2];
                if (tp != null)
                    foreach (FoliageRecord r in tp.Trees)
                    {
                        int pi;
                        if (!protoOf.TryGetValue((int)r.Model, out pi))
                        {
                            GameObject prefab;
                            if (!treePrefabs.TryGetValue((int)r.Model, out prefab)) { sum.foliageSkipped++; continue; }
                            pi = treeProtos.Count;
                            protoOf[(int)r.Model] = pi;
                            treeProtos.Add(new TreePrototype { prefab = prefab, bendFactor = 0f });
                        }
                        instances.Add(new TreeInstance
                        {
                            prototypeIndex = pi,
                            position = new Vector3((float)((r.M[9] - X0) / tile.Size[0]), (float)((-(double)r.M[11] - ut.HMin) / range),
                                                   (float)((r.M[10] - Z0) / tile.Size[2])),
                            rotation = r.UnityYaw,
                            widthScale = r.S * unityScale,
                            heightScale = r.S * unityScale,
                            color = Color.white,
                            lightmapColor = Color.white,
                        });
                    }
                foreach (TreeInstance ti in instances) if (ti.position.y < 0f || ti.position.y > 1f) ++treesOutsideRange;
                td.treePrototypes = treeProtos.ToArray();
                td.SetTreeInstances(instances.ToArray(), false);
                sum.treeInstances += instances.Count;
                if (fullSet)
                {
                    ++fileChecks;
                    string err = CompareTrees(P, tile, instances, tp != null ? tp.Trees : new List<FoliageRecord>(), ut);
                    if (err != null) fileFails.Add(tile.Id + ": " + err);
                }

                string dataPath = root + "/TerrainData/" + tile.Id + ".asset";
                if (AssetDatabase.LoadAssetAtPath<TerrainData>(dataPath) != null) AssetDatabase.DeleteAsset(dataPath);
                AssetDatabase.CreateAsset(td, dataPath);

                GameObject go = Terrain.CreateTerrainGameObject(td);
                go.name = "Tile_" + tile.Id;
                go.transform.SetParent(rootGo.transform, false);
                go.transform.localPosition = new Vector3((float)(tile.Position[0] * unityScale), (float)(tile.Position[1] * unityScale),
                                                         (float)(tile.Position[2] * unityScale));
                Terrain terrain = go.GetComponent<Terrain>();
                if (template != null) terrain.materialTemplate = template;
                if (treeDistance > 0f) { terrain.treeDistance = treeDistance; terrain.treeBillboardDistance = treeDistance; }
                if (detailDistance > 0f) terrain.detailObjectDistance = detailDistance;
                terrain.drawInstanced = true;
                GameObjectUtility.SetStaticEditorFlags(go, WorldImporter.StaticFlags);
                tiles.Add(terrain);
                byChunk[Key(tile.Chunks[0], tile.Chunks[1])] = terrain;
                sum.terrains++;
            }
            // Neighbours: north is map +y = chunk row - tileChunks (Unity +z).
            int tc = ut.TileChunks;
            foreach (WorldManifest.TerrainTile tile in ut.Tiles)
            {
                Terrain t;
                if (!byChunk.TryGetValue(Key(tile.Chunks[0], tile.Chunks[1]), out t)) continue;
                Terrain left, top, right, bottom;
                byChunk.TryGetValue(Key(tile.Chunks[0] - tc, tile.Chunks[1]), out left);
                byChunk.TryGetValue(Key(tile.Chunks[0], tile.Chunks[1] - tc), out top);
                byChunk.TryGetValue(Key(tile.Chunks[0] + tc, tile.Chunks[1]), out right);
                byChunk.TryGetValue(Key(tile.Chunks[0], tile.Chunks[1] + tc), out bottom);
                t.SetNeighbors(left, top, right, bottom);
            }
            if (treesOutsideRange > 0)
                sum.warnings.Add("unityTerrain: " + treesOutsideRange + " Terrain trees stand outside the terrain's height range: " +
                                 "Unity clamps them to its bottom or top");
            if (treeFar <= 0 && sum.treeInstances > 0) sum.warnings.Add("unityTerrain: no tree fadeFar: Unity's default treeDistance is kept");
            if (detailFar <= 0 && sum.detailPrototypes > 0) sum.warnings.Add("unityTerrain: no detail fadeFar: Unity's default detailObjectDistance is kept");
            res.Tiles = tiles.ToArray();
            res.FilesOk = fileFails.Count == 0;
            res.FilesLine = "TERRAIN_FILES checked=" + fileChecks + (fileChecks == 0 ? " (not at this density / foliage)" : "") +
                            (res.FilesOk ? " OK" : " FAIL " + string.Join("; ", fileFails.GetRange(0, Math.Min(5, fileFails.Count))));
            if (!res.FilesOk) sum.errors.Add("unityTerrain: the details or trees differ from the export's files: " + res.FilesLine);
            sum.warnings.Add("unityTerrain: treeDistance " + treeDistance.ToString("F1", CultureInfo.InvariantCulture) +
                             ", detailObjectDistance " + Math.Min(detailDistance, 250f).ToString("F1", CultureInfo.InvariantCulture) +
                             " (fadeFar x fadeQualityScale " + q.ToString("R", CultureInfo.InvariantCulture) + " x unityScale x Distance " +
                             opts.distance.ToString("R", CultureInfo.InvariantCulture) + "); billboards at treeDistance (UNPROVEN with LODGroup prefabs)");
            return res;
        }

        static long Key(int cx, int cy) { return ((long)cx << 32) ^ (uint)cy; }

        /// The Terrain trees against terrain/<tile>_trees.bin (same count; u, v, h, yaw and scale within 1e-5).
        static string CompareTrees(Func<string, string> P, WorldManifest.TerrainTile tile, List<TreeInstance> inst,
                                   List<FoliageRecord> recs, WorldManifest.UnityTerrainBlock ut)
        {
            if (inst.Count != tile.TreesRecords) return inst.Count + " trees, trees.bin has " + tile.TreesRecords;
            if (tile.TreesFile == null) return null;
            byte[] b;
            try { b = File.ReadAllBytes(P(tile.TreesFile)); }
            catch (IOException) { return "missing file " + P(tile.TreesFile); }
            if (b.Length != 24 * tile.TreesRecords) return tile.TreesFile + ": " + b.Length + " bytes";
            for (int i = 0; i < inst.Count; ++i)
            {
                float u = BitConverter.ToSingle(b, 24 * i + 4), v = BitConverter.ToSingle(b, 24 * i + 8), h = BitConverter.ToSingle(b, 24 * i + 12);
                float yaw = BitConverter.ToSingle(b, 24 * i + 16), s = BitConverter.ToSingle(b, 24 * i + 20);
                Vector3 p = inst[i].position;
                if (Math.Abs(p.x - u) > 1e-5 || Math.Abs(p.z - v) > 1e-5 || Math.Abs(p.y - h) > 1e-5 || yaw != recs[i].UnityYaw || s != recs[i].S)
                    return "tree " + i + " differs from " + tile.TreesFile;
            }
            return null;
        }

        /// TerrainDetails/<fileId>.prefab: the model's mesh baked at unityScale with its material (instancing on).
        static GameObject DetailPrefab(string root, WorldManifest m, int model, float unityScale, IDictionary<int, GameObject> modelPrefabs,
                                       Dictionary<int, GameObject> cache, ImportSummary sum)
        {
            GameObject done;
            if (cache.TryGetValue(model, out done)) return done;
            cache[model] = null;
            GameObject src;
            if (!modelPrefabs.TryGetValue(model, out src)) return null;
            // The first LOD0 mesh: never a "<name>_LOD<k>" object (Task 15b; details keep LOD0 only).
            MeshFilter mf = src.GetComponentsInChildren<MeshFilter>(true).FirstOrDefault(f => !LodGroups.IsLodObject(f.gameObject));
            MeshRenderer mr = mf != null ? mf.GetComponent<MeshRenderer>() : null;
            if (mf == null || mf.sharedMesh == null || mr == null)
            {
                sum.warnings.Add("detail model " + m.Models[model].FileId + ": no mesh renderer: not a detail");
                return null;
            }
            EnsureFolder(root + "/TerrainDetails");
            long fileId = m.Models[model].FileId;
            Matrix4x4 bake = Matrix4x4.Scale(Vector3.one * unityScale) * src.transform.worldToLocalMatrix * mf.transform.localToWorldMatrix;
            var mesh = Object.Instantiate(mf.sharedMesh);
            mesh.name = fileId + "_detail";
            Vector3[] v = mesh.vertices;
            for (int i = 0; i < v.Length; ++i) v[i] = bake.MultiplyPoint3x4(v[i]);
            mesh.vertices = v;
            Vector3[] nrm = mesh.normals;
            if (nrm != null && nrm.Length == v.Length)
            {
                Matrix4x4 nm = bake.inverse.transpose;
                for (int i = 0; i < nrm.Length; ++i) nrm[i] = nm.MultiplyVector(nrm[i]).normalized;
                mesh.normals = nrm;
            }
            mesh.RecalculateBounds();
            string meshPath = root + "/TerrainDetails/" + fileId + ".asset";
            if (AssetDatabase.LoadAssetAtPath<Mesh>(meshPath) != null) AssetDatabase.DeleteAsset(meshPath);
            AssetDatabase.CreateAsset(mesh, meshPath);
            Material mat = mr.sharedMaterial;
            if (mat != null && !mat.enableInstancing) { mat.enableInstancing = true; EditorUtility.SetDirty(mat); }
            var go = new GameObject(fileId.ToString());
            try
            {
                go.AddComponent<MeshFilter>().sharedMesh = mesh;
                go.AddComponent<MeshRenderer>().sharedMaterial = mat;
                bool ok;
                GameObject saved = PrefabUtility.SaveAsPrefabAsset(go, root + "/TerrainDetails/" + fileId + ".prefab", out ok);
                if (!ok || saved == null) { sum.errors.Add(root + "/TerrainDetails/" + fileId + ".prefab: prefab not saved"); return null; }
                cache[model] = saved;
                return saved;
            }
            finally { Object.DestroyImmediate(go); }
        }

        /// A square PNG's pixels (bottom row first, as Texture2D holds them), or null with an error.
        static Color32[] LoadPng(string path, int size, string what, ImportSummary sum)
        {
            byte[] b;
            try { b = File.ReadAllBytes(path); }
            catch (IOException) { sum.errors.Add(what + ": missing file " + path); return null; }
            var tex = new Texture2D(2, 2, TextureFormat.RGBA32, false, true);
            try
            {
                if (!tex.LoadImage(b) || tex.width != size || tex.height != size)
                {
                    sum.errors.Add(what + ": " + path + " is not a " + size + " x " + size + " PNG");
                    return null;
                }
                return tex.GetPixels32();
            }
            finally { Object.DestroyImmediate(tex); }
        }

        static void EnsureFolder(string path)
        {
            if (AssetDatabase.IsValidFolder(path)) return;
            int slash = path.LastIndexOf('/');
            EnsureFolder(path.Substring(0, slash));
            AssetDatabase.CreateFolder(path.Substring(0, slash), path.Substring(slash + 1));
        }

        /// The terrain frame check (spec D8): Terrain.SampleHeight at the manifest's 32 check points
        /// (unityTerrain.check: Unity xz and the expected height, map units before unityScale; heightmap
        /// sample vertices, ruling S6, so the triangle split does not matter). A mismatch over 0.5 units
        /// x unityScale fails, naming the axis: if the heights match with x mirrored, z mirrored or x and z
        /// swapped inside each tile, that axis is named; otherwise y (height scale or offset).
        public static bool CheckFrame(Terrain[] tiles, List<object> check, float unityScale, out string line)
        {
            double tol = 0.5 * unityScale;
            var pts = new List<double[]>();   // x, z, height (Unity units)
            foreach (object o in check)
            {
                var d = o as Dictionary<string, object>;
                var u = d != null && d.ContainsKey("unity") ? d["unity"] as List<object> : null;
                if (u == null || u.Count != 2 || !(u[0] is double) || !(u[1] is double) || !d.ContainsKey("height") || !(d["height"] is double))
                {
                    line = "TERRAIN_CHECK FAIL malformed unityTerrain.check entry";
                    return false;
                }
                pts.Add(new[] { (double)u[0] * unityScale, (double)u[1] * unityScale, (double)d["height"] * unityScale });
            }
            if (pts.Count == 0 || tiles.Length == 0)
            {
                line = "TERRAIN_CHECK FAIL " + (pts.Count == 0 ? "no check points" : "no tiles");
                return false;
            }
            // mode 0: as built; 1: x mirrored; 2: z mirrored; 3: x and z swapped (within the tile).
            var maxErr = new double[4];
            var fails = new int[4];
            int outside = 0;
            for (int k = 0; k < pts.Count; ++k)
            {
                Terrain t = TileAt(tiles, pts[k][0], pts[k][1]);
                if (t == null) { ++outside; continue; }
                Vector3 p0 = t.transform.position, sz = t.terrainData.size;
                double fx = (pts[k][0] - p0.x) / sz.x, fz = (pts[k][1] - p0.z) / sz.z;
                for (int mode = 0; mode < 4; ++mode)
                {
                    double ax = fx, az = fz;
                    if (mode == 1) ax = 1 - fx;
                    else if (mode == 2) az = 1 - fz;
                    else if (mode == 3) { ax = fz; az = fx; }
                    var at = new Vector3((float)(p0.x + ax * sz.x), 0f, (float)(p0.z + az * sz.z));
                    double got = t.SampleHeight(at) + p0.y;
                    double err = Math.Abs(got - pts[k][2]);
                    maxErr[mode] = Math.Max(maxErr[mode], err);
                    if (err > tol) fails[mode]++;
                }
            }
            bool ok = outside == 0 && fails[0] == 0;
            string head = "TERRAIN_CHECK points=" + pts.Count + " tiles=" + tiles.Length + " maxErr=" +
                          maxErr[0].ToString("G4", CultureInfo.InvariantCulture) + " tol=" + tol.ToString("G4", CultureInfo.InvariantCulture);
            if (ok) { line = head + " OK"; return true; }
            string axis;
            if (outside > 0) axis = "xz (" + outside + " points outside every tile)";
            else if (fails[2] == 0) axis = "z (heightmap rows flipped north-south)";
            else if (fails[1] == 0) axis = "x (heightmap columns flipped east-west)";
            else if (fails[3] == 0) axis = "x/z (heightmap axes swapped)";
            else axis = "y (height scale or offset)";
            line = head + " failed=" + fails[0] + " FAIL axis " + axis;
            return false;
        }

        static Terrain TileAt(Terrain[] tiles, double x, double z)
        {
            const double eps = 1e-3;
            foreach (Terrain t in tiles)
            {
                Vector3 p = t.transform.position, s = t.terrainData.size;
                if (x >= p.x - eps && x <= p.x + s.x + eps && z >= p.z - eps && z <= p.z + s.z + eps) return t;
            }
            return null;
        }
    }
}
