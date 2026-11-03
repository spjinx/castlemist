// castlemist world export: a smaller VRChat upload (2026-10-25). Castlemist > Optimize Build Size, on
// the open world scene's export folder (the scene's own folder). Lion's Arch built 4.2 GB uncompressed,
// 1.4 GB compressed: the Light Volume atlas 897 MB (RGBAHalf, uncompressible), the scene ("Levels")
// 1.4 GB (static batching's combined copy of every static mesh), textures 1.7 GB, meshes 609 MB.
//   static batching  off for Standalone (Player Settings): the copy goes; every castlemist material
//                    is GPU-instanced, so draw calls batch by instancing instead
//   meshes           every world .fbx with mesh compression off (2026-10-27: it quantized the UVs into blocks)
//   textures         every export texture capped at MaxTexture px, EXCEPT terrain/ (the baked ground),
//                    sky/, environment/ and Materials/ (the map, the water depth)
//   materials        (Clean Materials) every saved property the material's current shader does not
//                    have is removed: switching shaders keeps the old ones (Poiyomi's ~2000 floats on
//                    every castlemist material: 2600 materials, 518 MB on disk, 391 MB in the build)
//   light volumes    (separate: Coarse Light Volumes) the per-cell volumes at ~CoarseBudget voxels;
//                    the detail volumes over the walkable floors stay; needs a Light Volumes re-bake
//   prop textures    (separate: Smaller Prop Textures, 2026-10-26) textures/ at PropTexture px, crunched at
//                    CrunchQuality (Lion's Arch: 3833 of them in the build, 403 MB at 1024 px); only the
//                    ones the last build packed when it has a report
//   asset meshes     (in Optimize) the .asset meshes the importer writes (FoliagePatches, Meshes) at Medium
//                    mesh compression: the .fbx setting does not reach them (foliage patches: 186 MB)
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Castlemist
{
    public static class BuildSize
    {
        public const int MaxTexture = 1024;
        public const int CoarseBudget = 24576;   // ~29^3 (ImmersiveLighting's 196608 made 18.9M voxels in 96 volumes)

        [MenuItem("Castlemist/Optimize Build Size")]
        public static void OptimizeMenu() { Debug.Log("CASTLEMIST " + Optimize(true, true, true)); }

        public const int PropTexture = 512;
        public const int CrunchQuality = 50;

        [MenuItem("Castlemist/Smaller Prop Textures (512 px, crunched)")]
        public static void PropTexturesMenu() { Debug.Log("CASTLEMIST " + PropTextures(PropTexture, CrunchQuality)); }

        /// The export's prop textures (textures/: the ones the last build packed, else all) at @p max px,
        /// crunched at @p quality (0..100; below 0: not crunched); returns a one-line report.
        public static string PropTextures(int max, int quality)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "prop textures: the open scene is not in a castlemist export folder";
            string folder = root + "/textures/";
            var paths = new HashSet<string>();
            foreach (UnityEngine.Object o in UnityEditorInternal.InternalEditorUtility.LoadSerializedFileAndForget("Library/LastBuild.buildreport"))
                if (o is UnityEditor.Build.Reporting.BuildReport rep)
                    foreach (var pa in rep.packedAssets)
                        foreach (var c in pa.contents)
                            if (c.type == typeof(Texture2D) && c.sourceAssetPath != null && c.sourceAssetPath.StartsWith(folder)) paths.Add(c.sourceAssetPath);
            bool fromBuild = paths.Count > 0;
            if (!fromBuild) foreach (string g in AssetDatabase.FindAssets("t:Texture2D", new[] { folder.TrimEnd('/') })) paths.Add(AssetDatabase.GUIDToAssetPath(g));
            int n = 0;
            AssetDatabase.StartAssetEditing();
            try
            {
                foreach (string path in paths)
                {
                    if (!(AssetImporter.GetAtPath(path) is TextureImporter ti)) continue;
                    bool crunch = quality >= 0;
                    if (ti.maxTextureSize <= max && ti.crunchedCompression == crunch && (!crunch || ti.compressionQuality == quality)) continue;
                    ti.maxTextureSize = Mathf.Min(ti.maxTextureSize, max);
                    if (ti.textureCompression == TextureImporterCompression.Uncompressed) ti.textureCompression = TextureImporterCompression.Compressed;
                    ti.crunchedCompression = crunch;
                    if (crunch) ti.compressionQuality = quality;
                    ti.SaveAndReimport();
                    ++n;
                }
            }
            finally { AssetDatabase.StopAssetEditing(); }
            return "prop textures: " + n + " of " + paths.Count + (fromBuild ? " (the last build's)" : "") + " at " + max + " px" + (quality >= 0 ? ", crunched at " + quality : "");
        }

        /// The foliage patches (.asset meshes) at Medium mesh compression; returns how many.
        public static int AssetMeshes(string root)
        {
            int n = 0;
            // 2026-10-27: FoliagePatches only. Mesh compression quantizes the UVs, and the props' detail and
            // mask layers on their unwrap UVs (69887's rocks, Mad King's Labyrinth 506458) came out as
            // stair-stepped blocks; the baked static prop meshes in Meshes/ keep full precision.
            foreach (string sub in new[] { "FoliagePatches" })
            {
                if (!AssetDatabase.IsValidFolder(root + "/" + sub)) continue;
                foreach (string g in AssetDatabase.FindAssets("t:Mesh", new[] { root + "/" + sub }))
                {
                    string path = AssetDatabase.GUIDToAssetPath(g);
                    if (!path.EndsWith(".asset")) continue;
                    bool changed = false;
                    foreach (Mesh m in AssetDatabase.LoadAllAssetsAtPath(path).OfType<Mesh>())
                    {
                        if (MeshUtility.GetMeshCompression(m) != ModelImporterMeshCompression.Off) continue;
                        MeshUtility.SetMeshCompression(m, ModelImporterMeshCompression.Medium);
                        EditorUtility.SetDirty(m);
                        changed = true;
                    }
                    if (changed) ++n;
                }
            }
            AssetDatabase.SaveAssets();
            return n;
        }

        /// 2026-10-27: Vorbis quality of the sound effects (decompressed on load; the streamed music keeps its own).
        public const float SoundQuality = 0.5f;

        [MenuItem("Castlemist/Smaller Sound Effects (Vorbis 50%)")]
        public static void SoundsMenu() { Debug.Log("CASTLEMIST " + SmallerSounds(SoundQuality)); }

        /// The export's sound effects (every clip not streamed) at Vorbis @p quality when above it; returns a
        /// one-line report.
        public static string SmallerSounds(float quality)
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "sounds: the open scene is not in a castlemist export folder";
            int n = 0, all = 0;
            AssetDatabase.StartAssetEditing();
            try
            {
                foreach (string g in AssetDatabase.FindAssets("t:AudioClip", new[] { root }))
                {
                    if (!(AssetImporter.GetAtPath(AssetDatabase.GUIDToAssetPath(g)) is AudioImporter ai)) continue;
                    ++all;
                    AudioImporterSampleSettings s = ai.defaultSampleSettings;
                    if (s.loadType == AudioClipLoadType.Streaming || (s.compressionFormat == AudioCompressionFormat.Vorbis && s.quality <= quality + 1e-3f)) continue;
                    s.compressionFormat = AudioCompressionFormat.Vorbis;
                    s.quality = quality;
                    ai.defaultSampleSettings = s;
                    ai.SaveAndReimport();
                    ++n;
                }
            }
            finally { AssetDatabase.StopAssetEditing(); }
            return "sounds: " + n + " of " + all + " sound effect(s) at Vorbis " + Mathf.RoundToInt(quality * 100) + "% (streamed music kept)";
        }

        [MenuItem("Castlemist/Coarse Light Volumes (smaller atlas)")]
        public static void CoarseMenu() { Debug.Log("CASTLEMIST " + ImmersiveLighting.CoarseVolumes(CoarseBudget) + ": re-bake the Light Volumes"); }

        [MenuItem("Castlemist/Clean Materials (strip unused properties)")]
        public static void CleanMenu() { Debug.Log("CASTLEMIST " + CleanMaterials()); }

        /// Reflection probe resolution for the build (2026-10-25: 96 probes at 256 px were 104 MB).
        public const int ProbeResolution = 128;

        [MenuItem("Castlemist/Smaller Reflection Probes (re-bake)")]
        public static void ProbesMenu() { Debug.Log("CASTLEMIST " + SmallerProbes(true)); }

        /// Every reflection probe under the open scene at ProbeResolution px; @p bake re-bakes them.
        public static string SmallerProbes(bool bake)
        {
            int n = 0;
            foreach (ReflectionProbe p in UnityEngine.Object.FindObjectsByType<ReflectionProbe>(FindObjectsInactive.Include, FindObjectsSortMode.None))
            {
                if (p.resolution <= ProbeResolution) continue;
                Undo.RecordObject(p, "Probe resolution");
                p.resolution = ProbeResolution;
                ++n;
            }
            EditorSceneManager.MarkSceneDirty(EditorSceneManager.GetActiveScene());
            int baked = bake && n > 0 ? SceneSetup.BakeReflectionProbes() : 0;
            return "reflection probes: " + n + " set to " + ProbeResolution + " px" + (bake ? ", " + baked + " re-baked" : "");
        }

        /// Removes from every material of the open scene's export folder the saved properties (textures,
        /// floats, ints, colours) its shader does not declare; returns a one-line report.
        public static string CleanMaterials()
        {
            string root = Path.GetDirectoryName(EditorSceneManager.GetActiveScene().path)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "materials: the open scene is not in a castlemist export folder";
            int mats = 0, removed = 0;
            // One copy per material, not a delete per entry: SerializedProperty deletes run in the square
            // of the list (2000 x 2600 of them held the editor for half an hour, 2026-10-25). A fresh
            // material of the same shader gets the shader's properties, keywords, queue, instancing,
            // GI flags and RenderType, and is copied over the original (its GUID and references kept).
            foreach (string g in AssetDatabase.FindAssets("t:Material", new[] { root }))
            {
                var m = AssetDatabase.LoadAssetAtPath<Material>(AssetDatabase.GUIDToAssetPath(g));
                if (m == null || m.shader == null || m.shader.name == "Hidden/InternalErrorShader") continue;
                var so = new SerializedObject(m);
                int saved = 0;
                foreach (string list in new[] { "m_SavedProperties.m_TexEnvs", "m_SavedProperties.m_Floats", "m_SavedProperties.m_Ints", "m_SavedProperties.m_Colors" })
                {
                    SerializedProperty arr = so.FindProperty(list);
                    if (arr != null) saved += arr.arraySize;
                }
                Shader sh = m.shader;
                int count = sh.GetPropertyCount();
                if (saved <= count) continue;   // nothing extra
                var fresh = new Material(sh) { name = m.name };
                for (int i = 0; i < count; ++i)
                {
                    string pn = sh.GetPropertyName(i);
                    if (!m.HasProperty(pn)) continue;
                    switch (sh.GetPropertyType(i))
                    {
                        case UnityEngine.Rendering.ShaderPropertyType.Texture:
                            fresh.SetTexture(pn, m.GetTexture(pn));
                            fresh.SetTextureScale(pn, m.GetTextureScale(pn));
                            fresh.SetTextureOffset(pn, m.GetTextureOffset(pn));
                            break;
                        case UnityEngine.Rendering.ShaderPropertyType.Color: fresh.SetColor(pn, m.GetColor(pn)); break;
                        case UnityEngine.Rendering.ShaderPropertyType.Vector: fresh.SetVector(pn, m.GetVector(pn)); break;
                        case UnityEngine.Rendering.ShaderPropertyType.Int: fresh.SetInteger(pn, m.GetInteger(pn)); break;
                        default: fresh.SetFloat(pn, m.GetFloat(pn)); break;
                    }
                }
                fresh.shaderKeywords = m.shaderKeywords;
                fresh.renderQueue = m.renderQueue;
                fresh.enableInstancing = m.enableInstancing;
                fresh.doubleSidedGI = m.doubleSidedGI;
                fresh.globalIlluminationFlags = m.globalIlluminationFlags;
                string rt = m.GetTag("RenderType", false, "");
                if (rt != "") fresh.SetOverrideTag("RenderType", rt);
                EditorUtility.CopySerialized(fresh, m);
                UnityEngine.Object.DestroyImmediate(fresh);
                EditorUtility.SetDirty(m);
                removed += saved - count;
                ++mats;
            }
            AssetDatabase.SaveAssets();
            return "materials: " + removed + " unused saved propert(ies) removed from " + mats + " material(s)";
        }

        /// The optimisations above on the open scene's export folder; returns a one-line report.
        public static string Optimize(bool staticBatching, bool meshes, bool textures)
        {
            string scenePath = EditorSceneManager.GetActiveScene().path;
            string root = Path.GetDirectoryName(scenePath)?.Replace('\\', '/');
            if (string.IsNullOrEmpty(root) || !File.Exists(root + "/manifest.json")) return "build size: the open scene is not in a castlemist export folder";
            var parts = new List<string>();
            if (staticBatching) parts.Add(StaticBatchingOff());
            int nm = 0, nt = 0;
            AssetDatabase.StartAssetEditing();
            try
            {
                if (meshes)
                    foreach (string g in AssetDatabase.FindAssets("t:Model", new[] { root }))
                    {
                        string p = AssetDatabase.GUIDToAssetPath(g);
                        // 2026-10-27: off, not Medium: compression quantizes the UVs (see AssetMeshes); a world
                        // compressed by an earlier pass is put back.
                        if (!(AssetImporter.GetAtPath(p) is ModelImporter mi) || mi.meshCompression == ModelImporterMeshCompression.Off) continue;
                        mi.meshCompression = ModelImporterMeshCompression.Off;
                        mi.SaveAndReimport();
                        ++nm;
                    }
                if (textures)
                    foreach (string g in AssetDatabase.FindAssets("t:Texture2D", new[] { root }))
                    {
                        string p = AssetDatabase.GUIDToAssetPath(g);
                        string rel = p.Substring(root.Length + 1);
                        if (rel.StartsWith("terrain/") || rel.StartsWith("sky/") || rel.StartsWith("environment/") || rel.StartsWith("Materials/") ||
                            rel.StartsWith("TerrainLayers/") || rel.StartsWith("GW2_")) continue;
                        if (!(AssetImporter.GetAtPath(p) is TextureImporter ti) || ti.maxTextureSize <= MaxTexture) continue;
                        ti.maxTextureSize = MaxTexture;
                        ti.SaveAndReimport();
                        ++nt;
                    }
            }
            finally { AssetDatabase.StopAssetEditing(); }
            if (meshes) parts.Add(nm + " model(s) back to no mesh compression, " + AssetMeshes(root) + " foliage patch mesh(es) at Medium");
            if (textures) parts.Add(nt + " texture(s) capped at " + MaxTexture + " px (terrain, sky and environment kept)");
            return "build size: " + string.Join("; ", parts);
        }

        static string StaticBatchingOff()
        {
            MethodInfo get = typeof(PlayerSettings).GetMethod("GetBatchingForPlatform", BindingFlags.NonPublic | BindingFlags.Static);
            MethodInfo set = typeof(PlayerSettings).GetMethod("SetBatchingForPlatform", BindingFlags.NonPublic | BindingFlags.Static);
            if (get == null || set == null) return "static batching: not reachable in this Unity (untick Player Settings > Other > Static Batching)";
            var args = new object[] { BuildTarget.StandaloneWindows64, 0, 0 };
            get.Invoke(null, args);
            set.Invoke(null, new object[] { BuildTarget.StandaloneWindows64, 0, args[2] });
            AssetDatabase.SaveAssets();
            return "static batching off (was " + ((int)args[1] != 0 ? "on" : "off") + ")";
        }
    }
}
