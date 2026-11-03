// castlemist world import: each placed prop's colour (2026-10-27). A map prop stores a byte4 colour
// (PackMapPropObj.color; the export's instances[].color when it is not the neutral 0x80 x 4 or unset): the
// shaders' ModelColor, x 2 in them (75834's fire: rgb x ModelColor x 2; 233257's torch glow x ModelColor).
// Mad King's Labyrinth's torches 233262 burn green by it (00 B9 22 80). Applied here to the prop's particle
// systems (their start colour: the GW2 particle shader multiplies the vertex colour in) and its glowing
// materials (_EmissionColor in a property block, the material left shared). Byte order B, G, R, A (the
// D3D colour layout; UNPROVEN). Setting, not multiplying: safe to run again.

using System.Collections.Generic;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

namespace Castlemist
{
    public static class PropColors
    {
        [MenuItem("Castlemist/Apply Prop Colours (GW2 ModelColor)")]
        public static void Menu() { Debug.Log("CASTLEMIST " + Apply()); }

        public static string Apply()
        {
            var scene = EditorSceneManager.GetActiveScene();
            string root = Path.GetDirectoryName(scene.path).Replace('\\', '/');
            string manPath = root + "/manifest.json";
            if (!File.Exists(manPath)) return "prop colours: the open scene is not in a castlemist world export folder";
            WorldManifest m = WorldManifest.Parse(File.ReadAllText(manPath));
            var tints = new Dictionary<string, Color>();
            foreach (WorldManifest.CellEntry c in m.Cells)
                foreach (WorldManifest.InstanceEntry i in c.Instances)
                    if (i.Color != null && i.Model >= 0 && i.Model < m.Models.Count)
                        tints[m.Models[i.Model].FileId + "_" + i.Prop] = Tint(i.Color);
            if (tints.Count == 0) return "prop colours: the export has none (re-export with a castlemist from 2026-10-27 on)";
            GameObject mapRoot = scene.GetRootGameObjects().FirstOrDefault(g => g.name == "GW2_" + m.MapFileId);
            if (mapRoot == null) return "prop colours: no GW2_" + m.MapFileId + " root";
            int props = 0, systems = 0, glows = 0;
            var mpb = new MaterialPropertyBlock();
            foreach (Transform t in mapRoot.GetComponentsInChildren<Transform>(true))
            {
                if (!tints.TryGetValue(t.name, out Color tint)) continue;
                ++props;
                foreach (ParticleSystem ps in t.GetComponentsInChildren<ParticleSystem>(true))
                {
                    ParticleSystem.MainModule main = ps.main;
                    main.startColor = new Color(tint.r, tint.g, tint.b, 1f);
                    ++systems;
                }
                foreach (Renderer r in t.GetComponentsInChildren<Renderer>(true))
                {
                    if (r is ParticleSystemRenderer) continue;
                    Material mat = r.sharedMaterial;
                    if (mat == null || !mat.HasProperty("_UseEmission") || mat.GetFloat("_UseEmission") < 0.5f || !mat.HasProperty("_EmissionColor")) continue;
                    r.GetPropertyBlock(mpb);
                    Color e = mat.GetColor("_EmissionColor");
                    mpb.SetColor("_EmissionColor", new Color(e.r * tint.r, e.g * tint.g, e.b * tint.b, e.a));
                    r.SetPropertyBlock(mpb);
                    ++glows;
                }
                EditorUtility.SetDirty(t.gameObject);
            }
            EditorSceneManager.MarkSceneDirty(scene);
            return "prop colours: " + props + " placed prop(s) tinted (" + systems + " particle system(s), " + glows + " glowing renderer(s))";
        }

        /// The stored bytes (B, G, R, A) as ModelColor x 2: 0x80 is 1.
        static Color Tint(double[] c)
        {
            return new Color((float)(c[2] / 255.0 * 2.0), (float)(c[1] / 255.0 * 2.0), (float)(c[0] / 255.0 * 2.0), 1f);
        }
    }
}
