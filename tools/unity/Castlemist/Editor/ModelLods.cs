// castlemist world export: model LODs to Unity LODGroups (Task 15b). No UnityEngine
// dependency (Tests~ runs it).
//
// The export (models[].lods, instances[].lod; docs/research/gw2-model-lods.md amendment
// 2026-10-17) gives each model LOD0..LOD(levels-1) as FBX objects "<LOD0 name>_LOD<k>"
// (only for the levels a mesh has its own index set for) and the switch distances d1, d2
// in model units. The client switches at d_k x the instance's largest axis scale, from the
// camera to the model's bounds.
//
// Screen-relative height. Unity shows LOD k while the object's screen-relative height is
// at least lods[k].screenRelativeTransitionHeight, with
//     height = LODGroup.size x (largest lossy scale) / (2 x distance x tan(fov / 2))
// (before QualitySettings.lodBias, left at the project's value: VRChat's is UNPROVEN).
// LODGroup.size and the distance both carry the instance scale s and unityScale u, and the
// client's distance is d_k x s, so
//     h(d_k) = size / (2 x d_k x tan(fov / 2))
// with size the LODGroup's own size (prefab units = map units) and d_k in model units: one
// value per model, right for every instance. fov = lodCullFovDeg (60, VRChat's FOV UNPROVEN).
//
// Levels: LOD k (k < levels - 1) ends at h(d_{k+1}); the last level ends at the cull height:
// a prop's lodPropCullHeight (0: never culled, as before) or a foliage tree's
// FoliageRecords.CullHeight (fadeFar x fadeQualityScale x Distance). Heights are clamped
// to 1 and made strictly decreasing (a level squeezed to nothing keeps a 1e-6 relative
// sliver); a transition at or below the cull height drops the levels after it (they would
// never show), so a tree whose fadeFar is shorter than d2 has fewer levels.
//
// Renderers: level k of a LOD0 object "<b>" is "<b>_LOD<j>" for the largest j <= k that
// exists, else the LOD0 object itself (a mesh without its own level repeats its coarser
// one, settings.lodMissingLevel).

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace Castlemist
{
    public static class ModelLods
    {
        /// lodPropCullHeight (castlemist's choice): a prop's last level is never culled.
        public const double PropCullHeight = 0.0;

        static readonly Regex LodSuffix = new Regex(@"_LOD(\d+)$", RegexOptions.CultureInvariant);

        /// size / (2 d tan(fov / 2)); +infinity for d <= 0, 0 for a size that is not positive.
        public static double ScreenHeight(double size, double distance, double fovDeg = FoliageRecords.CullFovDeg)
        {
            if (!(size > 0) || double.IsInfinity(size)) return 0.0;
            if (!(distance > 0)) return double.PositiveInfinity;
            return size / (2.0 * distance * Math.Tan(fovDeg * Math.PI / 360.0));
        }

        /// The transition heights of the levels kept (see the file comment): element k is
        /// LOD k's screenRelativeTransitionHeight, the last one is cullHeight. Never empty.
        public static double[] TransitionHeights(double size, double[] distances, int levels, double cullHeight,
                                                 double fovDeg = FoliageRecords.CullFovDeg)
        {
            if (levels < 1) levels = 1;
            if (!(cullHeight >= 0) || double.IsInfinity(cullHeight)) cullHeight = 0;
            cullHeight = Math.Min(cullHeight, 1.0);
            var h = new List<double>();
            for (int k = 1; k < levels; ++k)
            {
                double d = distances != null && k - 1 < distances.Length ? distances[k - 1] : 0;
                h.Add(Math.Min(1.0, ScreenHeight(size, d, fovDeg)));
            }
            for (int k = 1; k < h.Count; ++k)
                if (h[k] >= h[k - 1]) h[k] = h[k - 1] * (1.0 - 1e-6);
            while (h.Count > 0 && h[h.Count - 1] <= cullHeight) h.RemoveAt(h.Count - 1);
            h.Add(cullHeight);
            return h.ToArray();
        }

        /// The LOD level of an FBX object name: k for "<base>_LOD<k>", else 0 (base = the name).
        public static int LevelOf(string name, out string baseName)
        {
            Match mt = name != null ? LodSuffix.Match(name) : Match.Empty;
            int k;
            if (mt.Success && int.TryParse(mt.Groups[1].Value, out k) && k > 0)
            {
                baseName = name.Substring(0, mt.Index);
                return k;
            }
            baseName = name;
            return 0;
        }

        /// For each level 0..levels-1, the indices into names of the objects drawn at it
        /// (see the file comment). An object "<b>_LOD<k>" with no LOD0 "<b>" is drawn at its
        /// level only and reported in orphans; one at a level >= levels is reported too.
        public static List<int>[] Assign(IList<string> names, int levels, List<string> orphans = null)
        {
            if (levels < 1) levels = 1;
            var byBase = new Dictionary<string, Dictionary<int, int>>(StringComparer.Ordinal);
            var order = new List<string>();
            for (int i = 0; i < names.Count; ++i)
            {
                string b;
                int k = LevelOf(names[i], out b);
                Dictionary<int, int> lv;
                if (!byBase.TryGetValue(b, out lv)) { lv = new Dictionary<int, int>(); byBase[b] = lv; order.Add(b); }
                if (!lv.ContainsKey(k)) lv[k] = i;
            }
            var outp = new List<int>[levels];
            for (int k = 0; k < levels; ++k) outp[k] = new List<int>();
            foreach (string b in order)
            {
                Dictionary<int, int> lv = byBase[b];
                foreach (int k in lv.Keys)
                    if (k >= levels && orphans != null) orphans.Add(names[lv[k]] + " (level " + k + " of " + levels + ")");
                if (!lv.ContainsKey(0))
                {
                    foreach (KeyValuePair<int, int> kv in lv)
                        if (kv.Key < levels) outp[kv.Key].Add(kv.Value);
                    if (orphans != null) foreach (int i in lv.Values) orphans.Add(names[i] + " (no LOD0 " + b + ")");
                    continue;
                }
                for (int k = 0; k < levels; ++k)
                {
                    int j = k;
                    while (j > 0 && !lv.ContainsKey(j)) --j;
                    outp[k].Add(lv[j]);
                }
            }
            return outp;
        }
    }
}
