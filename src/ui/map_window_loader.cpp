/// @file
/// @brief The Map window's worker threads (see detail/map_window_loader.h).

#include "detail/map_window_loader.h"
#include "detail/app_state.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <thread>

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/world/anim.h"
#include "castlemist/world/load_world.h"

namespace castlemist::ui {

namespace {

std::atomic<uint64_t> g_gen{0};

/// Map window threads still running (MapWinLiveThread, mapwin_wait_workers).
std::atomic<int> g_live{0};

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

/// @brief Hands @p payload to the window, or frees it: when the generation
///        has moved on, the window is gone, or the post fails.
template <class T>
void post(HWND wnd, UINT msg, uint64_t gen, std::unique_ptr<T> payload) {
    if (g_gen.load() != gen || !IsWindow(wnd)) return;
    if (PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(payload.get()))) payload.release();
}

bool cancelled(uint64_t gen) { return g_gen.load() != gen; }

} // namespace

MapWinLiveThread::MapWinLiveThread() { ++g_live; }
MapWinLiveThread::~MapWinLiveThread() { --g_live; }

uint64_t mapwin_generation() { return g_gen.load(); }
uint64_t mapwin_next_generation() { return ++g_gen; }

int mapwin_pool_size(unsigned hw) {
    // Every worker shares the one parsed dat (mapwin_start_models), so the
    // thread count no longer multiplies the ~50-70 MB MFT index. Loading
    // still scales past 8 threads (1151420: 68 s with 31 workers, 114 s
    // with 8), so no cap.
    return std::max(1, static_cast<int>(hw) - 1);
}

void mapwin_free_payload(UINT msg, LPARAM lparam) {
    switch (msg) {
    case WM_APP_MAPWIN_WORLD: delete reinterpret_cast<MapWinWorldMsg*>(lparam); break;
    case WM_APP_MAPWIN_COLLISION: delete reinterpret_cast<MapWinCollisionMsg*>(lparam); break;
    case WM_APP_MAPWIN_MODELS: delete reinterpret_cast<MapWinModelsMsg*>(lparam); break;
    default: break;
    }
}

bool mapwin_wait_workers(int ms) {
    const auto until = Clock::now() + std::chrono::milliseconds(ms);
    while (g_live.load() > 0) {
        if (Clock::now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Every thread body below is wrapped in catch (...): nothing may reach
// std::terminate from a detached worker, allocation failures included.

void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId) {
    std::thread([wnd, gen, datPath = std::move(datPath), mapFileId]() {
        MapWinLiveThread live;
        try {
            auto r = std::make_unique<MapWinWorldMsg>();
            r->gen = gen;
            const auto t0 = Clock::now();
            try {
                auto tpl = castlemist::tpl::get_or_auto_load();
                if (!tpl)
                    throw std::runtime_error("no struct template (gw2_packfile.json) found; File > Load Struct JSON...");
                auto dat = std::make_shared<Gw2Dat>();
                load_dat_file(*dat, datPath);
                if (cancelled(gen)) return;
                auto scene = std::make_shared<castlemist::world::WorldScene>(
                    castlemist::world::load_world(*dat, mapFileId, *tpl));
                r->scene = std::move(scene);
                r->dat = std::move(dat);   // handed on to the model pool, read-only from here
            } catch (const std::exception& e) {
                r->error = e.what();
            } catch (...) {
                r->error = "unknown error";
            }
            r->seconds = seconds_since(t0);
            post(wnd, WM_APP_MAPWIN_WORLD, gen, std::move(r));
        } catch (...) {
            // Out of memory before anything could be posted: the window stays
            // at "Loading map..." (and a headless run times out).
        }
    }).detach();
}

void mapwin_start_collision(HWND wnd, uint64_t gen, std::shared_ptr<const castlemist::world::WorldScene> scene) {
    std::thread([wnd, gen, scene = std::move(scene)]() {
        MapWinLiveThread live;
        try {
            if (cancelled(gen)) return;
            const auto t0 = Clock::now();
            auto r = std::make_unique<MapWinCollisionMsg>();
            r->gen = gen;
            try {
                r->index = std::make_shared<castlemist::world::CollisionIndex>(
                    castlemist::world::CollisionIndex::build(*scene, /*includeTerrain=*/true));
            } catch (const std::exception& e) {
                r->error = e.what();
            } catch (...) {
                r->error = "unknown error";
            }
            r->seconds = seconds_since(t0);
            post(wnd, WM_APP_MAPWIN_COLLISION, gen, std::move(r));
        } catch (...) {
        }
    }).detach();
}

int mapwin_start_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat, std::shared_ptr<MapWinModelQueue> queue) {
    // One parsed dat for every worker, as build_map_scene does
    // (src/extract/map_scene.cpp, "Load the unique models IN PARALLEL"):
    // load_dat_file closes the dat's own stream, read_entry_bytes opens a
    // file handle per read, and the lookups load_model_by_fileid makes
    // (get_by_base_id, get_by_file_id, the texture pair resolution) only read
    // the MFT tables. The template is shared behind tpl's mutex; the only
    // mutable static on the texture path is the atomic full-res flag.
    // Workers hold the shared_ptrs, so the dat and the queue (and its scene)
    // outlive the window while any of them still runs.
    const int workers = mapwin_pool_size(std::thread::hardware_concurrency());
    for (int w = 0; w < workers; ++w) {
        std::thread([wnd, gen, dat, queue]() {
            MapWinLiveThread live;
            try {
                const castlemist::world::WorldScene& scene = *queue->scene;
                constexpr size_t kBatch = 32;
                constexpr auto kInterval = std::chrono::milliseconds(250);
                auto batch = std::make_unique<MapWinModelsMsg>();
                batch->gen = gen;
                auto lastPost = Clock::now();
                uint32_t mi = 0;
                // pop() takes the model nearest the camera as of the window's
                // last reorder(): the order follows the camera as it moves.
                while (!cancelled(gen) && queue->queue.pop(mi)) {
                    castlemist::render::WorldModelLoad out;
                    out.model = mi;
                    if (mi < scene.models.size()) {
                        try {
                            // Geometry and header-only textures: the texture
                            // cache decodes each one once, capped, later.
                            std::shared_ptr<ModelPreview> m = load_model_by_fileid(
                                *dat, scene.models[mi].fileId,
                                ModelLoadOptions{/*wantGame=*/false, /*decodeTextures=*/false});
                            if (m) {
                                // Skinning only for a model some instance animates
                                // with a clip it really has; the others drop their
                                // clips so a big map's previews stay small. The
                                // textures stay: they are headers now, and the
                                // materials index them.
                                bool withSkin = false;
                                if (mi < queue->sequencesOf.size())
                                    for (uint64_t seq : queue->sequencesOf[mi])
                                        if (castlemist::world::resolve_prop_clip(m->animClipTokens, seq) >= 0)
                                            withSkin = true;
                                if (!withSkin) {
                                    m->animClips.clear();
                                    m->animClips.shrink_to_fit();
                                    m->animClipTokens.clear();
                                    m->animClipTokens.shrink_to_fit();
                                }
                                // The CPU prep (flatten, AO, MatVtx, ranges) runs
                                // here, so the window's add_models only uploads.
                                auto prep = castlemist::render::prepare_world_model_shared(*m, withSkin);
                                out.preview = std::move(m);
                                out.prep = std::move(prep);
                            }
                        } catch (...) {
                            // counted as failed (null preview / prep)
                            out.preview.reset();
                            out.prep.reset();
                        }
                    }
                    batch->batch.push_back(std::move(out));
                    if (batch->batch.size() >= kBatch || Clock::now() - lastPost >= kInterval) {
                        post(wnd, WM_APP_MAPWIN_MODELS, gen, std::move(batch));
                        batch = std::make_unique<MapWinModelsMsg>();
                        batch->gen = gen;
                        lastPost = Clock::now();
                    }
                }
                batch->workerDone = true;
                post(wnd, WM_APP_MAPWIN_MODELS, gen, std::move(batch));
            } catch (...) {
                // Still tell the window this worker is done, if anything can be allocated.
                try {
                    auto done = std::make_unique<MapWinModelsMsg>();
                    done->gen = gen;
                    done->workerDone = true;
                    post(wnd, WM_APP_MAPWIN_MODELS, gen, std::move(done));
                } catch (...) {
                }
            }
        }).detach();
    }
    return workers;
}

} // namespace castlemist::ui
