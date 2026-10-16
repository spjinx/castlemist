// castlemist world export: import settings by file role, before the first import.
//
// Textures (docs/research/poiyomi-10-importer.md §5, "Textures"): in a model
// folder (a Textures/ folder beside materials.json) the file name carries the
// role, "<Mat> - <Role>[_n].png":
//   Normal                                         -> NormalMap
//   Packed, EmissionMask, Distortion, DecalMask,
//   AlphaMask, RimMask                             -> sRGB off (data maps)
//   BaseColor                                      -> sRGB, alphaIsTransparency
//   EmissionMap, Emission, Decal, Projector        -> sRGB (colour maps)
// World files (an ancestor folder holds a castlemist-world manifest.json):
//   cells/<id>_albedo.png, cells/<id>_water.png    -> clamp (cell edges); water alphaIsTransparency
//   sky/**/equirect.png                            -> no mips, clamp V (Skybox/Panoramic)
// Every castlemist texture gets maxTextureSize >= its own size (read from the
// PNG header), so Unity's 2048 default never shrinks an equirect or albedo.
//
// Models (spec D6; world export Task 1): every .fbx under a castlemist world
// folder gets ModelImporter.bakeAxisConversion = FrameProbe.BakeAxisConversion
// (measured: east +X, north +Z, up +Y, no mirror, on Unity 6000.0.42f1 and 2022.3.22f1).
// Cells and the probe import no materials (the importer assigns its own);
// the probe is readable so FrameProbe can check its winding.
// There is no auto-build trigger for worlds (spec D6 "Entry").

using System;
using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public sealed class CastlemistPostprocessor : AssetPostprocessor
    {
        static readonly Dictionary<string, string> Roots = new Dictionary<string, string>(StringComparer.Ordinal);

        void OnPreprocessTexture()
        {
            var ti = (TextureImporter)assetImporter;
            string path = assetPath.Replace('\\', '/');
            if (!path.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) return;
            string dir = Path.GetDirectoryName(path).Replace('\\', '/');
            string name = Path.GetFileNameWithoutExtension(path);

            bool ours = false;
            if (Path.GetFileName(dir) == "Textures" && File.Exists(Path.GetDirectoryName(dir) + "/materials.json"))
            {
                ours = true;
                ApplyRole(ti, RoleOf(name));
            }
            else if (WorldRoot(dir) != null)
            {
                ours = true;
                if (name.EndsWith("_albedo", StringComparison.Ordinal) || name.EndsWith("_water", StringComparison.Ordinal))
                {
                    ti.wrapMode = TextureWrapMode.Clamp;
                    if (name.EndsWith("_water", StringComparison.Ordinal)) ti.alphaIsTransparency = true;
                }
                else if (name == "equirect")
                {
                    ti.mipmapEnabled = false;
                    ti.wrapModeU = TextureWrapMode.Repeat;
                    ti.wrapModeV = TextureWrapMode.Clamp;
                }
            }
            if (!ours) return;
            int size = PngSize(path);
            if (size > ti.maxTextureSize) ti.maxTextureSize = Math.Min(16384, NextPow2(size));
        }

        void OnPreprocessModel()
        {
            string path = assetPath.Replace('\\', '/');
            if (!path.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase)) return;
            string dir = Path.GetDirectoryName(path).Replace('\\', '/');
            string root = WorldRoot(dir);
            if (root == null && !File.Exists(dir + "/materials.json")) return;
            string rel = root != null ? path.Substring(root.Length + 1) : Path.GetFileName(path);
            ApplyModelSettings((ModelImporter)assetImporter, rel);
        }

        /// The settings above for a world .fbx at `rel` (relative to the
        /// export folder). Returns true when something changed.
        public static bool ApplyModelSettings(ModelImporter mi, string rel)
        {
            bool changed = false;
            if (mi.bakeAxisConversion != FrameProbe.BakeAxisConversion) { mi.bakeAxisConversion = FrameProbe.BakeAxisConversion; changed = true; }
            bool probe = rel == "probe.fbx";
            if (rel.StartsWith("cells/", StringComparison.Ordinal) || probe)
            {
                if (mi.materialImportMode != ModelImporterMaterialImportMode.None) { mi.materialImportMode = ModelImporterMaterialImportMode.None; changed = true; }
                if (mi.importAnimation) { mi.importAnimation = false; changed = true; }
                if (mi.animationType != ModelImporterAnimationType.None) { mi.animationType = ModelImporterAnimationType.None; changed = true; }
            }
            if (probe && !mi.isReadable) { mi.isReadable = true; changed = true; }
            return changed;
        }

        public enum Role { Unknown, Normal, Data, BaseColor, Colour }

        public static Role RoleOf(string fileStem)
        {
            int dash = fileStem.LastIndexOf(" - ", StringComparison.Ordinal);
            if (dash < 0) return Role.Unknown;
            string role = fileStem.Substring(dash + 3);
            int us = role.LastIndexOf('_');  // unique_name's "_2" suffix
            if (us > 0 && int.TryParse(role.Substring(us + 1), out _)) role = role.Substring(0, us);
            switch (role)
            {
                case "Normal": return Role.Normal;
                case "Packed": case "EmissionMask": case "Distortion": case "DecalMask": case "AlphaMask": case "RimMask":
                    return Role.Data;
                case "BaseColor": return Role.BaseColor;
                case "EmissionMap": case "Emission": case "Decal": case "Projector": return Role.Colour;
                default: return Role.Unknown;
            }
        }

        static void ApplyRole(TextureImporter ti, Role r)
        {
            switch (r)
            {
                case Role.Normal: ti.textureType = TextureImporterType.NormalMap; break;
                case Role.Data: ti.sRGBTexture = false; break;
                case Role.BaseColor: ti.sRGBTexture = true; ti.alphaIsTransparency = true; break;
                case Role.Colour: ti.sRGBTexture = true; break;
            }
        }

        /// The asset folder of the castlemist world export that contains `dir`
        /// (a folder holding a manifest.json with "castlemist-world"), or null.
        public static string WorldRoot(string dir)
        {
            string d = dir;
            var seen = new List<string>();
            while (!string.IsNullOrEmpty(d) && d.StartsWith("Assets", StringComparison.Ordinal))
            {
                string known;
                if (Roots.TryGetValue(d, out known)) { foreach (string s in seen) Roots[s] = known; return known; }
                seen.Add(d);
                string man = d + "/manifest.json";
                if (File.Exists(man) && Head(man).Contains("castlemist-world"))
                {
                    foreach (string s in seen) Roots[s] = d;
                    return d;
                }
                int slash = d.LastIndexOf('/');
                d = slash > 0 ? d.Substring(0, slash) : null;
            }
            foreach (string s in seen) Roots[s] = null;
            return null;
        }

        /// Forgets the folder lookups (call after a world folder is added).
        public static void ClearCache() { Roots.Clear(); }

        static string Head(string path)
        {
            try
            {
                // "format" is near the top only by key order; read enough to be safe.
                using (var r = new StreamReader(path))
                {
                    var buf = new char[1 << 16];
                    int n = r.Read(buf, 0, buf.Length);
                    string s = new string(buf, 0, n);
                    if (s.Contains("castlemist-world") || n < buf.Length) return s;
                    return r.ReadToEnd();
                }
            }
            catch (IOException) { return ""; }
        }

        /// max(width, height) from a PNG's IHDR, or 0.
        static int PngSize(string path)
        {
            try
            {
                using (var f = File.OpenRead(path))
                {
                    var h = new byte[24];
                    if (f.Read(h, 0, 24) != 24 || h[1] != 'P' || h[2] != 'N' || h[3] != 'G') return 0;
                    int w = (h[16] << 24) | (h[17] << 16) | (h[18] << 8) | h[19];
                    int hh = (h[20] << 24) | (h[21] << 16) | (h[22] << 8) | h[23];
                    return Math.Max(w, hh);
                }
            }
            catch (IOException) { return 0; }
        }

        static int NextPow2(int v)
        {
            int p = 32;
            while (p < v) p <<= 1;
            return p;
        }
    }
}
