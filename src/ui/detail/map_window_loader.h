/// @file
/// @brief The Map window's worker threads: the world loader, the collision
///        index builder and the model pool (map_window_loader.cpp).
///
/// Every result goes back to the window as a PostMessage whose lparam is a
/// heap payload the window takes ownership of. Each payload carries the
/// window generation it was started for; the window drops (and frees) any
/// payload whose generation is not its own, and a worker stops as soon as the
/// current generation moves on (WM_DESTROY bumps it). A worker never touches
/// the window beyond IsWindow + PostMessage, and frees the payload itself when
/// the post cannot be made.
///
/// @warning Not a public header -- internal to `src/ui/`.

#pragma once

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "castlemist/native/gw2dat.h"
#include "castlemist/render/world_foliage_grid.h"
#include "castlemist/render/world_prep.h"
#include "castlemist/world/stream.h"
#include "castlemist/world/walk.h"
#include "castlemist/world/world_scene.h"

struct ModelPreview;
struct CappedTexture;   // castlemist/extract/model_types.h

namespace castlemist::ui {

/// @brief The current Map window generation. A new window takes a fresh value
///        (mapwin_next_generation); WM_DESTROY bumps it again so every worker
///        of the closed window stops and every late post is dropped.
uint64_t mapwin_generation();
uint64_t mapwin_next_generation();

/// @brief Model pool workers for @p hw hardware threads:
///        max(1, hw - 1). The workers share one parsed dat, so the count
///        does not multiply its index.
int mapwin_pool_size(unsigned hw);

/// @brief Waits up to @p ms for every Map window thread to finish (call after
///        the message loop, once WM_DESTROY has bumped the generation). True
///        when none is left.
bool mapwin_wait_workers(int ms);

/// @brief Counts one Map window thread for its whole body (RAII; what
///        mapwin_wait_workers waits on). Every worker body starts with one.
struct MapWinLiveThread {
    MapWinLiveThread();
    ~MapWinLiveThread();
    MapWinLiveThread(const MapWinLiveThread&) = delete;
    MapWinLiveThread& operator=(const MapWinLiveThread&) = delete;
};

/// @brief The models still to load and the scene they index, shared by the
///        window (which reorder()s it as the camera moves) and the pool
///        (which pops from the front). The scene outlives the queue.
///        `sequencesOf` is read-only after the constructor.
struct MapWinModelQueue {
    MapWinModelQueue(std::shared_ptr<const castlemist::world::WorldScene> s, const float cam[3])
        : scene(std::move(s)), queue(*scene, cam), sequencesOf(scene->models.size()) {
        for (const castlemist::world::PropInstance& p : scene->props) {
            if (p.animSequence == 0 || p.model >= sequencesOf.size()) continue;
            std::vector<uint64_t>& seq = sequencesOf[p.model];
            if (std::find(seq.begin(), seq.end(), p.animSequence) == seq.end()) seq.push_back(p.animSequence);
        }
    }
    std::shared_ptr<const castlemist::world::WorldScene> scene;
    castlemist::world::ModelQueue queue;
    /// Per model: the distinct non-zero PropInstance::animSequence of its
    /// instances (whether the worker prepares skinning and keeps the clips).
    std::vector<std::vector<uint64_t>> sequencesOf;
};

/// @brief The texture fileIds some thread of one window already decodes, so
///        each texture is decoded once across the model workers and the
///        terrain-texture thread. Thread-safe.
struct MapWinTextureClaims {
    /// @brief True for the first caller of a non-zero @p fileId; false for 0
    ///        or an id already taken.
    bool claim(uint32_t fileId) {
        if (fileId == 0) return false;
        std::lock_guard<std::mutex> lock(m_);
        return taken_.insert(fileId).second;
    }

private:
    std::mutex m_;
    std::unordered_set<uint32_t> taken_;
};

/// @brief A batch posts once its decoded textures reach this many bytes
///        (CappedTexture::bytes), which keeps each UI-thread add_textures +
///        add_models under the 50 ms target.
inline constexpr size_t kMapWinBatchTextureBytes = 16u * 1024 * 1024;

/// @brief Frees a worker payload by its message (WM_APP_MAPWIN_*); for
///        draining the queue of a window being destroyed.
void mapwin_free_payload(UINT msg, LPARAM lparam);

/// @brief WM_APP_MAPWIN_WORLD: the loaded scene, or the load error.
struct MapWinWorldMsg {
    uint64_t gen = 0;
    std::shared_ptr<const castlemist::world::WorldScene> scene;   ///< null on error
    std::string error;
    double seconds = 0;   ///< dat open + load_world + the foliage grid
    std::shared_ptr<Gw2Dat> dat;   ///< the parsed dat the world came from, for the model pool (read-only)
    /// The scene's FoliageGrid (build_foliage_grid), built on the worker so
    /// WorldView::set_world only takes it over (spec D6); null on error.
    std::unique_ptr<castlemist::render::FoliageGrid> foliage;
    double foliageSeconds = 0;   ///< the grid build alone
};

/// @brief WM_APP_MAPWIN_COLLISION: the walk index of the scene.
struct MapWinCollisionMsg {
    uint64_t gen = 0;
    std::shared_ptr<const castlemist::world::CollisionIndex> index;   ///< null when the build failed
    std::string error;   ///< why the build failed
    double seconds = 0;
};

/// @brief WM_APP_MAPWIN_MODELS: up to 32 models, each with its preview and
///        its worker-prepared mesh (null when it failed), so the window only
///        uploads, plus the diffuse textures this worker claimed and decoded
///        for them (failed decodes included, with empty mips). `workerDone`
///        is set on a worker's last post (possibly with an empty batch).
struct MapWinModelsMsg {
    uint64_t gen = 0;
    std::vector<castlemist::render::WorldModelLoad> batch;
    std::vector<std::shared_ptr<const CappedTexture>> textures;
    bool workerDone = false;
};

/// @brief WM_APP_MAPWIN_TEXTURES: decoded terrain, page and water textures
///        from the terrain-texture thread. `done` is set on its last post.
struct MapWinTexturesMsg {
    uint64_t gen = 0;
    std::vector<std::shared_ptr<const CappedTexture>> textures;
    bool done = false;
};

/// @brief WM_APP_MAPWIN_SKY: the baked sky modes (bake_world_sky) and every
///        warning of the bake and its texture decode, for the proven-only
///        bake (`modes`, `warnings`) and the approximate one (`approxModes`,
///        `approxWarnings`).
struct MapWinSkyMsg {
    uint64_t gen = 0;
    std::vector<castlemist::render::WorldSkyImage> modes;
    std::vector<std::string> warnings;
    std::vector<castlemist::render::WorldSkyImage> approxModes;
    std::vector<std::string> approxWarnings;
    double seconds = 0;   ///< texture decode + bake
};

/// @brief WM_APP_MAPWIN_SHADER: up to 32 models' traced game materials
///        (WorldGameLoad) plus the textures their shaders sample that this
///        worker claimed and decoded. `workerDone` on a worker's last post.
struct MapWinShaderMsg {
    uint64_t gen = 0;
    std::vector<castlemist::render::WorldGameLoad> batch;
    std::vector<std::shared_ptr<const CappedTexture>> textures;
    bool workerDone = false;
};

/// @brief Equirect width of the Map window's sky (height = width / 2).
inline constexpr int kMapWinSkyWidth = 2048;

/// @brief Starts the sky thread: decodes the textures of
///        `scene->environment.sky` (load_sky_textures) and bakes modes 0 and
///        1 at kMapWinSkyWidth (bake_world_sky), proven-only and approximate;
///        posts one WM_APP_MAPWIN_SKY.
void mapwin_start_sky(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                      std::shared_ptr<const castlemist::world::WorldScene> scene, uint32_t mapFileId);

/// @brief Shader pool workers for @p hw hardware threads: max(1, (hw - 1) / 2),
///        so it shares the cores with a model pool still loading.
int mapwin_shader_pool_size(unsigned hw);

/// @brief Starts the shader pool: mapwin_shader_pool_size workers pop models
///        from @p queue (nearest the camera first), load each again with the
///        game shaders traced and header-only textures
///        (ModelLoadOptions{wantGame, !decodeTextures}), keep only its
///        `gameMaterials` and `textures`, decode every material texture those
///        shaders sample that they claim() from @p claims, and post
///        WM_APP_MAPWIN_SHADER batches as the model pool does.
/// @return the number of workers started (each posts workerDone once).
int mapwin_start_shader_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                               std::shared_ptr<MapWinModelQueue> queue, std::shared_ptr<MapWinTextureClaims> claims);

/// @brief Opens a Gw2Dat on @p datPath and runs load_world on @p mapFileId
///        with the struct template; posts WM_APP_MAPWIN_WORLD.
void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId);

/// @brief Builds CollisionIndex::build(scene, includeTerrain = true); posts
///        WM_APP_MAPWIN_COLLISION.
void mapwin_start_collision(HWND wnd, uint64_t gen, std::shared_ptr<const castlemist::world::WorldScene> scene);

/// @brief Starts the terrain-texture thread: decodes every
///        world_texture_ids(*scene) fileId it claim()s from @p claims
///        (decode_texture_capped at WorldMaterialSettings::maxTextureSize) and
///        posts WM_APP_MAPWIN_TEXTURES batches as the model pool does (32
///        textures, 250 ms or 16 MB), `done` on the last. Start it before
///        mapwin_start_models, so the terrain claims its textures first.
void mapwin_start_terrain_textures(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                                   std::shared_ptr<const castlemist::world::WorldScene> scene,
                                   std::shared_ptr<MapWinTextureClaims> claims);

/// @brief Starts the model pool: mapwin_pool_size(hardware_concurrency) workers,
///        sharing the one parsed, read-only @p dat, pop models from @p queue
///        (nearest to the camera at the time each is taken) and post batches
///        of up to 32 models, every 250 ms, or once the batch's textures reach
///        kMapWinBatchTextureBytes. Each worker decodes the diffuse texture of
///        every material (diffuseTex >= 0, diffuseUv <= 1) whose fileId it
///        claim()s from @p claims and attaches it to its batch.
/// @return the number of workers started (each posts workerDone once).
int mapwin_start_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat, std::shared_ptr<MapWinModelQueue> queue,
                        std::shared_ptr<MapWinTextureClaims> claims);

} // namespace castlemist::ui
