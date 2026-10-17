/// @file
/// @brief Model LODs for the world export (world_lods.h).
#include "castlemist/exportgltf/world_lods.h"

#include <algorithm>
#include <cmath>

namespace castlemist::exportgltf {

using nlohmann::json;

void model_lod_distances(const float lodOverride[2], float out[2], bool fromFile[2]) {
    for (int k = 0; k < 2; ++k) {
        const float v = lodOverride ? lodOverride[k] : 0.0f;
        fromFile[k] = std::isfinite(v) && v > 0.0f;
        out[k] = fromFile[k] ? std::min(v, kLodMaxDistance) : kLodDefaultDistance[k];
    }
}

bool prop_lod_override(float lod1, float lod2, uint32_t flags, float out[2]) {
    if (!(flags & kPropFlagOverrideLod)) return false;
    if (!std::isfinite(lod1) || !std::isfinite(lod2)) return false;
    if (lod1 == 0.0f && lod2 == 0.0f) return false;
    const float in[2] = {lod1, lod2};
    for (int k = 0; k < 2; ++k) out[k] = std::min(std::max(in[k], 0.0f), kLodMaxDistance);
    return true;
}

bool mesh_drawable(const ModelMeshCPU& m) { return !m.vertices.empty() && m.indices.size() >= 3; }

int mesh_lod_source(const ModelMeshCPU& m, int level) {
    for (int j = std::min<int>(level, static_cast<int>(m.lodIndices.size())); j >= 1; --j)
        if (m.lodIndices[size_t(j - 1)].size() >= 3) return j - 1;
    return -1;
}

int model_lod_levels(const ModelPreview& m) {
    int levels = 1;
    for (const ModelMeshCPU& mesh : m.meshes) {
        if (!mesh_drawable(mesh)) continue;
        for (int j = 1; j < kMaxLodLevels && j <= static_cast<int>(mesh.lodIndices.size()); ++j)
            if (mesh.lodIndices[size_t(j - 1)].size() >= 3) levels = std::max(levels, j + 1);
    }
    return levels;
}

std::vector<uint64_t> model_lod_triangles(const ModelPreview& m, int levels) {
    std::vector<uint64_t> t(size_t(std::max(levels, 1)), 0);
    for (const ModelMeshCPU& mesh : m.meshes) {
        if (!mesh_drawable(mesh)) continue;
        for (int k = 0; k < static_cast<int>(t.size()); ++k) {
            const int s = mesh_lod_source(mesh, k);
            t[size_t(k)] += (s < 0 ? mesh.indices.size() : mesh.lodIndices[size_t(s)].size()) / 3;
        }
    }
    return t;
}

json model_lods_json(const ModelPreview& m) {
    float d[2];
    bool file[2];
    model_lod_distances(m.lodOverride, d, file);
    const int levels = model_lod_levels(m);
    return {{"levels", levels},
            {"distances", {d[0], d[1]}},
            {"distanceSource", {file[0] ? "lodOverride" : "default", file[1] ? "lodOverride" : "default"}},
            {"lodOverride", m.hasLodOverride && std::isfinite(m.lodOverride[0]) && std::isfinite(m.lodOverride[1]) ? json{m.lodOverride[0], m.lodOverride[1]} : json(nullptr)},
            {"triangles", model_lod_triangles(m, levels)}};
}

} // namespace castlemist::exportgltf
