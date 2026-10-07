/// @file
/// @brief The AMAT fileId -> ShaderProfile table (docs/research/gw2-material-channels.md
///        sections 4 and 5) and the legacy-untagged trait.

#include "castlemist/exportgltf/shader_profiles.h"

#include <vector>

namespace castlemist::exportgltf {

namespace {

struct Entry {
    std::vector<uint32_t> amats;
    ShaderProfile profile;
};

ShaderProfile make(const char* name, AlphaUse alpha, bool clips) {
    ShaderProfile p;
    p.name = name;
    p.diffuseAlpha = alpha;
    p.clips = clips;
    return p;
}

ShaderProfile unsupported(ShaderProfile p) {
    p.supported = false;
    return p;
}

std::vector<Entry> build_table() {
    std::vector<Entry> t;

    // weapon-glow: lit core, holes < 64, shine saturate(2a-1).
    t.push_back({{561567, 511755, 510615}, make("weapon-glow", AlphaUse::HolesAndShine, true)});
    // Same lit core, but the glow is an animated legendary effect (still supported).
    t.push_back({{2083141, 2140066}, make("weapon-glow-legendary", AlphaUse::HolesAndShine, true)});

    {
        ShaderProfile p = make("weapon-spec", AlphaUse::HolesAndShine, true);
        p.specLayer = SpecLayer::GlossInAlpha;
        p.glowOnUv2MaskOnUv0 = true;
        t.push_back({{2348484}, p});
    }

    // legacy-spec: shine is reflection only; the spec layer's alpha is the exponent.
    {
        ShaderProfile p = make("legacy-spec", AlphaUse::ReflectionOnly, false);
        p.specLayer = SpecLayer::ExponentInAlpha;
        t.push_back({{13822, 13831, 1749692, 2069382}, p});

        ShaderProfile clip = p;
        clip.clips = true;
        t.push_back({{1891783}, clip});

        // On these the mask is R = glow gate (G = glow-perturb gate), not metal/gloss.
        clip.maskGlowGate = Channel::R;
        t.push_back({{14149, 14213, 14165}, clip});
    }

    t.push_back({{13843, 13856, 13864, 14003, 31327, 32657, 34181, 44707, 44708, 72583, 27352,
                  19911, 47468, 47469},
                 make("prop-lit", AlphaUse::HolesAndShine, true)});
    t.push_back({{15999, 54592, 57634, 57715, 27353},
                 make("prop-lit-noclip", AlphaUse::Shine, false)});
    t.push_back({{54632}, make("subsurface-decal", AlphaUse::Shine, false)});

    {
        ShaderProfile p = make("armor-mask", AlphaUse::HolesAndShine, true);
        p.maskMetal = Channel::R;
        p.maskGloss = Channel::G;
        p.maskSheen = Channel::B;
        p.maskGlow = Channel::A;
        t.push_back({{2449347, 2234037, 2777930}, p});
        p.name = "armor-mask-noglowA";
        p.maskGlow = Channel::None;
        t.push_back({{1171332, 1699091, 1674959}, p});
    }

    t.push_back({{2507831, 3252849},
                 unsupported(make("armor-silk", AlphaUse::HolesAndShine, true))});
    t.push_back({{2472137},
                 unsupported(make("jade-interior", AlphaUse::InteriorWeight, false))});

    // Legacy-untagged: tex0 diffuse, tex1 normal, tex2.R opacity. The AMATs here are
    // matched by id; any other material with the same traits is matched in profile_for.
    {
        ShaderProfile p = make("legacy-untagged", AlphaUse::Shine, true);
        p.opacityTexture = 2;
        t.push_back({{185120, 187842, 188779, 835943, 835971, 1255675, 2333606}, p});
    }

    // Effects: intensity in alpha, rgb premultiplied by it.
    {
        ShaderProfile p = make("fx-soft-additive", AlphaUse::Intensity, false);
        p.premultiplyRgbByAlpha = true;
        t.push_back({{19092, 23497, 57224, 23496, 21471, 46460, 25489, 43029, 45993, 49624, 55650,
                      55903, 882285, 217286},
                     p});
    }
    t.push_back({{20760, 23408, 54721, 14196, 19116, 53260, 217998},
                 make("fx-premultiplied", AlphaUse::Intensity, false)});
    t.push_back({{740364, 965703}, unsupported(make("fx-fire", AlphaUse::Unused, false))});
    t.push_back({{709206, 47396, 27304, 339341, 630598},
                 unsupported(make("fx-distort", AlphaUse::Unused, false))});
    return t;
}

const std::vector<Entry>& table() {
    static const std::vector<Entry> t = build_table();
    return t;
}

/// The legacy-untagged trait: SrcAlpha/InvSrcAlpha RGB factors (bits 12 and 16).
bool is_src_alpha_blend(uint64_t renderState) {
    return ((renderState >> 12) & 0xF) == 5 && ((renderState >> 16) & 0xF) == 6;
}

}  // namespace

const ShaderProfile& default_profile() {
    static const ShaderProfile p = [] {
        ShaderProfile d;
        d.name = "default";
        return d;
    }();
    return p;
}

const ShaderProfile& profile_for(const ModelMaterialCPU& mat, uint64_t renderState) {
    for (const Entry& e : table())
        for (uint32_t id : e.amats)
            if (id == mat.materialFile && id != 0) return e.profile;

    if (mat.materialId == 0 && mat.materialFlags == 0 && is_src_alpha_blend(renderState)) {
        bool allEmpty = true;
        for (const auto& x : mat.extraTextures)
            if (!x.role.empty()) { allEmpty = false; break; }
        if (allEmpty)
            for (const Entry& e : table())
                if (e.profile.name == "legacy-untagged") return e.profile;
    }
    return default_profile();
}

}  // namespace castlemist::exportgltf
