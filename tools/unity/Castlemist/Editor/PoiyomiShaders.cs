// castlemist world export: which Poiyomi shader the importer uses, and its
// rendering presets (Task 15c). No UnityEngine dependency (the Unity-free tests
// run it); PoiyomiMaterials does the Shader.Find calls and passes the results in.
//
// Shader names, read from the .shader files' `Shader "..."` lines:
//   Poiyomi Pro 11.0.22   11.0: ".poiyomi/Poiyomi Pro", ".poiyomi/Poiyomi Toon";
//                         10.0.23: ".poiyomi/Old Versions/10.0/Poiyomi Pro" / "... Toon";
//                         9.3.67: ".poiyomi/Old Versions/9.3/Poiyomi Pro" / "... Toon"
//   Poiyomi Pro 10.0.9    10.0: ".poiyomi/Poiyomi Pro", ".poiyomi/Poiyomi Toon";
//                         9.3: under ".poiyomi/Old Versions/9.3/"
//   Poiyomi Toon 10 (free) ".poiyomi/Poiyomi Toon"
// So a plain name can be 10 or 11: the version comes from the hidden
// shader_master_label property ("Poiyomi 11.0.22").
//
// Choice: the highest major version, then Pro over Toon, then the order of
// Names; a shader with compile errors is never chosen. Every property the
// importer sets has the same name and type in 10.0.9, 10.0.23 and 11.0.22 (and
// 9.3.67), and the _Mode preset actions are identical in all of them, so one
// property map serves both; the presets are still read from the chosen
// shader's own _Mode description (ParsePresets), so a version that changes
// them is followed.

using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;

namespace Castlemist
{
    public static class PoiyomiShaders
    {
        /// The names tried with Shader.Find, in this order.
        public static readonly string[] Names =
        {
            ".poiyomi/Poiyomi Pro",
            ".poiyomi/Poiyomi Toon",
            ".poiyomi/Old Versions/10.0/Poiyomi Pro",
            ".poiyomi/Old Versions/10.0/Poiyomi Toon",
            ".poiyomi/Old Versions/9.3/Poiyomi Pro",
            ".poiyomi/Old Versions/9.3/Poiyomi Toon",
        };

        /// Every property PoiyomiMaterials may set on a Poiyomi material; the
        /// importer warns about any the chosen shader lacks.
        public static readonly string[] Properties =
        {
            "_Mode", "_SrcBlend", "_DstBlend", "_BlendOp", "_SrcBlendAlpha", "_DstBlendAlpha", "_BlendOpAlpha",
            "_ZWrite", "_ZTest", "_Cull", "_Cutoff", "_AlphaForceOpaque", "_AlphaPremultiply", "_AlphaToCoverage",
            "_AddSrcBlend", "_AddDstBlend", "_AddSrcBlendAlpha", "_AddDstBlendAlpha",
            "_OutlineSrcBlend", "_OutlineDstBlend", "_OutlineSrcBlendAlpha", "_OutlineDstBlendAlpha",
            "_OutlineBlendOp", "_OutlineBlendOpAlpha",
            "_MainTex", "_Color", "_MainTexUV", "_MainTexPan", "_BumpMap", "_BumpMapUV", "_BumpScale",
            "_MochieBRDF", "_MochieMetallicMaps", "_MochieMetallicMapsUV", "_MochieMetallicMultiplier",
            "_MochieRoughnessMultiplier", "_MochieSpecularTint", "_MochieReflectionTint",
            "_EnableEmission", "_EmissionMap", "_EmissionMapUV", "_EmissionMask", "_EmissionMaskUV",
            "_EmissionColor", "_EmissionStrength", "_LightingMode",
        };

        public sealed class Found
        {
            public string Name;
            /// From shader_master_label, or null.
            public string Version;
            public bool HasError;
        }

        /// "11.0.22" from "<color=#E75898ff>Poiyomi 11.0.22</color>", or null.
        public static string VersionOf(string label)
        {
            if (label == null) return null;
            Match m = Regex.Match(label, @"Poiyomi (\d+\.\d+[^<\s]*)");
            return m.Success ? m.Groups[1].Value : null;
        }

        /// The major version, or -1.
        public static int Major(string version)
        {
            if (version == null) return -1;
            Match m = Regex.Match(version, @"^(\d+)\.");
            return m.Success ? int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture) : -1;
        }

        public static bool IsPro(string name)
        {
            return name != null && name.EndsWith("Poiyomi Pro", System.StringComparison.Ordinal);
        }

        /// The majors whose property names were checked against the shader source (10.0.9, 10.0.23, 11.0.22).
        public static bool Checked(int major) { return major == 10 || major == 11; }

        /// The index in `found` of the shader to use, or -1 when none can be.
        public static int Choose(IList<Found> found)
        {
            int best = -1;
            for (int i = 0; i < found.Count; ++i)
            {
                if (found[i].HasError) continue;
                if (best < 0 || Better(found[i], found[best])) best = i;
            }
            return best;
        }

        static bool Better(Found a, Found b)
        {
            int ma = Major(a.Version), mb = Major(b.Version);
            if (ma != mb) return ma > mb;
            return IsPro(a.Name) && !IsPro(b.Name);   // otherwise the earlier one stays
        }

        /// One _Mode value's actions: the render queue, the RenderType tag and the float properties it sets.
        public sealed class PresetActions
        {
            public int Queue = -1;
            public string RenderType;
            public readonly Dictionary<string, float> Floats = new Dictionary<string, float>();
            /// The float properties in the shader's order.
            public readonly List<string> Order = new List<string>();
        }

        /// The presets in a _Mode property description ("Rendering Preset--{on_value_actions:[{value:N,actions:[
        /// {type:SET_PROPERTY,data:render_queue=2000}, ...]}, ...]}"), by value. What ThryEditor's inspector does
        /// when the preset is picked; a script setting _Mode must do it itself. Rows without a render_queue
        /// are left out; an empty result means "use the importer's own table".
        public static Dictionary<int, PresetActions> ParsePresets(string description)
        {
            var r = new Dictionary<int, PresetActions>();
            if (description == null) return r;
            foreach (Match row in Regex.Matches(description, @"\{\s*value\s*:\s*(\d+)\s*,\s*actions\s*:\s*\[(.*?)\]\s*\}", RegexOptions.Singleline))
            {
                var p = new PresetActions();
                foreach (Match a in Regex.Matches(row.Groups[2].Value, @"\{\s*type\s*:\s*SET_PROPERTY\s*,\s*data\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^}]*?)\s*\}"))
                {
                    string key = a.Groups[1].Value, val = a.Groups[2].Value;
                    if (key == "render_queue")
                    {
                        int q;
                        if (int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out q)) p.Queue = q;
                    }
                    else if (key == "render_type") p.RenderType = val;
                    else
                    {
                        float f;
                        if (!float.TryParse(val, NumberStyles.Float, CultureInfo.InvariantCulture, out f)) continue;
                        if (!p.Floats.ContainsKey(key)) p.Order.Add(key);
                        p.Floats[key] = f;
                    }
                }
                if (p.Queue >= 0) r[int.Parse(row.Groups[1].Value, CultureInfo.InvariantCulture)] = p;
            }
            return r;
        }
    }
}
