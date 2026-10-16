/// @file
/// @brief Zones: a map's `zon2` zone scatter in WorldScene, and the link from
///        each havk `zoneModels` collider to its tree model
///        (docs/superpowers/specs/2026-10-16-world-foliage-design.md D2-D4).
///
/// Everything is in map space, as stored, with fields named as in the
/// template. The grid, the model slot and the link are the data's (spec M2,
/// M3, D3), not proven to be the client's rule.
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"

#include <cstdint>
#include <vector>

namespace castlemist::world {

struct WorldScene;

/// @brief One `modelArray` entry of a zone layer (spec D2).
struct ZoneLayerModel {
    uint32_t fileId = 0;            ///< modelArray[i].filename
    uint32_t model = UINT32_MAX;    ///< index into WorldScene::models once the model is used; else UINT32_MAX
    float probability = 0; uint32_t flags = 0; float hslOffset[3]{}; float zOffsets[2]{}; uint64_t permutation = 0;
};

/// @brief One `layerDefArray` entry (spec D2).
struct ZoneLayer {
    uint32_t defToken = 0; uint32_t indexInDef = 0;     ///< 0-based
    uint8_t type = 0, width = 0, height = 0, radiusGround = 0, sortGroup = 0, tiling = 0, noise = 0;
    float scaleRange[2]{}, probability = 0, fadeRange[2]{}, rotRange[3][2]{}, hslRanges[4][2]{};
    float instanceScaleJitter = 0; uint32_t layerFlags = 0; uint32_t materialFileId = 0;
    std::vector<ZoneLayerModel> models;
};

/// @brief One `zoneArray` entry (spec D2).
struct Zone {
    uint32_t defToken = 0; int32_t vertRect[4]{}; uint8_t seed = 0; float range[2]{}, zPos = 0, waterHeight = 0;
    uint32_t zoneFlags = 0; uint16_t broadId = 0;
    uint32_t firstPlacement = 0, placementCount = 0;    ///< into ZoneScatter::placements
    std::vector<float> collideData;                     ///< normalX, normalY, zPos triples, as stored
    std::vector<uint16_t> offsetData;                   ///< as stored
};

/// @brief One non-zero flag byte of the run-length walk (spec D2, Measurements notation).
struct ZonePlacement {
    uint32_t zone = 0, layer = UINT32_MAX;              ///< layer: index into ZoneScatter::layers; UINT32_MAX = unresolved
    int32_t c = 0, r = 0;                               ///< grid point (Measurements, notation)
    uint8_t flag = 0;                                   ///< as stored: layer (f>>4), slot ((f>>1)&7), bit 0
    uint32_t collider = UINT32_MAX;                     ///< index of the havk zoneModels entry linked to it, else UINT32_MAX
};

/// @brief Where a ZoneInstance came from (spec D2).
enum class ZoneSource : uint8_t { Collider, Scatter, Density };

/// @brief One drawn zone model (spec D2, D4).
struct ZoneInstance {
    uint32_t model = 0;             ///< index into WorldScene::models
    uint32_t layer = UINT32_MAX;    ///< index into ZoneScatter::layers (UINT32_MAX: a collider linked by geometry only)
    uint32_t placement = UINT32_MAX;///< index into placements (UINT32_MAX: as above)
    uint32_t collider = UINT32_MAX; ///< havk zoneModels index (Collider source only)
    float world[16] = {};           ///< column-major, model-local -> map space (D4)
    ZoneSource source = ZoneSource::Collider;
};

/// @brief The scatter rule in force (spec D5, D11 `zoneScatter`). Phase 1 places colliders only.
enum class ZoneScatterMode : uint8_t { Colliders, Exact, DensityMatched };

/// @brief The map's zone scatter (spec D2).
struct ZoneScatter {
    bool present = false;
    std::vector<ZoneLayer> layers;              ///< every layer of every def, in def order
    std::vector<Zone> zones;
    std::vector<ZonePlacement> placements;      ///< zoneArray order, then walk order
    std::vector<ZoneInstance> instances;
    /// zonePlacementsLinked: zoneModels entries linked by anchor (D3 step 2);
    /// collidersLinkedByGeometry: unmatched entries given their geometry's model (step 3);
    /// collidersUnlinked: entries with neither (step 3).
    uint32_t zonePlacementsLinked = 0, collidersLinkedByGeometry = 0, collidersUnlinked = 0;
    /// colliderEntries: havk zoneModels entries; colliderGeometries: their distinct geometryIndex;
    /// propCrossChecks / propDisagreements: geometries checked against a prp2 prop (step 4) and
    /// those whose prop has another fileId; placementsUnresolved: def or layer did not resolve (step 1).
    uint32_t colliderEntries = 0, colliderGeometries = 0, propCrossChecks = 0, propDisagreements = 0,
             placementsUnresolved = 0;
    ZoneScatterMode mode = ZoneScatterMode::Colliders;
};

/// The anchor of grid point (c, r) for parm origin (x0, y0): (x0 + 48(c-1), y0 + 48 r) (spec M2).
void zone_anchor(float x0, float y0, int32_t c, int32_t r, float out[2]);

/// @brief Spec D3 steps 1-5: decode the placements, link every havk
///        `zoneModels` entry to a model (anchor ±24 to a type-3 placement, as
///        M2 measured, then the geometry vote),
///        cross-check against props, and make one Collider instance per
///        linked entry with `world = client_world_matrix(translate, rotate,
///        scale)` (D4; UNPROVEN, warned as `zoneVisualTransform`).
///
/// Anchors use scene.bounds[0..1] (the parm rect); !hasBounds: warn "zone: no
/// parm rect; zones not placed" and stop (after decoding). Appends zone-only
/// models to scene.models (after the prop models, in the order instances are
/// made); a model already in the table keeps its index, so PropInstance::model
/// is unchanged. Every non-zero count is one `zone:` warning. Fills
/// scene.zones; replaces anything in it. A `zon2` that is not present gives
/// `zones.present == false` and no warning. Never throws for bad data.
void build_zones(const castlemist::model::Extractor::MapZon2& z, const castlemist::model::Extractor::MapHavok& h,
                 const std::vector<castlemist::model::Extractor::MapProp>& props, WorldScene& scene);

/// MurmurHash3's 32-bit finaliser (spec D9's thinning hash).
uint32_t fmix32(uint32_t h);

/// One splitmix64 step: advances `state` and returns the output.
uint64_t splitmix64_next(uint64_t& state);

} // namespace castlemist::world
