/// @file
/// @brief The one content-map (cntc) build shared by every dialog that resolves
///        game ids to dat assets (chat-link decoder, Character Ripper).

#include "detail/app_state.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "castlemist/format/content_map.h"

namespace castlemist::ui {
namespace {

std::atomic<bool> g_cmap_building{false};
std::mutex g_waiters_mutex;
std::vector<HWND> g_waiters;  // windows to tell (WM_APP_CMAP_DONE) when the build finishes

void add_waiter(HWND hwnd) {
    if (!hwnd) return;
    std::lock_guard<std::mutex> lock(g_waiters_mutex);
    if (std::find(g_waiters.begin(), g_waiters.end(), hwnd) == g_waiters.end()) g_waiters.push_back(hwnd);
}

void notify_waiters() {
    std::vector<HWND> waiters;
    {
        std::lock_guard<std::mutex> lock(g_waiters_mutex);
        waiters.swap(g_waiters);
    }
    for (HWND w : waiters)
        if (IsWindow(w)) PostMessageW(w, WM_APP_CMAP_DONE, 0, 0);
}

} // namespace

std::wstring cmap_cache_path() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    return p + L"content_map.bin";
}

// Ready when the map is in memory or its disk cache loads; otherwise starts the
// one-time background build (parsing every cntc pack -- needs the main Gw2.dat
// index loaded for the cntc list) and tells `notify` when it is done.
CmapEnsure ensure_content_map(HWND notify) {
    if (castlemist::cmap::built()) return CmapEnsure::Ready;
    if (g_cmap_building) {
        add_waiter(notify);
        return CmapEnsure::Building;
    }
    if (!g_app->dat_loaded) return CmapEnsure::NeedDat;
    if (castlemist::cmap::load(cmap_cache_path())) return CmapEnsure::Ready;

    std::vector<uint32_t> base_ids;
    if (g_app->index_loaded) base_ids = castlemist::db::query_base_ids("", "cntc", 0, false, false, 100000);
    if (base_ids.empty()) return CmapEnsure::NeedIndex;

    // Copy the MftData for each cntc (baseId -> physical index baseId-1) so the
    // worker touches no shared mutable state.
    // Also each pack's fileId: cross-pack (item->skin) links count packs in fileId order.
    std::vector<MftData> entries;
    std::vector<uint32_t> file_ids;
    entries.reserve(base_ids.size());
    file_ids.reserve(base_ids.size());
    for (uint32_t b : base_ids) {
        uint32_t idx = b - 1;
        if (idx >= g_app->data_gw2.mft_data_list.size()) continue;
        entries.push_back(g_app->data_gw2.mft_data_list[idx]);
        std::vector<uint32_t> fids = get_by_file_id(g_app->data_gw2, b);
        file_ids.push_back(fids.empty() ? 0 : *std::min_element(fids.begin(), fids.end()));
    }
    std::string dat_path = g_app->data_gw2.file_info.file_path;
    std::wstring cache = cmap_cache_path();
    g_cmap_building = true;
    add_waiter(notify);
    std::thread([dat_path, entries, file_ids, cache]() {
        castlemist::cmap::build(dat_path, entries, file_ids, nullptr);
        castlemist::cmap::save(cache);
        g_cmap_building = false;
        notify_waiters();
    }).detach();
    return CmapEnsure::Started;
}

// Throw away the in-memory map and its disk cache, then build it again from the
// dat (e.g. after a game patch -- the cache carries no game-version check).
CmapEnsure rebuild_content_map(HWND notify) {
    if (g_cmap_building) {
        add_waiter(notify);
        return CmapEnsure::Building;
    }
    castlemist::cmap::clear();
    DeleteFileW(cmap_cache_path().c_str());
    return ensure_content_map(notify);
}

bool content_map_building() { return g_cmap_building; }

} // namespace castlemist::ui
