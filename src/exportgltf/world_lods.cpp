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

bool mesh_is_lod_proxy(const ModelMeshCPU& m) {
    return (m.meshFlags & (kMeshFlagLodProxy | kMeshFlagLodProxy2)) != 0;
}

int append_cloth_meshes(ModelPreview& m) {
    int added = 0;
    for (const ClothPieceCPU& cp : m.clothPieces) {
        if (cp.vertices.empty() || cp.indices.size() < 3) continue;
        ModelMeshCPU mesh;
        mesh.vertices = cp.vertices;
        mesh.indices = cp.indices;
        mesh.indices.resize(mesh.indices.size() / 3 * 3);
        mesh.materialIndex = cp.materialIndex;
        mesh.vertexCount = static_cast<uint32_t>(cp.vertices.size());
        mesh.meshName = "cloth";
        long agree = 0;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const GVertex* t[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (mesh.indices[i + k] >= mesh.vertices.size()) { ok = false; break; }
                t[k] = &mesh.vertices[mesh.indices[i + k]];
            }
            if (!ok) continue;
            const float ax = t[1]->px - t[0]->px, ay = t[1]->py - t[0]->py, az = t[1]->pz - t[0]->pz;
            const float bx = t[2]->px - t[0]->px, by = t[2]->py - t[0]->py, bz = t[2]->pz - t[0]->pz;
            const float gx = ay * bz - az * by, gy = az * bx - ax * bz, gz = ax * by - ay * bx;
            const float nx = t[0]->nx + t[1]->nx + t[2]->nx, ny = t[0]->ny + t[1]->ny + t[2]->ny,
                        nz = t[0]->nz + t[1]->nz + t[2]->nz;
            agree += (gx * nx + gy * ny + gz * nz) >= 0 ? 1 : -1;
        }
        if (agree < 0)
            for (GVertex& v : mesh.vertices) {
                v.nx = -v.nx; v.ny = -v.ny; v.nz = -v.nz;
                v.bx = -v.bx; v.by = -v.by; v.bz = -v.bz;
            }
        m.meshes.push_back(std::move(mesh));
        ++added;
    }
    return added;
}

std::string mesh_node_name(const ModelPreview& m, size_t i, const std::string& stem) {
    const std::string& meshName = m.meshes[i].meshName;
    return !meshName.empty() ? meshName + "_" + std::to_string(i) : stem + "_mesh" + std::to_string(i);
}

int mesh_lod_source(const ModelMeshCPU& m, int level) {
    for (int j = std::min<int>(level, static_cast<int>(m.lodIndices.size())); j >= 1; --j)
        if (m.lodIndices[size_t(j - 1)].size() >= 3) return j - 1;
    return -1;
}

int model_lod_levels(const ModelPreview& m) {
    int levels = 1;
    for (const ModelMeshCPU& mesh : m.meshes) {
        if (!mesh_drawable(mesh) || mesh_is_lod_proxy(mesh)) continue;
        for (int j = 1; j < kMaxLodLevels && j <= static_cast<int>(mesh.lodIndices.size()); ++j)
            if (mesh.lodIndices[size_t(j - 1)].size() >= 3) levels = std::max(levels, j + 1);
    }
    return levels;
}

std::vector<uint64_t> model_lod_triangles(const ModelPreview& m, int levels) {
    std::vector<uint64_t> t(size_t(std::max(levels, 1)), 0);
    for (const ModelMeshCPU& mesh : m.meshes) {
        if (!mesh_drawable(mesh) || mesh_is_lod_proxy(mesh)) continue;
        for (int k = 0; k < static_cast<int>(t.size()); ++k) {
            const int s = mesh_lod_source(mesh, k);
            t[size_t(k)] += (s < 0 ? mesh.indices.size() : mesh.lodIndices[size_t(s)].size()) / 3;
        }
    }
    return t;
}

json model_lods_json(const ModelPreview& m, const std::string& stem) {
    float d[2];
    bool file[2];
    model_lod_distances(m.lodOverride, d, file);
    const int levels = model_lod_levels(m);
    bool anyFlag4 = false;
    for (const ModelMeshCPU& mesh : m.meshes)
        anyFlag4 = anyFlag4 || (mesh_drawable(mesh) && (mesh.meshFlags & kMeshFlagLodProxy));
    json far = json::array(), hidden = json::array();
    uint64_t farTriangles = 0;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
        if (!mesh_drawable(m.meshes[i]) || !mesh_is_lod_proxy(m.meshes[i])) continue;
        if (anyFlag4 && !(m.meshes[i].meshFlags & kMeshFlagLodProxy)) {
            hidden.push_back(mesh_node_name(m, i, stem));
            continue;
        }
        far.push_back(mesh_node_name(m, i, stem));
        farTriangles += m.meshes[i].indices.size() / 3;
    }
    return {{"levels", levels},
            {"distances", {d[0], d[1]}},
            {"distanceSource", {file[0] ? "lodOverride" : "default", file[1] ? "lodOverride" : "default"}},
            {"lodOverride", m.hasLodOverride && std::isfinite(m.lodOverride[0]) && std::isfinite(m.lodOverride[1]) ? json{m.lodOverride[0], m.lodOverride[1]} : json(nullptr)},
            {"triangles", model_lod_triangles(m, levels)},
            {"farMeshes", std::move(far)},
            {"farTriangles", farTriangles},
            {"hiddenMeshes", std::move(hidden)}};
}

} // namespace castlemist::exportgltf
