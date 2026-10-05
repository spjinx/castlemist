/// @file
/// @brief Tools > Character Ripper: pick a saved GW2 API key, fetch the
///        account's characters, and show one character's equipped gear resolved
///        to dat assets (castlemist::character). All network work runs on a
///        worker thread; results come back as WM_APP_CHAR_* messages whose
///        lParam is a heap result this dialog takes ownership of.

#include "detail/app_state.h"
#include "detail/character_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "castlemist/character/fetch.h"
#include "castlemist/character/key_store.h"
#include "castlemist/character/look_store.h"
#include "castlemist/character/manifest_json.h"
#include "castlemist/format/content_map.h"
#include "castlemist/ripper/assemble.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/look.h"

#include <shlobj.h>
#include <windowsx.h>

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
HWND g_ch_export_btn = nullptr;
HWND g_ch_assemble_btn = nullptr;
HWND g_ch_combine_chk = nullptr;
HWND g_ch_face_combo = nullptr;
HWND g_ch_hair_combo = nullptr;
HWND g_ch_palette_wnd = nullptr;  // the open colour picker popup, if any

// The shown character's look (Look row), and every saved look.
ch::LookStore g_ch_looks;
ch::CharacterLook g_ch_look;
int g_ch_palette_target = 0;  // which colour the picker sets: 0 skin, 1 hair, 2 hair 2

void load_look_for_current();

ch::KeyStore g_ch_keys;
RipperState g_ch;                             // names / shown character / their key
std::atomic<unsigned> g_ch_request{0};        // newest request id; stale results are dropped

struct NamesDone {
    unsigned request = 0;
    std::string key;
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
    uint64_t skin_token(uint32_t) const override { return 0; }
};

void set_status(const std::wstring& s) { SetWindowTextW(g_ch_status, s.c_str()); }

void set_busy(bool busy) {
    EnableWindow(g_ch_fetch_btn, !busy);
    EnableWindow(g_ch_char_combo, !busy);
    EnableWindow(g_ch_tab_combo, !busy);
    EnableWindow(g_ch_build_btn, !busy);
    EnableWindow(g_ch_export_btn, !busy);
    EnableWindow(g_ch_assemble_btn, !busy);
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
    if (!g_ch.current) return;
    int row = 0;
    for (const ch::ManifestPiece& p : g_ch.current->manifest.pieces) {
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
    if (!g_ch.current) return;
    int i = 0, select = 0;
    for (const ch::TabSummary& t : g_ch.current->tabs) {
        std::wstring label = L"Tab " + std::to_wstring(t.tab);
        if (!t.name.empty()) label += L": " + utf8_to_wide(t.name);
        if (t.is_active) label += L" (active)";
        SendMessageW(g_ch_tab_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (t.tab == g_ch.current->manifest.tab_id) select = i;
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
        auto* r = new NamesDone{req, key, {}, {}};
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
}

std::string selected_character() {
    LRESULT sel = SendMessageW(g_ch_char_combo, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR || static_cast<size_t>(sel) >= g_ch.names.size()) return {};
    return g_ch.names[static_cast<size_t>(sel)];
}

void start_character_fetch(std::optional<int> tab) {
    std::string name = selected_character();
    if (name.empty() || g_ch.current_key.empty()) return;
    CmapEnsure map = ensure_content_map(g_ch_wnd);
    bool had_map = map == CmapEnsure::Ready;
    std::string key = g_ch.current_key;
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
    g_ch.names_done(r->key, std::move(r->names), r->error);
    fill_table();
    fill_tabs();
    SendMessageW(g_ch_char_combo, CB_RESETCONTENT, 0, 0);
    for (const std::string& n : g_ch.names)
        SendMessageW(g_ch_char_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(n).c_str()));
    set_status(r->error.empty() ? std::to_wstring(g_ch.names.size()) + L" characters - pick one."
                                : utf8_to_wide(r->error));
}

void on_fetch_done(std::unique_ptr<FetchDone> r) {
    if (r->request != g_ch_request) return;
    set_busy(false);
    g_ch.fetch_done(std::move(r->result), r->error);
    fill_tabs();
    fill_table();
    if (!r->error.empty()) {
        set_status(utf8_to_wide(r->error));
        return;
    }
    size_t ok = 0;
    for (const ch::ManifestPiece& p : g_ch.current->manifest.pieces) ok += p.status == ch::PieceStatus::Ok;
    const ch::CharacterManifest& m = g_ch.current->manifest;
    std::wstring s = utf8_to_wide(m.name) + L" - " + utf8_to_wide(m.race) + L" " + utf8_to_wide(m.gender) + L" " +
                     utf8_to_wide(m.profession) + L": " + std::to_wstring(ok) + L" of " +
                     std::to_wstring(m.pieces.size()) + L" pieces resolved to models.";
    if (!r->had_map) s += L" Content map not built - models unavailable (Build map).";
    set_status(s);
    load_look_for_current();
}

// ---- look ------------------------------------------------------------------------
// The Look row: face and hair style (indices into the race's Composite lists)
// and the skin / hair colours (ids in the race's character-creator palettes,
// read from the content map -- never while it is being built).

uint32_t& look_color(int which) {
    return which == 0 ? g_ch_look.skin_color : which == 1 ? g_ch_look.hair_color : g_ch_look.hair_color2;
}

const castlemist::cmap::Palette* look_palette(int which) {
    if (!g_ch.current || content_map_building() || !castlemist::cmap::built()) return nullptr;
    const ch::CharacterManifest& m = g_ch.current->manifest;
    const castlemist::ripper::RacePalettes pal = castlemist::ripper::race_palettes(m.race, m.gender);
    return castlemist::cmap::palette(which == 0 ? pal.skin : pal.hair);
}

// The swatch colour shown for `which`; nullopt = as authored (or unknown).
std::optional<std::array<uint8_t, 3>> look_swatch(int which) {
    uint32_t id = look_color(which);
    if (which == 2 && !id) return std::nullopt;  // "same as hair"
    if (!id) return std::nullopt;
    if (const castlemist::cmap::Palette* p = look_palette(which))
        for (const auto& c : p->colors)
            if (c.id == id) return castlemist::ripper::swatch_rgb(*p, c);
    return std::nullopt;
}

void refresh_swatches() {
    for (UINT_PTR id : {ID_CH_SKIN_COLOR, ID_CH_HAIR_COLOR, ID_CH_HAIR_COLOR2})
        if (HWND b = GetDlgItem(g_ch_wnd, static_cast<int>(id))) InvalidateRect(b, nullptr, TRUE);
}

// Fills the face / hair style combos for the shown character's race and selects
// its saved look (or the defaults).
void load_look_for_current() {
    std::string err;
    if (!g_ch_looks.load(ch::default_look_file(), &err)) set_status(utf8_to_wide(err));
    g_ch_look = {};
    SendMessageW(g_ch_face_combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_ch_hair_combo, CB_RESETCONTENT, 0, 0);
    if (!g_ch.current) return;
    const ch::CharacterManifest& m = g_ch.current->manifest;
    g_ch_look = g_ch_looks.get(m.name).value_or(ch::CharacterLook{});
    size_t faces = static_cast<size_t>(g_ch_look.face) + 1, hairs = static_cast<size_t>(g_ch_look.hair) + 1;
    if (g_app->dat_loaded) {
        static std::optional<castlemist::composite::Composite> comp;  // loaded once per session
        if (!comp) comp = castlemist::ripper::load_composite(g_app->data_gw2);
        if (comp)
            if (const auto* r = comp->race(m.race + m.gender)) {
                faces = std::max(faces, r->faces.size());
                hairs = std::max(hairs, r->hair_styles.size());
            }
    }
    for (size_t i = 0; i < faces; ++i)
        SendMessageW(g_ch_face_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Face " + std::to_wstring(i + 1)).c_str()));
    for (size_t i = 0; i < hairs; ++i)
        SendMessageW(g_ch_hair_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Hair " + std::to_wstring(i + 1)).c_str()));
    SendMessageW(g_ch_face_combo, CB_SETCURSEL, static_cast<WPARAM>(g_ch_look.face), 0);
    SendMessageW(g_ch_hair_combo, CB_SETCURSEL, static_cast<WPARAM>(g_ch_look.hair), 0);
    refresh_swatches();
}

void save_look() {
    if (!g_ch.current) {
        set_status(L"Fetch a character first.");
        return;
    }
    std::string err;
    ch::LookStore store;  // re-read so looks saved elsewhere (CLI) survive
    if (!store.load(ch::default_look_file(), &err)) {
        set_status(utf8_to_wide(err));
        return;
    }
    store.set(g_ch.current->manifest.name, g_ch_look);
    if (!store.save(ch::default_look_file(), &err)) {
        set_status(utf8_to_wide(err));
        return;
    }
    g_ch_looks = store;
    set_status(L"Saved the look of " + utf8_to_wide(g_ch.current->manifest.name) + L" - Export character uses it.");
}

// Owner-drawn swatch button: the chosen colour, or a light cross for "as authored".
void draw_swatch(const DRAWITEMSTRUCT* di) {
    const int which = di->CtlID == ID_CH_SKIN_COLOR ? 0 : di->CtlID == ID_CH_HAIR_COLOR ? 1 : 2;
    RECT r = di->rcItem;
    DrawEdge(di->hDC, &r, (di->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_ADJUST);
    std::optional<std::array<uint8_t, 3>> c = look_swatch(which);
    if (c) {
        HBRUSH b = CreateSolidBrush(RGB((*c)[0], (*c)[1], (*c)[2]));
        FillRect(di->hDC, &r, b);
        DeleteObject(b);
    } else {
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(di->hDC, TRANSPARENT);
        const wchar_t* t = which == 2 ? L"= hair" : L"default";
        DrawTextW(di->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (di->itemState & ODS_FOCUS) DrawFocusRect(di->hDC, &r);
}

// ---- colour picker popup: the palette as a grid of swatches; cell 0 = default.
constexpr int kCell = 20, kCols = 16;

int palette_cells() {
    const castlemist::cmap::Palette* p = look_palette(g_ch_palette_target);
    return p ? static_cast<int>(p->colors.size()) + 1 : 1;
}

LRESULT CALLBACK PaletteWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const castlemist::cmap::Palette* p = look_palette(g_ch_palette_target);
        const uint32_t chosen = look_color(g_ch_palette_target);
        for (int i = 0; i < palette_cells(); ++i) {
            RECT r{(i % kCols) * kCell + 2, (i / kCols) * kCell + 2, 0, 0};
            r.right = r.left + kCell - 2;
            r.bottom = r.top + kCell - 2;
            if (i == 0) {
                FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
                MoveToEx(dc, r.left, r.top, nullptr);
                LineTo(dc, r.right, r.bottom);
            } else if (p) {
                const auto rgb = castlemist::ripper::swatch_rgb(*p, p->colors[static_cast<size_t>(i - 1)]);
                HBRUSH b = CreateSolidBrush(RGB(rgb[0], rgb[1], rgb[2]));
                FillRect(dc, &r, b);
                DeleteObject(b);
            }
            const bool sel = i == 0 ? chosen == 0 : (p && p->colors[static_cast<size_t>(i - 1)].id == chosen);
            if (sel) {
                RECT o{r.left - 2, r.top - 2, r.right + 2, r.bottom + 2};
                FrameRect(dc, &o, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                InflateRect(&o, -1, -1);
                FrameRect(dc, &o, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int i = GET_Y_LPARAM(lparam) / kCell * kCols + GET_X_LPARAM(lparam) / kCell;
        const castlemist::cmap::Palette* p = look_palette(g_ch_palette_target);
        if (i == 0) set_status(L"As the texture is authored (no colour).");
        else if (p && i - 1 < static_cast<int>(p->colors.size()))
            set_status(L"Colour " + std::to_wstring(i) + L" of " + std::to_wstring(p->colors.size()) + L" (id " +
                       std::to_wstring(p->colors[static_cast<size_t>(i - 1)].id) + L")");
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int col = GET_X_LPARAM(lparam) / kCell, row = GET_Y_LPARAM(lparam) / kCell;
        const int i = row * kCols + col;
        const castlemist::cmap::Palette* p = look_palette(g_ch_palette_target);
        if (col < kCols && i < palette_cells()) {
            look_color(g_ch_palette_target) = (i == 0 || !p) ? 0 : p->colors[static_cast<size_t>(i - 1)].id;
            refresh_swatches();
            set_status(L"Look changed - Save look to keep it for this character.");
        }
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: g_ch_palette_wnd = nullptr; return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void open_palette(int which, HWND button) {
    if (!g_ch.current) {
        set_status(L"Fetch a character first.");
        return;
    }
    if (!look_palette(which)) {
        set_status(castlemist::ripper::race_palettes(g_ch.current->manifest.race, g_ch.current->manifest.gender).skin
                       ? L"The colours come from the content map - Build map first."
                       : L"No colour palette known for this race.");
        return;
    }
    if (g_ch_palette_wnd) DestroyWindow(g_ch_palette_wnd);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PaletteWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Gw2PaletteWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    g_ch_palette_target = which;
    const int cells = palette_cells();
    const int cols = std::min(cells, kCols), rows = (cells + kCols - 1) / kCols;
    RECT br;
    GetWindowRect(button, &br);
    RECT wr{0, 0, cols * kCell + 2, rows * kCell + 2};
    AdjustWindowRectEx(&wr, WS_POPUP | WS_BORDER, FALSE, WS_EX_TOOLWINDOW);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    g_ch_palette_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2PaletteWnd", L"", WS_POPUP | WS_BORDER, br.left,
                                       br.top - h - 2, w, h, g_ch_wnd, nullptr, g_hinstance, nullptr);
    if (!g_ch_palette_wnd) return;
    ShowWindow(g_ch_palette_wnd, SW_SHOW);
    SetFocus(g_ch_palette_wnd);
}

// ---- buttons -------------------------------------------------------------------

void open_selected_model() {
    LRESULT row = SendMessageW(g_ch_table, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED);
    if (!g_ch.current || row < 0 || static_cast<size_t>(row) >= g_ch.current->manifest.pieces.size()) {
        set_status(L"Select a row first.");
        return;
    }
    const ch::ManifestPiece& p = g_ch.current->manifest.pieces[static_cast<size_t>(row)];
    if (p.status != ch::PieceStatus::Ok || p.file_ids.empty()) {
        set_status(L"That piece has no model (" + utf8_to_wide(ch::to_string(p.status)) + L").");
        return;
    }
    uint32_t mesh = p.file_ids[0];
    for (const auto& [slot, m] : g_ch.exported_mesh)
        if (slot == p.slot && m) mesh = m;  // the race/gender model once an export resolved it
    navigate_to_file_id(mesh);
}

struct ExportDone {
    std::string folder;
    castlemist::ripper::CharacterExportReport report;
};

// Exports every piece of the shown character into a folder the user picks.
void export_pieces() {
    if (!g_ch.current) {
        set_status(L"Fetch a character first.");
        return;
    }
    if (!g_app->dat_loaded) {
        set_status(L"Open Gw2.dat first (File > Open) - the models and textures come from it.");
        return;
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = g_ch_wnd;
    bi.lpszTitle = L"Export the character's pieces (.glb) into:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t folder[MAX_PATH] = L"";
    bool ok = SHGetPathFromIDListW(pidl, folder);
    CoTaskMemFree(pidl);
    if (!ok) return;
    std::string dir = wide_to_utf8(folder);
    std::string dat_path = g_app->data_gw2.file_info.file_path;
    ch::CharacterManifest manifest = g_ch.current->manifest;
    ++g_ch_request;
    set_busy(true);
    set_status(L"Exporting " + std::to_wstring(manifest.pieces.size()) + L" pieces to " + folder + L"...");
    std::thread([manifest, dat_path, dir]() {
        auto* r = new ExportDone{dir, {}};
        try {
            r->report = castlemist::ripper::export_character(manifest, dat_path, dir);
        } catch (const std::exception& e) {  // never let a bad texture or full disk take the app down
            r->report.error = std::string("Export failed: ") + e.what();
        }
        post_result(WM_APP_CHAR_EXPORT_DONE, r);
    }).detach();
}

struct AssembleDone {
    std::string path;
    castlemist::ripper::AssemblyReport report;
};

// The whole character as one rigged .glb, saved where the user picks.
// The character as body.glb + one file per piece (default), or as one combined
// .glb when "Combine into one file" is ticked.
void assemble_character_glb() {
    if (!g_ch.current) {
        set_status(L"Fetch a character first.");
        return;
    }
    if (!g_app->dat_loaded) {
        set_status(L"Open Gw2.dat first (File > Open) - the models and textures come from it.");
        return;
    }
    const bool combined = SendMessageW(g_ch_combine_chk, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring name = utf8_to_wide(g_ch.current->manifest.name);
    for (wchar_t& c : name)
        if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    std::wstring target;
    if (combined) {
        wchar_t path[MAX_PATH] = L"";
        swprintf(path, MAX_PATH, L"%ls.glb", name.c_str());
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_ch_wnd;
        ofn.lpstrFilter = L"glTF binary (*.glb)\0*.glb\0All files\0*.*\0";
        ofn.lpstrFile = path;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrDefExt = L"glb";
        ofn.lpstrTitle = L"Export the whole character as one rigged .glb";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        if (!GetSaveFileNameW(&ofn)) return;
        target = path;
    } else {
        BROWSEINFOW bi{};
        bi.hwndOwner = g_ch_wnd;
        bi.lpszTitle = L"Export the body and each piece (.glb, one skeleton) into:";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
        if (!pidl) return;
        wchar_t folder[MAX_PATH] = L"";
        bool ok = SHGetPathFromIDListW(pidl, folder);
        CoTaskMemFree(pidl);
        if (!ok) return;
        target = std::wstring(folder) + L"\\" + name;  // its own subfolder: body.glb, 01_Coat_...
    }
    std::string out = wide_to_utf8(target);
    std::string dat_path = g_app->data_gw2.file_info.file_path;
    ch::CharacterManifest manifest = g_ch.current->manifest;
    // The Look row, resolved here: palettes are read on this thread only.
    castlemist::ripper::AssemblyOptions opt;
    if (!content_map_building() && castlemist::cmap::built())
        castlemist::ripper::apply_look(opt, g_ch_look, manifest.race, manifest.gender);
    else
        opt.face = g_ch_look.face, opt.hair = g_ch_look.hair;
    ++g_ch_request;
    set_busy(true);
    set_status(L"Assembling " + name + L"...");
    std::thread([manifest, dat_path, out, combined, opt]() {
        auto* r = new AssembleDone{out, {}};
        try {
            r->report = combined ? castlemist::ripper::assemble_character(manifest, dat_path, out, opt)
                                 : castlemist::ripper::assemble_character_separate(manifest, dat_path, out, opt);
        } catch (const std::exception& e) {
            r->report.error = std::string("Assembly failed: ") + e.what();
        }
        post_result(WM_APP_CHAR_ASSEMBLE_DONE, r);
    }).detach();
}

void on_assemble_done(std::unique_ptr<AssembleDone> r) {
    set_busy(false);
    if (!r->report.ok) {
        set_status(utf8_to_wide(r->report.error));
        return;
    }
    size_t used = 0;
    for (const auto& p : r->report.parts) used += p.status == "used";
    std::set<std::string> files;
    for (const auto& p : r->report.parts)
        if (!p.file.empty()) files.insert(p.file);
    set_status(L"Saved " + utf8_to_wide(r->path) + L" (" + std::to_wstring(used) + L" parts" +
               (files.empty() ? std::wstring() : L" in " + std::to_wstring(files.size()) + L" files") + L", " +
               std::to_wstring(r->report.joints) + L" joints).");
}

void on_export_done(std::unique_ptr<ExportDone> r) {
    set_busy(false);
    if (!r->report.error.empty()) {
        set_status(utf8_to_wide(r->report.error));
        return;
    }
    g_ch.exported_mesh.clear();
    for (const auto& [slot, pr] : r->report.pieces) g_ch.exported_mesh.emplace_back(slot, pr.ok ? pr.mesh : 0);
    set_status(L"Exported " + std::to_wstring(r->report.exported()) + L" of " +
               std::to_wstring(r->report.pieces.size()) + L" pieces to " + utf8_to_wide(r->folder) +
               L" (details: export_report.json).");
}

void save_manifest() {
    if (!g_ch.current) {
        set_status(L"Fetch a character first.");
        return;
    }
    std::wstring name = utf8_to_wide(g_ch.current->manifest.name);
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
    out << ch::manifest_to_json(g_ch.current->manifest).dump(2) << '\n';
    set_status(out ? L"Saved " + std::wstring(path) : L"Could not write " + std::wstring(path));
}

void build_map() {
    CmapEnsure r = castlemist::cmap::built() ? rebuild_content_map(g_ch_wnd) : ensure_content_map(g_ch_wnd);
    switch (r) {
    case CmapEnsure::Ready:
        set_status(L"Content map ready.");
        if (g_ch.current) start_character_fetch(g_ch.current->manifest.tab_id);
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
    if (!g_ch.current || sel == CB_ERR || static_cast<size_t>(sel) >= g_ch.current->tabs.size()) return std::nullopt;
    return g_ch.current->tabs[static_cast<size_t>(sel)].tab;
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
        case ID_CH_EXPORT: export_pieces(); return 0;
        case ID_CH_ASSEMBLE: assemble_character_glb(); return 0;
        case ID_CH_FACE:
            if (HIWORD(wparam) == CBN_SELCHANGE)
                g_ch_look.face = static_cast<int>(SendMessageW(g_ch_face_combo, CB_GETCURSEL, 0, 0));
            return 0;
        case ID_CH_HAIR:
            if (HIWORD(wparam) == CBN_SELCHANGE)
                g_ch_look.hair = static_cast<int>(SendMessageW(g_ch_hair_combo, CB_GETCURSEL, 0, 0));
            return 0;
        case ID_CH_SKIN_COLOR: open_palette(0, reinterpret_cast<HWND>(lparam)); return 0;
        case ID_CH_HAIR_COLOR: open_palette(1, reinterpret_cast<HWND>(lparam)); return 0;
        case ID_CH_HAIR_COLOR2: open_palette(2, reinterpret_cast<HWND>(lparam)); return 0;
        case ID_CH_SAVE_LOOK: save_look(); return 0;
        case ID_CH_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (di->CtlID == ID_CH_SKIN_COLOR || di->CtlID == ID_CH_HAIR_COLOR || di->CtlID == ID_CH_HAIR_COLOR2) {
            draw_swatch(di);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lparam);
        if (nm->idFrom == ID_CH_TABLE && nm->code == NM_DBLCLK) open_selected_model();
        break;
    }
    case WM_APP_CHAR_NAMES_DONE: on_names_done(std::unique_ptr<NamesDone>(reinterpret_cast<NamesDone*>(lparam))); return 0;
    case WM_APP_CHAR_FETCH_DONE: on_fetch_done(std::unique_ptr<FetchDone>(reinterpret_cast<FetchDone*>(lparam))); return 0;
    case WM_APP_CHAR_EXPORT_DONE: on_export_done(std::unique_ptr<ExportDone>(reinterpret_cast<ExportDone*>(lparam))); return 0;
    case WM_APP_CHAR_ASSEMBLE_DONE: on_assemble_done(std::unique_ptr<AssembleDone>(reinterpret_cast<AssembleDone*>(lparam))); return 0;
    case WM_APP_CMAP_DONE:
        set_status(L"Content map built.");
        refresh_swatches();
        if (g_ch.current) start_character_fetch(g_ch.current->manifest.tab_id);
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        ++g_ch_request;  // drop any in-flight result
        if (g_ch_palette_wnd) DestroyWindow(g_ch_palette_wnd);
        g_ch_wnd = g_ch_key_combo = g_ch_char_combo = g_ch_tab_combo = g_ch_table = g_ch_status = g_ch_fetch_btn =
            g_ch_build_btn = g_ch_export_btn = g_ch_assemble_btn = g_ch_combine_chk = g_ch_face_combo =
                g_ch_hair_combo = nullptr;
        g_ch_look = {};
        g_ch = RipperState{};
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
    const int W = 820, H = 600;
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
                    H - 240, ID_CH_TABLE);
    SendMessageW(g_ch_table, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    add_column(0, L"Slot", 105);
    add_column(1, L"Item", 210);
    add_column(2, L"Skin", 170);
    add_column(3, L"Dyes", 210);
    add_column(4, L"Status", 85);

    // The Look row: what the API doesn't say, from the game's character creator.
    const int ly = H - 148;
    mk(L"STATIC", L"Look:", SS_LEFT, 10, ly + 4, 35, 18, 0);
    g_ch_face_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 45, ly, 85, 300, ID_CH_FACE);
    g_ch_hair_combo = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, 136, ly, 85, 300, ID_CH_HAIR);
    mk(L"STATIC", L"Skin:", SS_RIGHT, 228, ly + 4, 32, 18, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW, 264, ly, 56, 24, ID_CH_SKIN_COLOR);
    mk(L"STATIC", L"Hair:", SS_RIGHT, 326, ly + 4, 32, 18, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW, 362, ly, 56, 24, ID_CH_HAIR_COLOR);
    mk(L"STATIC", L"Hair 2:", SS_RIGHT, 424, ly + 4, 40, 18, 0);
    mk(L"BUTTON", L"", BS_OWNERDRAW, 468, ly, 56, 24, ID_CH_HAIR_COLOR2);
    mk(L"BUTTON", L"Save look", BS_PUSHBUTTON, 534, ly - 1, 85, 26, ID_CH_SAVE_LOOK);

    g_ch_status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 10, H - 112, W - 36, 18, 0);
    mk(L"BUTTON", L"Open model", BS_PUSHBUTTON, 10, H - 84, 100, 28, ID_CH_OPEN_MODEL);
    mk(L"BUTTON", L"Save manifest...", BS_PUSHBUTTON, 115, H - 84, 115, 28, ID_CH_SAVE);
    g_ch_build_btn = mk(L"BUTTON", L"Build map", BS_PUSHBUTTON, 235, H - 84, 90, 28, ID_CH_BUILD_MAP);
    g_ch_export_btn = mk(L"BUTTON", L"Export pieces...", BS_PUSHBUTTON, 330, H - 84, 115, 28, ID_CH_EXPORT);
    g_ch_assemble_btn = mk(L"BUTTON", L"Export character...", BS_PUSHBUTTON, 450, H - 84, 130, 28, ID_CH_ASSEMBLE);
    g_ch_combine_chk = mk(L"BUTTON", L"Combine into one file", BS_AUTOCHECKBOX, 588, H - 80, 140, 20, ID_CH_COMBINE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 106, H - 84, 80, 28, ID_CH_CLOSE);

    refill_keys();
    if (!g_ch_keys.list().empty())
        set_status(L"Pick a key and click Fetch characters. (Goes online: api.guildwars2.com only.)");
    ShowWindow(g_ch_wnd, SW_SHOW);
}

} // namespace castlemist::ui
