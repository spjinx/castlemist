/// @file
/// @brief Tools > NPC catalogue: browse the datastore's creatures by body, list their NPCs,
///        show a body's animation clips, open its model, export an NPC to glb.

#include "detail/app_state.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>

#include "castlemist/npc/browse.h"
#include "castlemist/npc/catalogue.h"
#include "castlemist/npc/export.h"
#include "castlemist/npc/tokens.h"

namespace castlemist::ui {
namespace {

namespace npc = castlemist::npc;

constexpr UINT WM_APP_NPC_BUILT = WM_APP + 30;  // lParam: BuildResult* (owned by the receiver)
constexpr int ID_NPC_FILTER = 2200, ID_NPC_CREATURES = 2201, ID_NPC_LIST = 2202, ID_NPC_CLIPS = 2203;
constexpr UINT_PTR ID_NPC_SHOW_CLIPS = 2204, ID_NPC_OPEN_MODEL = 2205, ID_NPC_EXPORT = 2206, ID_NPC_CLOSE = 2207;

struct BuildResult {
    unsigned generation = 0;
    bool from_cache = false;
    std::optional<npc::Catalogue> catalogue;  ///< nullopt: the cache did not load.
};

HWND g_wnd = nullptr, g_filter = nullptr, g_creatures = nullptr, g_npcs = nullptr, g_clips = nullptr,
     g_status = nullptr;
unsigned g_generation = 0;  // bumped when the window closes: late build results are dropped
npc::Catalogue g_cat;
std::vector<npc::Group> g_groups;  // point into g_cat
int g_group = -1;

std::filesystem::path cache_file() {
    return std::filesystem::path(cmap_cache_path()).replace_filename(L"npc_catalogue.json");
}

void set_status(const std::wstring& s) { SetWindowTextW(g_status, s.c_str()); }

npc::SkillName skill_names() {
    return [](uint32_t id) {
        const std::string* n = cached_content_name(64, id);  // content type 64 = /v2/skills
        return n ? *n : std::string();
    };
}

std::string filter_text() {
    wchar_t buf[256] = L"";
    GetWindowTextW(g_filter, buf, 256);
    return wide_to_utf8(buf);
}

void add_row(HWND lv, const std::vector<std::wstring>& cols) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = const_cast<wchar_t*>(cols[0].c_str());
    ListView_InsertItem(lv, &it);
    for (size_t i = 1; i < cols.size(); ++i) ListView_SetItemText(lv, it.iItem, static_cast<int>(i), const_cast<wchar_t*>(cols[i].c_str()));
}

int selected(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

const npc::NpcRecord* current_npc() {
    int i = selected(g_npcs);
    if (g_group < 0 || g_group >= static_cast<int>(g_groups.size()) || i < 0 ||
        i >= static_cast<int>(g_groups[g_group].npcs.size()))
        return nullptr;
    return g_groups[g_group].npcs[i];
}

void fill_npcs() {
    ListView_DeleteAllItems(g_npcs);
    SendMessageW(g_clips, LB_RESETCONTENT, 0, 0);
    if (g_group < 0 || g_group >= static_cast<int>(g_groups.size())) return;
    for (const npc::NpcRecord* n : g_groups[g_group].npcs)
        add_row(g_npcs, {std::to_wstring(n->id), utf8_to_wide(n->codename), utf8_to_wide(n->race),
                         std::to_wstring(n->weapons.size()), std::to_wstring(n->skills.size())});
    if (ListView_GetItemCount(g_npcs) > 0) ListView_SetItemState(g_npcs, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void refilter() {
    g_groups = npc::group_creatures(g_cat, filter_text(), skill_names());
    g_group = -1;
    SendMessageW(g_creatures, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_creatures);
    for (const npc::Group& g : g_groups) add_row(g_creatures, {utf8_to_wide(npc::group_label(g)), std::to_wstring(g.npcs.size())});
    SendMessageW(g_creatures, WM_SETREDRAW, TRUE, 0);
    fill_npcs();
    if (!g_groups.empty()) ListView_SetItemState(g_creatures, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}

void show_coverage(bool from_cache) {
    const npc::Coverage c = g_cat.coverage();
    wchar_t st[300];
    swprintf(st, 300, L"%zu NPCs · %zu with a body model · %zu with a race · %zu with weapons · %zu with skills (%ls)",
             c.npcs, c.with_model, c.with_race, c.with_weapons, c.with_skills, from_cache ? L"cached" : L"built from the dat");
    set_status(st);
}

// ---- build ------------------------------------------------------------------

void post_result(HWND hwnd, BuildResult* r) {
    if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_APP_NPC_BUILT, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

// Loads the JSON cache, or (use_cache false) builds the catalogue from the cntc packs. Everything the
// worker reads is copied here on the UI thread: it touches neither g_app nor the open index DB.
void start_build(bool use_cache) {
    const std::filesystem::path cache = cache_file();
    const unsigned gen = g_generation;
    HWND hwnd = g_wnd;
    if (use_cache && std::filesystem::exists(cache)) {
        set_status(L"Loading the NPC catalogue cache...");
        std::thread([hwnd, gen, cache]() {
            auto* r = new BuildResult{gen, true, npc::load(cache)};
            post_result(hwnd, r);
        }).detach();
        return;
    }
    if (!castlemist::cmap::built()) { set_status(L"The content map is not available - build it from the chat-link decoder."); return; }

    std::vector<MftData> entries;
    std::vector<uint32_t> file_ids;
    for (uint32_t b : castlemist::db::query_base_ids("", "cntc", 0, false, false, 100000)) {
        if (b == 0 || b - 1 >= g_app->data_gw2.mft_data_list.size()) continue;
        entries.push_back(g_app->data_gw2.mft_data_list[b - 1]);
        std::vector<uint32_t> f = get_by_file_id(g_app->data_gw2, b);
        file_ids.push_back(f.empty() ? 0 : *std::min_element(f.begin(), f.end()));
    }
    // container_of for exactly the assets of the type-67 (character) objects, resolved here because the
    // Gw2Dat and the index DB belong to the UI thread.
    auto containers = std::make_shared<std::unordered_map<uint32_t, std::string>>();
    for (const castlemist::cmap::ContentRef& ref : castlemist::cmap::objects()) {
        if (ref.type != 67) continue;
        for (uint32_t fid : castlemist::cmap::resolve_all(67, ref.id)) {
            if (containers->count(fid)) continue;
            uint32_t b = get_by_base_id(g_app->data_gw2, fid);
            (*containers)[fid] = b ? castlemist::db::lookup(b).container : std::string();
        }
    }
    const std::string dat_path = g_app->data_gw2.file_info.file_path;
    set_status(L"Building the NPC catalogue from the dat... (one-time)");
    std::thread([hwnd, gen, cache, dat_path, entries, file_ids, containers]() {
        auto container_of = [&](uint32_t fid) {
            auto it = containers->find(fid);
            return it == containers->end() ? std::string() : it->second;
        };
        npc::Catalogue cat = npc::build_catalogue(dat_path, entries, file_ids, container_of, nullptr);
        if (!cat.npcs.empty()) npc::save(cat, cache);  // never cache an empty (failed) build
        post_result(hwnd, new BuildResult{gen, false, std::move(cat)});
    }).detach();
}

void begin() {
    if (!g_app->dat_loaded || !g_app->index_loaded) { set_status(L"Load a dat and its index first"); return; }
    switch (ensure_content_map(g_wnd)) {
    case CmapEnsure::Ready: start_build(true); return;
    case CmapEnsure::Building:
    case CmapEnsure::Started: set_status(L"Building the content map... (one-time)"); return;
    default: set_status(L"Load a dat and its index first"); return;
    }
}

void on_built(BuildResult* raw) {
    std::unique_ptr<BuildResult> r(raw);
    if (r->generation != g_generation) return;
    if (!r->catalogue) { start_build(false); return; }  // unreadable cache: rebuild
    g_cat = std::move(*r->catalogue);
    refilter();
    show_coverage(r->from_cache);
}

// ---- buttons ----------------------------------------------------------------

void show_clips() {
    SendMessageW(g_clips, LB_RESETCONTENT, 0, 0);
    const npc::NpcRecord* n = current_npc();
    if (!n) return;
    if (!n->model) { set_status(L"This NPC has no body model."); return; }
    if (!g_app->dat_loaded) { set_status(L"Open Gw2.dat first."); return; }
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    auto pv = load_model_by_fileid(g_app->data_gw2, n->model);
    SetCursor(old);
    if (!pv) { set_status(L"The body model could not be loaded."); return; }
    for (const auto& c : pv->animClips) {
        wchar_t line[256];
        swprintf(line, 256, L"%ls — %ls (%.2f s)", utf8_to_wide(c.name).c_str(),
                 utf8_to_wide(npc::clip_group(c.name)).c_str(), static_cast<double>(c.duration));
        SendMessageW(g_clips, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line));
    }
    set_status(std::to_wstring(pv->animClips.size()) + L" clips on model " + std::to_wstring(n->model));
}

void export_selected() {
    const npc::NpcRecord* n = current_npc();
    if (!n) return;
    if (!g_app->dat_loaded) { set_status(L"Open Gw2.dat first - the models come from it."); return; }
    BROWSEINFOW bi{};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Export the NPC (body.glb, weapons, manifest.json) into:";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t folder[MAX_PATH] = L"";
    bool ok = SHGetPathFromIDListW(pidl, folder);
    CoTaskMemFree(pidl);
    if (!ok) return;
    set_status(L"Exporting...");
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    npc::ExportResult r = npc::export_npc(g_app->data_gw2, *n, std::filesystem::path(folder));
    SetCursor(old);
    set_status((r.ok ? L"Exported to " : L"Export failed: ") + r.folder.wstring() + L" (" + std::to_wstring(r.files.size()) +
               L" files, " + std::to_wstring(r.notes.size()) + L" notes)");
}

LRESULT on_notify(NMHDR* h) {
    if (h->code != LVN_ITEMCHANGED) return 0;
    auto* nm = reinterpret_cast<NMLISTVIEW*>(h);
    if (!(nm->uChanged & LVIF_STATE) || !(nm->uNewState & LVIS_SELECTED)) return 0;
    if (h->idFrom == ID_NPC_CREATURES) {
        g_group = nm->iItem;
        fill_npcs();
    }
    return 0;
}

LRESULT CALLBACK NpcWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_NPC_FILTER: if (HIWORD(wparam) == EN_CHANGE && !g_cat.npcs.empty()) refilter(); return 0;
        case ID_NPC_SHOW_CLIPS: show_clips(); return 0;
        case ID_NPC_OPEN_MODEL:
            if (const npc::NpcRecord* n = current_npc()) navigate_to_file_id(n->model);
            return 0;
        case ID_NPC_EXPORT: export_selected(); return 0;
        case ID_NPC_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_NOTIFY: return on_notify(reinterpret_cast<NMHDR*>(lparam));
    case WM_APP_CMAP_DONE:
        if (castlemist::cmap::built()) start_build(true);
        else set_status(L"The content map build failed - see the chat-link decoder.");
        return 0;
    case WM_APP_NPC_BUILT: on_built(reinterpret_cast<BuildResult*>(lparam)); return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        ++g_generation;
        g_wnd = g_filter = g_creatures = g_npcs = g_clips = g_status = nullptr;
        g_cat = {};
        g_groups.clear();
        g_group = -1;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

void open_npc_catalogue(HWND owner) {
    if (g_wnd) { SetForegroundWindow(g_wnd); return; }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = NpcWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"Gw2NpcCatalogueWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT rc{0, 0, 900, 600};
    AdjustWindowRectEx(&rc, style, FALSE, WS_EX_TOOLWINDOW);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"Gw2NpcCatalogueWnd", L"NPC catalogue", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, owner, nullptr, g_hinstance, nullptr);
    if (!g_wnd) return;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, UINT_PTR id) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_wnd, reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    auto column = [](HWND lv, int i, const wchar_t* title, int w) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<wchar_t*>(title);
        col.cx = w;
        ListView_InsertColumn(lv, i, &col);
    };
    const DWORD lv_style = LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP;

    mk(L"STATIC", L"Filter (id, codename, race, model, skill name):", SS_LEFT, 10, 12, 290, 18, 0);
    g_filter = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 300, 8, 300, 24, ID_NPC_FILTER);
    mk(L"STATIC", L"Creatures", SS_LEFT, 10, 40, 200, 16, 0);
    g_creatures = mk(WC_LISTVIEWW, L"", lv_style, 10, 58, 300, 290, ID_NPC_CREATURES);
    column(g_creatures, 0, L"Body", 210);
    column(g_creatures, 1, L"NPCs", 60);
    mk(L"STATIC", L"NPCs", SS_LEFT, 320, 40, 200, 16, 0);
    g_npcs = mk(WC_LISTVIEWW, L"", lv_style, 320, 58, 570, 290, ID_NPC_LIST);
    column(g_npcs, 0, L"Id", 70);
    column(g_npcs, 1, L"Codename", 250);
    column(g_npcs, 2, L"Race", 100);
    column(g_npcs, 3, L"Weapons", 70);
    column(g_npcs, 4, L"Skills", 60);
    for (HWND lv : {g_creatures, g_npcs}) ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
    mk(L"STATIC", L"Clips", SS_LEFT, 10, 356, 200, 16, 0);
    g_clips = mk(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 10, 374, 880, 140, ID_NPC_CLIPS);
    mk(L"BUTTON", L"Show clips", BS_PUSHBUTTON, 10, 524, 100, 28, ID_NPC_SHOW_CLIPS);
    mk(L"BUTTON", L"Open model", BS_PUSHBUTTON, 115, 524, 100, 28, ID_NPC_OPEN_MODEL);
    mk(L"BUTTON", L"Export...", BS_PUSHBUTTON, 220, 524, 100, 28, ID_NPC_EXPORT);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, 810, 524, 80, 28, ID_NPC_CLOSE);
    g_status = mk(L"STATIC", L"", SS_LEFT, 10, 566, 880, 20, 0);

    ShowWindow(g_wnd, SW_SHOW);
    SetFocus(g_filter);
    begin();
}

}  // namespace castlemist::ui
