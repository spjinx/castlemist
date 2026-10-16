// castlemist world export: what one Unity import built (spec D6 step 10).
//
// PlanLine() has the format of tools/world/validate_manifest.py --plan, and
// each count follows the same rule, so the two lines can be compared as text
// (CHECKLIST item 1):
//   materials        model materials built (usedByMeshes != false) + one per
//                    terrain cell + one per water cell
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
// No UnityEngine dependency.

using System.Collections.Generic;

namespace Castlemist
{
    public sealed class ImportSummary
    {
        public int materials, prefabs, controllers, cells, instances, colliders, terrainColliders, water, lights, skybox;
        /// Collider_k parts in all cells (not part of the PLAN line).
        public int colliderParts;
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
                   " skybox=" + skybox;
        }
    }
}
