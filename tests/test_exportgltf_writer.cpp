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

#include "castlemist/exportgltf/world_export.h"

#include "castlemist/native/granny_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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

/// @brief The quad with a weapon-style glow: a tiling "glow" colour texture on
///        UV0 and a "glowmask" saying where it shows, on UV2 (Forged Dagger).
ModelPreview make_glowmask_quad() {
    ModelPreview mp = make_static_quad();
    ModelTextureCPU glow;  // flat orange
    glow.fileId = 7;
    glow.width = glow.height = 2;
    for (int i = 0; i < 4; ++i) glow.rgba.insert(glow.rgba.end(), {200, 100, 0, 255});
    ModelTextureCPU mask;
    mask.fileId = 8;
    mask.width = mask.height = 2;
    mask.rgba.assign(2 * 2 * 4, 255);
    mp.textures.push_back(glow);
    mp.textures.push_back(mask);
    mp.materials[0].extraTextures = {{1, 0, 7, "glow"}, {2, 2, 8, "glowmask"}};
    return mp;
}

CM_TEST(exportgltf, glowmask_is_the_emissive_map_tinted_by_the_glow) {
    ModelPreview model = make_glowmask_quad();
    fs::path dir = make_temp_dir("glowmask");
    fs::path glbPath = dir / "dagger.glb";
    CHECK(export_model_gltf(model, glbPath.string()).ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json& material = g.doc["materials"][0];
    CHECK(material.contains("emissiveTexture"));
    int texIdx = material["emissiveTexture"]["index"].get<int>();
    int imgIdx = g.doc["textures"][static_cast<size_t>(texIdx)]["source"].get<int>();
    CHECK_EQ(g.doc["images"][static_cast<size_t>(imgIdx)]["name"].get<std::string>(), std::string("8"));
    CHECK_EQ(material["emissiveTexture"]["texCoord"].get<int>(), 2);
    std::vector<double> f = material["emissiveFactor"].get<std::vector<double>>();
    CHECK_NEAR(f[0], 1.0, 1e-6);  // the glow's colour, brightest channel at 1
    CHECK_NEAR(f[1], 0.5, 1e-6);
    CHECK_NEAR(f[2], 0.0, 1e-6);
    CHECK(g.doc["meshes"][0]["primitives"][0]["attributes"].contains("TEXCOORD_2"));
}

CM_TEST(exportgltf, embedded_images_are_named_after_their_file_id) {
    ModelPreview model = make_effect_quad();
    fs::path dir = make_temp_dir("names");
    fs::path glbPath = dir / "named.glb";
    CHECK(export_model_gltf(model, glbPath.string()).ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    CHECK_EQ(g.doc["images"][0]["name"].get<std::string>(), std::string("42"));
    CHECK_EQ(g.doc["images"][1]["name"].get<std::string>(), std::string("42_emissive"));
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

CM_TEST(exportgltf, root_rotation_and_scale_are_configurable) {
    ModelPreview model = make_static_quad();
    fs::path dir = make_temp_dir("rootxform");
    fs::path glbPath = dir / "quad.glb";
    GltfExportOptions opts;
    opts.rootRotation = {0.0, -0.70710678, 0.70710678, 0.0};  // the character assembler's upright turn
    opts.rootScale = 0.0254;                                   // GW2 inches -> metres
    GltfExportResult result = export_model_gltf(model, glbPath.string(), opts);
    CHECK(result.ok);

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json* root = nullptr;
    for (const json& node : g.doc["nodes"])
        if (node.value("name", std::string()) == "GW2_ZupToYup") root = &node;
    CHECK(root != nullptr);
    CHECK_NEAR((*root)["rotation"][1].get<double>(), -0.70710678, 1e-6);
    CHECK_NEAR((*root)["rotation"][2].get<double>(), 0.70710678, 1e-6);
    CHECK_NEAR((*root)["scale"][0].get<double>(), 0.0254, 1e-9);
    CHECK_NEAR((*root)["scale"][2].get<double>(), 0.0254, 1e-9);
}

CM_TEST(exportgltf, default_root_has_no_scale) {
    ModelPreview model = make_static_quad();
    fs::path dir = make_temp_dir("rootdefault");
    fs::path glbPath = dir / "quad.glb";
    CHECK(export_model_gltf(model, glbPath.string()).ok);
    ParsedGlb g = parse_glb(read_bytes(glbPath));
    for (const json& node : g.doc["nodes"])
        if (node.value("name", std::string()) == "GW2_ZupToYup") {
            CHECK(!node.contains("scale"));
            CHECK_NEAR(node["rotation"][0].get<double>(), -0.70710678, 1e-6);
        }
}

CM_TEST(exportgltf, generated_textures_are_never_merged) {
    // Two in-memory textures (fileId 0, e.g. a baked diffuse and normal atlas)
    // must stay two textures -- the normal map is not the diffuse.
    ModelPreview model = make_static_quad();
    model.textures[0].fileId = 0;
    ModelTextureCPU normal = model.textures[0];
    normal.rgba.assign(2 * 2 * 4, 128);
    normal.isNormal = true;
    model.textures.push_back(normal);
    model.materials[0].normalTex = 1;
    fs::path dir = make_temp_dir("gentex");
    fs::path glbPath = dir / "quad.glb";
    CHECK(export_model_gltf(model, glbPath.string()).ok);
    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json& mat = g.doc["materials"][0];
    CHECK(mat["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<int>() !=
          mat["normalTexture"]["index"].get<int>());
}

CM_TEST(exportgltf, morph_targets_export_as_named_shape_keys) {
    ModelPreview model = make_static_quad();
    MorphTargetCPU t;
    t.name = "Jaw Width+";
    t.delta.assign(model.meshes[0].vertices.size() * 3, 0.0f);
    t.delta[0] = 0.5f;
    t.weight = 0.25f;
    model.meshes[0].morphs.push_back(t);
    fs::path dir = make_temp_dir("morphs");
    fs::path glbPath = dir / "quad.glb";
    CHECK(export_model_gltf(model, glbPath.string()).ok);
    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json& mesh = g.doc["meshes"][0];
    CHECK_EQ(mesh["primitives"][0]["targets"].size(), size_t{1});
    CHECK(mesh["primitives"][0]["targets"][0].contains("POSITION"));
    CHECK_EQ(mesh["extras"]["targetNames"][0].get<std::string>(), std::string("Jaw Width+"));
    CHECK_NEAR(mesh["weights"][0].get<double>(), 0.25, 1e-6);
}

namespace {

// A 1x1-chunk terrain (3x3 samples, flat at map z = -5, i.e. 5 up), one prop
// whose model loaded, one whose model did not, and one collision instance.
castlemist::world::WorldScene make_world() {
    castlemist::world::WorldScene s;
    s.terrain.present = true;
    s.terrain.chunksX = s.terrain.chunksY = 1;
    castlemist::world::TerrainChunk c;
    c.rect[0] = 0; c.rect[1] = 0; c.rect[2] = 10; c.rect[3] = 10;
    c.samples = 3;
    c.heights.assign(9, -5.0f);
    s.terrain.chunks.push_back(c);

    s.models = {{100}, {200}};
    castlemist::world::PropInstance p;
    p.model = 0;
    const float m[16] = {2,0,0,0, 0,2,0,0, 0,0,2,0, 3,4,5,1};
    std::copy(m, m + 16, p.world);
    s.props.push_back(p);
    p.model = 1;
    s.props.push_back(p);

    castlemist::world::CollisionMesh hull;
    hull.verts = {0,0,0, 1,0,0, 0,1,0};
    hull.indices = {0, 1, 2};
    s.collision.meshes.push_back(hull);
    castlemist::world::CollisionInstance ci;
    const float id[16] = {1,0,0,0, 0,1,0,0, 0,0,-1,0, 0,0,0,1};
    std::copy(id, id + 16, ci.world);
    ci.group = "obs";
    s.collision.instances.push_back(ci);
    return s;
}

const json* find_node(const ParsedGlb& g, const std::string& name) {
    for (const json& n : g.doc["nodes"])
        if (n.value("name", std::string()) == name) return &n;
    return nullptr;
}

} // namespace

CM_TEST(exportgltf, world_export_root_maps_map_space_to_gltf) {
    // Map (x, y, z) -> glTF (x, -z, -y): east stays +X, up (-Z) becomes +Y,
    // north (+Y) becomes -Z. Negative determinant: the handedness change.
    std::array<float, 16> r = map_to_gltf_root(1.0);
    auto apply = [&](float x, float y, float z, int row) { return r[row] * x + r[4 + row] * y + r[8 + row] * z; };
    CHECK_NEAR(apply(1, 0, 0, 0), 1.0, 1e-6);
    CHECK_NEAR(apply(0, 0, -1, 1), 1.0, 1e-6);   // up
    CHECK_NEAR(apply(0, 1, 0, 2), -1.0, 1e-6);   // north
    std::array<float, 16> half = map_to_gltf_root(0.5);
    CHECK_NEAR(half[0], 0.5, 1e-6);
    CHECK_NEAR(half[9], -0.5, 1e-6);
}

CM_TEST(exportgltf, world_export_writes_terrain_props_and_skips_unloaded) {
    castlemist::world::WorldScene s = make_world();
    std::vector<std::shared_ptr<ModelPreview>> models = {std::make_shared<ModelPreview>(make_static_quad()), nullptr};
    fs::path glbPath = make_temp_dir("world") / "map.glb";
    WorldExportReport rep = export_world_gltf(s, models, glbPath.string());
    CHECK(rep.ok);
    CHECK_EQ(rep.terrainChunks, size_t(1));
    CHECK_EQ(rep.propsWritten, size_t(1));
    CHECK_EQ(rep.propsSkipped, size_t(1));
    CHECK_EQ(rep.modelsWritten, size_t(1));
    CHECK_EQ(rep.collisionInstances, size_t(0));  // off by default

    ParsedGlb g = parse_glb(read_bytes(glbPath));
    const json* root = find_node(g, "GW2_MapToGltf");
    CHECK(root != nullptr);
    CHECK_EQ((*root)["children"].size(), size_t(2));  // Terrain, Props
    CHECK(find_node(g, "Collision") == nullptr);

    // The prop carries PropInstance::world unchanged (both column-major).
    const json* prop = find_node(g, "prop0_100");
    CHECK(prop != nullptr);
    CHECK_NEAR((*prop)["matrix"][0].get<double>(), 2.0, 1e-6);
    CHECK_NEAR((*prop)["matrix"][12].get<double>(), 3.0, 1e-6);
    CHECK_NEAR((*prop)["matrix"][14].get<double>(), 5.0, 1e-6);

    // Terrain: 9 vertices, 8 triangles, flat ground's normal is map up (-Z).
    const json* chunk = find_node(g, "chunk_0_0");
    CHECK(chunk != nullptr);
    const json& prim = g.doc["meshes"][(*chunk)["mesh"].get<size_t>()]["primitives"][0];
    std::vector<double> pos = accessor_values(g, prim["attributes"]["POSITION"].get<int>());
    std::vector<double> nrm = accessor_values(g, prim["attributes"]["NORMAL"].get<int>());
    std::vector<double> uv = accessor_values(g, prim["attributes"]["TEXCOORD_0"].get<int>());
    std::vector<double> idx = accessor_values(g, prim["indices"].get<int>());
    CHECK_EQ(pos.size(), size_t(27));
    CHECK_EQ(idx.size(), size_t(24));
    CHECK_NEAR(nrm[2], -1.0, 1e-6);
    // Row 0 is the north edge (y = rect[3]) with v = 0 there.
    CHECK_NEAR(pos[1], 10.0, 1e-6);
    CHECK_NEAR(uv[1], 0.0, 1e-6);
    CHECK_NEAR(uv[uv.size() - 1], 1.0, 1e-6);
    // Every triangle's map-space cross product points up (-Z): outward.
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        auto P = [&](size_t k, int c) { return pos[static_cast<size_t>(idx[t + k]) * 3 + c]; };
        double ax = P(1, 0) - P(0, 0), ay = P(1, 1) - P(0, 1);
        double bx = P(2, 0) - P(0, 0), by = P(2, 1) - P(0, 1);
        CHECK(ax * by - ay * bx < 0);
    }
}

CM_TEST(exportgltf, world_export_collision_and_empty_scene) {
    castlemist::world::WorldScene s = make_world();
    WorldExportOptions opt;
    opt.terrain = false;
    opt.props = false;
    opt.collision = true;
    fs::path dir = make_temp_dir("world_coll");
    WorldExportReport rep = export_world_gltf(s, {}, (dir / "c.glb").string(), opt);
    CHECK(rep.ok);
    CHECK_EQ(rep.collisionInstances, size_t(1));
    ParsedGlb g = parse_glb(read_bytes(dir / "c.glb"));
    const json* inst = find_node(g, "obs_0");
    CHECK(inst != nullptr);
    CHECK_NEAR((*inst)["matrix"][10].get<double>(), -1.0, 1e-6);

    castlemist::world::WorldScene empty;
    WorldExportReport none = export_world_gltf(empty, {}, (dir / "e.glb").string());
    CHECK(!none.ok);
    CHECK(!fs::exists(dir / "e.glb"));
}

CM_TEST(exportgltf, vertex_colours_only_when_asked) {
    // GW2's fx-flow (2591650) reads its parameters from the vertex colour; the world
    // export writes it as COLOR_0, other exports leave it out (viewers tint by it).
    ModelPreview model = make_static_quad();
    model.meshes[0].hasColor = true;
    for (GVertex& v : model.meshes[0].vertices) {
        v.col[0] = 255; v.col[1] = 51; v.col[2] = 204; v.col[3] = 102;
    }
    fs::path dir = make_temp_dir("vcol");

    fs::path plain = dir / "plain.glb";
    CHECK(export_model_gltf(model, plain.string()).ok);
    ParsedGlb a = parse_glb(read_bytes(plain));
    CHECK(!a.doc["meshes"][0]["primitives"][0]["attributes"].contains("COLOR_0"));

    GltfExportOptions opts;
    opts.vertexColors = true;
    fs::path coloured = dir / "coloured.glb";
    CHECK(export_model_gltf(model, coloured.string(), opts).ok);
    ParsedGlb b = parse_glb(read_bytes(coloured));
    const json& attrs = b.doc["meshes"][0]["primitives"][0]["attributes"];
    CHECK(attrs.contains("COLOR_0"));
    if (attrs.contains("COLOR_0")) {
        std::vector<double> c = accessor_values(b, attrs["COLOR_0"].get<int>());
        CHECK_EQ(c.size(), model.meshes[0].vertices.size() * 4);
        CHECK_NEAR(c[1], 0.2, 1e-6);
        CHECK_NEAR(c[3], 0.4, 1e-6);
    }
}
