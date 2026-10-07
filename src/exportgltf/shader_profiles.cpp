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
    t.push_back({{561567, 511755, 510615, 1203843, 2212806}, make("weapon-glow", AlphaUse::HolesAndShine, true)});
    // Same lit core, but the glow is an animated legendary effect (still supported;
    // the animated layers go out raw with a warning).
    {
        ShaderProfile p = make("weapon-glow-legendary", AlphaUse::HolesAndShine, true);
        p.animatedGlowLayers = true;
        t.push_back({{2083141, 2140066}, p});
    }

    {
        ShaderProfile p = make("weapon-spec", AlphaUse::HolesAndShine, true);
        p.specLayer = SpecLayer::GlossInAlpha;
        p.glowOnUv2MaskOnUv0 = true;
        t.push_back({{2348484, 3423592}, p});
    }

    // legacy-spec: shine is reflection only; the spec layer's alpha is the exponent.
    {
        ShaderProfile p = make("legacy-spec", AlphaUse::ReflectionOnly, false);
        p.specLayer = SpecLayer::ExponentInAlpha;
        t.push_back({{13822, 13831, 1749692, 2069382}, p});

        ShaderProfile clip = p;
        clip.clips = true;
        t.push_back({{1891783, 13361}, clip});

        // On these the mask is R = glow gate (G = glow-perturb gate), not metal/gloss.
        clip.maskGlowGate = Channel::R;
        t.push_back({{14149, 14213, 14165}, clip});
    }

    t.push_back({{13843, 13856, 13864, 14003, 31327, 32657, 34181, 44707, 44708, 72583, 27352,
                  19911, 47468, 47469,
                  // second survey (note section 8.3/8.4), hand-read or identical opcode stream
                  20041, 62080, 56795, 69668, 57606, 75035, 57714, 57752, 53237, 16104, 27303,
                  57685, 60319, 58654},
                 make("prop-lit", AlphaUse::HolesAndShine, true)});
    t.push_back({{15999, 54592, 57634, 57715, 27353,
                  14084, 23508, 73205, 73655, 84923, 27305, 76858, 23672, 54889, 231183, 57701,
                  75778},
                 make("prop-lit-noclip", AlphaUse::Shine, false)});
    // subsurface-decal: albedo = lerp(decal, diffuse, decal.a) (note section 4).
    {
        ShaderProfile p = make("subsurface-decal", AlphaUse::Shine, false);
        p.decalMode = DecalMode::DiffuseOverDecal;
        p.decalParallax = true;  // decal UV offset by pardist
        t.push_back({{54632}, p});
    }

    // prop-decal (note section 8.4): albedo = lerp(diffuse, decal, saturate(2*decal.a)
    // [x mask]), shine lerped toward saturate(2*decal.a-1); no texture discard. Hand-read
    // ids only (signature-only 76643, 81309, 62170, 69623 are left out).
    {
        ShaderProfile p = make("prop-decal", AlphaUse::Shine, false);
        p.decalMode = DecalMode::DecalOverDiffuse;
        t.push_back({{56533, 60027, 62212, 44479, 60145, 69913, 79884, 69713, 2597095}, p});

        ShaderProfile m = p;
        m.decalMaskRole = "decalmask";
        m.decalMaskChannel = Channel::R;
        t.push_back({{19910, 525886}, m});
        m.decalMaskRole = "mask";
        m.maskSpecular = Channel::B;  // 69887 also multiplies the specular by mask.B
        t.push_back({{69887}, m});
        m.maskSpecular = Channel::None;
        m.decalMaskRole = "blend";
        m.decalMaskChannel = Channel::G;
        t.push_back({{60530}, m});

        // 57806 also adds decal.rgb * saturate(2*decal.a-1) unlit.
        ShaderProfile g = p;
        g.decalGlow = DecalGlow::AboveHalf;
        t.push_back({{57806}, g});
    }

    // decal-glow (note section 8, 57131): albedo = lerp(decal, diffuse, decal.a);
    // emission = decal.rgb * glowcol * 2 * (1 - decal.a).
    {
        ShaderProfile p = make("decal-glow", AlphaUse::Shine, false);
        p.decalMode = DecalMode::DiffuseOverDecal;
        p.decalGlow = DecalGlow::BelowHalf;
        p.decalParallax = true;  // decal rgb at a pardist/decptrb-offset UV
        t.push_back({{57131}, p});
    }

    // Second survey (docs/research/gw2-material-channels.md section 8.4): new profiles
    // that fit the existing fields.
    // prop-spec: the weapon-spec layout without the glow (spec.A = gloss).
    {
        ShaderProfile p = make("prop-spec", AlphaUse::HolesAndShine, true);
        p.specLayer = SpecLayer::GlossInAlpha;
        t.push_back({{1729747}, p});
    }
    // prop-metalmask (note section 8.3): the 561567 lit core; metal = metalmask.G
    // (t2), mod x2 on UV1.
    {
        ShaderProfile p = make("prop-metalmask", AlphaUse::HolesAndShine, true);
        p.maskRole = "metalmask";
        p.maskMetal = Channel::G;
        t.push_back({{3121953}, p});
    }
    t.push_back({{77876}, make("prop-unlit-holes", AlphaUse::Unused, true)});
    t.push_back({{77598, 189570}, make("prop-diffuse-only", AlphaUse::Unused, false)});

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
                      55903, 882285, 217286, 87345, 1053007},
                     p});
    }
    t.push_back({{20760, 23408, 54721, 14196, 19116, 53260, 217998, 48767, 1171330},
                 make("fx-premultiplied", AlphaUse::Intensity, false)});
    t.push_back({{740364, 965703, 977200, 1171331}, unsupported(make("fx-fire", AlphaUse::Unused, false))});
    t.push_back({{709206, 47396, 27304, 339341, 630598},
                 unsupported(make("fx-distort", AlphaUse::Unused, false))});

    // fx-alpha: straight opacity in alpha (SrcA/InvSrcA), rgb not premultiplied.
    t.push_back({{23507, 19255}, make("fx-alpha", AlphaUse::Opacity, false)});
    // fx-alpha-glow (note section 8.2, 44709): alpha = opacity below half, self-illumination above.
    t.push_back({{44709}, make("fx-alpha-glow", AlphaUse::OpacityAndGlow, false)});
    // armor-prism: shine, animated flake/prism layers raw; mask G/R are layer weights.
    {
        ShaderProfile p = make("armor-prism", AlphaUse::Shine, false);
        p.animatedGlowLayers = true;
        t.push_back({{2329259}, p});
    }
    // Cutout layers (note sections 8.2, 8.3): the discard is on a `cutout` layer on its
    // own UV, shipped as the alphaMask. weapon-cutout-glow: cutout.R*A*cutfade < 0.5 (a
    // dissolve; never cuts at rest on 3123167/1823422); the diffuse alpha is shine only,
    // so `clips` (diffuse holes) stays false and alpha_tested() comes from the cutout.
    {
        ShaderProfile p = make("weapon-cutout-glow", AlphaUse::Shine, false);
        p.cutoutRole = "cutout";
        p.cutoutChannels = CutoutChannels::RxA;
        t.push_back({{511663}, p});
    }
    // prop-cutout (53858, "cutout + mod"): discard cutout.R * saturate(2*diffA) < 0.5
    // (cutout on UV2), shine, mod x2 (an extra).
    {
        ShaderProfile p = make("prop-cutout", AlphaUse::HolesAndShine, true);
        p.cutoutRole = "cutout";
        p.cutoutChannels = CutoutChannels::R;
        t.push_back({{53858}, p});
    }
    // fx-parallax-layer (note section 8.2, 842652, Astralaria): colour = the `parallax`
    // layer (UV0) at a UV offset by the castlemist "diffuse" (UV3, x paraper) and
    // the view parallax (pardist); opacity = parallax.A x mask.R (UV1, offset by
    // mskptrb x cutptrb) x diffade; SrcA/InvSrcA, AlphaRef fade only; unlit.
    {
        ShaderProfile p = make("fx-parallax-layer", AlphaUse::Opacity, false);
        p.baseColorRole = "parallax";
        p.diffuseUse = "uv-offset";
        p.opacityRole = "mask";
        p.opacityChannel = Channel::R;
        p.unlit = true;
        t.push_back({{842652}, p});
    }
    // Unsupported: need their own pass.
    t.push_back({{157432, 3718974, 15206},
                 unsupported(make("fx-multiply", AlphaUse::Unused, false))});
    t.push_back({{221571}, unsupported(make("fx-cubemap", AlphaUse::Unused, false))});
    t.push_back({{49659, 63923, 57026},
                 unsupported(make("glass-refract", AlphaUse::Unused, false))});
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

std::vector<uint32_t> all_profile_amats() {
    std::vector<uint32_t> ids;
    for (const Entry& e : table()) ids.insert(ids.end(), e.amats.begin(), e.amats.end());
    return ids;
}

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
