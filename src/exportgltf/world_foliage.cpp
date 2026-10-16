/// @file
/// @brief The world export's foliage records, foliage block and wind flags
///        (world_foliage.h; docs/superpowers/specs/2026-10-16-world-foliage-design.md D7).

#include "castlemist/exportgltf/world_foliage.h"

#include "castlemist/world/cells.h"
#include "castlemist/world/zones.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <tuple>
#include <unordered_map>

namespace castlemist::exportgltf {

using nlohmann::json;
namespace cw = castlemist::world;

namespace {

void put_u16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put_u32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put_f32(uint8_t* p, float f) { uint32_t v; std::memcpy(&v, &f, 4); put_u32(p, v); }
uint16_t get_u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t get_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
float get_f32(const uint8_t* p) { const uint32_t v = get_u32(p); float f; std::memcpy(&f, &v, 4); return f; }

constexpr double kPi = 3.14159265358979323846;

} // namespace

void encode_foliage_record(const FoliageRecord& r, uint8_t out[kFoliageRecordBytes]) {
    put_u32(out + 0, r.model);
    put_u16(out + 4, r.layer);
    put_u16(out + 6, r.flags);
    put_u32(out + 8, r.placement);
    for (int k = 0; k < 12; ++k) put_f32(out + 12 + 4 * k, r.m[k]);
    for (int k = 0; k < 3; ++k) put_f32(out + 60 + 4 * k, r.unity.t[k]);
    for (int k = 0; k < 4; ++k) put_f32(out + 72 + 4 * k, r.unity.r[k]);
    put_f32(out + 88, r.unity.s);
    put_f32(out + 92, r.unityYaw);
    put_f32(out + 96, r.tiltDeg);
}

FoliageRecord decode_foliage_record(const uint8_t* in) {
    FoliageRecord r;
    r.model = get_u32(in + 0);
    r.layer = get_u16(in + 4);
    r.flags = get_u16(in + 6);
    r.placement = get_u32(in + 8);
    for (int k = 0; k < 12; ++k) r.m[k] = get_f32(in + 12 + 4 * k);
    for (int k = 0; k < 3; ++k) r.unity.t[k] = get_f32(in + 60 + 4 * k);
    for (int k = 0; k < 4; ++k) r.unity.r[k] = get_f32(in + 72 + 4 * k);
    r.unity.s = get_f32(in + 88);
    r.unityYaw = get_f32(in + 92);
    r.tiltDeg = get_f32(in + 96);
    return r;
}

std::vector<FoliageRecord> cell_foliage_records(const cw::WorldScene& s, const cw::Cell& c, FoliageExport mode,
                                                size_t& skipped) {
    std::vector<FoliageRecord> out;
    if (mode == FoliageExport::Off) return out;
    struct Keyed { FoliageRecord r; uint32_t collider; };
    std::vector<Keyed> keyed;
    keyed.reserve(c.foliage.size());
    const bool terrain = s.terrain.present && !s.terrain.chunks.empty();
    for (uint32_t zi : c.foliage) {
        if (zi >= s.zones.instances.size()) continue;
        const cw::ZoneInstance& z = s.zones.instances[zi];
        if (mode == FoliageExport::Colliders && z.source != cw::ZoneSource::Collider) continue;
        FoliageRecord r;
        if (!unity_trs(z.world, c.origin, r.unity)) { ++skipped; continue; }   // also every non-finite world
        r.model = z.model;
        r.layer = z.layer >= 0xFFFFu ? uint16_t(0xFFFF) : uint16_t(z.layer);
        r.placement = z.placement;
        uint16_t flags = uint16_t((uint16_t(z.source) & 3u) << kFoliageSourceShift);
        if (z.source == cw::ZoneSource::Collider) flags |= kFoliageFlagCollider;
        if (terrain && !cw::point_in_chunk_grid(s.terrain, z.world[12], z.world[13])) flags |= kFoliageFlagOutsideTerrain;
        r.flags = flags;
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 3; ++row) r.m[col * 3 + row] = z.world[col * 4 + row];
        // unityYaw: the Unity forward (0, 0, 1) turned by unity.r, as a heading about +Y.
        const double x = r.unity.r[0], y = r.unity.r[1], zq = r.unity.r[2], w = r.unity.r[3];
        const double fx = 2 * (x * zq + y * w), fz = 1 - 2 * (x * x + y * y);
        r.unityYaw = float(std::atan2(fx, fz));
        // tiltDeg: the instance's up (M3 (0, 0, -1), map up = -Z) against map up.
        const double ux = -z.world[8], uy = -z.world[9], uz = -z.world[10];
        const double n = std::sqrt(ux * ux + uy * uy + uz * uz);
        const double cosT = std::clamp(n > 0 ? -uz / n : 1.0, -1.0, 1.0);
        r.tiltDeg = float(std::acos(cosT) * 180.0 / kPi);
        if (!std::isfinite(r.unityYaw) || !std::isfinite(r.tiltDeg)) { ++skipped; continue; }
        keyed.push_back({r, z.collider});
    }
    std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
        return std::tie(a.r.model, a.r.placement, a.collider) < std::tie(b.r.model, b.r.placement, b.collider);
    });
    out.reserve(keyed.size());
    for (const Keyed& k : keyed) out.push_back(k.r);
    return out;
}

json model_wind(const ModelPreview& p) {
    bool traced = false;
    for (const GameMaterial& g : p.gameMaterials) {
        if (!g.ok) continue;
        traced = true;
        for (const GameShaderUniform& u : g.vsUniforms)
            if (u.name == "VegPara") return true;
    }
    return traced ? json(false) : json(nullptr);
}

json foliage_manifest_json(const cw::WorldScene& s, const std::vector<ManifestModel>& models,
                           const std::vector<char>& used) {
    const cw::ZoneScatter& z = s.zones;
    std::unordered_map<uint32_t, size_t> pos;   // scene index -> position in models[]
    for (size_t k = 0; k < models.size(); ++k) pos.emplace(models[k].index, k);

    json layers = json::array();
    for (size_t li = 0; li < z.layers.size(); ++li) {
        const cw::ZoneLayer& L = z.layers[li];
        json slots = json::array();
        for (const cw::ZoneLayerModel& m : L.models) {
            const auto it = m.model == UINT32_MAX ? pos.end() : pos.find(m.model);
            slots.push_back(it == pos.end() ? json(nullptr) : json(it->second));
        }
        json rot = json::array();
        for (const auto& r : L.rotRange) rot.push_back({r[0], r[1]});
        // zoneLayerTypeTarget (D11): 1-2 detail, 3 tree; any other type has no target.
        const json target = L.type == 3 ? json("tree") : (L.type == 1 || L.type == 2) ? json("detail") : json(nullptr);
        layers.push_back({{"index", li},
                          {"zoneDef", L.defToken},
                          {"layer", L.indexInDef},
                          {"type", L.type},
                          {"width", L.width},
                          {"height", L.height},
                          {"radiusGround", L.radiusGround},
                          {"fadeRange", {L.fadeRange[0], L.fadeRange[1]}},
                          {"scaleRange", {L.scaleRange[0], L.scaleRange[1]}},
                          {"rotRange", rot},
                          {"layerFlags", L.layerFlags},
                          {"material", L.materialFileId ? json(L.materialFileId) : json(nullptr)},
                          {"target", target},
                          {"models", slots}});
    }

    // Foliage models: those a zone instance uses (or, given @p used, those with a record).
    std::vector<char> want(models.size(), 0);
    if (!used.empty()) {
        for (size_t k = 0; k < models.size() && k < used.size(); ++k) want[k] = used[k];
    } else {
        for (const cw::ZoneInstance& zi : z.instances) {
            const auto it = pos.find(zi.model);
            if (it != pos.end()) want[it->second] = 1;
        }
    }
    json fm = json::array();
    for (size_t k = 0; k < models.size(); ++k) {
        if (!want[k]) continue;
        const ManifestModel& m = models[k];
        std::set<int> types;
        bool anyFade = false;
        float fadeFar = 0;
        for (const cw::ZoneLayer& L : z.layers)
            for (const cw::ZoneLayerModel& lm : L.models)
                if (lm.model == m.index) {
                    types.insert(L.type);
                    if (std::isfinite(L.fadeRange[1])) { fadeFar = anyFade ? std::max(fadeFar, L.fadeRange[1]) : L.fadeRange[1]; anyFade = true; }
                }
        const bool detailType = types.count(1) || types.count(2);
        const json wind = m.wind.is_boolean() ? m.wind : json(nullptr);
        fm.push_back({{"model", k},
                      {"zoneTypes", json(std::vector<int>(types.begin(), types.end()))},
                      {"fadeFar", anyFade ? json(fadeFar) : json(nullptr)},
                      {"wind", wind},
                      {"terrainDetail", m.ok && m.meshes == 1 && m.materialCount == 1 && detailType}});
    }

    const char* scatter = z.mode == cw::ZoneScatterMode::Exact            ? "exact"
                          : z.mode == cw::ZoneScatterMode::DensityMatched ? "densityMatched"
                                                                          : "colliders";
    return {{"scatter", scatter},
            {"recordLayout", kFoliageRecordLayout},
            {"recordBytes", kFoliageRecordBytes},
            {"layers", layers},
            {"models", fm},
            {"colliders",
             {{"entries", z.colliderEntries},
              {"linkedByAnchor", z.zonePlacementsLinked},
              {"linkedByGeometry", z.collidersLinkedByGeometry},
              {"unlinked", z.collidersUnlinked},
              {"geometries", z.colliderGeometries},
              {"propCrossChecks", z.propCrossChecks},
              {"propDisagreements", z.propDisagreements}}}};
}

} // namespace castlemist::exportgltf
