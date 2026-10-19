/// @file
/// @brief The Materials window's state and operations, apart from its UI:
///        the session's edits per model, the material and texture library,
///        and the thumbnails the inspector and browser show.
///
/// Private to src/ui. The window (material_dialog.cpp) draws from this and
/// calls these operations; apply_material_edits / material_edits_model_loaded
/// (app_state.h) are how the rest of the viewer reaches it.

#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "castlemist/extract/material_edit.h"
#include "castlemist/extract/model_types.h"

namespace castlemist::ui::matedit {

namespace ex = castlemist::extract;
using SourceMaterial = std::shared_ptr<const castlemist::model::Material>;

/// @brief A material that can be dropped onto a mesh slot.
struct LibraryEntry {
    uint32_t model = 0;   ///< File id of the model it belongs to.
    uint32_t index = 0;   ///< Its MODL material index.
    uint32_t amat = 0;    ///< Its AMAT.
    std::string name;     ///< Material name (may be empty).
    SourceMaterial source;
};

/// @brief An RGBA8 image (thumbnail).
struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return rgba.empty(); }
};

/// @brief Everything the Materials window keeps for the session.
struct Session {
    std::map<uint32_t, ex::MaterialEdits> edits;   ///< By model file id; 0 = a model with none (not kept).
    std::map<uint32_t, ModelTextureCPU> textures;  ///< Textures decoded for edits or the browser, by file id.
    std::map<uint32_t, std::vector<ex::ModelMaterialRef>> donors;  ///< Models loaded for their materials.
    std::vector<LibraryEntry> library;             ///< Most recent model first.
    uint32_t model = 0;                            ///< The current model's file id.
    std::vector<GameMaterial> authored;            ///< Its game materials before any edit.
    size_t authoredTextures = 0;                   ///< Its texture count before any edit.
    std::wstring problems;                         ///< What the last apply could not do.
    std::map<uint32_t, uint32_t> generation;       ///< Material index -> bumped on every change (thumbnails).
    std::set<std::string> asColour, asVector;      ///< Uniform names the user chose to show as colour / vector.
    std::map<std::string, Image> materialThumbs;   ///< By thumbnail key.
    std::map<uint32_t, Image> textureThumbs;       ///< By texture file id.
};

Session& session();
ex::MaterialEdits& current_edits();
ModelPreview* model();
GameMaterial* game_material(uint32_t index);
const ex::MaterialEdit* edit_of(uint32_t index);
/// @brief The material's name, from its meshes, or "Material #<index>".
std::string material_label(uint32_t index);

/// @brief Rebuilds the model's materials from what they were, then re-applies every edit.
void reapply_all();
/// @brief A uniform edit without a rebuild (the renderer refills constants every frame).
void set_uniform_live(uint32_t index, const std::string& key, const std::array<float, 4>& value);
/// @brief Marks material @p index changed, so its thumbnail is drawn again.
void touch(uint32_t index);
/// @brief Draws material @p index with library entry @p e (its AMAT, textures and constants).
void assign_material(uint32_t index, const LibraryEntry& e);
/// @brief Draws material @p index with AMAT @p amat (0 = its own); @p blended picks the blended effect.
void set_shader(uint32_t index, uint32_t amat, bool blended);
/// @brief Puts texture @p fileId in PS slot @p slot of material @p index.
void set_texture(uint32_t index, int slot, uint32_t fileId);
void revert_texture(uint32_t index, int slot);
void revert_uniform(uint32_t index, const std::string& key);
void revert_shader(uint32_t index);
void reset_material(uint32_t index);
void reset_all();
/// @brief Lists model @p modelId's materials in the library. @return How many, or -1.
int add_model_to_library(uint32_t modelId);
/// @brief Decodes texture @p fileId into the session's texture cache. False when it will not decode.
bool add_texture(uint32_t fileId);

/// @brief True when uniform @p name shows as a colour (by its name, or the user's choice).
bool shows_as_colour(const std::string& name);

/// @brief The sphere preview of material @p index as the model draws it now;
///        empty until rendered. Renders at most @p budget new previews per call
///        site per frame: pass a counter the caller decrements.
const Image& material_thumbnail(uint32_t index, int& budget);
/// @brief The sphere preview of library entry @p e.
const Image& library_thumbnail(const LibraryEntry& e, int& budget);
/// @brief A small thumbnail of texture @p fileId (from the model or the cache).
const Image& texture_thumbnail(uint32_t fileId);
/// @brief The texture @p fileId as decoded (model or cache), or null.
const ModelTextureCPU* find_texture(uint32_t fileId);

/// @brief Saves / loads the current model's edits as JSON. Empty string = done, else the problem.
std::wstring save_edits(const std::wstring& path);
std::wstring load_edits(const std::wstring& path);

} // namespace castlemist::ui::matedit
