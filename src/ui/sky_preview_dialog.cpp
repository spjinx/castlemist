/// @file
/// @brief Export Skybox (Map): a modal preview of what the bake puts below the
///        horizon (fade to black / white, or the game's stretched horizon
///        skirt) before the folder is written.

#include "detail/app_state.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "castlemist/exportgltf/sky_export.h"

namespace castlemist::ui {
namespace {

namespace sky = castlemist::exportgltf::sky;

constexpr UINT_PTR ID_SP_BLACK = 2400;
constexpr UINT_PTR ID_SP_WHITE = 2401;
constexpr UINT_PTR ID_SP_SKIRT = 2402;
constexpr UINT_PTR ID_SP_FADE = 2403;
constexpr UINT_PTR ID_SP_EXPORT = 2404;
constexpr UINT_PTR ID_SP_CANCEL = 2405;
constexpr int kPrevW = 768, kPrevH = 384;   // equirect preview, 2:1
constexpr float kPi = 3.14159265358979323846f;

struct PreviewUi {
    HWND wnd = nullptr, fade = nullptr, fadeLabel = nullptr;
    sky::Image raw;               // equirect of the mode as the game draws it (skirt below the horizon)
    std::vector<float> rowY;      // per row: the Unity direction's y (only y sets the fill weight)
    std::vector<uint8_t> bgra;    // composed preview, top-down
    sky::GroundOptions ground;
    bool done = false, accepted = false;
};

PreviewUi* g_sp = nullptr;

// Remembered between exports.
sky::GroundOptions g_last_ground;

void compose() {
    PreviewUi& p = *g_sp;
    p.bgra.resize(static_cast<size_t>(kPrevW) * kPrevH * 4);
    for (int y = 0; y < kPrevH; ++y) {
        const float d[3] = {0, p.rowY[y], 0};
        const float w = sky::ground_weight(d, p.ground.fadeDegrees);
        for (int x = 0; x < kPrevW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kPrevW + x) * 4;
            const uint8_t* s = &p.raw.rgba[i];
            const sky::Rgb c = sky::apply_ground(sky::Rgb{s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f}, w,
                                                 p.ground.fill);
            auto b = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            p.bgra[i + 0] = b(c.b);
            p.bgra[i + 1] = b(c.g);
            p.bgra[i + 2] = b(c.r);
            p.bgra[i + 3] = 255;
        }
    }
}

void refresh() {
    const int deg = static_cast<int>(SendMessageW(g_sp->fade, TBM_GETPOS, 0, 0));
    g_sp->ground.fadeDegrees = static_cast<float>(deg);
    SetWindowTextW(g_sp->fadeLabel, (L"Fade band: " + std::to_wstring(deg) + L"° below the horizon").c_str());
    EnableWindow(g_sp->fade, g_sp->ground.fill != sky::GroundFill::Skirt);
    compose();
    RECT r{10, 10, 10 + kPrevW, 10 + kPrevH};
    InvalidateRect(g_sp->wnd, &r, FALSE);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_sp && !g_sp->bgra.empty()) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = kPrevW;
            bi.bmiHeader.biHeight = -kPrevH;   // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            SetDIBitsToDevice(dc, 10, 10, kPrevW, kPrevH, 0, 0, 0, kPrevH, g_sp->bgra.data(), &bi, DIB_RGB_COLORS);
            // Horizon guide.
            HPEN pen = CreatePen(PS_DOT, 1, RGB(255, 64, 64));
            HGDIOBJ old = SelectObject(dc, pen);
            SetBkMode(dc, TRANSPARENT);
            MoveToEx(dc, 10, 10 + kPrevH / 2, nullptr);
            LineTo(dc, 10 + kPrevW, 10 + kPrevH / 2);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_HSCROLL:
        if (g_sp && reinterpret_cast<HWND>(lparam) == g_sp->fade) refresh();
        return 0;
    case WM_COMMAND:
        if (!g_sp) break;
        switch (LOWORD(wparam)) {
        case ID_SP_BLACK: g_sp->ground.fill = sky::GroundFill::Black; refresh(); return 0;
        case ID_SP_WHITE: g_sp->ground.fill = sky::GroundFill::White; refresh(); return 0;
        case ID_SP_SKIRT: g_sp->ground.fill = sky::GroundFill::Skirt; refresh(); return 0;
        case IDOK:
        case ID_SP_EXPORT: g_sp->accepted = true; DestroyWindow(hwnd); return 0;
        case IDCANCEL:
        case ID_SP_CANCEL: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_sp) g_sp->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

bool run_sky_ground_preview(HWND owner, const sky::SkyInputs& in, sky::GroundOptions& ground) {
    // The first mode that bakes: that is what the preview shows.
    const auto& s = in.sky;
    size_t mode = s.modes.size();
    sky::BakeResult bake;
    for (size_t j = 0; j < s.modes.size() && mode == s.modes.size(); ++j) {
        if (!s.modes[j].hasPanorama()) continue;
        bake = sky::make_sky_sampler(s, j, in.textures, &in.stars);
        if (bake.ok) mode = j;
    }
    if (mode == s.modes.size()) {   // nothing will be baked: nothing to preview
        ground = g_last_ground;
        return true;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpfnWndProc = PreviewProc;
        wc.lpszClassName = L"Gw2SkyPreviewWnd";
        RegisterClassW(&wc);
        registered = true;
    }

    PreviewUi ui;
    ui.ground = g_last_ground;
    {
        HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        ui.raw = sky::render_equirect(bake.radiance, kPrevW, kPrevH);
        SetCursor(old);
    }
    ui.rowY.resize(kPrevH);
    for (int y = 0; y < kPrevH; ++y) ui.rowY[y] = std::cos((y + 0.5f) / kPrevH * kPi);   // as render_equirect
    g_sp = &ui;

    const int W = kPrevW + 36, H = kPrevH + 170;
    const std::wstring title = L"Skybox below the horizon - preview: " + utf8_to_wide(sky::mode_name(mode));
    ui.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Gw2SkyPreviewWnd", title.c_str(),
                             WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner, nullptr,
                             g_hinstance, nullptr);
    if (!ui.wnd) {
        g_sp = nullptr;
        ground = g_last_ground;
        return true;
    }
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, ui.wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    const int y0 = 20 + kPrevH;
    mk(L"STATIC",
       L"The game repeats the horizon row below the horizon (terrain hides it in game). "
       L"Choose what the skybox shows there instead; the red line is the horizon.",
       SS_LEFT, 10, y0, kPrevW, 32, 0);
    HWND rb = mk(L"BUTTON", L"Fade to black", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, 10, y0 + 38, 120, 22, ID_SP_BLACK);
    HWND rw = mk(L"BUTTON", L"Fade to white", BS_AUTORADIOBUTTON, 140, y0 + 38, 120, 22, ID_SP_WHITE);
    HWND rs = mk(L"BUTTON", L"Stretched horizon (as the game)", BS_AUTORADIOBUTTON, 270, y0 + 38, 220, 22, ID_SP_SKIRT);
    SendMessageW(ui.ground.fill == sky::GroundFill::White   ? rw
                 : ui.ground.fill == sky::GroundFill::Skirt ? rs
                                                            : rb,
                 BM_SETCHECK, BST_CHECKED, 0);
    ui.fadeLabel = mk(L"STATIC", L"", SS_LEFT, 10, y0 + 72, 220, 18, 0);
    ui.fade = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP | WS_GROUP, 230, y0 + 66, 300, 28, ID_SP_FADE);
    SendMessageW(ui.fade, TBM_SETRANGE, TRUE, MAKELPARAM(0, 90));
    SendMessageW(ui.fade, TBM_SETPOS, TRUE, static_cast<LPARAM>(std::lround(ui.ground.fadeDegrees)));
    mk(L"BUTTON", L"Export", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, W - 206, H - 72, 90, 28, ID_SP_EXPORT);
    mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, W - 108, H - 72, 80, 28, ID_SP_CANCEL);
    refresh();

    EnableWindow(owner, FALSE);
    ShowWindow(ui.wnd, SW_SHOW);
    MSG m;
    while (!ui.done && GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(ui.wnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (!ui.done && m.message == WM_QUIT) PostQuitMessage(static_cast<int>(m.wParam));
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    g_sp = nullptr;

    if (ui.accepted) {
        g_last_ground = ui.ground;
        ground = ui.ground;
    }
    return ui.accepted;
}

} // namespace castlemist::ui
