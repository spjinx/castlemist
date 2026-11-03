// HazeCopy (2026-10-25): the GW2 Haze material's values onto a material that fogs itself
// (Shaders/GW2HazeFog.cginc: Castlemist/GW2 Water and Castlemist/GW2 Distort, drawn after the haze
// volume, which does not fog them). Apply(mapRoot, ...) does every such material under the map root.
using System.Collections.Generic;
using UnityEditor;
using UnityEngine;

namespace Castlemist
{
    public static class HazeCopy
    {
        /// The haze volume's material of map @p mapFileId in export folder @p root, or null.
        public static Material HazeOf(string root, long mapFileId)
        {
            return AssetDatabase.LoadAssetAtPath<Material>(root + "/GW2_" + mapFileId + "_Haze.mat");
        }

        /// @p haze's values onto @p m (_UseHaze 0 when @p haze is null). False when @p m cannot fog itself.
        public static bool CopyTo(Material m, Material haze)
        {
            if (m == null || !m.HasProperty("_UseHaze")) return false;
            m.SetFloat("_UseHaze", haze != null ? 1f : 0f);
            if (haze != null)
            {
                foreach (string c in new[] { "_HazeNear", "_HazeFar", "_HazeHeight" }) m.SetColor(c, haze.GetColor(c));
                m.SetVector("_HazeDistRange", haze.GetVector("_DistRange"));
                m.SetVector("_HazeHeightRange", haze.GetVector("_HeightRange"));
                m.SetFloat("_HazeDepthCue", haze.GetFloat("_DepthCue"));
                m.SetFloat("_HazeMapOriginY", haze.GetFloat("_MapOriginY"));
                m.SetFloat("_HazeUnityScale", haze.GetFloat("_UnityScale"));
                m.SetFloat("_HazeAmount", haze.HasProperty("_Amount") ? haze.GetFloat("_Amount") : 1f);
            }
            EditorUtility.SetDirty(m);
            return true;
        }

        /// Every self-fogging material the renderers under @p mapRoot use; returns them.
        public static List<Material> Apply(GameObject mapRoot, Material haze)
        {
            var done = new HashSet<Material>();
            foreach (Renderer r in mapRoot.GetComponentsInChildren<Renderer>(true))
                foreach (Material m in r.sharedMaterials)
                    if (m != null && !done.Contains(m) && m.HasProperty("_UseHaze") && CopyTo(m, haze)) done.Add(m);
            return new List<Material>(done);
        }
    }
}
