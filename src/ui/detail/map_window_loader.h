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
#include <string>
#include <utility>
#include <vector>

#include "castlemist/native/gw2dat.h"
#include "castlemist/render/world_prep.h"
#include "castlemist/world/stream.h"
#include "castlemist/world/walk.h"
#include "castlemist/world/world_scene.h"

struct ModelPreview;

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

/// @brief Frees a worker payload by its message (WM_APP_MAPWIN_*); for
///        draining the queue of a window being destroyed.
void mapwin_free_payload(UINT msg, LPARAM lparam);

/// @brief WM_APP_MAPWIN_WORLD: the loaded scene, or the load error.
struct MapWinWorldMsg {
    uint64_t gen = 0;
    std::shared_ptr<const castlemist::world::WorldScene> scene;   ///< null on error
    std::string error;
    double seconds = 0;   ///< dat open + load_world
    std::shared_ptr<Gw2Dat> dat;   ///< the parsed dat the world came from, for the model pool (read-only)
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
///        uploads. `workerDone` is set on a worker's last post (possibly with
///        an empty batch).
struct MapWinModelsMsg {
    uint64_t gen = 0;
    std::vector<castlemist::render::WorldModelLoad> batch;
    bool workerDone = false;
};

/// @brief Opens a Gw2Dat on @p datPath and runs load_world on @p mapFileId
///        with the struct template; posts WM_APP_MAPWIN_WORLD.
void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId);

/// @brief Builds CollisionIndex::build(scene, includeTerrain = true); posts
///        WM_APP_MAPWIN_COLLISION.
void mapwin_start_collision(HWND wnd, uint64_t gen, std::shared_ptr<const castlemist::world::WorldScene> scene);

/// @brief Starts the model pool: mapwin_pool_size(hardware_concurrency) workers,
///        sharing the one parsed, read-only @p dat, pop models from @p queue
///        (nearest to the camera at the time each is taken) and post batches
///        of up to 32 or every 250 ms.
/// @return the number of workers started (each posts workerDone once).
int mapwin_start_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat, std::shared_ptr<MapWinModelQueue> queue);

} // namespace castlemist::ui
