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

void mapwin_start_world(HWND wnd, uint64_t gen, std::string datPath, uint32_t mapFileId) {
    std::thread([wnd, gen, datPath = std::move(datPath), mapFileId]() {
        auto* r = new MapWinWorldMsg;
        r->gen = gen;
        const auto t0 = Clock::now();
        try {
            auto tpl = castlemist::tpl::get_or_auto_load();
            if (!tpl) throw std::runtime_error("no struct template (gw2_packfile.json) found; File > Load Struct JSON...");
            Gw2Dat dat;
            load_dat_file(dat, datPath);
            if (cancelled(gen)) { delete r; return; }
            auto scene = std::make_shared<castlemist::world::WorldScene>(
                castlemist::world::load_world(dat, mapFileId, *tpl));
            r->scene = std::move(scene);
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

int mapwin_start_models(HWND wnd, uint64_t gen, std::string datPath,
                        std::shared_ptr<const castlemist::world::WorldScene> scene, std::vector<uint32_t> order) {
    const unsigned hc = std::thread::hardware_concurrency();
    const int workers = std::max(1, static_cast<int>(hc) - 1);
    auto cursor = std::make_shared<std::atomic<size_t>>(0);
    auto list = std::make_shared<const std::vector<uint32_t>>(std::move(order));
    for (int w = 0; w < workers; ++w) {
        std::thread([wnd, gen, datPath, scene, cursor, list]() {
            constexpr size_t kBatch = 32;
            constexpr auto kInterval = std::chrono::milliseconds(250);
            auto* batch = new MapWinModelsMsg;
            batch->gen = gen;
            Gw2Dat dat;
            try {
                load_dat_file(dat, datPath);
            } catch (const std::exception& e) {
                batch->workerDone = true;
                batch->workerError = e.what();
                post(wnd, WM_APP_MAPWIN_MODELS, gen, batch);
                return;
            }
            auto lastPost = Clock::now();
            while (!cancelled(gen)) {
                const size_t i = cursor->fetch_add(1);
                if (i >= list->size()) break;
                const uint32_t mi = (*list)[i];
                std::shared_ptr<const ModelPreview> out;
                if (mi < scene->models.size()) {
                    try {
                        std::shared_ptr<ModelPreview> m =
                            load_model_by_fileid(dat, scene->models[mi].fileId, /*want_game=*/false);
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
