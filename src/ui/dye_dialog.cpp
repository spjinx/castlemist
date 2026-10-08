/// @file
/// @brief The viewer's "Dyes" window: the four dye channels of the armor piece
///        on screen, each a dye from the game's full dye list (the content
///        map's every-dye palette) and the material its shift is taken for.
///        A pick re-bakes the piece's rebuilt atlas in place (rebake_armor_dyes),
///        so Full, Shader and Game 1:1 all show it without a reload.

#include "detail/app_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#include "castlemist/ripper/armor_preview.h"
#include "castlemist/ripper/look.h"

#include <windowsx.h>

namespace castlemist::ui {
namespace {

namespace rp = castlemist::ripper;
namespace cm = castlemist::cmap;

constexpr UINT_PTR ID_DY_SWATCH0 = 2400;  // .. +3
constexpr UINT_PTR ID_DY_MAT0 = 2410;     // .. +3
constexpr UINT_PTR ID_DY_SEARCH = 2420;
constexpr UINT_PTR ID_DY_GRID = 2421;
constexpr UINT_PTR ID_DY_RESET = 2422;
constexpr int kCell = 17, kCols = 24, kRowH = 32;
const wchar_t* const kMaterialNames[4] = {L"Cloth", L"Leather", L"Metal", L"Fur"};

struct DyeUi {
    HWND wnd = nullptr, grid = nullptr, search = nullptr, status = nullptr;
    HWND swatch[4]{}, name[4]{}, mat[4]{};
    int channel = 0;                          // the channel a grid click dyes
    std::vector<const cm::PaletteColor*> all;  // every dye, sorted by colour
    std::vector<const cm::PaletteColor*> shown;  // `all` through the search filter
};
DyeUi* g_dy = nullptr;

void set_status(const std::wstring& s) { if (g_dy) SetWindowTextW(g_dy->status, s.c_str()); }

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring dye_name(uint32_t id) {
    const std::string n = rp::color_name(id);
    return n.empty() ? L"Colour " + std::to_wstring(id) : utf8_to_wide(n);
}

std::array<uint8_t, 3> swatch_of(const cm::PaletteColor& c, int material) {
    const cm::Palette* p = rp::dye_palette();
    return p ? rp::dye_swatch(*p, c, material) : std::array<uint8_t, 3>{128, 128, 128};
}

const cm::PaletteColor* find_dye(uint32_t id) {
    if (const cm::Palette* p = rp::dye_palette())
        for (const cm::PaletteColor& c : p->colors)
            if (c.id == id) return &c;
    return nullptr;
}

// Greys first (dark to light), then by hue in 15-degree bands, dark to light
// within a band: neighbours in the grid look alike.
void sort_dyes() {
    g_dy->all.clear();
    const cm::Palette* p = rp::dye_palette();
    if (!p) return;
    struct Key { int band; float light; const cm::PaletteColor* c; };
    std::vector<Key> keys;
    for (const cm::PaletteColor& c : p->colors) {
        const auto rgb = rp::dye_swatch(*p, c, 0);
        const float r = rgb[0] / 255.0f, g = rgb[1] / 255.0f, b = rgb[2] / 255.0f;
        const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), l = (mx + mn) / 2;
        const float d = mx - mn;
        int band = -1;  // grey
        if (d > 0.06f) {
            float h = mx == r ? std::fmod((g - b) / d, 6.0f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
            h *= 60.0f;
            if (h < 0) h += 360.0f;
            band = static_cast<int>(h / 15.0f);
        }
        keys.push_back({band, l, &c});
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
        return a.band != b.band ? a.band < b.band : a.light < b.light;
    });
    for (const Key& k : keys) g_dy->all.push_back(k.c);
}

void filter_dyes() {
    wchar_t buf[128] = L"";
    GetWindowTextW(g_dy->search, buf, 128);
    const std::wstring q = lower(buf);
    g_dy->shown.clear();
    for (const cm::PaletteColor* c : g_dy->all)
        if (q.empty() || lower(dye_name(c->id)).find(q) != std::wstring::npos) g_dy->shown.push_back(c);
    InvalidateRect(g_dy->grid, nullptr, TRUE);
}

void refresh_channels() {
    for (int i = 0; i < 4; ++i) {
        const rp::DyeChoice& d = g_app->armor_dyes[static_cast<size_t>(i)];
        const bool has = g_app->armor_channels[static_cast<size_t>(i)];
        std::wstring n = has ? dye_name(d.color_id) : L"(no channel)";
        SetWindowTextW(g_dy->name[i], n.c_str());
        SendMessageW(g_dy->mat[i], CB_SETCURSEL, static_cast<WPARAM>(std::clamp(d.material, 0, 3)), 0);
        EnableWindow(g_dy->swatch[i], has);
        EnableWindow(g_dy->mat[i], has);
        InvalidateRect(g_dy->swatch[i], nullptr, TRUE);
    }
    InvalidateRect(g_dy->grid, nullptr, TRUE);
}

void apply() {
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    if (!rebake_armor_dyes()) set_status(L"The model on screen isn't armor with dye channels.");
    refresh_channels();
}

int cell_at(LPARAM lparam) {
    const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
    const int col = x / kCell, i = y / kCell * kCols + col;
    return col < kCols && i >= 0 && i < static_cast<int>(g_dy->shown.size()) ? i : -1;
}

LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_dy) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT all;
        GetClientRect(hwnd, &all);
        FillRect(dc, &all, GetSysColorBrush(COLOR_WINDOW));
        const rp::DyeChoice& cur = g_app->armor_dyes[static_cast<size_t>(g_dy->channel)];
        for (size_t i = 0; i < g_dy->shown.size(); ++i) {
            RECT r{static_cast<LONG>(i % kCols) * kCell + 1, static_cast<LONG>(i / kCols) * kCell + 1, 0, 0};
            r.right = r.left + kCell - 2;
            r.bottom = r.top + kCell - 2;
            const auto rgb = swatch_of(*g_dy->shown[i], cur.material);
            HBRUSH b = CreateSolidBrush(RGB(rgb[0], rgb[1], rgb[2]));
            FillRect(dc, &r, b);
            DeleteObject(b);
            if (g_dy->shown[i]->id == cur.color_id) {
                RECT o{r.left - 1, r.top - 1, r.right + 1, r.bottom + 1};
                FrameRect(dc, &o, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
                InflateRect(&o, -1, -1);
                FrameRect(dc, &o, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
        }
        if (g_dy->shown.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            const wchar_t* t = rp::dye_palette() ? L"No dye matches." : L"The dye list isn't loaded.";
            DrawTextW(dc, t, -1, &all, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int i = cell_at(lparam);
        if (i >= 0) {
            const uint32_t id = g_dy->shown[static_cast<size_t>(i)]->id;
            set_status(dye_name(id) + L" (id " + std::to_wstring(id) + L") - click to dye channel " +
                       std::to_wstring(g_dy->channel + 1));
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int i = cell_at(lparam);
        if (i < 0) return 0;
        g_app->armor_dyes[static_cast<size_t>(g_dy->channel)].color_id = g_dy->shown[static_cast<size_t>(i)]->id;
        apply();
        set_status(L"Channel " + std::to_wstring(g_dy->channel + 1) + L": " +
                   dye_name(g_app->armor_dyes[static_cast<size_t>(g_dy->channel)].color_id));
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void draw_swatch(const DRAWITEMSTRUCT* di) {
    const int ch = static_cast<int>(di->CtlID - ID_DY_SWATCH0);
    RECT r = di->rcItem;
    const bool picked = ch == g_dy->channel;
    // The channel the grid dyes gets a thick dark frame.
    FillRect(di->hDC, &r, GetSysColorBrush(picked ? COLOR_HIGHLIGHT : COLOR_BTNFACE));
    InflateRect(&r, -3, -3);
    const rp::DyeChoice& d = g_app->armor_dyes[static_cast<size_t>(ch)];
    const cm::PaletteColor* c = find_dye(d.color_id);
    if (c && g_app->armor_channels[static_cast<size_t>(ch)]) {
        const auto rgb = swatch_of(*c, d.material);
        HBRUSH b = CreateSolidBrush(RGB(rgb[0], rgb[1], rgb[2]));
        FillRect(di->hDC, &r, b);
        DeleteObject(b);
    } else {
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        MoveToEx(di->hDC, r.left, r.top, nullptr);
        LineTo(di->hDC, r.right, r.bottom);
    }
    FrameRect(di->hDC, &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
}

void load_dye_list() {
    switch (ensure_content_map(g_dy->wnd)) {
    case CmapEnsure::Ready:
        sort_dyes();
        filter_dyes();
        set_status(g_dy->all.empty() ? L"The content map has no dye palette - rebuild it from the Character Ripper."
                                     : std::to_wstring(g_dy->all.size()) +
                                           L" dyes. Pick a channel, then a dye. The material sets how the dye shifts.");
        break;
    case CmapEnsure::Building:
    case CmapEnsure::Started: set_status(L"Building the content map for the dye list..."); break;
    case CmapEnsure::NeedDat: set_status(L"Open Gw2.dat first: the dye list comes from it."); break;
    case CmapEnsure::NeedIndex: set_status(L"The dye list needs the content map - open or build an index first."); break;
    case CmapEnsure::InUse: break;
    }
    refresh_channels();
}

LRESULT CALLBACK DyeWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_dy) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_COMMAND: {
        const UINT_PTR id = LOWORD(wparam);
        if (id >= ID_DY_SWATCH0 && id < ID_DY_SWATCH0 + 4) {
            g_dy->channel = static_cast<int>(id - ID_DY_SWATCH0);
            refresh_channels();
            set_status(L"Channel " + std::to_wstring(g_dy->channel + 1) + L" - click a dye.");
        } else if (id >= ID_DY_MAT0 && id < ID_DY_MAT0 + 4 && HIWORD(wparam) == CBN_SELCHANGE) {
            const int ch = static_cast<int>(id - ID_DY_MAT0);
            g_app->armor_dyes[static_cast<size_t>(ch)].material =
                static_cast<int>(SendMessageW(g_dy->mat[ch], CB_GETCURSEL, 0, 0));
            g_dy->channel = ch;
            apply();
        } else if (id == ID_DY_SEARCH && HIWORD(wparam) == EN_CHANGE) {
            filter_dyes();
        } else if (id == ID_DY_RESET) {
            g_app->armor_dyes = {};
            apply();
            set_status(L"Every channel back to Dye Remover (cloth).");
        }
        return 0;
    }
    case WM_DRAWITEM: {
        const auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (di->CtlID >= ID_DY_SWATCH0 && di->CtlID < ID_DY_SWATCH0 + 4) {
            draw_swatch(di);
            return TRUE;
        }
        break;
    }
    case WM_APP_CMAP_DONE:
        load_dye_list();
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_app->hwnd_dye_wnd = nullptr;
        delete g_dy;
        g_dy = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void dye_dialog_model_changed() {
    if (g_dy) refresh_channels();
}

void open_dye_dialog(HWND owner) {
    if (g_dy) {
        SetForegroundWindow(g_dy->wnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = DyeWndProc;
        wc.lpszClassName = L"Gw2DyeWnd";
        RegisterClassW(&wc);
        wc.lpfnWndProc = GridProc;
        wc.lpszClassName = L"Gw2DyeGridWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        registered = true;
    }
    const int gridW = kCols * kCell + 2, gridH = 28 * kCell + 2;  // 643 dyes fit in 27 rows
    const int pad = 10, W = gridW + 2 * pad;
    const int listY = pad + 4 * kRowH + 8, gridY = listY + 30;
    const int H = gridY + gridH + 74;
    RECT wr{0, 0, W, H};
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW);
    g_dy = new DyeUi;
    g_dy->wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2DyeWnd", L"Dyes", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, owner, nullptr,
                                g_hinstance, nullptr);
    if (!g_dy->wnd) {
        delete g_dy;
        g_dy = nullptr;
        return;
    }
    g_app->hwnd_dye_wnd = g_dy->wnd;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id,
                  DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_dy->wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    for (int i = 0; i < 4; ++i) {
        const int y = pad + i * kRowH;
        mk(L"STATIC", (L"Channel " + std::to_wstring(i + 1)).c_str(), SS_LEFT, pad, y + 7, 58, 18, 0);
        g_dy->swatch[i] = mk(L"BUTTON", L"", BS_OWNERDRAW, pad + 62, y, 52, 28, ID_DY_SWATCH0 + static_cast<UINT_PTR>(i));
        g_dy->name[i] = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, pad + 122, y + 7, W - 2 * pad - 122 - 96, 18, 0);
        g_dy->mat[i] = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, W - pad - 90, y + 3, 90, 200,
                          ID_DY_MAT0 + static_cast<UINT_PTR>(i));
        for (const wchar_t* m : kMaterialNames) SendMessageW(g_dy->mat[i], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(m));
    }
    mk(L"STATIC", L"Find", SS_LEFT, pad, listY + 4, 30, 18, 0);
    g_dy->search = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, pad + 34, listY, gridW - 34, 22, ID_DY_SEARCH);
    g_dy->grid = mk(L"Gw2DyeGridWnd", L"", WS_BORDER, pad, gridY, gridW, gridH, ID_DY_GRID);
    mk(L"BUTTON", L"Reset to Dye Remover", WS_TABSTOP, pad, gridY + gridH + 8, 140, 26, ID_DY_RESET);
    g_dy->status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, pad, gridY + gridH + 42, gridW, 18, 0);
    load_dye_list();
    ShowWindow(g_dy->wnd, SW_SHOW);
}

} // namespace castlemist::ui
