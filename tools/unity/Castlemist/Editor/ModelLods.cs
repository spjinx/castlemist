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
// a prop's lodPropCullHeight (1% of the screen, divided by the importer's Distance) or a foliage tree's
// FoliageRecords.CullHeight (fadeFar x fadeQualityScale x Distance). Heights are clamped
// to 1 and made strictly decreasing (a level squeezed to nothing keeps a 1e-6 relative
// sliver); a transition at or below the cull height drops the levels after it (they would
// never show), so a tree whose fadeFar is shorter than d2 has fewer levels.
//
// Renderers: level k of a LOD0 object "<b>" is "<b>_LOD<j>" for the largest j <= k that
// exists, else the LOD0 object itself (a mesh without its own level repeats its coarser
// one, settings.lodMissingLevel).
//
// Stand-ins (models[].lods.farMeshes, settings.lodFarMeshes): the "lod" meshes (mesh flag 0x4)
// are left out of the levels above and drawn alone as one more level, FarLevel(levels), from
// the next switch distance (d1 for a model with one level, else d2; with three levels they
// take LOD2's place). A model whose only meshes are stand-ins draws them as ordinary meshes.

using System;
using System.Collections.Generic;
using System.Text.RegularExpressions;

namespace Castlemist
{
    public static class ModelLods
    {
        /// lodPropCullHeight (castlemist's choice): a prop's last level culls below 1% of the
        /// screen height (GW2's prop fade distance is not traced).
        public const double PropCullHeight = 0.01;

        /// The prop cull height for the importer's Distance option (a larger Distance culls later).
        public static double PropCull(double distance)
        {
            return distance > 0 ? PropCullHeight / distance : PropCullHeight;
        }

        /// The level the stand-in meshes take for a model of `levels` levels: 1, 2 or 2.
        public static int FarLevel(int levels) { return Math.Min(Math.Max(levels, 1), 2); }

        /// The LODGroup's level count: `levels`, or FarLevel(levels) + 1 with stand-ins.
        public static int LevelCount(int levels, bool hasFar)
        {
            if (levels < 1) levels = 1;
            return hasFar ? FarLevel(levels) + 1 : levels;
        }

        /// True when the object `name` is one of the stand-ins `far` names (itself or its "_LOD<k>" base).
        public static bool IsFar(string name, ICollection<string> far)
        {
            if (far == null || far.Count == 0 || name == null) return false;
            string b;
            LevelOf(name, out b);
            return far.Contains(name) || far.Contains(b);
        }

        /// Assign with stand-ins: the others by Assign over `levels`, cut to FarLevel(levels)
        /// levels, then one level of the stand-ins. `hasFar` says whether the split applied
        /// (false: no stand-in matched, or nothing else is drawn; then it is Assign's result).
        public static List<int>[] Assign(IList<string> names, int levels, ICollection<string> far,
                                         out bool hasFar, List<string> orphans = null)
        {
            var normal = new List<int>();
            var farIdx = new List<int>();
            for (int i = 0; i < names.Count; ++i) (IsFar(names[i], far) ? farIdx : normal).Add(i);
            hasFar = farIdx.Count > 0 && normal.Count > 0;
            if (!hasFar) return Assign(names, levels, orphans);
            var sub = Assign(normal.ConvertAll(i => names[i]), levels, orphans);
            int f = FarLevel(levels);
            var outp = new List<int>[f + 1];
            for (int k = 0; k < f; ++k) outp[k] = sub[Math.Min(k, sub.Length - 1)].ConvertAll(j => normal[j]);
            // The stand-ins' own "_LOD<k>" objects (if any) are not drawn: LOD0 objects only.
            string b;
            outp[f] = farIdx.FindAll(i => LevelOf(names[i], out b) == 0);
            if (outp[f].Count == 0) outp[f] = farIdx;
            return outp;
        }

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
