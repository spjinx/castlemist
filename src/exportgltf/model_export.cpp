/// @file
/// @brief Orchestrates one ModelPreview -> one .glb document (the single-model export path).

#include "internal.h"

#include <filesystem>
#include <fstream>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;

GltfExportResult export_model_gltf(const ModelPreview& model, const std::string& glbPath,
                                   const GltfExportOptions& opts) {
    GltfExportResult result;
    result.glbPath = glbPath;

    if (model.meshes.empty()) {
        result.error = "This model has no drawable meshes to export.";
        return result;
    }

    fs::path outPath(glbPath);
    std::string stem = outPath.stem().string();

    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);

    GltfWriter w;

    std::vector<int> texIndices;
    write_model_textures(w, model.textures, texIndices);
    std::vector<int> materialIndices = write_materials(w, model, texIndices);
    std::vector<MeshExportInfo> meshes = write_meshes(w, model, materialIndices);

    std::vector<JointExportInfo> joints;
    int skinIndex = -1;
    if (!model.joints.empty()) {
        joints = write_skeleton(w, model, stem);
        skinIndex = write_skin(w, model, joints);
        write_animations(w, model, joints, opts.animFps);
    }

    json rootChildren = json::array();
    for (const MeshExportInfo& mesh : meshes) {
        json node{{"name", stem + "_mesh" + std::to_string(mesh.sourceIndex)}, {"mesh", mesh.meshIndex}};
        if (skinIndex >= 0 && model.meshes[mesh.sourceIndex].hasSkin) node["skin"] = skinIndex;
        rootChildren.push_back(w.add_node(std::move(node)));
    }
    for (size_t i = 0; i < model.joints.size(); ++i) {
        if (model.joints[i].parent < 0) rootChildren.push_back(joints[i].nodeIndex);
    }

    // The scene root absorbs GW2's Z-up -> glTF's Y-up conversion (a fixed
    // -90-degrees-about-X rotation) so every mesh vertex and joint transform
    // below it stays in GW2's own native numbers -- see internal.h's design note.
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
