/// @file
/// @brief "Manage keys" popup for the Character Ripper: add / rename / remove
///        the named GW2 API keys in api_keys.json (castlemist::character::KeyStore).

#include "detail/app_state.h"

#include <string>
#include <utility>

#include "castlemist/character/key_store.h"

namespace castlemist::ui {
namespace {

namespace ch = castlemist::character;

HWND g_ck_wnd = nullptr;
HWND g_ck_list = nullptr;
HWND g_ck_name = nullptr;
HWND g_ck_key = nullptr;
ch::KeyStore g_ck_store;
std::function<void()> g_ck_on_changed;

std::wstring window_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring w(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, w.data(), n + 1);
    w.resize(static_cast<size_t>(n));
    return w;
}

// Trim spaces: a key pasted from the ArenaNet page often carries a trailing one.
std::string trimmed_utf8(HWND h) {
    std::string s = wide_to_utf8(window_text(h));
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void refill_list(const std::string& select) {
    SendMessageW(g_ck_list, LB_RESETCONTENT, 0, 0);
    int i = 0;
    for (const ch::ApiKey& k : g_ck_store.list()) {
        SendMessageW(g_ck_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(utf8_to_wide(k.name).c_str()));
        if (k.name == select) SendMessageW(g_ck_list, LB_SETCURSEL, i, 0);
        ++i;
    }
}

std::string selected_name() {
    LRESULT sel = SendMessageW(g_ck_list, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR || static_cast<size_t>(sel) >= g_ck_store.list().size()) return {};
    return g_ck_store.list()[static_cast<size_t>(sel)].name;
}

// Writes the store and tells the Character Ripper to refresh its key combo.
bool persist() {
    std::string err;
    if (!g_ck_store.save(ch::default_key_file(), &err)) {
        MessageBoxW(g_ck_wnd, utf8_to_wide(err).c_str(), L"castlemist", MB_ICONERROR);
        return false;
    }
    if (g_ck_on_changed) g_ck_on_changed();
    request_data_status_refresh();
    return true;
}

void ck_save() {
    std::string name = trimmed_utf8(g_ck_name), key = trimmed_utf8(g_ck_key);
    if (name.empty() || key.empty()) {
        MessageBoxW(g_ck_wnd, L"Enter both a name and an API key.", L"castlemist", MB_ICONINFORMATION);
        return;
    }
    g_ck_store.add(name, key);
    if (persist()) refill_list(name);
}

void ck_rename() {
    std::string from = selected_name(), to = trimmed_utf8(g_ck_name);
    if (from.empty() || to.empty()) {
        MessageBoxW(g_ck_wnd, L"Select a key, type its new name, then Rename.", L"castlemist", MB_ICONINFORMATION);
        return;
    }
    if (!g_ck_store.rename(from, to)) {
        MessageBoxW(g_ck_wnd, L"A key with that name already exists.", L"castlemist", MB_ICONINFORMATION);
        return;
    }
    if (persist()) refill_list(to);
}

void ck_remove() {
    std::string name = selected_name();
    if (name.empty()) return;
    std::wstring q = L"Remove the key \"" + utf8_to_wide(name) + L"\"?";
    if (MessageBoxW(g_ck_wnd, q.c_str(), L"castlemist", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_ck_store.remove(name);
    if (persist()) {
        refill_list({});
        SetWindowTextW(g_ck_name, L"");
        SetWindowTextW(g_ck_key, L"");
    }
}

void ck_selection_changed() {
    const ch::ApiKey* k = g_ck_store.get(selected_name());
    if (!k) return;
    SetWindowTextW(g_ck_name, utf8_to_wide(k->name).c_str());
    SetWindowTextW(g_ck_key, utf8_to_wide(k->key).c_str());
}

void ck_toggle_show(HWND check) {
    bool show = SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SendMessageW(g_ck_key, EM_SETPASSWORDCHAR, show ? 0 : static_cast<WPARAM>(L'\x25CF'), 0);
    InvalidateRect(g_ck_key, nullptr, TRUE);
}

LRESULT CALLBACK CharacterKeysWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_CK_LIST:
            if (HIWORD(wparam) == LBN_SELCHANGE) ck_selection_changed();
            return 0;
        case ID_CK_SHOW: ck_toggle_show(reinterpret_cast<HWND>(lparam)); return 0;
        case ID_CK_SAVE: ck_save(); return 0;
        case ID_CK_RENAME: ck_rename(); return 0;
        case ID_CK_REMOVE: ck_remove(); return 0;
        case ID_CK_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        g_ck_wnd = g_ck_list = g_ck_name = g_ck_key = nullptr;
        g_ck_on_changed = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

void open_character_keys_dialog(HWND owner, std::function<void()> on_changed) {
    if (g_ck_wnd) {
        SetForegroundWindow(g_ck_wnd);
        return;
    }
    std::string err;
    if (!g_ck_store.load(ch::default_key_file(), &err)) {
        MessageBoxW(owner, (utf8_to_wide(err) + L"\n\nFix or delete the file, then try again.").c_str(),
                    L"castlemist", MB_ICONERROR);
        return;
    }
    g_ck_on_changed = std::move(on_changed);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CharacterKeysWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2CharacterKeysWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const int W = 520, H = 300;
    g_ck_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2CharacterKeysWnd", L"GW2 API Keys",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, W, H, owner,
                               nullptr, g_hinstance, nullptr);
    if (!g_ck_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_ck_wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };

    mk(L"STATIC", L"Saved keys:", SS_LEFT, 10, 10, 160, 18, 0);
    g_ck_list = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 10, 30, 160, 180, ID_CK_LIST);
    mk(L"STATIC", L"Name:", SS_LEFT, 185, 32, 60, 18, 0);
    g_ck_name = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 245, 30, W - 265, 22, ID_CK_NAME);
    mk(L"STATIC", L"API key:", SS_LEFT, 185, 64, 60, 18, 0);
    g_ck_key = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD, 245, 62, W - 265, 22, ID_CK_KEY);
    mk(L"BUTTON", L"Show key", BS_AUTOCHECKBOX, 245, 90, 100, 20, ID_CK_SHOW);
    mk(L"STATIC",
       L"Create a key at account.arena.net/applications with the account, characters and builds "
       L"permissions. Keys are stored in plain text in api_keys.json (git-ignored).",
       SS_LEFT, 185, 116, W - 205, 48, 0);
    mk(L"BUTTON", L"Save / Add", BS_PUSHBUTTON, 185, 172, 95, 28, ID_CK_SAVE);
    mk(L"BUTTON", L"Rename", BS_PUSHBUTTON, 285, 172, 80, 28, ID_CK_RENAME);
    mk(L"BUTTON", L"Remove", BS_PUSHBUTTON, 370, 172, 80, 28, ID_CK_REMOVE);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, W - 100, H - 76, 80, 28, ID_CK_CLOSE);

    refill_list({});
    ShowWindow(g_ck_wnd, SW_SHOW);
    SetFocus(g_ck_name);
}

} // namespace castlemist::ui
