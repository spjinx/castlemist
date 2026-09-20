/// @file
/// @brief Structural tests for the binary glTF (.glb) exporter.
///
/// No Blender/Unity is available in CI, so these check structure rather than
/// pixel-perfect output. Unlike the earlier FBX writer's tests, this one can
/// lean on nlohmann::json (already vendored and used by the exporter itself)
/// to parse the JSON chunk, so there's no need for a hand-rolled tree walker --
/// the .glb container framing (12-byte header + two length-prefixed chunks) is
/// the only binary structure this file parses itself.

#include "test_framework.h"

#include "internal.h"

#include "castlemist/native/granny_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace castlemist::exportgltf;
using nlohmann::json;

namespace {

struct ParsedGlb {
    json doc;
    std::vector<uint8_t> bin;
};

std::vector<uint8_t> read_bytes(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t read_u32(const std::vector<uint8_t>& b, size_t pos) {
    return static_cast<uint32_t>(b[pos]) | (static_cast<uint32_t>(b[pos + 1]) << 8) |
           (static_cast<uint32_t>(b[pos + 2]) << 16) | (static_cast<uint32_t>(b[pos + 3]) << 24);
}

ParsedGlb parse_glb(const std::vector<uint8_t>& bytes) {
    ParsedGlb out;
    if (bytes.size() < 12) throw std::runtime_error("file too small to be a .glb");
    if (read_u32(bytes, 0) != 0x46546C67u) throw std::runtime_error("bad glTF magic");
    if (read_u32(bytes, 4) != 2u) throw std::runtime_error("expected glTF version 2");
    uint32_t totalLen = read_u32(bytes, 8);
    if (totalLen != bytes.size()) throw std::runtime_error("header length does not match file size");

    size_t pos = 12;
    bool sawJson = false;
    while (pos + 8 <= bytes.size()) {
        uint32_t chunkLen = read_u32(bytes, pos);
        uint32_t chunkType = read_u32(bytes, pos + 4);
        size_t dataStart = pos + 8;
        if (dataStart + chunkLen > bytes.size()) throw std::runtime_error("chunk runs past end of file");
        if (chunkType == 0x4E4F534Au) { // "JSON"
            std::string text(reinterpret_cast<const char*>(&bytes[dataStart]), chunkLen);
            out.doc = json::parse(text);
            sawJson = true;
        } else if (chunkType == 0x004E4942u) { // "BIN\0"
            out.bin.assign(bytes.begin() + static_cast<std::ptrdiff_t>(dataStart),
                           bytes.begin() + static_cast<std::ptrdiff_t>(dataStart + chunkLen));
        }
        pos = dataStart + chunkLen;
    }
    if (!sawJson) throw std::runtime_error("no JSON chunk found");
    return out;
}

// Reads an accessor's raw values as doubles, upcasting whatever componentType
// it declares (FLOAT/UNSIGNED_SHORT/UNSIGNED_INT), for easy checking.
std::vector<double> accessor_values(const ParsedGlb& g, int accessorIndex) {
    const json& acc = g.doc["accessors"][static_cast<size_t>(accessorIndex)];
    const json& bv = g.doc["bufferViews"][acc["bufferView"].get<size_t>()];
    size_t byteOffset = bv.value("byteOffset", 0u);
    size_t count = acc["count"].get<size_t>();
    int componentType = acc["componentType"].get<int>();
    static const std::map<std::string, int> kComponents{
        {"SCALAR", 1}, {"VEC2", 2}, {"VEC3", 3}, {"VEC4", 4}, {"MAT4", 16}};
    int comps = kComponents.at(acc["type"].get<std::string>());

    std::vector<double> out;
    out.reserve(count * static_cast<size_t>(comps));
    const uint8_t* base = g.bin.data() + byteOffset;
    for (size_t i = 0; i < count * static_cast<size_t>(comps); ++i) {
        if (componentType == 5126) { // FLOAT
            float v; std::memcpy(&v, base + i * 4, 4); out.push_back(v);
        } else if (componentType == 5123) { // UNSIGNED_SHORT
            uint16_t v; std::memcpy(&v, base + i * 2, 2); out.push_back(v);
        } else if (componentType == 5125) { // UNSIGNED_INT
            uint32_t v; std::memcpy(&v, base + i * 4, 4); out.push_back(v);
        } else {
            throw std::runtime_error("unhandled componentType in test reader");
        }
    }
    return out;
}

bool starts_with_png_signature(const uint8_t* p, size_t n) {
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (n < 8) return false;
    for (int i = 0; i < 8; ++i)
        if (p[i] != sig[i]) return false;
    return true;
}

fs::path make_temp_dir(const char* label) {
    fs::path dir = fs::temp_directory_path() / (std::string("cm_exportgltf_") + label);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

GVertex make_vertex(float x, float y, float z, float u, float v) {
    GVertex g{};
    g.px = x; g.py = y; g.pz = z;
    g.nx = 0; g.ny = 0; g.nz = 1;
    g.tx = 1; g.ty = 0; g.tz = 0;
    g.bx = 0; g.by = 1; g.bz = 0;
    g.u = u; g.v = v;
    return g;
}

/// @brief A one-quad, one-material, one-texture model -- no skeleton.
ModelPreview make_static_quad() {
    ModelPreview mp;

    ModelMeshCPU mesh;
    mesh.vertices = {make_vertex(0, 0, 0, 0, 0), make_vertex(1, 0, 0, 1, 0),
                     make_vertex(1, 1, 0, 1, 1), make_vertex(0, 1, 0, 0, 1)};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    mesh.vertexCount = 4;
    mesh.hasTangents = true;
    mesh.materialIndex = 0;
    mp.meshes.push_back(mesh);

    ModelTextureCPU tex;
    tex.fileId = 42;
    tex.width = 2;
    tex.height = 2;
    tex.rgba.assign(2 * 2 * 4, 200);
    mp.textures.push_back(tex);

    ModelMaterialCPU mat;
    mat.index = 0;
    mat.diffuseTex = 0;
    mp.materials.push_back(mat);

    return mp;
}

/// @brief The same quad, but as an additive "effect" (glow) material.
ModelPreview make_effect_quad() {
    ModelPreview mp = make_static_quad();
    mp.materials[0].isEffect = true;
    return mp;
}

/// @brief The same quad, rigidly split across two joints and animated.
ModelPreview make_skinned_quad() {
    ModelPreview mp = make_static_quad();
    mp.meshes[0].hasSkin = true;
    mp.meshes[0].vertices[0].bidx[0] = 0; mp.meshes[0].vertices[0].bwt[0] = 1.0f;
    mp.meshes[0].vertices[1].bidx[0] = 0; mp.meshes[0].vertices[1].bwt[0] = 1.0f;
    mp.meshes[0].vertices[2].bidx[0] = 1; mp.meshes[0].vertices[2].bwt[0] = 1.0f;
    mp.meshes[0].vertices[3].bidx[0] = 1; mp.meshes[0].vertices[3].bwt[0] = 1.0f;

    auto identity16 = [](float* out) {
        static const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        std::copy(std::begin(id), std::end(id), out);
    };

    ModelJoint root;
    root.name = "root";
    root.parent = -1;
    root.localQuat[3] = 1;
    root.localScale[0] = root.localScale[4] = root.localScale[8] = 1;
    identity16(root.invWorld);

    ModelJoint child;
    child.name = "child";
    child.parent = 0;
    child.localPos[0] = 1.0f;
    child.localQuat[3] = 1;
    child.localScale[0] = child.localScale[4] = child.localScale[8] = 1;
    identity16(child.invWorld);
    child.invWorld[12] = -1.0f; // model->bone translation row: bind world was translate(+1,0,0)

    mp.joints = {root, child};

    castlemist::granny::Anim clip;
    clip.valid = true;
    clip.name = "TestClip";
    clip.duration = 1.0f;

    castlemist::granny::Track track;
    track.name = "child";
    track.pos.fmt = castlemist::granny::F_K32fC32f;
    track.pos.dim = 3;
    track.pos.degree = 0;
    track.pos.knots = {0.0f, 1.0f};
    track.pos.controls = {1, 0, 0, 2, 0, 0}; // (1,0,0) at t=0 -> (2,0,0) at t=duration
    clip.tracks.push_back(track);

    mp.animClips.push_back(clip);
    mp.hasAnimation = true;
    return mp;
}

/// @brief The static quad plus one baked particle cloud with two emitters --
///        one a normal additive-colour emitter, one shaped like GW2's
///        refraction/heat-haze "flat normal as colour" encoding -- and one
///        effect light, so the sidecar writer has something of everything to
///        serialize.
ModelPreview make_quad_with_effects() {
    ModelPreview mp = make_static_quad();

    ParticleEmitterCPU visible;
    visible.spawnShape = 4;
    visible.spawnPeriod = 0.1f;
    visible.lifetime[0] = 1.5f;
    visible.colorBegin[0][0] = 1.0f; visible.colorBegin[0][1] = 0.6f; visible.colorBegin[0][2] = 0.1f;
    visible.colorBegin[0][3] = 1.0f;
    visible.texCoordRect[0] = 0.5f; visible.texCoordRect[1] = 0.0f;
    visible.texCoordRect[2] = 1.0f; visible.texCoordRect[3] = 0.5f;
    visible.flipbook.present = true;
    visible.flipbook.columns = 2; visible.flipbook.rows = 2; visible.flipbook.count = 4; visible.flipbook.fps = 12;
    visible.opacityCurve = {{0.0f, 0.0f}, {0.2f, 1.0f}, {1.0f, 0.0f}};

    ParticleEmitterCPU distortion;
    distortion.colorBegin[0][0] = 0.0f; distortion.colorBegin[0][1] = 0.0f; distortion.colorBegin[0][2] = 1.0f;
    distortion.colorEnd[0][0] = 1.0f; distortion.colorEnd[0][1] = 0.0f;

    mp.emitters = {visible, distortion};

    ParticleCloudCPU cloud;
    cloud.materialIndex = 0;
    cloud.velocity[1] = 2.0f;
    cloud.emitterIndices = {0, 1};
    mp.clouds = {cloud};

    EffectLightCPU light;
    light.color[0] = 1.0f; light.color[1] = 0.8f; light.color[2] = 0.4f;
    light.intensity = 2.0f;
    mp.effectLights = {light};

    return mp;
}

} // namespace

CM_TEST(exportgltf, particle_sidecar_written_for_model_with_effects) {
    ModelPreview model = make_quad_with_effects();
    fs::path dir = make_temp_dir("particles");
    fs::path glbPath = dir / "fx.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);
    CHECK(!result.particlesJsonPath.empty());
    CHECK(fs::exists(result.particlesJsonPath));

    std::ifstream in(result.particlesJsonPath);
    json doc = json::parse(in);

    CHECK_EQ(doc["clouds"].size(), size_t(1));
    const json& cloud = doc["clouds"][0];
    CHECK(cloud.contains("gltfMaterial"));
    CHECK_EQ(cloud["gltfMaterial"].get<int>(), 0); // the quad's one material
    CHECK_NEAR(cloud["velocity"][1].get<double>(), 2.0, 1e-9);
    CHECK_EQ(cloud["emitters"].size(), size_t(2));

    const json& visible = cloud["emitters"][0];
    CHECK_EQ(visible["isDistortion"].get<bool>(), false);
    CHECK_NEAR(visible["texCoordRect"][0].get<double>(), 0.5, 1e-9);
    CHECK_EQ(visible["flipbook"]["count"].get<int>(), 4);
    CHECK_EQ(visible["opacityCurve"].size(), size_t(3));

    const json& distortion = cloud["emitters"][1];
    CHECK_EQ(distortion["isDistortion"].get<bool>(), true);

    CHECK_EQ(doc["effectLights"].size(), size_t(1));
    CHECK_NEAR(doc["effectLights"][0]["intensity"].get<double>(), 2.0, 1e-9);
}

CM_TEST(exportgltf, no_particle_sidecar_without_baked_effects) {
    ModelPreview model = make_static_quad();
    fs::path dir = make_temp_dir("no_particles");
    fs::path glbPath = dir / "plain.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);
    CHECK(result.particlesJsonPath.empty());
    CHECK(!fs::exists(dir / "plain_particles.json"));
}

CM_TEST(exportgltf, glb_header_and_json_are_well_formed) {
    ModelPreview model = make_static_quad();
    fs::path dir = make_temp_dir("header");
    fs::path glbPath = dir / "quad.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);
    CHECK(fs::exists(glbPath));

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    CHECK(g.doc.contains("asset"));
    CHECK_EQ(g.doc["asset"]["version"].get<std::string>(), std::string("2.0"));
    CHECK(!g.bin.empty());
}

CM_TEST(exportgltf, model_export_structure) {
    ModelPreview model = make_static_quad();
    fs::path dir = make_temp_dir("model");
    fs::path glbPath = dir / "quad.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    CHECK_EQ(g.doc["meshes"].size(), size_t(1));
    CHECK_EQ(g.doc["materials"].size(), size_t(1));
    CHECK_EQ(g.doc["textures"].size(), size_t(1));
    CHECK_EQ(g.doc["images"].size(), size_t(1));

    // The texture is embedded, not a sibling file -- prove the bytes really
    // are a PNG at the declared bufferView.
    const json& image = g.doc["images"][0];
    const json& bv = g.doc["bufferViews"][image["bufferView"].get<size_t>()];
    size_t offset = bv["byteOffset"].get<size_t>();
    CHECK(starts_with_png_signature(g.bin.data() + offset, bv["byteLength"].get<size_t>()));

    // Root node carries the fixed Z-up -> Y-up rotation and owns everything.
    // It's created last (after the mesh/joint nodes it parents), so find it
    // by name rather than assuming a position in the nodes array.
    const json* root = nullptr;
    for (const json& node : g.doc["nodes"]) {
        if (node.value("name", std::string()) == "GW2_ZupToYup") { root = &node; break; }
    }
    CHECK(root != nullptr);
    CHECK(root->contains("rotation"));
    CHECK_EQ((*root)["children"].size(), size_t(1)); // the one mesh node
}

CM_TEST(exportgltf, effect_material_gets_baked_emissive_texture) {
    ModelPreview model = make_effect_quad();
    fs::path dir = make_temp_dir("effect");
    fs::path glbPath = dir / "glow.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json& material = g.doc["materials"][0];
    CHECK(material.contains("emissiveTexture"));
    std::vector<double> emissiveFactor = material["emissiveFactor"].get<std::vector<double>>();
    CHECK_EQ(emissiveFactor.size(), size_t(3));
    CHECK_NEAR(emissiveFactor[0], 1.0, 1e-9);
    CHECK_NEAR(emissiveFactor[1], 1.0, 1e-9);
    CHECK_NEAR(emissiveFactor[2], 1.0, 1e-9);

    // A real, separate embedded image for the emissive channel -- not a
    // reused reference to the diffuse texture's own image.
    CHECK_EQ(g.doc["images"].size(), size_t(2));
    int emissiveTexIdx = material["emissiveTexture"]["index"].get<int>();
    int emissiveImageIdx = g.doc["textures"][static_cast<size_t>(emissiveTexIdx)]["source"].get<int>();
    const json& bv = g.doc["bufferViews"][g.doc["images"][static_cast<size_t>(emissiveImageIdx)]["bufferView"].get<size_t>()];
    CHECK(starts_with_png_signature(g.bin.data() + bv["byteOffset"].get<size_t>(), bv["byteLength"].get<size_t>()));
}

CM_TEST(exportgltf, skinning_weights_sum_to_vertex_count) {
    ModelPreview model = make_skinned_quad();
    fs::path dir = make_temp_dir("skin");
    fs::path glbPath = dir / "rig.glb";

    GltfExportResult result = export_model_gltf(model, glbPath.string());
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    CHECK_EQ(g.doc["skins"].size(), size_t(1));
    CHECK_EQ(g.doc["skins"][0]["joints"].size(), size_t(2));

    // Find the mesh's WEIGHTS_0 accessor and sum it: every skinned vertex's
    // weights sum to 1 by construction (see make_skinned_quad), so the total
    // must equal the vertex count -- proof no weight was dropped or misrouted.
    const json& primitive = g.doc["meshes"][0]["primitives"][0];
    CHECK(primitive["attributes"].contains("WEIGHTS_0"));
    std::vector<double> weights = accessor_values(g, primitive["attributes"]["WEIGHTS_0"].get<int>());
    double total = 0.0;
    for (double v : weights) total += v;
    CHECK_NEAR(total, static_cast<double>(model.meshes[0].vertices.size()), 1e-3);

    // inverseBindMatrices should be GW2's own invWorld values verbatim
    // (column-major-flattened), not re-derived -- spot-check the child
    // joint's translation row lands at [12] after the transpose-flatten.
    std::vector<double> ibm = accessor_values(g, g.doc["skins"][0]["inverseBindMatrices"].get<int>());
    CHECK_EQ(ibm.size(), size_t(2 * 16));
    CHECK_NEAR(ibm[16 + 12], -1.0, 1e-6); // second joint's (child's) matrix, translation.x
}

CM_TEST(exportgltf, animation_bakes_expected_endpoints) {
    ModelPreview model = make_skinned_quad();
    fs::path dir = make_temp_dir("anim");
    fs::path glbPath = dir / "anim.glb";

    GltfExportOptions opts;
    opts.animFps = 30;
    GltfExportResult result = export_model_gltf(model, glbPath.string(), opts);
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    CHECK_EQ(g.doc["animations"].size(), size_t(1));
    const json& anim = g.doc["animations"][0];

    int translationSampler = -1;
    for (const json& ch : anim["channels"]) {
        if (ch["target"]["path"] == "translation") { translationSampler = ch["sampler"].get<int>(); break; }
    }
    CHECK(translationSampler >= 0);
    int outputAcc = anim["samplers"][static_cast<size_t>(translationSampler)]["output"].get<int>();
    std::vector<double> values = accessor_values(g, outputAcc); // VEC3 per keyframe
    CHECK(!values.empty());
    CHECK_NEAR(values[0], 1.0, 1e-3);                      // first keyframe's X
    CHECK_NEAR(values[values.size() - 3], 2.0, 1e-3);      // last keyframe's X
}

CM_TEST(exportgltf, map_export_shares_geometry_across_instances) {
    MapScene scene;
    scene.models.push_back(make_static_quad());
    scene.modelFileIds.push_back(1);

    MapInstance a;
    a.model = 0; a.pos[0] = 0; a.pos[1] = 0; a.pos[2] = 0; a.rot[2] = 0.0f; a.scale = 1.0f;
    MapInstance b;
    b.model = 0; b.pos[0] = 10; b.pos[1] = 5; b.pos[2] = 0; b.rot[2] = 1.2f; b.scale = 2.0f;
    scene.instances = {a, b};

    fs::path dir = make_temp_dir("map");
    fs::path glbPath = dir / "area.glb";
    GltfExportResult result = export_map_gltf(scene, glbPath.string());
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    // True node instancing: one mesh shared by two placement nodes, not two
    // vertex-baked copies -- the point of moving off FBX's decomposition risk.
    CHECK_EQ(g.doc["meshes"].size(), size_t(1));

    std::vector<std::vector<double>> matrices;
    for (const json& node : g.doc["nodes"]) {
        if (node.contains("matrix")) matrices.push_back(node["matrix"].get<std::vector<double>>());
    }
    CHECK_EQ(matrices.size(), size_t(2));
    bool differs = false;
    for (size_t i = 0; i < 16; ++i) {
        if (std::fabs(matrices[0][i] - matrices[1][i]) > 1e-6) { differs = true; break; }
    }
    CHECK(differs); // different placement -> different instance matrices
}
