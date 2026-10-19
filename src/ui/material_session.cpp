/// @file
/// @brief The Materials window's state and operations (detail/material_session.h).

#include "detail/app_state.h"
#include "detail/material_session.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "castlemist/render/material_preview.h"

namespace castlemist::ui {

void material_window_model_changed();  // material_dialog.cpp

namespace matedit {
namespace {

constexpr size_t kLibraryCap = 400;
constexpr int kPreviewSize = 128;
constexpr int kTextureThumb = 96;
const Image kNoImage;

void note(const std::wstring& s) {
    std::wstring& p = session().problems;
    if (!p.empty()) p += L"  ";
    p += s;
}

void invalidate_views() {
    if (g_app->hwnd_model) InvalidateRect(g_app->hwnd_model, nullptr, FALSE);
    if (g_app->hwnd_model_bgfx) InvalidateRect(g_app->hwnd_model_bgfx, nullptr, FALSE);
}

void touch_all() {
    if (ModelPreview* m = model())
        for (const GameMaterial& g : m->gameMaterials) ++session().generation[g.index];
}

const castlemist::model::Material* donor(uint32_t modelId, uint32_t index) {
    Session& s = session();
    for (const LibraryEntry& e : s.library)
        if (e.model == modelId && e.index == index) return e.source.get();
    auto it = s.donors.find(modelId);
    if (it == s.donors.end()) {
        std::vector<ex::ModelMaterialRef> mats;
        if (auto tpl = castlemist::tpl::get_or_auto_load(); tpl && g_app->dat_loaded)
            mats = ex::load_model_materials(g_app->data_gw2, *tpl, modelId);
        it = s.donors.emplace(modelId, std::move(mats)).first;
    }
    return ex::find_model_material(it->second, index);
}

std::string name_of(const ModelPreview& m, uint32_t index) {
    for (const ModelMaterialCPU& mat : m.materials)
        if (mat.index == index && !mat.materialName.empty()) return mat.materialName;
    return {};
}

void put_in_library(std::vector<LibraryEntry> fresh, uint32_t modelId) {
    Session& s = session();
    s.library.erase(std::remove_if(s.library.begin(), s.library.end(),
                                   [&](const LibraryEntry& e) { return e.model == modelId; }),
                    s.library.end());
    s.library.insert(s.library.begin(), fresh.begin(), fresh.end());
    if (s.library.size() > kLibraryCap) s.library.resize(kLibraryCap);
}

// Every material of the model just loaded joins the library.
void add_to_library(const ModelPreview& m, uint32_t modelId) {
    std::vector<LibraryEntry> fresh;
    for (const GameMaterial& g : m.gameMaterials)
        if (g.source && g.ok) fresh.push_back({modelId, g.index, g.amatFile, name_of(m, g.index), g.source});
    put_in_library(std::move(fresh), modelId);
}

// Drops empty records so "edited" markers and saved files stay honest.
void prune() {
    auto& e = current_edits();
    e.erase(std::remove_if(e.begin(), e.end(), [](const ex::MaterialEdit& m) { return m.empty(); }), e.end());
}

Image downsample(const ModelTextureCPU& t) {
    Image out;
    if (t.width <= 0 || t.height <= 0 || t.rgba.size() < static_cast<size_t>(t.width) * t.height * 4) return out;
    const float scale = std::min(1.0f, static_cast<float>(kTextureThumb) / std::max(t.width, t.height));
    out.width = std::max(1, static_cast<int>(t.width * scale));
    out.height = std::max(1, static_cast<int>(t.height * scale));
    out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y)
        for (int x = 0; x < out.width; ++x) {
            // Box filter over the source footprint (a few samples, enough for a thumbnail).
            const int x0 = x * t.width / out.width, x1 = std::max(x0 + 1, (x + 1) * t.width / out.width);
            const int y0 = y * t.height / out.height, y1 = std::max(y0 + 1, (y + 1) * t.height / out.height);
            const int sx = std::max(1, (x1 - x0) / 4), sy = std::max(1, (y1 - y0) / 4);
            unsigned sum[4] = {}, n = 0;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx) {
                    const uint8_t* p = &t.rgba[(static_cast<size_t>(yy) * t.width + xx) * 4];
                    for (int k = 0; k < 4; ++k) sum[k] += p[k];
                    ++n;
                }
            uint8_t* o = &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4];
            for (int k = 0; k < 3; ++k) o[k] = static_cast<uint8_t>(sum[k] / n);
            o[3] = 255;
        }
    return out;
}

} // namespace

Session& session() {
    static Session s;
    return s;
}

ex::MaterialEdits& current_edits() { return session().edits[session().model]; }

ModelPreview* model() { return g_app && g_app->current_entry.model ? g_app->current_entry.model.get() : nullptr; }

GameMaterial* game_material(uint32_t index) {
    ModelPreview* m = model();
    if (!m) return nullptr;
    for (GameMaterial& g : m->gameMaterials)
        if (g.index == index) return &g;
    return nullptr;
}

const ex::MaterialEdit* edit_of(uint32_t index) { return ex::find_edit(current_edits(), index); }

std::string material_label(uint32_t index) {
    const ModelPreview* m = model();
    const std::string n = m ? name_of(*m, index) : std::string{};
    return n.empty() ? "Material #" + std::to_string(index) : n;
}

void reapply_all() {
    ModelPreview* m = model();
    if (!m) return;
    Session& s = session();
    prune();
    if (g_app->armor_pristine) {
        rebake_armor_dyes();  // rebuilds the materials itself, then apply_material_edits
    } else {
        if (m->textures.size() > s.authoredTextures) m->textures.resize(s.authoredTextures);
        m->gameMaterials = s.authored;
        apply_material_edits(*m, nullptr);
        castlemist::render::refresh_model_materials(*m);
        castlemist::texpanel::set_model(g_app->hwnd_tex_info, m);
        invalidate_views();
    }
    touch_all();
}

void set_uniform_live(uint32_t index, const std::string& key, const std::array<float, 4>& value) {
    GameMaterial* g = game_material(index);
    if (!g) return;
    ex::edit_for(current_edits(), index).uniforms[key] = value;
    ex::set_material_uniform(*g, key, value);
    castlemist::render::set_game_material_constants(*g);
    invalidate_views();
}

void touch(uint32_t index) { ++session().generation[index]; }

void assign_material(uint32_t index, const LibraryEntry& e) {
    if (!game_material(index)) return;
    ex::MaterialEdit& ed = ex::edit_for(current_edits(), index);
    ed = ex::MaterialEdit{};  // slots and uniforms belong to the old shaders
    ed.index = index;
    if (e.model != session().model || e.index != index) {  // its own authored material = no edit
        ed.fromModel = e.model;
        ed.fromMaterial = e.index;
    }
    reapply_all();
}

void set_shader(uint32_t index, uint32_t amat, bool blended) {
    if (!game_material(index)) return;
    ex::MaterialEdit& ed = ex::edit_for(current_edits(), index);
    if (amat != ed.amatFile) {
        ed.textures.clear();
        ed.uniforms.clear();
    }
    ed.amatFile = amat;
    ed.blended = blended;
    reapply_all();
}

void set_texture(uint32_t index, int slot, uint32_t fileId) {
    if (!game_material(index) || !fileId) return;
    ex::edit_for(current_edits(), index).textures[slot] = fileId;
    reapply_all();
}

void revert_texture(uint32_t index, int slot) {
    if (ex::MaterialEdit* e = ex::find_edit(current_edits(), index); e && e->textures.erase(slot)) reapply_all();
}

void revert_uniform(uint32_t index, const std::string& key) {
    if (ex::MaterialEdit* e = ex::find_edit(current_edits(), index); e && e->uniforms.erase(key)) reapply_all();
}

void revert_shader(uint32_t index) {
    if (ex::MaterialEdit* e = ex::find_edit(current_edits(), index)) {
        e->fromModel = e->fromMaterial = e->amatFile = 0;
        e->blended = false;
        reapply_all();
    }
}

void reset_material(uint32_t index) {
    auto& edits = current_edits();
    edits.erase(std::remove_if(edits.begin(), edits.end(), [&](const ex::MaterialEdit& e) { return e.index == index; }),
                edits.end());
    reapply_all();
}

void reset_all() {
    current_edits().clear();
    reapply_all();
}

int add_model_to_library(uint32_t modelId) {
    if (!modelId || !g_app->dat_loaded || !castlemist::tpl::get_or_auto_load()) return -1;
    Session& s = session();
    s.donors.erase(modelId);  // load afresh: the id may have failed before
    donor(modelId, 0);
    const std::vector<ex::ModelMaterialRef>& mats = s.donors[modelId];
    if (mats.empty()) return -1;
    std::vector<LibraryEntry> fresh;
    for (const ex::ModelMaterialRef& r : mats) fresh.push_back({modelId, r.index, r.amatFile, r.name, r.source});
    put_in_library(std::move(fresh), modelId);
    return static_cast<int>(mats.size());
}

bool add_texture(uint32_t fileId) {
    if (find_texture(fileId)) return true;
    if (!g_app->dat_loaded) return false;
    ModelPreview scratch;
    return ex::model_texture_lookup(g_app->data_gw2, scratch, &session().textures)(fileId) >= 0;
}

bool shows_as_colour(const std::string& name) {
    const Session& s = session();
    if (s.asColour.count(name)) return true;
    if (s.asVector.count(name)) return false;
    // GW2's own short names: "holoclr", "glocolor", "tint" ...
    for (const char* hint : {"clr", "col", "tint"})
        if (name.find(hint) != std::string::npos) return true;
    return false;
}

const Image& material_thumbnail(uint32_t index, int& budget) {
    Session& s = session();
    const std::string key = "m" + std::to_string(s.model) + ":" + std::to_string(index) + ":" +
                            std::to_string(s.generation[index]);
    auto it = s.materialThumbs.find(key);
    if (it != s.materialThumbs.end()) return it->second;
    const GameMaterial* g = game_material(index);
    if (!g || budget <= 0) return kNoImage;
    --budget;
    Image img;
    if (castlemist::render::render_material_preview(*g, *model(), kPreviewSize, img.rgba))
        img.width = img.height = kPreviewSize;
    return s.materialThumbs.emplace(key, std::move(img)).first->second;  // an empty image = tried, failed
}

const Image& library_thumbnail(const LibraryEntry& e, int& budget) {
    Session& s = session();
    const std::string key = "l" + std::to_string(e.model) + ":" + std::to_string(e.index);
    auto it = s.materialThumbs.find(key);
    if (it != s.materialThumbs.end()) return it->second;
    const auto tpl = castlemist::tpl::get_or_auto_load();
    if (budget <= 0 || !tpl || !g_app->dat_loaded || !e.source) return kNoImage;
    --budget;
    ModelPreview scratch;  // the entry's own textures, decoded through the session cache
    GameMaterial current;
    current.index = e.index;
    const GameMaterial g = ex::rebuild_game_material(g_app->data_gw2, *tpl, current, e.source.get(), ex::MaterialEdit{},
                                                     ex::model_texture_lookup(g_app->data_gw2, scratch, &s.textures));
    Image img;
    if (g.ok && castlemist::render::render_material_preview(g, scratch, kPreviewSize, img.rgba))
        img.width = img.height = kPreviewSize;
    return s.materialThumbs.emplace(key, std::move(img)).first->second;
}

const ModelTextureCPU* find_texture(uint32_t fileId) {
    if (!fileId) return nullptr;
    if (const ModelPreview* m = model())
        for (const ModelTextureCPU& t : m->textures)
            if (t.fileId == fileId) return &t;
    auto it = session().textures.find(fileId);
    return it == session().textures.end() ? nullptr : &it->second;
}

const Image& texture_thumbnail(uint32_t fileId) {
    Session& s = session();
    auto it = s.textureThumbs.find(fileId);
    if (it != s.textureThumbs.end()) return it->second;
    const ModelTextureCPU* t = find_texture(fileId);
    if (!t) return kNoImage;
    return s.textureThumbs.emplace(fileId, downsample(*t)).first->second;
}

std::wstring save_edits(const std::wstring& path) {
    prune();
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    f << ex::material_edits_to_json(current_edits(), session().model).dump(2);
    return f ? L"" : L"Could not write that file.";
}

std::wstring load_edits(const std::wstring& path) {
    try {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        current_edits() = ex::material_edits_from_json(nlohmann::json::parse(f));
    } catch (const std::exception& e) {
        return L"Not a material edits file: " + utf8_to_wide(e.what());
    }
    reapply_all();
    return L"";
}

} // namespace matedit

namespace me = matedit;

void apply_material_edits(ModelPreview& m, const std::map<std::string, std::array<float, 4>>* dyes) {
    me::Session& s = me::session();
    s.problems.clear();
    auto found = s.edits.find(s.model);
    if (found == s.edits.end() || found->second.empty() || !g_app->dat_loaded) return;
    const auto tpl = castlemist::tpl::get_or_auto_load();
    const me::ex::TextureLookup lookup = me::ex::model_texture_lookup(g_app->data_gw2, m, &s.textures);
    for (const me::ex::MaterialEdit& e : found->second) {
        auto gm = std::find_if(m.gameMaterials.begin(), m.gameMaterials.end(),
                               [&](const GameMaterial& g) { return g.index == e.index; });
        if (gm == m.gameMaterials.end()) continue;
        const std::wstring which = L"Material #" + std::to_wstring(e.index) + L": ";
        if (e.rebuilds()) {
            const castlemist::model::Material* source = e.fromModel ? me::donor(e.fromModel, e.fromMaterial) : nullptr;
            if (!tpl) {
                me::note(which + L"load the struct template to change its shaders.");
            } else if (e.fromModel && !source) {
                me::note(which + L"model " + std::to_wstring(e.fromModel) + L" has no material " +
                         std::to_wstring(e.fromMaterial) + L".");
            } else {
                std::string error;
                GameMaterial g = me::ex::rebuild_game_material(g_app->data_gw2, *tpl, *gm, source, e, lookup, &error);
                if (g.ok) {
                    // A mount's dyes are uniforms of the shaders just replaced.
                    if (dyes)
                        for (const auto& [key, value] : *dyes) me::ex::set_material_uniform(g, key, value);
                    *gm = std::move(g);
                } else {
                    me::note(which + utf8_to_wide(error) + L"; kept as it was.");
                }
            }
        }
        if (const int missed = me::ex::apply_texture_and_uniform_edits(*gm, e, lookup))
            me::note(which + std::to_wstring(missed) +
                     L" edit(s) did not apply (no such slot or uniform, or a texture that would not decode).");
    }
}

void material_edits_model_loaded(uint32_t mft_index, const std::map<std::string, std::array<float, 4>>* dyes) {
    me::Session& s = me::session();
    ModelPreview* m = me::model();
    s.model = 0;
    if (m && g_app->dat_loaded && mft_index != UINT32_MAX) {
        const std::vector<uint32_t> ids = get_by_file_id(g_app->data_gw2, mft_index + 1);
        if (!ids.empty()) s.model = ids.front();
    }
    s.edits.erase(0);  // a model with no file id cannot be told apart from the next one
    s.authored = m ? m->gameMaterials : std::vector<GameMaterial>{};
    s.authoredTextures = m ? m->textures.size() : 0;
    // Thumbnails of the previous model's materials go stale with it.
    for (auto it = s.materialThumbs.begin(); it != s.materialThumbs.end();)
        it = it->first[0] == 'm' ? s.materialThumbs.erase(it) : std::next(it);
    if (m && s.model) me::add_to_library(*m, s.model);
    if (m) apply_material_edits(*m, dyes);
    material_window_model_changed();
}

} // namespace castlemist::ui
