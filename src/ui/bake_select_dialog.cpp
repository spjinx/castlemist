/// @file
/// @brief The atlas-bake material selection popup ("Export glTF (Model, Baked
///        UV Atlas)..." now asks first): a checked list of the model's
///        materials, so baking can be applied selectively rather than
///        all-or-nothing.
///
/// Baking is a one-way trip -- it replaces a material's texture and mesh
/// geometry with a flattened, no-longer-editable result (see
/// bake_model_atlas's own doc comment). Some materials benefit from that
/// (trim sheets whose own UVs have no meaningful single-texture reading);
/// others are perfectly fine already and are better left alone, editable,
/// with their original textures -- there is no one right answer per model, so
/// this lets the user decide per material instead of guessing for them.

#include "detail/app_state.h"

#include "castlemist/core/text.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>

namespace castlemist::ui {

namespace {

HWND g_bs_wnd = nullptr;
HWND g_bs_list = nullptr;
HWND g_bs_label = nullptr;
HWND g_bs_owner = nullptr;
bool g_bs_done = false;
BakeSelectResult g_bs_result;

void bs_set_all_checked(bool checked) {
    int n = ListView_GetItemCount(g_bs_list);
    for (int i = 0; i < n; ++i) ListView_SetCheckState(g_bs_list, i, checked);
}

void bs_finish(bool proceed) {
    if (proceed) {
        g_bs_result.proceed = true;
        g_bs_result.selected.clear();
        int n = ListView_GetItemCount(g_bs_list);
        for (int i = 0; i < n; ++i) {
            if (!ListView_GetCheckState(g_bs_list, i)) continue;
            LVITEMW it{};
            it.mask = LVIF_PARAM;
            it.iItem = i;
            ListView_GetItem(g_bs_list, &it);
            g_bs_result.selected.insert(static_cast<uint32_t>(it.lParam));
        }
    } else {
        g_bs_result.proceed = false;
        g_bs_result.selected.clear();
    }
    g_bs_done = true;
}

void bs_relayout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    constexpr int kMargin = 10, kBtnH = 28, kBtnW = 110, kLabelH = 44;

    MoveWindow(g_bs_label, kMargin, kMargin, std::max(0, w - 2 * kMargin), kLabelH, TRUE);
    int listY = kMargin + kLabelH + 6;
    MoveWindow(g_bs_list, kMargin, listY, std::max(0, w - 2 * kMargin), std::max(0, h - listY - kBtnH - kMargin), TRUE);

    int y = h - kBtnH - kMargin;
    int x = kMargin;
    MoveWindow(GetDlgItem(hwnd, static_cast<int>(ID_BAKESEL_ALL)), x, y, kBtnW, kBtnH, TRUE); x += kBtnW + 6;
    MoveWindow(GetDlgItem(hwnd, static_cast<int>(ID_BAKESEL_NONE)), x, y, kBtnW, kBtnH, TRUE);
    int rx = w - kMargin;
    rx -= kBtnW; MoveWindow(GetDlgItem(hwnd, static_cast<int>(ID_BAKESEL_CANCEL)), rx, y, kBtnW, kBtnH, TRUE);
    rx -= kBtnW + 6; MoveWindow(GetDlgItem(hwnd, static_cast<int>(ID_BAKESEL_OK)), rx, y, kBtnW, kBtnH, TRUE);
}

LRESULT CALLBACK BakeSelectWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_BAKESEL_ALL: bs_set_all_checked(true); return 0;
        case ID_BAKESEL_NONE: bs_set_all_checked(false); return 0;
        case ID_BAKESEL_OK: bs_finish(true); return 0;
        case ID_BAKESEL_CANCEL: bs_finish(false); return 0;
        }
        break;
    case WM_SIZE:
        bs_relayout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = 420;
        mmi->ptMinTrackSize.y = 260;
        return 0;
    }
    case WM_CLOSE:
        bs_finish(false);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

BakeSelectResult show_bake_select_dialog(HWND owner, const ModelPreview& model) {
    BakeSelectResult noneResult;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = BakeSelectWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2BakeSelectWnd";
        RegisterClassW(&wc);
        registered = true;
    }

    const int W = 640, H = 480;
    g_bs_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME, L"Gw2BakeSelectWnd",
                              L"Select Materials to Bake", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, W, H,
                              owner, nullptr, g_hinstance, nullptr);
    if (!g_bs_wnd) return noneResult;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_bs_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    g_bs_label = mk(L"STATIC",
                    L"Unchecked materials keep their original textures and UVs, still fully editable "
                    L"(same as \"Export glTF (Model)\"). Checked materials get baked into one flattened, "
                    L"no-longer-editable texture per material.",
                    SS_LEFT, 0);

    g_bs_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | LVS_REPORT | WS_BORDER, 0, 0, 0, 0, g_bs_wnd,
                                reinterpret_cast<HMENU>(ID_BAKESEL_LIST), g_hinstance, nullptr);
    ListView_SetExtendedListViewStyle(g_bs_list, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES);
    lv_add_col(g_bs_list, 0, L"Material", 170);
    lv_add_col(g_bs_list, 1, L"Submeshes", 160);
    lv_add_col(g_bs_list, 2, L"Textures (fileId)", 280);

    mk(L"BUTTON", L"Select All", BS_PUSHBUTTON, ID_BAKESEL_ALL);
    mk(L"BUTTON", L"Select None", BS_PUSHBUTTON, ID_BAKESEL_NONE);
    mk(L"BUTTON", L"Bake Selected", BS_DEFPUSHBUTTON, ID_BAKESEL_OK);
    mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON, ID_BAKESEL_CANCEL);

    // One row per material actually used by some mesh -- an unused material
    // slot (present in model.materials but no ModelMeshCPU references it)
    // has nothing to bake either way, so it would only clutter the list.
    // Submesh indices are collected (not just a count) per the same
    // numbering the main window's "Submesh" combo and the UV Map viewer use,
    // so a choice made here can be cross-referenced against either one.
    std::map<uint32_t, std::vector<size_t>> meshesByMat;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const auto& m = model.meshes[i];
        if (!m.vertices.empty() && !m.indices.empty()) meshesByMat[m.materialIndex].push_back(i);
    }

    for (const auto& mat : model.materials) {
        auto it = meshesByMat.find(mat.index);
        if (it == meshesByMat.end()) continue;

        // Same priority as material_export.cpp: the real, artist-authored
        // materialName when a mesh actually named it, else materialFile.
        std::wstring name = !mat.materialName.empty() ? castlemist::core::from_ascii(mat.materialName)
                           : L"Mat_" + std::to_wstring(mat.materialFile != 0 ? mat.materialFile : mat.index);

        std::wstring subs;
        for (size_t mi : it->second) {
            if (!subs.empty()) subs += L", ";
            subs += std::to_wstring(mi);
        }

        std::wstring texs;
        for (uint32_t fid : mat.textureFileIds) {
            if (!texs.empty()) texs += L", ";
            texs += std::to_wstring(fid);
        }

        int row = lv_add_row(g_bs_list, static_cast<LPARAM>(mat.index), name.c_str(), subs.c_str(), texs.c_str(),
                             nullptr, nullptr);
        ListView_SetCheckState(g_bs_list, row, TRUE); // bake everything by default -- opt OUT, not opt in
    }

    bs_relayout(g_bs_wnd);
    g_bs_owner = owner;
    g_bs_done = false;
    g_bs_result = BakeSelectResult{};
    EnableWindow(owner, FALSE);
    ShowWindow(g_bs_wnd, SW_SHOW);
    SetForegroundWindow(g_bs_wnd);

    MSG msg;
    while (!g_bs_done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(g_bs_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(owner, TRUE);
    DestroyWindow(g_bs_wnd);
    g_bs_wnd = nullptr;
    g_bs_list = nullptr;
    g_bs_label = nullptr;
    SetForegroundWindow(owner);

    return g_bs_result;
}

} // namespace castlemist::ui
