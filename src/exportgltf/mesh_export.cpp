/// @file
/// @brief ModelMeshCPU -> glTF mesh (one primitive: positions/normals/UV,
///        optional tangents and skin attributes, and an index buffer).

#include "internal.h"

#include <algorithm>
#include <limits>

namespace castlemist::exportgltf {

using nlohmann::json;

namespace {

constexpr int kFloat = 5126;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kArrayBuffer = 34962;
constexpr int kElementArrayBuffer = 34963;

} // namespace

std::vector<MeshExportInfo> write_meshes(GltfWriter& w, const ModelPreview& model,
                                         const std::vector<int>& materialIndices) {
    std::vector<MeshExportInfo> out;
    out.reserve(model.meshes.size());

    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        const ModelMeshCPU& mesh = model.meshes[mi];
        if (mesh.vertices.empty() || mesh.indices.size() < 3) continue;

        size_t n = mesh.vertices.size();
        std::vector<float> positions, normals, uvs, tangents;
        std::vector<uint16_t> joints;
        std::vector<float> weights;
        positions.reserve(n * 3);
        normals.reserve(n * 3);
        uvs.reserve(n * 2);

        float minP[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
        float maxP[3] = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};

        for (const GVertex& v : mesh.vertices) {
            positions.push_back(v.px); positions.push_back(v.py); positions.push_back(v.pz);
            minP[0] = std::min(minP[0], v.px); minP[1] = std::min(minP[1], v.py); minP[2] = std::min(minP[2], v.pz);
            maxP[0] = std::max(maxP[0], v.px); maxP[1] = std::max(maxP[1], v.py); maxP[2] = std::max(maxP[2], v.pz);

            normals.push_back(v.nx); normals.push_back(v.ny); normals.push_back(v.nz);
            uvs.push_back(v.u); uvs.push_back(v.v);

            if (mesh.hasTangents) {
                // glTF TANGENT is a vec4: xyz plus a handedness sign for the
                // bitangent, derived from GW2's own explicit bitangent rather
                // than assumed, since a UV-mirrored region flips it.
                float cx = v.ny * v.tz - v.nz * v.ty;
                float cy = v.nz * v.tx - v.nx * v.tz;
                float cz = v.nx * v.ty - v.ny * v.tx;
                float dot = cx * v.bx + cy * v.by + cz * v.bz;
                tangents.push_back(v.tx); tangents.push_back(v.ty); tangents.push_back(v.tz);
                tangents.push_back(dot < 0.0f ? -1.0f : 1.0f);
            }
            if (mesh.hasSkin) {
                for (int k = 0; k < 4; ++k) joints.push_back(static_cast<uint16_t>(v.bidx[k]));
                for (int k = 0; k < 4; ++k) weights.push_back(v.bwt[k]);
            }
        }

        json minJson = {minP[0], minP[1], minP[2]};
        json maxJson = {maxP[0], maxP[1], maxP[2]};
        int posAcc = w.add_accessor(positions.data(), positions.size() * sizeof(float), kFloat, "VEC3", n,
                                    kArrayBuffer, &minJson, &maxJson);
        int normAcc = w.add_accessor(normals.data(), normals.size() * sizeof(float), kFloat, "VEC3", n, kArrayBuffer);
        int uvAcc = w.add_accessor(uvs.data(), uvs.size() * sizeof(float), kFloat, "VEC2", n, kArrayBuffer);

        json attributes{{"POSITION", posAcc}, {"NORMAL", normAcc}, {"TEXCOORD_0", uvAcc}};

        if (mesh.hasTangents) {
            int tanAcc = w.add_accessor(tangents.data(), tangents.size() * sizeof(float), kFloat, "VEC4", n, kArrayBuffer);
            attributes["TANGENT"] = tanAcc;
        }
        if (mesh.hasSkin) {
            int jointsAcc = w.add_accessor(joints.data(), joints.size() * sizeof(uint16_t), kUnsignedShort, "VEC4",
                                           n, kArrayBuffer);
            int weightsAcc = w.add_accessor(weights.data(), weights.size() * sizeof(float), kFloat, "VEC4", n,
                                            kArrayBuffer);
            attributes["JOINTS_0"] = jointsAcc;
            attributes["WEIGHTS_0"] = weightsAcc;
        }

        std::vector<uint32_t> indices32(mesh.indices.begin(), mesh.indices.end());
        int indexAcc = w.add_accessor(indices32.data(), indices32.size() * sizeof(uint32_t), kUnsignedInt, "SCALAR",
                                      indices32.size(), kElementArrayBuffer);

        json primitive{{"attributes", attributes}, {"indices", indexAcc}};
        if (mesh.materialIndex < materialIndices.size()) {
            primitive["material"] = materialIndices[mesh.materialIndex];
        }

        int meshIdx = w.add_mesh(json{{"primitives", json::array({primitive})}});

        MeshExportInfo info;
        info.meshIndex = meshIdx;
        info.materialIndex = mesh.materialIndex;
        info.sourceIndex = mi;
        out.push_back(info);
    }
    return out;
}

} // namespace castlemist::exportgltf
