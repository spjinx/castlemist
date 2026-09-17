/// @file
/// @brief Orchestrates a MapScene -> one .glb document (every placed prop, world-transformed).

#include "internal.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;

GltfExportResult export_map_gltf(const MapScene& scene, const std::string& glbPath,
                                 const GltfExportOptions& opts) {
    GltfExportResult result;
    result.glbPath = glbPath;

    if (scene.models.empty() || scene.instances.empty()) {
        result.error = "This map scene has no placed props to export.";
        return result;
    }

    fs::path outPath(glbPath);
    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    GltfWriter w;

    // Mesh, material and texture data is shared per unique model (it doesn't
    // depend on an instance's placement) and referenced by every instance's
    // own node via ordinary glTF node instancing -- unlike the FBX path, a
    // node's arbitrary 4x4 `matrix` means GW2's own placement composition
    // never needs decomposing, so real geometry sharing is safe here.
    struct ModelCache {
        std::vector<MeshExportInfo> meshes;
    };
    std::vector<ModelCache> cache(scene.models.size());
    for (size_t mi = 0; mi < scene.models.size(); ++mi) {
        std::vector<int> texIndices;
        write_model_textures(w, scene.models[mi].textures, texIndices);
        std::vector<int> materialIndices = write_materials(w, scene.models[mi], texIndices);
        cache[mi].meshes = write_meshes(w, scene.models[mi], materialIndices);
    }

    json rootChildren = json::array();
    int instanceCounter = 0;
    for (const MapInstance& inst : scene.instances) {
        if (inst.model < 0 || inst.model >= static_cast<int>(scene.models.size())) continue;
        const ModelPreview& model = scene.models[static_cast<size_t>(inst.model)];
        if (model.meshes.empty()) continue;
        const ModelCache& mc = cache[static_cast<size_t>(inst.model)];

        char nameBuf[32];
        std::snprintf(nameBuf, sizeof(nameBuf), "inst%d", instanceCounter++);
        std::string instName = nameBuf;

        // A skinned/animated prop gets its own skeleton+skin per instance
        // (rare in practice), since sharing skin nodes across instances would
        // force them all into the exact same pose; mesh/material data above
        // is still safely shared regardless.
        std::vector<JointExportInfo> joints;
        int skinIndex = -1;
        if (!model.joints.empty()) {
            joints = write_skeleton(w, model, instName);
            skinIndex = write_skin(w, model, joints);
            write_animations(w, model, joints, opts.animFps);
        }

        json children = json::array();
        for (const MeshExportInfo& mesh : mc.meshes) {
            json node{{"mesh", mesh.meshIndex}};
            if (skinIndex >= 0 && model.meshes[mesh.sourceIndex].hasSkin) node["skin"] = skinIndex;
            children.push_back(w.add_node(std::move(node)));
        }
        for (size_t ji = 0; ji < model.joints.size(); ++ji) {
            if (model.joints[ji].parent < 0) children.push_back(joints[ji].nodeIndex);
        }

        Vec3 pos{inst.pos[0], inst.pos[1], inst.pos[2]};
        Vec3 rot{inst.rot[0], inst.rot[1], inst.rot[2]};
        std::array<float, 16> matrix = flatten_column_major(scene_world(pos, rot, inst.scale));

        int wrapperIndex = w.add_node(json{
            {"name", instName},
            {"matrix", std::vector<float>(matrix.begin(), matrix.end())},
            {"children", children},
        });
        rootChildren.push_back(wrapperIndex);
    }

    // The scene root absorbs GW2's Z-up -> glTF's Y-up conversion; see
    // model_export.cpp / internal.h's design note for why this is a fixed
    // -90-degrees-about-X rotation rather than a per-vertex conversion.
    int rootIndex = w.add_node(json{
        {"name", "GW2_ZupToYup"},
        {"rotation", {-0.70710678, 0.0, 0.0, 0.70710678}},
        {"children", rootChildren},
    });
    w.add_scene_root(rootIndex);

    std::vector<uint8_t> glb = w.finish();
    std::ofstream out(glbPath, std::ios::binary);
    if (!out) {
        result.error = "Could not open '" + glbPath + "' for writing.";
        return result;
    }
    out.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    if (!out) {
        result.error = "Failed writing '" + glbPath + "'.";
        return result;
    }

    result.ok = true;
    return result;
}

} // namespace castlemist::exportgltf
