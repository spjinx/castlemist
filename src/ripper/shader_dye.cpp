#include "castlemist/ripper/shader_dye.h"

#include <algorithm>
#include <cstring>

namespace castlemist::ripper {
namespace {

bool is_dye_uniform(const std::string& name) {
    for (const auto& ch : kDyeUniforms)
        for (const char* n : ch)
            if (name == n) return true;
    return false;
}

bool declares_dyes(const ModelMaterialCPU& m) {
    for (const auto& [name, v] : m.namedConstantVectors)
        if (is_dye_uniform(name)) return true;
    return false;
}

const ModelMaterialCPU::ExtraTexture* dye_mask(const ModelMaterialCPU& m) {
    for (const auto& e : m.extraTextures)
        if (e.role == "dyemask") return &e;
    return nullptr;
}

bool identity(const ShaderUniforms& u) {
    for (int ch = 0; ch < 4; ++ch)
        for (int row = 0; row < 3; ++row) {
            auto it = u.find(kDyeUniforms[ch][row]);
            if (it == u.end()) continue;
            for (int k = 0; k < 4; ++k)
                if (it->second[static_cast<size_t>(k)] != (k == row ? 1.0f : 0.0f)) return false;
        }
    return true;
}

// The diffuse with the dyes applied through the mask, as the springer's colour
// PS does it: per channel, lerp toward the channel's rows applied to the texel.
// The mask is sampled nearest at the same UV (it can be smaller than the diffuse).
ModelTextureCPU bake(const ModelTextureCPU& diffuse, const ModelTextureCPU& mask, const ShaderUniforms& u) {
    ModelTextureCPU out = diffuse;
    std::array<DyeRows, 4> rows{};
    for (size_t ch = 0; ch < 4; ++ch)
        for (size_t row = 0; row < 3; ++row) {
            auto it = u.find(kDyeUniforms[ch][row]);
            std::array<float, 4> v{};
            v[row] = 1;
            rows[ch][row] = it == u.end() ? v : it->second;
        }
    const int w = diffuse.width, h = diffuse.height;
    for (int y = 0; y < h; ++y) {
        const int my = y * mask.height / h;
        for (int x = 0; x < w; ++x) {
            const int mx = x * mask.width / w;
            const uint8_t* mk = mask.rgba.data() + (static_cast<size_t>(my) * mask.width + mx) * 4;
            uint8_t* px = out.rgba.data() + (static_cast<size_t>(y) * w + x) * 4;
            const float base[3] = {px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f};
            float c[3] = {base[0], base[1], base[2]};
            for (size_t ch = 0; ch < 4; ++ch) {
                const float wgt = mk[ch] / 255.0f;
                if (wgt <= 0) continue;
                for (size_t i = 0; i < 3; ++i) {
                    const auto& r = rows[ch][i];
                    const float d = r[0] * base[0] + r[1] * base[1] + r[2] * base[2] + r[3];
                    c[i] += (d - c[i]) * wgt;
                }
            }
            for (int i = 0; i < 3; ++i)
                px[i] = static_cast<uint8_t>(std::clamp(c[i] * 255.0f + 0.5f, 0.0f, 255.0f));
        }
    }
    return out;
}

} // namespace

DyeRows dye_shader_rows(const ColorMatrix& m) {
    // m maps (b, g, r, 1) in 0..255 to (b', g', r'). Output row i (RGB) is m's
    // row 2-i; input column j (RGB) is m's column 2-j.
    DyeRows r{};
    for (int i = 0; i < 3; ++i) {
        const auto& src = m[static_cast<size_t>(2 - i)];
        for (int j = 0; j < 3; ++j) r[static_cast<size_t>(i)][static_cast<size_t>(j)] = static_cast<float>(src[static_cast<size_t>(2 - j)]);
        r[static_cast<size_t>(i)][3] = static_cast<float>(src[3] / 255.0);
    }
    return r;
}

bool has_shader_dyes(const ModelPreview& model) {
    return std::any_of(model.materials.begin(), model.materials.end(), declares_dyes);
}

std::array<bool, 4> shader_dye_channels(const ModelPreview& model) {
    std::array<bool, 4> used{};
    for (const ModelMaterialCPU& m : model.materials) {
        const auto* e = declares_dyes(m) ? dye_mask(m) : nullptr;
        if (!e || e->texIndex < 0 || e->texIndex >= static_cast<int>(model.textures.size())) continue;
        const auto& px = model.textures[static_cast<size_t>(e->texIndex)].rgba;
        for (size_t i = 0; i + 3 < px.size(); i += 4)
            for (size_t ch = 0; ch < 4; ++ch)
                if (px[i + ch] > 8) used[ch] = true;
    }
    return used;
}

ShaderUniforms shader_dye_uniforms(const std::array<std::optional<ColorMatrix>, 4>& dyes) {
    ShaderUniforms u;
    for (size_t ch = 0; ch < 4; ++ch) {
        DyeRows rows{};
        if (dyes[ch]) rows = dye_shader_rows(*dyes[ch]);
        else
            for (size_t row = 0; row < 3; ++row) rows[row][row] = 1;
        for (size_t row = 0; row < 3; ++row) u[kDyeUniforms[ch][row]] = rows[row];
    }
    return u;
}

int set_shader_dyes(ModelPreview& model, const ModelPreview& pristine, const ShaderUniforms& uniforms) {
    model.textures = pristine.textures;
    model.materials = pristine.materials;
    model.gameMaterials = pristine.gameMaterials;

    // Shader mode: rewrite (or add) the constant at each dye uniform's offset.
    for (GameMaterial& g : model.gameMaterials)
        for (const GameShaderUniform& un : g.psUniforms) {
            auto it = uniforms.find(un.name);
            if (it == uniforms.end()) continue;
            auto c = std::find_if(g.psConsts.begin(), g.psConsts.end(),
                                  [&](const GameConstOverride& o) { return o.byteOff == un.byteOff; });
            if (c == g.psConsts.end()) c = g.psConsts.insert(g.psConsts.end(), GameConstOverride{un.byteOff, {}});
            std::memcpy(c->value, it->second.data(), sizeof c->value);
        }

    // Full mode: a dyed copy of each (diffuse, dyemask) pair, shared by the
    // materials that use the same pair. As authored needs no copy.
    const bool authored = identity(uniforms);
    std::map<std::pair<int, int>, int> baked;
    int changed = 0;
    for (ModelMaterialCPU& m : model.materials) {
        if (!declares_dyes(m)) continue;
        ++changed;
        const auto* e = dye_mask(m);
        const int n = static_cast<int>(model.textures.size());
        if (authored || !e || m.diffuseTex < 0 || m.diffuseTex >= n || e->texIndex < 0 || e->texIndex >= n ||
            e->uvIndex != m.diffuseUv)
            continue;
        const auto key = std::make_pair(m.diffuseTex, e->texIndex);
        auto it = baked.find(key);
        if (it == baked.end()) {
            const ModelTextureCPU& d = model.textures[static_cast<size_t>(m.diffuseTex)];
            const ModelTextureCPU& k = model.textures[static_cast<size_t>(e->texIndex)];
            if (d.rgba.size() < static_cast<size_t>(d.width) * d.height * 4 ||
                k.rgba.size() < static_cast<size_t>(k.width) * k.height * 4 || k.width <= 0 || k.height <= 0)
                continue;
            model.textures.push_back(bake(d, k, uniforms));
            it = baked.emplace(key, static_cast<int>(model.textures.size()) - 1).first;
        }
        m.diffuseTex = it->second;
    }
    return changed;
}

} // namespace castlemist::ripper
