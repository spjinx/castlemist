/// @file
/// @brief View > Texture Gallery...: a thumbnail browser over every image
///        entry in the index (ATEU / ATEX family, DDS, WebP, PNG).
///
/// The source list is the index's `entries` rows of the image types, read once
/// with no decoding. Width, height and format are not in the index, so a pool of
/// workers reads them from the dat in the background -- only the first few KB of
/// each entry, inflated no further than its header -- UI art first, and keeps
/// them in a side file next to the index (gw2_texture_headers.bin, tied to the
/// dat fingerprint) so the next gallery opens with them. Size, icon and alpha
/// filters refine as headers arrive. The same workers decode the thumbnails of
/// the cells in view (plus a page either side) ahead of the scan, into a
/// memory-capped LRU on the UI thread.
///
/// Click shows the entry in the main preview; Ctrl / Shift extend the
/// selection, which "Export selected..." writes as <id>.png and "Copy IDs"
/// puts on the clipboard.

#include "detail/app_state.h"
#include "detail/texture_gallery.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>

#include "castlemist/core/env.h"
#include "castlemist/db/data_status.h"
#include "castlemist/exportgltf/sky_project.h"

namespace castlemist::ui {
namespace {

namespace gl = castlemist::ui::gallery;

constexpr UINT WM_APP_GALLERY_THUMB = WM_APP + 40;   // lparam: ThumbDone* (owned by the receiver)
constexpr UINT WM_APP_GALLERY_EXPORT = WM_APP + 41;  // wparam: done, lparam: total; done > total = finished
constexpr UINT_PTR ID_TG_KIND0 = 2500;               // .. +3: UI, World, Web, DDS
constexpr UINT_PTR ID_TG_MIN = 2504;
constexpr UINT_PTR ID_TG_MAX = 2505;
constexpr UINT_PTR ID_TG_ICONS = 2506;
constexpr UINT_PTR ID_TG_ALPHA = 2507;
constexpr UINT_PTR ID_TG_SEARCH = 2508;
constexpr UINT_PTR ID_TG_SIZE = 2509;
constexpr UINT_PTR ID_TG_EXPORT = 2510;
constexpr UINT_PTR ID_TG_COPY = 2511;
constexpr UINT_PTR ID_TG_GRID = 2512;
constexpr UINT_PTR TIMER_TG_POLL = 1;    // headers arriving, status line
constexpr UINT_PTR TIMER_TG_EDIT = 2;    // debounce for the search and size boxes
constexpr wchar_t kWndClass[] = L"Gw2TextureGalleryWnd";
constexpr wchar_t kGridClass[] = L"Gw2TextureGalleryGrid";
constexpr int kThumbBox = 128;           // thumbs are made at the largest cell size and scaled to the others
constexpr int kBoxes[] = {64, 96, 128};
constexpr size_t kHeaderPrefix = 512;    // decompressed bytes a header read inflates
constexpr size_t kCacheBytes = size_t{256} << 20;
constexpr int kTopBarH = 66, kStatusH = 22, kPad = 8;
constexpr int kLabelMinW = 100;          // cells stay wide enough for "1024x1024" at 64 px
constexpr const char* kTypes[] = {"texture", "dds", "riff", "png"};

/// What the workers share with the window. Held by shared_ptr so a worker that
/// is still finishing an entry when the window closes reads valid memory.
struct Shared {
    std::string dat_path;
    std::vector<uint32_t> ids;                        ///< base id per entry
    std::vector<MftData> mft;                         ///< its MFT record
    std::unique_ptr<std::atomic<uint64_t>[]> headers; ///< gl::Header::pack() per entry, 0 = not read
    std::atomic<size_t> known{0};
    std::vector<uint32_t> scan_order;                 ///< entries the header scan visits, UI art first
    std::atomic<size_t> scan_next{0};
    std::atomic<bool> stop{false};
    HWND notify = nullptr;

    std::mutex m;
    std::condition_variable cv;
    std::deque<uint32_t> thumb_jobs;                  ///< entries to thumbnail, most wanted first
    std::unordered_set<uint32_t> in_flight;

    /// Replace the thumbnail queue with @p entries (anything no longer wanted is dropped).
    void want(const std::vector<uint32_t>& entries) {
        {
            std::lock_guard<std::mutex> lk(m);
            thumb_jobs.clear();
            for (uint32_t e : entries)
                if (!in_flight.count(e)) thumb_jobs.push_back(e);
        }
        cv.notify_all();
    }
    void publish(uint32_t entry, const gl::Header& h) {
        uint64_t expected = 0;
        if (headers[entry].compare_exchange_strong(expected, h.pack())) known.fetch_add(1);
    }
};

struct ThumbDone {
    uint32_t entry = 0;
    gl::Thumb thumb;  ///< empty = could not be decoded
};

void worker_main(std::shared_ptr<Shared> sh) {
    std::ifstream dat(sh->dat_path, std::ios::binary);
    while (!sh->stop) {
        uint32_t job = 0;
        bool thumb = false;
        {
            std::unique_lock<std::mutex> lk(sh->m);
            if (!sh->thumb_jobs.empty()) {
                job = sh->thumb_jobs.front();
                sh->thumb_jobs.pop_front();
                sh->in_flight.insert(job);
                thumb = true;
            } else if (sh->scan_next >= sh->scan_order.size()) {
                sh->cv.wait_for(lk, std::chrono::milliseconds(250));
                continue;
            }
        }
        if (thumb) {
            auto done = std::make_unique<ThumbDone>();
            done->entry = job;
            try {
                std::vector<uint8_t> bytes = gl::read_whole(dat, sh->mft[job]);
                sh->publish(job, gl::read_header(bytes.data(), bytes.size()));
                gl::Rgba img;
                if (gl::decode_image(std::move(bytes), kThumbBox, img)) done->thumb = gl::make_thumb(img, kThumbBox);
            } catch (const std::exception&) {
            }
            {
                std::lock_guard<std::mutex> lk(sh->m);
                sh->in_flight.erase(job);
            }
            if (!sh->stop && PostMessageW(sh->notify, WM_APP_GALLERY_THUMB, 0, reinterpret_cast<LPARAM>(done.get())))
                done.release();
            continue;
        }
        const size_t i = sh->scan_next.fetch_add(1);
        if (i >= sh->scan_order.size()) continue;
        const uint32_t e = sh->scan_order[i];
        if (sh->headers[e].load()) continue;
        gl::Header h;
        try {
            std::vector<uint8_t> prefix = gl::read_prefix(dat, sh->mft[e], kHeaderPrefix);
            h = gl::read_header(prefix.data(), prefix.size());
        } catch (const std::exception&) {
            h.known = true;  // unreadable: hidden, like any non-image
        }
        sh->publish(e, h);
    }
}

struct Gallery {
    HWND wnd = nullptr, grid = nullptr, status = nullptr;
    HWND kinds[gl::kKindCount]{}, min_edit = nullptr, max_edit = nullptr, icons = nullptr, alpha = nullptr;
    HWND search = nullptr, size_combo = nullptr, export_btn = nullptr, copy_btn = nullptr;
    std::shared_ptr<Shared> sh;
    std::vector<gl::Kind> kind;          ///< per entry
    std::vector<std::thread> workers;
    std::thread exporter;
    gl::Filter filter;
    std::string filter_error;
    std::vector<uint32_t> shown;         ///< entries passing the filter, in base id order
    size_t known_at_filter = 0;
    gl::ThumbCache cache{kCacheBytes};
    std::set<uint32_t> selected;         ///< entries
    int anchor = -1;                     ///< position in `shown` Shift extends from
    int box = 96;
    int line_h = 16;                     ///< label line height, from the grid font
    int top_row = 0;
    std::filesystem::path header_file;
    std::string fingerprint;
    size_t known_saved = 0;              ///< headers on disk
    bool exporting = false;
    std::wstring export_note;
    std::string shot_path;               ///< GW2_GALLERY_SHOT: capture once the view settles
    int shot_wait = 0;
};
Gallery* g_tg = nullptr;

// ---- the filtered list -------------------------------------------------------

gl::Header header_of(uint32_t entry) { return gl::Header::unpack(g_tg->sh->headers[entry].load()); }

struct GridMetrics {
    int cell_w, cell_h, cols, x0, visible_rows, full_rows, total_rows;
};

GridMetrics metrics() {
    RECT rc;
    GetClientRect(g_tg->grid, &rc);
    GridMetrics m{};
    m.cell_w = std::max(g_tg->box, kLabelMinW) + 2 * kPad;
    m.cell_h = g_tg->box + 3 * g_tg->line_h + kPad;
    m.cols = std::max(1, static_cast<int>(rc.right) / m.cell_w);
    m.x0 = std::max(0, (static_cast<int>(rc.right) - m.cols * m.cell_w) / 2);
    m.full_rows = std::max(1, static_cast<int>(rc.bottom) / m.cell_h);
    m.visible_rows = static_cast<int>(rc.bottom) / m.cell_h + 2;
    m.total_rows = static_cast<int>((g_tg->shown.size() + m.cols - 1) / m.cols);
    return m;
}

void update_scrollbar() {
    const GridMetrics m = metrics();
    g_tg->top_row = std::clamp(g_tg->top_row, 0, std::max(0, m.total_rows - m.full_rows));
    SCROLLINFO si{sizeof si};
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = std::max(0, m.total_rows - 1);
    si.nPage = static_cast<UINT>(m.full_rows);
    si.nPos = g_tg->top_row;
    SetScrollInfo(g_tg->grid, SB_VERT, &si, TRUE);
}

void update_status() {
    const size_t total = g_tg->sh->ids.size();
    const size_t known = g_tg->sh->known.load();
    wchar_t buf[256];
    swprintf(buf, 256, L"%zu of %zu shown  |  %zu selected  |  headers %zu / %zu%ls  |  thumbs %.0f MB",
             g_tg->shown.size(), total, g_tg->selected.size(), known, total,
             known < total && g_tg->filter.needs_header() ? L" (list grows as they arrive)" : L"",
             g_tg->cache.bytes() / 1048576.0);
    std::wstring s = buf;
    if (!g_tg->filter_error.empty()) s += L"  |  " + utf8_to_wide(g_tg->filter_error);
    if (!g_tg->export_note.empty()) s += L"  |  " + g_tg->export_note;
    SetWindowTextW(g_tg->status, s.c_str());
}

void refilter() {
    const GridMetrics before = metrics();
    const size_t top_pos = static_cast<size_t>(g_tg->top_row) * before.cols;
    const uint32_t top_entry = top_pos < g_tg->shown.size() ? g_tg->shown[top_pos] : 0;

    g_tg->known_at_filter = g_tg->sh->known.load();
    g_tg->shown.clear();
    const auto& ids = g_tg->sh->ids;
    for (uint32_t e = 0; e < ids.size(); ++e)
        if (gl::matches(g_tg->filter, g_tg->kind[e], ids[e], header_of(e))) g_tg->shown.push_back(e);

    // Keep the first visible entry (or its successor) at the top.
    const size_t pos = std::lower_bound(g_tg->shown.begin(), g_tg->shown.end(), top_entry) - g_tg->shown.begin();
    g_tg->top_row = static_cast<int>(pos / metrics().cols);
    g_tg->anchor = -1;
    update_scrollbar();
    update_status();
    InvalidateRect(g_tg->grid, nullptr, FALSE);
}

int edit_int(HWND e) {
    wchar_t b[32] = {};
    GetWindowTextW(e, b, 32);
    return std::max(0, _wtoi(b));
}

void read_controls() {
    gl::Filter& f = g_tg->filter;
    for (int k = 0; k < gl::kKindCount; ++k) f.kinds[k] = Button_GetCheck(g_tg->kinds[k]) == BST_CHECKED;
    f.min_side = edit_int(g_tg->min_edit);
    f.max_side = edit_int(g_tg->max_edit);
    f.exclude_icons = Button_GetCheck(g_tg->icons) == BST_CHECKED;
    f.alpha_only = Button_GetCheck(g_tg->alpha) == BST_CHECKED;
    const int len = GetWindowTextLengthW(g_tg->search);
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(g_tg->search, text.data(), len + 1);
    text.resize(static_cast<size_t>(len));
    gl::IdQuery q;
    std::string err;
    if (gl::parse_id_query(wide_to_utf8(text), q, err)) {
        f.ids = std::move(q);
        g_tg->filter_error.clear();
    } else {
        g_tg->filter_error = err;  // keep the last good query
    }
    const int sel = static_cast<int>(SendMessageW(g_tg->size_combo, CB_GETCURSEL, 0, 0));
    g_tg->box = kBoxes[std::clamp(sel, 0, 2)];
}

// ---- thumbnails ----------------------------------------------------------------

/// Ask for the thumbs of the cells in view, then a page below and above.
void request_visible() {
    const GridMetrics m = metrics();
    const long long first = static_cast<long long>(g_tg->top_row) * m.cols;
    const long long page = static_cast<long long>(m.visible_rows) * m.cols;
    std::vector<uint32_t> want;
    auto add = [&](long long from, long long to) {
        for (long long p = std::max(0LL, from); p < std::min<long long>(to, g_tg->shown.size()); ++p)
            if (!g_tg->cache.contains(g_tg->shown[p])) want.push_back(g_tg->shown[p]);
    };
    add(first, first + page);
    add(first + page, first + 2 * page);
    add(first - page, first);
    g_tg->sh->want(want);
}

void on_thumb(ThumbDone* raw) {
    std::unique_ptr<ThumbDone> d(raw);
    if (!g_tg) return;
    g_tg->cache.put(d->entry, std::move(d->thumb));
    InvalidateRect(g_tg->grid, nullptr, FALSE);
}

// ---- the grid ------------------------------------------------------------------

void paint_grid(HDC dc, const RECT& rc) {
    const GridMetrics m = metrics();
    FillRect(dc, &rc, theme_brush(kColPanel));
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_ui_font ? static_cast<HGDIOBJ>(g_ui_font) : GetStockObject(DEFAULT_GUI_FONT));
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    const int box = g_tg->box;
    for (int r = 0; r < m.visible_rows; ++r) {
        for (int c = 0; c < m.cols; ++c) {
            const size_t pos = static_cast<size_t>(g_tg->top_row + r) * m.cols + c;
            if (pos >= g_tg->shown.size()) break;
            const uint32_t e = g_tg->shown[pos];
            const int x = m.x0 + c * m.cell_w, y = r * m.cell_h;
            RECT cell{x + 2, y + 2, x + m.cell_w - 2, y + m.cell_h - 2};
            if (g_tg->selected.count(e)) FillRect(dc, &cell, theme_brush(kColAccent));
            else if (static_cast<int>(pos) == g_tg->anchor) FillRect(dc, &cell, theme_brush(kColHover));
            const int bx = x + (m.cell_w - box) / 2, by = y + kPad / 2;
            if (const gl::Thumb* t = g_tg->cache.get(e); t && t->width > 0) {
                // Downscale smoothly; small art scales up by a whole factor, unblurred.
                double s = std::min(static_cast<double>(box) / t->width, static_cast<double>(box) / t->height);
                if (s > 1.0) s = std::floor(s);
                const int dw = std::max(1, static_cast<int>(t->width * s)), dh = std::max(1, static_cast<int>(t->height * s));
                SetStretchBltMode(dc, s < 1.0 ? HALFTONE : COLORONCOLOR);
                SetBrushOrgEx(dc, 0, 0, nullptr);
                bi.bmiHeader.biWidth = t->width;
                bi.bmiHeader.biHeight = -t->height;
                StretchDIBits(dc, bx + (box - dw) / 2, by + (box - dh) / 2, dw, dh, 0, 0, t->width, t->height,
                              t->bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                RECT tr{bx, by, bx + box, by + box};
                FrameRect(dc, &tr, theme_brush(kColBorder));
                SetTextColor(dc, kColSubtle);
                DrawTextW(dc, t ? L"(no preview)" : L"\x2026", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            // Id, size, format: one line each.
            const gl::Header h = header_of(e);
            wchar_t lines[3][32] = {};
            swprintf(lines[0], 32, L"%u", g_tg->sh->ids[e]);
            if (h.known && h.image) {
                swprintf(lines[1], 32, L"%u\x00D7%u", h.width, h.height);
                swprintf(lines[2], 32, L"%hs", gl::format_label(h.format).c_str());
            }
            for (int i = 0; i < 3; ++i) {
                SetTextColor(dc, i == 0 || g_tg->selected.count(e) ? kColText : kColSubtle);
                RECT tr{x + 2, by + box + 2 + i * g_tg->line_h, x + m.cell_w - 2, by + box + 2 + (i + 1) * g_tg->line_h};
                DrawTextW(dc, lines[i], -1, &tr, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
    }
    if (g_tg->shown.empty()) {
        SetTextColor(dc, kColSubtle);
        RECT tr = rc;
        DrawTextW(dc,
                  g_tg->filter.needs_header() && g_tg->sh->known.load() < g_tg->sh->ids.size()
                      ? L"Nothing yet - reading texture headers..." : L"Nothing matches the filter.",
                  -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

int hit_test(int px, int py) {
    const GridMetrics m = metrics();
    if (px < m.x0 || px >= m.x0 + m.cols * m.cell_w || py < 0) return -1;
    const size_t pos = static_cast<size_t>(g_tg->top_row + py / m.cell_h) * m.cols + (px - m.x0) / m.cell_w;
    return pos < g_tg->shown.size() ? static_cast<int>(pos) : -1;
}

void show_in_preview(uint32_t entry) {
    if (!g_app || !g_app->dat_loaded) return;
    const uint32_t base = g_tg->sh->ids[entry];
    if (base == 0 || base - 1 >= g_app->data_gw2.mft_data_list.size()) return;
    TabCtrl_SetCurSel(g_app->hwnd_tab, static_cast<int>(MiddleTab::Preview));
    on_entry_selected(base - 1);
    relayout();
}

void on_click(int pos, bool ctrl, bool shift) {
    if (pos < 0) return;
    const uint32_t e = g_tg->shown[pos];
    if (shift && g_tg->anchor >= 0) {
        if (!ctrl) g_tg->selected.clear();
        const int a = std::min(g_tg->anchor, pos), b = std::max(g_tg->anchor, pos);
        for (int p = a; p <= b; ++p) g_tg->selected.insert(g_tg->shown[p]);
    } else if (ctrl) {
        if (!g_tg->selected.erase(e)) g_tg->selected.insert(e);
        g_tg->anchor = pos;
    } else {
        g_tg->selected.clear();
        g_tg->selected.insert(e);
        g_tg->anchor = pos;
    }
    InvalidateRect(g_tg->grid, nullptr, FALSE);
    update_status();
    show_in_preview(e);
}

void scroll_to(int row) {
    g_tg->top_row = row;
    update_scrollbar();
    InvalidateRect(g_tg->grid, nullptr, FALSE);
}

void copy_ids();
void export_selected();

LRESULT CALLBACK GridProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_tg || g_tg->grid != hwnd) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_grid(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        request_visible();
        return 0;
    }
    case WM_SIZE: update_scrollbar(); InvalidateRect(hwnd, nullptr, FALSE); return 0;
    case WM_VSCROLL: {
        const GridMetrics m = metrics();
        SCROLLINFO si{sizeof si};
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int row = g_tg->top_row;
        switch (LOWORD(wparam)) {
        case SB_LINEUP: row -= 1; break;
        case SB_LINEDOWN: row += 1; break;
        case SB_PAGEUP: row -= m.full_rows; break;
        case SB_PAGEDOWN: row += m.full_rows; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: row = si.nTrackPos; break;  // 32-bit, unlike HIWORD
        case SB_TOP: row = 0; break;
        case SB_BOTTOM: row = m.total_rows; break;
        }
        scroll_to(row);
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(g_tg->top_row - GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA * 2);
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        on_click(hit_test(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)), (wparam & MK_CONTROL) != 0,
                 (wparam & MK_SHIFT) != 0);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN: {
        const GridMetrics m = metrics();
        const bool ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wparam) {
        case 'A':
            if (ctrl) {
                g_tg->selected.insert(g_tg->shown.begin(), g_tg->shown.end());
                InvalidateRect(hwnd, nullptr, FALSE);
                update_status();
            }
            return 0;
        case 'C': if (ctrl) copy_ids(); return 0;
        case 'E': if (ctrl) export_selected(); return 0;
        case VK_ESCAPE:
            g_tg->selected.clear();
            InvalidateRect(hwnd, nullptr, FALSE);
            update_status();
            return 0;
        case VK_HOME: scroll_to(0); return 0;
        case VK_END: scroll_to(m.total_rows); return 0;
        case VK_PRIOR: scroll_to(g_tg->top_row - m.full_rows); return 0;
        case VK_NEXT: scroll_to(g_tg->top_row + m.full_rows); return 0;
        case VK_UP: scroll_to(g_tg->top_row - 1); return 0;
        case VK_DOWN: scroll_to(g_tg->top_row + 1); return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---- selection out -------------------------------------------------------------

void copy_ids() {
    if (g_tg->selected.empty()) {
        g_tg->export_note = L"select thumbnails first";
        update_status();
        return;
    }
    std::wstring text;
    for (uint32_t e : g_tg->selected) {  // entries are in base id order
        if (!text.empty()) text += L", ";
        text += std::to_wstring(g_tg->sh->ids[e]);
    }
    if (!OpenClipboard(g_tg->wnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        std::memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
    g_tg->export_note = L"copied " + std::to_wstring(g_tg->selected.size()) + L" ids";
    update_status();
}

void export_selected() {
    if (g_tg->exporting) return;
    if (g_tg->selected.empty()) {
        g_tg->export_note = L"select thumbnails first";
        update_status();
        return;
    }
    BROWSEINFOW bi{};
    bi.hwndOwner = g_tg->wnd;
    bi.lpszTitle = L"Export the selected textures (<id>.png) into:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t folder[MAX_PATH] = L"";
    const bool ok = SHGetPathFromIDListW(pidl, folder);
    CoTaskMemFree(pidl);
    if (!ok) return;
    if (g_tg->exporter.joinable()) g_tg->exporter.join();
    g_tg->exporting = true;
    EnableWindow(g_tg->export_btn, FALSE);
    std::vector<uint32_t> entries(g_tg->selected.begin(), g_tg->selected.end());
    g_tg->exporter = std::thread([sh = g_tg->sh, entries, dir = std::filesystem::path(folder)] {
        std::ifstream dat(sh->dat_path, std::ios::binary);
        size_t failed = 0;
        for (size_t i = 0; i < entries.size() && !sh->stop; ++i) {
            try {
                gl::Rgba img;
                if (gl::decode_image(gl::read_whole(dat, sh->mft[entries[i]]), 0, img)) {
                    exportgltf::sky::Image out{img.width, img.height, std::move(img.px)};
                    std::string err;
                    const auto path = dir / (std::to_string(sh->ids[entries[i]]) + ".png");
                    if (!exportgltf::sky::write_png(out, path.string(), err)) ++failed;
                } else {
                    ++failed;
                }
            } catch (const std::exception&) {
                ++failed;
            }
            PostMessageW(sh->notify, WM_APP_GALLERY_EXPORT, i + 1, static_cast<LPARAM>(entries.size()));
        }
        PostMessageW(sh->notify, WM_APP_GALLERY_EXPORT, entries.size() + 1 + failed, static_cast<LPARAM>(entries.size()));
    });
}

void on_export_progress(size_t done, size_t total) {
    wchar_t b[96];
    if (done <= total) {
        swprintf(b, 96, L"exporting %zu / %zu", done, total);
    } else {
        const size_t failed = done - total - 1;
        if (failed) swprintf(b, 96, L"exported %zu of %zu (%zu could not be decoded)", total - failed, total, failed);
        else swprintf(b, 96, L"exported %zu", total);
        g_tg->exporting = false;
        EnableWindow(g_tg->export_btn, TRUE);
    }
    g_tg->export_note = b;
    update_status();
}

// ---- headers on disk -----------------------------------------------------------

void save_header_file() {
    if (g_tg->header_file.empty()) return;
    const size_t known = g_tg->sh->known.load();
    if (known == g_tg->known_saved) return;
    std::vector<std::pair<uint32_t, uint64_t>> rows;
    rows.reserve(known);
    for (uint32_t e = 0; e < g_tg->sh->ids.size(); ++e)
        if (const uint64_t v = g_tg->sh->headers[e].load()) rows.emplace_back(g_tg->sh->ids[e], v);
    if (gl::save_headers(g_tg->header_file, g_tg->fingerprint, rows)) g_tg->known_saved = known;
}

void load_header_file() {
    std::vector<std::pair<uint32_t, uint64_t>> rows;
    if (!gl::load_headers(g_tg->header_file, g_tg->fingerprint, rows)) return;
    const auto& ids = g_tg->sh->ids;
    for (const auto& [id, v] : rows) {
        auto it = std::lower_bound(ids.begin(), ids.end(), id);
        if (it != ids.end() && *it == id) g_tg->sh->publish(static_cast<uint32_t>(it - ids.begin()), gl::Header::unpack(v));
    }
    g_tg->known_saved = g_tg->sh->known.load();
}

// ---- the window ----------------------------------------------------------------

void layout() {
    RECT rc;
    GetClientRect(g_tg->wnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int y2 = 36;
    int x = 8;
    // Row 2: the search box takes what the buttons leave.
    const int right = w - 8;
    MoveWindow(g_tg->copy_btn, right - 80, y2, 80, 24, TRUE);
    MoveWindow(g_tg->export_btn, right - 80 - 6 - 120, y2, 120, 24, TRUE);
    MoveWindow(g_tg->size_combo, right - 80 - 6 - 120 - 6 - 80, y2, 80, 200, TRUE);
    x = 60;
    MoveWindow(g_tg->search, x, y2, std::max(80, right - 80 - 6 - 120 - 6 - 80 - 8 - x), 24, TRUE);
    MoveWindow(g_tg->grid, 0, kTopBarH, w, std::max(10, h - kTopBarH - kStatusH), TRUE);
    MoveWindow(g_tg->status, 8, h - kStatusH + 3, w - 16, kStatusH - 4, TRUE);
}

void maybe_shoot() {
    if (g_tg->shot_path.empty()) return;
    // Wait for the UI art's headers (the scan's first part) and the cells in view.
    const GridMetrics m = metrics();
    const size_t first = static_cast<size_t>(g_tg->top_row) * m.cols;
    const size_t last = std::min(g_tg->shown.size(), first + static_cast<size_t>(m.full_rows) * m.cols);
    bool ready = !g_tg->shown.empty() && g_tg->known_at_filter == g_tg->sh->known.load();
    for (size_t p = first; ready && p < last; ++p) ready = g_tg->cache.contains(g_tg->shown[p]);
    size_t ui_left = 0;
    for (uint32_t e = 0; e < g_tg->kind.size(); ++e)
        if (g_tg->kind[e] == gl::Kind::Ui && !g_tg->sh->headers[e].load()) ++ui_left;
    if ((!ready || ui_left) && ++g_tg->shot_wait < 2400) return;  // up to ~12 minutes
    RECT rc;
    GetWindowRect(g_tg->wnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_tg->wnd, mem, 2 /* PW_RENDERFULLCONTENT */);
    exportgltf::sky::Image img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
    const auto* src = static_cast<const uint8_t*>(bits);
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        img.rgba[i] = src[i + 2];
        img.rgba[i + 1] = src[i + 1];
        img.rgba[i + 2] = src[i];
        img.rgba[i + 3] = 255;
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    std::string err;
    exportgltf::sky::write_png(img, g_tg->shot_path, err);
    if (FILE* f = std::fopen((g_tg->shot_path + ".txt").c_str(), "wb")) {
        std::fprintf(f, "shown=%zu total=%zu headers=%zu ready=%d ui_left=%zu\n", g_tg->shown.size(),
                     g_tg->sh->ids.size(), g_tg->sh->known.load(), ready ? 1 : 0, ui_left);
        std::fclose(f);
    }
    g_tg->shot_path.clear();
    if (castlemist::core::env::is_set("GW2_GALLERY_QUIT") && g_app) PostMessageW(g_app->hwnd_main, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK GalleryWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_tg || g_tg->wnd != hwnd) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
        mm->ptMinTrackSize = {720, 360};
        return 0;
    }
    case WM_COMMAND: {
        const UINT id = LOWORD(wparam), code = HIWORD(wparam);
        if ((id >= ID_TG_KIND0 && id < ID_TG_KIND0 + gl::kKindCount) || id == ID_TG_ICONS || id == ID_TG_ALPHA) {
            read_controls();
            refilter();
        } else if ((id == ID_TG_MIN || id == ID_TG_MAX || id == ID_TG_SEARCH) && code == EN_CHANGE) {
            SetTimer(hwnd, TIMER_TG_EDIT, 300, nullptr);
        } else if (id == ID_TG_SIZE && code == CBN_SELCHANGE) {
            read_controls();
            refilter();
        } else if (id == ID_TG_EXPORT) {
            export_selected();
        } else if (id == ID_TG_COPY) {
            copy_ids();
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_TG_EDIT) {
            KillTimer(hwnd, TIMER_TG_EDIT);
            read_controls();
            refilter();
        } else if (wparam == TIMER_TG_POLL) {
            const size_t known = g_tg->sh->known.load();
            if (g_tg->filter.needs_header() && known != g_tg->known_at_filter) refilter();
            else update_status();
            if (known == g_tg->sh->ids.size()) save_header_file();
            maybe_shoot();
        }
        return 0;
    case WM_APP_GALLERY_THUMB: on_thumb(reinterpret_cast<ThumbDone*>(lparam)); return 0;
    case WM_APP_GALLERY_EXPORT: on_export_progress(wparam, static_cast<size_t>(lparam)); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, TIMER_TG_POLL);
        KillTimer(hwnd, TIMER_TG_EDIT);
        g_tg->sh->stop = true;
        g_tg->sh->cv.notify_all();
        for (std::thread& t : g_tg->workers) t.join();
        if (g_tg->exporter.joinable()) g_tg->exporter.join();
        save_header_file();
        // Drop thumbs and exports still queued for this window.
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_APP_GALLERY_THUMB, WM_APP_GALLERY_THUMB, PM_REMOVE))
            delete reinterpret_cast<ThumbDone*>(m.lParam);
        delete g_tg;
        g_tg = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// The search box is one line, but a pasted list is many: paste it as one.
LRESULT CALLBACK SearchSubclass(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (msg == WM_PASTE && OpenClipboard(hwnd)) {
        std::wstring text;
        if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                text = p;
                GlobalUnlock(h);
            }
        }
        CloseClipboard();
        for (wchar_t& c : text)
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
        SendMessageW(hwnd, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(text.c_str()));
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

bool build_source(Gallery& g, std::wstring& why) {
    if (!g_app || !g_app->index_loaded || !castlemist::db::is_open()) {
        why = L"Open the index first (File > Open Index DB): the gallery lists its image entries.";
        return false;
    }
    if (!g_app->dat_loaded) {
        why = L"Open Gw2.dat first: the thumbnails are decoded from it.";
        return false;
    }
    auto sh = std::make_shared<Shared>();
    sh->dat_path = g_app->data_gw2.file_info.file_path;
    const auto& mft = g_app->data_gw2.mft_data_list;
    castlemist::db::load_magics({std::begin(kTypes), std::end(kTypes)},
                                [&](uint32_t base, const char* magic, const char* type) {
                                    if (base == 0 || base - 1 >= mft.size()) return;
                                    sh->ids.push_back(base);
                                    sh->mft.push_back(mft[base - 1]);
                                    g.kind.push_back(gl::kind_of(magic, type));
                                });
    if (sh->ids.empty()) {
        why = L"The index lists no image entries.";
        return false;
    }
    sh->headers = std::make_unique<std::atomic<uint64_t>[]>(sh->ids.size());
    // UI art first (what the gallery is mostly for), then web, DDS and the world textures.
    for (gl::Kind k : {gl::Kind::Ui, gl::Kind::Web, gl::Kind::Dds, gl::Kind::World})
        for (uint32_t e = 0; e < g.kind.size(); ++e)
            if (g.kind[e] == k) sh->scan_order.push_back(e);
    g.sh = std::move(sh);
    g.fingerprint = castlemist::db::fingerprint_of(g_app->data_gw2).to_string();
    g.header_file = std::filesystem::path(castlemist::db::path()).parent_path() / L"gw2_texture_headers.bin";
    return true;
}

}  // namespace

void close_texture_gallery() {
    if (g_tg) DestroyWindow(g_tg->wnd);
}

void open_texture_gallery(HWND owner, const char* preset) {
    if (g_tg) {
        ShowWindow(g_tg->wnd, SW_RESTORE);
        SetForegroundWindow(g_tg->wnd);
        return;
    }
    auto g = std::make_unique<Gallery>();
    std::wstring why;
    if (!build_source(*g, why)) {
        MessageBoxW(owner, why.c_str(), L"Texture Gallery", MB_ICONINFORMATION);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = GalleryWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWndClass;
        RegisterClassW(&wc);
        wc.lpfnWndProc = GridProc;
        wc.hbrBackground = nullptr;
        wc.style = CS_DBLCLKS;
        wc.lpszClassName = kGridClass;
        RegisterClassW(&wc);
        registered = true;
    }
    g_tg = g.release();
    // Not owned by the main window, like the Map window: an owned window stays
    // above its owner, and would cover the preview a click opens there.
    g_tg->wnd = CreateWindowExW(0, kWndClass, L"Texture Gallery", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1180, 820, nullptr, nullptr, g_hinstance, nullptr);
    if (!g_tg->wnd) {
        delete g_tg;
        g_tg = nullptr;
        return;
    }
    g_tg->sh->notify = g_tg->wnd;
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, UINT_PTR id,
                  DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_tg->wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    int x = 8;
    const int y = 8, h = 22;
    auto next = [&](int w) { int at = x; x += w + 6; return at; };
    mk(L"STATIC", L"Kinds:", SS_LEFT, next(40), y + 4, 40, 16, 0);
    const wchar_t* kind_names[gl::kKindCount] = {L"UI (ATEU)", L"World (ATEX/ATEP)", L"Web / launcher", L"DDS"};
    const int kind_w[gl::kKindCount] = {78, 122, 102, 48};
    for (int k = 0; k < gl::kKindCount; ++k)
        g_tg->kinds[k] = mk(L"BUTTON", kind_names[k], BS_AUTOCHECKBOX | WS_TABSTOP, next(kind_w[k]), y, kind_w[k], h,
                            ID_TG_KIND0 + k);
    x += 14;
    mk(L"STATIC", L"Longest side:", SS_LEFT, next(72), y + 4, 72, 16, 0);
    g_tg->min_edit = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, next(48), y, 48, h, ID_TG_MIN);
    mk(L"STATIC", L"to", SS_CENTER, next(14), y + 4, 14, 16, 0);
    g_tg->max_edit = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | WS_TABSTOP, next(48), y, 48, h, ID_TG_MAX);
    x += 14;
    g_tg->icons = mk(L"BUTTON", L"Exclude square icon sizes (16-128)", BS_AUTOCHECKBOX | WS_TABSTOP, next(200), y, 200,
                     h, ID_TG_ICONS);
    g_tg->alpha = mk(L"BUTTON", L"Has alpha", BS_AUTOCHECKBOX | WS_TABSTOP, next(80), y, 80, h, ID_TG_ALPHA);
    mk(L"STATIC", L"IDs:", SS_LEFT, 8, 40, 40, 16, 0);
    g_tg->search = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 60, 36, 400, 24, ID_TG_SEARCH);
    SendMessageW(g_tg->search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"an id, a range (21900-22100) or a pasted list"));
    SetWindowSubclass(g_tg->search, SearchSubclass, 1, 0);
    g_tg->size_combo = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 36, 80, 200, ID_TG_SIZE);
    for (const wchar_t* s : {L"64 px", L"96 px", L"128 px"})
        SendMessageW(g_tg->size_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(g_tg->size_combo, CB_SETCURSEL, 1, 0);
    g_tg->export_btn = mk(L"BUTTON", L"Export selected...", BS_PUSHBUTTON | WS_TABSTOP, 0, 36, 120, 24, ID_TG_EXPORT);
    g_tg->copy_btn = mk(L"BUTTON", L"Copy IDs", BS_PUSHBUTTON | WS_TABSTOP, 0, 36, 80, 24, ID_TG_COPY);
    g_tg->grid = mk(kGridClass, L"", WS_VSCROLL | WS_TABSTOP, 0, kTopBarH, 100, 100, ID_TG_GRID, WS_EX_CLIENTEDGE);
    g_tg->status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 8, 0, 100, 18, 0);

    Button_SetCheck(g_tg->kinds[0], BST_CHECKED);
    if (preset && std::string(preset) == "ui-candidates") {
        // The window-art survey's filter: UI art, no square icons, alpha formats, at most 1024.
        Button_SetCheck(g_tg->icons, BST_CHECKED);
        Button_SetCheck(g_tg->alpha, BST_CHECKED);
        SetWindowTextW(g_tg->max_edit, L"1024");
        KillTimer(g_tg->wnd, TIMER_TG_EDIT);
        g_tg->shot_path = castlemist::core::env::text("GW2_GALLERY_SHOT");
    }

    {
        HDC dc = GetDC(g_tg->grid);
        HGDIOBJ old = SelectObject(dc, g_ui_font ? static_cast<HGDIOBJ>(g_ui_font) : GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(g_tg->grid, dc);
        g_tg->line_h = std::max(12, static_cast<int>(tm.tmHeight));
    }
    load_header_file();
    read_controls();
    layout();
    refilter();

    const unsigned hw = std::thread::hardware_concurrency();
    const int n = std::clamp(static_cast<int>(hw) - 1, 2, 6);
    for (int i = 0; i < n; ++i) g_tg->workers.emplace_back(worker_main, g_tg->sh);
    SetTimer(g_tg->wnd, TIMER_TG_POLL, 300, nullptr);
    ShowWindow(g_tg->wnd, SW_SHOW);
    SetFocus(g_tg->grid);
}

}  // namespace castlemist::ui
