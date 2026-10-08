/// @file
/// @brief WorldScene: one renderer-free description of a GW2 map (terrain,
///        props, collision, water, environment), in map space.
/// @ingroup world
#pragma once

#include "castlemist/native/gw2model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace castlemist::world {

/// @brief The textures and blend pages one terrain chunk is painted with
///        (docs/research/gw2-world-frame.md §4).
struct TerrainMaterial {
    bool resolved = false;
    /// Ground colour textures bound to the material tokens "color", "colorb",
    /// "colorc", "colord" (slots 0-3, dat order on every test map); 0 where the
    /// chunk binds none.
    std::vector<uint32_t> textureFileIds;
    /// Normal maps bound to "normal", "normalb", "normalc", "normald" (slot k
    /// pairs with textureFileIds[k]); 0 where the chunk binds none.
    std::vector<uint32_t> normalFileIds;
    uint32_t materialFileId = 0;            ///< loResMaterial.materialFile (the terrain shader, AMAT)
    uint32_t pickerFileId = 0, picker2FileId = 0;  ///< blend pages: PIMG layer 0 ("blend") / 1 ("modx"); 0 = none
    /// Stored `solidColor` of the layer 0 / 1 page when that page has no file
    /// (byte order as stored, channel meaning UNPROVEN); zero otherwise.
    uint8_t pickerSolid[4] = {0, 0, 0, 0}, picker2Solid[4] = {0, 0, 0, 0};
    /// This chunk's sub-rect in its page, in page-image UV: u from the image's
    /// first column (west edge), v from its first stored row (north edge).
    float pickerOffset[2] = {0, 0};
    float pickerScale = 0.25f;              ///< sub-rect size: 1 / chunks per page side (4 on every test map)
    float uvScale = 0;                      ///< ground-texture tiling per chunk; 0 = unknown (UNPROVEN, §4.4)
    uint8_t tiling[3] = {0, 0, 0};          ///< the chunk's `tiling` bytes as stored (meaning UNPROVEN, §4.4)
};

/// @brief One heightfield chunk (layout: docs/research/gw2-world-frame.md §3).
struct TerrainChunk {
    int cx = 0, cy = 0;                     ///< cx counts east from the map's west edge, cy south from its north edge
    float rect[4] = {0, 0, 0, 0};           ///< x0, y0, x1, y1 in map space
    int samples = 0;                        ///< per side (segments + 1)
    /// samples*samples, row-major, map-space Z as stored (up = -Z). Row 0 is
    /// the north edge (y = rect[3]), column 0 the west edge (x = rect[0]);
    /// sample (i, j) sits at (x0 + i*(x1-x0)/(samples-1), y1 - j*(y1-y0)/(samples-1)).
    std::vector<float> heights;
    TerrainMaterial material;
};

/// @brief The map's terrain as a grid of chunks.
struct Terrain {
    bool present = false;
    int chunksX = 0, chunksY = 0;
    std::vector<TerrainChunk> chunks;
};

/// @brief A distinct prop model, referenced by PropInstance::model.
struct PropModel { uint32_t fileId = 0; };

/// @brief One placed prop.
struct PropInstance {
    uint32_t model = 0;                     ///< index into WorldScene::models
    float world[16] = {};                   ///< column-major, map space
    float pos[3] = {}, rot[3] = {}; float scale = 1;  ///< as stored, for references
    std::string group;                      ///< "propArray" | "propAnimArray" | "propMetaArray" | "propInstanceArray"
};

/// @brief A collision mesh in local space.
struct CollisionMesh { std::vector<float> verts; std::vector<uint32_t> indices; };

/// @brief One placed collision mesh.
struct CollisionInstance { uint32_t mesh = 0; float world[16] = {}; std::string group; };   // "obs" | "prop" | "zone"

struct Collision { std::vector<CollisionMesh> meshes; std::vector<CollisionInstance> instances; };

struct Water {
    std::vector<castlemist::model::Extractor::MapWaterSurface> surfaces;
    castlemist::model::Extractor::MapShore shore;
};

struct Environment {
    castlemist::model::Extractor::MapSky sky;
    castlemist::model::Extractor::MapEnvLight light;
};

/// @brief Animated content.
struct Motion { std::vector<uint32_t> animatedProps; };   ///< indices into props

/// @brief Everything known about one map.
struct WorldScene {
    uint32_t mapFileId = 0;
    float bounds[4] = {0, 0, 0, 0}; bool hasBounds = false;   ///< parm rect, map space
    Terrain terrain;
    std::vector<PropModel> models;
    std::vector<PropInstance> props;
    Collision collision;
    Water water;
    Environment environment;
    Motion motion;
    std::vector<std::string> warnings;
};

} // namespace castlemist::world
