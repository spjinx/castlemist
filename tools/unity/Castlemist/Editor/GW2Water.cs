// GW2Water (2026-10-25): the map's water as ONE plane on ONE Castlemist/GW2 Water material (its
// header has the formulas) when the export carries the preset's surface (manifest water.material).
// The cells' water bakes (rgb the colour by depth, a the depth) are stitched into one map-wide
// texture, Materials/water/WaterDepth.png, over the bounding rect of the cells that have one; the
// shader looks it up by world position (_MapRect), so there is no per-cell material and no seam.
// Past the rect the texture clamps to its edge; texels no cell covers are the deep colour (open sea).
// "GW2 Water Plane" spans the rect plus PlaneMargin map units at water.z; the cells' own Water
// renderers are switched off (kept). The bump is textures[BumpTextureIndex]; units UNPROVEN: angles
// in degrees as the Map Window (world_prep.h), tile unit, speed and strength castlemist's looks.
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace Castlemist
{
    public static class GW2Water
    {
        public const int BumpTextureIndex = 1;      // AMAT 187551's ssBump0 (world_prep.h)
        public const float BumpStrengthScale = 0.007f;   // 2026-10-25: 0.02 (the Map Window's) read choppy
        public const float TileUnit = 4000f, SpeedScale = 0.02f;   // see GW2 Water's header
        public const int MaxSize = 4096;            // the stitched texture's longest side
        public const float PlaneMargin = 40000f;    // map units past the cells: the sea to the horizon
        public const string PlaneName = "GW2 Water Plane";

        /// Builds the plane, texture and material under @p mapRoot; returns 1, or 0 when not built.
        public static int Apply(GameObject mapRoot, WorldManifest m, string root, float unityScale, List<string> warnings)
        {
            if (m.Water == null || !m.Water.HasMaterial) return 0;
            Shader shader = Shader.Find("Castlemist/GW2 Water");
            if (shader == null) { warnings.Add("water: Castlemist/GW2 Water is not compiled"); return 0; }

            // The cells with a bake, and their rect.
            var cells = new List<WorldManifest.CellEntry>();
            double x0 = double.MaxValue, y0 = double.MaxValue, x1 = double.MinValue, y1 = double.MinValue, density = 0;
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                if (c.WaterTexture == null || c.Rect == null) continue;
                var tex = Readable(root + "/" + c.WaterTexture);
                if (tex == null) continue;
                cells.Add(c);
                x0 = System.Math.Min(x0, c.Rect[0]); y0 = System.Math.Min(y0, c.Rect[1]);
                x1 = System.Math.Max(x1, c.Rect[2]); y1 = System.Math.Max(y1, c.Rect[3]);
                density = System.Math.Max(density, tex.width / System.Math.Max(c.Rect[2] - c.Rect[0], 1));
            }
            if (cells.Count == 0) { warnings.Add("water: no cell water bake: no water plane"); return 0; }
            double k = System.Math.Min(density, MaxSize / System.Math.Max(x1 - x0, y1 - y0));
            int W = Mathf.Clamp((int)System.Math.Ceiling((x1 - x0) * k), 16, MaxSize), H = Mathf.Clamp((int)System.Math.Ceiling((y1 - y0) * k), 16, MaxSize);

            // Stitch: row 0 south, as the shader's v = (y - y0) / (y1 - y0).
            var px = new Color[W * H];
            Color deep = m.Water.Deep != null ? new Color((float)m.Water.Deep[0], (float)m.Water.Deep[1], (float)m.Water.Deep[2], 1f) : new Color(0.05f, 0.2f, 0.25f, 1f);
            for (int i = 0; i < px.Length; ++i) px[i] = deep;
            foreach (WorldManifest.CellEntry c in cells)
            {
                var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(root + "/" + c.WaterTexture);
                int i0 = Mathf.Clamp((int)((c.Rect[0] - x0) / (x1 - x0) * W), 0, W), i1 = Mathf.Clamp((int)System.Math.Ceiling((c.Rect[2] - x0) / (x1 - x0) * W), 0, W);
                int j0 = Mathf.Clamp((int)((c.Rect[1] - y0) / (y1 - y0) * H), 0, H), j1 = Mathf.Clamp((int)System.Math.Ceiling((c.Rect[3] - y0) / (y1 - y0) * H), 0, H);
                for (int j = j0; j < j1; ++j)
                {
                    double y = y0 + (j + 0.5) / H * (y1 - y0);
                    float v = (float)((y - c.Rect[1]) / (c.Rect[3] - c.Rect[1]));
                    if (v < 0 || v > 1) continue;
                    for (int i = i0; i < i1; ++i)
                    {
                        double x = x0 + (i + 0.5) / W * (x1 - x0);
                        float u = (float)((x - c.Rect[0]) / (c.Rect[2] - c.Rect[0]));
                        if (u < 0 || u > 1) continue;
                        px[j * W + i] = tex.GetPixelBilinear(u, v);
                    }
                }
            }
            if (!AssetDatabase.IsValidFolder(root + "/Materials/water")) AssetDatabase.CreateFolder(root + "/Materials", "water");
            string texPath = root + "/Materials/water/WaterDepth.png";
            var atlas = new Texture2D(W, H, TextureFormat.RGBA32, false);
            atlas.SetPixels(px);
            File.WriteAllBytes(texPath, atlas.EncodeToPNG());
            Object.DestroyImmediate(atlas);
            AssetDatabase.ImportAsset(texPath);
            if (AssetImporter.GetAtPath(texPath) is TextureImporter ati)
            {
                ati.wrapMode = TextureWrapMode.Clamp;
                ati.textureCompression = TextureImporterCompression.CompressedHQ;
                ati.maxTextureSize = MaxSize;
                ati.SaveAndReimport();
            }

            // The bump.
            string bumpPath = BumpTextureIndex < m.Water.Textures.Count ? m.Water.Textures[BumpTextureIndex] : null;
            Texture2D bump = bumpPath != null ? AssetDatabase.LoadAssetAtPath<Texture2D>(root + "/" + bumpPath) : null;
            if (bump != null && AssetImporter.GetAtPath(root + "/" + bumpPath) is TextureImporter bti &&
                (bti.sRGBTexture || bti.wrapMode != TextureWrapMode.Repeat))
            {
                bti.sRGBTexture = false;   // the xy are a normal, read raw
                bti.wrapMode = TextureWrapMode.Repeat;
                bti.SaveAndReimport();
            }
            if (bump == null) warnings.Add("water: no bump texture (textures[" + BumpTextureIndex + "]): flat water");

            // The material.
            string matPath = root + "/Materials/water/GW2_Water.mat";
            var mat = AssetDatabase.LoadAssetAtPath<Material>(matPath);
            if (mat == null) { mat = new Material(shader) { name = "GW2_Water" }; AssetDatabase.CreateAsset(mat, matPath); }
            mat.shader = shader;
            mat.SetTexture("_MainTex", AssetDatabase.LoadAssetAtPath<Texture2D>(texPath));
            mat.SetVector("_MapRect", new Vector4((float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)));
            mat.SetTexture("_BumpTex", bump);
            mat.SetVector("_BumpTile", V2(m.Water.BumpTile, 5, 13.6));
            mat.SetVector("_BumpSpeed", V2(m.Water.BumpSpeed, 1.3, 0.64));
            mat.SetVector("_BumpAngle", V2(m.Water.BumpAngle, 0, 0));
            mat.SetFloat("_BumpAmount", bump != null ? (float)m.Water.BumpAmount * BumpStrengthScale : 0f);
            mat.SetFloat("_UnityScale", unityScale);
            mat.SetFloat("_TileUnit", TileUnit);
            mat.SetFloat("_SpeedScale", SpeedScale);
            mat.SetFloat("_Fresnel", (float)m.Water.Fresnel);
            // The haze volume's values (see GW2 Water's header): the water fogs itself.
            var haze = HazeCopy.HazeOf(root, m.MapFileId);
            HazeCopy.CopyTo(mat, haze);
            mat.renderQueue = (int)RenderQueue.Transparent - 100;   // after the haze volume (Transparent-216): it fogs itself
            EditorUtility.SetDirty(mat);

            // The per-cell materials of the first version (<cell>_gw2.mat) go.
            foreach (string g in AssetDatabase.FindAssets("t:Material", new[] { root + "/Materials/water" }))
            {
                string p = AssetDatabase.GUIDToAssetPath(g);
                if (p.EndsWith("_gw2.mat")) AssetDatabase.DeleteAsset(p);
            }

            // The plane (map units, Unity axes, under the scaled root), and the cells' planes off.
            Transform old = mapRoot.transform.Find(PlaneName);
            if (old != null) Object.DestroyImmediate(old.gameObject);
            float h = -(float)m.Water.Z;   // map z is down
            float ax = (float)x0 - PlaneMargin, bx = (float)x1 + PlaneMargin, ay = (float)y0 - PlaneMargin, by = (float)y1 + PlaneMargin;
            var mesh = new Mesh { name = "GW2 Water Plane" };
            mesh.vertices = new[] { new Vector3(ax, h, ay), new Vector3(ax, h, by), new Vector3(bx, h, ay), new Vector3(bx, h, by) };
            mesh.uv = new[] { Vector2.zero, Vector2.up, Vector2.right, Vector2.one };
            mesh.triangles = new[] { 0, 1, 2, 2, 1, 3 };
            mesh.RecalculateNormals();
            mesh.RecalculateBounds();
            string meshPath = root + "/Materials/water/GW2_WaterPlane.asset";
            if (AssetDatabase.LoadAssetAtPath<Mesh>(meshPath) != null) AssetDatabase.DeleteAsset(meshPath);
            AssetDatabase.CreateAsset(mesh, meshPath);
            var go = new GameObject(PlaneName);
            go.transform.SetParent(mapRoot.transform, false);
            go.AddComponent<MeshFilter>().sharedMesh = mesh;
            var mr = go.AddComponent<MeshRenderer>();
            mr.sharedMaterial = mat;
            mr.shadowCastingMode = ShadowCastingMode.Off;
            mr.receiveShadows = false;
            mr.lightProbeUsage = LightProbeUsage.Off;
            GameObjectUtility.SetStaticEditorFlags(go, StaticEditorFlags.BatchingStatic);
            int off = 0;
            foreach (WorldManifest.CellEntry c in m.Cells)
            {
                Transform w = mapRoot.transform.Find("Cell_" + c.Id + "/Water");
                var wr = w != null ? w.GetComponent<MeshRenderer>() : null;
                if (wr != null && wr.enabled) { wr.enabled = false; ++off; }
            }
            warnings.Add("water: one GW2 Water plane over " + cells.Count + " cell bake(s) (" + W + "x" + H + "); " + off + " cell water renderer(s) switched off");
            return 1;
        }

        /// The texture at @p path, made CPU-readable (reimported when it was not).
        static Texture2D Readable(string path)
        {
            if (!(AssetImporter.GetAtPath(path) is TextureImporter ti)) return null;
            if (!ti.isReadable) { ti.isReadable = true; ti.SaveAndReimport(); }
            return AssetDatabase.LoadAssetAtPath<Texture2D>(path);
        }

        static Vector4 V2(double[] v, double a, double b)
        {
            return v != null && v.Length >= 2 ? new Vector4((float)v[0], (float)v[1], 0, 0) : new Vector4((float)a, (float)b, 0, 0);
        }
    }
}
