// castlemist world export: materials.json -> castlemist's own shaders (ImportOptions.shader =
// ShaderMode.Castlemist; the Shader option of the Import World window).
//
// The property mapping is tools/unity/Castlemist/Shaders/README.md's:
//   Castlemist/GW2           every material but the particle-like ones, one shader whose blocks
//                            are material floats (no keywords): colour, mod, normal, packed,
//                            tints, emission; the GW2 parallax layer, mask, perturb, ramp, step
//                            and fades (IsParallax); lit, or unlit for GW2's unlit effects; every
//                            MODL texTransform as its UV set's animation; the preset's render
//                            state from PoiyomiMaterials.PresetFor, raw bgfx factors where exact
//                            is false
//   Castlemist/GW2 Particle  Additive, Soft Additive and the two Multiplicative presets (not a
//                            parallax layer): the base colour with the preset's blend, unlit, fog
//                            to black for the additive ones
// Every block float is written on every build (ResetBlocks), so a re-import never keeps a block
// a material had before. Blended materials have their ShadowCaster pass off. Existing .mat assets
// are updated in place (stable GUIDs).

using System.Collections.Generic;
using System.IO;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace Castlemist
{
    public static class CastlemistMaterials
    {
        /// Every mesh material is drawn by Castlemist/GW2 (effects unlit with their blend);
        /// Castlemist/GW2 Particle is for particle systems (BuildParticleMaterial).
        public static Material BuildModelMaterial(MaterialContext ctx, MaterialsFile.MaterialEntry e, string modelFolder,
                                                  bool darkDecal, WorldManifest.UvScroll scroll, string assetPath)
        {
            return BuildGW2(ctx, ctx.Shader, e, modelFolder, darkDecal, scroll, assetPath);
        }

        /// The GW2 parallax-layer effects (shader_profiles.cpp fx-parallax-layer 842652,
        /// fx-parallax-ramp 1507908, fx-parallax-fade 1730340): the parallax block draws them.
        public static bool IsParallax(MaterialsFile.MaterialEntry e)
        {
            return e.Profile == "fx-parallax-layer" || e.Profile == "fx-parallax-ramp" || e.Profile == "fx-parallax-fade";
        }

        /// GW2's screen-distortion effects (shader_profiles.cpp fx-distort-opaque 50159,
        /// fx-distort-opaque-mask 339341, fx-distort-mask 47396, fx-distort-soft 709206,
        /// fx-distort-step 592660 / 27304, fx-distort-lit 630598) and the refracting glass and water
        /// (IsGlass): Castlemist/GW2 Distort draws them.
        public static bool IsDistort(MaterialsFile.MaterialEntry e)
        {
            switch (e.Profile)
            {
                case "fx-distort-opaque": case "fx-distort-opaque-mask": case "fx-distort-mask":
                case "fx-distort-soft": case "fx-distort-step": case "fx-distort-lit":
                    return true;
            }
            return IsGlass(e);
        }

        /// GW2's lit refractions (glass-refract 49659, glass-refract-mask 63923, water-refract 57026):
        /// the _Glass block of Castlemist/GW2 Distort.
        public static bool IsGlass(MaterialsFile.MaterialEntry e)
        {
            return e.Profile == "glass-refract" || e.Profile == "glass-refract-mask" || e.Profile == "water-refract";
        }

        /// A distortion material on Castlemist/GW2 Distort (formulas in its header), in either shader mode.
        public static Material BuildDistort(MaterialContext ctx, MaterialsFile.MaterialEntry e, string folder, string assetPath)
        {
            string tag = Path.GetFileName(folder) + "/" + e.Name;
            Material mat = PoiyomiMaterials.GetOrCreate(ctx.DistortShader, assetPath);
            mat.shaderKeywords = new string[0];
            bool opaqueMask = e.Profile == "fx-distort-opaque-mask";
            bool glassOpaque = e.Profile == "glass-refract";
            bool glass = glassOpaque || e.Profile == "glass-refract-mask";
            bool water = e.Profile == "water-refract";
            bool opaque = e.Profile == "fx-distort-opaque" || opaqueMask || glassOpaque;
            bool soft = e.Profile == "fx-distort-soft", lit = e.Profile == "fx-distort-lit";
            // Opaque: the grabbed scene replaces the surface; the others blend it by their alpha.
            SetF(mat, "_SrcBlend", opaque ? (float)BlendMode.One : (float)BlendMode.SrcAlpha);
            SetF(mat, "_DstBlend", opaque ? (float)BlendMode.Zero : (float)BlendMode.OneMinusSrcAlpha);
            SetF(mat, "_ZWrite", opaque ? 1f : 0f);
            SetF(mat, "_Cull", e.Cull == "Off" ? (float)CullMode.Off : (float)CullMode.Back);
            mat.renderQueue = 3000 + e.RenderQueueOffset;   // after the opaque scene: the grab sees it
            mat.SetOverrideTag("RenderType", "Transparent");

            // The distortion map: "distort" (50159, 339341, 709206, 592660, 27304, 49659, 63923; 630598's
            // normal map), or "adddistort" + "normal" (47396), or "normal" (57026). A raw x-distort is
            // GW2's RG as stored. Glass and water also take the exporter's "Normal" (a Unity normal map,
            // green flipped): castlemist's role picker makes 63923's distort the normal when the same
            // file is also its decal (75926).
            MaterialsFile.Extra d = water ? null : e.ExtraOf("distort") ?? e.ExtraOf("adddistort");
            MaterialsFile.MapRef dm = d != null ? d.Map : null;
            bool dIsNormal = false;
            if (dm == null && (glass || water))
            {
                dm = e.Map("normal");
                dIsNormal = dm != null;
                if (dm == null) { MaterialsFile.Extra xn = e.ExtraOf("normal"); dm = xn != null ? xn.Map : null; }
            }
            Layer(ctx, mat, tag, folder, dm, "_DistortTex", "_DistortUV");
            SetF(mat, "_DistortIsNormal", dIsNormal ? 1f : 0f);
            MaterialsFile.MapRef n2 = e.Profile == "fx-distort-mask" ? e.Map("normal") : null;
            if (water) { MaterialsFile.Extra dn = e.ExtraOf("decalnormal"); n2 = dn != null ? dn.Map : null; }
            Layer(ctx, mat, tag, folder, n2, "_DistortTex2", "_DistortUV2");
            SetF(mat, "_UseMap2", n2 != null && !water ? 1f : 0f);   // the 47396 blend; water reads map2 itself
            SetF(mat, "_DistortTex2IsNormal", 0f);   // 47396 as before (raw read); 57026's decalnormal ships raw
            // 630598 has no distort uniform: its offset is normal.xy x 0.025 (a literal in the PS).
            SetF(mat, "_Distort", lit ? 0.025f : (float)e.Const("distort", 0, 0.0));
            // 49659 / 63923 / 57026 also add distort: (n.xy + 1) x distort (57026's in the shader).
            SetF(mat, "_DistortBias", e.Profile == "fx-distort-opaque" || glass ? 1f : 0f);
            // 339341's VS has no normal / WorldView: its offset is not scaled by the view reflection.
            SetF(mat, "_ViewReflect", opaqueMask ? 0f : 1f);
            // 709206 and 630598 have no four-tap foreground test.
            SetF(mat, "_DepthReject", soft || lit ? 0f : 1f);
            Color tint = Color.white;
            if (e.Gw2Vectors.ContainsKey("modxcol"))
                tint = new Color((float)(2 * e.Const("modxcol", 0, 0.5)), (float)(2 * e.Const("modxcol", 1, 0.5)), (float)(2 * e.Const("modxcol", 2, 0.5)), 1f);
            SetColor(mat, "_Tint", tint);
            SetF(mat, "_UseVertexColor", soft ? 1f : 0f);   // 709206: x vertex colour x 2, alpha x vertex alpha

            MaterialsFile.Extra mask = e.ExtraOf("mask");
            Layer(ctx, mat, tag, folder, mask != null ? mask.Map : null, "_MaskTex", "_MaskUV");
            // 339341: mask.R scales the offset; the material stays opaque.
            SetF(mat, "_MaskOffset", opaqueMask && mask != null ? 1f : 0f);
            float source = 0f;
            SetF(mat, "_MaskChannel", 0f);
            if ((e.Profile == "fx-distort-mask" || soft || water) && mask != null) source = 1f;   // mask.R
            if (e.Profile == "glass-refract-mask" && mask != null) { source = 1f; SetF(mat, "_MaskChannel", 1f); }   // mask.G
            if ((e.Profile == "glass-refract-mask" || water) && mask == null)
                ctx.Warnings.Add(tag + ": " + e.Profile + " without its mask: drawn fully opaque");
            if (e.Profile == "fx-distort-step" && mask != null)
            {
                source = 2f;
                SetF(mat, "_MaskChannel", 1f);   // mask.G
                SetF(mat, "_StepMin", (float)e.Const("stepmin", 0, 0.75));
                SetVec(mat, "_StepCol", new Vector4((float)e.Const("stepcol", 0, 1.0), (float)e.Const("stepcol", 1, 1.0),
                                                    (float)e.Const("stepcol", 2, 1.0), (float)e.Const("stepcol", 3, 1.0)));
                // 27304 (WvW "rebuild" ghosts): stepmin 0 in every MODL seen, which makes alpha 0;
                // the game presumably drives it with the repair progress.
                if (e.Const("stepmin", 0, 0.75) <= 0.0)
                    ctx.Warnings.Add(tag + ": " + e.Profile + " with stepmin 0 is fully transparent (a runtime-driven step?)");
            }
            SetF(mat, "_AlphaSource", source);
            SetF(mat, "_Cutoff", 1f / 255f);   // AlphaRef (an engine value; 1/255 also for 63923, 57026)
            MaterialsFile.Extra ramp = e.ExtraOf("ramp");
            Layer(ctx, mat, tag, folder, ramp != null ? ramp.Map : null, "_RampTex", null);
            SetF(mat, "_UseRamp", ramp != null ? 1f : 0f);
            SetF(mat, "_SoftFade", 0f);   // needs a depth texture; off as in Castlemist/GW2 (592660, 709206 have one)
            SetF(mat, "_DepDist", (float)(e.Const("depdist", 0, 16.0) * ctx.UnityScale));

            // 630598: lit glass. Its diffuse slot is fileId 0 on every model seen (a character's
            // runtime composite), so baseColor is usually absent and _LitDiffuse stands in.
            // 49659 / 63923: the same slot carries the decal (A: saturate(2a) coverage, saturate(2a - 1) gloss).
            SetF(mat, "_UseLit", lit ? 1f : 0f);
            MaterialsFile.MapRef dif = lit ? e.Map("baseColor") : null;
            MaterialsFile.Extra decal = glass ? e.ExtraOf("decal") : null;
            if (decal != null) dif = decal.Map;
            Layer(ctx, mat, tag, folder, dif, "_MainTex", "_MainUV");
            SetF(mat, "_UseMainTex", dif != null ? 1f : 0f);
            SetColor(mat, "_LitDiffuse", new Color(0.5f, 0.5f, 0.5f, 0.5f));
            SetF(mat, "_LitFadeDist", (float)(ctx.UnityScale / 0.000326));   // the PS's 1 - viewZ x 0.000326
            SetF(mat, "_SunScale", 1f);
            if (lit && dif == null)
                ctx.Warnings.Add(tag + ": fx-distort-lit without its diffuse (GW2 composites it at runtime): a neutral grey stands in");
            if (glass && decal == null)
                ctx.Warnings.Add(tag + ": " + e.Profile + " without its decal: drawn as clear refracting glass");

            // Refracting glass and water (49659, 63923: _Glass 1; 57026: _Glass 2).
            SetF(mat, "_Glass", glass ? 1f : water ? 2f : 0f);
            if (water)
            {
                // speccp: rgb a gamma-space colour (x 2 via unity_ColorSpaceDouble), w the exponent.
                SetColor(mat, "_GlassSpec", new Color((float)e.Const("speccp", 0, 0.0), (float)e.Const("speccp", 1, 0.0),
                                                      (float)e.Const("speccp", 2, 0.0), 1f));
                SetF(mat, "_GlassSpecPower", (float)e.Const("speccp", 3, 256.0));
                SetF(mat, "_GlassSpecDouble", 1f);
            }
            else
            {
                SetColor(mat, "_GlassSpec", Color.white);   // 630598, 49659, 63923: shSunColor x pow(N.H, 256)
                SetF(mat, "_GlassSpecPower", 256f);
                SetF(mat, "_GlassSpecDouble", 0f);
            }
            SetF(mat, "_ReflectionStrength", 1f);
            SetF(mat, "_LightVolumes", 1f);

            var anim = new Vector4[4];
            foreach (MaterialsFile.TexTransform t in e.TexTransforms)
                if (t.Uv >= 0 && t.Uv < 4)
                    anim[t.Uv] = new Vector4((float)t.Scroll[0], (float)-t.Scroll[1], (float)-t.Rotate, 0f);
            for (int k = 0; k < 4; ++k) SetVec(mat, "_UVAnim" + k, anim[k]);
            // A flat placeholder layer (layerConstants) is no offset, as in the game: nothing to say.
            if (dm == null && !e.LayerConstants.ContainsKey("distort") && !e.LayerConstants.ContainsKey("adddistort") &&
                !e.LayerConstants.ContainsKey("normal"))
                ctx.Warnings.Add(tag + ": " + e.Profile + " without its distortion map: the scene shows through undistorted");
            mat.enableInstancing = true;
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        /// A parallax-layer material on Castlemist/GW2 in either shader mode (Poiyomi cannot draw it).
        public static Material BuildParallax(MaterialContext ctx, MaterialsFile.MaterialEntry e, string modelFolder, string assetPath)
        {
            return BuildGW2(ctx, ctx.ParallaxShader, e, modelFolder, false, null, assetPath);
        }

        /// One material on Castlemist/GW2 (`shader`).
        static Material BuildGW2(MaterialContext ctx, Shader shader, MaterialsFile.MaterialEntry e, string modelFolder,
                                 bool darkDecal, WorldManifest.UvScroll scroll, string assetPath)
        {
            string tag = Path.GetFileName(modelFolder) + "/" + e.Name;
            int mode = darkDecal ? PoiyomiMaterials.Fade : e.Mode;
            Material mat = PoiyomiMaterials.GetOrCreate(shader, assetPath);
            ResetBlocks(mat);
            ApplyPreset(mat, mode);
            if (!darkDecal && !e.Exact && e.Blend != null) ApplyRawBlend(mat, e.Blend);
            SetF(mat, "_Cull", e.Cull == "Off" ? (float)CullMode.Off : (float)CullMode.Back);
            mat.renderQueue = ctx.QueueFor(mode) + e.RenderQueueOffset;
            SetF(mat, "_AlphaPremultiply", mode == PoiyomiMaterials.Transparent ? 1f : 0f);
            if (mode == PoiyomiMaterials.Cutout || mode == PoiyomiMaterials.TransClipping)
            {
                SetF(mat, "_AlphaTest", 1f);
                SetF(mat, "_Cutoff", (float)e.AlphaCutoff);
            }
            SetF(mat, "_UV1FromUV4", ctx.Uv1InUv4 ? 1f : 0f);
            // GW2's effect blends (Additive .. 2x Multiplicative) and unlit shaders apply no lighting.
            SetF(mat, "_Unlit", e.Unlit || PoiyomiMaterials.ParticleLike(mode, false) ? 1f : 0f);
            SetF(mat, "_AdditiveFog", AddsLight(mode) ? 1f : 0f);
            var unmapped = new List<string>();

            if (IsParallax(e)) Parallax(ctx, mat, tag, modelFolder, e);
            else
            {
                Surface(ctx, mat, tag, modelFolder, e);
                LitCoreRim(ctx, mat, tag, modelFolder, e);
                PerturbedGlow(ctx, mat, tag, modelFolder, e);
                DiffuseScaleTint(mat, e);
                Decal(ctx, mat, tag, modelFolder, e);
                Singletons(ctx, mat, tag, modelFolder, e);
                FxShaping(ctx, mat, tag, modelFolder, e, mode);
                VertexColourFx(ctx, mat, tag, modelFolder, e);
                MultiplyAndCubeFx(ctx, mat, tag, modelFolder, e);
                if (e.Profile == "prop-interior") Interior(ctx, mat, tag, modelFolder, e);
                else if (e.Profile == "prop-burn") Burn(ctx, mat, tag, modelFolder, e);
                else if (e.Profile == "fx-ramp-glow" || e.Profile == "fx-perturb-mod-glow") RampGlow(ctx, mat, tag, modelFolder, e);
                else if (e.Profile == "fx-fire") Fire(ctx, mat, tag, modelFolder, e);
            }

            // Every texTransform animates its UV set (the game's TexTransform<n>); an export before
            // texTransforms has only the manifest's uvScroll of the base colour's set.
            var anim = new Vector4[4];
            foreach (MaterialsFile.TexTransform t in e.TexTransforms)
                if (t.Uv >= 0 && t.Uv < 4)
                    anim[t.Uv] = new Vector4((float)t.Scroll[0], (float)-t.Scroll[1], (float)-t.Rotate, 0f);
            if (e.TexTransforms.Count == 0 && scroll != null && scroll.UvIndex >= 0 && scroll.UvIndex < 4)
                anim[scroll.UvIndex] = new Vector4((float)scroll.U, (float)-scroll.V, 0f, 0f);
            for (int k = 0; k < 4; ++k) SetVec(mat, "_UVAnim" + k, anim[k]);

            // A distortion map that is a perturb layer is read through _PerturbTex; a decal through the
            // interior block's decal mode.
            if (e.Map("distortion") != null && !mat.GetTexture("_PerturbTex")) unmapped.Add("distortion");
            bool decalUsed = On(mat, "_UseInterior") && mat.GetFloat("_InteriorMode") > 1.5f;
            if (e.Map("decal") != null && !decalUsed) unmapped.Add("decal");
            if (e.Map("decalMask") != null && !decalUsed) unmapped.Add("decalMask");
            if (!IsParallax(e) && e.Map("alphaMask") != null) unmapped.Add("alphaMask");
            if (e.HasRim) unmapped.Add("rim");
            if (e.HasProjector) unmapped.Add("projector");
            if (unmapped.Count > 0)
                ctx.Warnings.Add(tag + ": not mapped by the castlemist shaders: " + string.Join(", ", unmapped));

            mat.enableInstancing = true;  // spec D6 step 7
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        /// The lit surface blocks: colour, normal, packed, tints, mod, emission.
        static void Surface(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            Layer(ctx, mat, tag, folder, e.Map("baseColor"), "_MainTex", "_MainTexUV");

            MaterialsFile.MapRef n = e.Map("normal");
            if (n != null)
            {
                Layer(ctx, mat, tag, folder, n, "_BumpMap", "_BumpMapUV");
                SetF(mat, "_BumpScale", 1f);
                SetF(mat, "_UseNormalMap", 1f);
            }
            MaterialsFile.MapRef packed = e.Map("packed");
            if (packed != null)
            {
                Layer(ctx, mat, tag, folder, packed, "_PackedMap", "_PackedMapUV");
                SetF(mat, "_Metallic", 1f);
                SetF(mat, "_Glossiness", 1f);
                SetF(mat, "_UsePacked", 1f);
            }
            SetColor(mat, "_ReflectionTint", e.ReflectionTint != null ? Rgb(e.ReflectionTint) : Color.white);
            SetColor(mat, "_SpecularTint", e.SpecularTint != null ? Rgb(e.SpecularTint) : Color.white);

            MaterialsFile.Extra mod = e.Extras.Find(x => x.Use == "detail-multiply2x");
            if (mod != null)
            {
                Layer(ctx, mat, tag, folder, mod.Map, "_DetailMod", "_DetailModUV");
                SetF(mat, "_DetailModStrength", 1f);
                SetF(mat, "_UseDetailMod", 1f);
            }

            MaterialsFile.MapRef emap = e.Map("emissionMap"), emask = e.Map("emissionMask");
            if (emap == null && emask == null) emap = e.Map("emissionBaked");
            if (e.HasEmission || emap != null || emask != null)
            {
                Color c = e.HasEmission ? Rgb(e.EmissionColor) : Color.white;
                float strength = e.HasEmission ? (float)e.EmissionStrength : 1f;
                if (strength <= 0f) strength = 1f;
                Layer(ctx, mat, tag, folder, emap, "_EmissionMap", "_EmissionMapUV");
                Layer(ctx, mat, tag, folder, emask, "_EmissionMask", "_EmissionMaskUV");
                SetF(mat, "_EmissionMaskChannel", 0f);
                SetColor(mat, "_EmissionColor", c);
                SetF(mat, "_EmissionStrength", strength);
                SetF(mat, "_UseEmission", 1f);
            }
        }

        /// The parallax block (the GW2 formula is in GW2Core.cginc's header): the parallax layer
        /// (BaseColor), the uv-offset "diffuse", the mask (AlphaMask), the "mskptrb" perturb and the
        /// "ramp" (extras), and the GW2 constants.
        static void Parallax(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            SetF(mat, "_UseParallax", 1f);
            Layer(ctx, mat, tag, folder, e.Map("baseColor"), "_ParallaxTex", "_ParallaxUV");
            MaterialsFile.Extra x = e.ExtraOf("diffuse");
            Layer(ctx, mat, tag, folder, x != null ? x.Map : null, "_OffsetTex", "_OffsetUV");
            MaterialsFile.MapRef mask = e.Map("alphaMask");
            Layer(ctx, mat, tag, folder, mask, "_MaskTex", "_MaskUV");
            SetF(mat, "_UseMask", mask != null ? 1f : 0f);
            x = e.ExtraOf("mskptrb");
            Layer(ctx, mat, tag, folder, x != null ? x.Map : null, "_PerturbTex", "_PerturbUV");
            x = e.ExtraOf("ramp");
            Layer(ctx, mat, tag, folder, x != null ? x.Map : null, "_RampTex", null);
            SetF(mat, "_UseRamp", x != null ? 1f : 0f);

            SetF(mat, "_ParaPer", (float)e.Const("paraper", 0, 0.0));
            SetF(mat, "_ParDist", (float)e.Const("pardist", 0, 0.0));
            // 842652 offsets the mask by mskptrb x cutptrb (note section 8.2); 1507908 by mskptrb.
            SetF(mat, "_MskPtrb", (float)(e.Const("mskptrb", 0, 0.0) * e.Const("cutptrb", 0, 1.0)));
            bool step = e.Gw2.ContainsKey("stepmin") || e.Gw2Vectors.ContainsKey("stepmin");
            SetF(mat, "_UseStep", step ? 1f : 0f);
            SetF(mat, "_StepMin", (float)e.Const("stepmin", 0, 0.75));
            SetVec(mat, "_StepCol", new Vector4((float)e.Const("stepcol", 0, 1.0), (float)e.Const("stepcol", 1, 1.0),
                                                (float)e.Const("stepcol", 2, 1.0), (float)e.Const("stepcol", 3, 1.0)));
            // Without the step: x parfade (1730340) or x diffade (842652).
            SetF(mat, "_AlphaScale", (float)e.Const("parfade", 0, e.Const("diffade", 0, 1.0)));
            SetF(mat, "_DepDist", (float)(e.Const("depdist", 0, 16.0) * ctx.UnityScale));
            SetF(mat, "_Unlit", 1f);   // the game's parallax shaders apply no lighting
            SetF(mat, "_AlphaTest", 1f);   // and discard below AlphaRef (1 / 255)
            SetF(mat, "_Cutoff", 1f / 255f);
        }

        /// The interior block (GW2 prop-interior 3036412; its formula is in GW2Core.cginc's header and
        /// shader_profiles.cpp): the "interior-parallax" layer lerped with the base colour by mask.G,
        /// both perturbed by the signed "perturb" map (difptrb, mskptrb), and the rim (rimcp).
        static void Interior(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            MaterialsFile.Extra layer = e.Extras.Find(x => x.Use == "interior-parallax");
            if (layer == null || !Mask(ctx, mat, tag, folder, e))
            {
                ctx.Warnings.Add(tag + ": prop-interior without its interior layer or mask: drawn as a plain surface");
                return;
            }
            SetF(mat, "_UseInterior", 1f);
            Layer(ctx, mat, tag, folder, layer.Map, "_InteriorTex", "_InteriorUV");
            SetF(mat, "_MaskChannel", 1f);   // mask.G
            MaterialsFile.Extra perturb = e.ExtraOf("perturb");
            Layer(ctx, mat, tag, folder, perturb != null ? perturb.Map : null, "_PerturbTex", "_PerturbUV");
            SetF(mat, "_PerturbSigned", 1f);
            SetF(mat, "_BasePtrb", perturb != null ? (float)e.Const("difptrb", 0, 0.0) : 0f);
            SetF(mat, "_MskPtrb", perturb != null ? (float)e.Const("mskptrb", 0, 0.0) : 0f);
            SetF(mat, "_ParDist", (float)e.Const("pardist", 0, 0.0));
            SetF(mat, "_InteriorNormalShift", 0.01833f);
            // 2314655 steps the mask (stepmin, decfade) and discards cutout.G x opacity < 0.5.
            StepMix(mat, e);
            Cutout(ctx, mat, tag, folder, e, 1f);
            Glow(mat, e);
            if (e.Gw2Vectors.ContainsKey("rimcp"))
            {
                SetF(mat, "_UseRim", 1f);
                SetColor(mat, "_RimColor", new Color((float)e.Const("rimcp", 0, 0), (float)e.Const("rimcp", 1, 0), (float)e.Const("rimcp", 2, 0), 1f));
                SetF(mat, "_RimPower", (float)e.Const("rimcp", 3, 3.0));
            }
        }

        /// GW2 prop-burn (3122105; formula in shader_profiles.cpp): the decal layer mixed in by the
        /// stepped mask.G x decal alpha, the mod by modfade, the cutout.R x opacity holes and the
        /// burncol edge glow; the glow x glofade.
        static void Burn(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            MaterialsFile.Extra decal = e.ExtraOf("decal");
            if (decal != null && Mask(ctx, mat, tag, folder, e))
            {
                SetF(mat, "_UseInterior", 1f);
                SetF(mat, "_InteriorMode", 1f);
                Layer(ctx, mat, tag, folder, decal.Map, "_InteriorTex", "_InteriorUV");
                SetF(mat, "_MaskChannel", 1f);   // mask.G
                SetF(mat, "_ParDist", 0f);
                SetF(mat, "_InteriorNormalShift", 0f);
                StepMix(mat, e);
            }
            if (e.Gw2.ContainsKey("modfade")) SetF(mat, "_DetailModStrength", (float)e.Const("modfade", 0, 1.0));
            Cutout(ctx, mat, tag, folder, e, 0f);
            SetF(mat, "_BurnOffset", (float)e.Const("burnoff", 0, 0.0));
            SetColor(mat, "_BurnColor", new Color((float)e.Const("burncol", 0, 0), (float)e.Const("burncol", 1, 0), (float)e.Const("burncol", 2, 0), 1f));
            Glow(mat, e);
        }

        /// The prop-metalmask lit core's rim (3576633, 3121869; ps 61 read 2026-10-22): sun x rimcp.rgb x
        /// (1 - shine) x (1 - metalmask.G) x (1 - N.V)^(rimcp.w x (1 - shine)).
        static void LitCoreRim(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            if (!e.Gw2Vectors.ContainsKey("rimcp") || (e.Amat != 3576633 && e.Amat != 3121869)) return;
            SetF(mat, "_UseRim", 1f);
            SetColor(mat, "_RimColor", new Color((float)e.Const("rimcp", 0, 0), (float)e.Const("rimcp", 1, 0), (float)e.Const("rimcp", 2, 0), 1f));
            SetF(mat, "_RimPower", (float)e.Const("rimcp", 3, 3.0));
            SetF(mat, "_RimShineFade", 1f);
            MaterialsFile.Extra mm = e.ExtraOf("metalmask");
            MaterialsFile.Color32Value c;
            if (mm != null)
            {
                Layer(ctx, mat, tag, folder, mm.Map, "_MaskTex", "_MaskUV");
                SetF(mat, "_MaskChannel", 1f);
                SetF(mat, "_RimUseMask", 1f);
            }
            else if (e.LayerConstants.TryGetValue("metalmask", out c))
            {
                SetF(mat, "_UseMaskConstant", 1f);
                SetVec(mat, "_MaskConstant", new Vector4((float)c.R, (float)c.G, (float)c.B, (float)c.A));
                SetF(mat, "_MaskChannel", 1f);
                SetF(mat, "_RimUseMask", 1f);
            }
        }

        /// The glow of the lit glow shaders (3123985, 3132741, 3124730; ps 62 read 2026-10-22):
        /// glow(UV + (perturb x 2 - 1) x gloptrb) x glofade x 2, added.
        static void PerturbedGlow(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            if (!e.Gw2.ContainsKey("glofade") || e.Profile != "default") return;
            Glow(mat, e);
            MaterialsFile.Extra perturb = e.ExtraOf("perturb");
            if (perturb == null || !e.Gw2.ContainsKey("gloptrb")) return;
            Layer(ctx, mat, tag, folder, perturb.Map, "_PerturbTex", "_PerturbUV");
            SetF(mat, "_PerturbSigned", 1f);
            SetF(mat, "_EmissionPtrb", (float)e.Const("gloptrb", 0, 0.0));
        }

        /// The export's decal map (prop-decal, subsurface-decal: RGB the decal, A its coverage; note
        /// section 8.4 and 4) lerped over the colour, x maps.decalMask when present; subsurface-decal
        /// offsets the decal by the view x pardist (decalParallax).
        static void Decal(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            MaterialsFile.MapRef decal = e.Map("decal");
            if (decal == null || On(mat, "_UseInterior")) return;
            SetF(mat, "_UseInterior", 1f);
            SetF(mat, "_InteriorMode", 2f);
            SetF(mat, "_InteriorStep", 0f);
            Layer(ctx, mat, tag, folder, decal, "_InteriorTex", "_InteriorUV");
            MaterialsFile.MapRef dm = e.Map("decalMask");
            if (dm != null)
            {
                Layer(ctx, mat, tag, folder, dm, "_MaskTex", "_MaskUV");
                SetF(mat, "_MaskChannel", 0f);
            }
            else
            {
                SetF(mat, "_UseMaskConstant", 1f);
                SetVec(mat, "_MaskConstant", Vector4.one);
                SetF(mat, "_MaskChannel", 0f);
            }
            SetF(mat, "_ParDist", e.Profile == "subsurface-decal" ? (float)e.Const("pardist", 0, 0.0) : 0f);
            SetF(mat, "_InteriorNormalShift", 0f);
        }

        static bool On(Material m, string p) { return m.HasProperty(p) && m.GetFloat(p) > 0.5f; }

        /// The Mists vault's one-material shaders (read 2026-10-22):
        ///   3100353 (ps 82): lit; the diffuse and the normal at their UV + (perturb x 2 - 1) x ptrb
        ///     (x perturbmask, a constant 1 here); discard 2 x diffuse.a < 0.5 (a < 0.25).
        ///   1676989 (ps 30): One / InvSrcAlpha; ramp(|N.V|) x diffuse, stepped by step and color.w
        ///     (color.w 0 here: transparent at rest, an animated reveal), x diffade, rgb x color x 2.
        ///   80417 (ps 11): One / InvSrcColor; screen(diffuse, add) x ramp(N.V) rgba x fadeadd x diffade.
        static void Singletons(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            if (e.Amat == 3100353)
            {
                MaterialsFile.Extra perturb = e.ExtraOf("perturb");
                if (perturb != null)
                {
                    Layer(ctx, mat, tag, folder, perturb.Map, "_PerturbTex", "_PerturbUV");
                    SetF(mat, "_PerturbSigned", 1f);
                    MaterialsFile.Color32Value pm;
                    double k = e.LayerConstants.TryGetValue("perturbmask", out pm) ? pm.R : 1.0;
                    SetF(mat, "_BasePtrb", (float)(e.Const("ptrb", 0, 0.0) * k));
                    SetF(mat, "_PerturbNormal", 1f);
                }
                SetF(mat, "_AlphaTest", 1f);
                SetF(mat, "_Cutoff", 0.25f);
            }
            else if (e.Amat == 1676989)
            {
                SetF(mat, "_Unlit", 1f);
                SetF(mat, "_UseEmission", 0f);
                SetF(mat, "_UsePacked", 0f);
                MaterialsFile.Extra ramp = e.ExtraOf("ramp");
                if (ramp != null)
                {
                    Layer(ctx, mat, tag, folder, ramp.Map, "_RampTex", null);
                    SetF(mat, "_UseRamp", 1f);
                    SetF(mat, "_RampRGBA", 1f);
                }
                MaterialsFile.Extra perturb = e.ExtraOf("perturb");
                if (perturb != null)
                {
                    // Bound as GW2 has it; difptrb is 0 on the Mists vault's, so it does not move yet.
                    Layer(ctx, mat, tag, folder, perturb.Map, "_PerturbTex", "_PerturbUV");
                    SetF(mat, "_PerturbSigned", 1f);
                    SetF(mat, "_BasePtrb", (float)e.Const("difptrb", 0, 0.0));
                }
                SetF(mat, "_UseStep", 1f);
                SetF(mat, "_StepMin", (float)e.Const("step", 0, 0.9));
                SetVec(mat, "_StepCol", new Vector4((float)e.Const("diffade", 0, 1.0), 1f, 1f, (float)e.Const("color", 3, 0.0)));
                SetColor(mat, "_Color", new Color((float)(2 * e.Const("color", 0, 0.5)), (float)(2 * e.Const("color", 1, 0.5)), (float)(2 * e.Const("color", 2, 0.5)), 1f));
                SetF(mat, "_AlphaPremultiply", 1f);
            }
            else if (e.Amat == 3121604)
            {
                // The water (see GW2Core.cginc): unlit, its caustics are the diffuse, its flow the "flow" layer.
                SetF(mat, "_UseWater", 1f);
                SetF(mat, "_Unlit", 1f);
                SetF(mat, "_UsePacked", 0f);
                SetF(mat, "_UseEmission", 0f);
                MaterialsFile.Extra flow = e.ExtraOf("flow");
                Layer(ctx, mat, tag, folder, flow != null ? flow.Map : null, "_FlowTex", null);
                MaterialsFile.Extra proj = e.CubeOf("projection"), refl = e.CubeOf("reflect");
                Layer(ctx, mat, tag, folder, proj != null ? proj.Map : null, "_ProjCube", null);
                Layer(ctx, mat, tag, folder, refl != null ? refl.Map : null, "_ReflCube", null);
                SetF(mat, "_UseWaterCubes", proj != null && refl != null ? 1f : 0f);
                SetColor(mat, "_WaterEdge", new Color((float)e.Const("edgcolr", 0, 0), (float)e.Const("edgcolr", 1, 1), (float)e.Const("edgcolr", 2, 1), 1f));
                SetVec(mat, "_WaterFalloff", new Vector4((float)e.Const("falloff", 0, 1), (float)e.Const("falloff", 1, 0.2), (float)e.Const("falloff", 2, 0), (float)e.Const("falloff", 3, 1)));
                SetF(mat, "_CausScale", (float)e.Const("causscl", 0, 0.0025));
                SetF(mat, "_FlowScale", (float)e.Const("flowscl", 0, 0.0075));
                SetF(mat, "_FlowStrength", (float)e.Const("flowstr", 0, 0.15));
                SetVec(mat, "_ScrollSpeed", new Vector4((float)e.Const("scrspd", 0, 0), (float)e.Const("scrspd", 1, 0), (float)e.Const("scrspd", 2, 0), (float)e.Const("scrspd", 3, 0)));
                SetColor(mat, "_WaveColor", new Color((float)e.Const("waveclr", 0, 1), (float)e.Const("waveclr", 1, 1), (float)e.Const("waveclr", 2, 1), 1f));
                SetF(mat, "_UnityScale", ctx.UnityScale);
            }
            else if (e.Amat == 80417)
            {
                SetF(mat, "_Unlit", 1f);
                SetF(mat, "_UseEmission", 0f);
                SetF(mat, "_UsePacked", 0f);
                SetF(mat, "_AlphaPremultiply", 0f);
                SetF(mat, "_PremultipliedBase", 0f);
                MaterialsFile.Extra add = e.ExtraOf("add"), ramp = e.ExtraOf("ramp");
                if (add != null)
                {
                    Layer(ctx, mat, tag, folder, add.Map, "_ScreenTex", "_ScreenUV");
                    SetF(mat, "_UseScreenLayer", 1f);
                }
                if (ramp != null)
                {
                    Layer(ctx, mat, tag, folder, ramp.Map, "_RampTex", null);
                    SetF(mat, "_UseRamp", 1f);
                    SetF(mat, "_RampRGBA", 1f);
                }
                float k = (float)(e.Const("fadeadd", 0, 1.0) * e.Const("diffade", 0, 1.0));
                SetColor(mat, "_Color", new Color(k, k, k, k));
                SetF(mat, "_AlphaScale", 1f);
            }
        }

        /// difscal / difcol (2600237, ps 43 read 2026-10-22): the diffuse at UV0 x difscal, x difcol x 2.
        static void DiffuseScaleTint(Material mat, MaterialsFile.MaterialEntry e)
        {
            if (e.Gw2.ContainsKey("difscal"))
            {
                float k = (float)e.Const("difscal", 0, 1.0);
                if (k != 0f) mat.SetTextureScale("_MainTex", new Vector2(k, k));
            }
            if (e.Gw2Vectors.ContainsKey("difcol"))
                SetColor(mat, "_Color", new Color((float)(2 * e.Const("difcol", 0, 0.5)), (float)(2 * e.Const("difcol", 1, 0.5)),
                                                  (float)(2 * e.Const("difcol", 2, 0.5)), 1f));
        }

        /// GW2 effect shaping read from the shaders (2026-10-22): the view-angle "ramp" and diffade on
        /// the alpha (1478777, 882285, 23507, 569376), modxcol x 2 on the colour (569376), and for
        /// 882285 the signed perturb (difptrb on the colour, masptrb on the mask.R). The export's
        /// fx-soft-additive colour is premultiplied by its alpha (_PremultipliedBase).
        static void FxShaping(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e, int mode)
        {
            bool fx = e.Unlit || PoiyomiMaterials.ParticleLike(mode, false) || e.Profile == "fx-alpha";
            if (!fx || e.Profile == "fx-ramp-glow" || e.Profile == "fx-perturb-mod-glow") return;
            SetF(mat, "_Unlit", 1f);
            SetF(mat, "_UseEmission", 0f);   // the colour is the effect
            SetF(mat, "_UsePacked", 0f);
            MaterialsFile.Extra ramp = e.ExtraOf("ramp");
            if (ramp != null)
            {
                Layer(ctx, mat, tag, folder, ramp.Map, "_RampTex", null);
                SetF(mat, "_UseRamp", 1f);
            }
            if (e.Gw2.ContainsKey("diffade")) SetF(mat, "_AlphaScale", (float)e.Const("diffade", 0, 1.0));
            if (e.Gw2Vectors.ContainsKey("modxcol"))
                SetColor(mat, "_Color", new Color((float)(2 * e.Const("modxcol", 0, 0.5)), (float)(2 * e.Const("modxcol", 1, 0.5)), (float)(2 * e.Const("modxcol", 2, 0.5)), 1f));
            if (e.Profile == "fx-soft-additive")
            {
                SetF(mat, "_PremultipliedBase", 1f);
                SetF(mat, "_AlphaPremultiply", 0f);
                MaterialsFile.Extra perturb = e.ExtraOf("perturb"), mask = e.ExtraOf("mask");
                if (perturb != null && e.Gw2.ContainsKey("difptrb"))
                {
                    Layer(ctx, mat, tag, folder, perturb.Map, "_PerturbTex", "_PerturbUV");
                    SetF(mat, "_PerturbSigned", 1f);
                    SetF(mat, "_BasePtrb", (float)e.Const("difptrb", 0, 0.0));
                    SetF(mat, "_MskPtrb", (float)e.Const("masptrb", 0, 0.0));
                }
                if (mask != null)
                {
                    Layer(ctx, mat, tag, folder, mask.Map, "_MaskTex", "_MaskUV");
                    SetF(mat, "_UseMask", 1f);
                    SetF(mat, "_MaskChannel", 0f);   // mask.R
                }
            }
            if (e.Profile == "fx-perturb-glow")
            {
                // 1946706 (ps 31): colour = diffuse(UV0 + (perturb x 2 - 1) x perturb) x color.
                MaterialsFile.Extra perturb = e.ExtraOf("perturb");
                if (perturb != null)
                {
                    Layer(ctx, mat, tag, folder, perturb.Map, "_PerturbTex", "_PerturbUV");
                    SetF(mat, "_PerturbSigned", 1f);
                    SetF(mat, "_BasePtrb", (float)e.Const("perturb", 0, 0.0));
                }
                if (e.Gw2Vectors.ContainsKey("color"))
                    SetColor(mat, "_Color", new Color((float)e.Const("color", 0, 1.0), (float)e.Const("color", 1, 1.0), (float)e.Const("color", 2, 1.0), 1f));
            }
            if (e.Profile == "fx-alpha")
            {
                SetF(mat, "_AlphaTest", 1f);
                SetF(mat, "_Cutoff", 1f / 255f);
            }
        }

        /// GW2 effects that read the vertex colour (read 2026-10-22): 709501 / 2434562 multiply the
        /// colour by it; fx-flow (2591650, 2759298) takes its parameters from it (GW2Core.cginc's
        /// flow block). Their soft depth fade (depfade / dfadevl, GW2 units) sets _DepDist.
        static void VertexColourFx(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            double fade = e.Const("depfade", 0, e.Const("dfadevl", 0, 0));
            if (fade > 0) SetF(mat, "_DepDist", (float)(fade * ctx.UnityScale));
            if (e.Amat == 709501 || e.Amat == 2434562) SetF(mat, "_UseVertexColor", 1f);
            if (e.Profile != "fx-flow") return;
            MaterialsFile.Extra flow = e.ExtraOf("flow");
            if (flow == null) return;
            Layer(ctx, mat, tag, folder, flow.Map, "_FlowTex", null);
            SetF(mat, "_UseFlow", 1f);
            SetVec(mat, "_Flow", new Vector4((float)e.Const("flow", 0, 1), (float)e.Const("flow", 1, 1), (float)e.Const("flow", 2, 1), (float)e.Const("flow", 3, 0)));
            SetColor(mat, "_FlowStart", new Color((float)e.Const("startcl", 0, 1), (float)e.Const("startcl", 1, 1), (float)e.Const("startcl", 2, 1), 1f));
            SetColor(mat, "_FlowEnd", new Color((float)e.Const("endcl", 0, 1), (float)e.Const("endcl", 1, 1), (float)e.Const("endcl", 2, 1), 1f));
            SetVec(mat, "_FlowStep", new Vector4((float)e.Const("stepcol", 0, 0), (float)e.Const("stepcol", 1, 1), 0, 0));
            SetF(mat, "_FlowPremul", e.Amat == 2759298 ? 1f : 0f);
            SetF(mat, "_AlphaScale", 1f);
        }

        /// GW2's multiply decals and the view cubemap (read 2026-10-22; GW2Core.cginc's envcube,
        /// bumpshade and fogneutral blocks). fx-multiply (15206): the colour, fog to white.
        /// fx-multiply-bump (157432, 3718974): the bump shading of the normal map (the "diffuse"
        /// extra, use normal-map), fog to 0.5; 3718974 x its colour map (the BaseColor).
        /// fx-cubemap (221571): the cubemap along the view, perturbed by castlemist's diffuse.
        static void MultiplyAndCubeFx(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            if (e.Profile == "fx-multiply")
            {
                SetF(mat, "_Unlit", 1f);
                SetF(mat, "_AlphaPremultiply", 0f);
                SetF(mat, "_UseFogNeutral", 1f);
                SetF(mat, "_FogNeutral", 1f);
            }
            else if (e.Profile == "fx-multiply-bump")
            {
                SetF(mat, "_Unlit", 1f);
                SetF(mat, "_UseBumpShade", 1f);
                SetF(mat, "_UseFogNeutral", 1f);
                SetF(mat, "_FogNeutral", 0.5f);
                MaterialsFile.Extra n = e.ExtraOf("diffuse");
                Layer(ctx, mat, tag, folder, n != null ? n.Map : null, "_BumpMap", "_BumpMapUV");
                SetF(mat, "_UseNormalMap", 0f);   // read raw by the bumpshade block
                MaterialsFile.MapRef colour = e.Map("baseColor");
                if (e.Amat == 3718974 && colour != null)
                {
                    Layer(ctx, mat, tag, folder, colour, "_BumpShadeTex", "_BumpShadeUV");
                    SetF(mat, "_UseBumpShadeTex", 1f);
                    SetF(mat, "_BumpShadeOpacity", (float)e.Const("opacity", 0, 1.0));
                }
            }
            else if (e.Profile == "fx-cubemap")
            {
                SetF(mat, "_Unlit", 1f);
                MaterialsFile.Extra cube = e.Cubemaps.Count > 0 ? e.Cubemaps[0] : null;
                Layer(ctx, mat, tag, folder, cube != null ? cube.Map : null, "_EnvCube", null);
                SetF(mat, "_UseEnvCube", cube != null ? 1f : 0f);
                MaterialsFile.MapRef p = e.Map("baseColor");
                if (p != null) Layer(ctx, mat, tag, folder, p, "_PerturbTex", "_PerturbUV");
            }
        }

        /// GW2 fx-fire (740364, 965703, 977200, 1171331; port_fire/SPEC.md, formulas in GW2Core.cginc's
        /// fire block): unlit, premultiplied (One / InvSrcAlpha), the colour and alpha computed by the
        /// shader. The shader is told apart by its uniforms (an AMAT with the same code gets the same
        /// mode), else by the AMAT. GW2 constants go in raw (SetVec: no sRGB conversion).
        static void Fire(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            int mode = e.Gw2Vectors.ContainsKey("firesfl") || e.Gw2Vectors.ContainsKey("flowbuv") ? 0
                     : e.Gw2Vectors.ContainsKey("rampstp") ? 1
                     : e.Gw2Vectors.ContainsKey("dyedot") ? 3
                     : e.Gw2Vectors.ContainsKey("scaleb") ? 2
                     : e.Amat == 965703 ? 1 : e.Amat == 977200 ? 2 : e.Amat == 1171331 ? 3 : 0;
            SetF(mat, "_UseFire", 1f);
            SetF(mat, "_FireMode", mode);
            SetF(mat, "_Unlit", 1f);
            SetF(mat, "_AlphaPremultiply", 0f);   // the shader's rgb is already x its coverage
            SetF(mat, "_PremultipliedBase", 0f);
            SetF(mat, "_AdditiveFog", 1f);        // premultiplied: fog to black = GW2's x (1 - fog)
            SetF(mat, "_UseEmission", 0f);
            SetF(mat, "_UsePacked", 0f);
            SetF(mat, "_UseRamp", 0f);            // the ramp is read by the fire block, not the alpha shaping
            SetF(mat, "_UseMask", 0f);
            SetF(mat, "_UseVertexColor", 0f);     // modes 2 / 3 apply the vertex colour themselves
            SetF(mat, "_AlphaTest", 0f);

            // The shape / noise: the export's raw "diffuse" extra (linear); older exports: BaseColor
            // (imported sRGB, so slightly darker than GW2). 1171331's noise is the "fire" layer.
            MaterialsFile.Extra diffuse = e.ExtraOf("diffuse");
            MaterialsFile.MapRef shape = diffuse != null ? diffuse.Map : e.Map("baseColor");
            if (mode == 3)
            {
                MaterialsFile.Extra fire = e.ExtraOf("fire");
                shape = fire != null ? fire.Map : null;
            }
            Layer(ctx, mat, tag, folder, shape, "_MainTex", null);   // UVs are fixed by the shader
            MaterialsFile.Extra flow = e.ExtraOf("flow");
            Layer(ctx, mat, tag, folder, flow != null ? flow.Map : null, "_FlowTex", null);
            var missing = new List<string>();
            if (shape == null) missing.Add(mode == 3 ? "fire" : "diffuse");
            if (flow == null) missing.Add("flow");

            Vector4 V(string n, Vector4 fb)
            {
                return new Vector4((float)e.Const(n, 0, fb.x), (float)e.Const(n, 1, fb.y), (float)e.Const(n, 2, fb.z), (float)e.Const(n, 3, fb.w));
            }
            float X(string n, double fb) { return (float)e.Const(n, 0, fb); }
            SetVec(mat, "_FireColorL", V("colorl", Vector4.one));
            SetVec(mat, "_FireColorH", V("colorh", Vector4.one));

            if (mode <= 1)
            {
                MaterialsFile.Extra detail = e.ExtraOf("firedetail"), ramp = e.ExtraOf("ramp");
                Layer(ctx, mat, tag, folder, detail != null ? detail.Map : null, "_FireDetail", null);
                Layer(ctx, mat, tag, folder, ramp != null ? ramp.Map : null, "_RampTex", null);
                if (detail == null) missing.Add("firedetail");
                if (ramp == null) missing.Add("ramp");
                var still = new Vector4(0, 0, 1, 1);   // no scroll, scale 1
                SetVec(mat, "_FireUV0", V("fireduv", still));
                SetVec(mat, "_FireUV1", V("flowuv", still));
                if (mode == 0)
                {
                    MaterialsFile.Extra flowb = e.ExtraOf("flowb");
                    Layer(ctx, mat, tag, folder, flowb != null ? flowb.Map : null, "_FireFlowB", null);
                    if (flowb == null) missing.Add("flowb");
                    SetVec(mat, "_FireUV2", V("flowbuv", still));
                    float opacity = X("opacity", 1.0);
                    SetVec(mat, "_FireParams", new Vector4(X("flowspd", 0.0), X("firedfl", 0.0), X("firesfl", 0.0), opacity));
                    SetVec(mat, "_FireParams2", new Vector4((float)e.Const("ramprmp", 0, 1.0), (float)e.Const("ramprmp", 1, 1.0), 0f, 0f));
                    if (opacity <= 0f) ctx.Warnings.Add(tag + ": fx-fire opacity is 0: invisible as stored (the game may animate it)");
                }
                else
                {
                    float fadedif = X("fadedif", 1.0);
                    SetVec(mat, "_FireParams", new Vector4((float)e.Const("rampflw", 0, 0.0), (float)e.Const("rampflw", 1, 0.0), X("detailf", 1.0), fadedif));
                    SetVec(mat, "_FireParams2", V("rampstp", new Vector4(0, 1, 0, 1)));
                    SetVec(mat, "_FireParams3", new Vector4((float)e.Const("stepcol", 0, 0.0), (float)e.Const("stepcol", 1, 1.0), X("premult", 1.0), 0f));
                    if (fadedif <= 0f) ctx.Warnings.Add(tag + ": fx-fire fadedif is 0: invisible as stored (the game may animate it)");
                }
            }
            else
            {
                MaterialsFile.Extra mask = e.ExtraOf("mask");
                Layer(ctx, mat, tag, folder, mask != null ? mask.Map : null, "_MaskTex", null);
                if (mask == null) missing.Add("mask");
                SetVec(mat, "_FireParams", new Vector4(X("perturb", 0.0), X("scalep", 1.0), X("speedp", 0.0), X("speed", 0.0)));
                SetVec(mat, "_FireParams2", new Vector4(X("scale", 1.0), X("scaleb", 1.0), 0f, 0f));
                // 1171331's dye palette is a runtime composite (fileId 0): white stands in (UNPROVEN).
                SetVec(mat, "_FireDye", Vector4.one);
                ctx.Warnings.Add(tag + ": fx-fire " + e.Amat + " is a GW2 particle shader: it needs the particle's UV1-3 and colour streams");
            }
            if (missing.Count > 0)
                ctx.Warnings.Add(tag + ": fx-fire without its " + string.Join(", ", missing) + " layer(s): the flame is incomplete");
        }

        /// The "mask" layer: its map, or its placeholder constant (layerConstants). False when neither.
        static bool Mask(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            MaterialsFile.Extra mask = e.ExtraOf("mask");
            if (mask != null)
            {
                Layer(ctx, mat, tag, folder, mask.Map, "_MaskTex", "_MaskUV");
                return true;
            }
            MaterialsFile.Color32Value c;
            if (!e.LayerConstants.TryGetValue("mask", out c)) return false;
            SetF(mat, "_UseMaskConstant", 1f);
            SetVec(mat, "_MaskConstant", new Vector4((float)c.R, (float)c.G, (float)c.B, (float)c.A));
            return true;
        }

        /// GW2's stepped mix (stepmin, decfade) when the material has both constants.
        static void StepMix(Material mat, MaterialsFile.MaterialEntry e)
        {
            bool step = e.Gw2.ContainsKey("stepmin") && e.Gw2.ContainsKey("decfade");
            SetF(mat, "_InteriorStep", step ? 1f : 0f);
            if (!step) return;
            SetF(mat, "_StepMin", (float)e.Const("stepmin", 0, 0.75));
            SetF(mat, "_InteriorStepOffset", (float)e.Const("decfade", 0, 1.0));
        }

        /// The "cutout" layer's holes (channel x opacity < 0.5) when the material has both.
        static void Cutout(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e, float channel)
        {
            MaterialsFile.Extra cut = e.ExtraOf("cutout");
            if (cut == null || !e.Gw2.ContainsKey("opacity")) return;
            SetF(mat, "_UseCutout", 1f);
            Layer(ctx, mat, tag, folder, cut.Map, "_CutoutTex", "_CutoutUV");
            SetF(mat, "_CutoutChannel", channel);
            SetF(mat, "_CutoutScale", (float)e.Const("opacity", 0, 1.0));
        }

        /// The "glow" layer as emission x glofade x 2 (GW2's add of glow x glofade, x 2).
        static void Glow(Material mat, MaterialsFile.MaterialEntry e)
        {
            if (!e.Gw2.ContainsKey("glofade") || !mat.GetTexture("_EmissionMap")) return;
            SetF(mat, "_UseEmission", 1f);
            SetF(mat, "_EmissionStrength", (float)(2.0 * e.Const("glofade", 0, 0.0)));
            SetColor(mat, "_EmissionColor", Color.white);
        }

        /// The ramp glows (GW2 fx-ramp-glow 1998356, fx-perturb-mod-glow 574526; formulas in
        /// shader_profiles.cpp): unlit, One / InvSrcColor with the colour premultiplied by the alpha,
        /// alpha x ramp(|N.V|) x diffade; 1998356 tints by `color`, 574526 perturbs the alpha (difptrb)
        /// and the mod (modptrb x mask.R) and multiplies the alpha by mask.G.
        static void RampGlow(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MaterialEntry e)
        {
            SetF(mat, "_Unlit", 1f);
            SetF(mat, "_AlphaPremultiply", 1f);
            SetF(mat, "_UseEmission", 0f);   // the colour is the glow: no extra emission
            SetF(mat, "_UsePacked", 0f);
            SetF(mat, "_AlphaScale", (float)e.Const("diffade", 0, 1.0));
            MaterialsFile.Extra ramp = e.ExtraOf("ramp");
            Layer(ctx, mat, tag, folder, ramp != null ? ramp.Map : null, "_RampTex", null);
            SetF(mat, "_UseRamp", ramp != null ? 1f : 0f);
            if (ramp == null) ctx.Warnings.Add(tag + ": " + e.Profile + " without its ramp layer: no view-angle fade");
            SetF(mat, "_DepDist", (float)(e.Const("depdist", 0, 16.0) * ctx.UnityScale));
            if (e.Profile == "fx-ramp-glow")
            {
                SetColor(mat, "_Color", new Color((float)e.Const("color", 0, 1.0), (float)e.Const("color", 1, 1.0),
                                                  (float)e.Const("color", 2, 1.0), 1f));
                return;
            }
            MaterialsFile.Extra perturb = e.ExtraOf("perturb"), mask = e.ExtraOf("mask");
            Layer(ctx, mat, tag, folder, perturb != null ? perturb.Map : null, "_PerturbTex", "_PerturbUV");
            SetF(mat, "_PerturbSigned", 1f);
            SetF(mat, "_PerturbAlphaOnly", 1f);
            SetF(mat, "_BasePtrb", perturb != null ? (float)e.Const("difptrb", 0, 0.0) : 0f);
            SetF(mat, "_ModPtrb", perturb != null && mask != null ? (float)e.Const("modptrb", 0, 0.0) : 0f);
            if (mask != null)
            {
                Layer(ctx, mat, tag, folder, mask.Map, "_MaskTex", "_MaskUV");
                SetF(mat, "_UseMask", 1f);
                SetF(mat, "_MaskChannel", 1f);   // mask.G multiplies the alpha; mask.R scales the mod perturb
            }
        }

        /// Every block of Castlemist/GW2 off, its textures cleared, its scales neutral.
        static void ResetBlocks(Material mat)
        {
            foreach (string f in new[] { "_AlphaTest", "_UseDetailMod", "_UseNormalMap", "_UsePacked", "_UseEmission",
                                         "_UseParallax", "_UseMask", "_UseRamp", "_UseStep", "_SoftFade", "_Unlit",
                                         "_AlphaPremultiply", "_AdditiveFog", "_MskPtrb", "_ParaPer", "_ParDist", "_Metallic",
                                         "_UseInterior", "_UseRim", "_BasePtrb", "_PerturbSigned", "_MaskChannel",
                                         "_PerturbAlphaOnly", "_ModPtrb", "_InteriorStep", "_InteriorMode", "_UseCutout",
                                         "_BurnOffset", "_CutoutChannel", "_UseMaskConstant", "_RimShineFade", "_RimUseMask",
                                         "_PremultipliedBase", "_EmissionPtrb", "_UseScreenLayer", "_RampRGBA", "_PerturbNormal",
                                         "_UseWater", "_UseWaterCubes", "_UseVertexColor", "_UseFlow", "_FlowPremul",
                                         "_UseEnvCube", "_UseBumpShade", "_UseBumpShadeTex", "_UseFogNeutral",
                                         "_UseFire", "_FireMode" })
                SetF(mat, f, 0f);
            foreach (string t in new[] { "_MainTex", "_DetailMod", "_BumpMap", "_PackedMap", "_EmissionMap", "_EmissionMask",
                                         "_ParallaxTex", "_OffsetTex", "_MaskTex", "_PerturbTex", "_RampTex", "_InteriorTex",
                                         "_CutoutTex", "_ScreenTex", "_FlowTex", "_ProjCube", "_ReflCube", "_EnvCube",
                                         "_BumpShadeTex", "_FireDetail", "_FireFlowB" })
                if (mat.HasProperty(t)) mat.SetTexture(t, null);
            SetColor(mat, "_BurnColor", Color.black);
            SetF(mat, "_CutoutScale", 1f);
            SetF(mat, "_InteriorStepOffset", 1f);
            SetF(mat, "_AlphaScale", 1f);
            SetF(mat, "_Glossiness", 0.5f);
            if (mat.HasProperty("_MainTex")) { mat.SetTextureScale("_MainTex", Vector2.one); mat.SetTextureOffset("_MainTex", Vector2.zero); }
            SetColor(mat, "_Color", Color.white);
            SetColor(mat, "_ReflectionTint", Color.white);
            SetColor(mat, "_SpecularTint", Color.white);
            mat.shaderKeywords = new string[0];
        }

        /// A material on Castlemist/GW2 Particle, for GW2 particle systems (not placed by the world
        /// import yet: its mesh effects are Castlemist/GW2, unlit).
        public static Material BuildParticleMaterial(MaterialContext ctx, MaterialsFile.MaterialEntry e, string modelFolder,
                                      bool darkDecal, WorldManifest.UvScroll scroll, string assetPath)
        {
            string tag = Path.GetFileName(modelFolder) + "/" + e.Name;
            int mode = darkDecal ? PoiyomiMaterials.Fade : e.Mode;
            Material mat = PoiyomiMaterials.GetOrCreate(ctx.ParticleShader, assetPath);
            ApplyPreset(mat, mode);
            if (!darkDecal && !e.Exact && e.Blend != null) ApplyRawBlend(mat, e.Blend);
            SetF(mat, "_Cull", e.Cull == "Off" ? (float)CullMode.Off : (float)CullMode.Back);
            mat.renderQueue = ctx.QueueFor(mode) + e.RenderQueueOffset;
            MaterialsFile.MapRef bc = e.Map("baseColor");
            if (bc != null) PoiyomiMaterials.SetTex(ctx, mat, "_MainTex", modelFolder, bc, tag);
            if (bc != null && bc.Uv != 0)
                ctx.Warnings.Add(tag + ": baseColor UV" + bc.Uv + " not mapped (the particle shader reads UV0)");
            SetColor(mat, "_TintColor", Color.white);
            SetF(mat, "_Intensity", 1f);
            SetF(mat, "_AlphaPremultiply", mode == PoiyomiMaterials.Transparent ? 1f : 0f);
            SetF(mat, "_AdditiveFog", AddsLight(mode) ? 1f : 0f);
            Vector4 sv = Vector4.zero;
            foreach (MaterialsFile.TexTransform t in e.TexTransforms)
                if (bc != null && t.Uv == bc.Uv) sv = new Vector4((float)t.Scroll[0], (float)-t.Scroll[1], 0f, 0f);
            if (e.TexTransforms.Count == 0 && scroll != null && bc != null && bc.Uv == scroll.UvIndex)
                sv = new Vector4((float)scroll.U, (float)-scroll.V, 0f, 0f);
            SetVec(mat, "_UVScroll", sv);
            mat.enableInstancing = true;
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        static void Layer(MaterialContext ctx, Material mat, string tag, string folder, MaterialsFile.MapRef m, string tex, string uvProp)
        {
            if (m == null) { if (mat.HasProperty(tex)) mat.SetTexture(tex, null); return; }
            PoiyomiMaterials.SetTex(ctx, mat, tex, folder, m, tag);
            if (uvProp != null) SetF(mat, uvProp, Mathf.Clamp(m.Uv, 0, 3));
        }

        /// Terrain (Opaque) or water (Fade) material with one main texture, on Castlemist/GW2.
        public static Material BuildSimple(MaterialContext ctx, string assetPath, Texture tex, int mode)
        {
            Material mat = PoiyomiMaterials.GetOrCreate(ctx.Shader, assetPath);
            ResetBlocks(mat);
            ApplyPreset(mat, mode);
            mat.renderQueue = ctx.QueueFor(mode);
            if (mat.HasProperty("_MainTex")) mat.SetTexture("_MainTex", tex);
            SetF(mat, "_MainTexUV", 0f);
            for (int k = 0; k < 4; ++k) SetVec(mat, "_UVAnim" + k, Vector4.zero);
            SetF(mat, "_Cull", (float)CullMode.Back);
            EditorUtility.SetDirty(mat);
            ctx.Built++;
            return mat;
        }

        /// The destination adds light (dst One): Additive and Soft Additive.
        static bool AddsLight(int mode) { return mode == 4 || mode == 5; }

        /// The render mode `mode` (Poiyomi's _Mode numbering) on a Castlemist/GW2 or GW2 Particle
        /// material: the preset's blend, ZWrite, RenderType tag, queue, the shadow pass, and the
        /// mode's own switches (_AlphaTest, _AlphaPremultiply, _AdditiveFog). The importer and GW2ShaderGUI.
        public static void ApplyRenderMode(Material mat, int mode)
        {
            ApplyPreset(mat, mode);
            mat.renderQueue = PoiyomiMaterials.FallbackQueue(mode);
            bool clip = mode == PoiyomiMaterials.Cutout || mode == PoiyomiMaterials.TransClipping;
            SetF(mat, "_AlphaTest", clip ? 1f : 0f);
            SetF(mat, "_AlphaPremultiply", mode == PoiyomiMaterials.Transparent ? 1f : 0f);
            SetF(mat, "_AdditiveFog", AddsLight(mode) ? 1f : 0f);
        }

        static void ApplyPreset(Material mat, int mode)
        {
            PoiyomiMaterials.Preset p = PoiyomiMaterials.PresetFor(mode);
            mat.SetOverrideTag("RenderType", p.RenderType);
            SetF(mat, "_Mode", mode);
            SetF(mat, "_SrcBlend", p.Src);
            SetF(mat, "_DstBlend", p.Dst);
            SetF(mat, "_BlendOp", (float)BlendOp.Add);
            SetF(mat, "_SrcBlendAlpha", p.Src);
            SetF(mat, "_DstBlendAlpha", p.Dst);
            SetF(mat, "_BlendOpAlpha", (float)BlendOp.Add);
            SetF(mat, "_ZWrite", p.ZWrite);
            SetF(mat, "_ZTest", (float)CompareFunction.LessEqual);
            // The shadow pass clips only with _AlphaTest: a blended material casts none.
            mat.SetShaderPassEnabled("ShadowCaster", p.ZWrite == 1);
        }

        static void ApplyRawBlend(Material mat, MaterialsFile.BlendRaw b)
        {
            int src = PoiyomiMaterials.UnityFactor(b.SrcRgb), dst = PoiyomiMaterials.UnityFactor(b.DstRgb);
            int srcA = PoiyomiMaterials.UnityFactor(b.SrcA), dstA = PoiyomiMaterials.UnityFactor(b.DstA);
            if (src >= 0) SetF(mat, "_SrcBlend", src);
            if (dst >= 0) SetF(mat, "_DstBlend", dst);
            if (srcA >= 0) SetF(mat, "_SrcBlendAlpha", srcA);
            if (dstA >= 0) SetF(mat, "_DstBlendAlpha", dstA);
            SetF(mat, "_BlendOp", PoiyomiMaterials.UnityOp(b.EqRgb));
            SetF(mat, "_BlendOpAlpha", PoiyomiMaterials.UnityOp(b.EqA));
        }

        static void SetF(Material m, string p, float v) { if (m.HasProperty(p)) m.SetFloat(p, v); }
        static void SetColor(Material m, string p, Color c) { if (m.HasProperty(p)) m.SetColor(p, c); }
        static void SetVec(Material m, string p, Vector4 v) { if (m.HasProperty(p)) m.SetVector(p, v); }

        /// materials.json colours are sRGB / 255, as Unity material colours are authored.
        static Color Rgb(double[] c) { return new Color((float)c[0], (float)c[1], (float)c[2], 1f); }
    }
}
