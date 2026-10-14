/// @file
/// @brief Tools > Map Window...: the fileId prompt that opens the Map window
///        (map_window.cpp), defaulting to the selected entry when it is a map.

#include "detail/app_state.h"
#include "detail/map_window.h"

#include <cwchar>

namespace castlemist::ui {

namespace {

/// The selected entry's fileId when it is a map (lowest fileId of its baseId), else 0.
uint32_t selected_map_file_id() {
    if (!g_app || !g_app->dat_loaded || !g_app->has_loaded_entry) return 0;
    if (g_app->current_entry.kind != PreviewKind::Map) return 0;
    const uint32_t base = g_app->current_mft_index + 1;   // baseId = MFT index + 1
    uint32_t best = 0;
    for (const MftFileIdData& e : g_app->data_gw2.mft_file_id_data_list)
        if (e.base_id == base && (best == 0 || e.file_id < best)) best = e.file_id;
    return best;
}

struct Prompt {
    HWND wnd = nullptr, edit = nullptr;
    bool done = false, ok = false;
    uint32_t value = 0;
};
Prompt* g_prompt = nullptr;

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Prompt* p = g_prompt;
    switch (msg) {
    case WM_COMMAND:
        if (!p) break;
        if (LOWORD(wparam) == IDOK) {
            wchar_t buf[32] = {};
            GetWindowTextW(p->edit, buf, 32);
            wchar_t* end = nullptr;
            const unsigned long v = wcstoul(buf, &end, 10);
            if (end == buf || v == 0 || v > 0xFFFFFFFFul) {
                MessageBeep(MB_ICONWARNING);
                SetFocus(p->edit);
                return 0;
            }
            p->value = static_cast<uint32_t>(v);
            p->ok = true;
            p->done = true;
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            p->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        if (p) p->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

/// A small modal prompt for a map fileId. False on cancel.
bool ask_file_id(HWND owner, uint32_t& io) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = PromptProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2MapWindowPrompt";
        RegisterClassW(&wc);
        registered = true;
    }
    Prompt p;
    g_prompt = &p;
    RECT r{0, 0, 340, 108};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, style, FALSE, WS_EX_DLGMODALFRAME);
    RECT orc{};
    GetWindowRect(owner, &orc);
    const int w = r.right - r.left, h = r.bottom - r.top;
    p.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Gw2MapWindowPrompt", L"Map Window", style,
                            orc.left + (orc.right - orc.left - w) / 2, orc.top + (orc.bottom - orc.top - h) / 2, w, h,
                            owner, nullptr, g_hinstance, nullptr);
    if (!p.wnd) {
        g_prompt = nullptr;
        return false;
    }
    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int cw, int ch, UINT_PTR id,
                  DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, cw, ch, p.wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    mk(L"STATIC", L"Map fileId (e.g. 192711 Queensdale, 191000 Lion's Arch):", SS_LEFT, 12, 12, 316, 18, 0);
    wchar_t init[16] = L"";
    if (io) swprintf(init, 16, L"%u", io);
    p.edit = mk(L"EDIT", init, ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, 12, 34, 316, 22, 0, WS_EX_CLIENTEDGE);
    mk(L"BUTTON", L"Open", BS_DEFPUSHBUTTON | WS_TABSTOP, 166, 70, 78, 26, IDOK);
    mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, 250, 70, 78, 26, IDCANCEL);
    SendMessageW(p.edit, EM_SETSEL, 0, -1);

    EnableWindow(owner, FALSE);
    ShowWindow(p.wnd, SW_SHOW);
    SetFocus(p.edit);
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && (msg.hwnd == p.edit || msg.hwnd == p.wnd)) {
            SendMessageW(p.wnd, WM_COMMAND, IDOK, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            p.done = true;
            continue;
        }
        if (!IsDialogMessageW(p.wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (msg.message == WM_QUIT) break;
    }
    if (msg.message == WM_QUIT) PostQuitMessage(static_cast<int>(msg.wParam));
    EnableWindow(owner, TRUE);
    DestroyWindow(p.wnd);
    SetForegroundWindow(owner);
    g_prompt = nullptr;
    if (p.ok) io = p.value;
    return p.ok;
}

} // namespace

void prompt_map_window(HWND owner) {
    if (!g_app || !g_app->dat_loaded) {
        MessageBoxW(owner, L"Open the .dat first (File > Open .dat...).", L"Map Window", MB_ICONINFORMATION);
        return;
    }
    uint32_t id = selected_map_file_id();
    if (!ask_file_id(owner, id)) return;
    open_map_window(owner, id);
}

} // namespace castlemist::ui
