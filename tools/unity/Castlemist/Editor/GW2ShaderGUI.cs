// castlemist: the material inspectors of Castlemist/GW2 and Castlemist/GW2 Particle
// (tools/unity/Castlemist/Shaders; the shaders name them with CustomEditor).
//
// GW2: a Render Mode popup (Poiyomi's _Mode numbering, applied with
// CastlemistMaterials.ApplyRenderMode, the importer's own preset table), a Lit / Unlit switch,
// then foldout sections that follow the shader's blocks: Colour, Normal, Packed, Detail mod,
// Emission, Parallax layer, Alpha shaping, UV sets and animation, VRC Light Volumes, Advanced.
// Blocks are material floats (no keywords): assigning a normal, packed, detail, emission or
// parallax map turns its block on, clearing it turns it off. "Custom" keeps the blend factors
// as they are and shows them for editing.
// GW2 Particle: the GW2 particle shader family, a Blend popup (Additive, Soft Additive, Alpha,
// Premultiplied, Multiply, Custom), then the family's own maps and constants, soft fade, render state.

using System;
using UnityEditor;
using UnityEngine;
using UnityEngine.Rendering;

namespace Castlemist
{
    /// Shared foldout and property helpers.
    public abstract class GW2GUIBase : ShaderGUI
    {
        protected MaterialEditor editor;
        protected MaterialProperty[] props;
        static readonly string[] UvNames = { "UV0", "UV1", "UV2", "UV3" };

        protected MaterialProperty P(string name) { return FindProperty(name, props, false); }

        protected bool On(string name) { MaterialProperty p = P(name); return p != null && p.floatValue > 0.5f; }

        protected void Prop(string name, string label = null, string tip = null)
        {
            MaterialProperty p = P(name);
            if (p != null) editor.ShaderProperty(p, new GUIContent(label ?? p.displayName, tip));
        }

        /// A texture on one line, with an optional colour or float on its own line under it (inline
        /// beside the texture, the UV set popup that follows covered the colour's hit area: the swatch
        /// could not be clicked, 2026-10-22); true when either changed.
        protected bool Tex(string tex, string label, string tip, string extra = null)
        {
            MaterialProperty t = P(tex);
            if (t == null) return false;
            EditorGUI.BeginChangeCheck();
            editor.TexturePropertySingleLine(new GUIContent(label, tip), t);
            MaterialProperty e = extra != null ? P(extra) : null;
            if (e != null)
            {
                EditorGUI.indentLevel++;
                editor.ShaderProperty(e, new GUIContent(e.displayName, tip));
                EditorGUI.indentLevel--;
            }
            return EditorGUI.EndChangeCheck();
        }

        /// A foldout section, its state remembered per shader for the editor session.
        protected bool Section(string title, bool open = true)
        {
            string key = "Castlemist.GW2GUI." + GetType().Name + "." + title;
            bool v = SessionState.GetBool(key, open);
            EditorGUILayout.Space(4);
            bool n = EditorGUILayout.BeginFoldoutHeaderGroup(v, title);
            EditorGUILayout.EndFoldoutHeaderGroup();
            if (n != v) SessionState.SetBool(key, n);
            if (n) EditorGUI.indentLevel++;
            return n;
        }

        protected void EndSection() { EditorGUI.indentLevel--; }

        /// Sets the block float `toggle` on every target from whether `tex` is assigned.
        protected void BlockFromTexture(string tex, string toggle)
        {
            foreach (UnityEngine.Object o in editor.targets)
            {
                var m = (Material)o;
                if (m.HasProperty(toggle)) m.SetFloat(toggle, m.GetTexture(tex) != null ? 1f : 0f);
                EditorUtility.SetDirty(m);
            }
        }

        protected void Uv(string name)
        {
            MaterialProperty p = P(name);
            if (p == null) return;
            EditorGUI.BeginChangeCheck();
            EditorGUI.showMixedValue = p.hasMixedValue;
            int v = EditorGUILayout.Popup("UV set", Mathf.Clamp((int)p.floatValue, 0, 3), UvNames);
            EditorGUI.showMixedValue = false;
            if (EditorGUI.EndChangeCheck()) p.floatValue = v;
        }

        protected static void Note(string text) { EditorGUILayout.LabelField(text, EditorStyles.wordWrappedMiniLabel); }
    }

    public class GW2GUI : GW2GUIBase
    {
        static readonly string[] ModeNames =
        {
            "Opaque", "Cutout", "Fade", "Transparent (premultiplied)", "Additive", "Soft Additive",
            "Multiplicative", "2x Multiplicative", "Transparent Clipping",
        };
        static readonly int[] ModeValues = { 0, 1, 2, 3, 4, 5, 6, 7, 9 };

        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;
            var mat = (Material)editor.target;

            EditorGUILayout.LabelField("castlemist GW2", EditorStyles.boldLabel);
            Note("One shader for GW2 world materials: every block is a switch on the material (no keywords). " +
                 "Imported values come from the export's materials.json; re-importing the world overwrites edits.");
            RenderMode(mat);
            Prop("_Unlit", "Unlit", "GW2's effect shaders apply no lighting (fog only)");

            bool parallax = On("_UseParallax");
            if (!parallax && Section("Colour"))
            {
                Tex("_MainTex", "Base colour", "RGB colour, A alpha (cutout / blend) or GW2 shine", "_Color");
                Uv("_MainTexUV");
                editor.TextureScaleOffsetProperty(P("_MainTex"));
                if (On("_AlphaTest")) Prop("_Cutoff", "Alpha cutoff");
                EndSection();
            }
            if (Section("Parallax layer", parallax))
            {
                Prop("_UseParallax", "Parallax layer", "GW2 fx-parallax: a layer seen through the surface, offset by a map and the view");
                if (On("_UseParallax"))
                {
                    Tex("_ParallaxTex", "Parallax colour", "The layer seen through the surface (RGB) and its alpha");
                    Uv("_ParallaxUV");
                    editor.TextureScaleOffsetProperty(P("_ParallaxTex"));
                    Tex("_OffsetTex", "UV offset map", "RG offsets the layer's UV, x paraper");
                    Uv("_OffsetUV");
                    Prop("_ParDist", "Parallax depth (pardist)", "How far the layer seems to sit behind the surface");
                    Prop("_ParaPer", "Offset strength (paraper)");
                    if (On("_AlphaTest")) Prop("_Cutoff", "AlphaRef");
                }
                EndSection();
            }
            bool interior = On("_UseInterior");
            if (Section("Interior layer", interior))
            {
                Prop("_UseInterior", "Interior layer", "GW2 prop-interior: a layer seen inside the surface, mixed with the colour by the mask");
                if (On("_UseInterior"))
                {
                    Tex("_InteriorTex", "Interior colour", "RGB colour, A shine; offset by the view (pardist) and the normal");
                    Uv("_InteriorUV");
                    Prop("_ParDist", "Depth (pardist)");
                    Prop("_InteriorNormalShift", "Normal shift");
                    Prop("_InteriorMode", "Mix");
                    Prop("_InteriorStep", "Step the mix (stepmin, decfade)");
                    if (On("_InteriorStep"))
                    {
                        Prop("_StepMin", "stepmin");
                        Prop("_InteriorStepOffset", "decfade");
                    }
                }
                EndSection();
            }
            if (Section("Cutout and burn edge", On("_UseCutout")))
            {
                Prop("_UseCutout", "Cutout", "GW2 opacity: holes where the cutout channel x opacity is below 0.5");
                if (On("_UseCutout"))
                {
                    Tex("_CutoutTex", "Cutout", "Linear; its RGB also drives the burn edge");
                    Uv("_CutoutUV");
                    Prop("_CutoutChannel", "Channel");
                    Prop("_CutoutScale", "opacity");
                    Prop("_BurnColor", "Burn edge colour (burncol)");
                    Prop("_BurnOffset", "burnoff");
                }
                EndSection();
            }
            if (Section("Mask and perturb", parallax || interior))
            {
                Tex("_MaskTex", "Mask", "The opacity mask, or the interior / colour mix");
                Uv("_MaskUV");
                Prop("_MaskChannel", "Mask channel");
                Tex("_PerturbTex", "Perturb", "RG offsets the mask's UV (x mskptrb) and the colour's (x difptrb)");
                Uv("_PerturbUV");
                Prop("_PerturbSigned", "Signed (x 2 - 1)");
                Prop("_MskPtrb", "Mask perturb (mskptrb)");
                Prop("_BasePtrb", "Colour perturb (difptrb)");
                EndSection();
            }
            if (Section("Alpha shaping", parallax))
            {
                Prop("_UseMask", "Opacity mask", "The mask channel multiplies the alpha");
                Prop("_UseRamp", "View-angle ramp");
                if (On("_UseRamp")) Tex("_RampTex", "Ramp", "R at |N.V|: fades by view angle");
                Prop("_UseStep", "Step (stepmin / stepcol)");
                if (On("_UseStep"))
                {
                    Prop("_StepMin", "stepmin");
                    Prop("_StepCol", "stepcol (x scale, w offset)");
                }
                else Prop("_AlphaScale", "Alpha scale (fade)");
                Prop("_SoftFade", "Soft depth fade", "Fades where it meets geometry; needs the camera depth texture (a shadow-casting light in VRChat)");
                if (On("_SoftFade")) Prop("_DepDist", "Fade distance (Unity units)");
                EndSection();
            }
            if (Section("Normal map"))
            {
                if (Tex("_BumpMap", "Normal map", "Tangent-space normal map (imported as Normal map)", On("_UseNormalMap") ? "_BumpScale" : null))
                    BlockFromTexture("_BumpMap", "_UseNormalMap");
                if (On("_UseNormalMap")) Uv("_BumpMapUV");
                EndSection();
            }
            if (Section("Metal / smoothness (packed)"))
            {
                if (Tex("_PackedMap", "Packed map", "R metal, G smoothness, B reflection mask, A specular mask (linear)"))
                    BlockFromTexture("_PackedMap", "_UsePacked");
                bool packed = On("_UsePacked");
                if (packed) Uv("_PackedMapUV");
                Prop("_Metallic", packed ? "Metallic (x map R)" : "Metallic");
                Prop("_Glossiness", packed ? "Smoothness (x map G)" : "Smoothness");
                Prop("_ReflectionTint", "Reflection tint");
                Prop("_ReflectionStrength", "Reflection strength");
                Prop("_SpecularTint", "Specular tint");
                Prop("_SpecularStrength", "Specular strength");
                EndSection();
            }
            if (Section("Detail mod", false))
            {
                Note("GW2's mod map: albedo x 2 x mod, so 0.5 grey changes nothing.");
                if (Tex("_DetailMod", "Mod map", "RGB detail, multiplied x2"))
                    BlockFromTexture("_DetailMod", "_UseDetailMod");
                if (On("_UseDetailMod"))
                {
                    Uv("_DetailModUV");
                    editor.TextureScaleOffsetProperty(P("_DetailMod"));
                    Prop("_DetailModStrength", "Strength");
                }
                EndSection();
            }
            if (Section("Emission", false))
            {
                Prop("_UseEmission", "Emission", "Self-illumination");
                if (On("_UseEmission"))
                {
                    Tex("_EmissionMap", "Emission map", "RGB emission", "_EmissionColor");
                    if (mat.GetTexture("_EmissionMap") != null) Uv("_EmissionMapUV");
                    Tex("_EmissionMask", "Emission mask", "Mask channel x emission (linear)");
                    if (mat.GetTexture("_EmissionMask") != null)
                    {
                        Uv("_EmissionMaskUV");
                        Prop("_EmissionMaskChannel", "Mask channel");
                    }
                    Prop("_EmissionStrength", "Strength");
                    editor.LightmapEmissionProperty();
                }
                EndSection();
            }
            if (Section("Wave (cloth, flags)", On("_UseWave")))
            {
                Note("castlemist's own: GW2 runs cloth physics on banners; this waves them in the vertex shader. " +
                     "'GW2 cloth pins' uses the export's distance from the cloth's pinned edge (meshes without it stay still).");
                Prop("_UseWave", "Wave");
                if (On("_UseWave"))
                {
                    Prop("_WaveMask", "Wave weight");
                    Prop("_WaveAmplitude", "Amplitude (Unity units)");
                    Prop("_WaveSpeed", "Speed");
                    Prop("_WaveFrequency", "Ripples along the cloth");
                    Prop("_WaveSway", "Sway with the wind");
                    Prop("_WaveDir", "Wind direction (world)");
                    // The tessellated twin (same properties): the wave subdivided on the GPU. PC only.
                    var cur = (Material)editor.target;
                    bool tess = cur.shader != null && cur.shader.name == CastlemistMaterials.TessellatedShaderName;
                    bool want = EditorGUILayout.Toggle(new GUIContent("Tessellate (PC)", "Castlemist/GW2 Tessellated: smoother waves, heavier; not on Quest"), tess);
                    if (want != tess)
                    {
                        Shader s = Shader.Find(want ? CastlemistMaterials.TessellatedShaderName : "Castlemist/GW2");
                        if (s != null)
                            foreach (UnityEngine.Object o in editor.targets) { Undo.RecordObject(o, "Tessellate"); ((Material)o).shader = s; }
                    }
                    if (tess)
                    {
                        Prop("_TessEdgeLength", "Edge length (pixels)");
                        Prop("_TessMaxFactor", "Max subdivision");
                        Prop("_TessMaxDistance", "Fade out by (Unity units)");
                    }
                }
                EndSection();
            }
            if (Section("UV sets and animation", false))
            {
                Prop("_UV1FromUV4", "GW2 UV1 from UV4", "On: GW2's UV1 maps read the export's UV4 copy, so lightmaps can own UV1");
                Note("Per UV set, from the GW2 model's texTransforms: scroll (x, y in UV per second) and rotation (z, radians per second) about the UV centre.");
                for (int k = 0; k < 4; ++k) Prop("_UVAnim" + k, "UV" + k);
                EndSection();
            }
            if (!On("_Unlit") && Section("Rim light", On("_UseRim")))
            {
                Prop("_UseRim", "Rim light", "GW2 rimcp: the main light x colour x (1 - N.V)^power");
                if (On("_UseRim"))
                {
                    Prop("_RimColor", "Colour (rimcp rgb)");
                    Prop("_RimPower", "Power (rimcp w)");
                }
                EndSection();
            }
            if (!On("_Unlit") && Section("VRC Light Volumes", false))
            {
                Prop("_LightVolumes", "Use Light Volumes", "Sample VRC Light Volumes when the scene has them; light probes otherwise");
                Prop("_LightVolumeSpecular", "Volume speculars");
                Prop("_LightVolumesBias", "Bias along the normal");
                Prop("_PointLightVolumesShading", "Point Light Volumes shading");
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_AlphaTest", "Alpha test");
                Prop("_Cull", "Cull");
                Prop("_ZWrite", "ZWrite");
                Prop("_ZTest", "ZTest");
                Prop("_AdditiveFog", "Fog to black", "For additive blends: fog fades them out instead of adding fog colour");
                Prop("_AlphaPremultiply", "Premultiply alpha");
                editor.RenderQueueField();
                editor.EnableInstancingField();
                editor.DoubleSidedGIField();
                EndSection();
            }
        }

        void RenderMode(Material mat)
        {
            MaterialProperty mode = P("_Mode");
            if (mode == null) return;
            int cur = Array.IndexOf(ModeValues, (int)mode.floatValue);
            bool custom = cur < 0;
            var names = new string[ModeNames.Length + 1];
            ModeNames.CopyTo(names, 0);
            names[ModeNames.Length] = "Custom (blend factors below)";
            EditorGUI.showMixedValue = mode.hasMixedValue;
            EditorGUI.BeginChangeCheck();
            int pick = EditorGUILayout.Popup(new GUIContent("Render mode", "The GW2 blend as a preset; sets blend, ZWrite, queue, shadows"),
                                             custom ? ModeNames.Length : cur, names);
            EditorGUI.showMixedValue = false;
            if (EditorGUI.EndChangeCheck())
            {
                editor.RegisterPropertyChangeUndo("Render mode");
                if (pick < ModeNames.Length)
                    foreach (UnityEngine.Object o in editor.targets)
                    {
                        CastlemistMaterials.ApplyRenderMode((Material)o, ModeValues[pick]);
                        EditorUtility.SetDirty(o);
                    }
                else mode.floatValue = -1f;   // Custom: keep the factors, edit them below
            }
            if (pick == ModeNames.Length || custom)
            {
                EditorGUI.indentLevel++;
                Prop("_SrcBlend", "Src blend");
                Prop("_DstBlend", "Dst blend");
                Prop("_BlendOp", "Blend op");
                Prop("_SrcBlendAlpha", "Src blend (alpha)");
                Prop("_DstBlendAlpha", "Dst blend (alpha)");
                Prop("_BlendOpAlpha", "Blend op (alpha)");
                EditorGUI.indentLevel--;
            }
        }
    }

    public class GW2ParticleGUI : GW2GUIBase
    {
        // name, src, dst, premultiply, fog to black
        static readonly string[] Names = { "Additive", "Soft Additive", "Alpha blend", "Premultiplied", "Multiply", "Custom" };

        static readonly BlendMode[,] Factors =
        {
            { BlendMode.SrcAlpha, BlendMode.One },
            { BlendMode.OneMinusDstColor, BlendMode.One },
            { BlendMode.SrcAlpha, BlendMode.OneMinusSrcAlpha },
            { BlendMode.One, BlendMode.OneMinusSrcAlpha },
            { BlendMode.DstColor, BlendMode.Zero },
        };

        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;
            var mat = (Material)editor.target;

            EditorGUILayout.LabelField("castlemist GW2 Particle", EditorStyles.boldLabel);
            Note("Unlit. One of GW2's particle shaders (read from the game), chosen by the importer. " +
                 "The particle renderer needs the custom vertex streams UV, UV2, AnimBlend, StableRandomXYZ.");
            Prop("_PtcShader", "GW2 particle shader");
            Blend(mat);
            int fam = (int)(P("_PtcShader") != null ? P("_PtcShader").floatValue : 0f);

            if (Section("Colour"))
            {
                Tex("_MainTex", "Texture", "RGB colour, A alpha");
                editor.TextureScaleOffsetProperty(P("_MainTex"));
                Prop("_FrameBlend", "Blend flipbook frames");
                if (fam == 1) Prop("_PtcTexAlphaRgb", "Colour x texture alpha");
                if (fam == 3 || fam == 4) Prop("_PtcVcColour", "Colour x 2 x vertex colour");
                if (fam == 0) Prop("_AlphaPremultiply", "Colour x alpha");
                Prop("_UVAnim0", "UV animation (scroll xy, rotate z)");
                EndSection();
            }
            if (fam == 3 && Section("Step dissolve"))
            {
                Prop("_StepMin", "step");
                Prop("_PtcFadeVcR", "Fade by vertex red");
                Prop("_AlphaScale", "diffade");
                EndSection();
            }
            if (fam == 4 && Section("Add layer (second pass)"))
            {
                EditorGUI.BeginChangeCheck();
                Prop("_UseAddLayer", "Add layer");
                if (EditorGUI.EndChangeCheck())
                    foreach (UnityEngine.Object o in editor.targets)
                        ((Material)o).SetShaderPassEnabled("Always", ((Material)o).GetFloat("_UseAddLayer") > 0.5f);
                Tex("_AddTex", "add", "R step, A coverage");
                Prop("_AddColor", "addcolr");
                Prop("_AddStepMin", "stepmin");
                EndSection();
            }
            if ((fam == 5 || fam == 7) && Section("Flow"))
            {
                Tex("_FlowTex", "Flow", "RG offset, linear");
                if (fam == 5)
                {
                    Prop("_Flow", "flow (uv scale, strength, power, scroll)");
                    Prop("_FlowStart", "startcl");
                    Prop("_FlowEnd", "endcl");
                    Prop("_FlowStep", "stepcol");
                }
                else
                {
                    Tex("_LayerTex", "Layer", "opacitytile");
                    Prop("_LayerScroll", "uvscrol");
                    Prop("_LayerFlowUV", "flowuv");
                }
                EndSection();
            }
            if (fam == 6 && Section("Fire", false))
            {
                Prop("_FireMode", "Fire shader");
                Tex("_MaskTex", "mask", "fire noise mask");
                Tex("_FlowTex", "flow", "RG offset, linear");
                Prop("_FireParams", "perturb, scalep, speedp, speed");
                Prop("_FireParams2", "scale, scaleb");
                Prop("_FireColorL", "colorl");
                Prop("_FireColorH", "colorh");
                EndSection();
            }
            if (Section("Soft fade", false))
            {
                Prop("_SoftFade", "Soft depth fade", "Needs a depth texture");
                Prop("_DepDist", "Distance (Unity units)");
                Prop("_Cutoff", "AlphaRef");
                EndSection();
            }
            if (Section("Advanced", false))
            {
                Prop("_Cull", "Cull");
                Prop("_ZWrite", "ZWrite");
                Prop("_ZTest", "ZTest");
                editor.RenderQueueField();
                editor.EnableInstancingField();
                EndSection();
            }
        }

        void Blend(Material mat)
        {
            int src = (int)mat.GetFloat("_SrcBlend"), dst = (int)mat.GetFloat("_DstBlend");
            int cur = Names.Length - 1;
            for (int k = 0; k < Factors.GetLength(0); ++k)
                if ((int)Factors[k, 0] == src && (int)Factors[k, 1] == dst) { cur = k; break; }
            EditorGUI.BeginChangeCheck();
            int pick = EditorGUILayout.Popup(new GUIContent("Blend", "How the particle combines with what is behind it"), cur, Names);
            if (EditorGUI.EndChangeCheck() && pick < Factors.GetLength(0))
            {
                editor.RegisterPropertyChangeUndo("Blend");
                foreach (UnityEngine.Object o in editor.targets)
                {
                    var m = (Material)o;
                    m.SetFloat("_SrcBlend", (float)Factors[pick, 0]);
                    m.SetFloat("_DstBlend", (float)Factors[pick, 1]);
                    m.SetFloat("_BlendOp", (float)BlendOp.Add);
                    m.SetFloat("_AlphaPremultiply", pick == 3 ? 1f : 0f);
                    m.SetFloat("_AdditiveFog", pick <= 1 ? 1f : 0f);
                    m.SetFloat("_ZWrite", 0f);
                    EditorUtility.SetDirty(m);
                }
            }
            if (pick == Names.Length - 1)
            {
                EditorGUI.indentLevel++;
                Prop("_SrcBlend", "Src blend");
                Prop("_DstBlend", "Dst blend");
                Prop("_BlendOp", "Blend op");
                Prop("_AlphaPremultiply", "Multiply colour by alpha");
                Prop("_AdditiveFog", "Fog to black");
                EditorGUI.indentLevel--;
            }
        }
    }

    /// The inspector of Castlemist/GW2 Distort: GW2's screen distortion, lit armour glass and the
    /// refracting glass / water, in the sections of its shader (2026-10-22).
    public class GW2DistortGUI : GW2GUIBase
    {
        public override void OnGUI(MaterialEditor materialEditor, MaterialProperty[] properties)
        {
            editor = materialEditor;
            props = properties;

            EditorGUILayout.LabelField("castlemist GW2 Distort", EditorStyles.boldLabel);
            Note("The scene behind (a grab pass), offset by the distortion map. GW2's screen distortion " +
                 "(50159, 339341, 47396, 709206, 592660, 27304), lit armour glass (630598) and refracting " +
                 "glass / water (49659, 63923, 57026). The importer sets these from materials.json.");

            if (Section("Distortion"))
            {
                Tex("_DistortTex", "Distortion map", "RG x 2 - 1 offsets the grabbed scene");
                Uv("_DistortUV");
                Prop("_DistortIsNormal", "Map is a Unity normal map");
                Prop("_Distort", "Strength (distort)");
                Prop("_DistortBias", "Add strength as a bias (50159)");
                Prop("_ViewReflect", "Offset x view reflection");
                Prop("_MaskOffset", "Offset x mask R (339341)");
                Prop("_DepthReject", "Keep the foreground out (depth test)");
                Prop("_Tint", "Tint (modxcol x 2)");
                Prop("_UseVertexColor", "x vertex colour (709206)");
                Prop("_UseMap2", "Blend a second normal map");
                if (On("_UseMap2"))
                {
                    Tex("_DistortTex2", "Second normal map", "47396's second map");
                    Uv("_DistortUV2");
                    Prop("_DistortTex2IsNormal", "Second map is a Unity normal map");
                }
                EndSection();
            }
            if (Section("Alpha", false))
            {
                Prop("_AlphaSource", "Alpha");
                Tex("_MaskTex", "Mask", "Opacity mask (linear)");
                Uv("_MaskUV");
                Prop("_MaskChannel", "Mask channel");
                Prop("_Cutoff", "AlphaRef");
                Prop("_StepMin", "stepmin");
                Prop("_StepCol", "stepcol");
                Prop("_UseRamp", "View-angle ramp");
                if (On("_UseRamp")) Tex("_RampTex", "Ramp", "R at |N.V|");
                Prop("_SoftFade", "Soft depth fade (needs a depth texture)");
                if (On("_SoftFade")) Prop("_DepDist", "depdist (Unity units)");
                EndSection();
            }
            if (Section("Lit glass (630598)", On("_UseLit")))
            {
                Prop("_UseLit", "Lit glass");
                if (On("_UseLit"))
                {
                    Tex("_MainTex", "Diffuse / decal", "RGB colour, A coverage and gloss");
                    Uv("_MainUV");
                    Prop("_UseMainTex", "Diffuse from the map");
                    Prop("_LitDiffuse", "Diffuse without a map");
                    Prop("_LitFadeDist", "Distortion fade distance");
                    Prop("_SunScale", "Sun colour scale");
                }
                EndSection();
            }
            if (Section("Refracting glass and water", On("_Glass")))
            {
                Prop("_Glass", "Mode");
                if (On("_Glass"))
                {
                    Prop("_GlassSpec", "Specular colour");
                    Prop("_GlassSpecPower", "Specular power");
                    Prop("_GlassSpecDouble", "Specular x2 (water)");
                    Prop("_ReflectionStrength", "Reflection probe strength");
                    Prop("_LightVolumes", "Use Light Volumes");
                    Prop("_LightVolumesBias", "Light Volumes bias");
                    Prop("_PointLightVolumesShading", "Point Light Volumes shading");
                }
                EndSection();
            }
            if (Section("UV animation", false))
            {
                Note("Per UV set: scroll (x, y in UV per second) and rotation (z, radians per second).");
                for (int k = 0; k < 4; ++k) Prop("_UVAnim" + k, "UV" + k);
                EndSection();
            }
            if (Section("Render state", false))
            {
                Prop("_SrcBlend", "Src blend");
                Prop("_DstBlend", "Dst blend");
                Prop("_ZWrite", "ZWrite");
                Prop("_Cull", "Cull");
                editor.RenderQueueField();
                editor.EnableInstancingField();
                EndSection();
            }
        }
    }
}
