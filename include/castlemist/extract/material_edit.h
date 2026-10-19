/// @file
/// @brief Live edits to a model's game-shader materials: swap the AMAT, its
///        effect or the whole material, rebind textures, set uniform values.
/// @ingroup extract
///
/// Everything here works on GameMaterial, the payload "Shader" mode draws with,
/// so an edit only ever uses the game's own shaders and textures. Edits are kept
/// as MaterialEdit records apart from the model and re-applied on top of it,
/// because the model's materials are rebuilt from scratch whenever its dyes
/// change; that is also what lets them be saved and loaded as JSON.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "castlemist/native/gw2dat.h"
#include "castlemist/extract/model_types.h"

namespace castlemist::extract {

/// @brief One material's edits. Every field left at its default means "as authored".
struct MaterialEdit {
    uint32_t index = 0;          ///< GameMaterial::index of the material edited.
    /// @brief Draw with another model's material instead (its AMAT, textures and
    ///        constants): that model's file id and material index. 0 = this material.
    uint32_t fromModel = 0;
    uint32_t fromMaterial = 0;
    uint32_t amatFile = 0;       ///< AMAT (material file id) to draw with; 0 = the material's own.
    bool blended = false;        ///< The AMAT's blended colour effect instead of its default.
    std::map<int, uint32_t> textures;                      ///< PS sampler slot -> texture file id.
    std::map<std::string, std::array<float, 4>> uniforms;  ///< Uniform key (see MaterialUniform) -> value.

    /// @brief The shaders themselves change, so the material has to be rebuilt.
    bool rebuilds() const { return fromModel != 0 || amatFile != 0 || blended; }
    bool empty() const { return !rebuilds() && textures.empty() && uniforms.empty(); }
};

/// @brief Every edited material of one model.
using MaterialEdits = std::vector<MaterialEdit>;

/// @brief The edit for material @p index, or null.
MaterialEdit* find_edit(MaterialEdits& edits, uint32_t index);
/// @brief The edit for material @p index, added when there is none.
MaterialEdit& edit_for(MaterialEdits& edits, uint32_t index);

/// @brief A material uniform the editor can set, one per float4 register.
struct MaterialUniform {
    /// @brief `name` for register 0, `name[i]` for register i of an array uniform.
    std::string key;
    std::string name;   ///< bgfx uniform name ("gloover", "envcp", ...).
    int reg = 0;        ///< Register within the uniform.
    bool vs = false;    ///< The vertex shader reads it.
    bool ps = false;    ///< The pixel shader reads it.
    /// @brief The value the material draws with: its constant, else 0 (the cbuffer's fill).
    std::array<float, 4> value{};
    bool set = false;   ///< A constant holds @ref value (from the MODL or an edit).
};

/// @brief The material uniforms of @p g, in shader order; engine-filled ones
///        (camera, lighting, fog, ...) are left out, the renderer owns those.
std::vector<MaterialUniform> material_uniforms(const GameMaterial& g);

/// @brief True for a uniform the engine fills, not the material.
bool is_engine_uniform(const std::string& name);

/// @brief Writes @p value to uniform @p key in every stage that reads it.
/// @return False when neither shader has that uniform.
bool set_material_uniform(GameMaterial& g, const std::string& key, const std::array<float, 4>& value);

/// @brief Binds texture @p texIndex (into ModelPreview::textures) at PS sampler @p slot.
/// @return False when the pixel shader samples nothing at that slot.
bool set_sampler_texture(GameMaterial& g, int slot, int texIndex);

/// @brief Maps a texture file id to its index in the model's textures, decoding
///        and appending it when the model does not have it yet; -1 on failure.
using TextureLookup = std::function<int(uint32_t fileId)>;

/// @brief A TextureLookup over @p model that decodes from @p dat, keeping each
///        decode in @p cache (may be null) so a re-apply does not decode again.
TextureLookup model_texture_lookup(Gw2Dat& dat, ModelPreview& model, std::map<uint32_t, ModelTextureCPU>* cache);

/// @brief Rebuilds @p current from @p source (another model's material, or null
///        for current's own) with the AMAT and effect the edit asks for.
///
/// Texture slots naming the composited armor atlas keep the atlas @p current
/// already binds. The result's `ok` is false when the AMAT has no usable
/// shaders; @p error then says why.
GameMaterial rebuild_game_material(Gw2Dat& dat, const nlohmann::json& tpl, const GameMaterial& current,
                                   const castlemist::model::Material* source, const MaterialEdit& edit,
                                   const TextureLookup& textures, std::string* error = nullptr);

/// @brief Applies @p edit's texture and uniform edits to @p g (not the rebuild).
/// @return How many of them did not apply (a slot or uniform the shaders lack,
///         a texture that would not decode).
int apply_texture_and_uniform_edits(GameMaterial& g, const MaterialEdit& edit, const TextureLookup& textures);

/// @brief One MODL material of a model, kept for rebuild_game_material.
struct ModelMaterialRef {
    uint32_t index = 0;      ///< MODL material index.
    uint32_t amatFile = 0;   ///< Its AMAT.
    std::string name;        ///< The material name its meshes give (may be empty).
    std::shared_ptr<const castlemist::model::Material> source;
};

/// @brief The MODL materials of model @p fileId; empty when it does not parse.
std::vector<ModelMaterialRef> load_model_materials(Gw2Dat& dat, const nlohmann::json& tpl, uint32_t fileId);

/// @brief The material whose MODL index is @p index in @p materials, or null.
const castlemist::model::Material* find_model_material(const std::vector<ModelMaterialRef>& materials, uint32_t index);

/// @brief The edits as JSON: `{"version":1,"model":<fileId>,"materials":[...]}`.
nlohmann::json material_edits_to_json(const MaterialEdits& edits, uint32_t modelFileId);
/// @brief Reads material_edits_to_json's output; entries it cannot read are skipped.
/// @throws std::invalid_argument when @p j is not an object.
MaterialEdits material_edits_from_json(const nlohmann::json& j);

} // namespace castlemist::extract
