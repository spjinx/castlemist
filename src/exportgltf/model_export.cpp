/// @file
/// @brief Orchestrates one ModelPreview -> one .glb document (the single-model export path).

#include "internal.h"
#include "castlemist/exportgltf/world_lods.h"
#include "long_path.h"
#include "sha1.h"

#include <filesystem>
#include <fstream>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

/// Every textureInfo under @p j ("...Texture": {index, texCoord}) with its index
/// replaced by the image URI it samples ("image"), so the key names content
/// (pool files are content- or fileId-named), not this file's numbering.
void resolve_textures(const GltfWriter& w, json& j) {
    if (!j.is_object()) return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        json& v = it.value();
        const std::string& k = it.key();
        if (v.is_object() && k.size() > 7 && k.compare(k.size() - 7, 7, "Texture") == 0 && v.contains("index") &&
            v["index"].is_number_integer()) {
            v["image"] = w.texture_uri(v["index"].get<int>());
            v.erase("index");
        } else if (v.is_object()) {
            resolve_textures(w, v);
        }
    }
}

/// Task 15d: each material's key, SHA-1 of {gltf: the material without its name,
/// textures resolved; vrchat: the caller's input}, written as
/// extras.castlemistMaterialKey. nlohmann::json keeps object keys sorted, so the
/// text is canonical for equal content.
std::vector<std::string> write_material_keys(GltfWriter& w, const std::vector<int>& materialIndices,
                                             const std::vector<std::string>& inputs) {
    std::vector<std::string> keys;
    keys.reserve(materialIndices.size());
    for (size_t i = 0; i < materialIndices.size(); ++i) {
        json g = w.material_json(materialIndices[i]);
        g.erase("name");
        resolve_textures(w, g);
        const std::string key = sha1_hex(json{{"gltf", g}, {"vrchat", inputs[i]}}.dump());
        w.material_json(materialIndices[i])["extras"]["castlemistMaterialKey"] = key;
        keys.push_back(key);
    }
    return keys;
}

} // namespace

GltfExportResult export_model_gltf(const ModelPreview& model, const std::string& glbPath,
                                   const GltfExportOptions& opts) {
    GltfExportResult result;
    result.glbPath = glbPath;

    if (model.meshes.empty()) {
        result.error = "This model has no drawable meshes to export.";
        return result;
    }

    fs::path outPath = fs::path(std::u8string(glbPath.begin(), glbPath.end()));  // glbPath is UTF-8
    std::string stem = outPath.stem().string();

    std::error_code ec;
    make_dirs(outPath.parent_path(), ec);

    GltfWriter w;
    if (opts.generatedImages) w.set_generated_pool(opts.generatedImages.get(), opts.generatedImageUriPrefix);

    std::vector<int> texIndices;
    write_model_textures(w, model.textures, texIndices, opts.imageDirUtf8, opts.imageUriPrefix,
                         opts.sharedImages.get(), opts.sharedImageUriPrefix);
    std::vector<int> materialIndices = write_materials(w, model, texIndices);
    if (!opts.materialKeyInputs.empty() && opts.materialKeyInputs.size() == materialIndices.size())
        result.materialKeys = write_material_keys(w, materialIndices, opts.materialKeyInputs);
    std::vector<MeshExportInfo> meshes =
        write_meshes(w, model, materialIndices, opts.exportLods ? model_lod_levels(model) : 1, opts.uv1CopyToUv4,
                     opts.vertexColors);
    result.particlesJsonPath = write_particle_sidecar(model, materialIndices, glbPath);

    std::vector<JointExportInfo> joints;
    int skinIndex = -1;
    if (!model.joints.empty()) {
        joints = write_skeleton(w, model, stem);
        skinIndex = write_skin(w, model, joints);
        write_animations(w, model, joints, opts.animFps);
    }

    json rootChildren = json::array();
    for (const MeshExportInfo& mesh : meshes) {
        // ModelMeshDataV66.meshName when the file actually named this submesh
        // (see ModelMeshCPU::meshName's doc comment) -- still suffixed with
        // the source index, since several submeshes commonly share one name
        // (this exact model: most of them are just "airship") and glTF node
        // names aren't required to be unique but Blender's Outliner is much
        // more useful when they are.
        const std::string nodeName = mesh_node_name(model, mesh.sourceIndex, stem);
        json node{{"name", nodeName}, {"mesh", mesh.meshIndex}};
        if (skinIndex >= 0 && model.meshes[mesh.sourceIndex].hasSkin) node["skin"] = skinIndex;
        rootChildren.push_back(w.add_node(std::move(node)));
    }
    // World export LODs: "<LOD0 node name>_LOD<k>", after every LOD0 node, only for
    // the levels a mesh has its own index set for (world_lods.h).
    for (const MeshExportInfo& mesh : meshes) {
        if (mesh.lods.empty()) continue;
        const std::string base = mesh_node_name(model, mesh.sourceIndex, stem);
        for (const auto& [level, meshIndex] : mesh.lods) {
            json node{{"name", base + "_LOD" + std::to_string(level)}, {"mesh", meshIndex}};
            if (skinIndex >= 0 && model.meshes[mesh.sourceIndex].hasSkin) node["skin"] = skinIndex;
            rootChildren.push_back(w.add_node(std::move(node)));
        }
    }
    for (size_t i = 0; i < model.joints.size(); ++i) {
        if (model.joints[i].parent < 0) rootChildren.push_back(joints[i].nodeIndex);
    }

    // The scene root absorbs GW2's Z-up -> glTF's Y-up conversion (a fixed
    // -90-degrees-about-X rotation) so every mesh vertex and joint transform
    // below it stays in GW2's own native numbers -- see internal.h's design note.
    // A world model file takes map_to_gltf_root instead (a reflection, so a
    // matrix rather than a quaternion): its data is then in map space.
    json root;
    if (opts.rootMatrix) {
        root = json{
            {"name", "GW2_MapToGltf"},
            {"matrix", std::vector<float>(opts.rootMatrix->begin(), opts.rootMatrix->end())},
            {"children", rootChildren},
        };
    } else {
        root = json{
            {"name", "GW2_ZupToYup"},
            {"rotation", {opts.rootRotation[0], opts.rootRotation[1], opts.rootRotation[2], opts.rootRotation[3]}},
            {"children", rootChildren},
        };
        if (opts.rootScale != 1.0) root["scale"] = {opts.rootScale, opts.rootScale, opts.rootScale};
    }
    int rootIndex = w.add_node(std::move(root));
    w.add_scene_root(rootIndex);

    if (!w.image_error().empty()) {
        result.error = w.image_error();
        return result;
    }

    std::vector<uint8_t> glb = w.finish();
    std::ofstream out(long_path(outPath), std::ios::binary);
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
