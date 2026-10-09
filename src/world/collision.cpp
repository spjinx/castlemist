/// @file
/// @brief Collision meshes and placements in map space (docs/research/gw2-world-frame.md §7).
///
/// The placement rule (placement -> geometries[g].animations[last] ->
/// collisionIndices[] -> hulls, scale 32 * scale) follows spjinx/t3d
/// HavokRenderer.ts; §7 records what of it the data proves.

#include "castlemist/world/collision.h"

#include "castlemist/world/props.h"

#include <map>
#include <string>

namespace castlemist::world {

void build_collision(const castlemist::model::Extractor::MapHavok& h, WorldScene& out) {
    out.collision = {};

    // Meshes: one per hull, same index. Triangles that index past their hull
    // are dropped (T3D drops them too, HavokRenderer.ts:298-316).
    size_t droppedFaces = 0;
    out.collision.meshes.resize(h.hulls.size());
    for (size_t i = 0; i < h.hulls.size(); ++i) {
        const auto& hull = h.hulls[i];
        CollisionMesh& m = out.collision.meshes[i];
        m.verts = hull.verts;
        const size_t nv = hull.verts.size() / 3;
        m.indices.reserve(hull.indices.size());
        for (size_t k = 0; k + 2 < hull.indices.size(); k += 3) {
            const uint32_t a = hull.indices[k], b = hull.indices[k + 1], c = hull.indices[k + 2];
            if (a < nv && b < nv && c < nv) m.indices.insert(m.indices.end(), {a, b, c});
            else ++droppedFaces;
        }
    }

    // Instances: obs, prop, zone placements as h.placements orders them.
    size_t badPlacements = 0;
    std::map<std::string, uint32_t> indexInGroup;
    for (const auto& p : h.placements) {
        const uint32_t placement = indexInGroup[p.group]++;
        if (p.geometryIndex >= h.geometryAnimations.size()) { ++badPlacements; continue; }
        const auto& anims = h.geometryAnimations[p.geometryIndex];
        if (anims.empty()) continue;            // a geometry with no animation has no hull (§7.1)
        const uint32_t anim = anims.back();     // animations[last] (§7.1)
        if (anim >= h.animationCollisions.size()) { ++badPlacements; continue; }

        // §7.2-7.3: the client's placement transform at 32 * scale, then the
        // hull's own z (stored up = +Z) flipped into map space (up = -Z).
        float world[16];
        client_world_matrix(p.translate, p.rotate, 32.0f * p.scale, world);
        world[8] = -world[8]; world[9] = -world[9]; world[10] = -world[10];

        bool bad = false;
        for (uint32_t ci : h.animationCollisions[anim]) {
            if (ci >= h.hulls.size()) { bad = true; continue; }
            CollisionInstance inst;
            inst.mesh = ci;
            for (int k = 0; k < 16; ++k) inst.world[k] = world[k];
            inst.group = p.group;
            inst.placement = placement;
            out.collision.instances.push_back(std::move(inst));
        }
        if (bad) ++badPlacements;
    }

    if (badPlacements)
        out.warnings.push_back(std::to_string(badPlacements) +
                               " collision placements reference a geometry, animation or collision index out of "
                               "range; those references were skipped");
    if (droppedFaces)
        out.warnings.push_back(std::to_string(droppedFaces) +
                               " collision triangles index past their hull's vertices; dropped");
}

} // namespace castlemist::world
