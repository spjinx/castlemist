// castlemist world export: Unity LODGroups from the model LODs (Task 15b; the rules and the
// screen-height formula are in ModelLods.cs).
//
//   Prefabs/<fileId>.prefab          a model with lods.levels > 1: a LODGroup on the root,
//                                    LOD0..LOD(levels-1) from its "<name>_LOD<k>" objects,
//                                    heights h(d1), h(d2) and lodPropCullHeight (0, never culled)
//   Prefabs/Foliage/<fileId>.prefab  the same levels, the last one ending at the foliage cull
//                                    (FoliageRecords.CullHeight of the LODGroup's size); levels the
//                                    cull hides are dropped and their objects deactivated
//   Props/<fileId>_<prop>            instances[].lod: the instance's own heights (an override on
//                                    the prefab instance), same renderers
// A model without LODs keeps its prefab without a LODGroup (as before); the tree prefab keeps
// its LOD0-only cull LODGroup; grass patches use the LOD0 objects only (FoliagePatches).

using System;
using System.Collections.Generic;
using System.Linq;
using UnityEngine;

namespace Castlemist
{
    public static class LodGroups
    {
        /// The renderers of each level 0..levels-1 under `root` (ModelLods.Assign on the objects' names).
        public static Renderer[][] Levels(GameObject root, int levels, List<string> warnings, string what)
        {
            Renderer[] rs = root.GetComponentsInChildren<Renderer>(true);
            var orphans = new List<string>();
            List<int>[] idx = ModelLods.Assign(rs.Select(r => r.gameObject.name).ToList(), levels, orphans);
            if (orphans.Count > 0 && warnings != null)
                warnings.Add(what + ": LOD objects not matched to a LOD0 object: " + string.Join(", ", orphans.Take(5)));
            return idx.Select(l => l.Select(i => rs[i]).ToArray()).ToArray();
        }

        /// True for an object "<name>_LOD<k>", k > 0.
        public static bool IsLodObject(GameObject go)
        {
            string b;
            return ModelLods.LevelOf(go.name, out b) > 0;
        }

        /// Sets root's LODGroup (added when missing) to the model's levels: heights from
        /// ModelLods.TransitionHeights with the LODGroup's own size, the last one cullOfSize(size).
        /// Objects of dropped levels that no kept level draws are deactivated. Returns the levels kept.
        public static int Build(GameObject root, int levels, double[] distances, Func<double, double> cullOfSize,
                                List<string> warnings, string what)
        {
            Renderer[][] lv = Levels(root, levels, warnings, what);
            LODGroup lg = root.GetComponent<LODGroup>();
            if (lg == null) lg = root.AddComponent<LODGroup>();
            // Every level with a provisional height, so RecalculateBounds measures all of them.
            var all = new LOD[lv.Length];
            for (int k = 0; k < lv.Length; ++k) all[k] = new LOD(0.5f / (k + 1), lv[k]);
            lg.SetLODs(all);
            lg.RecalculateBounds();
            double size = lg.size;
            double[] h = ModelLods.TransitionHeights(size, distances, lv.Length, cullOfSize(size));
            var lods = new LOD[h.Length];
            var kept = new HashSet<Renderer>();
            for (int k = 0; k < h.Length; ++k)
            {
                lods[k] = new LOD((float)h[k], lv[k]);
                foreach (Renderer r in lv[k]) kept.Add(r);
            }
            for (int k = h.Length; k < lv.Length; ++k)
                foreach (Renderer r in lv[k])
                    if (!kept.Contains(r)) r.gameObject.SetActive(false);
            lg.SetLODs(lods);
            lg.RecalculateBounds();
            return h.Length;
        }

        /// An instance's own heights (instances[].lod): the prefab's renderers per level, the
        /// heights from `distances`, the prefab's own cull height kept. False when it has no LODGroup.
        public static bool Override(GameObject inst, double[] distances)
        {
            LODGroup lg = inst.GetComponent<LODGroup>();
            if (lg == null) return false;
            LOD[] lods = lg.GetLODs();
            if (lods.Length < 2) return false;
            double[] h = ModelLods.TransitionHeights(lg.size, distances, lods.Length, lods[lods.Length - 1].screenRelativeTransitionHeight);
            // A cull that drops levels on the override keeps them (props never cull: cannot happen for them).
            for (int k = 0; k < lods.Length - 1 && k < h.Length - 1; ++k) lods[k].screenRelativeTransitionHeight = (float)h[k];
            lg.SetLODs(lods);
            return true;
        }

        /// Strictly decreasing heights in (0, 1] then the last one in [0, 1), every level with a renderer.
        public static string Problem(LODGroup lg, int wantLevels)
        {
            if (lg == null) return "no LODGroup";
            LOD[] lods = lg.GetLODs();
            if (lods.Length != wantLevels) return lods.Length + " levels, want " + wantLevels;
            for (int k = 0; k < lods.Length; ++k)
            {
                float h = lods[k].screenRelativeTransitionHeight;
                if (!(h >= 0f && h <= 1f)) return "LOD" + k + " height " + h;
                if (k > 0 && !(h < lods[k - 1].screenRelativeTransitionHeight)) return "LOD" + k + " height not below LOD" + (k - 1);
                if (lods[k].renderers == null || lods[k].renderers.Length == 0 || lods[k].renderers.Any(r => r == null))
                    return "LOD" + k + " has no renderer";
            }
            return null;
        }
    }
}
