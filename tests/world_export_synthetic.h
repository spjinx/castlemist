/// @file
/// @brief The world export's synthetic fixture, shared by cm_test_exportgltf
///        (tests/test_exportgltf_world.cpp) and cm_world_export_fixture
///        (tests/tools/world_export_fixture.cpp).
///
/// synthetic_world(): a 2 x 2-chunk grid (33 samples per side, heights > 0, so
/// all of it is under the water plane at 0, up = -Z), models 101 (static, one
/// material) and 102 (skinned, clip "TestClip", animClipTokens {0x1234});
/// three props of 101 and one of 102 (propAnimArray, animSequence 0x1234),
/// one hull instance and a water plane at 0 with one env preset. The models
/// come from SyntheticModelSource, header-only textures as a dat load gives
/// them, decoded by texture().
///
/// Zones (world-foliage spec D2, "Testing" exportgltf): one type-3 layer whose
/// models are {101} and one type-1 layer whose models are {103} (model 103:
/// static grass, one mesh, one material, a traced game material naming
/// VegPara); a collider tree of 101 (Collider, placement 0, collider 0) in
/// chunk (0, 0); a geometry-only collider of 101 (layer = placement =
/// UINT32_MAX, collider 1) in chunk (1, 0); two grass instances of 103
/// (Density, placements 1 and 3) in chunks (0, 1) and (1, 1). fmix32(1) < 2^31
/// and fmix32(3) >= 2^31, so density 0.5 keeps exactly one of them. Model 101
/// is shared with the props, and 103 is zone-only.
///
/// static_cube() and skinned_cube() are adapted from make_static_quad /
/// make_skinned_quad of tests/test_exportgltf_writer.cpp.
#pragma once

#include "castlemist/exportgltf/world_folder.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/native/granny_anim.hpp"
#include "castlemist/world/dat_read.h"
#include "castlemist/world/world_scene.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace castlemist::test_world {

inline constexpr float kSynX0 = -15360.f, kSynY1 = 39936.f, kSynChunk = 3072.f;
inline constexpr uint64_t kSynClipToken = 0x1234;

inline GVertex syn_vertex(float x, float y, float z, float u, float v, float nx, float ny, float nz) {
    GVertex g{};
    g.px = x; g.py = y; g.pz = z;
    g.nx = nx; g.ny = ny; g.nz = nz;
    g.tx = 1; g.ty = 0; g.tz = 0;
    g.bx = 0; g.by = 1; g.bz = 0;
    g.u = u; g.v = v;
    return g;
}

/// @brief A unit cube (24 vertices, 12 triangles), one material, one
///        header-only texture (fileId 42, 2 x 2, rgba empty).
inline ModelPreview static_cube() {
    ModelPreview mp;
    ModelMeshCPU mesh;
    // Each face: normal n, two in-plane axes a, b; corners centre +- a +- b.
    const float faces[6][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},  {-1, 0, 0, 0, 0, 1, 0, 1, 0},
                               {0, 1, 0, 0, 0, 1, 1, 0, 0},  {0, -1, 0, 1, 0, 0, 0, 0, 1},
                               {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, 0, 1, 0, 1, 0, 0}};
    for (const auto& f : faces) {
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        for (int k = 0; k < 4; ++k) {
            const float sa = (k == 1 || k == 2) ? 1.f : -1.f, sb = (k >= 2) ? 1.f : -1.f;
            float p[3];
            for (int i = 0; i < 3; ++i) p[i] = 0.5f * f[i] + 0.5f * sa * f[3 + i] + 0.5f * sb * f[6 + i];
            mesh.vertices.push_back(syn_vertex(p[0], p[1], p[2], sa > 0 ? 1.f : 0.f, sb > 0 ? 1.f : 0.f, f[0], f[1], f[2]));
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
    }
    // Task 15b: LOD1 = the first four faces (8 triangles), LOD2 = the first two (4).
    mesh.lodIndices = {std::vector<uint32_t>(mesh.indices.begin(), mesh.indices.begin() + 24),
                       std::vector<uint32_t>(mesh.indices.begin(), mesh.indices.begin() + 12)};
    mesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    mesh.hasTangents = true;
    mesh.materialIndex = 0;
    mp.meshes.push_back(mesh);
    mp.hasLodOverride = true;
    mp.lodOverride[0] = 1500;   // LOD1 from the file; LOD2 0: the client's 7500
    mp.lodOverride[1] = 0;

    ModelTextureCPU tex;   // header-only, as ModelLoadOptions{.., decodeTextures = false}
    tex.fileId = 42;
    tex.width = 2;
    tex.height = 2;
    mp.textures.push_back(tex);

    ModelMaterialCPU mat;
    mat.index = 0;
    mat.materialFile = 58246;
    mat.diffuseTex = 0;
    mp.materials.push_back(mat);
    return mp;
}

/// @brief The cube, rigidly split across two joints, with one clip "TestClip"
///        whose token is kSynClipToken.
inline ModelPreview skinned_cube() {
    ModelPreview mp = static_cube();
    mp.materials[0].materialFile = 58247;
    ModelMeshCPU& mesh = mp.meshes[0];
    mesh.hasSkin = true;
    for (GVertex& v : mesh.vertices) {
        v.bidx[0] = v.px > 0 ? 1 : 0;
        v.bwt[0] = 1.0f;
    }

    auto identity16 = [](float* out) {
        static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
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
    child.invWorld[12] = -1.0f;
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
    track.pos.controls = {1, 0, 0, 2, 0, 0};
    clip.tracks.push_back(track);
    mp.animClips.push_back(clip);
    mp.animClipTokens = {kSynClipToken};
    mp.animationTokens = {kSynClipToken};
    mp.hasAnimation = true;
    return mp;
}

/// @brief The skinned frame probe (world export Task 11): the frame probe's
///        three bars (world_probe.h, map space: "East" x in [0, 300], "North"
///        y in [0, 200], "Up" z in [-100, 0], the other axes in [-10, 10]) as
///        one skinned mesh, each bar bound rigidly to its own joint under
///        "root"; all joints sit at the origin. "North" has a 90-degree rest
///        rotation about map +Y, so a wrong rest-frame conversion shows. Clip
///        "ProbeClip" (1 s, linear): East moves +100 east, North turns 90
///        degrees about map +Z (its bar ends pointing west), Up moves 50 up
///        (map -Z). castlemist_world.py --check-skinned-probe measures it.
inline ModelPreview skinned_probe() {
    ModelPreview mp;
    ModelMeshCPU mesh;
    const float h = 10.0f;
    const float lo[3][3] = {{0, -h, -h}, {-h, 0, -h}, {-h, -h, -100}};
    const float hi[3][3] = {{300, h, h}, {h, 200, h}, {h, h, 0}};
    // static_cube's face layout: normal n, in-plane axes a, b with a x b = n.
    const float faces[6][9] = {{1, 0, 0, 0, 1, 0, 0, 0, 1},  {-1, 0, 0, 0, 0, 1, 0, 1, 0},
                               {0, 1, 0, 0, 0, 1, 1, 0, 0},  {0, -1, 0, 1, 0, 0, 0, 0, 1},
                               {0, 0, 1, 1, 0, 0, 0, 1, 0},  {0, 0, -1, 0, 1, 0, 1, 0, 0}};
    for (uint32_t bar = 0; bar < 3; ++bar) {
        float c[3], e[3];
        for (int i = 0; i < 3; ++i) { c[i] = 0.5f * (lo[bar][i] + hi[bar][i]); e[i] = 0.5f * (hi[bar][i] - lo[bar][i]); }
        for (const auto& f : faces) {
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            for (int k = 0; k < 4; ++k) {
                const float sa = (k == 1 || k == 2) ? 1.f : -1.f, sb = (k >= 2) ? 1.f : -1.f;
                float p[3];
                for (int i = 0; i < 3; ++i) p[i] = c[i] + e[i] * (f[i] + sa * f[3 + i] + sb * f[6 + i]);
                GVertex v = syn_vertex(p[0], p[1], p[2], sa > 0 ? 1.f : 0.f, sb > 0 ? 1.f : 0.f, f[0], f[1], f[2]);
                v.bidx[0] = bar + 1;
                v.bwt[0] = 1.0f;
                mesh.vertices.push_back(v);
            }
            for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) mesh.indices.push_back(base + i);
        }
    }
    mesh.vertexCount = static_cast<uint32_t>(mesh.vertices.size());
    mesh.hasTangents = true;
    mesh.hasSkin = true;
    mesh.materialIndex = 0;
    mp.meshes.push_back(mesh);

    ModelMaterialCPU mat;
    mat.index = 0;
    mat.materialFile = 58248;
    mp.materials.push_back(mat);

    const float s = 0.70710678f;
    auto joint = [](const char* name, int parent) {
        ModelJoint j;
        j.name = name;
        j.parent = parent;
        return j;
    };
    ModelJoint north = joint("North", 0);
    north.localQuat[0] = 0; north.localQuat[1] = s; north.localQuat[2] = 0; north.localQuat[3] = s;   // 90 deg about +Y
    // invWorld is row-major in row-vector form: the column-convention rotation's own rows.
    const float ry[16] = {0, 0, 1, 0, 0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 0, 1};
    std::copy(std::begin(ry), std::end(ry), north.invWorld);
    mp.joints = {joint("root", -1), joint("East", 0), north, joint("Up", 0)};

    namespace gr = castlemist::granny;
    gr::Anim clip;
    clip.valid = true;
    clip.name = "ProbeClip";
    clip.duration = 1.0f;
    auto curve = [](int dim, std::vector<float> controls) {
        gr::Curve c;
        c.fmt = gr::F_K32fC32f;
        c.dim = dim;
        c.degree = 1;
        c.knots = {0.0f, 1.0f};
        c.controls = std::move(controls);
        return c;
    };
    gr::Track east;
    east.name = "East";
    east.pos = curve(3, {0, 0, 0, 100, 0, 0});
    gr::Track northT;
    northT.name = "North";
    northT.ori = curve(4, {0, s, 0, s, -0.5f, 0.5f, 0.5f, 0.5f});   // A, then Rz(90) * A
    gr::Track up;
    up.name = "Up";
    up.pos = curve(3, {0, 0, 0, 0, 0, -50});
    clip.tracks = {east, northT, up};
    mp.animClips.push_back(clip);
    mp.animClipTokens = {kSynClipToken + 1};
    mp.animationTokens = {kSynClipToken + 1};
    mp.hasAnimation = true;
    return mp;
}

/// @brief static_cube as zone grass: material 58249, and one traced game
///        material whose vertex stage names VegPara (a wind-capable AMAT).
inline ModelPreview static_grass() {
    ModelPreview mp = static_cube();
    mp.materials[0].materialFile = 58249;
    GameMaterial g;
    g.index = 0;
    g.ok = true;
    GameShaderUniform u;
    u.name = "VegPara";
    g.vsUniforms.push_back(u);
    mp.gameMaterials.push_back(g);
    return mp;
}

/// @brief A uniform scale @p s about z, then a translation, column-major.
inline void syn_place(float out[16], float x, float y, float z, float s, float yawCos = 1, float yawSin = 0) {
    std::fill(out, out + 16, 0.f);
    out[0] = s * yawCos; out[1] = s * yawSin;
    out[4] = -s * yawSin; out[5] = s * yawCos;
    out[10] = s;
    out[12] = x; out[13] = y; out[14] = z; out[15] = 1;
}

inline castlemist::world::WorldScene synthetic_world() {
    namespace cw = castlemist::world;
    cw::WorldScene s;
    s.mapFileId = 1151420;
    s.hasBounds = true;
    s.bounds[0] = kSynX0; s.bounds[1] = kSynY1 - 2 * kSynChunk;
    s.bounds[2] = kSynX0 + 2 * kSynChunk; s.bounds[3] = kSynY1;
    s.terrain.present = true;
    s.terrain.chunksX = s.terrain.chunksY = 2;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx) {
            cw::TerrainChunk c;
            c.cx = cx; c.cy = cy; c.samples = 33;
            c.rect[0] = kSynX0 + cx * kSynChunk; c.rect[2] = kSynX0 + (cx + 1) * kSynChunk;
            c.rect[3] = kSynY1 - cy * kSynChunk; c.rect[1] = kSynY1 - (cy + 1) * kSynChunk;
            c.material.pickerScale = 0.25f;
            c.heights.resize(33 * 33);
            for (int j = 0; j < 33; ++j)
                for (int i = 0; i < 33; ++i)
                    c.heights[size_t(j) * 33 + i] = 10.0f + 0.5f * float(cx * 32 + i) - 0.25f * float(cy * 32 + j);
            s.terrain.chunks.push_back(c);
        }

    s.models = {cw::PropModel{101}, cw::PropModel{102}, cw::PropModel{103}};
    const float pts[4][2] = {{-14000, 38000}, {-11000, 38500}, {-13000, 35000}, {-10000, 34500}};
    for (int k = 0; k < 4; ++k) {
        cw::PropInstance p;
        p.model = k < 3 ? 0 : 1;
        syn_place(p.world, pts[k][0], pts[k][1], -20.f, 1.5f + 0.5f * float(k), 0.6f, 0.8f);
        p.pos[0] = pts[k][0]; p.pos[1] = pts[k][1]; p.pos[2] = -20.f;
        p.scale = 1.5f + 0.5f * float(k);
        p.group = k < 3 ? "propArray" : "propAnimArray";
        if (k == 3) p.animSequence = kSynClipToken;
        if (k == 1) { p.flags = 1u << 16; p.lod1 = 1000; p.lod2 = 2000; }   // OVERRIDE_LOD (Task 15b)
        if (k == 2) { p.flags = 0; p.lod1 = 900; p.lod2 = 1800; }          // stored, but no OVERRIDE_LOD
        s.props.push_back(p);
    }

    cw::CollisionMesh hull;
    hull.verts = {-12500.f, 36000.f, -5.f, -11500.f, 36000.f, -5.f, -12500.f, 37000.f, -5.f};
    hull.indices = {0, 1, 2};
    s.collision.meshes.push_back(hull);
    cw::CollisionInstance inst;
    inst.mesh = 0;
    inst.world[0] = inst.world[5] = inst.world[10] = inst.world[15] = 1;
    inst.group = "obs";
    s.collision.instances.push_back(inst);

    s.water.hasPlane = true;
    s.water.planeZ = 0;
    castlemist::model::Extractor::MapEnvWater w;
    w.depthAttenuation = 10;
    w.surfaceShallowColor[2] = 255;
    w.surfaceDeepColor[0] = 255;
    s.environment.water.push_back(w);
    s.environment.light.presetIndex = 0;

    // Zones: two layers, four placements, four instances (see the file comment).
    cw::ZoneScatter& z = s.zones;
    z.present = true;
    z.mode = cw::ZoneScatterMode::Colliders;
    cw::ZoneLayer trees;
    trees.defToken = 17382; trees.indexInDef = 0; trees.type = 3; trees.width = 4; trees.height = 6;
    trees.radiusGround = 2; trees.fadeRange[0] = 7400; trees.fadeRange[1] = 7500;
    trees.scaleRange[0] = 0.5f; trees.scaleRange[1] = 1.5f; trees.rotRange[2][1] = 360; trees.layerFlags = 142;
    trees.models.push_back(cw::ZoneLayerModel{101, 0});
    cw::ZoneLayer grass;
    grass.defToken = 17382; grass.indexInDef = 1; grass.type = 1; grass.width = 1; grass.height = 1;
    grass.fadeRange[0] = 2900; grass.fadeRange[1] = 3000; grass.scaleRange[0] = 0.8f; grass.scaleRange[1] = 1.2f;
    grass.materialFileId = 184994;
    grass.models.push_back(cw::ZoneLayerModel{103, 2});
    z.layers = {trees, grass};
    cw::Zone zone;
    zone.defToken = 17382; zone.placementCount = 4;
    z.zones.push_back(zone);
    for (uint32_t k = 0; k < 4; ++k) {
        cw::ZonePlacement pl;
        pl.zone = 0; pl.layer = k == 0 ? 0 : 1; pl.c = int32_t(10 + k); pl.r = 5;
        pl.flag = uint8_t(((k == 0 ? 1 : 2) << 4) | 1);
        pl.collider = k == 0 ? 0 : UINT32_MAX;
        z.placements.push_back(pl);
    }
    auto zone_instance = [](uint32_t model, uint32_t layer, uint32_t placement, uint32_t collider,
                            cw::ZoneSource src, float x, float y, float sc, float c, float sn) {
        cw::ZoneInstance i;
        i.model = model; i.layer = layer; i.placement = placement; i.collider = collider; i.source = src;
        syn_place(i.world, x, y, -15.f, sc, c, sn);
        return i;
    };
    z.instances = {
        zone_instance(0, 0, 0, 0, cw::ZoneSource::Collider, -14500, 39000, 1.2f, 0.8f, 0.6f),       // chunk (0, 0)
        zone_instance(0, UINT32_MAX, UINT32_MAX, 1, cw::ZoneSource::Collider, -11500, 37500, 0.9f, 0, 1),   // (1, 0)
        zone_instance(2, 1, 1, UINT32_MAX, cw::ZoneSource::Density, -13500, 36000, 0.8f, 1, 0),      // (0, 1)
        zone_instance(2, 1, 3, UINT32_MAX, cw::ZoneSource::Density, -10500, 35500, 1.1f, -0.6f, 0.8f),   // (1, 1)
    };
    // The grass at placement 3 leans 10 degrees about map +X (a tilted instance).
    {
        const float a = 10.0f * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
        float* w = z.instances[3].world;
        float rx[16] = {1, 0, 0, 0, 0, ca, sa, 0, 0, -sa, ca, 0, 0, 0, 0, 1};
        float out[16] = {};
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k) out[col * 4 + row] += w[k * 4 + row] * rx[col * 4 + k];
        std::copy(out, out + 16, w);
    }
    z.zonePlacementsLinked = 1; z.collidersLinkedByGeometry = 1; z.collidersUnlinked = 0;
    z.colliderEntries = 2; z.colliderGeometries = 1;
    return s;
}

/// @brief ModelSource over static_cube (101), skinned_cube (102) and static_grass (103). Every
///        texture is a 2 x 2 mid-grey RGBA8 with mips down to 1 x 1.
class SyntheticModelSource : public castlemist::exportgltf::ModelSource {
public:
    int failIndex = -1;            ///< load() of this index returns null
    bool throwDatIo = false;       ///< texture() throws world::DatIoError
    std::atomic<int> loads{0}, releases{0};

    std::shared_ptr<ModelPreview> load(uint32_t index, uint32_t fileId) override {
        ++loads;
        if (static_cast<int>(index) == failIndex) return nullptr;
        if (fileId == 101) return std::make_shared<ModelPreview>(static_cube());
        if (fileId == 102) return std::make_shared<ModelPreview>(skinned_cube());
        if (fileId == 103) return std::make_shared<ModelPreview>(static_grass());
        return nullptr;
    }
    std::shared_ptr<const CappedTexture> texture(uint32_t fileId, int maxSize) override {
        if (throwDatIo) throw castlemist::world::DatIoError("file " + std::to_string(fileId) + ": cannot read: test");
        if (maxSize <= 0) return nullptr;
        auto t = std::make_shared<CappedTexture>();
        t->fileId = fileId;
        t->width = t->height = 2;
        t->mips.push_back(std::vector<uint8_t>(2 * 2 * 4, 128));
        t->mips.push_back(std::vector<uint8_t>(4, 128));
        return t;
    }
    void release_textures() override { ++releases; }
};

} // namespace castlemist::test_world
