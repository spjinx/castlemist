// castlemist world export: materials.json -> Poiyomi Toon 10 materials.
//
// Follows docs/research/poiyomi-10-importer.md:
//  §1  the shader is ".poiyomi/Poiyomi Toon", found with Shader.Find; no
//      compile-time reference to Poiyomi or ThryEditor.
//  §2  setting _Mode alone does nothing (ThryEditor's inspector runs the
//      preset's actions), so the preset table below writes the render queue,
//      the RenderType tag, the blend factors, _ZWrite, _Cutoff,
//      _AlphaForceOpaque and _AlphaPremultiply itself. castlemist's
//      alphaCutoff goes on after the preset; renderQueueOffset on top of the
//      queue. Where materials.json says exact: false, the raw bgfx factors
//      override _SrcBlend/_DstBlend/_BlendOp (and the alpha ones) as
//      Advanced Blending.
//  §2  packed map -> _MochieMetallicMaps with _MochieBRDF = 1 and keyword
//      MOCHIE_PBR; _MochieMetallicMultiplier = 1 (it defaults to 0).
//  §2  emission on slot 0: _EnableEmission + keyword _EMISSION,
//      _EmissionMap / _EmissionMask / _EmissionColor, _EmissionStrength > 0.
//  §2  UV scroll: _MainTexPan = 20 * scroll, because Poiyomi pans by
//      _Time.x = t / 20 (spec D3). V is negated: FBX/Unity UVs have v up,
//      glTF's (which the scroll is in) v down, the same flip the Blender
//      importer makes. The scroll unit itself is UNPROVEN
//      (settings.uvScrollUnitSeconds).
//  §3  keywords are enabled by hand for every toggle set; materials are
//      never locked (the VRChat upload hook locks them).
//  §5  existing .mat assets are updated in place, so GUIDs stay stable; user
//      edits are overwritten (spec D6 "Re-import").
// Without Poiyomi (a bare batchmode project), the Standard shader is used with
// the same blend table and one warning; that is the path batchmode can test.

using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace Castlemist
{
    public sealed class MaterialContext
    {
        public Shader Shader;
        public bool Poiyomi;
        public List<string> Warnings;
        public int Built;

        public static MaterialContext Create(List<string> warnings)
        {
            var c = new MaterialContext { Warnings = warnings };
            c.Shader = Shader.Find(PoiyomiMaterials.ShaderName);
            c.Poiyomi = c.Shader != null;
            if (!c.Poiyomi)
            {
                c.Shader = Shader.Find("Standard");
                warnings.Add("materials: Poiyomi Toon 10 ('" + PoiyomiMaterials.ShaderName +
                             "') is not in this project: the Standard shader is used instead");
            }
            else
            {
                string v = PoiyomiMaterials.PoiyomiVersion(c.Shader);
                if (v == null || !v.StartsWith("10."))
                    warnings.Add("materials: Poiyomi version " + (v ?? "unknown") +
                                 " found; the property names are those of 10.0.24 (docs/research/poiyomi-10-importer.md)");
            }
            return c;
        }
    }

    public static class PoiyomiMaterials
    {
        public const string ShaderName = ".poiyomi/Poiyomi Toon";

        // The preset rows of poiyomi-10-importer.md §2, indexed by _Mode.
        struct Preset
        {
            public int Queue, Src, Dst, ZWrite, AddSrc, AddDst;
            public string RenderType;
            public float Cutoff, ForceOpaque, Premultiply;
            public Preset(int queue, string type, int src, int dst, int zwrite, float cutoff, float forceOpaque, float premul, int addSrc, int addDst)
            {
                Queue = queue; RenderType = type; Src = src; Dst = dst; ZWrite = zwrite; Cutoff = cutoff;
                ForceOpaque = forceOpaque; Premultiply = premul; AddSrc = addSrc; AddDst = addDst;
            }
        }

        public const int Opaque = 0, Cutout = 1, Fade = 2, Transparent = 3, TransClipping = 9;

        static Preset PresetFor(int mode)
        {
            switch (mode)
            {
                case 0: return new Preset(2000, "Opaque", 1, 0, 1, 0f, 1f, 0f, 1, 1);
                case 1: return new Preset(2450, "TransparentCutout", 1, 0, 1, 0.5f, 0f, 0f, 1, 1);
                case 9: return new Preset(2460, "TransparentCutout", 5, 10, 1, 0.01f, 0f, 0f, 5, 1);
                case 2: return new Preset(3000, "Transparent", 5, 10, 0, 0.002f, 0f, 0f, 5, 1);
                case 3: return new Preset(3000, "Transparent", 1, 10, 0, 0f, 0f, 1f, 1, 1);
                case 4: return new Preset(3000, "Transparent", 1, 1, 0, 0f, 0f, 0f, 1, 1);
                case 5: return new Preset(3000, "Transparent", 4, 1, 0, 0f, 0f, 0f, 4, 1);
                case 6: return new Preset(3000, "Transparent", 2, 0, 0, 0f, 0f, 0f, 2, 1);
                case 7: return new Preset(3000, "Transparent", 2, 3, 0, 0f, 0f, 0f, 2, 1);
                default: return new Preset(3000, "Transparent", 5, 10, 0, 0.002f, 0f, 0f, 5, 1);  // Fade
            }
        }

        /// bgfx blend factor (exportgltf/blend_mode.h: 1 Zero, 2 One, 3 SrcColor,
        /// 4 InvSrcColor, 5 SrcAlpha, 6 InvSrcAlpha, 7 DstAlpha, 8 InvDstAlpha,
        /// 9 DstColor, 10 InvDstColor) -> UnityEngine.Rendering.BlendMode; -1 unset.
        public static int UnityFactor(int bgfx)
        {
            switch (bgfx)
            {
                case 1: return (int)BlendMode.Zero;
                case 2: return (int)BlendMode.One;
                case 3: return (int)BlendMode.SrcColor;
                case 4: return (int)BlendMode.OneMinusSrcColor;
                case 5: return (int)BlendMode.SrcAlpha;
                case 6: return (int)BlendMode.OneMinusSrcAlpha;
                case 7: return (int)BlendMode.DstAlpha;
                case 8: return (int)BlendMode.OneMinusDstAlpha;
                case 9: return (int)BlendMode.DstColor;
                case 10: return (int)BlendMode.OneMinusDstColor;
                default: return -1;
            }
        }

        /// bgfx equation (0 Add, 1 Sub, 2 RevSub, 3 Min, 4 Max) -> UnityEngine.Rendering.BlendOp.
        public static int UnityOp(int eq)
        {
            switch (eq)
            {
                case 1: return (int)BlendOp.Subtract;
                case 2: return (int)BlendOp.ReverseSubtract;
                case 3: return (int)BlendOp.Min;
                case 4: return (int)BlendOp.Max;
                default: return (int)BlendOp.Add;
            }
        }

        /// The version in Poiyomi's hidden shader_master_label ("Poiyomi 10.0.24"), or null.
        public static string PoiyomiVersion(Shader s)
        {
            int i = s.FindPropertyIndex("shader_master_label");
            if (i < 0) return null;
            Match m = Regex.Match(s.GetPropertyDescription(i), @"Poiyomi (\d+\.\d+[^<\s]*)");
            return m.Success ? m.Groups[1].Value : null;
        }

        /// One model material from its materials.json entry, created or updated
        /// at assetPath. darkDecal switches it to Fade (spec D3, darkDecalRule).
        public static Material BuildModelMaterial(MaterialContext ctx, MaterialsFile.MaterialEntry e, string modelFolder,
                                                  bool darkDecal, WorldManifest.UvScroll scroll, string assetPath)
        {
            string tag = Path.GetFileName(modelFolder) + "/" + e.Name;
            Material mat = GetOrCreate(ctx, assetPath);
            int mode = darkDecal ? Fade : e.Mode;
            ApplyPreset(ctx, mat, mode);
            if (!darkDecal && !e.Exact && e.Blend != null) ApplyRawBlend(ctx, mat, e.Blend);
            if (mode == Cutout || mode == TransClipping) SetF(mat, "_Cutoff", (float)e.AlphaCutoff);
            SetF(mat, "_Cull", e.Cull == "Off" ? (float)CullMode.Off : (float)CullMode.Back);
            mat.renderQueue = PresetFor(mode).Queue + e.RenderQueueOffset;

            SetColor(mat, "_Color", Color.white);
            MaterialsFile.MapRef bc = e.Map("baseColor");
            if (bc != null)
            {
                SetTex(ctx, mat, "_MainTex", modelFolder, bc, tag);
                SetF(mat, "_MainTexUV", bc.Uv);
            }
            MaterialsFile.MapRef n = e.Map("normal");
            if (n != null)
            {
                SetTex(ctx, mat, "_BumpMap", modelFolder, n, tag);
                SetF(mat, "_BumpMapUV", n.Uv);
                SetF(mat, "_BumpScale", 1f);
                if (!ctx.Poiyomi) mat.EnableKeyword("_NORMALMAP");
            }

            var unmapped = new List<string>();
            MaterialsFile.MapRef packed = e.Map("packed");
            if (packed != null)
            {
                if (ctx.Poiyomi)
                {
                    SetTex(ctx, mat, "_MochieMetallicMaps", modelFolder, packed, tag);
                    SetF(mat, "_MochieMetallicMapsUV", packed.Uv);
                    SetF(mat, "_MochieBRDF", 1f);
                    mat.EnableKeyword("MOCHIE_PBR");
                    SetF(mat, "_MochieMetallicMultiplier", 1f);
                    SetF(mat, "_MochieRoughnessMultiplier", 1f);
                    if (e.SpecularTint != null) SetColor(mat, "_MochieSpecularTint", Rgb(e.SpecularTint));
                    if (e.ReflectionTint != null) SetColor(mat, "_MochieReflectionTint", Rgb(e.ReflectionTint));
                }
                else unmapped.Add("packed (Standard)");
            }

            MaterialsFile.MapRef emap = e.Map("emissionMap"), emask = e.Map("emissionMask");
            if (emap == null && emask == null) emap = e.Map("emissionBaked");
            if (e.HasEmission || emap != null || emask != null)
            {
                Color c = e.HasEmission ? Rgb(e.EmissionColor) : Color.white;
                float strength = e.HasEmission ? (float)e.EmissionStrength : 1f;
                if (strength <= 0f) strength = 1f;
                if (emap != null)
                {
                    SetTex(ctx, mat, "_EmissionMap", modelFolder, emap, tag);
                    SetF(mat, "_EmissionMapUV", emap.Uv);
                }
                if (ctx.Poiyomi)
                {
                    SetF(mat, "_EnableEmission", 1f);
                    mat.EnableKeyword("_EMISSION");
                    if (emask != null)
                    {
                        SetTex(ctx, mat, "_EmissionMask", modelFolder, emask, tag);
                        SetF(mat, "_EmissionMaskUV", emask.Uv);
                    }
                    SetColor(mat, "_EmissionColor", c);
                    SetF(mat, "_EmissionStrength", strength);
                }
                else
                {
                    mat.EnableKeyword("_EMISSION");
                    mat.globalIlluminationFlags = MaterialGlobalIlluminationFlags.RealtimeEmissive;
                    SetColor(mat, "_EmissionColor", c * strength);
                    if (emask != null) unmapped.Add("emissionMask (Standard)");
                }
            }

            if (scroll != null)
            {
                if (bc != null && bc.Uv == scroll.UvIndex)
                    SetVec(mat, "_MainTexPan", new Vector4((float)(20.0 * scroll.U), (float)(-20.0 * scroll.V), 0f, 0f));
                else
                    ctx.Warnings.Add(tag + ": uvScroll on UV" + scroll.UvIndex + " is not the base colour's UV: not panned");
                if (!ctx.Poiyomi) unmapped.Add("uvScroll (Standard)");
            }

            foreach (string role in new[] { "distortion", "decal", "decalMask", "alphaMask" })
                if (e.Map(role) != null) unmapped.Add(role);
            if (e.HasRim) unmapped.Add("rim");
            if (e.HasProjector) unmapped.Add("projector");
            if (e.HasExtras) unmapped.Add("extras");
            if (unmapped.Count > 0)
                ctx.Warnings.Add(tag + ": not mapped by the importer: " + string.Join(", ", unmapped));

            mat.enableInstancing = true;  // spec D6 step 7
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        /// Terrain (Opaque) or water (Fade) material with one main texture.
        public static Material BuildSimple(MaterialContext ctx, string assetPath, Texture tex, int mode)
        {
            Material mat = GetOrCreate(ctx, assetPath);
            ApplyPreset(ctx, mat, mode);
            mat.renderQueue = PresetFor(mode).Queue;
            SetColor(mat, "_Color", Color.white);
            if (mat.HasProperty("_MainTex")) mat.SetTexture("_MainTex", tex);
            SetF(mat, "_Cull", (float)CullMode.Back);
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        static Material GetOrCreate(MaterialContext ctx, string assetPath)
        {
            var mat = AssetDatabase.LoadAssetAtPath<Material>(assetPath);
            if (mat != null)
            {
                // Update in place (stable GUID); start from the keywords a new
                // material of the shader has, as a first import does.
                if (mat.shader != ctx.Shader) mat.shader = ctx.Shader;
                var fresh = new Material(ctx.Shader);
                mat.shaderKeywords = fresh.shaderKeywords;
                Object.DestroyImmediate(fresh);
                return mat;
            }
            mat = new Material(ctx.Shader) { name = Path.GetFileNameWithoutExtension(assetPath) };
            AssetDatabase.CreateAsset(mat, assetPath);
            return mat;
        }

        static void ApplyPreset(MaterialContext ctx, Material mat, int mode)
        {
            Preset p = PresetFor(mode);
            mat.SetOverrideTag("RenderType", p.RenderType);
            SetF(mat, "_SrcBlend", p.Src);
            SetF(mat, "_DstBlend", p.Dst);
            SetF(mat, "_ZWrite", p.ZWrite);
            SetF(mat, "_Cutoff", p.Cutoff);
            if (ctx.Poiyomi)
            {
                SetF(mat, "_Mode", mode);
                SetF(mat, "_AlphaForceOpaque", p.ForceOpaque);
                SetF(mat, "_AlphaPremultiply", p.Premultiply);
                SetF(mat, "_AddSrcBlend", p.AddSrc);
                SetF(mat, "_AddDstBlend", p.AddDst);
                SetF(mat, "_OutlineSrcBlend", p.Src);
                SetF(mat, "_OutlineDstBlend", p.Dst);
                // Common to every preset (§2).
                SetF(mat, "_BlendOp", 0);
                SetF(mat, "_BlendOpAlpha", 4);
                SetF(mat, "_SrcBlendAlpha", 1);
                SetF(mat, "_DstBlendAlpha", 1);
                SetF(mat, "_AddSrcBlendAlpha", 0);
                SetF(mat, "_AddDstBlendAlpha", 1);
                SetF(mat, "_OutlineSrcBlendAlpha", 1);
                SetF(mat, "_OutlineDstBlendAlpha", 1);
                SetF(mat, "_AlphaToCoverage", 0);
                SetF(mat, "_ZTest", 4);
            }
            else
            {
                // Standard reads _SrcBlend/_DstBlend/_ZWrite; its _Mode and
                // keywords pick the alpha path.
                int std = mode == Opaque ? 0 : (mode == Cutout || mode == TransClipping) ? 1 : mode == Transparent ? 3 : 2;
                SetF(mat, "_Mode", std);
                if (std == 1) mat.EnableKeyword("_ALPHATEST_ON");
                else if (std == 2) mat.EnableKeyword("_ALPHABLEND_ON");
                else if (std == 3) mat.EnableKeyword("_ALPHAPREMULTIPLY_ON");
            }
        }

        static void ApplyRawBlend(MaterialContext ctx, Material mat, MaterialsFile.BlendRaw b)
        {
            int src = UnityFactor(b.SrcRgb), dst = UnityFactor(b.DstRgb);
            int srcA = UnityFactor(b.SrcA), dstA = UnityFactor(b.DstA);
            if (src >= 0) SetF(mat, "_SrcBlend", src);
            if (dst >= 0) SetF(mat, "_DstBlend", dst);
            SetF(mat, "_BlendOp", UnityOp(b.EqRgb));
            if (ctx.Poiyomi)
            {
                if (srcA >= 0) SetF(mat, "_SrcBlendAlpha", srcA);
                if (dstA >= 0) SetF(mat, "_DstBlendAlpha", dstA);
                SetF(mat, "_BlendOpAlpha", UnityOp(b.EqA));
                if (src >= 0) SetF(mat, "_OutlineSrcBlend", src);
                if (dst >= 0) SetF(mat, "_OutlineDstBlend", dst);
            }
        }

        static void SetTex(MaterialContext ctx, Material mat, string prop, string folder, MaterialsFile.MapRef m, string tag)
        {
            string path = AssetPath(folder + "/" + m.File.Replace('\\', '/'));
            var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(path);
            if (tex == null) { ctx.Warnings.Add(tag + ": " + prop + " texture missing: " + path); return; }
            if (mat.HasProperty(prop)) mat.SetTexture(prop, tex);
        }

        /// `path` with "." and ".." segments folded (a world export's maps are
        /// ../../textures/gen/... from the model folder; the AssetDatabase wants
        /// a plain Assets/... path).
        public static string AssetPath(string path)
        {
            var parts = new List<string>();
            foreach (string s in path.Split('/'))
            {
                if (s == "." || s.Length == 0) continue;
                if (s == ".." && parts.Count > 0 && parts[parts.Count - 1] != "..") parts.RemoveAt(parts.Count - 1);
                else parts.Add(s);
            }
            return string.Join("/", parts.ToArray());
        }

        static void SetF(Material m, string p, float v) { if (m.HasProperty(p)) m.SetFloat(p, v); }
        static void SetColor(Material m, string p, Color c) { if (m.HasProperty(p)) m.SetColor(p, c); }
        static void SetVec(Material m, string p, Vector4 v) { if (m.HasProperty(p)) m.SetVector(p, v); }

        /// materials.json colours are sRGB bytes / 255; Unity material colours
        /// are authored in sRGB too, so they are used as they are.
        static Color Rgb(double[] c) { return new Color((float)c[0], (float)c[1], (float)c[2], 1f); }
    }
}
