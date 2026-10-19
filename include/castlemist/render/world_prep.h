/// @file
/// @brief The off-UI-thread half of WorldView's model upload: a streamed
///        model's preview and its prepared (flattened, AO-baked) mesh.
///
/// No Direct3D here: the map window's load workers include this, call
/// prepare_world_model_shared, and post the result; WorldView::add_models
/// then only creates the GPU buffers.
/// @ingroup render
#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "castlemist/world/terrain_blend.h"
#include "castlemist/world/world_scene.h"

struct ModelPreview;   // castlemist/extract/model_types.h

namespace castlemist::render {

struct PreparedModel;   // src/render/world_props.h (internal)

/// @brief WorldView's material settings. Every UNPROVEN value is named in
///        WorldView::warnings() (Task 9).
///
/// Declared here rather than in world_view.h so the load workers (which see
/// only this header) read the same defaults, e.g. `maxTextureSize`;
/// world_view.h includes this header.
struct WorldMaterialSettings {
    float groundRepeatsPerChunk = castlemist::world::kGroundRepeatsPerChunk;   ///< UNPROVEN, note §1.6
    float uvScrollUnitSeconds = 1;    ///< UNPROVEN: offset = frac(scroll * t / unit) on uvIndex, note §4
    bool terrainNormalMaps = true;    ///< UNPROVEN sign: tangent +X east, bitangent = increasing t (south), note §1.2, §2
    float terrainSpecPower = 32;      ///< castlemist's choice: forward stand-in for the light buffer's L.a, note §1.4
    int maxTextureSize = 512;         ///< memory bound (spec)
    int waterBumpTextureIndex = 1;    ///< UNPROVEN beyond AMAT 187551 (ssBump0 = textureIndex 1), note §6
    float waterTileUnit = 1000;       ///< UNPROVEN: units of bumpTile/bumpSpeed/bumpAngle, note §6
    float waterSpecPower = 64;        ///< castlemist's choice
    /// UNPROVEN (note §6, ruling P10): bumpAngle0/1 read as degrees. Queensdale's
    /// stored 143.3 / 61.7 read as degrees; no shader transcription settles it.
    bool waterBumpAngleDegrees = true;
    /// castlemist's choice (note §6, ruling P10): the normal's xy gain is
    /// bumpAmount * this. Queensdale's bumpAmount 23.73 and its bump 186359
    /// (rms |xy| 0.495 per sample, ~0.70 for the two-sample sum) then give an
    /// rms slope of 0.70 * 0.475 = 0.33, an ~18 degree tilt.
    float waterBumpStrengthScale = 0.02f;
    /// castlemist's choice: colour textures decode sRGB to linear; the shaders
    /// gamma-encode (world_view_shaders.h:53).
    bool srgbColourTextures = true;
    /// castlemist's choice (Shader shading): props whose world sphere is
    /// within this many map units of the camera draw with the game's shaders,
    /// at most `shaderMaxInstances` of them, nearest first. The game shaders
    /// read their World matrix from a constant buffer, so they draw one
    /// instance per call: the map's instanced Materials pass draws the rest.
    float shaderRange = 6000;
    int shaderMaxInstances = 1500;
};

/// @brief The terrain and water textures of @p s, for the terrain-texture
///        thread: distinct non-zero fileIds, per chunk in index order
///        `textureFileIds[0..3]`, `normalFileIds[0..3]`, `pickerFileId`,
///        `picker2FileId`; then the water bump
///        `environment.water[light.presetIndex].textureFileIds[cfg.waterBumpTextureIndex]`
///        when it exists. No Direct3D; safe on any thread.
std::vector<uint32_t> world_texture_ids(const castlemist::world::WorldScene& s, const WorldMaterialSettings& cfg);

/// @brief One streamed model, ready for WorldView::add_models.
///
/// Not an aggregate on purpose: with a constructor that needs all three, a
/// braced `{index, preview}` pair cannot convert to it, so the pair overload of
/// WorldView::add_models stays unambiguous for `add_models({{0u, cube}})`.
struct WorldModelLoad {
    WorldModelLoad() = default;
    WorldModelLoad(uint32_t m, std::shared_ptr<const ModelPreview> p, std::shared_ptr<const PreparedModel> q)
        : model(m), preview(std::move(p)), prep(std::move(q)) {}
    uint32_t model = 0;                            ///< index into WorldScene::models
    std::shared_ptr<const ModelPreview> preview;   ///< picking, materials, joints, clips; null = failed
    std::shared_ptr<const PreparedModel> prep;     ///< null or !ok = failed
};

/// @brief The Map window's time of day for the sky: Day shows sky mode 0,
///        Night mode 1 (docs/research/gw2-sky.md §3; modes 2 and 3 are
///        UNPROVEN and not shown), Off the flat background.
enum class WorldSkyTime { Day, Night, Off };
/// @brief Which sky bake the Map window shows: the approximate sky (haze,
///        sun glow, clouds, material cards with labelled inputs; gw2-sky.md
///        amendment 2026-10-17) or the proven-only one.
enum class WorldSkyKind { Approx, Proven };

/// @brief One baked sky mode for WorldView::set_sky: an equirect laid out
///        as exportgltf's render_equirect writes it (Unity Skybox/Panoramic:
///        row 0 looks up, the centre at Unity +X = map east), RGBA8,
///        display-referred.
struct WorldSkyImage {
    int mode = 0;   ///< index into MapSky::modes
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    /// The colour distant ground fades to and the sky shows below the
    /// horizon, display values. Set by the approximate bake to its
    /// FogColorFar (the game's own fog colour); when unset the view takes
    /// world_sky_horizon of the image.
    bool hasHorizon = false;
    float horizon[3] = {0, 0, 0};
};

/// @brief One streamed model's game-shader materials for
///        WorldView::add_game_materials: a preview loaded with the game
///        shaders traced (ModelLoadOptions::wantGame) whose `gameMaterials`
///        and `textures` (headers: fileIds) are what the view reads; its
///        meshes may be dropped. Null `game` = the trace failed.
struct WorldGameLoad {
    uint32_t model = 0;                        ///< index into WorldScene::models
    std::shared_ptr<const ModelPreview> game;
};

/// @brief prepare_world_model (flatten, AO bake, MatVtx, per-submesh ranges,
///        SkinVtx when @p withSkin) on a shared, immutable result. Touches no
///        Direct3D and no shared mutable state: safe on any thread.
std::shared_ptr<const PreparedModel> prepare_world_model_shared(const ModelPreview& m, bool withSkin);

} // namespace castlemist::render
