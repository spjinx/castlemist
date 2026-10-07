/// @file
/// @brief Character Ripper > Edit look...: a character's look -- what the GW2
///        API doesn't say -- picked from the game's own character-creator
///        options: face, hair style, ears and skin pattern as pictures (rendered
///        from the dat in the current colours, on a worker thread), and the
///        skin / hair / eye / pattern / glow colours from the race's palettes,
///        named. Save look writes it to character_looks.json, which Export
///        character applies.

#include "detail/app_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "castlemist/character/look_store.h"
#include "castlemist/character/manifest.h"
#include "castlemist/ripper/assemble.h"
#include "castlemist/ripper/face_morphs.h"
#include "castlemist/ripper/look.h"

#include <windowsx.h>

namespace castlemist::ui {
namespace {

namespace ch = castlemist::character;
namespace rp = castlemist::ripper;

constexpr UINT WM_APP_LOOK_THUMBS = WM_APP + 10;
constexpr UINT WM_APP_LOOK_PHYSIQUES = WM_APP + 12;  // wparam: the race's physique count
constexpr UINT_PTR ID_LK_TAB0 = 2300;  // .. +4: Face, Hair, Ears, Pattern, Face details
constexpr UINT_PTR ID_LK_SLIDER0 = 2340;  // .. one per face slider
constexpr UINT_PTR ID_LK_GRID = 2310;
constexpr UINT_PTR ID_LK_SWATCH0 = 2320;  // .. +5: skin, hair, hair 2, eyes, pattern, glow
constexpr UINT_PTR ID_LK_GLOW = 2330;
constexpr UINT_PTR ID_LK_SAVE = 2331;
constexpr UINT_PTR ID_LK_CLOSE = 2332;
constexpr UINT_PTR ID_LK_PHYSIQUE = 2333;
constexpr int kThumb = 112;
constexpr int kColours = 6;
const wchar_t* const kTabNames[5] = {L"Face", L"Hair", L"Ears", L"Pattern", L"Face details"};
const wchar_t* const kColourNames[kColours] = {L"Skin", L"Hair", L"Hair 2", L"Eyes", L"Pattern", L"Glow"};

struct Thumbs {
    unsigned request = 0;
    int tab = 0;
    std::string key;
    std::vector<rp::ImageRgba> images;
    std::string error;
};

struct LookUi {
    HWND wnd = nullptr, grid = nullptr, status = nullptr, glow = nullptr, palette = nullptr, physique = nullptr;
    HWND tabs[5]{}, swatches[kColours]{}, names[kColours]{};
    HWND sliders_panel = nullptr;  // the Face details tab
    std::vector<HWND> sliders;
    ch::CharacterManifest manifest;
    std::string dat_path;
    ch::CharacterLook look;
    int tab = 0;
    int scroll = 0;            // grid scroll offset, px
    int palette_target = 0;    // which colour the picker sets
    std::atomic<unsigned> request{0};
    std::map<std::string, std::vector<HBITMAP>> bitmaps;  // by thumbnail key
    std::string shown_key;     // key whose bitmaps the grid shows
    std::string pending_key;   // key being rendered
};
LookUi* g_lk = nullptr;

void set_status(const std::wstring& s) { if (g_lk) SetWindowTextW(g_lk->status, s.c_str()); }

// ---- colours -------------------------------------------------------------------

uint32_t& colour_of(int which) {
    ch::CharacterLook& l = g_lk->look;
    switch (which) {
    case 0: return l.skin_color;
    case 1: return l.hair_color;
    case 2: return l.hair_color2;
    case 3: return l.eye_color;
    case 4: return l.pattern_color;
    default: return l.glow_color;
    }
}

const castlemist::cmap::Palette* palette_of(int which) {
    if (!g_lk || content_map_building() || !castlemist::cmap::built()) return nullptr;
    const rp::RacePalettes p = rp::race_palettes(g_lk->manifest.race, g_lk->manifest.gender);
    const uint32_t uid = which == 0 ? p.skin : which <= 2 ? p.hair : which == 3 ? p.eye : which == 4 ? p.pattern : p.glow;
    return uid ? castlemist::cmap::palette(uid) : nullptr;
}

std::optional<std::array<uint8_t, 3>> swatch_of(int which, uint32_t id) {
    if (!id) return std::nullopt;
    if (const castlemist::cmap::Palette* p = palette_of(which))
        for (const auto& c : p->colors)
            if (c.id == id) return rp::swatch_rgb(*p, c);
    return std::nullopt;
}

std::wstring name_of(int which) {
    const uint32_t id = colour_of(which);
    if (!id) return which == 2 ? L"same as hair" : which == 5 ? L"no glow" : L"default";
    const std::string n = rp::color_name(id);
    return n.empty() ? L"colour " + std::to_wstring(id) : utf8_to_wide(n);
}

void refresh_colours() {
    for (int i = 0; i < kColours; ++i) {
        InvalidateRect(g_lk->swatches[i], nullptr, TRUE);
        SetWindowTextW(g_lk->names[i], name_of(i).c_str());
        const bool usable = palette_of(i) != nullptr;
        EnableWindow(g_lk->swatches[i], usable);
    }
}

// ---- thumbnails ------------------------------------------------------------------

// Everything that changes how the current tab's pictures look.
std::string thumbs_key(int tab) {
    const ch::CharacterLook& l = g_lk->look;
    char k[256];
    std::snprintf(k, sizeof k, "%d|%d|%d|%d|%u|%u|%u|%u|%d|%u", tab, tab == 0 ? -1 : l.face, tab == 1 ? -1 : l.hair,
                  tab == 2 ? -1 : l.ears, l.skin_color, l.hair_color, l.hair_color2, l.eye_color,
                  tab == 3 ? -1 : l.pattern, l.pattern_color);
    return k;
}

HBITMAP to_bitmap(const rp::ImageRgba& im) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = im.w;
    bi.bmiHeader.biHeight = -im.h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP b = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!b || !bits) return b;
    auto* d = static_cast<uint8_t*>(bits);
    for (size_t i = 0; i + 3 < im.px.size(); i += 4) {
        d[i] = im.px[i + 2];
        d[i + 1] = im.px[i + 1];
        d[i + 2] = im.px[i];
        d[i + 3] = 255;
    }
    return b;
}

int selected_index() {
    const ch::CharacterLook& l = g_lk->look;
    return g_lk->tab == 0 ? l.face : g_lk->tab == 1 ? l.hair : g_lk->tab == 2 ? l.ears : l.pattern;
}

void start_thumbs() {
    const std::string key = thumbs_key(g_lk->tab);
    if (g_lk->bitmaps.count(key)) {
        g_lk->shown_key = key;
        InvalidateRect(g_lk->grid, nullptr, TRUE);
        return;
    }
    if (!g_app->dat_loaded) {
        set_status(L"Open Gw2.dat first (File > Open) - the pictures come from it.");
        return;
    }
    if (g_lk->pending_key == key) return;
    g_lk->pending_key = key;
    const unsigned req = ++g_lk->request;
    const int tab = g_lk->tab;
    // Colours resolve here: palettes are read on the UI thread only.
    rp::AssemblyOptions opt;
    if (!content_map_building() && castlemist::cmap::built())
        rp::apply_look(opt, g_lk->look, g_lk->manifest.race, g_lk->manifest.gender);
    else
        opt.face = g_lk->look.face, opt.hair = g_lk->look.hair, opt.ears = g_lk->look.ears;
    opt.glow_rgb.reset();
    const auto skin = swatch_of(0, g_lk->look.skin_color).value_or(std::array<uint8_t, 3>{190, 160, 120});
    const auto pattern = swatch_of(4, g_lk->look.pattern_color).value_or(std::array<uint8_t, 3>{40, 70, 40});
    HWND wnd = g_lk->wnd;
    set_status(L"Rendering the " + std::wstring(kTabNames[tab]) + L" pictures...");
    std::thread([req, tab, key, opt, skin, pattern, wnd, manifest = g_lk->manifest, dat = g_lk->dat_path]() {
        auto* r = new Thumbs{req, tab, key, {}, {}};
        try {
            if (tab == 3)
                r->images = rp::pattern_thumbnails(manifest, dat, skin, pattern, kThumb * 2, &r->error);
            else
                r->images = rp::look_thumbnails(manifest, dat,
                                                tab == 0 ? rp::LookPart::Face : tab == 1 ? rp::LookPart::Hair
                                                                                         : rp::LookPart::Ears,
                                                opt, kThumb, &r->error);
        } catch (const std::exception& e) {
            r->error = e.what();
        }
        if (!IsWindow(wnd) || !PostMessageW(wnd, WM_APP_LOOK_THUMBS, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void on_thumbs(std::unique_ptr<Thumbs> r) {
    if (r->key == g_lk->pending_key) g_lk->pending_key.clear();
    if (!r->error.empty() && r->images.empty()) {
        set_status(L"Could not render the pictures: " + utf8_to_wide(r->error));
        return;
    }
    std::vector<HBITMAP> bms;
    for (const auto& im : r->images) bms.push_back(to_bitmap(im));
    g_lk->bitmaps[r->key] = std::move(bms);
    if (r->tab == g_lk->tab && r->key == thumbs_key(g_lk->tab)) {
        g_lk->shown_key = r->key;
        InvalidateRect(g_lk->grid, nullptr, TRUE);
        set_status(L"Pick a " + std::wstring(kTabNames[g_lk->tab]) + L". Colours are on the right.");
    } else {
        start_thumbs();  // the look changed meanwhile
    }
}

// ---- the picture grid -------------------------------------------------------------

struct Cell { int w, h; };
Cell cell_size() { return g_lk->tab == 3 ? Cell{kThumb * 2 + 8, kThumb + 26} : Cell{kThumb + 8, kThumb + 26}; }

int grid_count() {
    auto it = g_lk->bitmaps.find(g_lk->shown_key);
    const int n = it == g_lk->bitmaps.end() ? 0 : static_cast<int>(it->second.size());
    return g_lk->tab == 3 ? n + 1 : n;  // patterns: a "None" cell first
}

int grid_cols() {
    RECT r;
    GetClientRect(g_lk->grid, &r);
    return std::max(1, static_cast<int>(r.right) / cell_size().w);
}

void update_scroll() {
    RECT r;
    GetClientRect(g_lk->grid, &r);
    const int rows = (grid_count() + grid_cols() - 1) / grid_cols();
    const int total = rows * cell_size().h;
    g_lk->scroll = std::clamp(g_lk->scroll, 0, std::max(0, total - static_cast<int>(r.bottom)));
    SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, total - 1), static_cast<UINT>(r.bottom),
                  g_lk->scroll, 0};
    SetScrollInfo(g_lk->grid, SB_VERT, &si, TRUE);
}

LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_lk) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        GetClientRect(hwnd, &cr);
        FillRect(dc, &cr, GetSysColorBrush(COLOR_APPWORKSPACE));
        update_scroll();
        auto it = g_lk->bitmaps.find(g_lk->shown_key);
        if (it == g_lk->bitmaps.end()) {
            SetBkMode(dc, TRANSPARENT);
            DrawTextW(dc, L"Rendering...", -1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(hwnd, &ps);
            return 0;
        }
        const Cell c = cell_size();
        const int cols = grid_cols(), n = grid_count(), sel = selected_index();
        HDC mem = CreateCompatibleDC(dc);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        for (int i = 0; i < n; ++i) {
            const int x = (i % cols) * c.w + 4, y = (i / cols) * c.h + 4 - g_lk->scroll;
            if (y + c.h < 0 || y > cr.bottom) continue;
            const int option = g_lk->tab == 3 ? i - 1 : i;  // patterns: -1 = none
            RECT img{x, y, x + c.w - 8, y + c.h - 26};
            if (g_lk->tab == 3 && i == 0) {
                FillRect(dc, &img, GetSysColorBrush(COLOR_BTNFACE));
                DrawTextW(dc, L"No pattern", -1, &img, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            } else {
                HBITMAP b = it->second[static_cast<size_t>(option)];
                BITMAP bm{};
                GetObject(b, sizeof bm, &bm);
                HGDIOBJ old = SelectObject(mem, b);
                BitBlt(dc, img.left, img.top, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
            }
            RECT label{x, img.bottom + 2, x + c.w - 8, y + c.h - 4};
            const std::wstring text = option < 0 ? L"" : std::wstring(kTabNames[g_lk->tab]) + L" " + std::to_wstring(option + 1);
            DrawTextW(dc, text.c_str(), -1, &label, DT_CENTER | DT_SINGLELINE);
            if (option == sel) {
                RECT o{img.left - 3, img.top - 3, img.right + 3, img.bottom + 3};
                HBRUSH hb = CreateSolidBrush(RGB(255, 200, 40));
                for (int k = 0; k < 3; ++k) {
                    FrameRect(dc, &o, hb);
                    InflateRect(&o, -1, -1);
                }
                DeleteObject(hb);
            }
        }
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: update_scroll(); return 0;
    case WM_VSCROLL: {
        SCROLLINFO si{sizeof si, SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wparam)) {
        case SB_LINEUP: pos -= 40; break;
        case SB_LINEDOWN: pos += 40; break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        g_lk->scroll = pos;
        update_scroll();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_lk->scroll -= GET_WHEEL_DELTA_WPARAM(wparam) / 2;
        update_scroll();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        const Cell c = cell_size();
        const int col = GET_X_LPARAM(lparam) / c.w, row = (GET_Y_LPARAM(lparam) + g_lk->scroll) / c.h;
        const int i = row * grid_cols() + col;
        if (col >= grid_cols() || i >= grid_count()) return 0;
        const int option = g_lk->tab == 3 ? i - 1 : i;
        ch::CharacterLook& l = g_lk->look;
        (g_lk->tab == 0 ? l.face : g_lk->tab == 1 ? l.hair : g_lk->tab == 2 ? l.ears : l.pattern) = option;
        InvalidateRect(hwnd, nullptr, FALSE);
        set_status(option < 0 ? L"No pattern - Save look to keep it."
                              : std::wstring(kTabNames[g_lk->tab]) + L" " + std::to_wstring(option + 1) +
                                    L" - Save look to keep it.");
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---- colour picker popup (the palette as a grid; cell 0 = default) -------------------

constexpr int kCell = 20, kCols = 16;

int palette_cells() {
    const castlemist::cmap::Palette* p = palette_of(g_lk->palette_target);
    return p ? static_cast<int>(p->colors.size()) + 1 : 1;
}

LRESULT CALLBACK PaletteProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_lk) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        const castlemist::cmap::Palette* p = palette_of(g_lk->palette_target);
        const uint32_t chosen = colour_of(g_lk->palette_target);
        for (int i = 0; i < palette_cells(); ++i) {
            RECT r{(i % kCols) * kCell + 2, (i / kCols) * kCell + 2, 0, 0};
            r.right = r.left + kCell - 2;
            r.bottom = r.top + kCell - 2;
            if (i == 0) {
                FillRect(dc, &r, GetSysColorBrush(COLOR_BTNFACE));
                MoveToEx(dc, r.left, r.top, nullptr);
                LineTo(dc, r.right, r.bottom);
            } else if (p) {
                const auto rgb = rp::swatch_rgb(*p, p->colors[static_cast<size_t>(i - 1)]);
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
        const int col = GET_X_LPARAM(lparam) / kCell;
        const int i = GET_Y_LPARAM(lparam) / kCell * kCols + col;
        const castlemist::cmap::Palette* p = palette_of(g_lk->palette_target);
        if (col >= kCols) return 0;
        if (i == 0) {
            set_status(L"Default (as the texture is authored).");
        } else if (p && i - 1 < static_cast<int>(p->colors.size())) {
            const uint32_t id = p->colors[static_cast<size_t>(i - 1)].id;
            const std::string n = rp::color_name(id);
            set_status((n.empty() ? L"Colour" : utf8_to_wide(n)) + L" - " + std::to_wstring(i) + L" of " +
                       std::to_wstring(p->colors.size()) + L" (id " + std::to_wstring(id) + L")");
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int col = GET_X_LPARAM(lparam) / kCell;
        const int i = GET_Y_LPARAM(lparam) / kCell * kCols + col;
        const castlemist::cmap::Palette* p = palette_of(g_lk->palette_target);
        if (col < kCols && i < palette_cells()) {
            colour_of(g_lk->palette_target) = (i == 0 || !p) ? 0 : p->colors[static_cast<size_t>(i - 1)].id;
            refresh_colours();
            set_status(std::wstring(kColourNames[g_lk->palette_target]) + L": " + name_of(g_lk->palette_target) +
                       L" - Save look to keep it.");
            if (g_lk->palette_target != 5) start_thumbs();  // the pictures show the new colour
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
    case WM_DESTROY:
        if (g_lk) g_lk->palette = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void open_palette(int which) {
    if (!palette_of(which)) {
        set_status(L"The colours come from the content map - Build map in the Character Ripper first.");
        return;
    }
    if (g_lk->palette) DestroyWindow(g_lk->palette);
    g_lk->palette_target = which;
    const int cells = palette_cells();
    const int cols = std::min(cells, kCols), rows = (cells + kCols - 1) / kCols;
    RECT br;
    GetWindowRect(g_lk->swatches[which], &br);
    RECT wr{0, 0, cols * kCell + 2, rows * kCell + 2};
    AdjustWindowRectEx(&wr, WS_POPUP | WS_BORDER, FALSE, WS_EX_TOOLWINDOW);
    const int w = wr.right - wr.left, h = wr.bottom - wr.top;
    g_lk->palette = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2LookPaletteWnd", L"", WS_POPUP | WS_BORDER,
                                    std::max(0L, br.right - w), br.bottom + 2, w, h, g_lk->wnd, nullptr, g_hinstance,
                                    nullptr);
    if (!g_lk->palette) return;
    ShowWindow(g_lk->palette, SW_SHOW);
    SetFocus(g_lk->palette);
}

void draw_swatch(const DRAWITEMSTRUCT* di) {
    const int which = static_cast<int>(di->CtlID - ID_LK_SWATCH0);
    RECT r = di->rcItem;
    DrawEdge(di->hDC, &r, (di->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT | BF_ADJUST);
    if (auto c = swatch_of(which, colour_of(which))) {
        // Glow swatches are stored dim; show them lit, the way they glow.
        std::array<uint8_t, 3> rgb = *c;
        if (which == 5) {
            const int peak = std::max({rgb[0], rgb[1], rgb[2]});
            if (peak > 0)
                for (uint8_t& v : rgb) v = static_cast<uint8_t>(v * 255 / peak);
        }
        HBRUSH b = CreateSolidBrush(RGB(rgb[0], rgb[1], rgb[2]));
        FillRect(di->hDC, &r, b);
        DeleteObject(b);
    } else {
        FillRect(di->hDC, &r, GetSysColorBrush(COLOR_BTNFACE));
        MoveToEx(di->hDC, r.left, r.top, nullptr);
        LineTo(di->hDC, r.right, r.bottom);
    }
}

// ---- face details ---------------------------------------------------------------------

LRESULT CALLBACK SlidersProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_HSCROLL && g_lk) {
        HWND t = reinterpret_cast<HWND>(lparam);
        const auto& names = rp::face_slider_names();
        for (size_t i = 0; i < g_lk->sliders.size() && i < names.size(); ++i)
            if (g_lk->sliders[i] == t) {
                const int pos = static_cast<int>(SendMessageW(t, TBM_GETPOS, 0, 0));
                g_lk->look.sliders[names[i]] = pos / 100.0f;
                set_status(utf8_to_wide(names[i]) + L": " + std::to_wstring(pos) + L"% (50 = middle) - Save look to keep it.");
            }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---- the dialog --------------------------------------------------------------------

void save_look() {
    std::string err;
    ch::LookStore store;  // re-read so looks saved elsewhere (CLI) survive
    if (!store.load(ch::default_look_file(), &err)) {
        set_status(utf8_to_wide(err));
        return;
    }
    store.set(g_lk->manifest.name, g_lk->look);
    if (!store.save(ch::default_look_file(), &err)) {
        set_status(utf8_to_wide(err));
        return;
    }
    set_status(L"Saved the look of " + utf8_to_wide(g_lk->manifest.name) + L" - Export character applies it.");
}

void select_tab(int tab) {
    for (int i = 0; i < 5; ++i) SendMessageW(g_lk->tabs[i], BM_SETCHECK, i == tab ? BST_CHECKED : BST_UNCHECKED, 0);
    ShowWindow(g_lk->sliders_panel, tab == 4 ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lk->grid, tab == 4 ? SW_HIDE : SW_SHOW);
    if (tab == 4) {
        set_status(L"Face details become blend shapes (shape keys) on the export; these set their starting values.");
        return;
    }
    g_lk->tab = tab;
    g_lk->scroll = 0;
    g_lk->shown_key.clear();
    InvalidateRect(g_lk->grid, nullptr, TRUE);
    start_thumbs();
}

LRESULT CALLBACK LookWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_lk) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_COMMAND: {
        const UINT_PTR id = LOWORD(wparam);
        if (id >= ID_LK_TAB0 && id < ID_LK_TAB0 + 5) select_tab(static_cast<int>(id - ID_LK_TAB0));
        else if (id >= ID_LK_SWATCH0 && id < ID_LK_SWATCH0 + kColours) open_palette(static_cast<int>(id - ID_LK_SWATCH0));
        else if (id == ID_LK_PHYSIQUE && HIWORD(wparam) == CBN_SELCHANGE) {
            // Item 0 = not set (the models as they come), then Physique 1..N.
            const LRESULT sel = SendMessageW(g_lk->physique, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) {
                g_lk->look.physique = static_cast<int>(sel) - 1;
                set_status((sel == 0 ? std::wstring(L"Physique not set") : L"Physique " + std::to_wstring(sel)) +
                           L" - Save look to keep it.");
            }
        }
        else if (id == ID_LK_SAVE) save_look();
        else if (id == ID_LK_CLOSE) DestroyWindow(hwnd);
        return 0;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == g_lk->glow) {
            g_lk->look.glow_intensity = static_cast<float>(SendMessageW(g_lk->glow, TBM_GETPOS, 0, 0)) / 100.0f;
            set_status(L"Glow intensity " + std::to_wstring(static_cast<int>(g_lk->look.glow_intensity * 100 + 0.5f)) +
                       L"% - Save look to keep it.");
        }
        return 0;
    case WM_DRAWITEM: {
        auto* di = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (di->CtlID >= ID_LK_SWATCH0 && di->CtlID < ID_LK_SWATCH0 + kColours) {
            draw_swatch(di);
            return TRUE;
        }
        break;
    }
    case WM_APP_LOOK_THUMBS: on_thumbs(std::unique_ptr<Thumbs>(reinterpret_cast<Thumbs*>(lparam))); return 0;
    case WM_APP_LOOK_PHYSIQUES: {
        const int n = std::max(static_cast<int>(wparam), g_lk->look.physique + 1);
        SendMessageW(g_lk->physique, CB_RESETCONTENT, 0, 0);
        SendMessageW(g_lk->physique, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Not set"));
        for (int i = 1; i <= n; ++i)
            SendMessageW(g_lk->physique, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((L"Physique " + std::to_wstring(i)).c_str()));
        SendMessageW(g_lk->physique, CB_SETCURSEL, static_cast<WPARAM>(g_lk->look.physique + 1), 0);
        EnableWindow(g_lk->physique, n > 0);
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        if (g_lk->palette) DestroyWindow(g_lk->palette);
        for (auto& [k, v] : g_lk->bitmaps)
            for (HBITMAP b : v) DeleteObject(b);
        LookUi* dead = g_lk;
        g_lk = nullptr;  // late thumbnail results find no window and free themselves
        delete dead;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void open_look_dialog(HWND owner, const castlemist::character::CharacterManifest& manifest) {
    if (g_lk) DestroyWindow(g_lk->wnd);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = LookWndProc;
        wc.lpszClassName = L"Gw2LookWnd";
        RegisterClassW(&wc);
        wc.lpfnWndProc = GridProc;
        wc.lpszClassName = L"Gw2LookGridWnd";
        wc.hbrBackground = nullptr;
        RegisterClassW(&wc);
        wc.lpfnWndProc = SlidersProc;
        wc.lpszClassName = L"Gw2LookSlidersWnd";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassW(&wc);
        wc.lpfnWndProc = PaletteProc;
        wc.lpszClassName = L"Gw2LookPaletteWnd";
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    g_lk = new LookUi;
    g_lk->manifest = manifest;
    g_lk->dat_path = g_app->dat_loaded ? g_app->data_gw2.file_info.file_path : std::string();
    {
        ch::LookStore store;
        std::string err;
        if (store.load(ch::default_look_file(), &err)) g_lk->look = store.get(manifest.name).value_or(ch::CharacterLook{});
    }
    const int W = 1000, H = 720;
    const std::wstring title = L"Look - " + utf8_to_wide(manifest.name) + L" (" + utf8_to_wide(manifest.race) + L" " +
                               utf8_to_wide(manifest.gender) + L")";
    g_lk->wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2LookWnd", title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner, nullptr, g_hinstance, nullptr);
    if (!g_lk->wnd) {
        delete g_lk;
        g_lk = nullptr;
        return;
    }
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id,
                  DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_lk->wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    for (int i = 0; i < 5; ++i)
        g_lk->tabs[i] = mk(L"BUTTON", kTabNames[i], BS_AUTORADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0),
                           10 + i * 92, 10, 88, 28, ID_LK_TAB0 + static_cast<UINT_PTR>(i));
    g_lk->grid = mk(L"Gw2LookGridWnd", L"", WS_VSCROLL | WS_BORDER | WS_TABSTOP, 10, 46, 690, H - 150, ID_LK_GRID);
    // Face details: one slider per makeover-kit slider (0..100, 50 = the middle tick).
    g_lk->sliders_panel = CreateWindowExW(0, L"Gw2LookSlidersWnd", L"", WS_CHILD | WS_BORDER, 10, 46, 690, H - 150,
                                          g_lk->wnd, nullptr, g_hinstance, nullptr);
    {
        const auto& names = rp::face_slider_names();
        const int rows = (static_cast<int>(names.size()) + 1) / 2;
        for (size_t i = 0; i < names.size(); ++i) {
            const int col = static_cast<int>(i) / rows, row = static_cast<int>(i) % rows;
            const int x = 10 + col * 340, y = 10 + row * 44;
            HWND l = CreateWindowExW(0, L"STATIC", utf8_to_wide(names[i]).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, x, y,
                                     320, 16, g_lk->sliders_panel, nullptr, g_hinstance, nullptr);
            SendMessageW(l, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            HWND t = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS, x, y + 16,
                                     320, 26, g_lk->sliders_panel,
                                     reinterpret_cast<HMENU>(ID_LK_SLIDER0 + i), g_hinstance, nullptr);
            SendMessageW(t, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(t, TBM_SETTICFREQ, 50, 0);
            auto it = g_lk->look.sliders.find(names[i]);
            SendMessageW(t, TBM_SETPOS, TRUE, static_cast<LPARAM>((it == g_lk->look.sliders.end() ? 0.5f : it->second) * 100 + 0.5f));
            g_lk->sliders.push_back(t);
        }
    }

    const int cx = 716;
    mk(L"STATIC", L"Colours", SS_LEFT, cx, 16, 120, 18, 0);
    for (int i = 0; i < kColours; ++i) {
        const int y = 46 + i * 40;
        mk(L"STATIC", kColourNames[i], SS_LEFT, cx, y + 5, 52, 18, 0);
        g_lk->swatches[i] = mk(L"BUTTON", L"", BS_OWNERDRAW, cx + 56, y, 56, 26, ID_LK_SWATCH0 + static_cast<UINT_PTR>(i));
        g_lk->names[i] = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, cx + 120, y + 5, 150, 18, 0);
    }
    const int gy = 46 + kColours * 40 + 6;
    mk(L"STATIC", L"Glow intensity", SS_LEFT, cx, gy + 4, 90, 18, 0);
    g_lk->glow = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS, cx + 92, gy, 170, 28, ID_LK_GLOW);
    SendMessageW(g_lk->glow, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_lk->glow, TBM_SETPOS, TRUE, static_cast<LPARAM>(g_lk->look.glow_intensity * 100 + 0.5f));
    if (rp::race_palettes(manifest.race, manifest.gender).glow == 0) EnableWindow(g_lk->glow, FALSE);
    // Physique: the character creator's body types, in its order; the count
    // comes off the dat on a worker.
    mk(L"STATIC", L"Physique", SS_LEFT, cx, gy + 40, 90, 18, 0);
    g_lk->physique = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, cx + 92, gy + 36, 170, 200, ID_LK_PHYSIQUE);
    SendMessageW(g_lk->physique, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(g_lk->look.physique < 0 ? L"Not set"
                                                                   : (L"Physique " + std::to_wstring(g_lk->look.physique + 1)).c_str()));
    SendMessageW(g_lk->physique, CB_SETCURSEL, 0, 0);
    EnableWindow(g_lk->physique, FALSE);
    {
        HWND wnd = g_lk->wnd;
        std::thread([wnd, manifest, dat = g_lk->dat_path]() {
            int n = 0;
            try {
                n = rp::physique_count(manifest, dat);
            } catch (const std::exception&) {
            }
            if (IsWindow(wnd)) PostMessageW(wnd, WM_APP_LOOK_PHYSIQUES, static_cast<WPARAM>(n), 0);
        }).detach();
    }
    mk(L"STATIC",
       L"Styles are the game's character-creator options, drawn from the dat in this look's colours. "
       L"Colour names: the GW2 API (dyes) and the GW2 wiki.",
       SS_LEFT, cx, gy + 80, 260, 80, 0);

    g_lk->status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 10, H - 96, W - 36, 18, 0);
    mk(L"BUTTON", L"Save look", BS_DEFPUSHBUTTON, W - 220, H - 70, 100, 28, ID_LK_SAVE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 110, H - 70, 80, 28, ID_LK_CLOSE);

    refresh_colours();
    select_tab(0);
    ShowWindow(g_lk->wnd, SW_SHOW);
}

} // namespace castlemist::ui
