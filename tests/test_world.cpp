/// @file
/// @brief Tests for the world layer (WorldScene types).

#include "test_framework.h"

#include "castlemist/world/world_scene.h"

CM_TEST(world, default_scene_is_empty) {
    castlemist::world::WorldScene scene;
    CHECK_FALSE(scene.terrain.present);
    CHECK(scene.props.empty());
    CHECK(scene.models.empty());
    CHECK(scene.collision.instances.empty());
    CHECK(scene.warnings.empty());
    CHECK_FALSE(scene.hasBounds);
    // §4.2: no chunks-per-page is assumed; 0 means unknown until pages resolve.
    CHECK_EQ(castlemist::world::TerrainMaterial{}.pickerScale, 0.0f);
}

// ---- frame (docs/research/gw2-world-frame.md §1) ----

#include "castlemist/world/frame.h"

#include <cmath>

namespace {

using ConvVec = void (*)(const float*, float*);
using ConvMat = void (*)(const float*, float*);

/// Determinant of the 3x3 whose columns are conv(east), conv(north), conv(down).
float conv_det(ConvVec conv) {
    const float e[3] = {1, 0, 0}, n[3] = {0, 1, 0}, d[3] = {0, 0, 1};
    float a[3], b[3], c[3];
    conv(e, a); conv(n, b); conv(d, c);
    return a[0] * (b[1] * c[2] - b[2] * c[1]) - b[0] * (a[1] * c[2] - a[2] * c[1]) +
           c[0] * (a[1] * b[2] - a[2] * b[1]);
}

/// Determinant of a column-major 4x4's upper-left 3x3.
float linear_det(const float m[16]) {
    return m[0] * (m[5] * m[10] - m[6] * m[9]) - m[4] * (m[1] * m[10] - m[2] * m[9]) +
           m[8] * (m[1] * m[6] - m[2] * m[5]);
}

/// p' = M p for a column-major 4x4 and a point (w = 1).
void apply(const float m[16], const float p[3], float out[3]) {
    for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
}

void check_vec(ConvVec conv, float x, float y, float z, float ex, float ey, float ez) {
    const float in[3] = {x, y, z};
    float out[3] = {9, 9, 9};
    conv(in, out);
    CHECK_EQ(out[0], ex);
    CHECK_EQ(out[1], ey);
    CHECK_EQ(out[2], ez);
}

/// conv_mat(M) * conv(p) == conv(M * p), on a rotated, scaled, translated M.
void check_matrix_commutes(ConvVec conv, ConvMat conv_mat) {
    const float a = 0.7f, b = -0.4f, s = 1.5f;
    const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
    // Rz(a) * Rx(b) * s, column-major, translation (100, -200, 300).
    const float m[16] = {
        s * ca,      s * sa,      0,      0,
        -s * sa * cb, s * ca * cb, s * sb, 0,
        s * sa * sb, -s * ca * sb, s * cb, 0,
        100,         -200,        300,    1};
    const float p[3] = {3, -5, 7};
    float mp[3], want[3], cp[3], got[3], cm[16];
    apply(m, p, mp);
    conv(mp, want);
    conv(p, cp);
    conv_mat(m, cm);
    apply(cm, cp, got);
    for (int i = 0; i < 3; ++i) CHECK_NEAR(got[i], want[i], 1e-3);
    CHECK_NEAR(cm[3], 0, 0); CHECK_NEAR(cm[7], 0, 0); CHECK_NEAR(cm[11], 0, 0); CHECK_EQ(cm[15], 1.0f);
    CHECK_NEAR(linear_det(cm), linear_det(m), 1e-4);
}

void check_identity_is_proper_rotation(ConvMat conv_mat) {
    const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float out[16];
    conv_mat(id, out);
    for (int i = 0; i < 16; ++i) CHECK_EQ(out[i], id[i]);
    CHECK_EQ(linear_det(out), 1.0f);
}

} // namespace

// §1.4: map east +X, north +Y, up -Z -> Unity +X, +Z, +Y; det +1 (LH -> LH).
CM_TEST(world, frame_to_unity_axes) {
    using castlemist::world::map_to_unity;
    using castlemist::world::map_matrix_to_unity;
    check_vec(map_to_unity, 1, 0, 0, 1, 0, 0);    // east  -> +X
    check_vec(map_to_unity, 0, 1, 0, 0, 0, 1);    // north -> +Z (forward)
    check_vec(map_to_unity, 0, 0, -1, 0, 1, 0);   // up    -> +Y
    check_vec(map_to_unity, 2, 3, 4, 2, -4, 3);   // a point
    CHECK_EQ(conv_det(map_to_unity), 1.0f);        // proper rotation
    float v[3] = {2, 3, 4};
    map_to_unity(v, v);                            // in == out
    CHECK_EQ(v[0], 2.0f); CHECK_EQ(v[1], -4.0f); CHECK_EQ(v[2], 3.0f);
    check_identity_is_proper_rotation(map_matrix_to_unity);
    check_matrix_commutes(map_to_unity, map_matrix_to_unity);
}

// §1.4: map east +X, north +Y, up -Z -> Blender +X, +Y, +Z; det -1 is the
// LH -> RH handedness change, carried by the vectors, not the matrices.
CM_TEST(world, frame_to_blender_axes) {
    using castlemist::world::map_to_blender;
    using castlemist::world::map_matrix_to_blender;
    check_vec(map_to_blender, 1, 0, 0, 1, 0, 0);   // east  -> +X
    check_vec(map_to_blender, 0, 1, 0, 0, 1, 0);   // north -> +Y
    check_vec(map_to_blender, 0, 0, -1, 0, 0, 1);  // up    -> +Z
    check_vec(map_to_blender, 2, 3, 4, 2, 3, -4);  // a point
    CHECK_EQ(conv_det(map_to_blender), -1.0f);      // the handedness change
    float v[3] = {2, 3, 4};
    map_to_blender(v, v);
    CHECK_EQ(v[0], 2.0f); CHECK_EQ(v[1], 3.0f); CHECK_EQ(v[2], -4.0f);
    check_identity_is_proper_rotation(map_matrix_to_blender);
    check_matrix_commutes(map_to_blender, map_matrix_to_blender);
}

// ---- terrain (docs/research/gw2-world-frame.md §3) ----

#include "castlemist/world/terrain.h"

#include <string>
#include <vector>

// §3.2: 4 x 6 chunks of 32 segments, 35 stored samples per side.
CM_TEST(world, terrain_layout_from_dims_and_count) {
    auto l = castlemist::world::terrain_layout(128, 192, 32, 24 * 35 * 35);
    CHECK(l.ok);
    CHECK_EQ(l.chunksX, 4);
    CHECK_EQ(l.chunksY, 6);
    CHECK_EQ(l.segments, 32);
    CHECK_EQ(l.stored, 35);
    CHECK(l.why.empty());
}

// A sample count that is not a whole number of (segments+3)^2 chunks is rejected.
CM_TEST(world, terrain_layout_rejects_odd_sample_count) {
    auto l = castlemist::world::terrain_layout(128, 192, 32, 24 * 35 * 35 + 1);
    CHECK_FALSE(l.ok);
    CHECK_FALSE(l.why.empty());
    auto l0 = castlemist::world::terrain_layout(128, 192, 0, 24 * 35 * 35 + 1);   // no field, nothing solves
    CHECK_FALSE(l0.ok);
    CHECK_FALSE(l0.why.empty());
}

// §3.1/§3.3 on a hand-built 2 x 1 chunk map, segments 2, stored 5. Stored
// sample (row r, column c) of chunk k has the value 100*r + (2k + c): the
// three overlapping columns of the two chunks hold the same values (as the
// real data does), every other sample of a chunk is distinct.
CM_TEST(world, terrain_detile_hand_grid) {
    castlemist::model::Extractor::MapTerrain t;
    t.present = true;
    t.dimX = 4; t.dimY = 2; t.vertsPerChunkSide = 2;
    t.rect[0] = 0; t.rect[1] = 0; t.rect[2] = 200; t.rect[3] = 100; t.hasRect = true;
    for (int k = 0; k < 2; ++k)
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < 5; ++c) t.heights.push_back(float(100 * r + 2 * k + c));
    std::vector<std::string> warnings;
    auto terr = castlemist::world::build_terrain(t, warnings);
    CHECK(warnings.empty());
    CHECK(terr.present);
    CHECK_EQ(terr.chunksX, 2);
    CHECK_EQ(terr.chunksY, 1);
    CHECK_EQ(terr.chunks.size(), size_t(2));
    if (terr.chunks.size() != 2) return;
    for (int k = 0; k < 2; ++k) {
        const auto& ch = terr.chunks[k];
        CHECK_EQ(ch.cx, k);
        CHECK_EQ(ch.cy, 0);
        CHECK_EQ(ch.samples, 3);
        CHECK_EQ(ch.rect[0], 100.0f * k);
        CHECK_EQ(ch.rect[1], 0.0f);
        CHECK_EQ(ch.rect[2], 100.0f * (k + 1));
        CHECK_EQ(ch.rect[3], 100.0f);
        CHECK_EQ(ch.heights.size(), size_t(9));
        if (ch.heights.size() != 9) return;
        // The inner 3 x 3: stored rows/columns 1..3.
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) CHECK_EQ(ch.heights[j * 3 + i], float(100 * (j + 1) + 2 * k + i + 1));
    }
    // Shared edge: chunk 0's east column == chunk 1's west column.
    for (int j = 0; j < 3; ++j) CHECK_EQ(terr.chunks[0].heights[j * 3 + 2], terr.chunks[1].heights[j * 3 + 0]);
    // Row 0 is the north edge (largest y), column 0 the west edge.
    bool inside = false;
    CHECK_EQ(castlemist::world::terrain_height_at(terr, 0, 100, &inside), 101.0f);
    CHECK(inside);
    CHECK_EQ(castlemist::world::terrain_height_at(terr, 0, 0, &inside), 301.0f);
    CHECK_EQ(castlemist::world::terrain_height_at(terr, 200, 0, &inside), 305.0f);
    CHECK(inside);
    // Bilinear: halfway between samples (row 1, col 0) = 201 and (row 1, col 1) = 202.
    CHECK_NEAR(castlemist::world::terrain_height_at(terr, 25, 50, &inside), 201.5, 1e-4);
    castlemist::world::terrain_height_at(terr, -1, 50, &inside);
    CHECK_FALSE(inside);
    castlemist::world::terrain_height_at(terr, 50, 100.5f, &inside);
    CHECK_FALSE(inside);
}

// §2: no parm rect -> no terrain and a warning; no rect is invented.
CM_TEST(world, terrain_no_rect_warns) {
    castlemist::model::Extractor::MapTerrain t;
    t.present = true;
    t.dimX = 4; t.dimY = 2; t.vertsPerChunkSide = 2;
    t.heights.assign(50, 1.0f);
    t.hasRect = false;
    std::vector<std::string> warnings;
    auto terr = castlemist::world::build_terrain(t, warnings);
    CHECK_FALSE(terr.present);
    CHECK(terr.chunks.empty());
    bool found = false;
    for (const auto& w : warnings) found = found || w.rfind("terrain: no parm rect", 0) == 0;
    CHECK(found);
    CHECK_EQ(warnings.size(), size_t(1));   // the one missing-rect line (load_world adds none)
    // No samples either: the rect is still named, with the missing samples.
    t.heights.clear();
    warnings.clear();
    castlemist::world::build_terrain(t, warnings);
    CHECK_EQ(warnings.size(), size_t(2));
    CHECK(!warnings.empty() && warnings[0].rfind("terrain: no parm rect", 0) == 0);
}

// §3.2: with verticesPerChunkSide present, dims = chunks * segments is checked.
// 8 x 4 dims, 2 segments, two 5 x 5 chunks: the sample count gives a 2 x 1
// grid, which is 4 x 2 quads, not 8 x 4. No terrain, and the reason is named.
CM_TEST(world, terrain_dims_not_chunks_times_segments_warns) {
    castlemist::model::Extractor::MapTerrain t;
    t.present = true;
    t.dimX = 8; t.dimY = 4; t.vertsPerChunkSide = 2;
    t.rect[0] = 0; t.rect[1] = 0; t.rect[2] = 200; t.rect[3] = 100; t.hasRect = true;
    t.heights.assign(50, 1.0f);
    const auto l = castlemist::world::terrain_layout(t.dimX, t.dimY, t.vertsPerChunkSide, t.heights.size());
    CHECK_FALSE(l.ok);
    CHECK(l.why.find("not chunks 2x1 times verticesPerChunkSide 2") != std::string::npos);
    std::vector<std::string> warnings;
    auto terr = castlemist::world::build_terrain(t, warnings);
    CHECK_FALSE(terr.present);
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK(!warnings.empty() && warnings[0].rfind("terrain: dims 8x4 are not chunks 2x1", 0) == 0);
    // The consistent map (dims 4 x 2) still lays out.
    CHECK(castlemist::world::terrain_layout(4, 2, 2, 50).ok);
    // A chunk count that is no exact grid for the aspect is rejected (integer check):
    // 3 chunks at 4:2 would need chunksX = sqrt(6).
    CHECK_FALSE(castlemist::world::terrain_layout(4, 2, 2, 75).ok);
}

// ---- terrain materials (docs/research/gw2-world-frame.md §4) ----

namespace {

/// @brief A GW2 Token (base-23, §4.1) for a material texture name.
uint32_t token_of(const std::string& s) {
    static const std::string kAlpha = "abcdefghiklmnopvrstuwxy";
    uint32_t v = 0, p = 1;
    for (char c : s) { v += (uint32_t)kAlpha.find(c) * p; p *= 23; }
    return v + 0x30000000u;
}

} // namespace

// §4.1: a chunk whose texIndexArray points past texFileArray is left
// unresolved and named in a warning; its neighbour still resolves.
CM_TEST(world, terrain_material_index_out_of_range) {
    castlemist::model::Extractor::MapTerrain t;
    t.present = true;
    t.dimX = 4; t.dimY = 2; t.vertsPerChunkSide = 2;
    t.rect[0] = 0; t.rect[1] = 0; t.rect[2] = 200; t.rect[3] = 100; t.hasRect = true;
    t.heights.assign(50, 1.0f);
    std::vector<std::string> warnings;
    auto terr = castlemist::world::build_terrain(t, warnings);
    CHECK_EQ(terr.chunks.size(), size_t(2));
    if (terr.chunks.size() != 2) return;

    castlemist::model::Extractor::MapTerrainMaterials m;
    m.present = true;
    const char* names[] = {"color", "colorb", "colorc", "colord"};
    for (int k = 0; k < 4; ++k) {
        castlemist::model::Extractor::MapTerrainMaterials::Tex tx;
        tx.token = token_of(names[k]);
        tx.flags = 1;
        tx.fileId = 11 + k;
        tx.layer = 0xFFFFFFFFu;
        m.texFiles.push_back(tx);
        m.texFileIds.push_back(tx.fileId);
    }
    m.chunks.resize(2);
    m.chunks[0].materialFileId = 7;
    m.chunks[0].texIndices = {0, 1, 2, 3};
    m.chunks[1].materialFileId = 7;
    m.chunks[1].texIndices = {0, 1, 2, (uint32_t)m.texFileIds.size()};
    m.chunks[1].hasUvData = true;

    Gw2Dat dat;   // never read: the map has no paged image
    warnings.clear();
    castlemist::world::resolve_terrain_materials(terr, m, dat, nlohmann::json::object(), warnings);
    CHECK(terr.chunks[0].material.resolved);
    CHECK(terr.chunks[0].material.textureFileIds == std::vector<uint32_t>({11, 12, 13, 14}));
    CHECK_EQ(terr.chunks[0].material.materialFileId, uint32_t(7));
    CHECK_FALSE(terr.chunks[1].material.resolved);
    bool named = false;
    for (const auto& w : warnings) named = named || w.find("chunk 1 ") != std::string::npos;
    CHECK(named);
    // §4.1, §4.4: what is kept but UNPROVEN is one aggregated line each.
    auto count = [&](const std::string& prefix) {
        size_t n = 0;
        for (const auto& w : warnings) n += w.rfind(prefix, 0) == 0 && w.find("UNPROVEN") != std::string::npos;
        return n;
    };
    CHECK_EQ(count("terrain materials: uvScale UNPROVEN on all 2 chunks"), size_t(1));
    CHECK_EQ(count("terrain materials: tiling bytes kept as stored on 2 chunks"), size_t(1));
    CHECK_EQ(count("terrain materials: materialFileId is loResMaterial on 2 chunks"), size_t(1));
    CHECK_EQ(count("terrain materials: 1 chunks have a non-null uvData"), size_t(1));
    CHECK_EQ(count("terrain materials: 0 chunk blend pages"), size_t(0));   // no solid pages: no line
    CHECK_EQ(terr.chunks[0].material.pickerScale, 0.0f);                   // no pages: unknown
}

// ---- props (docs/research/gw2-world-frame.md §5) ----

#include "castlemist/world/props.h"

#include <cmath>

namespace {

castlemist::model::Extractor::MapProp make_prop(uint32_t id, const char* group, float x = 0, float y = 0, float z = 0) {
    castlemist::model::Extractor::MapProp p;
    p.fileId = id;
    p.pos[0] = x; p.pos[1] = y; p.pos[2] = z;
    p.group = group;
    return p;
}

} // namespace

// §5: the world matrix is the client's float3x4 (Gw2-64.exe, written out in
// src/render/detail/math.h:168-172) times the scale, column-major.
CM_TEST(world, prop_transform_matches_client) {
    auto p = make_prop(7, "propArray", 10, 20, 30);
    p.rot[0] = 0.3f; p.rot[1] = -0.2f; p.rot[2] = 1.1f;
    p.scale = 2.0f;
    castlemist::world::WorldScene scene;
    castlemist::world::build_props({p}, scene);
    CHECK_EQ(scene.props.size(), size_t(1));
    if (scene.props.size() != 1) return;

    const double cx = std::cos(0.3), sx = std::sin(0.3), cy = std::cos(-0.2), sy = std::sin(-0.2);
    const double cz = std::cos(1.1), sz = std::sin(1.1);
    // The client's rows [r0 r1 r2 | t], column vectors p' = M*p + t.
    // (cx..sz above are cos/sin of rot[0..2] = 0.3, -0.2, 1.1.)
    const double C[3][3] = {
        {cz * cy - sy * sx * sz, cz * sx * sy + sz * cy, -cx * sy},
        {-cx * sz, cz * cx, sx},
        {cy * sx * sz + cz * sy, sz * sy - cz * cy * sx, cy * cx}};
    const double t[3] = {10, 20, 30};
    const float* w = scene.props[0].world;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) CHECK_NEAR(w[col * 4 + row], 2.0 * C[row][col], 1e-6);
        CHECK_NEAR(w[12 + row], t[row], 1e-6);
    }
    CHECK_NEAR(w[3], 0, 0); CHECK_NEAR(w[7], 0, 0); CHECK_NEAR(w[11], 0, 0); CHECK_NEAR(w[15], 1, 0);
    CHECK_NEAR(scene.props[0].pos[0], 10, 0);
    CHECK_NEAR(scene.props[0].rot[2], 1.1, 1e-6);
    CHECK_NEAR(scene.props[0].scale, 2, 0);
}

CM_TEST(world, props_dedupe_models) {
    castlemist::world::WorldScene scene;
    castlemist::world::build_props({make_prop(100, "propArray"), make_prop(200, "propArray"),
                                    make_prop(100, "propInstanceArray")}, scene);
    CHECK_EQ(scene.models.size(), size_t(2));
    CHECK_EQ(scene.props.size(), size_t(3));
    if (scene.models.size() != 2 || scene.props.size() != 3) return;
    CHECK_EQ(scene.models[0].fileId, uint32_t(100));
    CHECK_EQ(scene.models[1].fileId, uint32_t(200));
    CHECK_EQ(scene.props[0].model, uint32_t(0));
    CHECK_EQ(scene.props[1].model, uint32_t(1));
    CHECK_EQ(scene.props[2].model, uint32_t(0));
    CHECK(scene.props[2].group == "propInstanceArray");
}

CM_TEST(world, anim_props_recorded) {
    castlemist::world::WorldScene scene;
    castlemist::world::build_props({make_prop(1, "propArray"), make_prop(2, "propAnimArray"),
                                    make_prop(3, "propMetaArray"), make_prop(4, "propAnimArray")}, scene);
    CHECK_EQ(scene.motion.animatedProps.size(), size_t(2));
    if (scene.motion.animatedProps.size() != 2) return;
    CHECK_EQ(scene.motion.animatedProps[0], uint32_t(1));
    CHECK_EQ(scene.motion.animatedProps[1], uint32_t(3));
}

// ---- collision (docs/research/gw2-world-frame.md §7) ----

#include "castlemist/world/collision.h"

// §7: a placement whose geometryIndex is out of range, and one whose
// animation names a collision index out of range, are skipped: no instance,
// one aggregated warning, no throw.
CM_TEST(world, collision_bad_indices_skipped) {
    castlemist::model::Extractor::MapHavok h;
    h.present = true;
    h.hulls.resize(1);
    h.hulls[0].verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    h.hulls[0].indices = {0, 1, 2};
    h.geometryAnimations = {{0}};
    h.animationCollisions = {{5}};             // hull 5 does not exist
    castlemist::model::Extractor::HavokPlacement badGeometry;
    badGeometry.geometryIndex = 7;             // only geometry 0 exists
    badGeometry.group = "obs";
    castlemist::model::Extractor::HavokPlacement badCollision;
    badCollision.geometryIndex = 0;
    badCollision.group = "prop";
    h.placements = {badGeometry, badCollision};

    castlemist::world::WorldScene scene;
    castlemist::world::build_collision(h, scene);
    CHECK(scene.collision.instances.empty());
    size_t mentions = 0;
    for (const auto& w : scene.warnings)
        if (w.find("collision: 2 placements") != std::string::npos) ++mentions;
    CHECK_EQ(scene.warnings.size(), size_t(1));
    CHECK_EQ(mentions, size_t(1));
}

// §7.1-7.2: what is kept but suspect is counted in one warning each: a
// placement whose sequence names an animation other than animations[last],
// a geometry with no animations, and scale 0.
CM_TEST(world, collision_suspect_placements_warned) {
    castlemist::model::Extractor::MapHavok h;
    h.present = true;
    h.hulls.resize(2);
    h.geometryAnimations = {{0, 1}, {}};
    h.animationCollisions = {{0}, {1}};
    h.animationSequences = {100, 200};
    auto place = [](uint32_t g, bool hasSeq, uint64_t seq, float scale) {
        castlemist::model::Extractor::HavokPlacement p;
        p.geometryIndex = g; p.group = "prop"; p.hasSequence = hasSeq; p.sequence = seq; p.scale = scale;
        return p;
    };
    h.placements = {place(0, true, 100, 1),    // names animation 0, not the last: warned
                    place(0, true, 200, 1),    // names the last: fine
                    place(0, false, 0, 0),     // no sequence field; scale 0: warned
                    place(1, true, 100, 1)};   // geometry with no animations: warned
    castlemist::world::WorldScene scene;
    castlemist::world::build_collision(h, scene);
    CHECK_EQ(scene.collision.instances.size(), size_t(3));
    auto has = [&](const char* text) {
        size_t n = 0;
        for (const auto& w : scene.warnings) n += w.find(text) != std::string::npos;
        return n;
    };
    CHECK_EQ(scene.warnings.size(), size_t(3));
    CHECK_EQ(has("collision: 1 placements use animations[last]"), size_t(1));
    CHECK_EQ(has("collision: 1 placements name a geometry with no animations"), size_t(1));
    CHECK_EQ(has("collision: 1 placements have scale 0"), size_t(1));
}

// ---- water (docs/research/gw2-world-frame.md §6) ----

#include "castlemist/world/load_world.h"

namespace {

/// @brief How many of `s.warnings` start with `prefix`.
size_t warnings_starting(const castlemist::world::WorldScene& s, const std::string& prefix) {
    size_t n = 0;
    for (const auto& w : s.warnings) n += w.rfind(prefix, 0) == 0;
    return n;
}

} // namespace

// §6: a V0 watr (no waterSurfaces field) is named, not silently empty, and
// gives no plane.
CM_TEST(world, water_v0_warns) {
    castlemist::world::WaterSources in;
    in.watr.chunk = true;
    in.watr.version = 0;
    castlemist::world::WorldScene scene;
    castlemist::world::build_water(in, scene);
    CHECK(!scene.water.hasPlane);
    CHECK(scene.water.surfaces.empty());
    CHECK_EQ(warnings_starting(scene, "watr: V0 not read"), size_t(1));
}

// §6: no water data means no water and no warning: nothing is invented.
CM_TEST(world, water_absent_is_empty) {
    castlemist::world::WorldScene scene;
    scene.water.hasPlane = true;   // replaced, not kept
    castlemist::world::build_water({}, scene);
    CHECK(!scene.water.hasPlane);
    CHECK(!scene.water.hasHavkSurfaceZ);
    CHECK(scene.water.surfaces.empty());
    CHECK(scene.water.rivers.empty());
    CHECK(!scene.water.shore.present);
    CHECK(scene.warnings.empty());
}

// §6: the plane, havk height, surfaces, rivers and shore are copied as
// stored; the plane's coverage, the surfaces' flags, the rivers' unread
// properties, havk water volumes and env water presets are named.
CM_TEST(world, water_copies_sources_and_names_gaps) {
    castlemist::world::WaterSources in;
    in.watr.chunk = true;
    in.watr.version = 1;
    in.watr.hasSurfacesField = true;
    in.watr.hasPlane = true;
    in.watr.planeZ = -12.5f;
    in.watr.flags = 3;
    castlemist::model::Extractor::MapWaterSurface s;
    s.z = 7;
    s.flags = 2;
    s.points = {0, 0, 1, 0, 1, 1};
    in.watr.surfaces = {s};
    in.watr.present = true;
    in.havk.hasSurfaceZ = true;
    in.havk.surfaceZ = -12.5f;
    in.havk.waterVolumes = 2;
    castlemist::model::Extractor::MapRivers::River r;
    r.name = "Brook";
    r.points = {1, 2, 3, 4, 5, 6};
    in.rivers.rivers = {r};
    in.rivers.present = true;
    in.shore.present = true;
    in.shore.chains.resize(1);
    in.envWaterPresets = 6;
    castlemist::world::WorldScene scene;
    castlemist::world::build_water(in, scene);
    const auto& w = scene.water;
    CHECK(w.hasPlane);
    CHECK_EQ(w.planeZ, -12.5f);
    CHECK_EQ(w.planeFlags, uint32_t(3));
    CHECK(w.hasHavkSurfaceZ);
    CHECK_EQ(w.havkSurfaceZ, -12.5f);
    CHECK_EQ(w.surfaces.size(), size_t(1));
    CHECK_EQ(w.surfaces[0].z, 7.0f);
    CHECK(w.surfaces[0].points == s.points);
    CHECK_EQ(w.rivers.size(), size_t(1));
    CHECK_EQ(w.rivers[0].name, std::string("Brook"));
    CHECK(w.rivers[0].points == r.points);
    CHECK(w.shore.present);
    CHECK_EQ(w.shore.chains.size(), size_t(1));
    CHECK_EQ(warnings_starting(scene, "watr: waterPlaneZ has no outline"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "watr: waterFlags 3 kept as stored; meaning UNPROVEN"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "shor: 1 shore chains read with field names after T3D's SHOR.ts, UNPROVEN"),
             size_t(1));
    CHECK_EQ(warnings_starting(scene, "watr: 1 waterSurfaces"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "rive: 1 rivers"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "havk: 2 waterVolumes not read"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "env: 6 water presets"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "watr: V0 not read"), size_t(0));
    CHECK_EQ(warnings_starting(scene, "water: watr waterPlaneZ"), size_t(0));   // the two heights agree
}

// §6: the watr plane and the havk water height disagreeing is named; both
// are kept as stored.
CM_TEST(world, water_plane_havk_disagree_warns) {
    castlemist::world::WaterSources in;
    in.watr.chunk = true;
    in.watr.version = 1;
    in.watr.hasSurfacesField = true;
    in.watr.hasPlane = true;
    in.watr.planeZ = 0;
    in.havk.hasSurfaceZ = true;
    in.havk.surfaceZ = 40;
    castlemist::world::WorldScene scene;
    castlemist::world::build_water(in, scene);
    CHECK_EQ(scene.water.planeZ, 0.0f);
    CHECK_EQ(scene.water.havkSurfaceZ, 40.0f);
    CHECK_EQ(warnings_starting(scene, "water: watr waterPlaneZ"), size_t(1));
}

// §6: parts of a present chunk that could not be read are named: a watr
// version without a template struct (instead of the V0 warning), and rivers
// whose points are not float3.
CM_TEST(world, water_unread_parts_named) {
    castlemist::world::WaterSources in;
    in.watr.chunk = true;
    in.watr.version = 7;
    in.watr.problem = "no template struct for watr v7; chunk not read";
    in.rivers.droppedPoints = 2;
    castlemist::world::WorldScene scene;
    castlemist::world::build_water(in, scene);
    CHECK_EQ(warnings_starting(scene, "watr: no template struct for watr v7"), size_t(1));
    CHECK_EQ(warnings_starting(scene, "watr: V0 not read"), size_t(0));
    CHECK_EQ(warnings_starting(scene, "rive: 2 rivers' points not read"), size_t(1));
    CHECK(!scene.water.hasPlane);
}

// §6.5: the sky and the one lighting preset are kept; the other presets and
// the per-zone blocks are named with their counts.
CM_TEST(world, environment_names_unattached) {
    castlemist::world::EnvSources in;
    in.sky.present = true;
    in.sky.starFile = 42;
    in.light.present = true;
    in.light.lightCount = 2;
    in.counts.present = true;
    in.counts.lightingPresets = 3;
    in.counts.localBlocks = 12;
    in.counts.overrideBlocks = 1;
    castlemist::world::WorldScene scene;
    castlemist::world::build_environment(in, scene);
    CHECK(scene.environment.sky.present);
    CHECK_EQ(scene.environment.sky.starFile, uint32_t(42));
    CHECK_EQ(scene.environment.light.lightCount, 2);
    CHECK_EQ(scene.warnings.size(), size_t(1));
    CHECK_EQ(warnings_starting(scene, "environment: 2 other dataGlobal lighting presets and 13 per-zone blocks "
                                      "(12 dataLocalArray, 1 dataOverrideArray"),
             size_t(1));

    castlemist::world::WorldScene empty;
    castlemist::world::build_environment({}, empty);
    CHECK_EQ(empty.warnings.size(), size_t(2));
    CHECK_EQ(warnings_starting(empty, "environment: no sky"), size_t(1));
    CHECK_EQ(warnings_starting(empty, "environment: no light rig"), size_t(1));
}

// ---- load_world's error policy (no dat needed) ----

#include "castlemist/world/load_world.h"

#include <stdexcept>

// A section that throws is named, and the sections after it still run.
CM_TEST(world, run_section_names_a_failure_and_carries_on) {
    std::vector<std::string> warnings;
    int ran = 0;
    castlemist::world::run_section(warnings, "props", [&] { throw std::runtime_error("boom"); });
    castlemist::world::run_section(warnings, "collision", [&] { ++ran; });
    CHECK_EQ(ran, 1);
    CHECK_EQ(warnings.size(), size_t(1));
    CHECK_EQ(warnings[0], std::string("props: exception: boom; section left empty"));
}

// Dat I/O failure is an error everywhere: run_section rethrows DatIoError
// (and adds no warning for it), while any other exception -- bad data, such
// as a secondary file that does not decompress -- still becomes a warning.
CM_TEST(world, run_section_rethrows_dat_io_error) {
    std::vector<std::string> warnings;
    bool rethrown = false;
    try {
        castlemist::world::run_section(warnings, "terrain materials", [&] {
            throw castlemist::world::DatIoError("file 191359: cannot read: short read");
        });
    } catch (const castlemist::world::DatIoError& e) {
        rethrown = std::string(e.what()) == "file 191359: cannot read: short read";
    }
    CHECK(rethrown);
    CHECK(warnings.empty());
    castlemist::world::run_section(warnings, "terrain materials", [&] {
        throw std::runtime_error("huffman decode failed");
    });
    CHECK_EQ(warnings.size(), size_t(1));
}

// The spec: a map with no props or no collision is valid, and warned.
CM_TEST(world, absent_chunk_warnings_name_missing_props_and_collision) {
    using Chunks = std::vector<std::pair<std::string, uint16_t>>;
    const auto none = castlemist::world::absent_chunk_warnings(Chunks{{"parm", 1}, {"trn", 2}});
    CHECK_EQ(none.size(), size_t(2));
    if (none.size() == 2) {
        CHECK_EQ(none[0], std::string("props: no prp2 chunk; map has no props"));
        CHECK_EQ(none[1], std::string("collision: no havk chunk; map has no collision"));
    }
    CHECK(castlemist::world::absent_chunk_warnings(Chunks{{"prp2", 3}, {"havk", 4}}).empty());
}

// A template without `types` is its own error, thrown before the dat is touched.
CM_TEST(world, missing_template_throws) {
    Gw2Dat dat;
    bool threw = false;
    try {
        castlemist::world::load_world(dat, 192711, nlohmann::json::object());
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()) == "struct template missing 'types'";
    }
    CHECK(threw);
}

// Read chunks are never listed; each unread chunk is listed once.
CM_TEST(world, unread_chunk_warnings_skip_consumed_and_dedupe) {
    const std::vector<std::pair<std::string, uint16_t>> chunks = {
        {"parm", 1}, {"trn", 2}, {"prp2", 3}, {"havk", 4}, {"env", 5}, {"watr", 1}, {"shor", 1}, {"rive", 1},
        {"zon2", 22}, {"zon2", 22}, {"dcal", 10}};
    const auto w = castlemist::world::unread_chunk_warnings(chunks);
    CHECK_EQ(w.size(), size_t(2));
    CHECK_EQ(w[0], std::string("chunk zon2 v22 not read"));
    CHECK_EQ(w[1], std::string("chunk dcal v10 not read"));
}
