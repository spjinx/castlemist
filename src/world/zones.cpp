/// @file
/// @brief Zone scatter in WorldScene: the zon2 decode, the link from havk
///        zoneModels colliders to their tree models (anchor, geometry vote,
///        prop cross-check) and the collider instances
///        (docs/superpowers/specs/2026-10-16-world-foliage-design.md D2-D4).

#include "castlemist/world/zones.h"

#include "castlemist/world/props.h"
#include "castlemist/world/world_scene.h"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace castlemist::world {

namespace {

using Ex = castlemist::model::Extractor;

constexpr float kCell = 48.0f;          ///< zone grid step, map units (spec M2, D11 zoneGrid)
constexpr double kTolerance = 24.0;     ///< anchor match, per axis (spec D3 step 2): half a cell

uint64_t cell_key(int64_t c, int64_t r) { return (uint64_t(uint32_t(int32_t(c))) << 32) | uint32_t(int32_t(r)); }

/// The fileId of placement @p p's model slot (spec M3), or 0 when the layer is
/// unresolved, the slot is past modelArray, or the entry has no file.
uint32_t slot_file(const ZoneScatter& z, const ZonePlacement& p) {
    if (p.layer >= z.layers.size()) return 0;
    const auto& models = z.layers[p.layer].models;
    const uint32_t slot = (p.flag >> 1) & 7u;
    return slot < models.size() ? models[slot].fileId : 0;
}

} // namespace

void zone_anchor(float x0, float y0, int32_t c, int32_t r, float out[2]) {
    // (float)(uint)(c * 48) + x0, as 0x140cc8150; rows count north from y0 (M2)
    out[0] = float(uint32_t(c) * 48u) + x0;
    out[1] = float(uint32_t(r) * 48u) + y0;
}

void build_zones(const Ex::MapZon2& z, const Ex::MapHavok& h, const std::vector<Ex::MapProp>& props,
                 WorldScene& scene) {
    scene.zones = {};
    ZoneScatter& out = scene.zones;
    std::vector<std::string>& warn = scene.warnings;
    if (!z.present) return;   // some maps have no zon2: not a warning (spec, Error handling)
    out.present = true;

    // ---- Step 1: decode. Layers of every def, in def order.
    std::unordered_map<uint32_t, uint32_t> defOf;   // token -> def index (the first def with that token)
    std::vector<uint32_t> layerBase(z.defs.size());
    for (uint32_t d = 0; d < z.defs.size(); ++d) {
        const Ex::Zon2Def& def = z.defs[d];
        defOf.try_emplace(def.token, d);
        layerBase[d] = uint32_t(out.layers.size());
        for (uint32_t li = 0; li < def.layers.size(); ++li) {
            const Ex::Zon2Layer& s = def.layers[li];
            ZoneLayer L;
            L.defToken = def.token;
            L.indexInDef = li;
            L.type = s.type; L.width = s.width; L.height = s.height; L.radiusGround = s.radiusGround;
            L.sortGroup = s.sortGroup; L.tiling = s.tiling; L.noise = s.noise;
            for (int k = 0; k < 2; ++k) { L.scaleRange[k] = s.scaleRange[k]; L.fadeRange[k] = s.fadeRange[k]; }
            L.probability = s.probability;
            for (int a = 0; a < 3; ++a) for (int k = 0; k < 2; ++k) L.rotRange[a][k] = s.rotRange[a][k];
            for (int a = 0; a < 4; ++a) for (int k = 0; k < 2; ++k) L.hslRanges[a][k] = s.hslRanges[a][k];
            L.instanceScaleJitter = s.instanceScaleJitter;
            L.layerFlags = s.layerFlags;
            L.materialFileId = s.materialFileId;
            L.models.resize(s.models.size());
            for (size_t m = 0; m < s.models.size(); ++m) {
                const Ex::Zon2LayerModel& sm = s.models[m];
                ZoneLayerModel& M = L.models[m];
                M.fileId = sm.fileId; M.probability = sm.probability; M.flags = sm.flags;
                for (int k = 0; k < 3; ++k) M.hslOffset[k] = sm.hslOffset[k];
                for (int k = 0; k < 2; ++k) M.zOffsets[k] = sm.zOffsets[k];
                M.permutation = sm.permutation;
            }
            if (s.hasSubModel) {
                L.hasSubModel = true;
                const Ex::Zon2LayerModel& sm = s.subModel;
                ZoneLayerModel& M = L.subModel;
                M.fileId = sm.fileId; M.probability = sm.probability; M.flags = sm.flags;
                for (int k = 0; k < 3; ++k) M.hslOffset[k] = sm.hslOffset[k];
                for (int k = 0; k < 2; ++k) M.zOffsets[k] = sm.zOffsets[k];
                M.permutation = sm.permutation;
            }
            out.layers.push_back(std::move(L));
        }
    }

    // Zones, and their placements in zoneArray order, then walk order.
    uint32_t badSlots = 0;
    out.zones.reserve(z.zones.size());
    for (uint32_t zi = 0; zi < z.zones.size(); ++zi) {
        const Ex::Zon2Zone& s = z.zones[zi];
        Zone Z;
        Z.defToken = s.defToken;
        for (int k = 0; k < 4; ++k) Z.vertRect[k] = s.vertRect[k];
        Z.seed = s.seed;
        Z.range[0] = s.range[0]; Z.range[1] = s.range[1];
        Z.zPos = s.zPos; Z.waterHeight = s.waterHeight; Z.zoneFlags = s.zoneFlags; Z.broadId = s.broadId;
        Z.collideData = s.collideData;
        Z.offsetData = s.offsetData;
        Z.firstPlacement = uint32_t(out.placements.size());
        const auto def = defOf.find(s.defToken);
        Ex::walkZoneFlags(s, [&](uint32_t, int32_t c, int32_t r, uint8_t f) {
            ZonePlacement p;
            p.zone = zi; p.c = c; p.r = r; p.flag = f;
            const uint32_t li = f >> 4;   // 1-based (M3)
            if (def != defOf.end() && li >= 1 && li <= z.defs[def->second].layers.size())
                p.layer = layerBase[def->second] + li - 1;
            if (p.layer == UINT32_MAX) ++out.placementsUnresolved;
            else if (!slot_file(out, p)) ++badSlots;
            out.placements.push_back(p);
        });
        Z.placementCount = uint32_t(out.placements.size()) - Z.firstPlacement;
        out.zones.push_back(std::move(Z));
    }
    if (out.placementsUnresolved)
        warn.push_back("zone: " + std::to_string(out.placementsUnresolved) +
                       " placements name a zone def or layer that does not resolve; kept with no layer, not drawn");
    if (badSlots)
        warn.push_back("zone: " + std::to_string(badSlots) +
                       " placements name a model slot ((flag >> 1) & 7) past their layer's modelArray, or one "
                       "with no file; kept, no model");

    // The havk zoneModels entries; entry i is CollisionInstance::placement i of group "zone".
    std::vector<const Ex::HavokPlacement*> entries;
    {
        std::unordered_set<uint32_t> geometries;
        for (const Ex::HavokPlacement& p : h.placements)
            if (p.group == "zone") { entries.push_back(&p); geometries.insert(p.geometryIndex); }
        out.colliderEntries = uint32_t(entries.size());
        out.colliderGeometries = uint32_t(geometries.size());
    }

    if (!scene.hasBounds) {
        warn.push_back("zone: no parm rect; zones not placed");
        return;
    }
    const float x0 = scene.bounds[0], y0 = scene.bounds[1];

    // ---- Step 2: anchor match, through a hash of (c, r) over the type-3 (tree)
    // placements. M2 measured the link against type-3 placements only; type-1
    // and type-2 ground cover shares the trees' cells, and as candidates it
    // takes ties and gives geometries a second (grass) model on Queensdale and
    // Spirit Vale, which M3 rules out.
    std::unordered_map<uint64_t, std::vector<uint32_t>> byCell;
    for (uint32_t pi = 0; pi < out.placements.size(); ++pi) {
        const ZonePlacement& p = out.placements[pi];
        if (p.layer != UINT32_MAX && out.layers[p.layer].type == 3) byCell[cell_key(p.c, p.r)].push_back(pi);
    }
    std::vector<uint32_t> matched(entries.size(), UINT32_MAX);   // entry -> placement
    for (uint32_t ei = 0; ei < entries.size(); ++ei) {
        const double x = entries[ei]->translate[0], y = entries[ei]->translate[1];
        const double fc = (x - x0) / kCell, fr = (y - y0) / kCell;
        if (!std::isfinite(fc) || !std::isfinite(fr) || std::fabs(fc) > 1e9 || std::fabs(fr) > 1e9) continue;
        const int64_t cc = std::llround(fc), rr = std::llround(fr);
        uint32_t best = UINT32_MAX;
        double bestD2 = 0;
        for (int64_t dc = -1; dc <= 1; ++dc)
            for (int64_t dr = -1; dr <= 1; ++dr) {
                const auto it = byCell.find(cell_key(cc + dc, rr + dr));
                if (it == byCell.end()) continue;
                for (uint32_t pi : it->second) {
                    const ZonePlacement& p = out.placements[pi];
                    if (p.collider != UINT32_MAX) continue;   // claimed
                    float a[2];
                    zone_anchor(x0, y0, p.c, p.r, a);
                    const double dx = x - a[0], dy = y - a[1];
                    if (std::fabs(dx) > kTolerance || std::fabs(dy) > kTolerance) continue;
                    const double d2 = dx * dx + dy * dy;
                    if (best == UINT32_MAX || d2 < bestD2 || (d2 == bestD2 && pi < best)) { best = pi; bestD2 = d2; }
                }
            }
        if (best == UINT32_MAX) continue;
        out.placements[best].collider = ei;
        matched[ei] = best;
        ++out.zonePlacementsLinked;
    }

    // ---- Step 3: geometry vote over the matched entries' slot models.
    std::map<uint32_t, std::map<uint32_t, uint32_t>> votes;   // geometry -> fileId -> count
    for (uint32_t ei = 0; ei < entries.size(); ++ei) {
        if (matched[ei] == UINT32_MAX) continue;
        const uint32_t f = slot_file(out, out.placements[matched[ei]]);
        if (f) ++votes[entries[ei]->geometryIndex][f];
    }
    std::unordered_map<uint32_t, uint32_t> voted;   // geometry -> fileId
    for (const auto& [g, counts] : votes) {
        uint32_t win = 0, winCount = 0;
        for (const auto& [f, n] : counts)
            if (n > winCount) { win = f; winCount = n; }   // a tied count goes to the lower fileId
        voted[g] = win;
        if (counts.size() > 1) {
            std::string line = "zone: geometry " + std::to_string(g) + ": its anchor-matched colliders disagree on the model (";
            bool first = true;
            for (const auto& [f, n] : counts) {
                line += (first ? "" : ", ") + std::string("fileId ") + std::to_string(f) + " x" + std::to_string(n);
                first = false;
            }
            warn.push_back(line + "); using the majority, " + std::to_string(win));
        }
    }
    std::vector<uint32_t> fileOf(entries.size(), 0);
    for (uint32_t ei = 0; ei < entries.size(); ++ei) {
        const auto v = voted.find(entries[ei]->geometryIndex);
        if (v == voted.end()) { ++out.collidersUnlinked; continue; }
        fileOf[ei] = v->second;
        if (matched[ei] == UINT32_MAX) ++out.collidersLinkedByGeometry;
    }

    // ---- Step 4: prop cross-check, per voted geometry that a propModels entry names through a prp2 guid.
    {
        std::unordered_map<uint64_t, uint32_t> fileOfGuid;
        for (const Ex::MapProp& p : props)
            if (p.guid) fileOfGuid.try_emplace(p.guid, p.fileId);
        std::map<uint32_t, std::set<uint32_t>> propFiles;   // geometry -> prop fileIds
        for (const Ex::HavokPlacement& p : h.placements) {
            if (p.group != "prop" || !p.token || !voted.count(p.geometryIndex)) continue;
            const auto it = fileOfGuid.find(p.token);
            if (it != fileOfGuid.end()) propFiles[p.geometryIndex].insert(it->second);
        }
        std::string detail;
        for (const auto& [g, files] : propFiles) {
            ++out.propCrossChecks;
            const uint32_t v = voted[g];
            bool disagree = false;
            for (uint32_t f : files)
                if (f != v) {
                    disagree = true;
                    if (out.propDisagreements < 8)
                        detail += (detail.empty() ? "" : "; ") + std::string("geometry ") + std::to_string(g) +
                                  ": voted fileId " + std::to_string(v) + ", prop fileId " + std::to_string(f);
                }
            out.propDisagreements += disagree;
        }
        if (out.propDisagreements)
            warn.push_back("zone: " + std::to_string(out.propDisagreements) + " of " +
                           std::to_string(out.propCrossChecks) +
                           " prop cross-checks disagree with the geometry's voted model (" + detail +
                           (out.propDisagreements > 8 ? "; ..." : "") + "); the voted model is kept");
    }

    // ---- Step 5: instances. Models join the shared table; a model already there keeps its index.
    std::unordered_map<uint32_t, uint32_t> modelOf;
    for (uint32_t m = 0; m < scene.models.size(); ++m) modelOf.try_emplace(scene.models[m].fileId, m);
    std::unordered_map<uint32_t, uint32_t> used;   // fileId -> model index, for the zone instances
    for (uint32_t ei = 0; ei < entries.size(); ++ei) {
        if (!fileOf[ei]) continue;
        const Ex::HavokPlacement& e = *entries[ei];
        auto [it, added] = modelOf.try_emplace(fileOf[ei], uint32_t(scene.models.size()));
        if (added) scene.models.push_back(PropModel{fileOf[ei]});
        used.emplace(fileOf[ei], it->second);
        ZoneInstance inst;
        inst.model = it->second;
        if (matched[ei] != UINT32_MAX) {
            inst.placement = matched[ei];
            inst.layer = out.placements[matched[ei]].layer;
        }
        inst.collider = ei;
        inst.source = ZoneSource::Collider;
        // D4: the prop rule at `scale`; UNPROVEN for the visual model (setting zoneVisualTransform).
        client_world_matrix(e.translate, e.rotate, e.scale, inst.world);
        out.instances.push_back(inst);
    }
    for (ZoneLayer& L : out.layers)
        for (ZoneLayerModel& M : L.models) {
            const auto it = used.find(M.fileId);
            if (it != used.end()) M.model = it->second;
        }

    if (out.collidersUnlinked)
        warn.push_back("zone: " + std::to_string(out.collidersUnlinked) +
                       " colliders have no tree model (anchor and geometry both failed)");
    if (!out.instances.empty())
        warn.push_back("zone: zoneVisualTransform = client_world_matrix(translate, rotate, scale) is UNPROVEN: "
                       "the prop rule, inferred for zone models (world-foliage spec D4)");
}

uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

uint64_t splitmix64_next(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace castlemist::world
