/// @file
/// @brief Live edits to a model's game-shader materials (material_edit.h).

#include "internal.h"
#include "castlemist/extract/material_edit.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace castlemist::extract {

namespace {

// "name" -> (name, 0); "name[3]" -> (name, 3).
std::pair<std::string, int> split_key(const std::string& key) {
    const size_t open = key.find('[');
    if (open == std::string::npos || key.back() != ']') return {key, 0};
    return {key.substr(0, open), std::atoi(key.c_str() + open + 1)};
}

std::string make_key(const std::string& name, int reg) {
    return reg == 0 ? name : name + "[" + std::to_string(reg) + "]";
}

int registers(const GameShaderUniform& u) { return std::max(1, u.vec4s); }

const GameConstOverride* find_const(const std::vector<GameConstOverride>& consts, int byteOff) {
    for (const auto& c : consts)
        if (c.byteOff == byteOff) return &c;
    return nullptr;
}

bool set_in_stage(const std::vector<GameShaderUniform>& unis, std::vector<GameConstOverride>& consts,
                  const std::string& name, int reg, const std::array<float, 4>& value) {
    bool hit = false;
    for (const auto& u : unis) {
        if (u.type == 0 || u.name != name || reg < 0 || reg >= registers(u)) continue;
        const int off = u.byteOff + 16 * reg;
        auto c = std::find_if(consts.begin(), consts.end(), [&](const GameConstOverride& o) { return o.byteOff == off; });
        if (c == consts.end()) c = consts.insert(consts.end(), GameConstOverride{off, {}});
        std::memcpy(c->value, value.data(), sizeof c->value);
        hit = true;
    }
    return hit;
}

} // namespace

MaterialEdit* find_edit(MaterialEdits& edits, uint32_t index) {
    for (auto& e : edits)
        if (e.index == index) return &e;
    return nullptr;
}

MaterialEdit& edit_for(MaterialEdits& edits, uint32_t index) {
    if (MaterialEdit* e = find_edit(edits, index)) return *e;
    edits.push_back({});
    edits.back().index = index;
    return edits.back();
}

std::vector<MaterialUniform> material_uniforms(const GameMaterial& g) {
    std::vector<MaterialUniform> out;
    auto add = [&](const std::vector<GameShaderUniform>& unis, const std::vector<GameConstOverride>& consts, bool ps) {
        for (const auto& u : unis) {
            if (u.type == 0 || is_engine_uniform(u.name)) continue;
            for (int r = 0; r < registers(u); ++r) {
                const std::string key = make_key(u.name, r);
                auto it = std::find_if(out.begin(), out.end(), [&](const MaterialUniform& m) { return m.key == key; });
                if (it == out.end()) {
                    out.push_back({key, u.name, r});
                    it = out.end() - 1;
                }
                (ps ? it->ps : it->vs) = true;
                if (const GameConstOverride* c = find_const(consts, u.byteOff + 16 * r); c && !it->set) {
                    std::memcpy(it->value.data(), c->value, sizeof c->value);
                    it->set = true;
                }
            }
        }
    };
    add(g.vsUniforms, g.vsConsts, false);
    add(g.psUniforms, g.psConsts, true);
    return out;
}

bool set_material_uniform(GameMaterial& g, const std::string& key, const std::array<float, 4>& value) {
    const auto [name, reg] = split_key(key);
    const bool vs = set_in_stage(g.vsUniforms, g.vsConsts, name, reg, value);
    const bool ps = set_in_stage(g.psUniforms, g.psConsts, name, reg, value);
    return vs || ps;
}

bool set_sampler_texture(GameMaterial& g, int slot, int texIndex) {
    bool hit = false;
    for (auto& s : g.samplers) {
        if (s.slot != slot) continue;
        s.gameTex = texIndex;
        s.global = texIndex < 0 ? 1 : 0;   // a texture that will not decode reads grey, as at extraction
        s.atlas = 0;
        hit = true;
    }
    return hit;
}

TextureLookup model_texture_lookup(Gw2Dat& dat, ModelPreview& model, std::map<uint32_t, ModelTextureCPU>* cache) {
    return [&dat, &model, cache](uint32_t fileId) -> int {
        if (fileId == 0) return -1;
        for (size_t i = 0; i < model.textures.size(); ++i)
            if (model.textures[i].fileId == fileId) return static_cast<int>(i);
        ModelTextureCPU tex;
        if (cache) {
            auto it = cache->find(fileId);
            if (it != cache->end()) {
                tex = it->second;
            } else {
                if (!decode_texture_by_fileid(dat, fileId, tex)) return -1;
                (*cache)[fileId] = tex;
            }
        } else if (!decode_texture_by_fileid(dat, fileId, tex)) {
            return -1;
        }
        model.textures.push_back(std::move(tex));
        return static_cast<int>(model.textures.size()) - 1;
    };
}

GameMaterial rebuild_game_material(Gw2Dat& dat, const nlohmann::json& tpl, const GameMaterial& current,
                                   const castlemist::model::Material* source, const MaterialEdit& edit,
                                   const TextureLookup& textures, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        GameMaterial g;
        g.index = current.index;
        return g;
    };
    if (!source) source = current.source.get();
    if (!source) return fail("the material's MODL source was not kept");
    castlemist::model::Material m = *source;
    m.index = current.index;
    if (edit.amatFile) m.materialFile = edit.amatFile;
    if (m.materialFile == 0) return fail("the material names no AMAT");
    if (get_by_base_id(dat, m.materialFile) == 0)
        return fail("AMAT " + std::to_string(m.materialFile) + " is not in the archive");
    GameMaterial g = extract_game_material(dat, tpl, m, textures, false, edit.blended);
    if (!g.ok) return fail("AMAT " + std::to_string(m.materialFile) + " has no usable colour shaders");
    // An atlas slot (fileId 0) has no texture of its own: keep the composited
    // atlas the current material binds for the same role.
    for (auto& s : g.samplers) {
        if (s.atlas == 0) continue;
        for (const auto& c : current.samplers)
            if (c.atlas == s.atlas && c.global == 0 && c.gameTex >= 0) {
                s.gameTex = c.gameTex;
                s.global = 0;
                break;
            }
    }
    if (error) error->clear();
    return g;
}

int apply_texture_and_uniform_edits(GameMaterial& g, const MaterialEdit& edit, const TextureLookup& textures) {
    int missed = 0;
    for (const auto& [slot, fileId] : edit.textures) {
        const int ti = textures(fileId);
        if (ti < 0 || !set_sampler_texture(g, slot, ti)) ++missed;
    }
    for (const auto& [key, value] : edit.uniforms)
        if (!set_material_uniform(g, key, value)) ++missed;
    return missed;
}

std::vector<ModelMaterialRef> load_model_materials(Gw2Dat& dat, const nlohmann::json& tpl, uint32_t fileId) {
    std::vector<ModelMaterialRef> out;
    try {
        const std::vector<uint8_t> bytes = load_modl_bytes_by_fileid(dat, fileId);
        if (bytes.empty()) return out;
        castlemist::model::Model model = castlemist::model::Extractor(bytes, tpl).extract();
        for (auto& m : model.materials) {
            ModelMaterialRef r;
            r.index = m.index;
            r.amatFile = m.materialFile;
            for (const auto& mesh : model.meshes)   // the name lives per mesh (model_preview.cpp)
                if (mesh.materialIndex == m.index && !mesh.materialName.empty()) {
                    r.name = mesh.materialName;
                    break;
                }
            r.source = std::make_shared<const castlemist::model::Material>(std::move(m));
            out.push_back(std::move(r));
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

const castlemist::model::Material* find_model_material(const std::vector<ModelMaterialRef>& materials, uint32_t index) {
    for (const auto& m : materials)
        if (m.index == index && m.source) return m.source.get();
    return nullptr;
}

nlohmann::json material_edits_to_json(const MaterialEdits& edits, uint32_t modelFileId) {
    nlohmann::json mats = nlohmann::json::array();
    for (const auto& e : edits) {
        if (e.empty()) continue;
        nlohmann::json m{{"index", e.index}};
        if (e.fromModel) m["from"] = {{"model", e.fromModel}, {"material", e.fromMaterial}};
        if (e.amatFile) m["amat"] = e.amatFile;
        if (e.blended) m["blended"] = true;
        if (!e.textures.empty()) {
            nlohmann::json t = nlohmann::json::object();
            for (const auto& [slot, fileId] : e.textures) t[std::to_string(slot)] = fileId;
            m["textures"] = std::move(t);
        }
        if (!e.uniforms.empty()) {
            nlohmann::json u = nlohmann::json::object();
            for (const auto& [key, v] : e.uniforms) u[key] = {v[0], v[1], v[2], v[3]};
            m["uniforms"] = std::move(u);
        }
        mats.push_back(std::move(m));
    }
    return {{"version", 1}, {"model", modelFileId}, {"materials", std::move(mats)}};
}

MaterialEdits material_edits_from_json(const nlohmann::json& j) {
    if (!j.is_object()) throw std::invalid_argument("material edits must be a JSON object");
    MaterialEdits out;
    const auto mats = j.find("materials");
    if (mats == j.end() || !mats->is_array()) return out;
    for (const auto& m : *mats) {
        if (!m.is_object() || !m.contains("index") || !m["index"].is_number_unsigned()) continue;
        MaterialEdit e;
        e.index = m["index"].get<uint32_t>();
        if (auto f = m.find("from"); f != m.end() && f->is_object()) {
            e.fromModel = f->value("model", 0u);
            e.fromMaterial = f->value("material", 0u);
        }
        e.amatFile = m.value("amat", 0u);
        e.blended = m.value("blended", false);
        if (auto t = m.find("textures"); t != m.end() && t->is_object())
            for (const auto& [slot, fileId] : t->items())
                if (fileId.is_number_unsigned()) e.textures[std::atoi(slot.c_str())] = fileId.get<uint32_t>();
        if (auto u = m.find("uniforms"); u != m.end() && u->is_object())
            for (const auto& [key, v] : u->items()) {
                if (!v.is_array() || v.size() != 4) continue;
                std::array<float, 4> a{};
                bool numbers = true;
                for (size_t k = 0; k < 4; ++k) {
                    numbers &= v[k].is_number();
                    if (numbers) a[k] = v[k].get<float>();
                }
                if (numbers) e.uniforms[key] = a;
            }
        if (!e.empty()) out.push_back(std::move(e));
    }
    return out;
}

} // namespace castlemist::extract
