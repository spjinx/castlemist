/// @file
/// @brief View > Particle Browser...: every GW2 effect model and effect object, run in the
///        main preview by the reference sim (fxsim), with an emitter inspector, solo, a
///        timeline and stats (docs/superpowers/specs/2026-10-29-particle-browser-design.md).
///
/// Models tab: MODLs that carry effects, found by a background scan (ModelScan) cached to
/// dumps/index/gw2_fx_index.bin, tied to the dat fingerprint; rows appear as it goes.
/// Effects tab: every type-183 object, read from the content packs on a worker; their files
/// are classified on the UI thread, which owns the index DB.
///
/// Selecting a row shows its model in the main preview (as the Texture Gallery does). The
/// inspector lists the preview sim's emitters (check = draw; unchecking all but one solos it)
/// with every decoded field and its meaning, and a 4 s statistics run of the browser's own sim.

#include "detail/app_state.h"
#include "detail/particle_browser.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <commctrl.h>
#include <commdlg.h>
#include <windowsx.h>

#include "castlemist/core/env.h"
#include "castlemist/db/data_status.h"
#include "castlemist/exportgltf/sky_project.h"
#include "castlemist/format/cntc_pack.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/fxsim/features.h"
#include "castlemist/fxsim/sim.h"
#include "castlemist/fxsim/stats.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/render/model_renderer.h"

namespace castlemist::ui {
namespace {

namespace pb = castlemist::ui::particles;

constexpr UINT WM_APP_PB_EFFECTS = WM_APP + 45;   // lparam: EffectsDone* (owned by the receiver)
constexpr UINT WM_APP_PB_STATS = WM_APP + 47;     // lparam: StatsDone* (owned by the receiver)
constexpr UINT_PTR TIMER_PB_POLL = 1, TIMER_PB_EDIT = 2, TIMER_PB_SEEK = 3;
constexpr UINT_PTR ID_PB_TAB = 100, ID_PB_LIST = 101, ID_PB_SEARCH = 102, ID_PB_TREE = 103, ID_PB_INFO = 104,
                   ID_PB_STREAK = 110, ID_PB_LIGHTNING = 111, ID_PB_MESH = 112, ID_PB_BURST = 113, ID_PB_TRACKS = 114,
                   ID_PB_ALIGN = 115, ID_PB_CLASS = 116, ID_PB_PLAY = 120, ID_PB_RESTART = 121, ID_PB_TIME = 122,
                   ID_PB_SEED = 123, ID_PB_SPEED = 124, ID_PB_EXPORT = 130, ID_PB_COPY = 131, ID_PB_STATS = 132;
constexpr wchar_t kWndClass[] = L"CastlemistParticleBrowser";
constexpr int kTopH = 64, kBottomH = 64;
constexpr float kStep = 1.0f / 60.0f;

struct StatsDone {
    uint64_t generation = 0;
    uint32_t seed = 0;
    fxsim::Stats stats;
    std::vector<std::string> approximations;
};

struct EffectsDone {
    std::shared_ptr<cntc::PackSet> set;
    std::string error;
};

struct Browser {
    HWND wnd = nullptr, tab = nullptr, list = nullptr, search = nullptr, tree = nullptr, info = nullptr,
         status = nullptr, streak = nullptr, lightning = nullptr, mesh = nullptr, burst = nullptr, tracks = nullptr,
         align = nullptr, cls = nullptr, play = nullptr, restart = nullptr, time = nullptr, seed = nullptr,
         speed = nullptr, exportBtn = nullptr, copy = nullptr, statsBtn = nullptr;
    int tabIndex = 0;   // 0 Models, 1 Effects
    // Models
    pb::ModelScan scan;
    std::vector<pb::ModelRow> models;
    std::vector<size_t> shownModels;
    size_t modelsAtFilter = SIZE_MAX;
    // Effects
    std::vector<skillfx::EffectRow> effects;
    std::vector<size_t> shownEffects;
    bool effectsLoading = false;
    std::string effectsError;
    // inspector
    const fxsim::Sim* inspected = nullptr;
    uint64_t inspectedGeneration = 0;   // render::effect_generation() the tree was built for
    size_t inspectedEmitters = 0;
    std::vector<HTREEITEM> emitterItems;
    fxsim::Stats stats;
    std::vector<std::string> approximations;
    bool paused = false;
    uint32_t seedValue = 1;
    // headless
    std::string shotPath;
    int shotWait = 0;
    int soloIndex = -1;
    uint32_t selectId = 0;
};
Browser* g_pb = nullptr;

std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void set_status(const std::wstring& s) { SetWindowTextW(g_pb->status, s.c_str()); }

// ---- the two sources ----------------------------------------------------------

pb::ScanSource model_source() {
    pb::ScanSource src;
    if (!g_app || !g_app->dat_loaded || !castlemist::db::is_open()) return src;
    std::vector<uint32_t> bases = castlemist::db::query_base_ids("", "MODL", 0, false, false, 1000000);
    std::sort(bases.begin(), bases.end());
    auto entries = std::make_shared<std::vector<MftData>>();
    auto fileIds = std::make_shared<std::vector<uint32_t>>();
    const auto& mft = g_app->data_gw2.mft_data_list;
    for (uint32_t b : bases) {
        if (b == 0 || b - 1 >= mft.size()) continue;
        entries->push_back(mft[b - 1]);
        std::vector<uint32_t> f = get_by_file_id(g_app->data_gw2, b);
        fileIds->push_back(f.empty() ? 0 : *std::min_element(f.begin(), f.end()));
    }
    std::shared_ptr<const nlohmann::json> tpl = castlemist::tpl::get();
    const std::string datPath = g_app->data_gw2.file_info.file_path;
    src.count = entries->size();
    src.read = [entries, fileIds, tpl, datPath](size_t i) -> std::optional<pb::ModelRow> {
        if (!tpl) return std::nullopt;
        try {
            const MftData& e = (*entries)[i];
            std::vector<uint8_t> data = read_entry_bytes(datPath, e);
            if (e.compression_flag) data = castlemist::cmp::decompress_entry(data);
            if (data.size() < 16 || data[0] != 'P' || data[1] != 'F' || std::memcmp(data.data() + 8, "MODL", 4) != 0)
                return std::nullopt;
            const castlemist::model::Model m = castlemist::model::Extractor(data, *tpl).extractEffects();
            const fxsim::ModelFeatures f = fxsim::summarize(m);
            if (!f.any()) return std::nullopt;
            pb::ModelRow r;
            r.fileId = (*fileIds)[i];
            r.clouds = f.clouds; r.emitters = f.emitters; r.streaks = f.streaks; r.lightning = f.lightning;
            r.lights = f.lights; r.meshEmitters = f.meshEmitters; r.bursts = f.bursts; r.clipTracks = f.clipTracks;
            r.alignMask = f.alignMask;
            return r;
        } catch (...) {
            return std::nullopt;
        }
    };
    return src;
}

std::filesystem::path index_file() {
    return std::filesystem::path(castlemist::db::path()).parent_path() / L"gw2_fx_index.bin";
}

void start_model_scan() {
    pb::ScanSource src = model_source();
    if (!src.count) { set_status(L"Load a dat and its index first."); return; }
    const std::string fp = castlemist::db::fingerprint_of(g_app->data_gw2).to_string();
    uint32_t upTo = 0;
    std::vector<pb::ModelRow> rows;
    if (!pb::load_index(index_file(), fp, upTo, rows)) { upTo = 0; rows.clear(); }
    g_pb->scan.start(std::move(src), index_file(), fp, upTo, std::move(rows));
}

void start_effects_load() {
    if (g_pb->effectsLoading || !g_pb->effects.empty()) return;
    if (!g_app || !g_app->dat_loaded || !castlemist::db::is_open()) return;
    std::vector<MftData> entries;
    std::vector<uint32_t> fileIds;
    for (uint32_t b : castlemist::db::query_base_ids("", "cntc", 0, false, false, 100000)) {
        if (b == 0 || b - 1 >= g_app->data_gw2.mft_data_list.size()) continue;
        entries.push_back(g_app->data_gw2.mft_data_list[b - 1]);
        std::vector<uint32_t> f = get_by_file_id(g_app->data_gw2, b);
        fileIds.push_back(f.empty() ? 0 : *std::min_element(f.begin(), f.end()));
    }
    g_pb->effectsLoading = true;
    const std::string datPath = g_app->data_gw2.file_info.file_path;
    HWND wnd = g_pb->wnd;
    std::thread([wnd, datPath, entries = std::move(entries), fileIds = std::move(fileIds)]() {
        auto* done = new EffectsDone;
        try {
            auto set = std::make_shared<cntc::PackSet>();
            for (size_t i = 0; i < entries.size(); ++i) {
                std::vector<uint8_t> raw = read_entry_bytes(datPath, entries[i]);
                std::vector<uint8_t> bytes = cntc::decompress_cntc(raw, entries[i].compression_flag != 0);
                const uint32_t base = static_cast<uint32_t>(entries[i].original_index) + 1;
                std::optional<cntc::Pack> p = bytes.empty() ? std::nullopt : cntc::parse_pack(bytes, base, fileIds[i]);
                if (!p) {
                    if (!fileIds[i]) continue;
                    cntc::Pack placeholder;   // keep the slot: external refs count packs in fileId order
                    placeholder.base_id = base;
                    placeholder.file_id = fileIds[i];
                    p = std::move(placeholder);
                }
                set->packs.push_back(std::move(*p));
            }
            set->finish();
            done->set = std::move(set);
        } catch (const std::exception& e) {
            done->error = e.what();
        }
        if (!PostMessageW(wnd, WM_APP_PB_EFFECTS, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

void on_effects(EffectsDone* raw) {
    std::unique_ptr<EffectsDone> done(raw);
    g_pb->effectsLoading = false;
    if (!done->set) { g_pb->effectsError = done->error.empty() ? "content packs unreadable" : done->error; return; }
    // Files are classified here: the index DB belongs to the UI thread.
    auto container_of = [](uint32_t fid) {
        const uint32_t b = get_by_base_id(g_app->data_gw2, fid);
        return b ? castlemist::db::lookup(b).container : std::string();
    };
    g_pb->effects = skillfx::effect_rows(*done->set, container_of);
}

// ---- the list --------------------------------------------------------------------

std::string search_text() {
    wchar_t buf[4096];
    GetWindowTextW(g_pb->search, buf, 4096);
    std::string s;
    for (const wchar_t* p = buf; *p; ++p) s.push_back(*p < 128 ? static_cast<char>(*p) : ' ');
    return s;
}

void refilter() {
    if (g_pb->tabIndex == 0) {
        pb::ModelFilter f;
        f.streak = Button_GetCheck(g_pb->streak) == BST_CHECKED;
        f.lightning = Button_GetCheck(g_pb->lightning) == BST_CHECKED;
        f.mesh = Button_GetCheck(g_pb->mesh) == BST_CHECKED;
        f.burst = Button_GetCheck(g_pb->burst) == BST_CHECKED;
        f.tracks = Button_GetCheck(g_pb->tracks) == BST_CHECKED;
        f.alignCase = static_cast<int>(SendMessageW(g_pb->align, CB_GETCURSEL, 0, 0)) - 1;
        f.search = search_text();
        g_pb->models = g_pb->scan.snapshot();
        g_pb->modelsAtFilter = g_pb->models.size();
        g_pb->shownModels = pb::filter_rows(g_pb->models, f);
        ListView_SetItemCountEx(g_pb->list, static_cast<int>(g_pb->shownModels.size()), LVSICF_NOSCROLL);
    } else {
        const int cls = static_cast<int>(SendMessageW(g_pb->cls, CB_GETCURSEL, 0, 0));
        g_pb->shownEffects = pb::filter_effects(g_pb->effects, cls <= 0 ? -1 : cls, search_text());
        ListView_SetItemCountEx(g_pb->list, static_cast<int>(g_pb->shownEffects.size()), LVSICF_NOSCROLL);
    }
    InvalidateRect(g_pb->list, nullptr, FALSE);
}

void set_columns() {
    while (ListView_DeleteColumn(g_pb->list, 0)) {}
    auto col = [](int i, const wchar_t* name, int w) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        c.pszText = const_cast<wchar_t*>(name);
        c.cx = w;
        ListView_InsertColumn(g_pb->list, i, &c);
    };
    if (g_pb->tabIndex == 0) {
        int i = 0;
        for (auto [n, w] : {std::pair{L"fileId", 70}, {L"clouds", 48}, {L"emitters", 56}, {L"streaks", 50},
                            {L"bolts", 42}, {L"lights", 42}, {L"mesh", 40}, {L"bursts", 46}, {L"tracks", 46}})
            col(i++, n, w);
    } else {
        int i = 0;
        for (auto [n, w] : {std::pair{L"uid", 64}, {L"class", 76}, {L"kind", 36}, {L"model", 70}, {L"sound", 64},
                            {L"children", 54}, {L"codename", 96}})
            col(i++, n, w);
    }
}

std::wstring cell_text(int row, int sub) {
    auto n = [](uint32_t v) { return std::to_wstring(v); };
    if (g_pb->tabIndex == 0) {
        if (row < 0 || static_cast<size_t>(row) >= g_pb->shownModels.size()) return {};
        const pb::ModelRow& r = g_pb->models[g_pb->shownModels[row]];
        const uint32_t v[9] = {r.fileId, r.clouds, r.emitters, r.streaks, r.lightning, r.lights, r.meshEmitters,
                               r.bursts, r.clipTracks};
        return sub >= 0 && sub < 9 ? n(v[sub]) : std::wstring();
    }
    if (row < 0 || static_cast<size_t>(row) >= g_pb->shownEffects.size()) return {};
    const skillfx::EffectRow& r = g_pb->effects[g_pb->shownEffects[row]];
    switch (sub) {
        case 0: return n(r.uid);
        case 1: return widen(skillfx::effect_class_name(skillfx::effect_class_of_kind(r.kind)));
        case 2: return n(r.kind);
        case 3: return r.model ? n(r.model) : L"";
        case 4: return r.sound ? n(r.sound) : L"";
        case 5: return r.children ? n(r.children) : L"";
        case 6: return widen(r.codename);
        default: return {};
    }
}

uint32_t model_of_row(int row) {
    if (g_pb->tabIndex == 0)
        return row >= 0 && static_cast<size_t>(row) < g_pb->shownModels.size() ? g_pb->models[g_pb->shownModels[row]].fileId : 0;
    return row >= 0 && static_cast<size_t>(row) < g_pb->shownEffects.size() ? g_pb->effects[g_pb->shownEffects[row]].model : 0;
}

void show_model(uint32_t fileId) {
    if (!fileId || !g_app || !g_app->dat_loaded) return;
    const uint32_t base = get_by_base_id(g_app->data_gw2, fileId);
    if (base == 0 || base - 1 >= g_app->data_gw2.mft_data_list.size()) return;
    TabCtrl_SetCurSel(g_app->hwnd_tab, static_cast<int>(MiddleTab::Preview));
    on_entry_selected(base - 1);
    relayout();
}

// ---- the inspector ---------------------------------------------------------------

std::wstring stats_text() {
    std::wstring t;
    for (const std::string& a : g_pb->approximations) t += L"approx.: " + widen(a) + L"\r\n";
    t += L"\r\n4 s run, seed " + std::to_wstring(g_pb->seedValue) + L" (live at the end, spread 5/50/95 %, speed)\r\n";
    char line[256];
    for (size_t i = 0; i < g_pb->stats.emitters.size(); ++i) {
        const fxsim::EmitterStats& e = g_pb->stats.emitters[i];
        std::snprintf(line, sizeof line, "e%zu: live %u, spawned %zu, spread %.0f / %.0f / %.0f, speed %.1f, alpha@0..1 %.2f %.2f %.2f %.2f %.2f\r\n",
                      i, e.live.empty() ? 0u : e.live.back(), e.spawned, e.spread[0], e.spread[1], e.spread[2], e.meanSpeed,
                      e.alphaAt[0], e.alphaAt[1], e.alphaAt[2], e.alphaAt[3], e.alphaAt[4]);
        t += widen(line);
    }
    return t;
}

// The browser's own statistics run (the preview's sim keeps no history), on a worker: a heavy
// model must not freeze the window or the preview. A result for an older model or seed is dropped.
void start_stats() {
    const fxsim::Sim* sim = render::effect_sim();
    if (!sim) return;
    SetWindowTextW(g_pb->info, L"Running the 4 s statistics...");
    HWND wnd = g_pb->wnd;
    std::thread([wnd, in = sim->input(), seed = g_pb->seedValue, gen = render::effect_generation()]() {
        auto* done = new StatsDone;
        done->generation = gen;
        done->seed = seed;
        fxsim::Sim run(in, seed, true);
        for (int k = 0; k < 240; ++k) run.step(kStep);
        done->stats = run.stats();
        done->approximations = run.approximations();
        if (!PostMessageW(wnd, WM_APP_PB_STATS, 0, reinterpret_cast<LPARAM>(done))) delete done;
    }).detach();
}

void on_stats(StatsDone* raw) {
    std::unique_ptr<StatsDone> done(raw);
    if (done->generation != g_pb->inspectedGeneration || done->seed != g_pb->seedValue) return;   // stale
    g_pb->stats = std::move(done->stats);
    g_pb->approximations = std::move(done->approximations);
    SetWindowTextW(g_pb->info, stats_text().c_str());
}

void rebuild_inspector() {
    const fxsim::Sim* sim = render::effect_sim();
    g_pb->inspected = sim;
    g_pb->inspectedGeneration = render::effect_generation();
    g_pb->inspectedEmitters = sim ? sim->input().emitters.size() : 0;
    TreeView_DeleteAllItems(g_pb->tree);
    g_pb->emitterItems.clear();
    if (!sim) { SetWindowTextW(g_pb->info, L"The model in the preview has no particle effects."); return; }
    const fxsim::SimInput& in = sim->input();
    for (size_t i = 0; i < in.emitters.size(); ++i) {
        const ParticleEmitterCPU& e = in.emitters[i].e;
        std::vector<pb::FieldLine> lines = pb::describe_emitter(e);
        std::wstring label = L"e" + std::to_wstring(i) + L"  cloud " + std::to_wstring(in.emitters[i].cloud);
        for (const auto& l : lines)
            if (l.label == "alignmentType" || l.label == "mesh") label += L"  -  " + widen(l.meaning);
        if (e.emitterFlags & 0x8) label += L"  -  burst";
        TVINSERTSTRUCTW ins{};
        ins.hParent = TVI_ROOT;
        ins.hInsertAfter = TVI_LAST;
        ins.item.mask = TVIF_TEXT | TVIF_PARAM;
        ins.item.pszText = label.data();
        ins.item.lParam = static_cast<LPARAM>(i);
        HTREEITEM item = TreeView_InsertItem(g_pb->tree, &ins);
        TreeView_SetCheckState(g_pb->tree, item, TRUE);
        g_pb->emitterItems.push_back(item);
        for (const auto& l : lines) {
            std::wstring text = widen(l.group + "  " + l.label + ": " + l.value + (l.meaning.empty() ? "" : "  (" + l.meaning + ")"));
            TVINSERTSTRUCTW child{};
            child.hParent = item;
            child.hInsertAfter = TVI_LAST;
            child.item.mask = TVIF_TEXT | TVIF_STATE;
            child.item.stateMask = TVIS_STATEIMAGEMASK;
            child.item.state = 0;   // no checkbox on field lines
            child.item.pszText = text.data();
            TreeView_InsertItem(g_pb->tree, &child);
        }
    }
    start_stats();
}

void apply_solo() {
    std::vector<bool> mask(g_pb->emitterItems.size(), true);
    bool any = false;
    for (size_t i = 0; i < mask.size(); ++i) {
        mask[i] = TreeView_GetCheckState(g_pb->tree, g_pb->emitterItems[i]) == 1;
        any = any || !mask[i];
    }
    render::set_effect_solo(any ? mask : std::vector<bool>{});
}

// ---- actions -----------------------------------------------------------------------

void copy_id() {
    const int row = ListView_GetNextItem(g_pb->list, -1, LVNI_SELECTED);
    if (row < 0) return;
    const std::wstring text = cell_text(row, 0);
    if (!OpenClipboard(g_pb->wnd)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        std::memcpy(GlobalLock(h), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

void save_stats() {
    wchar_t file[MAX_PATH] = L"fx_stats.json";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof of;
    of.hwndOwner = g_pb->wnd;
    of.lpstrFilter = L"JSON\0*.json\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH;
    of.Flags = OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameW(&of)) return;
    nlohmann::json j = fxsim::stats_json(g_pb->stats);
    j["seed"] = g_pb->seedValue;
    j["approximations"] = g_pb->approximations;
    std::ofstream(std::filesystem::path(file)) << j.dump(2);
}

void set_time_from_slider() {
    const int pos = static_cast<int>(SendMessageW(g_pb->time, TBM_GETPOS, 0, 0));
    g_pb->paused = true;
    render::set_effects_paused(true);
    SetWindowTextW(g_pb->play, L"Play");
    render::set_effect_time(static_cast<float>(pos) * kStep);
}

void read_seed_speed() {
    wchar_t buf[32];
    GetWindowTextW(g_pb->seed, buf, 32);
    const uint32_t seed = static_cast<uint32_t>(std::wcstoul(buf, nullptr, 10));
    if (seed && seed != g_pb->seedValue) {
        g_pb->seedValue = seed;
        render::restart_effects(seed);
        start_stats();   // the same model: the tree and the solo mask stay
    }
    const float speeds[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    const LRESULT k = SendMessageW(g_pb->speed, CB_GETCURSEL, 0, 0);
    if (k >= 0 && k < 6) render::set_effect_speed(speeds[k]);
}

// ---- the window ----------------------------------------------------------------------

void layout() {
    RECT rc;
    GetClientRect(g_pb->wnd, &rc);
    const int w = rc.right, h = rc.bottom;
    const int listW = std::max(320, w * 45 / 100);
    MoveWindow(g_pb->tab, 4, 4, 180, 24, TRUE);
    MoveWindow(g_pb->search, 190, 4, std::max(80, listW - 190), 24, TRUE);
    MoveWindow(g_pb->list, 4, kTopH, listW - 4, h - kTopH - kBottomH, TRUE);
    const int rx = listW + 6, rw = w - rx - 4, treeH = (h - kTopH - kBottomH) * 60 / 100;
    MoveWindow(g_pb->tree, rx, kTopH, rw, treeH, TRUE);
    MoveWindow(g_pb->info, rx, kTopH + treeH + 4, rw, h - kTopH - kBottomH - treeH - 4, TRUE);
    const int by = h - kBottomH + 6;
    MoveWindow(g_pb->play, 4, by, 60, 24, TRUE);
    MoveWindow(g_pb->restart, 68, by, 64, 24, TRUE);
    MoveWindow(g_pb->time, 136, by, std::max(100, w - 136 - 390), 24, TRUE);
    MoveWindow(g_pb->seed, w - 380, by, 60, 24, TRUE);
    MoveWindow(g_pb->speed, w - 316, by, 70, 200, TRUE);
    MoveWindow(g_pb->exportBtn, w - 242, by, 134, 24, TRUE);
    MoveWindow(g_pb->copy, w - 104, by, 100, 24, TRUE);
    MoveWindow(g_pb->statsBtn, w - 242, by + 28, 134, 24, TRUE);
    MoveWindow(g_pb->status, 4, by + 32, w - 252, 18, TRUE);
}

void show_tab_controls() {
    const bool models = g_pb->tabIndex == 0;
    for (HWND c : {g_pb->streak, g_pb->lightning, g_pb->mesh, g_pb->burst, g_pb->tracks, g_pb->align})
        ShowWindow(c, models ? SW_SHOW : SW_HIDE);
    ShowWindow(g_pb->cls, models ? SW_HIDE : SW_SHOW);
    set_columns();
    if (!models) start_effects_load();
    refilter();
}

void update_status() {
    std::wstring s;
    if (g_pb->tabIndex == 0) {
        s = L"Models: " + std::to_wstring(g_pb->shownModels.size()) + L" shown of " + std::to_wstring(g_pb->models.size()) +
            L" with effects; scanned " + std::to_wstring(g_pb->scan.done()) + L" / " + std::to_wstring(g_pb->scan.total()) +
            (g_pb->scan.running() ? L" (scanning)" : L"");
    } else if (g_pb->effectsLoading) {
        s = L"Reading the content packs...";
    } else if (!g_pb->effectsError.empty()) {
        s = L"Effects: " + widen(g_pb->effectsError);
    } else {
        s = L"Effects: " + std::to_wstring(g_pb->shownEffects.size()) + L" shown of " + std::to_wstring(g_pb->effects.size());
    }
    set_status(s);
}

void capture(const std::string& path) {
    RECT rc;
    GetWindowRect(g_pb->wnd, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_pb->wnd, mem, 2 /* PW_RENDERFULLCONTENT */);
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
    exportgltf::sky::write_png(img, path, err);
}

// GW2_PARTICLES=<fileId>: show that model, solo GW2_PARTICLES_SOLO, shoot GW2_PARTICLES_SHOT.
void maybe_shoot() {
    if (g_pb->shotPath.empty()) return;
    const bool ready = g_pb->inspected && g_pb->inspectedEmitters > 0;
    if (!ready && ++g_pb->shotWait < 600) return;   // up to ~2.5 minutes
    if (ready && g_pb->soloIndex >= 0 && static_cast<size_t>(g_pb->soloIndex) < g_pb->emitterItems.size()) {
        for (size_t i = 0; i < g_pb->emitterItems.size(); ++i)
            TreeView_SetCheckState(g_pb->tree, g_pb->emitterItems[i], static_cast<int>(i) == g_pb->soloIndex);
        apply_solo();
        g_pb->soloIndex = -1;
        g_pb->shotWait = 0;
        return;   // let the preview draw a few frames soloed
    }
    if (++g_pb->shotWait < 12) return;
    capture(g_pb->shotPath);
    render::save_screenshot((g_pb->shotPath + "_preview.bmp").c_str());
    g_pb->shotPath.clear();
    if (castlemist::core::env::is_set("GW2_PARTICLES_QUIT") && g_app) PostMessageW(g_app->hwnd_main, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK BrowserWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (!g_pb || g_pb->wnd != hwnd) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {900, 480}; return 0;
    case WM_NOTIFY: {
        auto* nh = reinterpret_cast<NMHDR*>(lparam);
        if (nh->idFrom == ID_PB_TAB && nh->code == TCN_SELCHANGE) {
            g_pb->tabIndex = TabCtrl_GetCurSel(g_pb->tab);
            show_tab_controls();
        } else if (nh->idFrom == ID_PB_LIST && nh->code == LVN_GETDISPINFOW) {
            auto* di = reinterpret_cast<NMLVDISPINFOW*>(lparam);
            if (di->item.mask & LVIF_TEXT) {
                const std::wstring t = cell_text(di->item.iItem, di->item.iSubItem);
                lstrcpynW(di->item.pszText, t.c_str(), di->item.cchTextMax);
            }
        } else if (nh->idFrom == ID_PB_LIST && nh->code == LVN_ITEMCHANGED) {
            auto* lv = reinterpret_cast<NMLISTVIEW*>(lparam);
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) show_model(model_of_row(lv->iItem));
        } else if (nh->idFrom == ID_PB_TREE && nh->code == NM_CLICK) {
            PostMessageW(hwnd, WM_APP_PB_EFFECTS + 1, 0, 0);   // the check state flips after NM_CLICK
        }
        return 0;
    }
    case WM_APP_PB_EFFECTS + 1: apply_solo(); return 0;
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lparam) == g_pb->time) SetTimer(hwnd, TIMER_PB_SEEK, 60, nullptr);   // coalesce a drag
        return 0;
    case WM_COMMAND: {
        const UINT id = LOWORD(wparam), code = HIWORD(wparam);
        if (id >= ID_PB_STREAK && id <= ID_PB_TRACKS) refilter();
        else if ((id == ID_PB_ALIGN || id == ID_PB_CLASS) && code == CBN_SELCHANGE) refilter();
        else if (id == ID_PB_SEARCH && code == EN_CHANGE) SetTimer(hwnd, TIMER_PB_EDIT, 300, nullptr);
        else if (id == ID_PB_SPEED && code == CBN_SELCHANGE) read_seed_speed();
        else if (id == ID_PB_SEED && code == EN_KILLFOCUS) read_seed_speed();
        else if (id == ID_PB_PLAY) {
            g_pb->paused = !g_pb->paused;
            render::set_effects_paused(g_pb->paused);
            SetWindowTextW(g_pb->play, g_pb->paused ? L"Play" : L"Pause");
        } else if (id == ID_PB_RESTART) {
            read_seed_speed();
            render::restart_effects(g_pb->seedValue);
            SendMessageW(g_pb->time, TBM_SETPOS, TRUE, 0);
        } else if (id == ID_PB_EXPORT && g_app) {
            PostMessageW(g_app->hwnd_main, WM_COMMAND, ID_FILE_EXPORT_GLTF_MODEL, 0);
        } else if (id == ID_PB_COPY) {
            copy_id();
        } else if (id == ID_PB_STATS) {
            save_stats();
        }
        return 0;
    }
    case WM_TIMER:
        if (wparam == TIMER_PB_EDIT) {
            KillTimer(hwnd, TIMER_PB_EDIT);
            refilter();
        } else if (wparam == TIMER_PB_SEEK) {
            KillTimer(hwnd, TIMER_PB_SEEK);
            set_time_from_slider();
        } else if (wparam == TIMER_PB_POLL) {
            if (g_pb->tabIndex == 0 && g_pb->scan.snapshot().size() != g_pb->modelsAtFilter) refilter();
            if (g_pb->tabIndex == 1 && !g_pb->effectsLoading && g_pb->shownEffects.empty() && !g_pb->effects.empty())
                refilter();
            if (render::effect_generation() != g_pb->inspectedGeneration) rebuild_inspector();
            update_status();
            maybe_shoot();
        }
        return 0;
    case WM_APP_PB_STATS: on_stats(reinterpret_cast<StatsDone*>(lparam)); return 0;
    case WM_APP_PB_EFFECTS:
        on_effects(reinterpret_cast<EffectsDone*>(lparam));
        if (g_pb->tabIndex == 1) refilter();
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: {
        KillTimer(hwnd, TIMER_PB_POLL);
        KillTimer(hwnd, TIMER_PB_EDIT);
        KillTimer(hwnd, TIMER_PB_SEEK);
        g_pb->scan.stop();   // saves the partial index
        render::set_effect_solo({});
        render::set_effects_paused(false);
        render::set_effect_speed(1.0f);
        MSG m;
        while (PeekMessageW(&m, hwnd, WM_APP_PB_EFFECTS, WM_APP_PB_EFFECTS, PM_REMOVE))
            delete reinterpret_cast<EffectsDone*>(m.lParam);
        while (PeekMessageW(&m, hwnd, WM_APP_PB_STATS, WM_APP_PB_STATS, PM_REMOVE))
            delete reinterpret_cast<StatsDone*>(m.lParam);
        delete g_pb;
        g_pb = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

void close_particle_browser() {
    if (g_pb) DestroyWindow(g_pb->wnd);
}

void open_particle_browser(HWND owner) {
    if (g_pb) {
        ShowWindow(g_pb->wnd, SW_RESTORE);
        SetForegroundWindow(g_pb->wnd);
        return;
    }
    if (!g_app || !g_app->index_loaded || !castlemist::db::is_open() || !g_app->dat_loaded) {
        MessageBoxW(owner, L"Open Gw2.dat and its index first: the browser scans the dat's models and content.",
                    L"Particle Browser", MB_ICONINFORMATION);
        return;
    }
    if (!castlemist::tpl::get()) {
        MessageBoxW(owner, L"The struct template (dumps/packfile/gw2_packfile.json) is not loaded.", L"Particle Browser",
                    MB_ICONINFORMATION);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = BrowserWndProc;
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWndClass;
        RegisterClassW(&wc);
        registered = true;
    }
    g_pb = new Browser;
    // Not owned by the main window (like the gallery): the preview it drives stays visible.
    g_pb->wnd = CreateWindowExW(0, kWndClass, L"Particle Browser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1280, 820, nullptr, nullptr, g_hinstance, nullptr);
    if (!g_pb->wnd) {
        delete g_pb;
        g_pb = nullptr;
        return;
    }
    HFONT font = g_ui_font ? g_ui_font : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD st, int x, int y, int w, int h, UINT_PTR id, DWORD ex = 0) {
        HWND c = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | st, x, y, w, h, g_pb->wnd,
                                 reinterpret_cast<HMENU>(id), g_hinstance, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return c;
    };
    g_pb->tab = mk(WC_TABCONTROLW, L"", WS_TABSTOP, 4, 4, 180, 24, ID_PB_TAB);
    TCITEMW ti{};
    ti.mask = TCIF_TEXT;
    ti.pszText = const_cast<wchar_t*>(L"Models");
    TabCtrl_InsertItem(g_pb->tab, 0, &ti);
    ti.pszText = const_cast<wchar_t*>(L"Effects");
    TabCtrl_InsertItem(g_pb->tab, 1, &ti);
    g_pb->search = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 190, 4, 300, 24, ID_PB_SEARCH);
    SendMessageW(g_pb->search, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"an id, a range (1450950-1451100), a pasted list; Effects: or codename text"));
    int x = 4;
    const int y = 34;
    auto box = [&](const wchar_t* t, int w, UINT_PTR id) { HWND c = mk(L"BUTTON", t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id); x += w + 4; return c; };
    g_pb->streak = box(L"streaks", 64, ID_PB_STREAK);
    g_pb->lightning = box(L"lightning", 84, ID_PB_LIGHTNING);
    g_pb->mesh = box(L"mesh particles", 118, ID_PB_MESH);
    g_pb->burst = box(L"bursts", 66, ID_PB_BURST);
    g_pb->tracks = box(L"clip tracks", 96, ID_PB_TRACKS);
    g_pb->align = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, x, y, 200, 300, ID_PB_ALIGN);
    for (const wchar_t* s : {L"any alignment", L"0: faces alignmentDir", L"1: camera-facing", L"2: faces motion",
                             L"3: faces away from motion", L"4: along on-screen motion", L"5: axis billboard"})
        SendMessageW(g_pb->align, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(g_pb->align, CB_SETCURSEL, 0, 0);
    g_pb->cls = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 4, y, 160, 300, ID_PB_CLASS);
    for (const wchar_t* s : {L"any class", L"audio", L"compound", L"decal", L"material", L"model", L"postproc", L"shakespring"})
        SendMessageW(g_pb->cls, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(g_pb->cls, CB_SETCURSEL, 0, 0);
    g_pb->list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 0, 0,
                    100, 100, ID_PB_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_pb->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    g_pb->tree = mk(WC_TREEVIEWW, L"", TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_CHECKBOXES | WS_TABSTOP, 0, 0,
                    100, 100, ID_PB_TREE, WS_EX_CLIENTEDGE);
    g_pb->info = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, 0, 100, 100, ID_PB_INFO,
                    WS_EX_CLIENTEDGE);
    g_pb->play = mk(L"BUTTON", L"Pause", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 60, 24, ID_PB_PLAY);
    g_pb->restart = mk(L"BUTTON", L"Restart", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 64, 24, ID_PB_RESTART);
    g_pb->time = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 0, 0, 100, 24, ID_PB_TIME);
    SendMessageW(g_pb->time, TBM_SETRANGE, TRUE, MAKELPARAM(0, 600));   // 10 s at 1/60 s
    g_pb->seed = mk(L"EDIT", L"1", WS_BORDER | ES_NUMBER | WS_TABSTOP, 0, 0, 60, 24, ID_PB_SEED);
    g_pb->speed = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, 0, 0, 70, 200, ID_PB_SPEED);
    for (const wchar_t* s : {L"0.1x", L"0.25x", L"0.5x", L"1x", L"2x", L"4x"})
        SendMessageW(g_pb->speed, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(g_pb->speed, CB_SETCURSEL, 3, 0);
    g_pb->exportBtn = mk(L"BUTTON", L"Export for Unity...", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 104, 24, ID_PB_EXPORT);
    g_pb->copy = mk(L"BUTTON", L"Copy ID", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 100, 24, ID_PB_COPY);
    g_pb->statsBtn = mk(L"BUTTON", L"Save stats...", BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 104, 24, ID_PB_STATS);
    g_pb->status = mk(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, 0, 100, 18, 0);

    // headless: GW2_PARTICLES=<fileId> [GW2_PARTICLES_SOLO=<emitter>] [GW2_PARTICLES_SHOT=<png>]
    if (castlemist::core::env::is_set("GW2_PARTICLES")) {
        g_pb->selectId = static_cast<uint32_t>(std::strtoul(castlemist::core::env::text("GW2_PARTICLES").c_str(), nullptr, 10));
        if (castlemist::core::env::is_set("GW2_PARTICLES_SOLO"))
            g_pb->soloIndex = std::atoi(castlemist::core::env::text("GW2_PARTICLES_SOLO").c_str());
        g_pb->shotPath = castlemist::core::env::text("GW2_PARTICLES_SHOT");
        SetWindowTextW(g_pb->search, std::to_wstring(g_pb->selectId).c_str());
        KillTimer(g_pb->wnd, TIMER_PB_EDIT);
    }
    layout();
    show_tab_controls();
    start_model_scan();
    if (g_pb->selectId) show_model(g_pb->selectId);
    SetTimer(g_pb->wnd, TIMER_PB_POLL, 250, nullptr);
    ShowWindow(g_pb->wnd, SW_SHOW);
}

}  // namespace castlemist::ui
