// castlemist world export: what one Unity import built (spec D6 step 10).
//
// PlanLine() has the format of tools/world/validate_manifest.py --plan, and
// each count follows the same rule, so the two lines can be compared as text
// (CHECKLIST item 1):
//   materials        model materials built (usedByMeshes != false): one per
//                    distinct (key, dark decal, uvScroll) -- Task 15d, the
//                    shared Materials/Shared/<key>.mat; an entry without a key
//                    (an older export) is its own -- + one per terrain cell +
//                    one per water cell
//   prefabs          prefabs saved, one per model with status ok
//   controllers      AnimatorControllers, one per distinct (model, clip) used
//   cells            Cell_<id> objects
//   instances        prefab instances placed
//   colliders        cells that got at least one Collider_k MeshCollider
//                    (the plan's rule: cells with colliderParts > 0;
//                    colliderParts is the total of parts, logged apart)
//   terrainColliders terrain MeshColliders
//   water            Water renderers
//   lights           Directional lights (always 1: a default sun when the
//                    manifest's light is null)
//   skybox           1 when RenderSettings.skybox was set from the sky
//   foliageTrees     foliage tree prefab instances placed (world-foliage D8):
//                    records whose layer has type 3 or is 0xFFFF and whose
//                    model is ok, shown and kept (FoliagePlan; validate_manifest.py
//                    plan_foliage), not Terrain trees
//   foliagePatches   foliage patch meshes (type 1-2 records not made details)
//   terrains         Unity Terrain tiles (Terrain mode)
//   detailPrototypes detail layers set, summed over the tiles
//   detailCells      non-zero 48-unit detail cells, summed over tiles and layers
//   treeInstances    Terrain TreeInstances (bit-3 records, Terrain mode)
//   lodPrefabs       model prefabs given a LODGroup (Task 15b): models with
//                    lods.levels > 1 whose prefab was saved
//   lodTreePrefabs   foliage tree prefabs of such models (their LODGroup has the
//                    model's levels, less any its cull drops)
//   lodOverrides     prop instances of such models with instances[].lod: their own
//                    LODGroup transition heights
// In Terrain mode the cells' mesh terrain is not built: terrainColliders is 0
// and materials has no terrain cell materials but one shared terrain template.
// No UnityEngine dependency.

using System.Collections.Generic;

namespace Castlemist
{
    public sealed class ImportSummary
    {
        public int materials, prefabs, controllers, cells, instances, colliders, terrainColliders, water, lights, skybox;
        /// Collider_k parts in all cells (not part of the PLAN line).
        public int colliderParts;
        /// Foliage tree instances placed (in the PLAN line).
        public int foliageTrees;
        /// Patches, Terrain tiles, detail layers and cells, Terrain trees (in the PLAN line).
        public int foliagePatches, terrains, detailPrototypes, detailCells, treeInstances;
        /// Task 15b (in the PLAN line).
        public int lodPrefabs, lodTreePrefabs, lodOverrides;
        /// Foliage records not placed (not in the PLAN line): skipped because their
        /// model failed or has no prefab or mesh.
        public int foliageSkipped;
        /// The TERRAIN_CHECK line (the terrain frame check), or null when not run.
        public string terrainCheckLine;
        public bool terrainCheckOk = true;
        public List<string> warnings = new List<string>();
        public List<string> errors = new List<string>();
        /// The frame probe's PROBE line, or null when there was no probe.
        public string probeLine;
        public bool probeOk;
        public double seconds;

        public string PlanLine()
        {
            return "PLAN materials=" + materials + " prefabs=" + prefabs + " controllers=" + controllers +
                   " cells=" + cells + " instances=" + instances + " colliders=" + colliders +
                   " terrainColliders=" + terrainColliders + " water=" + water + " lights=" + lights +
                   " skybox=" + skybox + " foliageTrees=" + foliageTrees + " foliagePatches=" + foliagePatches +
                   " terrains=" + terrains + " detailPrototypes=" + detailPrototypes + " detailCells=" + detailCells +
                   " treeInstances=" + treeInstances + " lodPrefabs=" + lodPrefabs + " lodTreePrefabs=" + lodTreePrefabs +
                   " lodOverrides=" + lodOverrides;
        }
    }
}
