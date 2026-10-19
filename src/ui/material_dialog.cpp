/// @file
/// @brief The viewer's "Materials" window, laid out like a game-engine editor:
///        the model's material slots (one per material, like a mesh renderer's
///        list), an inspector for the selected material, and a browser of
///        every material and texture seen this session.
///
/// Everything here is the game's own: a slot takes another GW2 material (its
/// AMAT, textures and constants), the inspector's Shader picks an AMAT and its
/// effect, texture slots take any texture file, and properties are the
/// shader's real uniforms. Thumbnails are spheres drawn with the material's
/// own shaders (render/material_preview.h). Drag a material onto a slot, or a
/// texture onto a texture slot; double-click assigns to the slot whose picker
/// button was pressed.
///
/// Drawn with Dear ImGui in its own top-level window and D3D11 device; the
/// state and operations live in material_session.cpp.

#include "detail/app_state.h"
#include "detail/material_session.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

namespace castlemist::ui {
namespace {

namespace me = matedit;
using Microsoft::WRL::ComPtr;

constexpr UINT_PTR kFrameTimer = 1;
constexpr int kPreviewBudget = 3;  // sphere previews rendered per frame

/// Where a double-click in the browser goes.
enum class Target { None, Element, TextureSlot };

struct Host {
    HWND wnd = nullptr;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11RenderTargetView> rtv;
    ImGuiContext* imgui = nullptr;
    ImFont* bold = nullptr;
    float scale = 1.0f;
    std::map<const me::Image*, ComPtr<ID3D11ShaderResourceView>> images;  // uploaded thumbnails

    uint32_t selected = 0;      // GameMaterial::index in the inspector
    int tab = 0;                // browser tab to bring forward once: 0 materials, 1 textures, -1 none
    int scope = 0;              // materials: 0 all, 1 this model, 2 other models
    float thumb = 76.0f;        // browser thumbnail size
    char search[96] = "";
    Target target = Target::None;
    uint32_t targetIndex = 0;
    int targetSlot = -1;
    int pickedEntry = -1;       // library row selected in the browser
    uint32_t pickedTexture = 0;
    char modelInput[16] = "", textureInput[16] = "", amatInput[16] = "";
    std::wstring status;
};
Host* g_host = nullptr;

// ---- colours and small drawing helpers ----------------------------------------

ImVec4 col(COLORREF c, float a = 1.0f) { return {GetRValue(c) / 255.0f, GetGValue(c) / 255.0f, GetBValue(c) / 255.0f, a}; }
ImU32 col32(COLORREF c, float a = 1.0f) { return ImGui::GetColorU32(col(c, a)); }

void apply_style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0;
    s.ChildRounding = 4;
    s.FrameRounding = 3;
    s.PopupRounding = 4;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.FramePadding = {7, 4};
    s.ItemSpacing = {8, 5};
    s.CellPadding = {6, 4};
    s.IndentSpacing = 14;
    s.ScrollbarSize = 12;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.FrameBorderSize = 1;
    s.ScaleAllSizes(g_host->scale);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col(kColText);
    c[ImGuiCol_TextDisabled] = col(kColSubtle);
    c[ImGuiCol_WindowBg] = col(kColBg);
    c[ImGuiCol_ChildBg] = col(kColPanel);
    c[ImGuiCol_PopupBg] = col(kColCard);
    c[ImGuiCol_Border] = col(kColBorder);
    c[ImGuiCol_FrameBg] = col(kColBand);
    c[ImGuiCol_FrameBgHovered] = col(kColHover);
    c[ImGuiCol_FrameBgActive] = col(kColHover);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = col(kColBand);
    c[ImGuiCol_Button] = col(kColCard);
    c[ImGuiCol_ButtonHovered] = col(kColHover);
    c[ImGuiCol_ButtonActive] = col(kColAccent, 0.6f);
    c[ImGuiCol_Header] = col(kColCard);
    c[ImGuiCol_HeaderHovered] = col(kColHover);
    c[ImGuiCol_HeaderActive] = col(kColAccent, 0.5f);
    c[ImGuiCol_CheckMark] = col(kColAccent);
    c[ImGuiCol_SliderGrab] = col(kColAccent, 0.8f);
    c[ImGuiCol_SliderGrabActive] = col(kColAccent);
    c[ImGuiCol_Tab] = col(kColBand);
    c[ImGuiCol_TabHovered] = col(kColHover);
    c[ImGuiCol_TabSelected] = col(kColCard);
    c[ImGuiCol_TabSelectedOverline] = col(kColAccent);
    c[ImGuiCol_Separator] = col(kColBorder);
    c[ImGuiCol_ScrollbarBg] = col(kColBg);
    c[ImGuiCol_ScrollbarGrab] = col(kColCard);
    c[ImGuiCol_ScrollbarGrabHovered] = col(kColHover);
    c[ImGuiCol_DragDropTarget] = col(kColAccent);
    c[ImGuiCol_TableBorderLight] = col(kColBorder);
}

ImTextureID image_texture(const me::Image& img) {
    if (img.empty()) return ImTextureID{};
    auto it = g_host->images.find(&img);
    if (it != g_host->images.end()) return reinterpret_cast<ImTextureID>(it->second.Get());
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(img.width);
    td.Height = static_cast<UINT>(img.height);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{img.rgba.data(), static_cast<UINT>(img.width) * 4, 0};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(g_host->dev->CreateTexture2D(&td, &sd, &tex)) ||
        FAILED(g_host->dev->CreateShaderResourceView(tex.Get(), nullptr, &srv)))
        return ImTextureID{};
    g_host->images[&img] = srv;
    return reinterpret_cast<ImTextureID>(srv.Get());
}

// An image, or a placeholder square while it is not ready (or will never be).
void draw_image(ImDrawList* dl, const me::Image& img, ImVec2 a, ImVec2 b, bool round) {
    const float r = round ? 4.0f * g_host->scale : 0.0f;
    if (ImTextureID t = image_texture(img)) {
        dl->AddImageRounded(t, a, b, {0, 0}, {1, 1}, IM_COL32_WHITE, r);
    } else {
        dl->AddRectFilled(a, b, col32(kColBand), r);
        const ImVec2 c{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
        dl->AddCircle(c, (b.x - a.x) * 0.28f, col32(kColBorder), 0, 1.5f);
    }
}

// The editor's override marker: an accent bar at the left edge of an edited row.
void edited_bar(float y0, float y1) {
    const ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled({wp.x + 1, y0}, {wp.x + 3.0f * g_host->scale, y1}, col32(kColAccent));
}

std::string upper_first(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void set_status(const std::wstring& s) { g_host->status = s; }

void pick_target(Target t, uint32_t index, int slot) {
    g_host->target = t;
    g_host->targetIndex = index;
    g_host->targetSlot = slot;
    g_host->tab = t == Target::TextureSlot ? 1 : 0;
}

std::wstring file_dialog(bool save) {
    wchar_t path[MAX_PATH] = L"";
    if (save) wcscpy(path, L"materials.json");
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_host->wnd;
    ofn.lpstrFilter = L"Material edits (*.json)\0*.json\0All Files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? path : L"";
}

bool section(const char* title, ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen) {
    ImGui::PushFont(g_host->bold);
    const bool open = ImGui::CollapsingHeader(title, flags);
    ImGui::PopFont();
    return open;
}

// ---- the material slots ("Mesh Renderer") --------------------------------------

void draw_slots(int& budget) {
    ModelPreview* m = me::model();
    const float s = g_host->scale;
    if (!section("Mesh Renderer")) return;
    if (!m || m->gameMaterials.empty()) {
        ImGui::TextDisabled(m ? "This model has no game-shader materials." : "Select a model in the viewer.");
        return;
    }
    ImGui::TextDisabled("Materials");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60 * s);
    ImGui::TextDisabled("Size  %d", static_cast<int>(m->gameMaterials.size()));
    const float rowH = 30 * s;
    for (size_t i = 0; i < m->gameMaterials.size(); ++i) {
        const GameMaterial& g = m->gameMaterials[i];
        ImGui::PushID(static_cast<int>(g.index));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool selected = g.index == g_host->selected;
        ImGui::InvisibleButton("slot", {w - 30 * s, rowH});
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) g_host->selected = g.index;
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("CM_MATERIAL")) {
                const int row = *static_cast<const int*>(pl->Data);
                if (row >= 0 && row < static_cast<int>(me::session().library.size()))
                    me::assign_material(g.index, me::session().library[static_cast<size_t>(row)]);
                g_host->selected = g.index;
            }
            ImGui::EndDragDropTarget();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p1{p0.x + w - 30 * s, p0.y + rowH};
        if (selected) dl->AddRectFilled(p0, p1, col32(kColAccent, 0.22f), 3 * s);
        else if (hovered) dl->AddRectFilled(p0, p1, col32(kColHover), 3 * s);
        dl->AddText({p0.x + 8 * s, p0.y + (rowH - ImGui::GetFontSize()) * 0.5f}, col32(kColSubtle),
                    ("Element " + std::to_string(i)).c_str());
        // The object field: framed, thumbnail + name, like an asset reference.
        const ImVec2 fa{p0.x + 86 * s, p0.y + 3 * s}, fb{p1.x - 2 * s, p1.y - 3 * s};
        dl->AddRectFilled(fa, fb, col32(kColBand), 3 * s);
        dl->AddRect(fa, fb, col32(selected ? kColAccent : kColBorder), 3 * s);
        const float th = fb.y - fa.y - 4 * s;
        draw_image(dl, me::material_thumbnail(g.index, budget), {fa.x + 2 * s, fa.y + 2 * s},
                   {fa.x + 2 * s + th, fa.y + 2 * s + th}, true);
        std::string label = me::material_label(g.index);
        label += g.ok ? "  (AMAT " + std::to_string(g.amatFile) + ")" : "  (no game shader)";
        dl->PushClipRect(fa, fb, true);
        dl->AddText({fa.x + th + 8 * s, fa.y + (fb.y - fa.y - ImGui::GetFontSize()) * 0.5f},
                    col32(g.ok ? kColText : kColSubtle), label.c_str());
        dl->PopClipRect();
        if (const me::ex::MaterialEdit* e = me::edit_of(g.index); e && !e->empty()) edited_bar(p0.y, p1.y);
        ImGui::SameLine(0, 4 * s);
        const bool targeted = g_host->target == Target::Element && g_host->targetIndex == g.index;
        if (targeted) ImGui::PushStyleColor(ImGuiCol_Button, col(kColAccent, 0.6f));
        if (ImGui::Button("##pick", {26 * s, rowH})) {
            g_host->selected = g.index;
            pick_target(Target::Element, g.index, -1);
        }
        {   // the object picker's target icon
            const ImVec2 c{(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                           (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f};
            dl->AddCircle(c, 6 * s, col32(kColText), 0, 1.4f * s);
            dl->AddCircleFilled(c, 2.2f * s, col32(kColText));
        }
        if (targeted) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick a material for this slot in the browser");
        ImGui::PopID();
    }
}

// ---- the inspector ------------------------------------------------------------------

void draw_shader_combo(const GameMaterial& g, const me::ex::MaterialEdit* e) {
    const float s = g_host->scale;
    const std::string preview = "GW2 / AMAT " + std::to_string(g.amatFile) + (g.blendedEffect ? "  (blended)" : "");
    ImGui::SetNextItemWidth(-1);
    if (!ImGui::BeginCombo("##shader", preview.c_str(), ImGuiComboFlags_HeightLarge)) return;
    const uint32_t ownAmat = e && e->amatFile ? e->amatFile : 0;
    ImGui::TextDisabled("Effect");
    if (ImGui::Selectable("Default effect", !g.blendedEffect)) me::set_shader(g.index, ownAmat, false);
    ImGui::BeginDisabled(!g.hasBlendedEffect);
    if (ImGui::Selectable("Blended effect", g.blendedEffect)) me::set_shader(g.index, ownAmat, true);
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::TextDisabled("AMATs seen this session");
    std::vector<uint32_t> amats;
    for (const auto& l : me::session().library)
        if (l.amat && std::find(amats.begin(), amats.end(), l.amat) == amats.end()) amats.push_back(l.amat);
    std::sort(amats.begin(), amats.end());
    for (uint32_t a : amats) {
        ImGui::PushID(static_cast<int>(a));
        if (ImGui::Selectable(("GW2 / AMAT " + std::to_string(a)).c_str(), a == g.amatFile))
            me::set_shader(g.index, a, false);
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(120 * s);
    ImGui::InputTextWithHint("##amat", "AMAT file id", g_host->amatInput, sizeof g_host->amatInput,
                             ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("Use")) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(g_host->amatInput, nullptr, 10));
        if (id) me::set_shader(g.index, id, false);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
}

void draw_textures(const GameMaterial& g, const me::ex::MaterialEdit* e) {
    const float s = g_host->scale;
    if (!section("Textures")) return;
    const ModelPreview& m = *me::model();
    const float th = 46 * s;
    bool any = false;
    for (const GameSamplerCPU& smp : g.samplers) {
        if (smp.role.empty() && smp.global != 0) continue;  // engine inputs: listed below
        any = true;
        ImGui::PushID(smp.slot);
        const float y0 = ImGui::GetCursorScreenPos().y;
        const ModelTextureCPU* t = smp.global == 0 && smp.gameTex >= 0 && smp.gameTex < static_cast<int>(m.textures.size())
                                       ? &m.textures[static_cast<size_t>(smp.gameTex)]
                                       : nullptr;
        const ImVec2 a = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("tex", {th, th});
        const bool targeted = g_host->target == Target::TextureSlot && g_host->targetIndex == g.index &&
                              g_host->targetSlot == smp.slot;
        if (ImGui::IsItemClicked()) pick_target(Target::TextureSlot, g.index, smp.slot);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("CM_TEXTURE"))
                me::set_texture(g.index, smp.slot, *static_cast<const uint32_t*>(pl->Data));
            ImGui::EndDragDropTarget();
        }
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (t && t->fileId) {
            draw_image(dl, me::texture_thumbnail(t->fileId), a, {a.x + th, a.y + th}, false);
        } else {
            dl->AddRectFilled(a, {a.x + th, a.y + th}, col32(kColBand));
            dl->AddText({a.x + 6 * s, a.y + th * 0.5f - ImGui::GetFontSize() * 0.5f}, col32(kColSubtle),
                        t ? "Atlas" : "None");
        }
        dl->AddRect(a, {a.x + th, a.y + th}, col32(targeted ? kColAccent : hovered ? kColSubtle : kColBorder), 0, 0,
                    targeted ? 2.0f : 1.0f);
        if (hovered) ImGui::SetTooltip("Slot %d: click, then double-click a texture in the browser,\nor drop one here", smp.slot);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(upper_first(smp.role).c_str());
        if (t && t->fileId)
            ImGui::TextDisabled("%u   %dx%d %s", t->fileId, t->width, t->height, t->fmt.c_str());
        else if (t)
            ImGui::TextDisabled("composited armor atlas   %dx%d", t->width, t->height);
        else
            ImGui::TextDisabled("missing: reads grey");
        const bool edited = e && e->textures.count(smp.slot);
        if (edited && ImGui::SmallButton("Revert")) me::revert_texture(g.index, smp.slot);
        ImGui::EndGroup();
        if (edited) edited_bar(y0, y0 + th);
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("This shader samples no material textures.");
}

void draw_properties(const GameMaterial& g, const me::ex::MaterialEdit* e) {
    const float s = g_host->scale;
    if (!section("Properties")) return;
    const auto rows = me::ex::material_uniforms(g);
    if (rows.empty()) {
        ImGui::TextDisabled("This shader has no material constants.");
        return;
    }
    if (!ImGui::BeginTable("props", 2, ImGuiTableFlags_SizingStretchProp)) return;
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110 * s);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& u : rows) {
        ImGui::PushID(u.key.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const float y0 = ImGui::GetCursorScreenPos().y;
        const bool edited = e && e->uniforms.count(u.key);
        ImGui::AlignTextToFramePadding();
        if (edited) ImGui::PushFont(g_host->bold);
        ImGui::TextUnformatted(u.key.c_str());
        if (edited) ImGui::PopFont();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s  register %d  (%s)%s", u.name.c_str(), u.reg, u.vs && u.ps ? "VS+PS" : u.vs ? "VS" : "PS",
                              u.set ? "" : "\nnot set by the material: reads 0");
        ImGui::TableNextColumn();
        float v[4] = {u.value[0], u.value[1], u.value[2], u.value[3]};
        ImGui::SetNextItemWidth(-1);
        bool changed;
        if (me::shows_as_colour(u.name)) {
            changed = ImGui::ColorEdit4("##v", v, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR |
                                                      ImGuiColorEditFlags_AlphaPreviewHalf);
        } else {
            const float big = std::max({std::abs(v[0]), std::abs(v[1]), std::abs(v[2]), std::abs(v[3]), 1.0f});
            changed = ImGui::DragFloat4("##v", v, 0.005f * big, 0, 0, "%.3f");
        }
        if (changed) me::set_uniform_live(g.index, u.key, {v[0], v[1], v[2], v[3]});
        if (ImGui::IsItemDeactivatedAfterEdit()) me::touch(g.index);
        if (ImGui::BeginPopupContextItem("ctx")) {
            if (ImGui::MenuItem("Revert", nullptr, false, edited)) me::revert_uniform(g.index, u.key);
            const bool colour = me::shows_as_colour(u.name);
            if (ImGui::MenuItem("Show as colour", nullptr, colour)) {
                me::session().asVector.erase(u.name);
                me::session().asColour.insert(u.name);
            }
            if (ImGui::MenuItem("Show as numbers", nullptr, !colour)) {
                me::session().asColour.erase(u.name);
                me::session().asVector.insert(u.name);
            }
            ImGui::EndPopup();
        }
        if (edited) edited_bar(y0, ImGui::GetCursorScreenPos().y);
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("Drag a value to scrub it; right-click for more.");
}

void draw_engine_inputs(const GameMaterial& g) {
    if (!section("Engine inputs", 0)) return;
    static const char* const kNames[] = {"", "Light buffer / other (grey)", "Environment cube (grey)",
                                         "Shadow map (white)", "Scene depth (far)"};
    for (const GameSamplerCPU& smp : g.samplers)
        if (smp.role.empty() && smp.global > 0 && smp.global < 5)
            ImGui::TextDisabled("Slot %d   %s", smp.slot, kNames[smp.global]);
    ImGui::TextDisabled("Filled by the engine; the viewer binds stand-ins.");
}

void draw_inspector(int& budget) {
    const float s = g_host->scale;
    GameMaterial* g = me::game_material(g_host->selected);
    if (!g) return;
    const me::ex::MaterialEdit* e = me::edit_of(g->index);
    ImGui::Spacing();
    if (!section((me::material_label(g->index) + "  (Material)###inspector").c_str())) return;
    // Header: preview, shader, where it came from.
    const float pv = 96 * s;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::Dummy({pv, pv});
    draw_image(ImGui::GetWindowDrawList(), me::material_thumbnail(g->index, budget), a, {a.x + pv, a.y + pv}, true);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Shader");
    ImGui::SameLine(60 * s);
    draw_shader_combo(*g, e);
    g = me::game_material(g_host->selected);  // the combo may have rebuilt it
    if (!g) {
        ImGui::EndGroup();
        return;
    }
    e = me::edit_of(g->index);
    if (e && e->fromModel)
        ImGui::TextDisabled("Material #%u of model %u", e->fromMaterial, e->fromModel);
    else if (e && (e->amatFile || e->blended))
        ImGui::TextDisabled("Shader changed");
    else
        ImGui::TextDisabled("As authored");
    if (!g->ok) ImGui::TextColored(col(RGB(0xE0, 0xA0, 0x40)), "No usable game shader: drawn with the reconstruction.");
    bool rebuilt = false;
    if (e && e->rebuilds()) {
        if (ImGui::SmallButton("Revert shader")) {
            me::revert_shader(g->index);
            rebuilt = true;
        }
        ImGui::SameLine();
    }
    if (!rebuilt && e && !e->empty() && ImGui::SmallButton("Reset material")) me::reset_material(g->index);
    ImGui::EndGroup();
    ImGui::Spacing();
    g = me::game_material(g_host->selected);
    if (!g) return;
    e = me::edit_of(g->index);
    ImGui::Indent(4 * s);
    draw_textures(*g, e);
    draw_properties(*g, e);
    draw_engine_inputs(*g);
    ImGui::Unindent(4 * s);
}

// ---- the browser ------------------------------------------------------------------

bool matches(const std::string& haystack) {
    const std::string q = lower(g_host->search);
    return q.empty() || lower(haystack).find(q) != std::string::npos;
}

// One thumbnail cell; returns true on a double-click.
bool cell(const char* id, const me::Image& img, const std::string& label, bool picked, bool round, float size) {
    const float s = g_host->scale;
    const float h = size + ImGui::GetFontSize() + 10 * s;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, {size + 8 * s, h});
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (picked) dl->AddRectFilled(a, {a.x + size + 8 * s, a.y + h}, col32(kColAccent, 0.35f), 4 * s);
    else if (hovered) dl->AddRectFilled(a, {a.x + size + 8 * s, a.y + h}, col32(kColHover), 4 * s);
    draw_image(dl, img, {a.x + 4 * s, a.y + 4 * s}, {a.x + 4 * s + size, a.y + 4 * s + size}, round);
    const ImVec2 ta{a.x + 2 * s, a.y + size + 6 * s}, tb{a.x + size + 6 * s, a.y + h};
    const ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->PushClipRect(ta, tb, true);
    dl->AddText({ts.x < size ? a.x + 4 * s + (size - ts.x) * 0.5f : ta.x, ta.y}, col32(picked ? kColText : kColSubtle),
                label.c_str());
    dl->PopClipRect();
    return hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
}

void next_cell(int& column, int columns) {
    if (++column < columns) ImGui::SameLine(0, 2 * g_host->scale);
    else column = 0;
}

void draw_material_grid(int& budget) {
    me::Session& ses = me::session();
    const float s = g_host->scale;
    ImGui::SetNextItemWidth(130 * s);
    ImGui::Combo("##scope", &g_host->scope, "All materials\0This model\0Other models\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110 * s);
    ImGui::InputTextWithHint("##model", "model file id", g_host->modelInput, sizeof g_host->modelInput,
                             ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("Add model")) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(g_host->modelInput, nullptr, 10));
        const int n = me::add_model_to_library(id);
        set_status(n < 0 ? L"File " + std::to_wstring(id) + L" is not a model with materials."
                         : std::to_wstring(n) + L" materials of model " + std::to_wstring(id) + L" added.");
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lists another model's materials here without opening it");

    ImGui::BeginChild("grid", {0, 0}, ImGuiChildFlags_Borders);
    const float size = g_host->thumb * s;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (size + 10 * s)));
    int column = 0, shown = 0;
    for (size_t i = 0; i < ses.library.size(); ++i) {
        const me::LibraryEntry& l = ses.library[i];
        const bool own = l.model == ses.model;
        if ((g_host->scope == 1 && !own) || (g_host->scope == 2 && own)) continue;
        const std::string name = l.name.empty() ? "Material #" + std::to_string(l.index) : l.name;
        if (!matches(name + " " + std::to_string(l.model) + " " + std::to_string(l.amat))) continue;
        ++shown;
        ImGui::PushID(static_cast<int>(i));
        // Cells scrolled out of view cost no preview render.
        const bool visible = ImGui::IsRectVisible({size + 8 * s, size + ImGui::GetFontSize() + 10 * s});
        int none = 0;
        const me::Image& img = me::library_thumbnail(l, visible ? budget : none);
        if (cell("m", img, name, g_host->pickedEntry == static_cast<int>(i), true, size)) {
            const uint32_t to = g_host->target == Target::Element ? g_host->targetIndex : g_host->selected;
            me::assign_material(to, l);
            g_host->selected = to;
            g_host->target = Target::None;
            set_status(utf8_to_wide(me::material_label(to)) + L" now draws with " + utf8_to_wide(name) + L".");
        }
        if (ImGui::IsItemClicked()) g_host->pickedEntry = static_cast<int>(i);
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            ImGui::SetTooltip("%s\nmodel %u, material #%u\nAMAT %u\n\nDouble-click: use for the %s slot\nDrag: onto any slot",
                              name.c_str(), l.model, l.index, l.amat,
                              g_host->target == Target::Element ? "picked" : "selected");
        if (ImGui::BeginDragDropSource()) {
            const int row = static_cast<int>(i);
            ImGui::SetDragDropPayload("CM_MATERIAL", &row, sizeof row);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            draw_image(ImGui::GetWindowDrawList(), img, p, {p.x + 48 * s, p.y + 48 * s}, true);
            ImGui::Dummy({48 * s, 48 * s});
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        next_cell(column, columns);
    }
    if (!shown)
        ImGui::TextDisabled(ses.library.empty() ? "Materials of every model you view land here." : "Nothing matches.");
    ImGui::EndChild();
}

void draw_texture_grid() {
    me::Session& ses = me::session();
    const float s = g_host->scale;
    ImGui::SetNextItemWidth(110 * s);
    ImGui::InputTextWithHint("##texid", "texture file id", g_host->textureInput, sizeof g_host->textureInput,
                             ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (ImGui::Button("Add texture")) {
        const uint32_t id = static_cast<uint32_t>(std::strtoul(g_host->textureInput, nullptr, 10));
        set_status(me::add_texture(id) ? L"Texture " + std::to_wstring(id) + L" added."
                                       : L"File " + std::to_wstring(id) + L" is not a texture that decodes.");
    }
    // This model's textures first, then everything else decoded this session.
    std::vector<uint32_t> ids;
    if (const ModelPreview* m = me::model())
        for (const ModelTextureCPU& t : m->textures)
            if (t.fileId && std::find(ids.begin(), ids.end(), t.fileId) == ids.end()) ids.push_back(t.fileId);
    for (const auto& [id, t] : ses.textures)
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);

    ImGui::BeginChild("grid", {0, 0}, ImGuiChildFlags_Borders);
    const float size = g_host->thumb * s;
    const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (size + 10 * s)));
    int column = 0, shown = 0;
    for (uint32_t id : ids) {
        const ModelTextureCPU* t = me::find_texture(id);
        const std::string label = std::to_string(id);
        if (!t || !matches(label + " " + t->fmt)) continue;
        ++shown;
        ImGui::PushID(static_cast<int>(id));
        const me::Image& img = me::texture_thumbnail(id);
        if (cell("t", img, label, g_host->pickedTexture == id, false, size)) {
            if (g_host->target == Target::TextureSlot) {
                me::set_texture(g_host->targetIndex, g_host->targetSlot, id);
                g_host->target = Target::None;
                set_status(L"Texture " + std::to_wstring(id) + L" set.");
            } else {
                set_status(L"Click a texture slot in the inspector first, or drag the texture onto it.");
            }
        }
        if (ImGui::IsItemClicked()) g_host->pickedTexture = id;
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            ImGui::SetTooltip("%u\n%dx%d %s", id, t->width, t->height, t->fmt.c_str());
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("CM_TEXTURE", &id, sizeof id);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            draw_image(ImGui::GetWindowDrawList(), img, p, {p.x + 48 * s, p.y + 48 * s}, false);
            ImGui::Dummy({48 * s, 48 * s});
            ImGui::Text("%u", id);
            ImGui::EndDragDropSource();
        }
        ImGui::PopID();
        next_cell(column, columns);
    }
    if (!shown) ImGui::TextDisabled("No textures yet.");
    ImGui::EndChild();
}

void search_row(const char* hint) {
    const float s = g_host->scale;
    ImGui::SetNextItemWidth(-100 * s);
    ImGui::InputTextWithHint("##search", hint, g_host->search, sizeof g_host->search);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1);
    ImGui::SliderFloat("##size", &g_host->thumb, 48, 140, "");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size");
}

void draw_browser(int& budget) {
    const float s = g_host->scale;
    if (g_host->target != Target::None) {
        const std::string what = g_host->target == Target::Element
                                     ? me::material_label(g_host->targetIndex)
                                     : "texture slot " + std::to_string(g_host->targetSlot) + " of " +
                                           me::material_label(g_host->targetIndex);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col(kColAccent, 0.22f));
        ImGui::BeginChild("target", {0, ImGui::GetFrameHeight() + 10 * s}, ImGuiChildFlags_Borders);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Select for %s - double-click to assign, or drag", what.c_str());
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 60 * s);
        if (ImGui::Button("Cancel")) g_host->target = Target::None;
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!ImGui::BeginTabBar("browser")) return;
    const int forced = g_host->tab;
    g_host->tab = -1;  // bring a tab forward once, then leave it to the user
    if (ImGui::BeginTabItem("Materials", nullptr, forced == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
        search_row("Search name, model or AMAT");
        draw_material_grid(budget);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Textures", nullptr, forced == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
        search_row("Search file id or format");
        draw_texture_grid();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void draw_toolbar() {
    const float s = g_host->scale;
    ImGui::PushFont(g_host->bold);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Materials");
    ImGui::PopFont();
    ImGui::SameLine();
    if (me::session().model) ImGui::TextDisabled("model %u", me::session().model);
    const bool shader = g_app->model_mode == castlemist::render::RenderMode::GameShader && !g_app->bgfx_view_active;
    if (!shader) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Show in Shader mode")) set_model_mode(castlemist::render::RenderMode::GameShader);
    }
    const float bw = 80 * s;
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 3 * bw - 2 * ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button("Save...", {bw, 0})) {
        const std::wstring p = file_dialog(true);
        if (!p.empty()) {
            const std::wstring err = me::save_edits(p);
            set_status(err.empty() ? L"Saved." : err);
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save this model's edits as JSON");
    ImGui::SameLine();
    if (ImGui::Button("Load...", {bw, 0})) {
        const std::wstring p = file_dialog(false);
        if (!p.empty()) {
            const std::wstring err = me::load_edits(p);
            set_status(!err.empty() ? err : L"Loaded.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all", {bw, 0})) me::reset_all();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Every material back to as authored");
    ImGui::Separator();
}

void draw_ui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##materials", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    int budget = kPreviewBudget;
    draw_toolbar();
    const float s = g_host->scale;
    const float statusH = ImGui::GetTextLineHeightWithSpacing() + 8 * s;
    const float leftW = std::max(380 * s, ImGui::GetContentRegionAvail().x * 0.47f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(kColBg));
    ImGui::BeginChild("left", {leftW, -statusH});
    draw_slots(budget);
    draw_inspector(budget);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("right", {0, -statusH});
    draw_browser(budget);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Separator();
    const std::wstring& problems = me::session().problems;
    const std::wstring& text = !problems.empty() ? problems : g_host->status;
    if (!problems.empty()) ImGui::PushStyleColor(ImGuiCol_Text, col(RGB(0xE0, 0xA0, 0x40)));
    ImGui::TextUnformatted(text.empty() ? "Edits show live in the viewer's Shader mode. The blue bar marks what is edited."
                                        : wide_to_utf8(text).c_str());
    if (!problems.empty()) ImGui::PopStyleColor();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_host->target = Target::None;
    ImGui::End();
}

// ---- the host window ----------------------------------------------------------------

void create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    if (SUCCEEDED(g_host->swap->GetBuffer(0, IID_PPV_ARGS(&back))))
        g_host->dev->CreateRenderTargetView(back.Get(), nullptr, &g_host->rtv);
}

void render_frame() {
    if (!g_host || !g_host->imgui || !g_host->rtv) return;
    ImGui::SetCurrentContext(g_host->imgui);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    draw_ui();
    ImGui::Render();
    const ImVec4 bg = col(kColBg);
    const float clear[4] = {bg.x, bg.y, bg.z, 1};
    ID3D11RenderTargetView* rtv = g_host->rtv.Get();
    g_host->ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_host->ctx->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_host->swap->Present(1, 0);
}

void destroy_host() {
    if (!g_host) return;
    if (g_host->imgui) {
        ImGui::SetCurrentContext(g_host->imgui);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_host->imgui);
    }
    delete g_host;
    g_host = nullptr;
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_host && g_host->imgui) {
        ImGui::SetCurrentContext(g_host->imgui);
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return 1;
    }
    switch (msg) {
    case WM_SIZE:
        if (g_host && g_host->swap && wparam != SIZE_MINIMIZED) {
            g_host->rtv.Reset();
            g_host->swap->ResizeBuffers(0, LOWORD(lparam), HIWORD(lparam), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv();
        }
        return 0;
    case WM_TIMER:
        if (wparam == kFrameTimer && IsWindowVisible(hwnd) && !IsIconic(hwnd)) render_frame();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        render_frame();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
        mm->ptMinTrackSize = {760, 520};
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kFrameTimer);
        destroy_host();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool create_host(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.hInstance = g_hinstance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpfnWndProc = HostProc;
        wc.lpszClassName = L"Gw2MaterialsWnd";
        RegisterClassW(&wc);
        registered = true;
    }
    g_host = new Host;
    RECT owr{};
    GetWindowRect(owner, &owr);
    const UINT dpi = GetDpiForWindow(owner);
    g_host->scale = dpi ? dpi / 96.0f : 1.0f;
    const int W = static_cast<int>(1080 * g_host->scale), H = static_cast<int>(820 * g_host->scale);
    g_host->wnd = CreateWindowExW(0, L"Gw2MaterialsWnd", L"Materials", WS_OVERLAPPEDWINDOW,
                                  std::max(0L, owr.right - W - 40), owr.top + 60, W, H, owner, nullptr, g_hinstance,
                                  nullptr);
    if (!g_host->wnd) {
        destroy_host();
        return false;
    }
    apply_titlebar_theme(g_host->wnd);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_host->wnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &g_host->swap, &g_host->dev, &fl, &g_host->ctx))) {
        DestroyWindow(g_host->wnd);
        return false;
    }
    create_rtv();

    IMGUI_CHECKVERSION();
    g_host->imgui = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_host->imgui);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    const float px = 15.0f * g_host->scale;
    ImFont* regular = nullptr;
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
        regular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", px);
    if (!regular) io.Fonts->AddFontDefault();
    if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeuib.ttf") != INVALID_FILE_ATTRIBUTES)
        g_host->bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", px);
    if (!g_host->bold) g_host->bold = io.Fonts->Fonts[0];
    apply_style();
    ImGui_ImplWin32_Init(g_host->wnd);
    ImGui_ImplDX11_Init(g_host->dev.Get(), g_host->ctx.Get());
    if (ModelPreview* m = me::model(); m && !m->gameMaterials.empty()) g_host->selected = m->gameMaterials.front().index;
    SetTimer(g_host->wnd, kFrameTimer, 16, nullptr);
    return true;
}

// Writes the window's last frame (its back buffer) as a 24-bit BMP.
bool save_frame_bmp(const std::string& path) {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_host->swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC td;
    back->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(g_host->dev->CreateTexture2D(&td, nullptr, &staging))) return false;
    g_host->ctx->CopyResource(staging.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_host->ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
    const int W = static_cast<int>(td.Width), H = static_cast<int>(td.Height);
    const uint32_t stride = (static_cast<uint32_t>(W) * 3 + 3) & ~3u;
    std::vector<uint8_t> px(static_cast<size_t>(stride) * H);
    for (int y = 0; y < H; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(H - 1 - y) * m.RowPitch;
        for (int x = 0; x < W; ++x) {  // RGBA -> BGR
            px[static_cast<size_t>(y) * stride + x * 3 + 0] = src[x * 4 + 2];
            px[static_cast<size_t>(y) * stride + x * 3 + 1] = src[x * 4 + 1];
            px[static_cast<size_t>(y) * stride + x * 3 + 2] = src[x * 4 + 0];
        }
    }
    g_host->ctx->Unmap(staging.Get(), 0);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof bi;
    bi.biWidth = W;
    bi.biHeight = H;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof bi;
    fh.bfSize = fh.bfOffBits + static_cast<DWORD>(px.size());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&fh), sizeof fh);
    f.write(reinterpret_cast<const char*>(&bi), sizeof bi);
    f.write(reinterpret_cast<const char*>(px.data()), static_cast<std::streamsize>(px.size()));
    return static_cast<bool>(f);
}

} // namespace

void material_window_model_changed() {
    if (!g_host) return;
    g_host->images.clear();  // the thumbnails they point at are gone with the old model
    g_host->target = Target::None;
    g_host->pickedEntry = -1;
    ModelPreview* m = me::model();
    g_host->selected = m && !m->gameMaterials.empty() ? m->gameMaterials.front().index : 0;
}

void open_material_dialog(HWND owner) {
    if (g_host) {
        ShowWindow(g_host->wnd, SW_RESTORE);
        SetForegroundWindow(g_host->wnd);
        return;
    }
    if (!create_host(owner)) return;
    // These are the materials Shader mode draws with: show them.
    if (me::model() && g_app->model_mode != castlemist::render::RenderMode::GameShader)
        set_model_mode(castlemist::render::RenderMode::GameShader);
    ShowWindow(g_host->wnd, SW_SHOW);
}

void run_material_selftest(const std::string& dir, uint32_t donorModel) {
    FILE* log = std::fopen((dir + "mattest.txt").c_str(), "w");
    auto say = [&](const std::string& t) {
        if (log) std::fprintf(log, "%s\n", t.c_str());
    };
    auto frames = [](int n) {
        for (int i = 0; i < n; ++i) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            render_frame();
        }
    };
    ModelPreview* m = me::model();
    const GameMaterial* first = nullptr;
    if (m)
        for (const GameMaterial& g : m->gameMaterials)
            if (g.ok && !first) first = &g;
    if (!first) {
        say("no model with a game-shader material");
        if (log) std::fclose(log);
        return;
    }
    const uint32_t index = first->index, authoredAmat = first->amatFile;
    open_material_dialog(g_app->hwnd_main);
    if (!g_host) {
        say("the Materials window did not open");
        if (log) std::fclose(log);
        return;
    }
    say("model " + std::to_string(me::session().model) + ": " + std::to_string(m->gameMaterials.size()) +
        " game materials, library " + std::to_string(me::session().library.size()));
    g_host->selected = index;
    frames(12);  // let the previews render
    castlemist::render::save_screenshot((dir + "shot_mat_before.bmp").c_str());
    say(std::string("frame before: ") + (save_frame_bmp(dir + "shot_mat_window_before.bmp") ? "ok" : "failed"));

    const auto rows = me::ex::material_uniforms(*me::game_material(index));
    if (!rows.empty()) {
        me::set_uniform_live(index, rows.front().key, {1, 0, 0, 1});
        me::touch(index);
        bool reached = false;
        for (const auto& r : me::ex::material_uniforms(*me::game_material(index)))
            if (r.key == rows.front().key) reached = r.value[0] == 1.0f && r.value[1] == 0.0f;
        say("live uniform " + rows.front().key + " = 1 0 0 1: " + (reached ? "in the model" : "NOT in the model"));
    }
    if (donorModel) {
        const int n = me::add_model_to_library(donorModel);
        say("donor materials listed: " + std::to_string(n));
        for (const me::LibraryEntry& l : me::session().library)
            if (l.model == donorModel) {
                me::assign_material(index, l);
                break;
            }
        const GameMaterial* g = me::game_material(index);
        say("after assigning the donor's first material: AMAT " + std::to_string(g ? g->amatFile : 0) +
            (g && g->ok ? " ok" : " FAILED") +
            (me::session().problems.empty() ? "" : "; " + wide_to_utf8(me::session().problems)));
        castlemist::render::save_screenshot((dir + "shot_mat_swapped.bmp").c_str());
    }
    pick_target(Target::Element, index, -1);  // show the picking banner in the capture
    frames(40);
    say(std::string("frame after: ") + (save_frame_bmp(dir + "shot_mat_window.bmp") ? "ok" : "failed"));
    say("edits: " + me::ex::material_edits_to_json(me::current_edits(), me::session().model).dump());
    me::reset_all();
    const GameMaterial* back = me::game_material(index);
    say("reset all: AMAT " + std::to_string(back ? back->amatFile : 0) +
        (back && back->amatFile == authoredAmat ? " (authored)" : " (NOT authored)"));
    if (log) std::fclose(log);
}

} // namespace castlemist::ui
