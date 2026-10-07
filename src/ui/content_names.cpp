/// @file
/// @brief Game names for content objects, and the info panel's "Game content"
///        section built from them. Names come from the public GW2 API (no key)
///        on a background thread, a batch at a time, and are kept in
///        content_names.tsv beside the exe so each id is only ever fetched once.

#include "detail/app_state.h"
#include "detail/content_links.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "castlemist/character/gw2_api.h"
#include "castlemist/character/http.h"
#include "castlemist/format/chat_link.h"

namespace castlemist::ui {

namespace cmap = castlemist::cmap;

// ---- formatting (no Win32, no network)

std::wstring format_content_ref(const cmap::ContentRef& r, const NameLookup& name) {
    const cmap::ContentKind* k = cmap::content_kind(r.type);
    wchar_t head[64];
    if (k)
        swprintf(head, 64, L"%hs %u", k->name, r.id);
    else
        swprintf(head, 64, L"Type %u #%u", r.type, r.id);
    std::wstring s = head;
    if (!k) return s;
    if (const std::string* n = name(r.type, r.id)) {
        if (!n->empty()) s += L"  " + utf8_to_wide(*n);
    } else {
        s += L"  (looking up name...)";
    }
    if (k->chat_header) {
        const std::string link = castlemist::chat::encode_id(k->chat_header, r.id);
        s += L"  " + std::wstring(link.begin(), link.end());
    }
    return s;
}

std::wstring format_content_links(const std::vector<LinkedObject>& users, size_t total, const NameLookup& name) {
    std::wstring s = L"\r\n--- Game content ---\r\n";
    if (total == 0) return s + L"No content object (item, skin, map, ...) lists this file as an asset.\r\n";
    wchar_t head[96];
    swprintf(head, 96, L"Used by %zu content object%ls:\r\n", total, total == 1 ? L"" : L"s");
    s += head;
    constexpr size_t kGrantersShown = 8;
    for (const LinkedObject& u : users) {
        s += L"  " + format_content_ref(u.ref, name) + L"\r\n";
        for (size_t i = 0; i < u.granted_by.size() && i < kGrantersShown; ++i)
            s += L"      unlocked by " + format_content_ref(u.granted_by[i], name) + L"\r\n";
        if (u.granted_by.size() > kGrantersShown) {
            wchar_t more[64];
            swprintf(more, 64, L"      ... and %zu more\r\n", u.granted_by.size() - kGrantersShown);
            s += more;
        }
    }
    if (total > users.size()) {
        wchar_t more[64];
        swprintf(more, 64, L"  ... and %zu more\r\n", total - users.size());
        s += more;
    }
    return s;
}

namespace {
constexpr const wchar_t* kBlank = L" \t";

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
} // namespace

bool is_name_query(const std::wstring& text) {
    for (wchar_t c : text)
        if (!wcschr(kBlank, c) && (c < L'0' || c > L'9')) return true;
    return false;
}

std::wstring name_query_key(const std::wstring& text) {
    const size_t b = text.find_first_not_of(kBlank);
    if (b == std::wstring::npos) return {};
    const size_t e = text.find_last_not_of(kBlank);
    return lower(text.substr(b, e - b + 1));
}

bool name_matches(const std::string& name, const std::wstring& key) {
    if (name.empty() || key.empty()) return false;
    return lower(utf8_to_wide(name)).find(key) != std::wstring::npos;
}

// ---- the name cache

namespace {

std::mutex g_names_mutex;
std::unordered_map<uint64_t, std::string> g_names;  // "" = the API has no name for it
std::set<uint64_t> g_wanted;                         // queued for the worker
bool g_names_loaded = false;
bool g_worker_running = false;
ULONGLONG g_offline_until = 0;  // after a failed fetch, don't retry before this tick
HWND g_names_notify = nullptr;

std::wstring names_cache_path() {
    std::wstring p = cmap_cache_path();
    return p.substr(0, p.find_last_of(L"\\/") + 1) + L"content_names.tsv";
}

// "type\tid\tname" lines. Call with g_names_mutex held.
void load_names_locked() {
    if (g_names_loaded) return;
    g_names_loaded = true;
    FILE* f = _wfopen(names_cache_path().c_str(), L"rb");
    if (!f) return;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        unsigned type = 0, id = 0;
        int used = 0;
        if (std::sscanf(line, "%u\t%u\t%n", &type, &id, &used) < 2 || used == 0) continue;
        std::string n = line + used;
        while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
        g_names[content_name_key(type, id)] = n;
    }
    std::fclose(f);
}

void append_names(const std::vector<std::pair<uint64_t, std::string>>& fresh) {
    FILE* f = _wfopen(names_cache_path().c_str(), L"ab");
    if (!f) return;
    for (const auto& [k, n] : fresh) {
        std::string clean = n;
        std::replace(clean.begin(), clean.end(), '\t', ' ');
        std::replace(clean.begin(), clean.end(), '\n', ' ');
        std::replace(clean.begin(), clean.end(), '\r', ' ');
        std::fprintf(f, "%u\t%u\t%s\n", static_cast<unsigned>(k >> 32), static_cast<unsigned>(k), clean.c_str());
    }
    std::fclose(f);
}

// Drains g_wanted a batch at a time, grouped by type (one endpoint each).
void names_worker() {
    castlemist::character::WinHttpClient http;
    castlemist::character::Gw2Api api(http, "");
    for (;;) {
        std::map<uint32_t, std::vector<uint32_t>> batch;
        {
            std::lock_guard<std::mutex> lock(g_names_mutex);
            if (g_wanted.empty()) {
                g_worker_running = false;
                break;
            }
            // A few API pages per round, so a big list fills in as it goes.
            constexpr size_t kPerRound = 600;
            size_t taken = 0;
            for (auto it = g_wanted.begin(); it != g_wanted.end() && taken < kPerRound; ++taken) {
                batch[static_cast<uint32_t>(*it >> 32)].push_back(static_cast<uint32_t>(*it));
                it = g_wanted.erase(it);
            }
        }
        std::vector<std::pair<uint64_t, std::string>> fresh;
        bool failed = false;
        for (const auto& [type, ids] : batch) {
            const cmap::ContentKind* k = cmap::content_kind(type);
            if (!k) continue;
            try {
                std::map<uint32_t, std::string> got = api.names(k->api, ids);
                for (uint32_t id : ids) {
                    auto it = got.find(id);
                    fresh.push_back({content_name_key(type, id), it == got.end() ? std::string() : it->second});
                }
            } catch (const std::exception&) {
                failed = true;  // offline / API down: leave these unknown, try again later
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_names_mutex);
            for (const auto& [key, n] : fresh) g_names[key] = n;
            if (failed) {
                g_offline_until = GetTickCount64() + 60 * 1000;
                g_wanted.clear();
            }
        }
        append_names(fresh);
        if (!fresh.empty() && g_names_notify && IsWindow(g_names_notify))
            PostMessageW(g_names_notify, WM_APP_CONTENT_NAMES_DONE, 0, 0);
    }
}

// ---- downloading every name at once (Tools > Download all game names)

std::atomic<bool> g_bulk_running{false};
bool g_bulk_after_cmap = false;  // start once the content map build finishes (UI thread only)
std::string g_bulk_error;        // why the last bulk run stopped; read after WM_APP_NAMES_BULK_DONE

// One page of names, waiting out the API's rate limit (HTTP 429) or a timed-out
// request (status 0) a few times before giving up. Other failures throw.
std::map<uint32_t, std::string> fetch_page(castlemist::character::Gw2Api& api, const char* endpoint,
                                           const std::vector<uint32_t>& ids) {
    for (int attempt = 0;; ++attempt) {
        try {
            return api.names(endpoint, ids);
        } catch (const castlemist::character::ApiError& e) {
            if ((e.status() != 429 && e.status() != 0) || attempt >= 6) throw;
            Sleep(e.status() == 429 ? 15000 : 5000);
        }
    }
}

void bulk_worker(HWND notify) {
    // Every object the API can name that isn't cached yet, by endpoint.
    std::map<uint32_t, std::vector<uint32_t>> todo;  // type -> ids
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        load_names_locked();
        for (const cmap::ContentRef& r : cmap::objects())
            if (cmap::content_kind(r.type) && !g_names.count(content_name_key(r.type, r.id))) {
                todo[r.type].push_back(r.id);
                ++total;
            }
    }
    castlemist::character::WinHttpClient http;
    castlemist::character::Gw2Api api(http, "");
    constexpr DWORD kPaceMs = 250;  // ~4 requests/s, under the ~300/min limit
    size_t done = 0;
    bool ok = true;
    PostMessageW(notify, WM_APP_NAMES_PROGRESS, 0, static_cast<LPARAM>(total));
    // The most useful names first; achievements last -- that endpoint is slow
    // (~20 s for 200 ids, past the client's timeout), so it goes 50 at a time.
    std::vector<uint32_t> order;
    for (const auto& [type, ids] : todo) order.push_back(type);
    std::stable_sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return (a == 0) < (b == 0); });
    for (uint32_t type : order) {
        std::vector<uint32_t>& ids = todo[type];
        std::sort(ids.begin(), ids.end());
        const char* endpoint = cmap::content_kind(type)->api;
        const size_t kPage = type == 0 ? 50 : 200;  // 200 = the API's ?ids= limit
        for (size_t at = 0; at < ids.size() && ok; at += kPage) {
            const std::vector<uint32_t> page(ids.begin() + at, ids.begin() + std::min(ids.size(), at + kPage));
            std::vector<std::pair<uint64_t, std::string>> fresh;
            try {
                std::map<uint32_t, std::string> got = fetch_page(api, endpoint, page);
                for (uint32_t id : page) {
                    auto it = got.find(id);
                    fresh.push_back({content_name_key(type, id), it == got.end() ? std::string() : it->second});
                }
            } catch (const std::exception& e) {
                g_bulk_error = std::string(endpoint) + ": " + e.what();
                ok = false;  // offline / API down: keep what we have; running it again resumes
                break;
            }
            {
                std::lock_guard<std::mutex> lock(g_names_mutex);
                for (const auto& [key, n] : fresh) g_names[key] = n;
            }
            append_names(fresh);
            done += page.size();
            PostMessageW(notify, WM_APP_NAMES_PROGRESS, static_cast<WPARAM>(done), static_cast<LPARAM>(total));
            if ((done / kPage) % 10 == 0) PostMessageW(notify, WM_APP_CONTENT_NAMES_DONE, 0, 0);
            Sleep(kPaceMs);
        }
        if (!ok) break;
    }
    g_bulk_running = false;
    PostMessageW(notify, WM_APP_CONTENT_NAMES_DONE, 0, 0);
    PostMessageW(notify, WM_APP_NAMES_BULK_DONE, ok ? 1 : 0, static_cast<LPARAM>(total));
}

} // namespace

void download_all_names(HWND hwnd, bool quiet) {
    if (g_bulk_running) {
        SetWindowTextW(g_app->hwnd_status_label, L"Game names are already downloading.");
        return;
    }
    switch (ensure_content_map(hwnd)) {
    case CmapEnsure::Ready: break;
    case CmapEnsure::Started:
    case CmapEnsure::Building:
        g_bulk_after_cmap = true;
        SetWindowTextW(g_app->hwnd_status_label, L"Building the content map first, then downloading game names...");
        return;
    default:
        if (quiet)
            SetWindowTextW(g_app->hwnd_status_label, L"Game names: open the .dat and an index to build the content map.");
        else
            MessageBoxW(hwnd, L"Game names come from the content map, which is built from the .dat.\n\n"
                              L"Open the .dat and an index DB first.", L"castlemist", MB_ICONINFORMATION | MB_OK);
        return;
    }
    g_bulk_after_cmap = false;
    g_bulk_running = true;
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        g_wanted.clear();  // the bulk run covers these
    }
    std::thread(bulk_worker, hwnd).detach();
}

void on_main_cmap_done(HWND hwnd) {
    if (g_bulk_after_cmap) download_all_names(hwnd, true);
}

void on_names_progress(size_t done, size_t total) {
    wchar_t s[128];
    swprintf(s, 128, L"Downloading game names: %zu / %zu", done, total);
    SetWindowTextW(g_app->hwnd_status_label, s);
}

void on_names_bulk_done(bool ok, size_t total) {
    size_t named = 0;
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        for (const auto& [k, n] : g_names) named += !n.empty();
    }
    wchar_t s[512];
    if (ok)
        swprintf(s, 512, L"Game names ready: %zu named (%zu looked up this run). Search the list by name.", named, total);
    else
        swprintf(s, 512, L"Game names stopped early (%ls); %zu named so far. Run it again to resume.",
                 utf8_to_wide(g_bulk_error).c_str(), named);
    SetWindowTextW(g_app->hwnd_status_label, s);
    InvalidateRect(g_app->hwnd_list, nullptr, FALSE);
}

std::vector<uint32_t> base_ids_matching_name(const std::wstring& key, size_t* named_total) {
    std::vector<uint32_t> out;
    std::wstring ignored;
    name_for_files({}, false, ignored);  // loads the content map from its cache if needed
    if (!cmap::built()) return out;

    // The (type, id)s whose cached name matches, then the files any of them use.
    std::unordered_set<uint64_t> hits;
    {
        std::lock_guard<std::mutex> lock(g_names_mutex);
        load_names_locked();
        if (named_total) *named_total = g_names.size();
        for (const auto& [k, n] : g_names)
            if (name_matches(n, key)) hits.insert(k);
    }
    if (hits.empty()) return out;
    for (const MftBaseIdData& e : g_app->data_gw2.mft_base_id_data_list) {
        bool match = false;
        for (uint32_t fid : e.file_id) {
            for (const cmap::ContentRef& r : cmap::users_of(fid))
                if (hits.count(content_name_key(r.type, r.id))) { match = true; break; }
            if (match) break;
        }
        if (match) out.push_back(e.base_id);
    }
    return out;
}

const std::string* cached_content_name(uint32_t type, uint32_t id) {
    std::lock_guard<std::mutex> lock(g_names_mutex);
    load_names_locked();
    auto it = g_names.find(content_name_key(type, id));
    return it == g_names.end() ? nullptr : &it->second;  // entries are never erased, so the pointer stays valid
}

void request_content_names(HWND notify, const std::vector<cmap::ContentRef>& refs) {
    std::lock_guard<std::mutex> lock(g_names_mutex);
    load_names_locked();
    if (GetTickCount64() < g_offline_until || g_bulk_running) return;  // a bulk run covers everything
    g_names_notify = notify;
    for (const cmap::ContentRef& r : refs) {
        const uint64_t k = content_name_key(r.type, r.id);
        if (cmap::content_kind(r.type) && !g_names.count(k)) g_wanted.insert(k);
    }
    if (g_wanted.empty() || g_worker_running) return;
    g_worker_running = true;
    std::thread(names_worker).detach();
}

// ---- the main list's Name column

bool name_for_files(const std::vector<uint32_t>& file_ids, bool fetch, std::wstring& out) {
    // The map from its disk cache, tried once; a build in progress answers nothing yet.
    static bool tried_load = false;
    if (content_map_building()) return false;
    if (!cmap::built()) {
        if (tried_load) return false;
        tried_load = true;
        if (!cmap::load(cmap_cache_path())) return false;
    }

    // Users the API can name, in users_of() order (by type, then id).
    std::vector<cmap::ContentRef> named;
    for (uint32_t fid : file_ids)
        for (const cmap::ContentRef& r : cmap::users_of(fid))
            if (cmap::content_kind(r.type)) named.push_back(r);
    if (named.empty()) return false;

    std::vector<cmap::ContentRef> unknown;
    for (const cmap::ContentRef& r : named) {
        const std::string* n = cached_content_name(r.type, r.id);
        if (!n) {
            if (unknown.size() < 8) unknown.push_back(r);
            continue;
        }
        if (n->empty()) continue;
        out = utf8_to_wide(*n);
        if (named.size() > 1) out += L"  (+" + std::to_wstring(named.size() - 1) + L")";
        return true;
    }
    if (unknown.empty()) return false;  // looked up, and the API has a name for none of them
    if (fetch) request_content_names(g_app->hwnd_main, unknown);
    out = L"\x2026";
    return true;
}

// ---- the info panel section

std::wstring content_links_text(uint32_t mft_index) {
    if (content_map_building())
        return L"\r\n--- Game content ---\r\nThe content map is being built; this fills in when it is done.\r\n";
    if (!cmap::built() && !cmap::load(cmap_cache_path()))
        return L"\r\n--- Game content ---\r\nNo content map yet: Tools > Decode Chat Link... > Resolve assets builds it once.\r\n";

    // The file's fileIds (one baseId can carry several); a cntc object selected
    // in the content browser shows that object instead.
    std::vector<cmap::ContentRef> objects;
    size_t total = 0;
    const auto& objs = g_app->current_entry.content_objects;
    const int oi = g_app->content_obj_sel;
    if (g_app->current_entry.kind == PreviewKind::Content && oi >= 0 && oi < static_cast<int>(objs.size())) {
        objects.push_back({objs[oi].type, objs[oi].data_id});
        total = 1;
    } else {
        std::set<std::pair<uint32_t, uint32_t>> seen;
        for (uint32_t fid : get_by_file_id(g_app->data_gw2, mft_index + 1))
            for (const cmap::ContentRef& r : cmap::users_of(fid))
                if (seen.insert({r.type, r.id}).second) objects.push_back(r);
        total = objects.size();
    }

    constexpr size_t kShown = 40;
    std::vector<LinkedObject> users;
    std::vector<cmap::ContentRef> want;
    for (size_t i = 0; i < objects.size() && i < kShown; ++i) {
        LinkedObject u{objects[i], cmap::granted_by(objects[i].type, objects[i].id)};
        want.push_back(u.ref);
        for (size_t g = 0; g < u.granted_by.size() && g < 8; ++g) want.push_back(u.granted_by[g]);
        users.push_back(std::move(u));
    }
    request_content_names(g_app->hwnd_main, want);
    return format_content_links(users, total, cached_content_name);
}

void refresh_entry_info() {
    if (!g_app || !g_app->has_loaded_entry) return;
    // Keep the reader's place: re-setting the text scrolls the edit to the top.
    const LRESULT first = SendMessageW(g_app->hwnd_info, EM_GETFIRSTVISIBLELINE, 0, 0);
    castlemist::info::show_entry_info(g_app->hwnd_info, g_app->data_gw2, g_app->current_mft_index,
                                      g_app->current_entry, content_links_text(g_app->current_mft_index));
    SendMessageW(g_app->hwnd_info, EM_LINESCROLL, 0, first);
}

} // namespace castlemist::ui
