/// @file
/// @brief The Map window's worker threads (see detail/map_window_loader.h).

#include "detail/map_window_loader.h"
#include "detail/app_state.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/world/load_world.h"

namespace castlemist::ui {

namespace {

std::atomic<uint64_t> g_gen{0};

/// Map window threads still running (mapwin_wait_workers).
std::atomic<int> g_live{0};

/// Counts one worker thread for its whole body.
struct LiveThread {
    LiveThread() { ++g_live; }
    ~LiveThread() { --g_live; }
    LiveThread(const LiveThread&) = delete;
    LiveThread& operator=(const LiveThread&) = delete;
};

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

/// @brief Hands @p payload to the window, or frees it: when the generation
///        has moved on, the window is gone, or the post fails.
template <class T>
void post(HWND wnd, UINT msg, uint64_t gen, T* payload) {
    if (g_gen.load() != gen || !IsWindow(wnd) || !PostMessageW(wnd, msg, 0, reinterpret_cast<LPARAM>(payload)))
        delete payload;
}

bool cancelled(uint64_t gen) { return g_gen.load() != gen; }

} // namespace

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

void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId) {
    std::thread([wnd, gen, datPath = std::move(datPath), mapFileId]() {
        LiveThread live;
        auto* r = new MapWinWorldMsg;
        r->gen = gen;
        const auto t0 = Clock::now();
        try {
            auto tpl = castlemist::tpl::get_or_auto_load();
            if (!tpl) throw std::runtime_error("no struct template (gw2_packfile.json) found; File > Load Struct JSON...");
            auto dat = std::make_shared<Gw2Dat>();
            load_dat_file(*dat, datPath);
            if (cancelled(gen)) { delete r; return; }
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
        post(wnd, WM_APP_MAPWIN_WORLD, gen, r);
    }).detach();
}

void mapwin_start_collision(HWND wnd, uint64_t gen, std::shared_ptr<const castlemist::world::WorldScene> scene) {
    std::thread([wnd, gen, scene = std::move(scene)]() {
        LiveThread live;
        if (cancelled(gen)) return;
        const auto t0 = Clock::now();
        auto* r = new MapWinCollisionMsg;
        r->gen = gen;
        try {
            r->index = std::make_shared<castlemist::world::CollisionIndex>(
                castlemist::world::CollisionIndex::build(*scene, /*includeTerrain=*/true));
        } catch (const std::exception&) {
            r->index = std::make_shared<castlemist::world::CollisionIndex>();   // empty: Walk stays off
        }
        r->seconds = seconds_since(t0);
        post(wnd, WM_APP_MAPWIN_COLLISION, gen, r);
    }).detach();
}

int mapwin_start_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                        std::shared_ptr<const castlemist::world::WorldScene> scene, std::vector<uint32_t> order) {
    // One parsed dat for every worker, as build_map_scene does
    // (src/extract/map_scene.cpp, "Load the unique models IN PARALLEL"):
    // load_dat_file closes the dat's own stream, read_entry_bytes opens a
    // file handle per read, and the lookups load_model_by_fileid makes
    // (get_by_base_id, get_by_file_id, the texture pair resolution) only read
    // the MFT tables. The template is shared behind tpl's mutex; the only
    // mutable static on the texture path is the atomic full-res flag.
    // Workers hold the shared_ptr, so the dat outlives the window while any
    // of them still runs.
    const int workers = mapwin_pool_size(std::thread::hardware_concurrency());
    auto cursor = std::make_shared<std::atomic<size_t>>(0);
    auto list = std::make_shared<const std::vector<uint32_t>>(std::move(order));
    for (int w = 0; w < workers; ++w) {
        std::thread([wnd, gen, dat, scene, cursor, list]() {
            LiveThread live;
            constexpr size_t kBatch = 32;
            constexpr auto kInterval = std::chrono::milliseconds(250);
            auto* batch = new MapWinModelsMsg;
            batch->gen = gen;
            auto lastPost = Clock::now();
            while (!cancelled(gen)) {
                const size_t i = cursor->fetch_add(1);
                if (i >= list->size()) break;
                const uint32_t mi = (*list)[i];
                std::shared_ptr<const ModelPreview> out;
                if (mi < scene->models.size()) {
                    try {
                        std::shared_ptr<ModelPreview> m =
                            load_model_by_fileid(*dat, scene->models[mi].fileId, /*want_game=*/false);
                        if (m) {
                            // The map view draws geometry only (textures are
                            // sub-project 3); drop the decoded pixels and clips
                            // so a big map's previews stay small.
                            m->textures.clear();
                            m->textures.shrink_to_fit();
                            m->animClips.clear();
                            m->animClips.shrink_to_fit();
                            out = std::move(m);
                        }
                    } catch (const std::exception&) {
                        // counted as failed (null)
                    }
                }
                batch->batch.emplace_back(mi, std::move(out));
                if (batch->batch.size() >= kBatch || Clock::now() - lastPost >= kInterval) {
                    post(wnd, WM_APP_MAPWIN_MODELS, gen, batch);
                    batch = new MapWinModelsMsg;
                    batch->gen = gen;
                    lastPost = Clock::now();
                }
            }
            batch->workerDone = true;
            post(wnd, WM_APP_MAPWIN_MODELS, gen, batch);
        }).detach();
    }
    return workers;
}

} // namespace castlemist::ui
