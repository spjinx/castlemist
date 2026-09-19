/// @file
/// @brief The UV map viewer popup: a flat 2D wireframe of the loaded model's
///        UV0 layout, following whichever submesh the main "Submesh" combo has
///        selected -- for cross-referencing against Blender's UV editor when a
///        glTF export doesn't look like castlemist's own preview.

#include "detail/app_state.h"

#include "castlemist/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <vector>

namespace castlemist::ui {

HWND g_uv_wnd = nullptr;
HWND g_uv_title = nullptr;
HWND g_uv_close = nullptr;

constexpr int kUvHeaderH = 36;

// Cycled per submesh when "All submeshes" is selected -- distinct enough to
// tell adjacent UV islands apart on the dark plot background.
constexpr COLORREF kUvPalette[] = {
    RGB(0x4C, 0x93, 0xFF), RGB(0xFF, 0x8A, 0x3D), RGB(0x5B, 0xD6, 0x7A), RGB(0xE0, 0x5C, 0xC6),
    RGB(0xF2, 0xD3, 0x4A), RGB(0x6D, 0xE0, 0xDA), RGB(0xE0, 0x5C, 0x5C), RGB(0xA0, 0x7C, 0xF0),
};
constexpr int kUvPaletteN = static_cast<int>(sizeof(kUvPalette) / sizeof(kUvPalette[0]));

// model.meshes indices that actually contribute a submesh slot in the 3D
// view -- castlemist::render::set_model() (geometry.cpp) skips any mesh whose
// LOD0 index list is empty, so "submesh k" as lod_target_submesh() numbers it
// is the k-th mesh HERE meeting that same condition, not model.meshes[k]
// directly. Kept in sync with that skip rule rather than exposing g_subs.
std::vector<int> uv_valid_mesh_indices(const ModelPreview& model) {
    std::vector<int> out;
    for (size_t i = 0; i < model.meshes.size(); ++i)
        if (!model.meshes[i].indices.empty()) out.push_back(static_cast<int>(i));
    return out;
}

const ModelPreview* uv_current_model() {
    if (!g_app || g_app->current_entry.kind != PreviewKind::Model) return nullptr;
    return g_app->current_entry.model.get();
}

void uv_refresh_title() {
    if (!g_uv_title) return;
    const ModelPreview* model = uv_current_model();
    if (!model || model->meshes.empty()) {
        SetWindowTextW(g_uv_title, L"No model loaded.");
        return;
    }
    std::vector<int> valid = uv_valid_mesh_indices(*model);
    int sel = lod_target_submesh();
    wchar_t buf[200];
    if (sel < 0 || sel >= static_cast<int>(valid.size())) {
        swprintf(buf, 200, L"All submeshes (%zu)", valid.size());
    } else {
        const ModelMeshCPU& mesh = model->meshes[static_cast<size_t>(valid[static_cast<size_t>(sel)])];
        swprintf(buf, 200, L"Submesh %d -- mat %u, %zu tris", sel, mesh.materialIndex, mesh.indices.size() / 3);
    }
    SetWindowTextW(g_uv_title, buf);
}

// Called whenever the loaded model or the submesh selection changes (a new
// entry loads, or the main window's Submesh combo fires CBN_SELCHANGE) so the
// popup -- if open -- never shows a stale layout. A no-op when it isn't open.
void uv_map_notify_changed() {
    if (!g_uv_wnd) return;
    uv_refresh_title();
    InvalidateRect(g_uv_wnd, nullptr, FALSE);
}

void uv_paint(HWND hwnd, HDC hdc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT plot = {0, kUvHeaderH, rc.right, rc.bottom};
    LONG plotW = std::max<LONG>(1, plot.right - plot.left);
    LONG plotH = std::max<LONG>(1, plot.bottom - plot.top);

    // Double-buffer: this repaints on every resize tick, and drawing straight
    // to the window DC flickers visibly at that rate.
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(mem, bmp));
    FillRect(mem, &rc, theme_brush(kColBg));

    const ModelPreview* model = uv_current_model();
    if (!model || model->meshes.empty()) {
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, kColSubtle);
        HFONT f = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT oldF = static_cast<HFONT>(SelectObject(mem, f));
        DrawTextW(mem, L"No model loaded.", -1, &plot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(mem, oldF);
    } else {
        std::vector<int> valid = uv_valid_mesh_indices(*model);
        int sel = lod_target_submesh(); // -1 = all, else index into `valid`

        std::vector<std::pair<int, int>> toDraw; // (model.meshes index, palette index)
        if (sel < 0) {
            for (size_t k = 0; k < valid.size(); ++k) toDraw.push_back({valid[k], static_cast<int>(k)});
        } else if (sel < static_cast<int>(valid.size())) {
            toDraw.push_back({valid[static_cast<size_t>(sel)], 0});
        }

        // Bounding box across every UV this pass draws, ALWAYS including the
        // [0,1] unit square -- tiling UVs (common on hull/trim materials)
        // legitimately run outside it, and seeing how far by eye is the point.
        float lo[2] = {0, 0}, hi[2] = {1, 1};
        for (const auto& mc : toDraw)
            for (const auto& v : model->meshes[static_cast<size_t>(mc.first)].vertices) {
                lo[0] = std::min(lo[0], v.u); hi[0] = std::max(hi[0], v.u);
                lo[1] = std::min(lo[1], v.v); hi[1] = std::max(hi[1], v.v);
            }

        constexpr int kMargin = 20;
        float spanU = std::max(1e-4f, hi[0] - lo[0]), spanV = std::max(1e-4f, hi[1] - lo[1]);
        float availW = static_cast<float>(std::max<LONG>(1, plotW - 2 * kMargin));
        float availH = static_cast<float>(std::max<LONG>(1, plotH - 2 * kMargin));
        float scale = std::min(availW / spanU, availH / spanV);
        float offX = static_cast<float>(plot.left) + kMargin + (availW - spanU * scale) * 0.5f;
        float offY = static_cast<float>(plot.top) + kMargin + (availH - spanV * scale) * 0.5f;
        // V grows downward in both GW2's UV convention and this window's client
        // coordinates, so no flip is needed to match a texture viewed right-way-up.
        auto toPx = [&](float u, float v) {
            return POINT{static_cast<LONG>(std::lround(offX + (u - lo[0]) * scale)),
                         static_cast<LONG>(std::lround(offY + (v - lo[1]) * scale))};
        };

        HPEN gridPen = CreatePen(PS_DOT, 1, kColBorder);
        HPEN oldPen = static_cast<HPEN>(SelectObject(mem, gridPen));
        POINT sq[5] = {toPx(0, 0), toPx(1, 0), toPx(1, 1), toPx(0, 1), toPx(0, 0)};
        Polyline(mem, sq, 5);
        SelectObject(mem, oldPen);
        DeleteObject(gridPen);

        for (const auto& mc : toDraw) {
            const ModelMeshCPU& mesh = model->meshes[static_cast<size_t>(mc.first)];
            HPEN pen = CreatePen(PS_SOLID, 1, kUvPalette[mc.second % kUvPaletteN]);
            HPEN old2 = static_cast<HPEN>(SelectObject(mem, pen));
            for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
                uint32_t ia = mesh.indices[t], ib = mesh.indices[t + 1], ic = mesh.indices[t + 2];
                if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) continue;
                POINT tri[4] = {toPx(mesh.vertices[ia].u, mesh.vertices[ia].v),
                                toPx(mesh.vertices[ib].u, mesh.vertices[ib].v),
                                toPx(mesh.vertices[ic].u, mesh.vertices[ic].v),
                                toPx(mesh.vertices[ia].u, mesh.vertices[ia].v)};
                Polyline(mem, tri, 4);
            }
            SelectObject(mem, old2);
            DeleteObject(pen);
        }
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void uv_relayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right;
    MoveWindow(g_uv_title, 10, 8, std::max(0, w - 100), 20, TRUE);
    MoveWindow(g_uv_close, std::max(10, w - 80), 5, 70, 26, TRUE);
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK UvMapWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_UV_CLOSE) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1; // uv_paint fills the whole client rect itself -- no flicker gap.
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        uv_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        uv_relayout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = 260;
        mmi->ptMinTrackSize.y = 220;
        return 0;
    }
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_uv_wnd = g_uv_title = g_uv_close = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void open_uv_map_viewer(HWND owner) {
    if (g_uv_wnd) { // already open -> just bring it forward and refresh
        SetForegroundWindow(g_uv_wnd);
        uv_map_notify_changed();
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = UvMapWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2UvMapWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 520, H = 520;
    g_uv_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2UvMapWnd", L"UV Map", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, W, H, owner, nullptr, g_hinstance, nullptr);
    if (!g_uv_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    g_uv_title = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, g_uv_wnd, nullptr,
                                 g_hinstance, nullptr);
    SendMessageW(g_uv_title, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    g_uv_close = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, g_uv_wnd,
                                 reinterpret_cast<HMENU>(ID_UV_CLOSE), g_hinstance, nullptr);
    SendMessageW(g_uv_close, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    uv_refresh_title();
    uv_relayout(g_uv_wnd);
    ShowWindow(g_uv_wnd, SW_SHOW);
}

} // namespace castlemist::ui
