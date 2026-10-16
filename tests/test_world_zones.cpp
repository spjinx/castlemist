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
#include "castlemist/world/zones.h"

#include <algorithm>
#include <cmath>
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
    uint32_t at = 0;
    for (const auto& [cr, f] : points) {
        const uint32_t k = uint32_t(cr.second * 10 + cr.first);
        uint32_t skip = k - at;
        while (skip > 255) { zone.flags.push_back(255); zone.flags.push_back(0); skip -= 255; }
        zone.flags.push_back(uint8_t(skip));
        zone.flags.push_back(f);
        at = k;
    }
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
    CHECK_EQ(a[0], -808.0f);
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
    grass.flags = {75, 0x21};   // (5, 7): layer 2 (type 1), slot 0, bit 0
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
