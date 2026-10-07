/// @file
/// @brief build_material_maps: GW2 material layers -> Poiyomi maps (BaseColor,
///        Normal, Packed, emission, distortion, decal, raw extras), per shader profile.

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

std::array<float, 3> average_rgb(const ModelTextureCPU& t) {
    const auto c = average_colour(t);
    return {static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])};
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
        if (isDefault_)
            out_.warnings.push_back("default profile used: AMAT " +
                                    std::to_string(mat_.materialFile) +
                                    " is not in the shader table, channels read generically; "
                                    "clip unknown: treated as not alpha-tested");
        collect_layers();
        classify_default_alpha();
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
        build_alpha_mask();
        build_normal();
        build_packed();
        build_decal();
        build_emission();
        build_alpha_glow();
        build_decal_glow();
        bake_emission();
        build_distortion();
        build_extras();
        warn_animated_glow();
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
        if (mat_.diffuseTex >= 0)
            diffuse_ = resolve("diffuse", mat_.diffuseTex, mat_.diffuseUv, 0);
        else if (profile_.supported)
            out_.warnings.push_back("no diffuse texture: BaseColor left out");
        if (mat_.normalTex >= 0) normal_ = resolve("normal", mat_.normalTex, mat_.normalUv, 0);
        for (const auto& x : mat_.extraTextures) {
            Layer l = resolve(x.role.empty() ? std::string("unnamed") : x.role, x.texIndex,
                              x.uvIndex, x.fileId);
            if (l.usable())
                layers_.push_back(std::move(l));
            else
                failedRoles_.push_back(l.role);
        }
        // A baked glow (e.g. a sylvari's pattern) stands in for a glow layer.
        if (mat_.emissiveTex >= 0) {
            Layer l = resolve("emissive", mat_.emissiveTex, mat_.diffuseUv, 0);
            if (l.usable())
                layers_.push_back(std::move(l));
            else
                failedRoles_.push_back(l.role);
        }
        if (mat_.metalRoughTex >= 0) {
            Layer l = resolve("metalRough", mat_.metalRoughTex, mat_.diffuseUv, 0);
            if (l.usable()) layers_.push_back(std::move(l));
        }
    }

    bool failed(const char* role) const {
        return std::find(failedRoles_.begin(), failedRoles_.end(), role) != failedRoles_.end();
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

    /// The default profile reads the diffuse alpha as shine only when that can
    /// be right: on a blended preset it is opacity (I4), and a uniform alpha
    /// carries no shine data at all (ruling R8). Profiled shaders are left alone.
    void classify_default_alpha() {
        if (!isDefault_ || !diffuse_.usable()) return;
        const bool blended = blend_.nearest != BlendPreset::Opaque &&
                             blend_.nearest != BlendPreset::Cutout;
        if (blended) {
            alphaIsOpacity_ = true;
            out_.warnings.push_back(
                "default profile on a blended preset: diffuse alpha kept as opacity, not shine");
            return;
        }
        uint8_t a = 0;
        if (uniform_alpha(diffuse_, a)) {
            alphaUnused_ = true;
            out_.warnings.push_back("diffuse alpha is uniform (" + std::to_string(a) +
                                    "): read as unused, not shine (default profile)");
        }
    }

    /// True when every texel of the layer has the same alpha (a placeholder always does).
    static bool uniform_alpha(const Layer& l, uint8_t& value) {
        if (l.placeholder) {
            value = l.constant[3];
            return true;
        }
        const auto& px = l.tex->rgba;
        value = px[3];
        for (size_t i = 7; i < px.size(); i += 4)
            if (px[i] != value) return false;
        return true;
    }

    bool diffuse_has_shine() const {
        if (alphaIsOpacity_ || alphaUnused_) return false;
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
                } else if (alphaIsOpacity_ || profile_.diffuseAlpha == AlphaUse::Intensity ||
                           profile_.diffuseAlpha == AlphaUse::Opacity) {
                    outA = a;
                } else if (profile_.diffuseAlpha == AlphaUse::OpacityAndGlow) {
                    outA = to_byte(2.0 * a / 255.0);
                } else if (profile_.clips) {
                    outA = a < 64 ? 0 : 255;
                }
                t.rgba[i + 3] = outA;
            }
        out_.baseColor = slot_of(std::move(t), diffuse_.uv, d.fileId, "diffuse");
    }

    /// The profile's cutout layer (511663, 53858) on its own UV: the clip value
    /// as a greyscale alphaMask, cutoff 0.5. A placeholder is a constant: it
    /// either never cuts (no map) or cuts the whole material (BaseColor A = 0).
    void build_alpha_mask() {
        if (profile_.cutoutRole.empty()) return;
        const std::string& role = profile_.cutoutRole;
        const bool rxa = profile_.cutoutChannels == CutoutChannels::RxA;
        const std::string source = rxa ? role + ".R x " + role + ".A" : role + ".R";
        const Layer* cut = find({role.c_str()});
        if (!cut) {
            // A failed decode already warned in resolve(); say what is lost either way.
            out_.warnings.push_back((failed(role.c_str()) ? role + " layer failed to decode"
                                                          : "no " + role + " layer") +
                                    ": alpha mask not built (the " + source +
                                    " < 0.5 clip is dropped)");
            return;
        }
        mapped_.push_back(cut);
        const auto value = [rxa](uint8_t r, uint8_t a) {
            return rxa ? static_cast<uint8_t>((r * a + 127) / 255) : r;
        };

        if (cut->placeholder) {
            const uint8_t v = value(cut->constant[0], cut->constant[3]);
            if (v >= 128) return;  // never cuts: nothing to map
            if (out_.baseColor.present)
                for (size_t i = 3; i < out_.baseColor.tex.rgba.size(); i += 4)
                    out_.baseColor.tex.rgba[i] = 0;
            out_.warnings.push_back(role + " layer is a placeholder with " + source + " = " +
                                    std::to_string(v) + "/255 < 0.5: it cuts the whole "
                                    "material (BaseColor alpha 0)");
            return;
        }

        const ModelTextureCPU& s = *cut->tex;
        ModelTextureCPU t = blank(s.width, s.height, s.fileId);
        bool neverCuts = true;
        for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
            const uint8_t v = value(s.rgba[i], s.rgba[i + 3]);
            t.rgba[i] = t.rgba[i + 1] = t.rgba[i + 2] = v;
            t.rgba[i + 3] = 255;
            if (v < 128) neverCuts = false;
        }
        out_.alphaMask = slot_of(std::move(t), cut->uv, cut->fileId, source);
        out_.alphaMaskCutoff = 0.5f;

        const auto fade = constant("cutfade");
        if (neverCuts)
            out_.warnings.push_back(
                source + " >= 0.5 on every texel: the cutout never cuts at rest, it is a "
                "dissolve" + (fade ? " driven by cutfade (" + std::to_string(*fade) + ")"
                                   : std::string()));
        if (fade && *fade != 1.0f)
            out_.warnings.push_back("cutfade (" + std::to_string(*fade) +
                                    ") not baked into the alphaMask");
        if (const auto p = constant("cutptrb"); p && *p != 0.0f)
            out_.warnings.push_back("cutout UV perturbation (cutptrb " + std::to_string(*p) +
                                    ") not mapped: the alphaMask is sampled unperturbed");
        if (profile_.clips)
            out_.warnings.push_back(
                "the game discards on " + source + " x saturate(2a) < 0.5; exported as "
                "BaseColor holes (saturate(2a) < 0.5) and the alphaMask (" + source +
                " < 0.5) tested apart, so texels where both are >= 0.5 but the product "
                "is < 0.5 are kept");
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
        // Gloss (weapon-spec) or exponent/128 (legacy-spec): either way the
        // specular layer's alpha is the smoothness.
        const Layer* spec = profile_.specLayer != SpecLayer::None ? find({"specular"}) : nullptr;
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
            out_.smoothSource = profile_.specLayer == SpecLayer::ExponentInAlpha
                                    ? "specular.A (exponent/128)"
                                    : "specular.A";
        } else if (shineGA) {
            ch[1].src = Src::Shine;
            out_.smoothSource = "diffuseAlpha";
        } else if (const auto s = constant("specstr")) {
            ch[1].value = to_byte(*s);
            out_.smoothSource = "specstr";
        } else {
            ch[1].value = 128;
            out_.smoothSource = "default";
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
            out_.specularSource = "default";
        }

        // Nothing read: Poiyomi's slider defaults do as well -- unless the
        // default profile set the shine aside on purpose (R8 / blended), where
        // the constants are the answer and the map records them.
        const auto unread = [](const std::string& s) { return s == "none" || s == "default"; };
        if (unread(out_.metalSource) && unread(out_.smoothSource) &&
            unread(out_.reflectionSource) && unread(out_.specularSource) &&
            !(alphaUnused_ || alphaIsOpacity_))
            return;

        // Packed follows the diffuse UV: a mask / specular layer on another one is misplaced.
        if (diffuse_.usable()) {
            const bool maskUsed = std::any_of(std::begin(ch), std::end(ch),
                                              [](const Ch& k) { return k.src == Src::Mask; });
            for (const Layer* l : {maskUsed ? mask : nullptr, spec})
                if (l && l->uv != diffuse_.uv)
                    out_.warnings.push_back(l->role + " layer uses UV" + std::to_string(l->uv) +
                                            ", the diffuse UV" + std::to_string(diffuse_.uv) +
                                            ": Packed follows the diffuse UV, its " + l->role +
                                            " channels may be misplaced");
        }

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
        // A glow layer that failed to decode is still a glow layer: never substitute.
        const bool glowFailed = !glow && (failed("glow") || failed("emissive"));
        const Layer* glowmask = find({"glowmask"});
        const Layer* mask = find({"mask"});

        if (profile_.glowOnUv2MaskOnUv0 && glow && glowmask && (glow->uv != 2 || glowmask->uv != 0))
            out_.warnings.push_back("profile '" + profile_.name +
                                    "' expects glow on UV2 and glowmask on UV0, material has "
                                    "glow UV" +
                                    std::to_string(glow->uv) + " and glowmask UV" +
                                    std::to_string(glowmask->uv) + ": kept the material's own");

        // Any glow source (even a failed or placeholder one) owns the emission:
        // a decal glow never replaces it.
        emissionTaken_ = glow || glowFailed || glowmask || failed("glowmask");

        // A uniform glowmask scales the whole emission; a black one switches it off.
        std::array<float, 3>& scale = emissionScale_;
        if (glowmask && glowmask->placeholder) {
            consumed_.push_back(glowmask);
            if (glowmask->constant[0] == 0 && glowmask->constant[1] == 0 &&
                glowmask->constant[2] == 0) {
                out_.warnings.push_back("glowmask layer (fileId " +
                                        std::to_string(glowmask->fileId) +
                                        ") is a black placeholder: no emission");
                if (glow) consumed_.push_back(glow);
                return;
            }
            for (size_t c = 0; c < 3; ++c) scale[c] = glowmask->constant[c] / 255.0f;
        }

        bool maskFromMaskGlow = false;
        if (glowmask && glowmask->real()) {
            out_.emissionMask = raw_slot(*glowmask, "glowmask");
            consumed_.push_back(glowmask);
        } else if (mask && mask->real() && profile_.maskGlow != Channel::None) {
            out_.emissionMask = grey_slot(*mask, profile_.maskGlow);
            maskFromMaskGlow = true;
        } else if (mask && mask->real() && profile_.maskGlowGate != Channel::None) {
            out_.emissionMask = grey_slot(*mask, profile_.maskGlowGate);
        }

        const bool additive = blend_.nearest == BlendPreset::Additive ||
                              blend_.nearest == BlendPreset::SoftAdditive;
        if (glow && glow->real()) {
            out_.emissionMap = raw_slot(*glow, glow->role);
            out_.emissionColor = average_rgb(out_.emissionMap.tex);
            consumed_.push_back(glow);
        } else if (glow) {  // placeholder: its colour, no map
            out_.emissionColor = {glow->constant[0] / 255.0f, glow->constant[1] / 255.0f,
                                  glow->constant[2] / 255.0f};
            consumed_.push_back(glow);
        } else if (!glowFailed && (maskFromMaskGlow || additive) && out_.baseColor.present) {
            // The base colour stands in only when there is no glow layer at all.
            out_.emissionMap = out_.baseColor;
            out_.emissionMap.source = "baseColor";
            out_.emissionColor = average_rgb(out_.emissionMap.tex);
        }
        for (size_t c = 0; c < 3; ++c) out_.emissionColor[c] *= scale[c];
        if (out_.emissionMap.present || out_.emissionMask.present) emissionTaken_ = true;
    }

    /// The decal on its own UV: RGB = decal.rgb, A = its coverage of the diffuse.
    /// Not baked into BaseColor (another UV) and its shine not into Packed.
    void build_decal() {
        if (profile_.decalMode == DecalMode::None) return;
        const Layer* decal = find({"decal"});
        // Missing or failed: resolve already warned. A placeholder: no map either.
        if (!decal || !decal->real()) return;
        const bool over = profile_.decalMode == DecalMode::DecalOverDiffuse;

        const Layer* mask = nullptr;
        const Layer* otherUvMask = nullptr;
        if (over && !profile_.decalMaskRole.empty() &&
            profile_.decalMaskChannel != Channel::None) {
            const std::string& role = profile_.decalMaskRole;
            mask = find({role.c_str()});
            // A placeholder is a constant: UV-independent, always multiplied in.
            if (mask && mask->real() && mask->uv != decal->uv) {
                // The shader samples it on its own UV (19910: TEXCOORD2, the decal
                // TEXCOORD1), so it cannot go into the decal's alpha: its own map.
                otherUvMask = mask;
                mask = nullptr;
            } else if (!mask && !failed(role.c_str())) {
                out_.warnings.push_back("no " + role + " layer: decal coverage not masked");
            }
        }

        const ModelTextureCPU& d = *decal->tex;
        const int w = d.width, h = d.height;
        ModelTextureCPU t = blank(w, h, d.fileId);
        const int mc = static_cast<int>(profile_.decalMaskChannel);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                                  static_cast<size_t>(x)) * 4;
                for (size_t c = 0; c < 3; ++c) t.rgba[i + c] = d.rgba[i + c];
                const uint8_t a = d.rgba[i + 3];
                uint8_t cov = over ? to_byte(2.0 * a / 255.0) : static_cast<uint8_t>(255 - a);
                if (mask) cov = static_cast<uint8_t>((cov * mask->at(x, y, w, h, mc) + 127) / 255);
                t.rgba[i + 3] = cov;
            }
        std::string source = over ? "decal saturate(2a)" : "decal 1-a";
        if (mask) source += " x " + mask->role + "." + channel_name(profile_.decalMaskChannel);
        if (otherUvMask) {
            source += " (x decalMask on UV" + std::to_string(otherUvMask->uv) + ")";
            out_.decalMask = grey_slot(*otherUvMask, profile_.decalMaskChannel);
            out_.decalMaskChannel = channel_name(profile_.decalMaskChannel);
            mapped_.push_back(otherUvMask);
            out_.warnings.push_back("decal coverage must be multiplied by maps.decalMask (UV" +
                                    std::to_string(otherUvMask->uv) + ")");
        }
        out_.decal = slot_of(std::move(t), decal->uv, decal->fileId, std::move(source));
        out_.decalMode = over ? "decal-over-diffuse" : "diffuse-over-decal";
        mapped_.push_back(decal);
        if (mask) mapped_.push_back(mask);
        if (over)
            out_.warnings.push_back("decal shine not mapped (decal is on UV" +
                                    std::to_string(decal->uv) + ")");
        if (profile_.decalParallax)
            out_.warnings.push_back("decal parallax not mapped (pardist): the decal is "
                                    "sampled without its view-dependent UV offset");
        if (profile_.maskSpecular != Channel::None && find({"mask"}))
            out_.warnings.push_back("specular x mask." + channel_name(profile_.maskSpecular) +
                                    " not mapped");
    }

    /// 44709: the upper half of the diffuse alpha is unlit self-illumination,
    /// `rgb * saturate(2a-1) * 2`. A real glow source keeps the emission slots.
    void build_alpha_glow() {
        if (profile_.diffuseAlpha != AlphaUse::OpacityAndGlow) return;
        out_.warnings.push_back("opacity ramp and diffade not mapped (opacity = saturate(2a) only)");
        if (!diffuse_.real()) return;
        if (emissionTaken_) {
            out_.warnings.push_back(
                "self-illumination (diffuse alpha above 0.5) not mapped: emission slot in use");
            return;
        }
        const ModelTextureCPU& d = *diffuse_.tex;
        ModelTextureCPU m = blank(d.width, d.height, d.fileId);
        for (size_t i = 0; i + 3 < m.rgba.size(); i += 4) {
            m.rgba[i] = m.rgba[i + 1] = m.rgba[i + 2] = shine(d.rgba[i + 3]);
            m.rgba[i + 3] = 255;
        }
        out_.emissionMask = slot_of(std::move(m), diffuse_.uv, d.fileId, "diffuse saturate(2a-1)");
        out_.emissionMap = out_.baseColor;
        out_.emissionMap.source = "baseColor";
        out_.emissionColor = {1, 1, 1};
        out_.emissionStrength = 2.0f;  // rgb * saturate(2a-1) * 2
        emissionTaken_ = true;
        decalGlowBake_ = true;  // bake map x mask x colour
    }

    /// A decal that glows (57131 below half its alpha, 57806 above), when no
    /// other source owns the emission.
    void build_decal_glow() {
        if (profile_.decalGlow == DecalGlow::None) return;
        const Layer* decal = find({"decal"});
        if (!decal || !decal->real()) return;
        if (emissionTaken_) {
            out_.warnings.push_back("decal glow not mapped: emission slot in use");
            return;
        }
        const bool below = profile_.decalGlow == DecalGlow::BelowHalf;
        out_.emissionMap = raw_slot(*decal, "decal");
        const ModelTextureCPU& d = *decal->tex;
        ModelTextureCPU m = blank(d.width, d.height, d.fileId);
        for (size_t i = 0; i + 3 < m.rgba.size(); i += 4) {
            const uint8_t a = d.rgba[i + 3];
            m.rgba[i] = m.rgba[i + 1] = m.rgba[i + 2] =
                below ? static_cast<uint8_t>(255 - a) : shine(a);
            m.rgba[i + 3] = 255;
        }
        out_.emissionMask = slot_of(std::move(m), decal->uv, decal->fileId,
                                    below ? "decal 1-a" : "decal saturate(2a-1)");
        out_.emissionColor = {1, 1, 1};
        out_.emissionStrength = 1.0f;
        if (below) {
            // decal.rgb * glowcol * 2: the colour at peak 1, the peak as strength.
            std::array<float, 3> c = {2, 2, 2};
            if (const auto g = vec("glowcol"))
                c = {2 * (*g)[0], 2 * (*g)[1], 2 * (*g)[2]};
            else
                out_.warnings.push_back("decal glow: no glowcol constant, white used");
            const float peak = std::max({c[0], c[1], c[2]});
            if (peak > 0) {
                out_.emissionColor = {c[0] / peak, c[1] / peak, c[2] / peak};
                out_.emissionStrength = peak;
            } else {
                out_.emissionColor = {0, 0, 0};
            }
        }
        emissionTaken_ = true;
        decalGlowBake_ = true;
    }

    void bake_emission() {
        const std::array<float, 3>& scale = emissionScale_;
        // The .glb's emissiveTexture: mask x emissionColor, else the map x the
        // uniform glowmask's scale.
        const MapSlot* src = out_.emissionMask.present  ? &out_.emissionMask
                             : out_.emissionMap.present ? &out_.emissionMap
                                                        : nullptr;
        if (!src) return;
        const bool fromMask = src == &out_.emissionMask;
        // A decal glow's map and mask are the same decal texture on one UV:
        // the bake carries the decal colour too.
        const ModelTextureCPU* map = decalGlowBake_ ? &out_.emissionMap.tex : nullptr;
        ModelTextureCPU t = src->tex;
        for (size_t i = 0; i + 3 < t.rgba.size(); i += 4) {
            for (size_t c = 0; c < 3; ++c)
                t.rgba[i + c] = to_byte(t.rgba[i + c] / 255.0 *
                                        (fromMask ? out_.emissionColor[c] : scale[c]) *
                                        (map ? map->rgba[i + c] / 255.0 : 1.0));
            t.rgba[i + 3] = 255;
        }
        out_.emissionBaked = slot_of(std::move(t), src->uv, fromMask ? 0 : src->fileId,
                                     map        ? "emissionMap x emissionMask x emissionColor"
                                     : fromMask ? "emissionMask x emissionColor"
                                                : src->source);
    }

    void build_distortion() {
        const Layer* p = find({"glowperturb", "perturb"});
        if (!p) return;
        consumed_.push_back(p);
        if (p->real()) out_.distortion = raw_slot(*p, p->role);
    }

    void build_extras() {
        for (const Layer& l : layers_) {
            if (std::find(mapped_.begin(), mapped_.end(), &l) != mapped_.end()) continue;
            const bool used = std::find(consumed_.begin(), consumed_.end(), &l) != consumed_.end();
            const std::string use = extra_use(l.role, profile_);
            if (!l.real() || (used && use.empty())) continue;
            out_.extras.push_back({l.role, use, raw_slot(l, l.role)});
        }
    }

    /// Legendary weapons replace the glow with an animated effect castlemist
    /// cannot express as maps; its layers are already raw extras, so say so.
    void warn_animated_glow() {
        if (!profile_.animatedGlowLayers) return;
        std::string roles;
        for (const auto& x : out_.extras) roles += (roles.empty() ? "" : ", ") + x.role;
        out_.warnings.push_back("animated legendary glow is not mapped (profile '" +
                                profile_.name + "'): its layers are exported raw as extras" +
                                (roles.empty() ? std::string() : " (" + roles + ")"));
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
        check("decal", out_.decal);
        check("decalMask", out_.decalMask);
        check("alphaMask", out_.alphaMask);
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
    std::vector<const Layer*> mapped_;      ///< fully mapped layers: never an extra
    std::array<float, 3> emissionScale_ = {1, 1, 1};  ///< a uniform glowmask's scale
    bool emissionTaken_ = false;            ///< a glow source owns the emission slots
    bool decalGlowBake_ = false;            ///< emission is a decal glow: bake map x mask x colour
    std::vector<std::string> failedRoles_;  ///< roles of layers that failed to decode
    const bool isDefault_ = &profile_ == &default_profile();
    bool alphaIsOpacity_ = false;  ///< default profile, blended preset: alpha is opacity (I4)
    bool alphaUnused_ = false;     ///< default profile, uniform alpha: no shine data (R8)
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
