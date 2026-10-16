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
#include "castlemist/exportgltf/sky_export.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/render/world_sky.h"
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

constexpr size_t kBatch = 32;
constexpr auto kInterval = std::chrono::milliseconds(250);

/// @brief Decodes @p fileId once for the window's texture cache (capped at
///        WorldMaterialSettings::maxTextureSize). A failed decode is still
///        returned (empty mips) so the view counts it. Never throws past
///        allocation of the result.
std::shared_ptr<const CappedTexture> decode_for_cache(Gw2Dat& dat, uint32_t fileId) {
    auto t = std::make_shared<CappedTexture>();
    decode_texture_capped(dat, fileId, castlemist::render::WorldMaterialSettings{}.maxTextureSize, *t);
    return t;
}

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
    case WM_APP_MAPWIN_TEXTURES: delete reinterpret_cast<MapWinTexturesMsg*>(lparam); break;
    case WM_APP_MAPWIN_SKY: delete reinterpret_cast<MapWinSkyMsg*>(lparam); break;
    case WM_APP_MAPWIN_SHADER: delete reinterpret_cast<MapWinShaderMsg*>(lparam); break;
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

void mapwin_start_terrain_textures(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                                   std::shared_ptr<const castlemist::world::WorldScene> scene,
                                   std::shared_ptr<MapWinTextureClaims> claims) {
    std::thread([wnd, gen, dat = std::move(dat), scene = std::move(scene), claims = std::move(claims)]() {
        MapWinLiveThread live;
        try {
            const std::vector<uint32_t> ids =
                (scene && dat) ? castlemist::render::world_texture_ids(*scene, castlemist::render::WorldMaterialSettings{})
                               : std::vector<uint32_t>{};
            auto batch = std::make_unique<MapWinTexturesMsg>();
            batch->gen = gen;
            size_t bytes = 0;
            auto lastPost = Clock::now();
            for (uint32_t id : ids) {
                if (cancelled(gen)) break;
                if (!claims || !claims->claim(id)) continue;
                auto t = decode_for_cache(*dat, id);
                bytes += t->bytes();
                batch->textures.push_back(std::move(t));
                if (batch->textures.size() >= kBatch || bytes >= kMapWinBatchTextureBytes ||
                    Clock::now() - lastPost >= kInterval) {
                    post(wnd, WM_APP_MAPWIN_TEXTURES, gen, std::move(batch));
                    batch = std::make_unique<MapWinTexturesMsg>();
                    batch->gen = gen;
                    bytes = 0;
                    lastPost = Clock::now();
                }
            }
            batch->done = true;
            post(wnd, WM_APP_MAPWIN_TEXTURES, gen, std::move(batch));
        } catch (...) {
            try {
                auto done = std::make_unique<MapWinTexturesMsg>();
                done->gen = gen;
                done->done = true;
                post(wnd, WM_APP_MAPWIN_TEXTURES, gen, std::move(done));
            } catch (...) {
            }
        }
    }).detach();
}

void mapwin_start_sky(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                      std::shared_ptr<const castlemist::world::WorldScene> scene, uint32_t mapFileId) {
    std::thread([wnd, gen, dat = std::move(dat), scene = std::move(scene), mapFileId]() {
        MapWinLiveThread live;
        try {
            auto r = std::make_unique<MapWinSkyMsg>();
            r->gen = gen;
            const auto t0 = Clock::now();
            try {
                auto tpl = castlemist::tpl::get_or_auto_load();
                if (!tpl) throw std::runtime_error("no struct template");
                if (!scene || !dat) throw std::runtime_error("no scene");
                castlemist::exportgltf::sky::SkyInputs in;
                in.sky = scene->environment.sky;
                in.mapFileId = mapFileId;
                if (cancelled(gen)) return;
                castlemist::exportgltf::sky::load_sky_textures(*dat, *tpl, in);
                if (cancelled(gen)) return;
                r->warnings = in.decodeWarnings;
                r->modes = castlemist::render::bake_world_sky(in, kMapWinSkyWidth, r->warnings);
            } catch (const std::exception& e) {
                r->warnings.push_back(std::string("sky: not baked: ") + e.what());
            }
            r->seconds = seconds_since(t0);
            post(wnd, WM_APP_MAPWIN_SKY, gen, std::move(r));
        } catch (...) {
        }
    }).detach();
}

int mapwin_shader_pool_size(unsigned hw) { return std::max(1, (static_cast<int>(hw) - 1) / 2); }

int mapwin_start_shader_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat,
                               std::shared_ptr<MapWinModelQueue> queue, std::shared_ptr<MapWinTextureClaims> claims) {
    // As mapwin_start_models, but each model is traced for its game shaders
    // (wantGame) and only what WorldView::add_game_materials reads is kept:
    // the window's model pool already delivered the geometry.
    const int workers = mapwin_shader_pool_size(std::thread::hardware_concurrency());
    for (int w = 0; w < workers; ++w) {
        std::thread([wnd, gen, dat, queue, claims]() {
            MapWinLiveThread live;
            // Below normal: with the model pool still loading, these threads
            // oversubscribe the cores, and the UI thread's uploads (add_models,
            // add_textures) were preempted into 30-70 ms stalls (Spirit Vale).
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            try {
                const castlemist::world::WorldScene& scene = *queue->scene;
                auto batch = std::make_unique<MapWinShaderMsg>();
                batch->gen = gen;
                size_t texBytes = 0;
                auto lastPost = Clock::now();
                uint32_t mi = 0;
                while (!cancelled(gen) && queue->queue.pop(mi)) {
                    castlemist::render::WorldGameLoad out;
                    out.model = mi;
                    if (mi < scene.models.size()) {
                        try {
                            std::shared_ptr<ModelPreview> m = load_model_by_fileid(
                                *dat, scene.models[mi].fileId,
                                ModelLoadOptions{/*wantGame=*/true, /*decodeTextures=*/false});
                            if (m) {
                                auto slim = std::make_shared<ModelPreview>();
                                slim->gameMaterials = std::move(m->gameMaterials);
                                slim->textures = std::move(m->textures);
                                for (const GameMaterial& gm : slim->gameMaterials)
                                    for (const GameSamplerCPU& smp : gm.samplers) {
                                        if (smp.global != 0 || smp.gameTex < 0 ||
                                            static_cast<size_t>(smp.gameTex) >= slim->textures.size())
                                            continue;
                                        const uint32_t id = slim->textures[static_cast<size_t>(smp.gameTex)].fileId;
                                        if (!claims || !claims->claim(id)) continue;
                                        auto t = decode_for_cache(*dat, id);
                                        texBytes += t->bytes();
                                        batch->textures.push_back(std::move(t));
                                    }
                                out.game = std::move(slim);
                            }
                        } catch (...) {
                            out.game.reset();   // counted as failed
                        }
                    }
                    batch->batch.push_back(std::move(out));
                    if (batch->batch.size() >= kBatch || texBytes >= kMapWinBatchTextureBytes ||
                        Clock::now() - lastPost >= kInterval) {
                        post(wnd, WM_APP_MAPWIN_SHADER, gen, std::move(batch));
                        batch = std::make_unique<MapWinShaderMsg>();
                        batch->gen = gen;
                        texBytes = 0;
                        lastPost = Clock::now();
                    }
                }
                batch->workerDone = true;
                post(wnd, WM_APP_MAPWIN_SHADER, gen, std::move(batch));
            } catch (...) {
                try {
                    auto done = std::make_unique<MapWinShaderMsg>();
                    done->gen = gen;
                    done->workerDone = true;
                    post(wnd, WM_APP_MAPWIN_SHADER, gen, std::move(done));
                } catch (...) {
                }
            }
        }).detach();
    }
    return workers;
}

int mapwin_start_models(HWND wnd, uint64_t gen, std::shared_ptr<Gw2Dat> dat, std::shared_ptr<MapWinModelQueue> queue,
                        std::shared_ptr<MapWinTextureClaims> claims) {
    // One parsed dat for every worker (read-only lookups), the unique models
    // loaded in parallel:
    // load_dat_file closes the dat's own stream, read_entry_bytes opens a
    // file handle per read, and the lookups load_model_by_fileid makes
    // (get_by_base_id, get_by_file_id, the texture pair resolution) only read
    // the MFT tables. The template is shared behind tpl's mutex; the only
    // mutable static on the texture path is the atomic full-res flag.
    // Workers hold the shared_ptrs, so the dat and the queue (and its scene)
    // outlive the window while any of them still runs.
    const int workers = mapwin_pool_size(std::thread::hardware_concurrency());
    for (int w = 0; w < workers; ++w) {
        std::thread([wnd, gen, dat, queue, claims]() {
            MapWinLiveThread live;
            try {
                const castlemist::world::WorldScene& scene = *queue->scene;
                auto batch = std::make_unique<MapWinModelsMsg>();
                batch->gen = gen;
                size_t texBytes = 0;
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
                                // Each diffuse texture once across the pool
                                // (and the terrain thread): the first worker
                                // to claim its fileId decodes it.
                                for (const ModelMaterialCPU& mat : m->materials) {
                                    if (mat.diffuseTex < 0 || mat.diffuseUv > 1 ||
                                        static_cast<size_t>(mat.diffuseTex) >= m->textures.size())
                                        continue;
                                    const uint32_t id = m->textures[static_cast<size_t>(mat.diffuseTex)].fileId;
                                    if (!claims || !claims->claim(id)) continue;
                                    auto t = decode_for_cache(*dat, id);
                                    texBytes += t->bytes();
                                    batch->textures.push_back(std::move(t));
                                }
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
                    if (batch->batch.size() >= kBatch || texBytes >= kMapWinBatchTextureBytes ||
                        Clock::now() - lastPost >= kInterval) {
                        post(wnd, WM_APP_MAPWIN_MODELS, gen, std::move(batch));
                        batch = std::make_unique<MapWinModelsMsg>();
                        batch->gen = gen;
                        texBytes = 0;
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
