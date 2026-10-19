/// @file
/// @brief Tests for world/zones.h: the zon2 decode, the anchor and geometry
///        link of havk zoneModels entries, the prop cross-check, the
///        instances and their matrices, the shared model table, and the
///        thinning hashes (docs/superpowers/specs/2026-10-16-world-foliage-design.md
///        D2-D4, D9, Testing).

#include "test_framework.h"

#include "castlemist/world/collision.h"
#include "castlemist/world/load_world.h"
#include "castlemist/world/props.h"
#include "castlemist/world/stream.h"
#include "castlemist/world/zone_scatter.h"
#include "castlemist/world/zones.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace castlemist::world;
using Ex = castlemist::model::Extractor;

namespace {

constexpr float kX0 = -1000.0f, kY0 = -2000.0f;
constexpr uint32_t kA = 501, kB = 502, kC = 503;
constexpr uint64_t kGuid = 0xABCDEF0123ull;

// Flag bytes: layer (1-based) in the high nibble, slot in bits 1-3.
constexpr uint8_t kL1S0 = 0x11, kL1S1 = 0x13, kL3 = 0x31;

/// (count, value) run-length pairs (the client decode) for @p points ((c, r) -> flag, in cell
/// order) in a zone of width @p w with vertRect origin (0, 0).
std::vector<uint8_t> rle_flags(const std::vector<std::pair<std::pair<int, int>, uint8_t>>& points, int w) {
    std::vector<uint8_t> out;
    uint32_t at = 0;
    for (const auto& [cr, f] : points) {
        const uint32_t k = uint32_t(cr.second * w + cr.first);
        uint32_t gap = k - at;
        while (gap > 255) { out.push_back(255); out.push_back(0); gap -= 255; }
        if (gap) { out.push_back(uint8_t(gap)); out.push_back(0); }
        out.push_back(1);
        out.push_back(f);
        at = k + 1;
    }
    return out;
}

/// The synthetic map: one def (token 7) of two layers, type 3 {A, B} and
/// type 1 {C}, and one zone with vertRect {0, 0, 10, 10} holding @p points
/// ((c, r) -> flag, in walk order).
Ex::MapZon2 make_zon2(const std::vector<std::pair<std::pair<int, int>, uint8_t>>& points) {
    Ex::MapZon2 z;
    z.present = true;
    z.hasRect = true;
    z.rect[0] = kX0; z.rect[1] = kY0; z.rect[2] = 8000; z.rect[3] = 8000;
    Ex::Zon2Def d;
    d.token = 7;
    Ex::Zon2Layer trees;
    trees.type = 3;
    trees.models.resize(2);
    trees.models[0].fileId = kA;
    trees.models[1].fileId = kB;
    Ex::Zon2Layer grass;
    grass.type = 1;
    grass.models.resize(1);
    grass.models[0].fileId = kC;
    d.layers = {trees, grass};
    z.defs.push_back(d);

    Ex::Zon2Zone zone;
    zone.defToken = 7;
    zone.vertRect[0] = 0; zone.vertRect[1] = 0; zone.vertRect[2] = 10; zone.vertRect[3] = 10;
    zone.flags = rle_flags(points, 10);
    zone.encodeData.resize(10);   // present: flags are (count, value) pairs (the walk does not read it)
    z.zones.push_back(zone);
    return z;
}

/// A scene with the parm rect and one prop of fileId A (guid kGuid), far from the zones.
WorldScene make_scene() {
    WorldScene s;
    s.hasBounds = true;
    s.bounds[0] = kX0; s.bounds[1] = kY0; s.bounds[2] = 8000; s.bounds[3] = 8000;
    Ex::MapProp p;
    p.fileId = kA;
    p.pos[0] = 7000; p.pos[1] = 7000;
    p.guid = kGuid;
    p.group = "propArray";
    build_props({p}, s);
    return s;
}

/// The anchor of (c, r) plus (dx, dy).
Ex::HavokPlacement zone_entry(int c, int r, float dx, float dy, uint32_t geometry) {
    float a[2];
    zone_anchor(kX0, kY0, c, r, a);
    Ex::HavokPlacement e;
    e.group = "zone";
    e.geometryIndex = geometry;
    e.translate[0] = a[0] + dx; e.translate[1] = a[1] + dy; e.translate[2] = -50;
    return e;
}

Ex::HavokPlacement far_entry(uint32_t geometry) {
    Ex::HavokPlacement e;
    e.group = "zone";
    e.geometryIndex = geometry;
    e.translate[0] = 5000; e.translate[1] = 5000;
    return e;
}

Ex::MapHavok havok(std::vector<Ex::HavokPlacement> entries) {
    Ex::MapHavok h;
    h.present = true;
    h.placements = std::move(entries);
    return h;
}

size_t zone_warnings(const WorldScene& s) {
    return size_t(std::count_if(s.warnings.begin(), s.warnings.end(),
                                [](const std::string& w) { return w.rfind("zone:", 0) == 0; }));
}

/// Zone warnings that contain every one of @p parts.
size_t zone_warnings_with(const WorldScene& s, std::initializer_list<const char*> parts) {
    size_t n = 0;
    for (const std::string& w : s.warnings) {
        if (w.rfind("zone:", 0) != 0) continue;
        bool all = true;
        for (const char* p : parts) all = all && w.find(p) != std::string::npos;
        n += all;
    }
    return n;
}

const ZoneInstance* instance_of(const WorldScene& s, uint32_t collider) {
    for (const ZoneInstance& i : s.zones.instances)
        if (i.collider == collider) return &i;
    return nullptr;
}

uint32_t file_of(const WorldScene& s, const ZoneInstance* i) {
    return i && i->model < s.models.size() ? s.models[i->model].fileId : 0;
}

} // namespace

// 0x13: layer 1, slot 1 -> layer 0, model B. 0x31: layer 3 of a two-layer def.
CM_TEST(zones, decode_layer_and_slot) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{5, 7}, kL1S1}, {{6, 7}, kL3}}), Ex::MapHavok{}, {}, s);
    const ZoneScatter& z = s.zones;
    CHECK(z.present);
    CHECK_EQ(z.layers.size(), size_t(2));
    CHECK_EQ(z.layers[1].indexInDef, 1u);
    CHECK_EQ(z.layers[1].defToken, 7u);
    CHECK_EQ(z.zones.size(), size_t(1));
    CHECK_EQ(z.zones[0].firstPlacement, 0u);
    CHECK_EQ(z.zones[0].placementCount, 2u);
    CHECK_EQ(z.placements.size(), size_t(2));
    const ZonePlacement& p = z.placements[0];
    CHECK_EQ(p.c, 5);
    CHECK_EQ(p.r, 7);
    CHECK_EQ(p.flag, kL1S1);
    CHECK_EQ(p.layer, 0u);
    CHECK_EQ(z.layers[p.layer].models[(p.flag >> 1) & 7].fileId, kB);
    CHECK_EQ(z.placements[1].layer, UINT32_MAX);
    CHECK_EQ(z.placementsUnresolved, 1u);
    CHECK_EQ(zone_warnings(s), size_t(1));
    CHECK(z.instances.empty());
}

CM_TEST(zones, anchor_formula) {
    float a[2];
    zone_anchor(-1000, -2000, 5, 7, a);
    CHECK_EQ(a[0], -760.0f);   // x0 + 48 c (the client decode; phase 1's c - 1 was the old walk's off-by-one)
    CHECK_EQ(a[1], -1664.0f);
}

// ±24 per axis: (23, -23) links, (25, 0) does not.
CM_TEST(zones, anchor_link_tolerance) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{2, 2}, kL1S1}, {{5, 7}, kL1S1}}),
                havok({zone_entry(5, 7, 23, -23, 4), zone_entry(2, 2, 25, 0, 5)}), {}, s);
    const ZoneScatter& z = s.zones;
    CHECK_EQ(z.placements[1].collider, 0u);
    CHECK_EQ(z.placements[0].collider, UINT32_MAX);
    CHECK_EQ(z.zonePlacementsLinked, 1u);
    const ZoneInstance* i = instance_of(s, 0);
    CHECK(i != nullptr);
    CHECK_EQ(file_of(s, i), kB);
    if (i) {
        CHECK_EQ(i->placement, 1u);
        CHECK_EQ(i->layer, 0u);
        CHECK(i->source == ZoneSource::Collider);
    }
    CHECK(instance_of(s, 1) == nullptr);   // geometry 5 has no matched entry either
    CHECK_EQ(z.collidersUnlinked, 1u);
}

// On the 48-unit grid with ±24, a point within reach of two placements is
// equidistant from both (it lies on their shared cell edge), so the nearest
// rule falls through to the lower placement index. The next entry at the same
// point takes the other placement: a claimed one is not reused.
CM_TEST(zones, tie_nearest_then_claimed) {
    WorldScene s = make_scene();
    // Walk order: indices 0, 1, 2, 3 = (2, 2), (3, 2), (5, 7), (6, 7).
    build_zones(make_zon2({{{2, 2}, kL1S1}, {{3, 2}, kL1S0}, {{5, 7}, kL1S1}, {{6, 7}, kL1S0}}),
                havok({zone_entry(5, 7, 24, 0, 1), zone_entry(5, 7, 24, 0, 2), zone_entry(3, 2, -10, 0, 3),
                       zone_entry(2, 2, 24, 0, 4)}),
                {}, s);
    const ZoneScatter& z = s.zones;
    CHECK_EQ(z.placements[2].c, 5);
    CHECK_EQ(z.placements[2].collider, 0u);   // tie -> lower index
    CHECK_EQ(z.placements[3].collider, 1u);   // claimed (5, 7) is not reused
    CHECK_EQ(z.placements[1].collider, 2u);   // (3, 2) is the only placement within reach
    CHECK_EQ(z.placements[0].collider, 3u);   // (3, 2) is claimed, so the tie goes to (2, 2)
    CHECK_EQ(z.zonePlacementsLinked, 4u);
    CHECK_EQ(file_of(s, instance_of(s, 0)), kB);
    CHECK_EQ(file_of(s, instance_of(s, 1)), kA);
}

// Anchor candidates are type-3 placements (M2): a type-1 placement in the
// same cell, with the lower index, does not take the collider.
CM_TEST(zones, anchor_candidates_are_type3) {
    WorldScene s = make_scene();
    Ex::MapZon2 z = make_zon2({{{5, 7}, kL1S1}});
    Ex::Zon2Zone grass = z.zones[0];
    grass.flags = {75, 0, 1, 0x21};   // (5, 7): layer 2 (type 1), slot 0, bit 0
    z.zones.insert(z.zones.begin(), grass);
    build_zones(z, havok({zone_entry(5, 7, 0, 0, 1)}), {}, s);
    CHECK_EQ(s.zones.placements.size(), size_t(2));
    CHECK_EQ(s.zones.placements[0].layer, 1u);
    CHECK_EQ(s.zones.placements[0].collider, UINT32_MAX);
    CHECK_EQ(s.zones.placements[1].collider, 0u);
    CHECK_EQ(file_of(s, instance_of(s, 0)), kB);
}

// Geometry 4: two anchor matches (B) and one far entry, which takes B by its geometry.
CM_TEST(zones, geometry_vote_fills_unmatched) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{2, 2}, kL1S1}, {{5, 7}, kL1S1}}),
                havok({zone_entry(5, 7, 1, 1, 4), zone_entry(2, 2, 0, 0, 4), far_entry(4)}), {}, s);
    const ZoneScatter& z = s.zones;
    CHECK_EQ(z.zonePlacementsLinked, 2u);
    CHECK_EQ(z.collidersLinkedByGeometry, 1u);
    CHECK_EQ(z.collidersUnlinked, 0u);
    const ZoneInstance* i = instance_of(s, 2);
    CHECK(i != nullptr);
    CHECK_EQ(file_of(s, i), kB);
    if (i) {
        CHECK_EQ(i->layer, UINT32_MAX);
        CHECK_EQ(i->placement, UINT32_MAX);
    }
    CHECK_EQ(z.instances.size(), size_t(3));
}

// Matched entries of geometry 9 give {A, A, B}: A is used, and one warning names 9, x2 and x1.
CM_TEST(zones, geometry_conflict_warns) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{2, 2}, kL1S0}, {{8, 3}, kL1S1}, {{5, 7}, kL1S0}}),
                havok({zone_entry(5, 7, 0, 0, 9), zone_entry(2, 2, 0, 0, 9), zone_entry(8, 3, 0, 0, 9)}), {}, s);
    for (uint32_t e = 0; e < 3; ++e) CHECK_EQ(file_of(s, instance_of(s, e)), kA);
    CHECK_EQ(zone_warnings_with(s, {"geometry 9", "x2", "x1"}), size_t(1));
}

// A propModels entry of geometry 4 names the prop (fileId A); geometry 4 voted B.
CM_TEST(zones, prop_cross_check_disagreement) {
    WorldScene s = make_scene();
    Ex::HavokPlacement prop;
    prop.group = "prop";
    prop.geometryIndex = 4;
    prop.token = kGuid;
    Ex::MapProp mp;
    mp.fileId = kA;
    mp.guid = kGuid;
    mp.group = "propArray";
    build_zones(make_zon2({{{5, 7}, kL1S1}}), havok({prop, zone_entry(5, 7, 0, 0, 4)}), {mp}, s);
    CHECK_EQ(s.zones.propCrossChecks, 1u);
    CHECK_EQ(s.zones.propDisagreements, 1u);
    CHECK_EQ(zone_warnings_with(s, {"prop cross-check", "geometry 4", "502", "501"}), size_t(1));
    // The zone entry is index 0 among the zone placements, whatever precedes it.
    CHECK_EQ(file_of(s, instance_of(s, 0)), kB);
}

CM_TEST(zones, unlinked_warned) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{5, 7}, kL1S1}}), havok({far_entry(6)}), {}, s);
    CHECK_EQ(s.zones.collidersUnlinked, 1u);
    CHECK(s.zones.instances.empty());
    CHECK(std::find(s.warnings.begin(), s.warnings.end(),
                    std::string("zone: 1 colliders have no tree model (anchor and geometry both failed)")) !=
          s.warnings.end());
}

// D4: CollisionInstance::world == ZoneInstance::world * diag(32, 32, -32).
CM_TEST(zones, collision_matrix_relation) {
    WorldScene s = make_scene();
    Ex::MapHavok h;
    h.present = true;
    Ex::MapCollision hull;
    hull.verts = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    hull.indices = {0, 1, 2};
    h.hulls = {hull};
    h.geometryAnimations = {{0}};
    h.animationCollisions = {{0}};
    Ex::HavokPlacement e = zone_entry(5, 7, 3, 4, 0);
    e.rotate[0] = 0.1f; e.rotate[1] = -0.2f; e.rotate[2] = 2.5f;
    e.scale = 0.84f;
    e.translate[2] = -120;
    h.placements = {e};
    build_collision(h, s);
    build_zones(make_zon2({{{5, 7}, kL1S1}}), h, {}, s);
    const CollisionInstance* c = nullptr;
    for (const CollisionInstance& ci : s.collision.instances)
        if (ci.group == "zone" && ci.placement == 0) c = &ci;
    const ZoneInstance* z = instance_of(s, 0);
    CHECK(c != nullptr);
    CHECK(z != nullptr);
    if (!c || !z) return;
    const float d[4] = {32, 32, -32, 1};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            const float x = c->world[col * 4 + row];
            const float y = z->world[col * 4 + row] * d[col];
            CHECK_NEAR(y, x, 1e-6 * std::max(1.0f, std::fabs(x)));
        }
}

// The prop's model A stays at 0 and is shared; B is appended at 1; C stays unused.
CM_TEST(zones, models_appended_after_props) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{2, 2}, kL1S0}, {{5, 7}, kL1S1}}),
                havok({zone_entry(5, 7, 0, 0, 2), zone_entry(2, 2, 0, 0, 1)}), {}, s);
    CHECK_EQ(s.models.size(), size_t(2));
    CHECK_EQ(s.models[0].fileId, kA);
    CHECK_EQ(s.props[0].model, 0u);
    if (s.models.size() > 1) CHECK_EQ(s.models[1].fileId, kB);
    const ZoneScatter& z = s.zones;
    CHECK_EQ(z.layers[0].models[0].model, 0u);
    CHECK_EQ(z.layers[0].models[1].model, 1u);
    CHECK_EQ(z.layers[1].models[0].model, UINT32_MAX);
    CHECK_EQ(z.layers[1].models[0].fileId, kC);
    CHECK_EQ(instance_of(s, 0) ? instance_of(s, 0)->model : 99u, 1u);
    CHECK_EQ(instance_of(s, 1) ? instance_of(s, 1)->model : 99u, 0u);

    // The summary's zones block.
    const nlohmann::json j = world_summary(s)["zones"];
    CHECK_EQ(j["present"].get<bool>(), true);
    CHECK(j["placementsByType"] == nlohmann::json::array({0, 0, 2}));
    CHECK_EQ(j["colliders"]["entries"].get<uint32_t>(), 2u);
    CHECK_EQ(j["colliders"]["anchor"].get<uint32_t>(), 2u);
    CHECK_EQ(j["colliders"]["geometry"].get<uint32_t>(), 0u);
    CHECK_EQ(j["colliders"]["unlinked"].get<uint32_t>(), 0u);
    CHECK_EQ(j["propCrossChecks"].get<uint32_t>(), 0u);
    CHECK_EQ(j["propDisagreements"].get<uint32_t>(), 0u);
    CHECK_EQ(j["scatter"].get<std::string>(), std::string("colliders"));
    // At least one instance: the UNPROVEN zoneVisualTransform is warned.
    CHECK_EQ(zone_warnings_with(s, {"zoneVisualTransform", "UNPROVEN"}), size_t(1));
}

// A zone-only model whose instance is nearest the camera loads first.
CM_TEST(zones, load_order_ranks_zone_instances) {
    WorldScene s = make_scene();   // prop A at (7000, 7000)
    build_zones(make_zon2({{{5, 7}, kL1S1}}), havok({zone_entry(5, 7, 0, 0, 1)}), {}, s);
    CHECK_EQ(s.models.size(), size_t(2));
    float a[2];
    zone_anchor(kX0, kY0, 5, 7, a);
    const float cam[3] = {a[0], a[1], 0};
    const auto order = model_load_order(s, cam);
    CHECK_EQ(order.size(), size_t(2));
    if (order.size() == 2) {
        CHECK_EQ(order[0], 1u);
        CHECK_EQ(order[1], 0u);
    }
}

CM_TEST(zones, absent_zon2_silent) {
    WorldScene s = make_scene();
    const size_t before = s.warnings.size();
    build_zones(Ex::MapZon2{}, havok({far_entry(1)}), {}, s);
    CHECK_FALSE(s.zones.present);
    CHECK_EQ(s.warnings.size(), before);
    CHECK(s.zones.instances.empty());
}

CM_TEST(zones, no_havk_decodes_no_instances) {
    WorldScene s = make_scene();
    build_zones(make_zon2({{{2, 2}, kL1S0}, {{5, 7}, kL1S1}}), Ex::MapHavok{}, {}, s);
    CHECK(s.zones.present);
    CHECK_EQ(s.zones.placements.size(), size_t(2));
    CHECK(s.zones.instances.empty());
    CHECK_EQ(zone_warnings_with(s, {"no tree model"}), size_t(0));
    CHECK_EQ(zone_warnings(s), size_t(0));
}

CM_TEST(zones, no_parm_rect_warns_and_stops) {
    WorldScene s = make_scene();
    s.hasBounds = false;
    build_zones(make_zon2({{{5, 7}, kL1S1}}), havok({zone_entry(5, 7, 0, 0, 1)}), {}, s);
    CHECK(s.zones.instances.empty());
    CHECK(std::find(s.warnings.begin(), s.warnings.end(), std::string("zone: no parm rect; zones not placed")) !=
          s.warnings.end());
}

CM_TEST(zones, hashes_known_vectors) {
    CHECK_EQ(fmix32(0), 0u);
    CHECK_EQ(fmix32(1), 0x514E28B7u);
    uint64_t state = 0;
    CHECK_EQ(splitmix64_next(state), 0xE220A8397B1DCDAFull);
    CHECK_EQ(splitmix64_next(state), 0x6E789E6AA1B965F4ull);
}

CM_TEST(world, unread_chunks_include_zon2) {
    CHECK(unread_chunk_warnings({{"zon2", 22}}).empty());
}

// ---- phase 2: the zone scatter (world foliage spec D5; docs/research/gw2-terrain-to-unity.md
// §1.3, 2026-10-16) ----

namespace {

/// One measured collider (zone_samples.csv, Task 9) and its inputs. `p` is the cell index (the
/// old walk's k - 1).
struct Sample {
    const char* what;
    float x0, y0;
    uint8_t seed; uint32_t p; int32_t vr[4]; uint8_t flag;
    float scale[2], rot[3][2]; uint32_t modelFlags; float zOff[2];
    float u, v, rx, ry, rz, s;
};

const Sample kSamples[] = {
    {"Queensdale entry 3356 (random tilt)", -43008, -27648, 22, 958, {41, 787, 162, 829}, 0x10, {0.819999993f, 1.19000006f},
     {{0, 5}, {0, 5}, {0, 360}}, 0x100008, {128, 128}, 16.8476562f, 18.1279297f, 0.0438350476f, -0.00121980917f,
     1.85244048f, 0.819999993f},
    {"Queensdale entry 0", -43008, -27648, 232, 1205, {1336, 837, 1499, 958}, 0x12, {0.600000024f, 1.5f},
     {{0, 90}, {0, 90}, {0, 360}}, 0x100000, {128, 128}, -1.20703125f, 7.09863281f, 0, 0, 0.560152411f, 0.986718774f},
    {"Queensdale entry 3609", -43008, -27648, 0, 1437, {1023, 349, 1133, 455}, 0x30, {0.879999995f, 1.5f},
     {{-20, 20}, {-20, 20}, {0, 360}}, 0x2000, {128, 128}, 11.6933594f, -18.4902344f, 0, 0, 2.07459569f, 1.03468752f},
    {"Lion's Arch entry 0", -27648, -18432, 0, 940, {725, 491, 778, 530}, 0x14, {0.5f, 1.5f},
     {{0, 90}, {0, 90}, {0, 360}}, 0x100000, {128, 128}, 14.1347656f, 13.3007812f, 0, 0, 3.95966625f, 0.88671875f},
    {"Spirit Vale entry 0 (random tilt, no sign)", -15360, -36864, 0, 665, {277, 240, 343, 323}, 0x10,
     {0.5f, 1.02999997f}, {{-41.2900009f, -26.2900009f}, {-101.290001f, 47.0999985f}, {0, 360}}, 0x120000,
     {108, 108}, 2.68579102f, -2.34375f, -0.484763503f, -0.49871698f, 5.37188816f, 0.654687524f},
};

/// A one-zone scene holding sample @p S's cell (raw flags, no terrain, no havk).
WorldScene sample_scene(const Sample& S) {
    WorldScene s;
    s.hasBounds = true;
    s.bounds[0] = S.x0; s.bounds[1] = S.y0; s.bounds[2] = S.x0 + 200000; s.bounds[3] = S.y0 + 200000;
    Ex::MapZon2 z;
    z.present = z.hasRect = true;
    for (int k = 0; k < 4; ++k) z.rect[k] = s.bounds[k];
    Ex::Zon2Def d;
    d.token = 1;
    d.layers.resize((S.flag >> 4));
    Ex::Zon2Layer& L = d.layers.back();
    L.type = 3;
    L.scaleRange[0] = S.scale[0]; L.scaleRange[1] = S.scale[1];
    for (int a = 0; a < 3; ++a) for (int k = 0; k < 2; ++k) L.rotRange[a][k] = S.rot[a][k];
    L.models.resize(((S.flag >> 1) & 7) + 1);
    Ex::Zon2LayerModel& M = L.models.back();
    M.fileId = 900;
    M.flags = S.modelFlags;
    M.zOffsets[0] = S.zOff[0]; M.zOffsets[1] = S.zOff[1];
    z.defs.push_back(d);
    Ex::Zon2Zone zone;
    zone.defToken = 1;
    zone.seed = S.seed;
    for (int k = 0; k < 4; ++k) zone.vertRect[k] = S.vr[k];
    zone.flags.assign(size_t(S.vr[2] - S.vr[0]) * size_t(S.vr[3] - S.vr[1]), 0);   // raw: no encodeData
    zone.flags[S.p] = S.flag;
    z.zones.push_back(zone);
    build_zones(z, Ex::MapHavok{}, {}, s);
    return s;
}

double angle_error(double a, double b) {
    const double twoPi = 6.283185307179586;
    double d = std::fmod(std::fabs(a - b), twoPi);
    return std::min(d, twoPi - d);
}

/// A scene of @p n type-3 single-instance placements (layer 0 slot 1 = model B) over a 40-wide zone at the origin, with
/// rotRange.z [10, 200] and scaleRange [0.5, 1.5]; the first @p collided get a collider.
WorldScene many_scene(int n, int collided) {
    std::vector<std::pair<std::pair<int, int>, uint8_t>> pts;
    for (int i = 0; i < n; ++i) pts.push_back({{i % 40, i / 40}, uint8_t(0x12)});   // layer 1, slot 1, bit 0 clear
    Ex::MapZon2 z = make_zon2({});
    z.zones[0].vertRect[2] = 40;
    z.zones[0].vertRect[3] = (n + 39) / 40;
    z.zones[0].flags = rle_flags(pts, 40);
    z.zones[0].encodeData.resize(z.zones[0].vertRect[3]);
    Ex::Zon2Layer& L = z.defs[0].layers[0];
    L.rotRange[2][0] = 10; L.rotRange[2][1] = 200;
    L.scaleRange[0] = 0.5f; L.scaleRange[1] = 1.5f;
    std::vector<Ex::HavokPlacement> es;
    for (int i = 0; i < collided; ++i) es.push_back(zone_entry(i % 40, i / 40, 0, 0, 1));
    WorldScene s = make_scene();
    s.mapFileId = 192711;
    build_zones(z, havok(es), {}, s);
    return s;
}

bool same_instances(const std::vector<ZoneInstance>& a, const std::vector<ZoneInstance>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].model != b[i].model || a[i].layer != b[i].layer || a[i].placement != b[i].placement ||
            a[i].collider != b[i].collider || a[i].source != b[i].source ||
            std::memcmp(a[i].world, b[i].world, sizeof a[i].world) != 0)
            return false;
    return true;
}

ZoneScatterRule density_rule() {
    ZoneScatterRule r;
    r.mode = ZoneScatterMode::DensityMatched;
    return r;
}

} // namespace

// Park-Miller 48271 with the client's fix-up (0x140e24b10): the minimal-standard sequence.
CM_TEST(scatter, rng_vectors) {
    ZoneRng a(1);
    CHECK_EQ(a.next(), 48271u);
    CHECK_EQ(a.next(), 182605794u);
    CHECK_EQ(a.next(), 1291394886u);
    ZoneRng zero(0);   // a zero seed takes 0x075BD924
    CHECK_EQ(zero.state, 0x075BD924u);
    CHECK_EQ(zero.next(), 264553971u);
    ZoneRng top(0x7FFFFFFF);
    CHECK_EQ(top.next(), 123459876u);
    ZoneRng r(5);
    for (int i = 0; i < 1000; ++i) {
        const float f = r.fA(), g = r.fB();
        CHECK(f > 0 && f <= 1);
        CHECK(g > 0 && g < 1.0001f);
        const uint32_t k = r.irange(1, 4);
        CHECK(k >= 1 && k <= 4);
    }
}

CM_TEST(scatter, runtime_flags_and_tiling_bound) {
    CHECK_EQ(zone_runtime_flags(0x1), 0x1u);
    CHECK_EQ(zone_runtime_flags(0x200000), 0x4u);
    CHECK_EQ(zone_runtime_flags(0x4), 0x8u);
    CHECK_EQ(zone_runtime_flags(0x100008), 0x40010u);
    CHECK_EQ(zone_runtime_flags(0x120000), 0x40020u);
    CHECK_EQ(zone_runtime_flags(0x10 | 0x800), 0x4040u);
    CHECK_EQ(zone_runtime_flags(0x1000), 0x8000u);
    CHECK_EQ(zone_runtime_flags(0x200), 0x100u);
    CHECK_EQ(zone_runtime_flags(0x800000), 0x100000u);
    CHECK_EQ(zone_tiling_max(4, 2), 4u);
    CHECK_EQ(zone_tiling_max(4, 1), 3u);
    CHECK_EQ(zone_tiling_max(4, 0), 2u);
    CHECK_EQ(zone_tiling_max(3, 0), 2u);
    CHECK_EQ(zone_tiling_max(3, 1), 3u);
    CHECK_EQ(zone_tiling_max(1, 0), 1u);
    // fadeRange x {0.85, 1, 1.5, 2}[quality] (research §1.3); out of range: 1
    CHECK_EQ(zone_fade_quality_scale(0), 0.85f);
    CHECK_EQ(zone_fade_quality_scale(1), 1.0f);
    CHECK_EQ(zone_fade_quality_scale(2), 1.5f);
    CHECK_EQ(zone_fade_quality_scale(3), 2.0f);
    CHECK_EQ(zone_fade_quality_scale(-1), 1.0f);
    CHECK_EQ(zone_fade_quality_scale(4), 1.0f);
    CHECK_EQ(zone_fade_quality_scale(zone_scatter_rule_in_force().graphicsQuality), 1.5f);
    // the seed: (seed << 8) ^ T[...] ^ (((4 ly) ^ lx) << 14); T[0] = 0x35A9260D (0x142592280)
    CHECK_EQ(zone_cell_seed(0, 0, 0, 10), 0x35A9260Du);
    CHECK_EQ(zone_cell_seed(1, 0, 0, 10), 0x35A9260Du ^ 0x100u);
}

// Five measured colliders (3 Queensdale, 1 Lion's Arch, 1 Spirit Vale) reproduced by the exact rule
// to spec D5 step 2's tolerances.
CM_TEST(scatter, exact_unit_vectors) {
    for (const Sample& S : kSamples) {
        const WorldScene s = sample_scene(S);
        CHECK_EQ(s.zones.placements.size(), size_t(1));
        if (s.zones.placements.size() != 1) continue;
        std::vector<ZoneScatterDraw> d;
        zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), {}, d);
        CHECK_EQ(d.size(), size_t(1));
        if (d.empty()) continue;
        float P[2];
        zone_anchor(S.x0, S.y0, s.zones.placements[0].c, s.zones.placements[0].r, P);
        std::printf("    %s: u %.4f v %.4f rx %.6f ry %.6f rz %.6f s %.6f\n", S.what, d[0].pos[0] - P[0],
                    d[0].pos[1] - P[1], d[0].rot[0], d[0].rot[1], d[0].rot[2], d[0].scale);
        CHECK_EQ(d[0].fileId, 900u);
        CHECK(std::fabs(d[0].pos[0] - P[0] - S.u) <= 0.01f);
        CHECK(std::fabs(d[0].pos[1] - P[1] - S.v) <= 0.01f);
        CHECK(angle_error(d[0].rot[2], S.rz) <= 1e-4);
        CHECK(angle_error(d[0].rot[0], S.rx) <= 1e-4);
        CHECK(angle_error(d[0].rot[1], S.ry) <= 1e-4);
        CHECK(std::fabs(d[0].scale - S.s) <= 1e-5f);
    }
}

// zOffsets 128 / 128 give 36 - 128/255 * 72 = -0.14 (0x140cbc110), on ground 0 (no terrain).
CM_TEST(scatter, exact_z_offset) {
    const WorldScene s = sample_scene(kSamples[1]);
    std::vector<ZoneScatterDraw> d;
    zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), {}, d);
    CHECK_EQ(d.size(), size_t(1));
    if (!d.empty()) CHECK_NEAR(d[0].pos[2], 36.0f - 128.0f / 255.0f * 72.0f, 1e-4);
}

// zoneFlags bit 1: z and the normal come from collideData[offsetData[chunk] + order in chunk].
CM_TEST(scatter, exact_collide_data) {
    Sample S = kSamples[1];
    S.modelFlags = 0x2;   // ground-aligned
    S.vr[0] = 30; S.vr[1] = 0; S.vr[2] = 70; S.vr[3] = 2;   // chunk columns: 30-31 | 32-63 | 64-69
    S.p = 0;
    WorldScene s = sample_scene(S);
    // Cells: (30, 0) chunk 0 first; (33, 0) chunk 1 first; (31, 1) chunk 0 second; (34, 1) chunk 1 second.
    ZoneScatter& z = s.zones;
    Zone& Z = z.zones[0];
    const ZonePlacement tmpl = z.placements[0];
    z.placements.clear();
    for (auto [c, r] : std::vector<std::pair<int, int>>{{30, 0}, {33, 0}, {31, 1}, {34, 1}}) {
        ZonePlacement p = tmpl;
        p.c = c; p.r = r;
        z.placements.push_back(p);
    }
    Z.placementCount = 4;
    Z.zoneFlags = 2;
    Z.offsetData = {10, 0, 99};
    Z.collideData.assign(3 * 12, 0.0f);
    Z.collideData[3 * 11 + 2] = -500;   // chunk 0's second cell -> index 11
    Z.collideData[3 * 11 + 0] = 0.6f;   // its normal x
    Z.collideData[3 * 1 + 2] = -700;    // chunk 1's second cell -> index 1
    const std::vector<uint32_t> ci = zone_collide_index(z, 0);
    CHECK_EQ(ci.size(), size_t(4));
    if (ci.size() == 4) {
        CHECK_EQ(ci[0], 10u);
        CHECK_EQ(ci[1], 0u);
        CHECK_EQ(ci[2], 11u);
        CHECK_EQ(ci[3], 1u);
    }
    std::vector<ZoneScatterDraw> d;
    zone_scatter_exact(s, 2, zone_scatter_rule_in_force(), ci, d);
    CHECK_EQ(d.size(), size_t(1));
    if (!d.empty()) {
        CHECK_NEAR(d[0].pos[2], -500.0f + (36.0f - 128.0f / 255.0f * 72.0f), 1e-3);
        // n = (0.6, 0, 0.8): ry = atan2(0.6, 0.8), rx = 0
        CHECK_NEAR(d[0].rot[1], std::atan2(0.6, 0.8), 1e-5);
        CHECK_NEAR(d[0].rot[0], 0.0, 1e-6);
    }
    d.clear();
    zone_scatter_exact(s, 3, zone_scatter_rule_in_force(), ci, d);
    CHECK_EQ(d.size(), size_t(1));
    if (!d.empty()) CHECK_NEAR(d[0].pos[2], -700.0f + (36.0f - 128.0f / 255.0f * 72.0f), 1e-3);
    // an index past collideData: the client skips the cell
    Z.offsetData = {100, 100, 100};
    d.clear();
    zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), zone_collide_index(z, 0), d);
    CHECK(d.empty());
}

// The tiling path (flag bit 0): 1-T instances around P + (24, 24); none with pack 0x800000 at quality 0.
CM_TEST(scatter, exact_tiling_path) {
    Sample S = kSamples[1];
    S.flag = 0x13;   // layer 0, slot 1, bit 0
    size_t total = 0, multi = 0;
    for (uint32_t p = 0; p < 200; ++p) {
        S.p = p;
        WorldScene s = sample_scene(S);
        s.zones.layers[0].tiling = 4;
        std::vector<ZoneScatterDraw> d;
        zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), {}, d);
        CHECK(d.size() >= 1 && d.size() <= 4);
        total += d.size();
        multi += d.size() > 1;
        float P[2];
        zone_anchor(S.x0, S.y0, s.zones.placements[0].c, s.zones.placements[0].r, P);
        for (const ZoneScatterDraw& e : d) {
            CHECK(std::fabs(e.pos[0] - (P[0] + 24)) <= 91.0f);   // the tables span -90.7 .. 85.3
            CHECK(std::fabs(e.pos[1] - (P[1] + 24)) <= 91.0f);
        }
        if (p == 0) {   // quality 0 with pack flag 0x800000: skipped
            s.zones.layers[0].models[1].flags |= 0x800000;
            ZoneScatterRule q0 = zone_scatter_rule_in_force();
            q0.graphicsQuality = 0;
            d.clear();
            zone_scatter_exact(s, 0, q0, {}, d);
            CHECK(d.empty());
        }
    }
    std::printf("    200 tiling cells (tiling 4, quality 2): %zu instances, %zu cells with more than one\n", total, multi);
    CHECK(multi > 0);
    CHECK(total > 200 && total < 800);
}

// subModel (0x140cbc210): probability 1 always adds a second instance at the same transform; -1 never does.
CM_TEST(scatter, exact_submodel) {
    WorldScene s = sample_scene(kSamples[2]);
    ZoneLayer& L = s.zones.layers[s.zones.placements[0].layer];
    L.hasSubModel = true;
    L.subModel.fileId = 901;
    L.subModel.probability = 1;
    std::vector<ZoneScatterDraw> d;
    zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), {}, d);
    CHECK_EQ(d.size(), size_t(2));
    if (d.size() == 2) {
        CHECK_EQ(d[1].fileId, 901u);
        CHECK(d[1].subModel);
        CHECK(std::memcmp(d[0].pos, d[1].pos, sizeof d[0].pos) == 0);
        CHECK(std::memcmp(d[0].rot, d[1].rot, sizeof d[0].rot) == 0);
        CHECK_EQ(d[0].scale, d[1].scale);
    }
    L.subModel.probability = -1;
    d.clear();
    zone_scatter_exact(s, 0, zone_scatter_rule_in_force(), {}, d);
    CHECK_EQ(d.size(), size_t(1));
}

CM_TEST(scatter, density_deterministic) {
    WorldScene a = many_scene(60, 5), b = many_scene(60, 5);
    CHECK_EQ(scatter_instances(a, density_rule()), size_t(55));
    CHECK_EQ(scatter_instances(b, density_rule()), size_t(55));
    CHECK(same_instances(a.zones.instances, b.zones.instances));
    CHECK(a.zones.mode == ZoneScatterMode::DensityMatched);
    WorldScene e1 = many_scene(60, 5), e2 = many_scene(60, 5);
    scatter_instances(e1, zone_scatter_rule_in_force());
    scatter_instances(e2, zone_scatter_rule_in_force());
    CHECK(same_instances(e1.zones.instances, e2.zones.instances));
}

CM_TEST(scatter, density_ranges) {
    WorldScene s = many_scene(1000, 0);
    CHECK_EQ(scatter_instances(s, density_rule()), size_t(1000));
    const double pi = 3.14159265358979323846;
    size_t bad = 0;
    for (const ZoneInstance& i : s.zones.instances) {
        CHECK(i.source == ZoneSource::Density);
        const ZonePlacement& p = s.zones.placements[i.placement];
        float a[2];
        zone_anchor(kX0, kY0, p.c, p.r, a);
        const double u = i.world[12] - a[0], v = i.world[13] - a[1];
        const double scale = std::hypot(i.world[0], i.world[1], i.world[2]);
        double yaw = std::atan2(i.world[4], i.world[0]);
        if (yaw < 0) yaw += 2 * pi;
        const bool ok = u >= -24.001 && u < 24.001 && v >= -24.001 && v < 24.001 && scale >= 0.5 - 1e-5 &&
                        scale <= 1.5 + 1e-5 && yaw >= 10 * pi / 180 - 1e-4 && yaw <= 200 * pi / 180 + 1e-4;
        bad += !ok;
    }
    CHECK_EQ(bad, size_t(0));
    uint64_t st = 7;
    for (int k = 0; k < 10000; ++k) {
        const double d = zone_unit_draw(st);
        CHECK(d >= 0 && d < 1);
    }
}

// D5 step 5: a placement with a collider gets no Scatter or Density instance.
CM_TEST(scatter, never_doubled) {
    for (const ZoneScatterRule& r : {density_rule(), zone_scatter_rule_in_force()}) {
        WorldScene s = many_scene(50, 20);
        CHECK_EQ(s.zones.instances.size(), size_t(20));
        const size_t added = scatter_instances(s, r);
        CHECK_EQ(added, size_t(30));
        std::vector<int> perPlacement(s.zones.placements.size(), 0);
        for (const ZoneInstance& i : s.zones.instances)
            if (i.placement < perPlacement.size()) ++perPlacement[i.placement];
        for (size_t pi = 0; pi < perPlacement.size(); ++pi) CHECK_EQ(perPlacement[pi], 1);
        for (const ZoneInstance& i : s.zones.instances)
            if (i.source != ZoneSource::Collider) CHECK_EQ(s.zones.placements[i.placement].collider, UINT32_MAX);
        // model B was already in the table (the collider trees): the scatter reuses its index
        CHECK_EQ(s.models.size(), size_t(2));
    }
}

CM_TEST(scatter, colliders_mode_adds_nothing) {
    WorldScene s = many_scene(50, 20);
    const size_t warnings = s.warnings.size();
    CHECK_EQ(scatter_instances(s, ZoneScatterRule{}), size_t(0));
    CHECK_EQ(s.zones.instances.size(), size_t(20));
    CHECK(s.zones.mode == ZoneScatterMode::Colliders);
    CHECK_EQ(s.warnings.size(), warnings);
}

// Final review M6: a collided placement keeps its Collider instance only, so a
// subModel on its layer is not drawn; that is counted and warned as UNPROVEN.
CM_TEST(scatter, collided_submodel_warned) {
    WorldScene s = many_scene(50, 20);
    const uint32_t li = s.zones.placements[0].layer;
    CHECK(li < s.zones.layers.size());
    if (li >= s.zones.layers.size()) return;
    s.zones.layers[li].hasSubModel = true;
    s.zones.layers[li].subModel.fileId = 901;
    s.zones.layers[li].subModel.probability = -1;   // never drawn for the scattered ones
    scatter_instances(s, zone_scatter_rule_in_force());
    CHECK_EQ(zone_warnings_with(s, {"UNPROVEN", "20 collided placements have a layer subModel"}), size_t(1));
    WorldScene plain = many_scene(50, 20);
    scatter_instances(plain, zone_scatter_rule_in_force());
    CHECK_EQ(zone_warnings_with(plain, {"collided placements have a layer subModel"}), size_t(0));
}

// The exact rule's warnings name the UNPROVEN parts; the summary counts instances by source.
CM_TEST(scatter, exact_warnings_and_summary) {
    WorldScene s = many_scene(50, 20);
    CHECK(zone_scatter_rule_in_force().mode == ZoneScatterMode::Exact);
    CHECK_EQ(zone_scatter_rule_in_force().graphicsQuality, 2);
    scatter_instances(s, zone_scatter_rule_in_force());
    CHECK_EQ(zone_warnings_with(s, {"zoneScatter = exact"}), size_t(1));
    CHECK_EQ(zone_warnings_with(s, {"UNPROVEN", "tiling path", "graphics quality 2", "(24, 24)"}), size_t(1));
    CHECK_EQ(zone_warnings_with(s, {"UNPROVEN branches", "clamp", "water", "random z", "yaw from normal", "snap",
                                    "subModel", "identity"}),
             size_t(1));
    const nlohmann::json j = world_summary(s)["zones"];
    CHECK(j["instances"] == nlohmann::json::array({20, 30, 0}));
    CHECK_EQ(j["scatter"].get<std::string>(), std::string("exact"));
}
