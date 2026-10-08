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

/// @brief The textures and blend pages one terrain chunk is painted with.
struct TerrainMaterial {
    bool resolved = false;
    std::vector<uint32_t> textureFileIds;   ///< ground textures, dat order
    uint32_t materialFileId = 0;            ///< the chunk's terrain material (shader) file
    uint32_t pickerFileId = 0, picker2FileId = 0;  ///< blend pages (layer 0 / 1); 0 = none
    float pickerOffset[2] = {0, 0};         ///< this chunk's sub-rect in its page
    float pickerScale = 0.25f;              ///< page covers 4x4 chunks
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
