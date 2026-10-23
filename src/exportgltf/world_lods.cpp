/// @file
/// @brief Model LODs for the world export (world_lods.h).
#include "castlemist/exportgltf/world_lods.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

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
        cloth_wave_weights(mesh, cp.lockCount);
        m.meshes.push_back(std::move(mesh));
        ++added;
    }
    return added;
}

int append_lightning_meshes(ModelPreview& m) {
    int added = 0;
    for (const LightningBoltCPU& b : m.lightning) {
        ModelMeshCPU mesh;
        mesh.materialIndex = b.materialIndex;
        mesh.meshName = "lightning";
        mesh.hasSkin = true;
        mesh.hasColor = true;
        const uint32_t c = b.colorBegin[0];
        const float op = std::clamp(0.5f * (b.opacity[0] + b.opacity[1]), 0.0f, 1.0f);
        const uint8_t col[4] = {static_cast<uint8_t>(c >> 16), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c),
                                static_cast<uint8_t>(std::lround(((c >> 24) & 0xff) * op))};
        const float width = std::max(0.5f * (b.thicknessRange[0] + b.thicknessRange[1]), 0.01f);
        const int segs = std::max<int>(b.numSegments, 1);
        for (const auto& [ja, jb] : b.edges) {
            if (ja < 0 || jb < 0 || static_cast<size_t>(ja) >= m.joints.size() ||
                static_cast<size_t>(jb) >= m.joints.size() || ja == jb)
                continue;
            const float* A = m.joints[static_cast<size_t>(ja)].pos;
            const float* B = m.joints[static_cast<size_t>(jb)].pos;
            float d[3] = {B[0] - A[0], B[1] - A[1], B[2] - A[2]};
            const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (!(len > 1e-4f)) continue;
            for (float& x : d) x /= len;
            // Two strips at right angles around the edge: any axis not along it starts them.
            const float up[3] = {std::fabs(d[2]) < 0.9f ? 0.0f : 1.0f, 0.0f, std::fabs(d[2]) < 0.9f ? 1.0f : 0.0f};
            float s1[3] = {d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0]};
            const float l1 = std::sqrt(s1[0] * s1[0] + s1[1] * s1[1] + s1[2] * s1[2]);
            for (float& x : s1) x /= l1;
            const float s2[3] = {d[1] * s1[2] - d[2] * s1[1], d[2] * s1[0] - d[0] * s1[2], d[0] * s1[1] - d[1] * s1[0]};
            const float uLen = b.texScale > 0 ? len / b.texScale : 1.0f;
            for (const float* side : {static_cast<const float*>(s1), s2}) {
                const float* n = side == s1 ? s2 : s1;
                const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
                for (int i = 0; i <= segs; ++i) {
                    const float t = static_cast<float>(i) / static_cast<float>(segs);
                    for (int e = 0; e < 2; ++e) {
                        GVertex v{};
                        const float o = (e ? 0.5f : -0.5f) * width;
                        v.px = A[0] + d[0] * len * t + side[0] * o;
                        v.py = A[1] + d[1] * len * t + side[1] * o;
                        v.pz = A[2] + d[2] * len * t + side[2] * o;
                        v.nx = n[0]; v.ny = n[1]; v.nz = n[2];
                        v.tx = d[0]; v.ty = d[1]; v.tz = d[2];
                        v.bx = side[0]; v.by = side[1]; v.bz = side[2];
                        v.u = b.texOffset + t * uLen;   // the texture runs along U (3442863's chain links)
                        v.v = b.texRange[e];
                        for (auto& uv : v.uv1) { uv[0] = v.u; uv[1] = v.v; }
                        v.bidx[0] = static_cast<uint32_t>(ja);
                        v.bidx[1] = static_cast<uint32_t>(jb);
                        v.bwt[0] = 1.0f - t;
                        v.bwt[1] = t;
                        v.bwt[2] = v.bwt[3] = 0.0f;
                        std::copy(col, col + 4, v.col);
                        mesh.vertices.push_back(v);
                    }
                }
                for (int i = 0; i < segs; ++i) {
                    const uint32_t q = base + 2u * static_cast<uint32_t>(i);
                    for (uint32_t k : {q, q + 2, q + 1, q + 1, q + 2, q + 3}) mesh.indices.push_back(k);
                }
            }
        }
        if (mesh.indices.empty()) continue;
        mesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
        m.meshes.push_back(std::move(mesh));
        ++added;
    }
    return added;
}

bool cloth_wave_weights(ModelMeshCPU& mesh, uint32_t lockCount) {
    const size_t n = mesh.vertices.size();
    if (lockCount == 0 || lockCount >= n) return false;
    // Edge lengths along the cloth's triangles; the distance from the pinned prefix by Dijkstra.
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(n);
    auto link = [&](uint32_t a, uint32_t b) {
        if (a >= n || b >= n || a == b) return;
        const GVertex &p = mesh.vertices[a], &q = mesh.vertices[b];
        const float d = std::sqrt((p.px - q.px) * (p.px - q.px) + (p.py - q.py) * (p.py - q.py) + (p.pz - q.pz) * (p.pz - q.pz));
        adj[a].push_back({b, d});
        adj[b].push_back({a, d});
    };
    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        link(mesh.indices[i], mesh.indices[i + 1]);
        link(mesh.indices[i + 1], mesh.indices[i + 2]);
        link(mesh.indices[i + 2], mesh.indices[i]);
    }
    std::vector<float> dist(n, std::numeric_limits<float>::infinity());
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    for (uint32_t i = 0; i < lockCount; ++i) { dist[i] = 0; q.push({0.f, i}); }
    while (!q.empty()) {
        const auto [d, v] = q.top();
        q.pop();
        if (d > dist[v]) continue;
        for (const auto& e : adj[v])
            if (d + e.second < dist[e.first]) { dist[e.first] = d + e.second; q.push({dist[e.first], e.first}); }
    }
    float far = 0;
    for (float d : dist) if (std::isfinite(d)) far = std::max(far, d);
    if (!(far > 0)) return false;
    for (size_t i = 0; i < n; ++i) {
        const float w = std::isfinite(dist[i]) ? std::min(1.0f, dist[i] / far) : 0.0f;
        GVertex& v = mesh.vertices[i];
        v.col[0] = static_cast<uint8_t>(std::lround((1.0f - w) * 255.0f));   // white = still
        v.col[1] = v.col[2] = v.col[3] = 255;
    }
    mesh.hasColor = true;
    return true;
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
