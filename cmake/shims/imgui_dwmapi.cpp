// imgui_impl_win32.cpp calls three dwmapi functions, all from
// ImGui_ImplWin32_EnableAlphaCompositing(), which castlemist never calls. The
// exe must not import dwmapi.dll (see the ui layer in CMakeLists.txt), so
// instead of linking it, the backend's import slots are filled here with
// stand-ins that resolve the real functions at run time.

#include <windows.h>
#include <dwmapi.h>

namespace {

FARPROC dwm(const char* name) {
    static HMODULE module = LoadLibraryW(L"dwmapi.dll");
    return module ? GetProcAddress(module, name) : nullptr;
}

HRESULT WINAPI is_composition_enabled(BOOL* enabled) {
    using Fn = HRESULT(WINAPI*)(BOOL*);
    if (auto f = reinterpret_cast<Fn>(dwm("DwmIsCompositionEnabled"))) return f(enabled);
    *enabled = FALSE;
    return E_NOTIMPL;
}

HRESULT WINAPI get_colorization_color(DWORD* color, BOOL* opaque) {
    using Fn = HRESULT(WINAPI*)(DWORD*, BOOL*);
    if (auto f = reinterpret_cast<Fn>(dwm("DwmGetColorizationColor"))) return f(color, opaque);
    return E_NOTIMPL;
}

HRESULT WINAPI enable_blur_behind_window(HWND hwnd, const DWM_BLURBEHIND* blur) {
    using Fn = HRESULT(WINAPI*)(HWND, const DWM_BLURBEHIND*);
    if (auto f = reinterpret_cast<Fn>(dwm("DwmEnableBlurBehindWindow"))) return f(hwnd, blur);
    return E_NOTIMPL;
}

} // namespace

// The import-table slots dwmapi.h's __declspec(dllimport) declarations read.
extern "C" {
decltype(&is_composition_enabled) __imp_DwmIsCompositionEnabled = is_composition_enabled;
decltype(&get_colorization_color) __imp_DwmGetColorizationColor = get_colorization_color;
decltype(&enable_blur_behind_window) __imp_DwmEnableBlurBehindWindow = enable_blur_behind_window;
}
