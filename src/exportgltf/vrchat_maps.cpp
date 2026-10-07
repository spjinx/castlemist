/// @file
/// @brief build_material_maps: GW2 material layers -> Poiyomi maps (BaseColor,
///        Normal, Packed, emission, distortion, raw extras), per shader profile.

#include "castlemist/exportgltf/vrchat_maps.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace castlemist::exportgltf {

namespace {

/// Unity wants OpenGL-style (Y+) normal maps; the atlas bake already flips GW2's
/// green and was checked in Blender. Spec "Open questions": one switch.
const bool kFlipNormalGreen = true;

using Rgba = std::array<uint8_t, 4>;

std::string channel_name(Channel c) {
    static const char* const kNames[] = {"R", "G", "B", "A"};
    return kNames[static_cast<int>(c)];
}

std::array<float, 3> rgb(const std::array<float, 4>& v) { return {v[0], v[1], v[2]}; }

/// A source texture as the builder sees it. A 4x4 uniform texture is a
/// placeholder (13368 white, 529633 flat normal, ...): it stands for its
/// colour and is never written as a map.
struct Layer {
    const ModelTextureCPU* tex = nullptr;
    uint8_t uv = 0;
    uint32_t fileId = 0;
    std::string role;
    bool placeholder = false;
    Rgba constant{};

    bool usable() const { return tex != nullptr; }
    bool real() const { return tex != nullptr && !placeholder; }

    /// Channel `c` at (x, y) of a `w` x `h` output, nearest-sampled.
    uint8_t at(int x, int y, int w, int h, int c) const {
        if (placeholder) return constant[static_cast<size_t>(c)];
        int sx = std::min(tex->width - 1, x * tex->width / w);
        int sy = std::min(tex->height - 1, y * tex->height / h);
        return tex->rgba[(static_cast<size_t>(sy) * static_cast<size_t>(tex->width) +
                          static_cast<size_t>(sx)) * 4 + static_cast<size_t>(c)];
    }
};

bool decoded(const ModelTextureCPU& t) {
    return t.width > 0 && t.height > 0 &&
           t.rgba.size() >= static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4;
}

bool is_placeholder(const ModelTextureCPU& t) {
    if (t.width != 4 || t.height != 4) return false;
    size_t n = static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * 4;
    for (size_t i = 4; i < n; ++i)
        if (t.rgba[i] != t.rgba[i % 4]) return false;
    return true;
}

uint8_t to_byte(double v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
}

/// Shine from a diffuse alpha: `saturate(2a - 1)`.
uint8_t shine(uint8_t a) { return to_byte(2.0 * a / 255.0 - 1.0); }

ModelTextureCPU blank(int w, int h, uint32_t fileId) {
    ModelTextureCPU t;
    t.fileId = fileId;
    t.width = w;
    t.height = h;
    t.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    return t;
}

MapSlot slot_of(ModelTextureCPU tex, uint8_t uv, uint32_t fileId, std::string source) {
    MapSlot s;
    s.tex = std::move(tex);
    s.uv = uv;
    s.fileId = fileId;
    s.source = std::move(source);
    s.present = true;
    return s;
}

/// A raw copy of a layer as a map.
MapSlot raw_slot(const Layer& l, std::string source) {
    return slot_of(*l.tex, l.uv, l.fileId, std::move(source));
}

std::array<double, 3> average_colour(const ModelTextureCPU& t) {
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i + 3 < t.rgba.size(); i += 4)
        for (int c = 0; c < 3; ++c) sum[c] += t.rgba[i + static_cast<size_t>(c)];
    double peak = std::max({sum[0], sum[1], sum[2]});
    if (peak <= 0) return {1.0, 1.0, 1.0};
    return {sum[0] / peak, sum[1] / peak, sum[2] / peak};
}

const char* extra_use(const std::string& role, const ShaderProfile& profile) {
    if (role == "mod") return "detail-multiply2x";
    if (role == "decal") return "lerp-by-decal-alpha";
    if (role == "height") return "parallax";
    if (role == "specular") return "specular-color";
    if (role == "mask" && profile.maskSheen != Channel::None) return "sheen-in-B";
    return "";
}

class Builder {
public:
    Builder(const ModelPreview& model, const ModelMaterialCPU& mat, const BlendInfo& blend,
            const ShaderProfile& profile)
        : model_(model), mat_(mat), blend_(blend), profile_(profile) {}

    MaterialMaps run() {
        out_.metalSource = out_.smoothSource = out_.reflectionSource = out_.specularSource = "none";
        collect_layers();
        if (const auto v = vec("speccp")) out_.specularTint = rgb(*v);
        if (const auto v = vec("envcr"))
            out_.reflectionTint = rgb(*v);
        else if (const auto w = vec("envcp"))
            out_.reflectionTint = rgb(*w);

        if (!profile_.supported) {
            out_.warnings.push_back("profile '" + profile_.name +
                                    "' is not mapped (needs its own pass): diffuse, normal and "
                                    "every layer exported raw, channels not interpreted");
            if (diffuse_.real()) out_.baseColor = raw_slot(diffuse_, "diffuse");
            if (normal_.real()) out_.normal = raw_slot(normal_, "normal");
            for (const Layer& l : layers_)
                if (l.real()) out_.extras.push_back({l.role, "", raw_slot(l, l.role)});
            warn_uvs();
            return std::move(out_);
        }

        build_base_color();
        build_normal();
        build_packed();
        build_emission();
        build_distortion();
        build_extras();
        warn_uvs();
        return std::move(out_);
    }

private:
    // ------------------------------------------------------------ sources --

    /// Resolve a texture index into a Layer; a failed decode or a placeholder warns.
    Layer resolve(const std::string& what, int texIndex, uint8_t uv, uint32_t fileId) {
        Layer l;
        l.uv = uv;
        l.role = what;
        const ModelTextureCPU* t =
            texIndex >= 0 && texIndex < static_cast<int>(model_.textures.size())
                ? &model_.textures[static_cast<size_t>(texIndex)]
                : nullptr;
        if (t && fileId == 0) fileId = t->fileId;
        l.fileId = fileId;
        if (!t || !decoded(*t)) {
            out_.warnings.push_back(what + " layer (fileId " + std::to_string(fileId) +
                                    ") failed to decode: map skipped");
            return l;
        }
        l.tex = t;
        if (is_placeholder(*t)) {
            l.placeholder = true;
            for (size_t c = 0; c < 4; ++c) l.constant[c] = t->rgba[c];
            out_.warnings.push_back(
                what + " layer (fileId " + std::to_string(fileId) + ") is a " +
                std::to_string(t->width) + "x" + std::to_string(t->height) +
                " placeholder for the constant (" + std::to_string(l.constant[0]) + ", " +
                std::to_string(l.constant[1]) + ", " + std::to_string(l.constant[2]) + ", " +
                std::to_string(l.constant[3]) + "): used as a value, not written as a map");
        }
        return l;
    }

    void collect_layers() {
        if (mat_.diffuseTex >= 0) diffuse_ = resolve("diffuse", mat_.diffuseTex, mat_.diffuseUv, 0);
        if (mat_.normalTex >= 0) normal_ = resolve("normal", mat_.normalTex, mat_.normalUv, 0);
        for (const auto& x : mat_.extraTextures) {
            Layer l = resolve(x.role.empty() ? std::string("unnamed") : x.role, x.texIndex,
                              x.uvIndex, x.fileId);
            if (l.usable()) layers_.push_back(std::move(l));
        }
        // A baked glow (e.g. a sylvari's pattern) stands in for a glow layer.
        if (mat_.emissiveTex >= 0) {
            Layer l = resolve("emissive", mat_.emissiveTex, mat_.diffuseUv, 0);
            if (l.usable()) layers_.push_back(std::move(l));
        }
        if (mat_.metalRoughTex >= 0) {
            Layer l = resolve("metalRough", mat_.metalRoughTex, mat_.diffuseUv, 0);
            if (l.usable()) layers_.push_back(std::move(l));
        }
    }

    /// First usable layer with one of the roles, or nullptr.
    const Layer* find(std::initializer_list<const char*> roles) const {
        for (const Layer& l : layers_)
            for (const char* r : roles)
                if (l.role == r) return &l;
        return nullptr;
    }

    std::optional<float> constant(const char* name) const {
        for (const auto& [n, v] : mat_.namedConstants)
            if (n == name) return v;
        for (const auto& [n, v] : mat_.namedConstantVectors)
            if (n == name) return v[0];
        return std::nullopt;
    }

    std::optional<std::array<float, 4>> vec(const char* name) const {
        for (const auto& [n, v] : mat_.namedConstantVectors)
            if (n == name) return v;
        return std::nullopt;
    }

    bool diffuse_has_shine() const {
        return diffuse_.usable() && (profile_.diffuseAlpha == AlphaUse::HolesAndShine ||
                                     profile_.diffuseAlpha == AlphaUse::Shine);
    }

    bool reads_mask() const {
        return profile_.maskMetal != Channel::None || profile_.maskGloss != Channel::None ||
               profile_.maskSheen != Channel::None || profile_.maskGlow != Channel::None ||
               profile_.maskGlowGate != Channel::None;
    }

    // ------------------------------------------------------------- outputs --

    void build_base_color() {
        if (!diffuse_.real()) return;
        const ModelTextureCPU& d = *diffuse_.tex;
        const int w = d.width, h = d.height;

        Layer opacity;
        if (profile_.opacityTexture >= 0) {
            const auto idx = static_cast<size_t>(profile_.opacityTexture);
            const std::string what = "opacity (texture " + std::to_string(idx) + ")";
            int texIndex = -1;
            uint32_t fid = idx < mat_.textureFileIds.size() ? mat_.textureFileIds[idx] : 0;
            for (size_t i = 0; fid != 0 && i < model_.textures.size(); ++i)
                if (model_.textures[i].fileId == fid) { texIndex = static_cast<int>(i); break; }
            // resolve warns when it is missing; the alpha then stays 255.
            opacity = resolve(what, texIndex, diffuse_.uv, fid);
        }

        ModelTextureCPU t = blank(w, h, d.fileId);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                                  static_cast<size_t>(x)) * 4;
                const uint8_t a = d.rgba[i + 3];
                for (size_t c = 0; c < 3; ++c)
                    t.rgba[i + c] = profile_.premultiplyRgbByAlpha
                                        ? static_cast<uint8_t>((d.rgba[i + c] * a + 127) / 255)
                                        : d.rgba[i + c];
                uint8_t outA = 255;
                if (profile_.opacityTexture >= 0) {
                    if (opacity.usable()) outA = opacity.at(x, y, w, h, 0);
                } else if (profile_.diffuseAlpha == AlphaUse::Intensity ||
                           profile_.diffuseAlpha == AlphaUse::Opacity) {
                    outA = a;
                } else if (profile_.clips) {
                    outA = a < 64 ? 0 : 255;
                }
                t.rgba[i + 3] = outA;
            }
        out_.baseColor = slot_of(std::move(t), diffuse_.uv, d.fileId, "diffuse");
    }

    void build_normal() {
        if (!normal_.real()) return;
        const ModelTextureCPU& n = *normal_.tex;
        ModelTextureCPU t = blank(n.width, n.height, n.fileId);
        for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
            const double x = n.rgba[i] / 255.0 * 2.0 - 1.0;
            const double y = n.rgba[i + 1] / 255.0 * 2.0 - 1.0;
            const double z = std::sqrt(std::max(0.0, 1.0 - x * x - y * y));
            t.rgba[i] = n.rgba[i];
            t.rgba[i + 1] =
                kFlipNormalGreen ? static_cast<uint8_t>(255 - n.rgba[i + 1]) : n.rgba[i + 1];
            t.rgba[i + 2] = to_byte(z * 0.5 + 0.5);
            t.rgba[i + 3] = 255;
        }
        out_.normal = slot_of(std::move(t), normal_.uv, n.fileId, "normal");
    }

    /// R metal, G smooth, B reflection mask, A specular mask.
    void build_packed() {
        const Layer* mask = reads_mask() ? find({"mask"}) : nullptr;
        const Layer* spec =
            profile_.specLayer == SpecLayer::GlossInAlpha ? find({"specular"}) : nullptr;
        const bool shineGA = diffuse_has_shine();
        const bool shineB = diffuse_.usable() && profile_.diffuseAlpha == AlphaUse::ReflectionOnly;

        // Each channel: a per-texel source, or a constant.
        enum class Src { Const, Mask, SpecA, Shine };
        struct Ch { Src src = Src::Const; int channel = 0; uint8_t value = 0; };
        Ch ch[4];

        if (mask && profile_.maskMetal != Channel::None) {
            ch[0] = {Src::Mask, static_cast<int>(profile_.maskMetal), 0};
            out_.metalSource = "mask." + channel_name(profile_.maskMetal);
        } else if (const auto m = constant("mtlness")) {
            ch[0].value = to_byte(*m);
            out_.metalSource = "mtlness";
        } else if (const auto c = constant("conduct")) {
            ch[0].value = to_byte(*c);
            out_.metalSource = "conduct";
        }

        if (mask && profile_.maskGloss != Channel::None) {
            ch[1] = {Src::Mask, static_cast<int>(profile_.maskGloss), 0};
            out_.smoothSource = "mask." + channel_name(profile_.maskGloss);
        } else if (spec) {
            ch[1] = {Src::SpecA, 3, 0};
            out_.smoothSource = "specular.A";
        } else if (shineGA) {
            ch[1].src = Src::Shine;
            out_.smoothSource = "diffuseAlpha";
        } else if (const auto s = constant("specstr")) {
            ch[1].value = to_byte(*s);
            out_.smoothSource = "specstr";
        } else {
            ch[1].value = 128;
        }

        if (shineB) {
            ch[2].src = Src::Shine;
            out_.reflectionSource = "diffuseAlpha";
        } else {
            for (const char* name : {"envcr", "envcp", "envstr"})
                if (constant(name)) {
                    ch[2].value = 255;
                    out_.reflectionSource = name;
                    break;
                }
        }

        if (shineGA) {
            ch[3].src = Src::Shine;
            out_.specularSource = "diffuseAlpha";
        } else {
            ch[3].value = 255;
        }

        if (out_.metalSource == "none" && out_.smoothSource == "none" &&
            out_.reflectionSource == "none" && out_.specularSource == "none")
            return;  // nothing known: Poiyomi's slider defaults do as well

        // Packed takes the diffuse size; mask/specular channels are nearest-sampled.
        int w = 4, h = 4;
        for (const Layer* l : std::initializer_list<const Layer*>{&diffuse_, mask, spec})
            if (l && l->real()) { w = l->tex->width; h = l->tex->height; break; }

        ModelTextureCPU t = blank(w, h, 0);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                                  static_cast<size_t>(x)) * 4;
                for (size_t c = 0; c < 4; ++c) {
                    const Ch& k = ch[c];
                    switch (k.src) {
                        case Src::Const: t.rgba[i + c] = k.value; break;
                        case Src::Mask: t.rgba[i + c] = mask->at(x, y, w, h, k.channel); break;
                        case Src::SpecA: t.rgba[i + c] = spec->at(x, y, w, h, 3); break;
                        case Src::Shine: t.rgba[i + c] = shine(diffuse_.at(x, y, w, h, 3)); break;
                    }
                }
            }
        const uint8_t uv = diffuse_.usable() ? diffuse_.uv : mask ? mask->uv : spec ? spec->uv : 0;
        out_.packed = slot_of(std::move(t), uv, 0, "packed");
        if (mask) consumed_.push_back(mask);
    }

    /// One channel of a layer as a greyscale map.
    MapSlot grey_slot(const Layer& l, Channel c) {
        const ModelTextureCPU& s = *l.tex;
        ModelTextureCPU t = blank(s.width, s.height, s.fileId);
        for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
            const uint8_t v = s.rgba[i + static_cast<size_t>(c)];
            t.rgba[i] = t.rgba[i + 1] = t.rgba[i + 2] = v;
            t.rgba[i + 3] = 255;
        }
        return slot_of(std::move(t), l.uv, l.fileId, l.role + "." + channel_name(c));
    }

    void build_emission() {
        const Layer* glow = find({"glow"});
        if (!glow) glow = find({"emissive"});
        const Layer* glowmask = find({"glowmask"});
        const Layer* mask = find({"mask"});

        if (profile_.glowOnUv2MaskOnUv0 && glow && glowmask && (glow->uv != 2 || glowmask->uv != 0))
            out_.warnings.push_back("profile '" + profile_.name +
                                    "' expects glow on UV2 and glowmask on UV0, material has "
                                    "glow UV" +
                                    std::to_string(glow->uv) + " and glowmask UV" +
                                    std::to_string(glowmask->uv) + ": kept the material's own");

        if (glowmask && glowmask->real()) {
            out_.emissionMask = raw_slot(*glowmask, "glowmask");
            consumed_.push_back(glowmask);
        } else if (mask && mask->real() && profile_.maskGlow != Channel::None) {
            out_.emissionMask = grey_slot(*mask, profile_.maskGlow);
        } else if (mask && mask->real() && profile_.maskGlowGate != Channel::None) {
            out_.emissionMask = grey_slot(*mask, profile_.maskGlowGate);
        }
        if (glowmask && glowmask->placeholder) consumed_.push_back(glowmask);

        const bool additive = blend_.nearest == BlendPreset::Additive ||
                              blend_.nearest == BlendPreset::SoftAdditive;
        if (glow && glow->real()) {
            out_.emissionMap = raw_slot(*glow, glow->role);
            consumed_.push_back(glow);
        } else if ((out_.emissionMask.present || additive) && out_.baseColor.present) {
            out_.emissionMap = out_.baseColor;
            out_.emissionMap.source = "baseColor";
        }
        if (glow && glow->placeholder) consumed_.push_back(glow);

        if (out_.emissionMap.present) {
            const auto c = average_colour(out_.emissionMap.tex);
            out_.emissionColor = {static_cast<float>(c[0]), static_cast<float>(c[1]),
                                  static_cast<float>(c[2])};
        }

        // The .glb's emissiveTexture: mask x the map's average colour, else the map.
        if (out_.emissionMask.present) {
            ModelTextureCPU t = out_.emissionMask.tex;
            for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
                for (size_t c = 0; c < 3; ++c)
                    t.rgba[i + c] = to_byte(t.rgba[i + c] / 255.0 * out_.emissionColor[c]);
                t.rgba[i + 3] = 255;
            }
            out_.emissionBaked =
                slot_of(std::move(t), out_.emissionMask.uv, 0, "emissionMask x emissionColor");
        } else if (out_.emissionMap.present) {
            out_.emissionBaked = out_.emissionMap;
        }
    }

    void build_distortion() {
        const Layer* p = find({"glowperturb", "perturb"});
        if (!p) return;
        consumed_.push_back(p);
        if (p->real()) out_.distortion = raw_slot(*p, p->role);
    }

    void build_extras() {
        for (const Layer& l : layers_) {
            const bool used = std::find(consumed_.begin(), consumed_.end(), &l) != consumed_.end();
            const std::string use = extra_use(l.role, profile_);
            if (!l.real() || (used && use.empty())) continue;
            out_.extras.push_back({l.role, use, raw_slot(l, l.role)});
        }
    }

    void warn_uvs() {
        auto check = [&](const char* name, const MapSlot& s) {
            if (!s.present) return;
            const std::string what =
                std::string(name) + " (" + s.source + ") uses UV" + std::to_string(s.uv);
            if (s.uv > 3)
                out_.warnings.push_back(what + ": Poiyomi textures read UV0-UV3 only");
            else if (s.uv >= 2)
                out_.warnings.push_back(what + ": check Poiyomi's UV choice");
        };
        check("baseColor", out_.baseColor);
        check("normal", out_.normal);
        check("packed", out_.packed);
        check("emissionMap", out_.emissionMap);
        check("emissionMask", out_.emissionMask);
        check("distortion", out_.distortion);
        for (const auto& x : out_.extras) check("extra", x.slot);
    }

    const ModelPreview& model_;
    const ModelMaterialCPU& mat_;
    const BlendInfo& blend_;
    const ShaderProfile& profile_;
    MaterialMaps out_;
    Layer diffuse_, normal_;
    std::vector<Layer> layers_;
    std::vector<const Layer*> consumed_;
};

}  // namespace

std::array<double, 3> glow_colour(const ModelPreview& model, int texIndex) {
    if (texIndex < 0 || texIndex >= static_cast<int>(model.textures.size())) return {1.0, 1.0, 1.0};
    return average_colour(model.textures[static_cast<size_t>(texIndex)]);
}

MaterialMaps build_material_maps(const ModelPreview& model, const ModelMaterialCPU& mat,
                                 const BlendInfo& blend, const ShaderProfile& profile) {
    return Builder(model, mat, blend, profile).run();
}

}  // namespace castlemist::exportgltf
