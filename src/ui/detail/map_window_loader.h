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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "castlemist/world/walk.h"
#include "castlemist/world/world_scene.h"

struct ModelPreview;

namespace castlemist::ui {

/// @brief The current Map window generation. A new window takes a fresh value
///        (mapwin_next_generation); WM_DESTROY bumps it again so every worker
///        of the closed window stops and every late post is dropped.
uint64_t mapwin_generation();
uint64_t mapwin_next_generation();

/// @brief WM_APP_MAPWIN_WORLD: the loaded scene, or the load error.
struct MapWinWorldMsg {
    uint64_t gen = 0;
    std::shared_ptr<const castlemist::world::WorldScene> scene;   ///< null on error
    std::string error;
    double seconds = 0;   ///< dat open + load_world
};

/// @brief WM_APP_MAPWIN_COLLISION: the walk index of the scene.
struct MapWinCollisionMsg {
    uint64_t gen = 0;
    std::shared_ptr<const castlemist::world::CollisionIndex> index;
    double seconds = 0;
};

/// @brief WM_APP_MAPWIN_MODELS: up to 32 models (index into scene.models,
///        preview or null when it failed). `workerDone` is set on a worker's
///        last post (possibly with an empty batch).
struct MapWinModelsMsg {
    uint64_t gen = 0;
    std::vector<std::pair<uint32_t, std::shared_ptr<const ModelPreview>>> batch;
    bool workerDone = false;
    std::string workerError;   ///< set when the worker could not open the dat
};

/// @brief Opens a Gw2Dat on @p datPath and runs load_world on @p mapFileId
///        with the struct template; posts WM_APP_MAPWIN_WORLD.
void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId);

/// @brief Builds CollisionIndex::build(scene, includeTerrain = true); posts
///        WM_APP_MAPWIN_COLLISION.
void mapwin_start_collision(HWND wnd, uint64_t gen, std::shared_ptr<const castlemist::world::WorldScene> scene);

/// @brief Starts the model pool: max(1, hardware_concurrency - 1) workers,
///        each with its own Gw2Dat, take models from @p order through one
///        shared cursor and post batches of up to 32 or every 250 ms.
/// @return the number of workers started (each posts workerDone once).
int mapwin_start_models(HWND wnd, uint64_t gen, std::string datPath,
                        std::shared_ptr<const castlemist::world::WorldScene> scene, std::vector<uint32_t> order);

} // namespace castlemist::ui
