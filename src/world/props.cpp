/// @file
/// @brief Prop models and instances in map space (docs/research/gw2-world-frame.md §5).

#include "castlemist/world/props.h"

#include <cmath>
#include <unordered_map>

namespace castlemist::world {

/// The client's prop world transform, as src/render/detail/math.h:160-189
/// documents it (Gw2-64.exe's leaf helper behind PrContext_LoadPropModel): a
/// float3x4 of three rows [r0 r1 r2 | t] for column vectors, p' = M*p + t.
/// With cx = cos(rot[0]), sx = sin(rot[0]), cy/sy = rot[1], cz/sz = rot[2]:
///
///   [ cz*cy - sy*sx*sz   cz*sx*sy + sz*cy   -cx*sy ]
///   [ -cx*sz             cz*cx               sx    ]
///   [ cy*sx*sz + cz*sy   sz*sy - cz*cy*sx    cy*cx ]
///
/// Ported here (world does not depend on render). Scale multiplies the 3x3.
/// Stored column-major: element (row r, column c) is w[c * 4 + r].
void client_world_matrix(const float pos[3], const float rot[3], float scale, float w[16]) {
    const float cx = std::cos(rot[0]), sx = std::sin(rot[0]);
    const float cy = std::cos(rot[1]), sy = std::sin(rot[1]);
    const float cz = std::cos(rot[2]), sz = std::sin(rot[2]);
    const float m[3][3] = {
        {cz * cy - sy * sx * sz, cz * sx * sy + sz * cy, -cx * sy},
        {-cx * sz, cz * cx, sx},
        {cy * sx * sz + cz * sy, sz * sy - cz * cy * sx, cy * cx}};
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) w[c * 4 + r] = scale * m[r][c];
    w[3] = w[7] = w[11] = 0.0f;
    w[12] = pos[0]; w[13] = pos[1]; w[14] = pos[2]; w[15] = 1.0f;
}

void build_props(const std::vector<castlemist::model::Extractor::MapProp>& in, WorldScene& out) {
    out.models.clear();
    out.props.clear();
    out.motion.animatedProps.clear();
    out.props.reserve(in.size());

    std::unordered_map<uint32_t, uint32_t> modelIndex;
    for (const auto& p : in) {
        auto [it, added] = modelIndex.try_emplace(p.fileId, (uint32_t)out.models.size());
        if (added) out.models.push_back(PropModel{p.fileId});

        PropInstance inst;
        inst.model = it->second;
        for (int k = 0; k < 3; ++k) { inst.pos[k] = p.pos[k]; inst.rot[k] = p.rot[k]; }
        inst.scale = p.scale;
        inst.group = p.group;
        client_world_matrix(p.pos, p.rot, p.scale, inst.world);
        if (p.group == "propAnimArray") out.motion.animatedProps.push_back((uint32_t)out.props.size());
        out.props.push_back(std::move(inst));
    }
}

} // namespace castlemist::world
