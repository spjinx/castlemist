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

    // weapon-rim-ramp (1465623, Astral Ribbons): the 561567 lit core plus a view-angle
    // rim, ramp(N.V, mask.R + voffset) x mask.R (note 8.2); described, not mapped.
    {
        ShaderProfile p = make("weapon-rim-ramp", AlphaUse::HolesAndShine, true);
        p.rimRampRole = "ramp";
        p.rimMaskChannel = Channel::R;
        t.push_back({{1465623}, p});
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
    // prop-metalmask (note section 8.3): the 561567 lit core, mod x2 on UV1; metalmask.G (t2)
    // only lerps the spec/reflection tint, lerp(envcr.x, 0.6*albedo+0.2, metalmask.G) (not
    // mapped, warned). The diffuse is always drawn, so it is no PBR metal (2026-10-21: as
    // metal it turned the Mists vault's stairs near black); metalmask ships as an extra.
    {
        ShaderProfile p = make("prop-metalmask", AlphaUse::HolesAndShine, true);
        p.maskRole = "metalmask";
        p.maskTintsReflection = Channel::G;
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
        // 1478777 (Mists vault glow planes, 3132790; ps 22 read 2026-10-21): the same, One /
        // InvSrcColor, rgb = diffuse.rgb x a with a = diffuse.A x diffade x a view-angle ramp
        // (t1 at |N.V|, not mapped) x a soft depth fade (t12, not mapped).
        t.push_back({{1478777}, p});
        // 709501, 2434562 (Mists vault, 3124566; ps 31 read 2026-10-22): the same, One /
        // InvSrcColor, colour = diffuse x the vertex colour, a = its alpha x a soft depth
        // fade (depfade.x / dfadevl.x GW2 units), rgb x 2 x a.
        t.push_back({{709501, 2434562}, p});
    }
    // fx-flow (2591650 SrcA / One, 2759298 One / InvSrcA; ps 23 read 2026-10-22): the diffuse at
    // uv0 + (flow map at (uv0 + (time x flow.w, 0)) x flow.x) x 2 - 1) x |vc.r|^flow.z x flow.y,
    // vc = the vertex colour; a = smoothstep01((diffuse.a - vc.g) / (vc.b - vc.g)) x vc.a x a soft
    // depth fade (depfade.x); rgb = diffuse x lerp(startcl, endcl, smoothstep01 of (a -
    // stepcol.x) / (stepcol.y - stepcol.x)) x 2 (2759298: x diffuse.a x a, premultiplied).
    {
        ShaderProfile p = make("fx-flow", AlphaUse::Opacity, false);
        p.unlit = true;
        t.push_back({{2591650, 2759298}, p});
    }
    t.push_back({{20760, 23408, 54721, 14196, 19116, 53260, 217998, 48767, 1171330},
                 make("fx-premultiplied", AlphaUse::Intensity, false)});
    // fx-fire (read 2026-10-22; port_fire/SPEC.md). One / InvSrcA, premultiplied, unlit, procedural:
    //   740364 (ps 2): a fireshape silhouette (t0.G, UV0) and a firedetail noise (t1.G, UV1) displaced
    //     by two flow maps (t2, t3 at UV1) with a two-phase time blend, colour lerp(colorl, colorh,
    //     noise), alpha smoothstep(noise) x shape x ramp-scaled distortion (t4.R) x opacity, rgb x 3.24,
    //     a x 1.8, faded at silhouettes by the view-space normal and by depfade.
    //   965703 (ps 2): the same structure, one flow map tapped twice, ramp.G picks rampflw / rampstp,
    //     colour stepped by stepcol, o.a = K x premult (0 additive .. 1 premultiplied), x fadedif.
    //   977200 / 1171331 (ps 34, particles only): 4 x mask x noise x noise (scrolled by speed, per-
    //     particle uv1..3 offsets), perturbed by flow x perturb, coloured by colorl / colorh
    //     (1171331: x normalize(dye palette at dyedot)), x vertex colour x 2.
    // The diffuse is a data map, not colour: shipped raw as the extra "diffuse" (linear in Unity).
    {
        ShaderProfile p = make("fx-fire", AlphaUse::Opacity, false);
        p.unlit = true;
        p.baseColorRole = "none";      // no such layer: BaseColor left out, the diffuse goes out as an extra
        p.diffuseUse = "fire-shape";
        t.push_back({{740364, 965703, 977200, 1171331}, p});
    }
    // Screen distortion (read 2026-10-22): the grabbed scene at the pixel + (map.xy x 2 - 1) x
    // distort x the view-space reflection's xy, foreground taps rejected; the importer's
    // Castlemist/GW2 Distort. fx-distort-opaque (50159, ps 31): + distort as a bias, opaque.
    // fx-distort-opaque-mask (339341, ps 26): offset x mask(uv1).R, no view reflection, opaque.
    // fx-distort-mask (47396, ps 46): two normals blended, alpha = mask, AlphaRef discard.
    // fx-distort-soft (709206, ps 39, particles): alpha = mask.R x vertex alpha x soft fade
    // (depdist), colour x vertex colour x 2, no foreground test.
    // fx-distort-step (592660 ps 11, 27304 ps 31): alpha = step(mask.G; stepmin, stepcol)
    // [592660: x ramp x soft fade]; colour x modxcol x 2.
    // fx-distort-lit (630598, ps 11, character armour): diffuse x grab x 2 (offset = normal.xy x
    // 0.025 x reflection x distance fade), fresnel dim, sun transmission, env cube by gloss x
    // fresnel, spec^256 x gloss; alpha = saturate(2a), gloss = saturate(2a - 1).
    t.push_back({{50159}, make("fx-distort-opaque", AlphaUse::Unused, false)});
    t.push_back({{339341}, make("fx-distort-opaque-mask", AlphaUse::Unused, false)});
    t.push_back({{47396}, make("fx-distort-mask", AlphaUse::Unused, false)});
    t.push_back({{709206}, make("fx-distort-soft", AlphaUse::Unused, false)});
    t.push_back({{592660, 27304}, make("fx-distort-step", AlphaUse::Unused, false)});
    t.push_back({{630598}, make("fx-distort-lit", AlphaUse::Unused, false)});

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
        // fx-parallax-ramp (1507908, Mists vault walls; ps 32 read 2026-10-21): the same
        // layers -- colour = t3 "parallax" at UV0 + t0 offset x paraper - view x pardist,
        // opacity x mask.R (t1 at UV1 + t2 "mskptrb" offset) -- times a view-angle ramp
        // (t4 "ramp" at |N.V|, not mapped) and a soft depth fade (t12, not mapped).
        p.name = "fx-parallax-ramp";
        t.push_back({{1507908}, p});
        // fx-parallax-fade (1730340, Mists vault domes and stars; ps 32 read 2026-10-22): 1507908
        // without the step: opacity x ramp x mask x parfade, soft depth fade (depdist).
        p.name = "fx-parallax-fade";
        t.push_back({{1730340}, p});
    }
    // prop-interior (3036412, Mists vault orbs; ps 68 read 2026-10-22): lit (GW2's sun, light
    // buffer and VSM shadows); albedo = lerp(decal at UV1 - viewTS x pardist + normalTS.xy x
    // 0.01833, diffuse at UV0 + (perturb x 2 - 1) x difptrb, mask.G at UV1 + (perturb x 2 - 1) x
    // mskptrb) x mod x 2 x subsurface; shine = the same lerp of saturate(2a - 1); a fresnel
    // reflection lerp (shine x envcr.w x (0.75 + 0.25 (1 - N.V)^2)) toward cube x envcr.rgb;
    // rim = sun x rimcp.rgb x (1 - N.V)^rimcp.w.
    {
        ShaderProfile p = make("prop-interior", AlphaUse::Shine, false);
        p.interiorRole = "decal";
        t.push_back({{3036412}, p});
        // The same core (ps 71 read 2026-10-22) without the perturb, with three normals blended
        // (normal, decalnormal x mask.B, addnormal: not mapped), the glow x glofade x 2: 1601802;
        // 2314655 also steps the mask (stepmin, decfade) and discards cutout.G x opacity < 0.5.
        t.push_back({{1601802, 2314655}, p});
    }
    // prop-burn (3122105, Mists vault; ps 38 read 2026-10-22): lit; discard cutout.R x opacity
    // < 0.5; colour = lerp(diffuse, decal, step(mask.G x saturate(2 decal.a), stepmin, decfade)),
    // mod lerped in by modfade; emission = smoothstep01(saturate((cutout.rgb x opacity + burnoff -
    // 1) x -10)) x burncol x 2 (the burn edge) + glow x glofade x 2 x a Time pulse (not mapped).
    {
        ShaderProfile p = make("prop-burn", AlphaUse::Shine, false);
        t.push_back({{3122105}, p});
    }
    // fx-ramp-glow (1998356, Mists vault glows; ps 22 read 2026-10-22): One / InvSrcColor,
    // unlit; a = diffuse.a x ramp(|N.V|).r x diffade x soft depth fade (depdist); rgb = diffuse.rgb
    // x color.rgb x a (premultiplied in the shader).
    {
        ShaderProfile p = make("fx-ramp-glow", AlphaUse::Opacity, false);
        p.unlit = true;
        t.push_back({{1998356}, p});
    }
    // fx-perturb-mod-glow (574526, Mists vault glows; ps 11 read 2026-10-22): One / InvSrcColor,
    // unlit; p = perturb(UV1) x 2 - 1; rgb = diffuse(UV0).rgb x mod(UV2 + p x modptrb x mask.R) x 2;
    // a = diffuse(UV0 + p x difptrb).a x ramp(|N.V|).r x diffade x mask(UV3).G; rgb x a.
    {
        ShaderProfile p = make("fx-perturb-mod-glow", AlphaUse::Opacity, false);
        p.unlit = true;
        t.push_back({{574526}, p});
    }
    // fx-perturb-glow (1946706, Mists sparkles; ps 31 read 2026-10-21): colour =
    // diffuse at UV0 + (t1 "perturb" x 2 - 1) x perturb, x the colour constant, fogged;
    // alpha a constant; no lighting. The effect's state has no blend bits, but the diffuse
    // is a glow on black: assumeAdditive.
    {
        ShaderProfile p = make("fx-perturb-glow", AlphaUse::Unused, false);
        p.unlit = true;
        p.assumeAdditive = true;
        t.push_back({{1946706}, p});
    }
    // prop-projector (note section 8.3, hand-read: 77238 = 835499, 69856 = 69792 = 512093 =
    // 512112 by opcode stream): prop-lit with mod; albedo = lerp(diffuse*mod*2,
    // projector.rgb, w), w from the world normal. Only 512093/512112 clip on the diffuse alpha.
    {
        ShaderProfile p = make("prop-projector", AlphaUse::Shine, false);
        p.projectorRole = "projector";
        t.push_back({{77238, 835499, 69856, 69792}, p});
        p.diffuseAlpha = AlphaUse::HolesAndShine;
        p.clips = true;
        t.push_back({{512093, 512112}, p});
    }
    // Multiply decals (read 2026-10-22), unlit, fogged toward their neutral colour.
    // fx-multiply (15206, ps 12, DstColor / Zero): o = diffuse, fog to white.
    // fx-multiply-bump (157432 ps 16, 3718974 ps 16; DstColor / SrcColor = 2x multiply): the
    // sun in the tangent frame (shSunData), o = dot(L, normal map) - L.z + 0.5, i.e. the bump
    // shading against a flat surface (0.5 = no change), fog to 0.5; 3718974: rgb = 2 x that x
    // t1 (UV1) x opacity + 0.5.
    {
        ShaderProfile p = make("fx-multiply", AlphaUse::Unused, false);
        p.unlit = true;
        t.push_back({{15206}, p});
        // The diffuse slot is the normal map (RG): it ships raw as the "diffuse" extra, use
        // "normal-map"; 3718974's colour layer has no role token ("unnamed").
        p.name = "fx-multiply-bump";
        p.baseColorRole = "unnamed";
        p.diffuseUse = "normal-map";
        t.push_back({{157432, 3718974}, p});
    }
    // fx-cubemap (221571, ps 21, opaque; Twilight 543947): o = (cube.rgb / cube.a)^2 along
    // -V - 0.03 x t0.rgb (t0 = perturb, castlemist's diffuse), fog. The cube is a DDS
    // (materials.json maps.cubemaps).
    {
        ShaderProfile p = make("fx-cubemap", AlphaUse::Unused, false);
        p.unlit = true;
        t.push_back({{221571}, p});
    }
    // Refracting glass (ps 31 read 2026-10-22; Castlemist/GW2 Distort _Glass 1): the grabbed scene at
    // the pixel + (distort.xy + 1) x distort x the view-space reflection's xy (foreground taps
    // rejected) x modxcol x 2, dimmed by 1 - 0.3 fresnel^6 and lerped to saturate(scene x sun x 1.5)
    // where the sun shines through; a decal (UV1) lit by SH + sun x shadow covers it by
    // saturate(2 decal.a); fresnel^1.5 env-cube reflection and a pow 256 sun highlight, both x
    // min(1, 1 - saturate(2a) + saturate(2a - 1)). The distort map (UV0) is also the lighting normal.
    // glass-refract (49659): opaque. glass-refract-mask (63923): SrcA/InvSrcA, alpha = mask.G (UV1),
    // AlphaRef discard, the highlight also x the sun shadow.
    t.push_back({{49659}, make("glass-refract", AlphaUse::Unused, false)});
    t.push_back({{63923}, make("glass-refract-mask", AlphaUse::Unused, false)});
    // water-refract (57026, ps 36, e.g. hylek well 422751; _Glass 2): SrcA/InvSrcA, alpha = mask.R
    // (UV2), AlphaRef discard; normal (UV0) x mask.G and decalnormal (UV1) x mask.B each shift their
    // own scene tap by (n.xy + 1) x distort (no view reflection; the decalnormal's tap subtracted),
    // lerped by fresnel^1.5; 0.25 fresnel^1.5 env cube; speccp x 2 x sun x pow(N.H, speccp.w).
    // No SH, no shadow, no tint.
    t.push_back({{57026}, make("water-refract", AlphaUse::Unused, false)});
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
