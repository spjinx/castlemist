/// @file
/// @brief Tools > Character Ripper: pick a saved GW2 API key, fetch the
///        account's characters, and show one character's equipped gear resolved
///        to dat assets (castlemist::character). All network work runs on a
///        worker thread; results come back as WM_APP_CHAR_* messages whose
///        lParam is a heap result this dialog takes ownership of.

#include "detail/app_state.h"

#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "castlemist/character/fetch.h"
#include "castlemist/character/key_store.h"
#include "castlemist/character/manifest_json.h"
#include "castlemist/format/content_map.h"

namespace castlemist::ui {

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

namespace {

namespace ch = castlemist::character;

HWND g_ch_wnd = nullptr;
HWND g_ch_key_combo = nullptr;
HWND g_ch_char_combo = nullptr;
HWND g_ch_tab_combo = nullptr;
HWND g_ch_table = nullptr;
HWND g_ch_status = nullptr;
HWND g_ch_fetch_btn = nullptr;
HWND g_ch_build_btn = nullptr;

ch::KeyStore g_ch_keys;
std::vector<std::string> g_ch_names;         // character names, combo order
std::optional<ch::FetchResult> g_ch_current;  // what the table shows
std::string g_ch_current_key;                 // key the current character list came from
std::atomic<unsigned> g_ch_request{0};        // newest request id; stale results are dropped

struct NamesDone {
    unsigned request = 0;
    std::vector<std::string> names;
    std::string error;
};

struct FetchDone {
    unsigned request = 0;
    std::optional<ch::FetchResult> result;
    std::string error;
    bool had_map = false;
};

// For fetches while the content map is missing or still building: the shared
// cmap must not be read from this worker while the build thread writes it.
struct NoMapLookup final : ch::AssetLookup {
    bool built() const override { return false; }
    std::vector<uint32_t> skin_assets(uint32_t) const override { return {}; }
    std::optional<uint32_t> item_skin(uint32_t) const override { return std::nullopt; }
};

void set_status(const std::wstring& s) { SetWindowTextW(g_ch_status, s.c_str()); }

void set_busy(bool busy) {
    EnableWindow(g_ch_fetch_btn, !busy);
    EnableWindow(g_ch_char_combo, !busy);
    EnableWindow(g_ch_tab_combo, !busy);
    EnableWindow(g_ch_build_btn, !busy);
}

// Posts `result` to the dialog, or frees it if the dialog is gone.
template <typename T>
void post_result(UINT msg, T* result) {
    HWND w = g_ch_wnd;
    if (!w || !IsWindow(w) || !PostMessageW(w, msg, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

void refill_keys() {
    std::string err;
    std::wstring keep;
    if (LRESULT sel = SendMessageW(g_ch_key_combo, CB_GETCURSEL, 0, 0); sel != CB_ERR) {
        wchar_t buf[256] = L"";
        SendMessageW(g_ch_key_combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(buf));
        keep = buf;
    }
    SendMessageW(g_ch_key_combo, CB_RESETCONTENT, 0, 0);
    if (!g_ch_keys.load(ch::default_key_file(), &err)) {
        set_status(utf8_to_wide(err));
        return;
    }
    int select = 0, i = 0;
    for (const ch::ApiKey& k : g_ch_keys.list()) {
        std::wstring name = utf8_to_wide(k.name);
        SendMessageW(g_ch_key_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        if (name == keep) select = i;
        ++i;
    }
    if (!g_ch_keys.list().empty()) SendMessageW(g_ch_key_combo, CB_SETCURSEL, select, 0);
    else set_status(L"No API keys saved yet - click Manage keys... to add one.");
}

const ch::ApiKey* selected_key() {
    LRESULT sel = SendMessageW(g_ch_key_combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || static_cast<size_t>(sel) >= g_ch_keys.list().size()) return nullptr;
    return &g_ch_keys.list()[static_cast<size_t>(sel)];
}

// ---- table ---------------------------------------------------------------------

void add_column(int index, const wchar_t* title, int width) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    SendMessageW(g_ch_table, LVM_INSERTCOLUMNW, index, reinterpret_cast<LPARAM>(&col));
}

void set_cell(int row, int col, const std::wstring& text) {
    LVITEMW it{};
    it.iSubItem = col;
    it.pszText = const_cast<wchar_t*>(text.c_str());
    SendMessageW(g_ch_table, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&it));
}

std::wstring dyes_text(const ch::ManifestPiece& p) {
    std::wstring out;
    for (const ch::ManifestDye& d : p.dyes) {
        if (!out.empty()) out += L", ";
        if (!d.known) {
            out += d.color_name.empty() ? L"?" : utf8_to_wide(d.color_name) + L" ?";
            continue;
        }
        wchar_t hex[16];
        swprintf(hex, 16, L" #%02X%02X%02X", d.rgb[0], d.rgb[1], d.rgb[2]);
        out += utf8_to_wide(d.color_name) + hex;
    }
    return out;
}

void fill_table() {
    SendMessageW(g_ch_table, LVM_DELETEALLITEMS, 0, 0);
    if (!g_ch_current) return;
    int row = 0;
    for (const ch::ManifestPiece& p : g_ch_current->manifest.pieces) {
        std::wstring slot = utf8_to_wide(p.slot);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = row;
        it.pszText = slot.data();
        SendMessageW(g_ch_table, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        set_cell(row, 1, utf8_to_wide(p.item_name.empty() ? "#" + std::to_string(p.item_id) : p.item_name));
        set_cell(row, 2, p.skin_id ? utf8_to_wide(p.skin_name.empty() ? "#" + std::to_string(p.skin_id) : p.skin_name)
                                   : std::wstring());
        set_cell(row, 3, dyes_text(p));
        set_cell(row, 4, utf8_to_wide(ch::to_string(p.status)));
        ++row;
    }
}

void fill_tabs() {
    SendMessageW(g_ch_tab_combo, CB_RESETCONTENT, 0, 0);
    if (!g_ch_current) return;
    int i = 0, select = 0;
    for (const ch::TabSummary& t : g_ch_current->tabs) {
        std::wstring label = L"Tab " + std::to_wstring(t.tab);
        if (!t.name.empty()) label += L": " + utf8_to_wide(t.name);
        if (t.is_active) label += L" (active)";
        SendMessageW(g_ch_tab_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (t.tab == g_ch_current->manifest.tab_id) select = i;
        ++i;
    }
    SendMessageW(g_ch_tab_combo, CB_SETCURSEL, select, 0);
}

// ---- workers -------------------------------------------------------------------

void start_names_fetch() {
    const ch::ApiKey* k = selected_key();
    if (!k) {
        set_status(L"Pick an API key first (Manage keys... to add one).");
        return;
    }
    std::string key = k->key;
    unsigned req = ++g_ch_request;
    set_busy(true);
    set_status(L"Checking the key and fetching characters...");
    std::thread([key, req]() {
        auto* r = new NamesDone{req, {}, {}};
        try {
            ch::WinHttpClient http;
            ch::Gw2Api api(http, key);
            std::vector<std::string> missing = ch::missing_scopes(api.token_info());
            if (!missing.empty()) {
                std::string list;
                for (const std::string& m : missing) list += (list.empty() ? "" : ", ") + m;
                r->error = "Key is missing scopes: " + list + ". Add them at account.arena.net/applications.";
            } else {
                r->names = api.character_names();
            }
        } catch (const std::exception& e) {
            r->error = e.what();
        }
        post_result(WM_APP_CHAR_NAMES_DONE, r);
    }).detach();
    g_ch_current_key = key;
}

std::string selected_character() {
    LRESULT sel = SendMessageW(g_ch_char_combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || static_cast<size_t>(sel) >= g_ch_names.size()) return {};
    return g_ch_names[static_cast<size_t>(sel)];
}

void start_character_fetch(std::optional<int> tab) {
    std::string name = selected_character();
    if (name.empty() || g_ch_current_key.empty()) return;
    CmapEnsure map = ensure_content_map(g_ch_wnd);
    bool had_map = map == CmapEnsure::Ready;
    std::string key = g_ch_current_key;
    unsigned req = ++g_ch_request;
    set_busy(true);
    set_status(L"Fetching " + utf8_to_wide(name) + L"...");
    // Hold the shared map for the whole fetch so a rebuild can't clear it under us.
    if (had_map) acquire_content_map_reader();
    std::thread([key, name, tab, req, had_map]() {
        auto* r = new FetchDone{req, std::nullopt, {}, had_map};
        try {
            ch::WinHttpClient http;
            ch::Gw2Api api(http, key);
            ch::CmapAssetLookup cmap_assets;
            NoMapLookup no_map;
            const ch::AssetLookup& assets = had_map ? static_cast<const ch::AssetLookup&>(cmap_assets) : no_map;
            r->result = ch::fetch_character(api, name, tab, assets);
        } catch (const std::exception& e) {
            r->error = e.what();
        }
        if (had_map) release_content_map_reader();
        post_result(WM_APP_CHAR_FETCH_DONE, r);
    }).detach();
}

void on_names_done(std::unique_ptr<NamesDone> r) {
    if (r->request != g_ch_request) return;
    set_busy(false);
    if (!r->error.empty()) {
        set_status(utf8_to_wide(r->error));
        return;
    }
    g_ch_names = std::move(r->names);
    g_ch_current.reset();
    fill_table();
    fill_tabs();
    SendMessageW(g_ch_char_combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& n : g_ch_names)
        SendMessageW(g_ch_char_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(n).c_str()));
    set_status(std::to_wstring(g_ch_names.size()) + L" characters - pick one.");
}

void on_fetch_done(std::unique_ptr<FetchDone> r) {
    if (r->request != g_ch_request) return;
    set_busy(false);
    if (!r->error.empty()) {
        set_status(utf8_to_wide(r->error));
        return;
    }
    g_ch_current = std::move(r->result);
    fill_tabs();
    fill_table();
    size_t ok = 0;
    for (const ch::ManifestPiece& p : g_ch_current->manifest.pieces) ok += p.status == ch::PieceStatus::Ok;
    const ch::CharacterManifest& m = g_ch_current->manifest;
    std::wstring s = utf8_to_wide(m.name) + L" - " + utf8_to_wide(m.race) + L" " + utf8_to_wide(m.gender) + L" " +
                     utf8_to_wide(m.profession) + L": " + std::to_wstring(ok) + L" of " +
                     std::to_wstring(m.pieces.size()) + L" pieces resolved to models.";
    if (!r->had_map) s += L" Content map not built - models unavailable (Build map).";
    set_status(s);
}

// ---- buttons -------------------------------------------------------------------

void open_selected_model() {
    LRESULT row = SendMessageW(g_ch_table, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED);
    if (!g_ch_current || row < 0 || static_cast<size_t>(row) >= g_ch_current->manifest.pieces.size()) {
        set_status(L"Select a row first.");
        return;
    }
    const ch::ManifestPiece& p = g_ch_current->manifest.pieces[static_cast<size_t>(row)];
    if (p.status != ch::PieceStatus::Ok || p.file_ids.empty()) {
        set_status(L"That piece has no model (" + utf8_to_wide(ch::to_string(p.status)) + L").");
        return;
    }
    navigate_to_file_id(p.file_ids[0]);
}

void save_manifest() {
    if (!g_ch_current) {
        set_status(L"Fetch a character first.");
        return;
    }
    std::wstring name = utf8_to_wide(g_ch_current->manifest.name);
    for (wchar_t& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    wchar_t path[MAX_PATH] = L"";
    swprintf(path, MAX_PATH, L"%ls.json", name.c_str());
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_ch_wnd;
    ofn.lpstrFilter = L"Character manifest (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Save character manifest";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    out << ch::manifest_to_json(g_ch_current->manifest).dump(2) << '\n';
    set_status(out ? L"Saved " + std::wstring(path) : L"Could not write " + std::wstring(path));
}

void build_map() {
    CmapEnsure r = castlemist::cmap::built() ? rebuild_content_map(g_ch_wnd) : ensure_content_map(g_ch_wnd);
    switch (r) {
    case CmapEnsure::Ready:
        set_status(L"Content map ready.");
        if (g_ch_current) start_character_fetch(g_ch_current->manifest.tab_id);
        break;
    case CmapEnsure::Building: set_status(L"Still building the content map..."); break;
    case CmapEnsure::Started: set_status(L"Building the content map from the cntc packs... (one-time)"); break;
    case CmapEnsure::InUse: set_status(L"Wait for the current fetch to finish, then Build map."); break;
    case CmapEnsure::NeedDat: set_status(L"Open Gw2.dat first (File > Open), then Build map. Current map kept."); break;
    case CmapEnsure::NeedIndex:
        set_status(L"Open the Gw2.dat index DB first (File > Open Index DB), then Build map. Current map kept.");
        break;
    }
}

std::optional<int> selected_tab() {
    LRESULT sel = SendMessageW(g_ch_tab_combo, CB_GETCURSEL, 0, 0);
    if (!g_ch_current || sel == CB_ERR || static_cast<size_t>(sel) >= g_ch_current->tabs.size()) return std::nullopt;
    return g_ch_current->tabs[static_cast<size_t>(sel)].tab;
}

LRESULT CALLBACK CharacterWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_CH_MANAGE: open_character_keys_dialog(hwnd, [] { refill_keys(); }); return 0;
        case ID_CH_FETCH: start_names_fetch(); return 0;
        case ID_CH_CHAR_LIST:
            if (HIWORD(wparam) == CBN_SELCHANGE) start_character_fetch(std::nullopt);
            return 0;
        case ID_CH_TAB_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) start_character_fetch(selected_tab());
            return 0;
        case ID_CH_OPEN_MODEL: open_selected_model(); return 0;
        case ID_CH_SAVE: save_manifest(); return 0;
        case ID_CH_BUILD_MAP: build_map(); return 0;
        case ID_CH_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lparam);
        if (nm->idFrom == ID_CH_TABLE && nm->code == NM_DBLCLK) open_selected_model();
        break;
    }
    case WM_APP_CHAR_NAMES_DONE: on_names_done(std::unique_ptr<NamesDone>(reinterpret_cast<NamesDone*>(lparam))); return 0;
    case WM_APP_CHAR_FETCH_DONE: on_fetch_done(std::unique_ptr<FetchDone>(reinterpret_cast<FetchDone*>(lparam))); return 0;
    case WM_APP_CMAP_DONE:
        set_status(L"Content map built.");
        if (g_ch_current) start_character_fetch(g_ch_current->manifest.tab_id);
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        ++g_ch_request;  // drop any in-flight result
        g_ch_wnd = g_ch_key_combo = g_ch_char_combo = g_ch_tab_combo = g_ch_table = g_ch_status = g_ch_fetch_btn =
            g_ch_build_btn = nullptr;
        g_ch_names.clear();
        g_ch_current.reset();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void open_character_dialog(HWND owner) {
    if (g_ch_wnd) {
        SetForegroundWindow(g_ch_wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CharacterWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2CharacterWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 820, H = 560;
    g_ch_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2CharacterWnd", L"Character Ripper",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, g_hinstance, nullptr);
    if (!g_ch_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_ch_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    mk(L"STATIC", L"API key:", SS_LEFT, 10, 14, 60, 18, 0);
    g_ch_key_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 75, 10, 200, 200, ID_CH_KEY_COMBO);
    mk(L"BUTTON", L"Manage keys...", BS_PUSHBUTTON, 285, 9, 110, 26, ID_CH_MANAGE);
    g_ch_fetch_btn = mk(L"BUTTON", L"Fetch characters", BS_DEFPUSHBUTTON, 405, 9, 130, 26, ID_CH_FETCH);

    mk(L"STATIC", L"Character:", SS_LEFT, 10, 48, 60, 18, 0);
    g_ch_char_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 75, 44, 260, 300, ID_CH_CHAR_LIST);
    mk(L"STATIC", L"Equipment tab:", SS_LEFT, 350, 48, 85, 18, 0);
    g_ch_tab_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 440, 44, 200, 200, ID_CH_TAB_COMBO);

    g_ch_table = mk(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 10, 78, W - 36,
                    H - 200, ID_CH_TABLE);
    SendMessageW(g_ch_table, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    add_column(0, L"Slot", 105);
    add_column(1, L"Item", 210);
    add_column(2, L"Skin", 170);
    add_column(3, L"Dyes", 210);
    add_column(4, L"Status", 85);

    g_ch_status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 10, H - 112, W - 36, 18, 0);
    mk(L"BUTTON", L"Open model", BS_PUSHBUTTON, 10, H - 84, 100, 28, ID_CH_OPEN_MODEL);
    mk(L"BUTTON", L"Save manifest...", BS_PUSHBUTTON, 115, H - 84, 115, 28, ID_CH_SAVE);
    g_ch_build_btn = mk(L"BUTTON", L"Build map", BS_PUSHBUTTON, 235, H - 84, 90, 28, ID_CH_BUILD_MAP);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 106, H - 84, 80, 28, ID_CH_CLOSE);

    refill_keys();
    if (!g_ch_keys.list().empty())
        set_status(L"Pick a key and click Fetch characters. (Goes online: api.guildwars2.com only.)");
    ShowWindow(g_ch_wnd, SW_SHOW);
}

} // namespace castlemist::ui
