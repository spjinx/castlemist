/// @file
/// @brief The Data status window and the status-bar badge that opens it: is
///        every file an export depends on current for the open Gw2.dat?
///
/// The checks themselves are castlemist::db::judge_*; this file gathers their
/// inputs. A refresh snapshots what only the UI thread may read (the open dat,
/// the index path, the template), then a worker does the disk, SQL and network
/// work and posts the rows back. Refreshes are requested by posting
/// WM_APP_DATA_STATUS_REFRESH, so any thread can ask for one and a burst of
/// requests runs once.

#include "detail/app_state.h"

#include <atomic>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "castlemist/character/gw2_api.h"
#include "castlemist/character/http.h"
#include "castlemist/character/key_store.h"
#include "castlemist/character/look_store.h"
#include "castlemist/db/data_status.h"
#include "castlemist/db/index_db.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/ripper/vrchat.h"

namespace castlemist::ui {
namespace {

namespace fs = std::filesystem;
namespace ch = castlemist::character;
using castlemist::db::Freshness;
using castlemist::db::Verdict;

std::vector<DataRow> g_rows;  // UI thread only
std::atomic<bool> g_running{false};
std::atomic<bool> g_rerun{false};
std::atomic<bool> g_rerun_keys{false};

// data_stamps.json, shared by the worker and by whoever stamps a fresh build.
std::mutex g_stamps_mutex;

// The online key check is slow and talks to the network, so it runs only when
// asked; its answer is kept and shown on later refreshes until the next check.
std::mutex g_keys_mutex;
std::optional<Verdict> g_key_check;

// The template-vs-index check reads every chunk version in the index (a second
// or two), so its answer is reused until either file changes.
struct UnmappedCache {
    std::wstring index;
    std::string tpl;
    int64_t index_mtime = 0, tpl_mtime = 0;
    std::vector<castlemist::db::ChunkVersion> unmapped;
    bool valid = false;
};
UnmappedCache g_unmapped;  // worker only (one worker at a time)

HWND g_ds_wnd = nullptr;
HWND g_ds_list = nullptr;
HWND g_ds_fix = nullptr;
HWND g_ds_note = nullptr;

fs::path exe_dir() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return fs::path(exe).parent_path();
}

fs::path stamps_path() { return exe_dir() / L"data_stamps.json"; }

// The dat and template paths are kept in the ANSI code page (they were opened
// through to_ansi), not UTF-8.
fs::path from_ansi(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return fs::path(w);
}

int64_t mtime_unix(const fs::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = a.ftLastWriteTime.dwLowDateTime;
    t.HighPart = a.ftLastWriteTime.dwHighDateTime;
    constexpr uint64_t kEpochDiff = 116444736000000000ull;  // 1601 -> 1970, in 100 ns
    return t.QuadPart > kEpochDiff ? static_cast<int64_t>((t.QuadPart - kEpochDiff) / 10000000ull) : 0;
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::exists(p, ec);
}

/// What the worker needs from state only the UI thread may touch.
struct Inputs {
    bool dat_loaded = false;
    fs::path dat_path;
    castlemist::db::DatFingerprint fp;
    size_t entries = 0;
    fs::path index_path;
    fs::path cmap_path, names_path, strs_keys_path;
    bool cmap_building = false;
    bool check_keys = false;
};

Inputs snapshot(bool check_keys) {
    Inputs in;
    in.dat_loaded = g_app && g_app->dat_loaded;
    if (in.dat_loaded) {
        in.dat_path = from_ansi(g_app->data_gw2.file_info.file_path);
        in.fp = castlemist::db::fingerprint_of(g_app->data_gw2);
        in.entries = g_app->data_gw2.mft_data_list.size();
    }
    if (g_app && g_app->index_loaded) in.index_path = castlemist::db::path();
    in.cmap_path = cmap_cache_path();
    in.names_path = in.cmap_path.parent_path() / L"content_names.tsv";
    in.strs_keys_path = loaded_string_keys_path();
    in.cmap_building = content_map_building();
    in.check_keys = check_keys;
    return in;
}

DataRow row(const wchar_t* name, const wchar_t* role, const fs::path& path, Verdict v, DataFix fix = DataFix::None) {
    DataRow r;
    r.name = name;
    r.role = role;
    r.path = path.wstring();
    r.state = v.state;
    r.detail = utf8_to_wide(v.detail);
    r.fix = fix;
    return r;
}

Verdict check_keys_online(const std::vector<ch::ApiKey>& keys) {
    ch::WinHttpClient http;
    std::string problems;
    bool offline = false;
    for (const ch::ApiKey& k : keys) {
        try {
            ch::Gw2Api api(http, k.key);
            std::vector<std::string> lacks = ch::missing_scopes(api.token_info());
            if (!lacks.empty()) {
                std::string list;
                for (const std::string& s : lacks) list += (list.empty() ? "" : ", ") + s;
                problems += (problems.empty() ? "" : " ") + ("\"" + k.name + "\" lacks " + list + ".");
            }
        } catch (const ch::ApiError& e) {
            if (e.status() == 0) {
                offline = true;
                break;
            }
            problems += (problems.empty() ? "" : " ") +
                        ("\"" + k.name + "\" was rejected (HTTP " + std::to_string(e.status()) +
                         "): deleted or mistyped?");
        } catch (const std::exception&) {
            offline = true;
            break;
        }
    }
    if (offline) return {Freshness::Unknown, "Could not reach api.guildwars2.com to check the keys."};
    if (!problems.empty()) return {Freshness::Stale, problems};
    return {Freshness::Ok, std::to_string(keys.size()) + " key" + (keys.size() == 1 ? "" : "s") +
                               ", all accepted by the API with the scopes the ripper needs."};
}

std::vector<DataRow> evaluate(const Inputs& in) {
    std::vector<DataRow> rows;
    const int64_t now = static_cast<int64_t>(std::time(nullptr));

    // ---- Gw2.dat: still the archive on disk? Every refresh also records the
    // dat in data_stamps.json, which is how a patch gets a date for the
    // mtime-judged files below.
    std::optional<castlemist::db::DatFingerprint> disk;
    if (in.dat_loaded) disk = castlemist::db::read_dat_fingerprint(in.dat_path);
    castlemist::db::StampStore stamps;
    {
        std::lock_guard<std::mutex> lock(g_stamps_mutex);
        stamps.load(stamps_path());
        const castlemist::db::DatFingerprint seen = disk ? *disk : in.fp;
        const bool first = !stamps.last_dat();
        if ((stamps.observe_dat(seen, now) || (first && seen.valid()))) stamps.save(stamps_path());
    }
    if (!in.dat_loaded) {
        rows.push_back(row(L"Gw2.dat", L"Every model, texture and map export reads it", {},
                           {Freshness::Missing, "Not open."}, DataFix::OpenDat));
    } else if (disk && !(*disk == in.fp)) {
        rows.push_back(row(L"Gw2.dat", L"Every model, texture and map export reads it", in.dat_path,
                           {Freshness::Stale, "The game has patched it since castlemist opened it: exports would "
                                              "read the old file table. Reopen it."},
                           DataFix::OpenDat));
    } else {
        rows.push_back(row(L"Gw2.dat", L"Every model, texture and map export reads it", in.dat_path,
                           {Freshness::Ok, "Open; " + std::to_string(in.entries) + " entries."}));
    }

    // ---- the index
    if (in.index_path.empty()) {
        rows.push_back(row(L"Index (gw2_index.db)", L"Builds the content map; type filters", {},
                           {Freshness::Missing, "Not open. File > Build Index DB from .dat."}, DataFix::BuildIndex));
    } else {
        Verdict v = castlemist::db::judge_index(castlemist::db::index_meta(in.index_path), in.fp, in.entries,
                                                wide_to_utf8(in.dat_path.wstring()));
        rows.push_back(row(L"Index (gw2_index.db)", L"Builds the content map; type filters", in.index_path, v,
                           v.state == Freshness::Ok ? DataFix::None : DataFix::BuildIndex));
    }

    // ---- the struct template
    {
        auto tpl = castlemist::tpl::get_or_auto_load();
        const fs::path tpl_path = from_ansi(castlemist::tpl::source_path());
        if (!tpl) {
            rows.push_back(row(L"Struct template (gw2_packfile.json)", L"Model, material and map exports", {},
                               {Freshness::Missing, "Not found: models cannot be read. Generate it with "
                                                    "tools/structs/gen_gw2_json_ghidra.py."},
                               DataFix::LoadTemplate));
        } else {
            const fs::path exe = in.dat_path.empty() ? fs::path() : in.dat_path.parent_path() / L"Gw2-64.exe";
            std::optional<uint32_t> exe_ts = file_exists(exe) ? castlemist::db::pe_timestamp(exe) : std::nullopt;
            int unmapped = -1;
            std::string example;
            if (!in.index_path.empty()) {
                const int64_t im = mtime_unix(in.index_path), tm = mtime_unix(tpl_path);
                if (!g_unmapped.valid || g_unmapped.index != in.index_path.wstring() ||
                    g_unmapped.tpl != tpl_path.string() || g_unmapped.index_mtime != im ||
                    g_unmapped.tpl_mtime != tm) {
                    g_unmapped = {in.index_path.wstring(), tpl_path.string(), im, tm,
                                  castlemist::db::unmapped_versions(
                                      *tpl, castlemist::db::index_chunk_versions(in.index_path)),
                                  true};
                }
                unmapped = static_cast<int>(g_unmapped.unmapped.size());
                for (size_t i = 0; i < g_unmapped.unmapped.size() && i < 3; ++i)
                    example += (i ? ", " : "") + g_unmapped.unmapped[i].fourcc + " v" +
                               std::to_string(g_unmapped.unmapped[i].version);
            }
            auto src = tpl->find("source");
            Verdict v = castlemist::db::judge_template(src == tpl->end() ? nullptr : &*src, exe_ts,
                                                       mtime_unix(tpl_path), file_exists(exe) ? mtime_unix(exe) : 0,
                                                       unmapped);
            if (!example.empty()) v.detail += " (" + example + (unmapped > 3 ? ", ..." : "") + ")";
            rows.push_back(row(L"Struct template (gw2_packfile.json)", L"Model, material and map exports", tpl_path,
                               v, v.state == Freshness::Ok ? DataFix::None : DataFix::LoadTemplate));
        }
    }

    // ---- the content map ("Build map")
    if (in.cmap_building) {
        rows.push_back(row(L"Content map (content_map.bin)", L"Character Ripper, chat links, names", in.cmap_path,
                           {Freshness::Unknown, "Building..."}));
    } else if (!file_exists(in.cmap_path)) {
        rows.push_back(row(L"Content map (content_map.bin)", L"Character Ripper, chat links, names", in.cmap_path,
                           {Freshness::Missing, "Not built yet."}, DataFix::BuildContentMap));
    } else {
        Verdict v = castlemist::db::judge_stamped(stamps.get("content_map"), in.fp);
        if (v.state == Freshness::Stale)
            v.detail += " Skins, dyes and palettes may resolve to the wrong files.";
        rows.push_back(row(L"Content map (content_map.bin)", L"Character Ripper, chat links, names", in.cmap_path, v,
                           v.state == Freshness::Ok ? DataFix::None : DataFix::RebuildContentMap));
    }

    // ---- game names
    if (!file_exists(in.names_path)) {
        rows.push_back(row(L"Game names (content_names.tsv)", L"Name column, name search", in.names_path,
                           {Freshness::Optional, "None cached; names are fetched as you browse."},
                           DataFix::DownloadNames));
    } else if (auto st = stamps.get("content_names")) {
        Verdict v = castlemist::db::judge_stamped(st, in.fp);
        if (v.state == Freshness::Stale) v.detail = "Downloaded before the last game patch: new items have no name yet.";
        rows.push_back(row(L"Game names (content_names.tsv)", L"Name column, name search", in.names_path, v,
                           v.state == Freshness::Ok ? DataFix::None : DataFix::DownloadNames));
    } else {
        rows.push_back(row(L"Game names (content_names.tsv)", L"Name column, name search", in.names_path,
                           {Freshness::Unknown, "Filled as you browse. Download all game names to complete it."},
                           DataFix::DownloadNames));
    }

    // ---- API keys
    {
        const fs::path keys_file = ch::default_key_file();
        ch::KeyStore keys;
        std::string err;
        if (!keys.load(keys_file, &err)) {
            rows.push_back(row(L"API keys (api_keys.json)", L"Character Ripper", keys_file,
                               {Freshness::Stale, "Cannot be read: " + err}, DataFix::ManageKeys));
        } else if (keys.list().empty()) {
            rows.push_back(row(L"API keys (api_keys.json)", L"Character Ripper", keys_file,
                               {Freshness::Missing, "No key saved; the ripper needs one."}, DataFix::ManageKeys));
        } else {
            Verdict v;
            if (in.check_keys) {
                v = check_keys_online(keys.list());
                std::lock_guard<std::mutex> lock(g_keys_mutex);
                g_key_check = v;
            } else {
                std::lock_guard<std::mutex> lock(g_keys_mutex);
                if (g_key_check)
                    v = *g_key_check;
                else
                    v = {Freshness::Ok, std::to_string(keys.list().size()) + " saved (not checked online yet)."};
            }
            rows.push_back(row(L"API keys (api_keys.json)", L"Character Ripper", keys_file, v,
                               v.state == Freshness::Ok ? DataFix::CheckKeys : DataFix::ManageKeys));
        }
    }

    // ---- saved looks
    {
        const fs::path looks_file = ch::default_look_file();
        if (!file_exists(looks_file)) {
            rows.push_back(row(L"Saved looks (character_looks.json)", L"Character export face, hair, colours",
                               looks_file, {Freshness::Optional, "None saved; exports use the default look."}));
        } else {
            ch::LookStore looks;
            std::string err;
            if (!looks.load(looks_file, &err))
                rows.push_back(row(L"Saved looks (character_looks.json)", L"Character export face, hair, colours",
                                   looks_file, {Freshness::Stale, "Cannot be read: " + err}));
            else
                rows.push_back(row(L"Saved looks (character_looks.json)", L"Character export face, hair, colours",
                                   looks_file,
                                   {Freshness::Ok, std::to_string(looks.all().size()) + " character look" +
                                                       (looks.all().size() == 1 ? "" : "s") + " saved."}));
        }
    }

    // ---- string keys (captured from a running client)
    if (in.strs_keys_path.empty()) {
        rows.push_back(row(L"String keys (textkeys.csv)", L"Packed string tables", {},
                           {Freshness::Optional, "Not loaded; packed strings stay locked."}, DataFix::LoadStringKeys));
    } else {
        Verdict v = castlemist::db::judge_by_mtime(mtime_unix(in.strs_keys_path), stamps.dat_first_seen());
        const fs::path base = in.strs_keys_path.parent_path() / L"strs_textbase.csv";
        if (v.state == Freshness::Ok && file_exists(base)) {
            Verdict b = castlemist::db::judge_by_mtime(mtime_unix(base), stamps.dat_first_seen());
            if (b.state != Freshness::Ok) v = {b.state, "strs_textbase.csv: " + b.detail};
        }
        if (v.state == Freshness::Stale) v.detail += " Strings added by the patch have no key; capture them again.";
        rows.push_back(row(L"String keys (textkeys.csv)", L"Packed string tables", in.strs_keys_path, v,
                           v.state == Freshness::Ok ? DataFix::None : DataFix::LoadStringKeys));
    }

    // ---- Blender
    {
        const std::string blender = castlemist::ripper::find_blender();
        if (blender.empty())
            rows.push_back(row(L"Blender", L"VRChat .fbx export", {},
                               {Freshness::Optional, "Not found; VRChat exports stop at .glb."}));
        else
            rows.push_back(row(L"Blender", L"VRChat .fbx export", utf8_to_wide(blender), {Freshness::Ok, "Found."}));
    }
    return rows;
}

void start_worker(bool check_keys) {
    if (g_running.exchange(true)) {
        g_rerun = true;
        if (check_keys) g_rerun_keys = true;
        return;
    }
    Inputs in = snapshot(check_keys);
    HWND notify = g_app->hwnd_main;
    std::thread([in, notify]() {
        std::vector<DataRow>* rows = nullptr;
        try {
            rows = new std::vector<DataRow>(evaluate(in));
        } catch (const std::exception& e) {
            rows = new std::vector<DataRow>{row(L"Data status", L"", {}, {Freshness::Unknown, e.what()})};
        }
        if (!PostMessageW(notify, WM_APP_DATA_STATUS_DONE, 0, reinterpret_cast<LPARAM>(rows))) delete rows;
    }).detach();
}

// ---- the window

const wchar_t* fix_label(DataFix f) {
    switch (f) {
    case DataFix::OpenDat: return L"Open Gw2.dat...";
    case DataFix::BuildIndex: return L"Build index...";
    case DataFix::LoadTemplate: return L"Load struct JSON...";
    case DataFix::BuildContentMap: return L"Build content map";
    case DataFix::RebuildContentMap: return L"Rebuild content map";
    case DataFix::DownloadNames: return L"Download all names";
    case DataFix::ManageKeys: return L"Manage keys...";
    case DataFix::CheckKeys: return L"Check keys online";
    case DataFix::LoadStringKeys: return L"Load string keys...";
    case DataFix::None: break;
    }
    return L"Nothing to fix";
}

COLORREF state_colour(Freshness f) {
    switch (f) {
    case Freshness::Ok: return RGB(0x3F, 0xB9, 0x50);
    case Freshness::Stale: return RGB(0xE3, 0x9B, 0x22);
    case Freshness::Missing: return RGB(0xE5, 0x53, 0x4B);
    case Freshness::Unknown:
    case Freshness::Optional: break;
    }
    return kColSubtle;
}

int selected_row() {
    if (!g_ds_list) return -1;
    return static_cast<int>(SendMessageW(g_ds_list, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

void sync_fix_button() {
    if (!g_ds_fix) return;
    int i = selected_row();
    DataFix f = (i >= 0 && i < static_cast<int>(g_rows.size())) ? g_rows[static_cast<size_t>(i)].fix : DataFix::None;
    SetWindowTextW(g_ds_fix, fix_label(f));
    EnableWindow(g_ds_fix, f != DataFix::None);
}

void set_cell(int item, int sub, const std::wstring& text) {
    LVITEMW it{};
    it.iSubItem = sub;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(g_ds_list, LVM_SETITEMTEXTW, item, reinterpret_cast<LPARAM>(&it));
}

void fill_list() {
    if (!g_ds_list) return;
    const int keep = selected_row();
    SendMessageW(g_ds_list, LVM_DELETEALLITEMS, 0, 0);
    for (size_t i = 0; i < g_rows.size(); ++i) {
        const DataRow& r = g_rows[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = static_cast<int>(i);
        it.pszText = const_cast<wchar_t*>(r.name.c_str());
        SendMessageW(g_ds_list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        set_cell(static_cast<int>(i), 1, utf8_to_wide(castlemist::db::to_string(r.state)));
        set_cell(static_cast<int>(i), 2, r.detail);
        set_cell(static_cast<int>(i), 3, r.role);
        set_cell(static_cast<int>(i), 4, r.path);
    }
    if (keep >= 0 && keep < static_cast<int>(g_rows.size())) {
        LVITEMW st{};
        st.stateMask = st.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_ds_list, LVM_SETITEMSTATE, keep, reinterpret_cast<LPARAM>(&st));
    }
    sync_fix_button();
}

void run_fix(HWND owner) {
    int i = selected_row();
    if (i < 0 || i >= static_cast<int>(g_rows.size())) return;
    HWND main = g_app->hwnd_main;
    switch (g_rows[static_cast<size_t>(i)].fix) {
    case DataFix::OpenDat: SendMessageW(main, WM_COMMAND, ID_FILE_OPEN, 0); break;
    case DataFix::BuildIndex: SendMessageW(main, WM_COMMAND, ID_FILE_BUILD_INDEX, 0); break;
    case DataFix::LoadTemplate: SendMessageW(main, WM_COMMAND, ID_FILE_LOAD_TEMPLATE, 0); break;
    case DataFix::LoadStringKeys: SendMessageW(main, WM_COMMAND, ID_FILE_LOAD_KEYS, 0); break;
    case DataFix::DownloadNames: SendMessageW(main, WM_COMMAND, ID_TOOLS_DOWNLOAD_NAMES, 0); break;
    case DataFix::ManageKeys: open_character_keys_dialog(owner, [] { request_data_status_refresh(); }); break;
    case DataFix::CheckKeys: request_data_status_refresh(true); break;
    case DataFix::BuildContentMap:
    case DataFix::RebuildContentMap: {
        const bool rebuild = g_rows[static_cast<size_t>(i)].fix == DataFix::RebuildContentMap;
        CmapEnsure r = rebuild ? rebuild_content_map(main) : ensure_content_map(main);
        const wchar_t* msg = nullptr;
        switch (r) {
        case CmapEnsure::NeedDat: msg = L"Open Gw2.dat first: the content map is built from it."; break;
        case CmapEnsure::NeedIndex: msg = L"Open or build an index first: it lists the content packs."; break;
        case CmapEnsure::InUse: msg = L"An export is reading the content map. Try again when it finishes."; break;
        default: break;
        }
        if (msg) MessageBoxW(owner, msg, L"castlemist", MB_ICONINFORMATION);
        request_data_status_refresh();
        break;
    }
    case DataFix::None: break;
    }
}

LRESULT on_custom_draw(LPARAM lparam) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const size_t i = static_cast<size_t>(cd->nmcd.dwItemSpec);
        cd->clrText = (cd->iSubItem == 1 && i < g_rows.size()) ? state_colour(g_rows[i].state) : kColText;
        cd->clrTextBk = kColPanel;
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

LRESULT CALLBACK DataStatusWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_DS_FIX: run_fix(hwnd); return 0;
        case ID_DS_KEYS:
            SetWindowTextW(g_ds_note, L"Checking the API keys online...");
            request_data_status_refresh(true);
            return 0;
        case ID_DS_REFRESH:
            SetWindowTextW(g_ds_note, L"Checking...");
            request_data_status_refresh();
            return 0;
        case ID_DS_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* h = reinterpret_cast<NMHDR*>(lparam);
        if (h->hwndFrom != g_ds_list) break;
        if (h->code == NM_CUSTOMDRAW) return on_custom_draw(lparam);
        if (h->code == LVN_ITEMCHANGED) sync_fix_button();
        if (h->code == NM_DBLCLK) run_fix(hwnd);
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, kColText);
        SetBkColor(dc, kColBg);
        return reinterpret_cast<LRESULT>(theme_brush(kColBg));
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_ds_wnd = g_ds_list = g_ds_fix = g_ds_note = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void request_data_status_refresh(bool check_keys) {
    if (g_app && g_app->hwnd_main) PostMessageW(g_app->hwnd_main, WM_APP_DATA_STATUS_REFRESH, check_keys ? 1 : 0, 0);
}

void on_data_status_refresh(bool check_keys) {
    if (g_app) start_worker(check_keys);
}

void on_data_status_done(LPARAM lparam) {
    std::unique_ptr<std::vector<DataRow>> rows(reinterpret_cast<std::vector<DataRow>*>(lparam));
    g_rows = std::move(*rows);
    g_running = false;
    update_data_badge();
    if (g_ds_wnd) {
        fill_list();
        SetWindowTextW(g_ds_note, data_status_summary().c_str());
    }
    if (g_rerun.exchange(false)) start_worker(g_rerun_keys.exchange(false));
}

DataBadge data_badge(const std::vector<DataRow>& rows) {
    DataBadge b;
    for (const DataRow& r : rows) {
        if (r.state == Freshness::Stale) ++b.stale;
        if (r.state == Freshness::Missing) ++b.missing;
        if (r.state == Freshness::Unknown) ++b.unknown;
    }
    wchar_t s[96];
    if (rows.empty())
        swprintf(s, 96, L"Data: checking...");
    else if (b.stale + b.missing == 0)
        swprintf(s, 96, b.unknown ? L"Data: up to date (%d unchecked)" : L"Data: up to date", b.unknown);
    else if (b.missing == 0)
        swprintf(s, 96, L"⚠ Data: %d stale", b.stale);
    else
        swprintf(s, 96, L"⚠ Data: %d need%ls attention", b.stale + b.missing, b.stale + b.missing == 1 ? L"s" : L"");
    b.text = s;
    return b;
}

std::wstring data_status_summary() {
    DataBadge b = data_badge(g_rows);
    if (b.stale + b.missing == 0)
        return L"Everything an export reads is current for the open Gw2.dat. Select a row for its fix.";
    return std::to_wstring(b.stale) + L" stale, " + std::to_wstring(b.missing) +
           L" missing. Select a row and click the fix button (or double-click it).";
}

std::wstring data_status_export_warning() {
    std::wstring names;
    for (const DataRow& r : g_rows) {
        if (r.state != Freshness::Stale) continue;
        // "Content map (content_map.bin)" -> "Content map"
        const std::wstring shortname = r.name.substr(0, r.name.find(L" ("));
        names += (names.empty() ? L"" : L", ") + shortname;
    }
    if (names.empty()) return {};
    return L"Heads up: stale for this game build: " + names + L". Tools > Data status before exporting.";
}

void update_data_badge() {
    if (!g_app || !g_app->hwnd_data_badge) return;
    DataBadge b = data_badge(g_rows);
    g_app->data_badge_alert = b.stale + b.missing > 0;
    SetWindowTextW(g_app->hwnd_data_badge, b.text.c_str());
    InvalidateRect(g_app->hwnd_data_badge, nullptr, TRUE);
}

COLORREF data_badge_colour() {
    if (!g_app || !g_app->data_badge_alert) return kColSubtle;
    return RGB(0xE3, 0x9B, 0x22);
}

void stamp_data_file(const char* key, const castlemist::db::DatFingerprint& fp) {
    if (!fp.valid()) return;
    std::lock_guard<std::mutex> lock(g_stamps_mutex);
    castlemist::db::StampStore s;
    s.load(stamps_path());
    s.set(key, {fp, static_cast<int64_t>(std::time(nullptr))});
    s.save(stamps_path());
}

void open_data_status_dialog(HWND owner) {
    if (g_ds_wnd) {
        SetForegroundWindow(g_ds_wnd);
        request_data_status_refresh();
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DataStatusWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = theme_brush(kColBg);
        wc.lpszClassName = L"Gw2DataStatusWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 1200, H = 420;
    g_ds_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2DataStatusWnd", L"Data status",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, g_hinstance, nullptr);
    if (!g_ds_wnd) return;

    HFONT font = g_ui_font ? g_ui_font : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_ds_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    mk(L"STATIC",
       L"What each export reads, checked against the open Gw2.dat. \"Stale\" means it was made for an older game "
       L"build; rebuild it before exporting.",
       SS_LEFT, 10, 10, W - 40, 20, 0);
    g_ds_list = mk(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 10, 36, W - 36,
                   H - 140, ID_DS_LIST);
    SendMessageW(g_ds_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    SendMessageW(g_ds_list, LVM_SETBKCOLOR, 0, kColPanel);
    SendMessageW(g_ds_list, LVM_SETTEXTBKCOLOR, 0, kColPanel);
    SendMessageW(g_ds_list, LVM_SETTEXTCOLOR, 0, kColText);
    const struct { const wchar_t* title; int width; } cols[] = {
        {L"File", 235}, {L"Status", 80}, {L"Details", 470}, {L"Used for", 220}, {L"Location", 300}};
    for (int i = 0; i < 5; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(cols[i].title);
        c.cx = cols[i].width;
        c.iSubItem = i;
        SendMessageW(g_ds_list, LVM_INSERTCOLUMNW, i, reinterpret_cast<LPARAM>(&c));
    }
    g_ds_note = mk(L"STATIC", L"Checking...", SS_LEFT | SS_ENDELLIPSIS, 10, H - 98, W - 40, 18, 0);
    g_ds_fix = mk(L"BUTTON", L"Nothing to fix", BS_PUSHBUTTON, 10, H - 72, 160, 28, ID_DS_FIX);
    mk(L"BUTTON", L"Check API keys online", BS_PUSHBUTTON, 176, H - 72, 160, 28, ID_DS_KEYS);
    mk(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 342, H - 72, 90, 28, ID_DS_REFRESH);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 106, H - 72, 80, 28, ID_DS_CLOSE);
    refresh_theme(g_ds_wnd);

    fill_list();
    if (!g_rows.empty()) SetWindowTextW(g_ds_note, data_status_summary().c_str());
    ShowWindow(g_ds_wnd, SW_SHOW);
    request_data_status_refresh();
}

} // namespace castlemist::ui
