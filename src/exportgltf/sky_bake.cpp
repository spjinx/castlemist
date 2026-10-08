/// @file
/// @brief Sky sampler from docs/research/gw2-sky.md: base hemicube (§2, §5),
///        stars (§8) and texture sky cards (§9).

#include "castlemist/exportgltf/sky_bake.h"

#include <algorithm>
#include <cmath>
#include <memory>

namespace castlemist::exportgltf::sky {

using castlemist::model::Extractor;

// gw2-sky.md §1: GW2 sky space is left-handed, X east, Y north, Z down.
void gw2_to_unity(const float gw2[3], float unity[3]) {
    const float x = gw2[0], y = gw2[1], z = gw2[2];
    unity[0] = x;
    unity[1] = -z;
    unity[2] = y;
}

void unity_to_gw2(const float unity[3], float gw2[3]) {
    const float X = unity[0], Y = unity[1], Z = unity[2];
    gw2[0] = X;
    gw2[1] = Z;
    gw2[2] = -Y;
}

namespace {

constexpr float kPi = 3.14159265358979323846f;

struct Rgba { float r = 0, g = 0, b = 0, a = 0; };

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/// HLSL saturate: clamp to [0, 1], NaN to 0 (a zero HazeFalloff divides by 0).
float saturate(float x) { return x > 0 ? (x < 1 ? x : 1) : 0; }

/// Bilinear lookup with clamp (gw2-sky.md §2 "L(t, u, v)"). Rows are clamped
/// to [row0, row1] so the two faces stored in one side texture never bleed
/// into each other at v = 0.5 (§2: "clamp v to that half").
Rgba lookup(const Image& img, float u, float v, int row0, int row1) {
    const float fx = u * img.width - 0.5f, fy = v * img.height - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    const int x0 = static_cast<int>(x0f), y0 = static_cast<int>(y0f);
    const int xs[2] = {std::clamp(x0, 0, img.width - 1), std::clamp(x0 + 1, 0, img.width - 1)};
    const int ys[2] = {std::clamp(y0, row0, row1), std::clamp(y0 + 1, row0, row1)};
    const float wx[2] = {1 - tx, tx}, wy[2] = {1 - ty, ty};
    Rgba out;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const uint8_t* p = &img.rgba[(static_cast<size_t>(ys[j]) * img.width + xs[i]) * 4];
            const float w = wx[i] * wy[j] / 255.0f;
            out.r += p[0] * w;
            out.g += p[1] * w;
            out.b += p[2] * w;
            out.a += p[3] * w;
        }
    return out;
}

Rgba lookup(const Image& img, float u, float v) { return lookup(img, u, v, 0, img.height - 1); }

bool image_ok(const Image& img) {
    return img.width > 0 && img.height > 0 &&
           img.rgba.size() >= static_cast<size_t>(img.width) * img.height * 4;
}

/// The three base textures of one mode and its Brightness.
struct Hemicube {
    Image ne, sw, t;
    float brightness = 0;

    /// Inset e = 1.4/W: the seam measurement's best fit (§2). The game's own
    /// inset is 25/F0 (§2, §7.1), which leaves the textures' authored ~1.4-texel
    /// edge overlap visible as seams; this hides them on purpose (warned).
    static float inset(const Image& img) { return 1.4f / img.width; }

    /// Face lookups, gw2-sky.md §2.
    Rgba side_upper(const Image& img, float u, float v) const {   // faces 1 (east) / 3 (west)
        v = std::clamp(v, inset(img), 0.5f);
        return lookup(img, u, v, 0, img.height / 2 - 1);
    }
    Rgba side_lower(const Image& img, float u, float v) const {   // faces 0 (north) / 2 (south)
        v = std::clamp(v, 0.5f, 1.0f - inset(img));
        return lookup(img, u, v, img.height / 2, img.height - 1);
    }

    /// §2 inverse projection: GW2 sky direction -> texel colour.
    Rgba base(const float d[3]) const {
        float x = d[0], y = d[1];
        const float up = -d[2];
        float ax = std::fabs(x), ay = std::fabs(y);
        if (ax == 0 && ay == 0 && up <= 0) {   // straight down: every side row is the
            x = 1;                             // same horizon stretch, pick any face
            ax = 1;
        }
        const float m = std::max(ax, ay);
        if (up > 0 && up >= m) {               // top cap, face 4, texture T
            const float a = inset(t), b = 1.0f - a;
            const float X = x / up, Y = y / up;
            return lookup(t, a + (b - a) * (1 + X) / 2, a + (b - a) * (1 + Y) / 2);
        }
        // Side face; below the horizon the skirt repeats the horizon row.
        const float U = up > 0 ? up / m : 0;   // 0 at horizon .. 1 at cube edge
        if (ax >= ay) {                        // east / west, upper halves
            const float Y = y / ax;
            const Image& img = x > 0 ? ne : sw;
            const float a = inset(img), b = 1.0f - a;
            const float u = x > 0 ? a + (b - a) * (1 - Y) / 2    // face 1, east
                                  : a + (b - a) * (1 + Y) / 2;   // face 3, west
            return side_upper(img, u, a + (0.5f - a) * (1 - U));
        }
        const float X = x / ay;                // north / south, lower halves (rotated 180°)
        const Image& img = y > 0 ? ne : sw;
        const float a = inset(img), b = 1.0f - a;
        const float u = y > 0 ? a + (b - a) * (1 - X) / 2        // face 0, north
                              : a + (b - a) * (1 + X) / 2;       // face 2, south
        return side_lower(img, u, 0.5f + (b - 0.5f) * U);
    }

    /// §5 with LightIntensity and HazeDensity left out: tex.rgb * tex.a *
    /// Brightness, before the 8-bit clamp (§8.5 adds the stars to this).
    void radiance(const float g[3], float out[3]) const {
        const Rgba c = base(g);
        const float k = c.a * brightness;
        out[0] = c.r * k;
        out[1] = c.g * k;
        out[2] = c.b * k;
    }
};

/// Stars, gw2-sky.md §8: one sprite per STAR record on a sphere of radius
/// R = 0.5 F0, drawn additively (ONE, ONE).
struct StarLayer {
    struct Sprite {
        float c[3], eL[3], eD[3];   // §8.3 centre and local +Y (u0 side) / +Z (v1 side)
        float au, av;               // tangent half-extents hu/R, hv/R
        float u0, u1, v0, v1;
    };
    std::vector<Sprite> sprites;
    Image atlas;
    float density = 0, hazeBottom = 0, hazeFalloff = 0, hazeDensity = 0;

    // Lookup acceleration only (no sky maths): sprites binned by the
    // azimuth/elevation cells their bounding caps touch.
    static constexpr int kAzBins = 720, kElBins = 360;
    std::vector<std::vector<uint32_t>> bins;

    static int el_bin(float el) {
        return std::clamp(static_cast<int>(std::floor((el + kPi / 2) / kPi * kElBins)), 0, kElBins - 1);
    }
    static int az_bin(float az) {
        int i = static_cast<int>(std::floor((az + kPi) / (2 * kPi) * kAzBins));
        return ((i % kAzBins) + kAzBins) % kAzBins;
    }
    static void angles(const float d[3], float& az, float& el) {
        el = std::asin(std::clamp(-d[2], -1.0f, 1.0f));
        az = std::atan2(d[1], d[0]);
    }

    void build() {
        bins.assign(static_cast<size_t>(kAzBins) * kElBins, {});
        const float cell = kPi / kElBins;
        for (uint32_t i = 0; i < sprites.size(); ++i) {
            const Sprite& s = sprites[i];
            const float r = std::atan(std::sqrt(s.au * s.au + s.av * s.av)) + cell;
            float az, el;
            angles(s.c, az, el);
            const int e0 = el_bin(el - r), e1 = el_bin(el + r);
            int a0 = 0, a1 = kAzBins - 1;
            if (std::fabs(el) + r < kPi / 2) {
                const float da = std::asin(std::min(1.0f, std::sin(r) / std::cos(std::fabs(el)))) + cell;
                a0 = static_cast<int>(std::floor((az - da + kPi) / (2 * kPi) * kAzBins));
                a1 = static_cast<int>(std::floor((az + da + kPi) / (2 * kPi) * kAzBins));
                if (a1 - a0 >= kAzBins) { a0 = 0; a1 = kAzBins - 1; }
            }
            for (int e = e0; e <= e1; ++e)
                for (int a = a0; a <= a1; ++a)
                    bins[static_cast<size_t>(e) * kAzBins + (((a % kAzBins) + kAzBins) % kAzBins)].push_back(i);
        }
    }

    /// §8.5 with twinkle tw = 0: add = sum 2 T.rgb^2, times the horizon-haze
    /// attenuation and StarDensity.
    void add(const float d[3], float out[3]) const {
        float az, el;
        angles(d, az, el);
        const auto& bin = bins[static_cast<size_t>(el_bin(el)) * kAzBins + az_bin(az)];
        float sum[3] = {0, 0, 0};
        for (uint32_t i : bin) {
            const Sprite& s = sprites[i];
            const float x = dot3(d, s.c);
            if (x <= 0) continue;
            const float a = dot3(d, s.eL) / x, b = dot3(d, s.eD) / x;
            if (std::fabs(a) > s.au || std::fabs(b) > s.av) continue;
            const float u = s.u0 + (s.u1 - s.u0) * (1 - a / s.au) / 2;
            const float v = s.v0 + (s.v1 - s.v0) * (1 + b / s.av) / 2;
            const Rgba T = lookup(atlas, u, v);
            // B = 2 T.rgb^2; rgb' = B + (2/3)(B.r+B.g+B.b) T.a tw, tw = 0.
            sum[0] += 2 * T.r * T.r;
            sum[1] += 2 * T.g * T.g;
            sum[2] += 2 * T.b * T.b;
        }
        if (sum[0] == 0 && sum[1] == 0 && sum[2] == 0) return;
        const float f = saturate((std::fabs(d[2]) - hazeBottom) / hazeFalloff);
        const float k = (1 - (1 - f * f * (3 - 2 * f)) * hazeDensity) * density;
        for (int i = 0; i < 3; ++i) out[i] += sum[i] * k;
    }
};

/// Texture sky cards, gw2-sky.md §9: a quad at distance F with half-size 1000,
/// alpha-blended (SRC_ALPHA, INV_SRC_ALPHA), in the reduced form of §9.5.
struct Card {
    float c[3], eL[3], eD[3];   // §9.2 centre, local +Y (uLeft side), local +Z (bottom side)
    float tu = 0, tv = 0;       // §9.5 tangent half-extents 1000 scale / F
    float uv[4] = {0, 1, 1, 0}; // textureUV (uLeft, uRight, 1 - vTop, 1 - vBottom)
    float brightness = 0, density = 0;
    std::shared_ptr<const Image> tex;

    void blend(const float d[3], float dst[3]) const {
        const float x = dot3(d, c);
        if (x <= 0) return;
        const float a = dot3(d, eL) / x, b = dot3(d, eD) / x;
        if (std::fabs(a) > tu || std::fabs(b) > tv) return;
        const float s = (1 - a / tu) / 2, r = (1 + b / tv) / 2;   // 0..1 left->right, top->bottom
        const float u = uv[0] + (uv[1] - uv[0]) * s;
        const float v = (1 - uv[2]) + ((1 - uv[3]) - (1 - uv[2])) * r;
        const Rgba T = lookup(*tex, u, v);
        // Reduced §9.4: col = T (lightIntensity term 0), h = 0 (haze terms 0),
        // cardFade = 1. The blend factor is clamped to [0, 1] as a UNORM
        // target clamps it (render target format not traced, §5).
        const float al = std::clamp(T.a * density, 0.0f, 1.0f);
        dst[0] = T.r * brightness * al + dst[0] * (1 - al);
        dst[1] = T.g * brightness * al + dst[1] * (1 - al);
        dst[2] = T.b * brightness * al + dst[2] * (1 - al);
    }
};

/// Every baked layer of one mode, composed in the §7 order the blend states
/// suggest: hemicube (opaque) -> stars (additive) -> cards (alpha).
struct Composite {
    Hemicube base;
    std::shared_ptr<const StarLayer> stars;
    std::vector<Card> cards;

    Rgb radiance(const float unity[3]) const {
        float g[3], c[3];
        unity_to_gw2(unity, g);
        base.radiance(g, c);
        if (stars) stars->add(g, c);
        for (const Card& k : cards) k.blend(g, c);
        // §5: the honest 8-bit mapping is clamp(c, 0, 1).
        return Rgb{std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 0.0f, 1.0f), std::clamp(c[2], 0.0f, 1.0f)};
    }
};

Rgb black(const float*) { return Rgb{}; }

} // namespace

BakeResult make_sky_sampler(const Extractor::MapSky& sky, size_t modeIndex, const TextureMap& textures,
                            const Extractor::MapStars* stars) {
    BakeResult res;
    res.radiance = black;
    const std::string mode = "mode " + std::to_string(modeIndex);

    if (modeIndex >= sky.modes.size()) {
        res.warnings.push_back(mode + ": not in the map's sky; nothing baked");
        return res;
    }
    // gw2-sky.md §3: modes 0/2 use the day* parameters (t = 1), 1/3 the
    // night* ones (t = 0), for the hemicube, stars and cards alike. Nothing is
    // known about a mode past 3.
    if (modeIndex > 3) {
        res.warnings.push_back(mode + ": parameter set UNPROVEN (gw2-sky.md §3); nothing baked");
        return res;
    }
    const bool day = modeIndex % 2 == 0;
    const Extractor::MapSkyParams& p = sky.params;
    const Extractor::MapSkyMode& m = sky.modes[modeIndex];
    const float F0 = sky.skyDistance;   // §7.1

    // §10.5: clouds need a camera height, cloudFade and the engine fog.
    if (!sky.clouds.empty()) res.warnings.push_back("clouds: UNPROVEN (gw2-sky.md §4); not baked");
    if ((day ? p.dayHazeDensity : p.nightHazeDensity) != 0)
        res.warnings.push_back("haze: inputs UNPROVEN (FogColorFar, gw2-sky.md §4/§5); not baked");
    if ((day ? p.dayLightIntensity : p.nightLightIntensity) != 0)
        res.warnings.push_back("sun glow: inputs UNPROVEN (sun colour, SH, gw2-sky.md §4/§5); not baked");

    // §2: the base layer is the NE / SW / T hemicube.
    if (!m.hasPanorama()) {
        res.warnings.push_back(mode + ": no hemicube textures (NE/SW/T); base not baked");
        return res;
    }
    auto find = [&](uint32_t id, const char* name, Image& out) {
        auto it = textures.find(id);
        if (it == textures.end() || it->second.width <= 0 || it->second.height < 2 ||
            !image_ok(it->second)) {
            res.warnings.push_back("base: " + std::string(name) + " texture " + std::to_string(id) +
                                   " missing or not decoded; " + mode + " not baked");
            return false;
        }
        out = it->second;
        return true;
    };
    auto sky_out = std::make_shared<Composite>();
    Hemicube& cube = sky_out->base;
    bool have = find(m.ne, "NE", cube.ne);
    have = find(m.sw, "SW", cube.sw) && have;
    have = find(m.top, "T", cube.t) && have;
    if (!have) return res;

    // §5: Brightness = dayBrightness (modes 0, 2) / nightBrightness (modes 1, 3).
    cube.brightness = day ? p.dayBrightness : p.nightBrightness;
    res.layers.push_back("base");
    res.warnings.push_back("seams: 1.4-texel inset hides the textures' authored overlap; "
                           "the game uses 25/F0 (gw2-sky.md §2)");

    // §8: stars. The draw hides the mesh while StarDensity is 0 (§8.1).
    const float starDensity = day ? p.dayStarDensity : p.nightStarDensity;
    if (sky.starFile && starDensity != 0) {
        const std::string sf = std::to_string(sky.starFile);
        auto atlas = stars ? textures.find(stars->atlas) : textures.end();
        if (!stars || !stars->present) {
            res.warnings.push_back("stars: star file " + sf + " not read; not baked");
        } else if (stars->stars.empty()) {
            res.warnings.push_back("stars: star file " + sf + " holds no stars; not baked");
        } else if (atlas == textures.end() || !image_ok(atlas->second)) {
            res.warnings.push_back("stars: atlas " + std::to_string(stars->atlas) + " of star file " + sf +
                                   " missing or not decoded; not baked");
        } else {
            auto layer = std::make_shared<StarLayer>();
            layer->atlas = atlas->second;
            layer->density = starDensity;
            layer->hazeBottom = day ? p.dayHazeBottom : p.nightHazeBottom;
            layer->hazeFalloff = day ? p.dayHazeFalloff : p.nightHazeFalloff;
            layer->hazeDensity = day ? p.dayHazeDensity : p.nightHazeDensity;
            const float R = 0.5f * F0;   // §7.1 star radius
            const float S = stars->scale;
            for (const Extractor::MapStar& s : stars->stars) {
                // §8.3: hu = 2500 S (u1 - u0), hv = 2500 S (v1 - v0); M2(e0) M1(e1).
                StarLayer::Sprite sp;
                const float ce0 = std::cos(s.e0), se0 = std::sin(s.e0);
                const float ce1 = std::cos(s.e1), se1 = std::sin(s.e1);
                sp.c[0] = ce1 * ce0; sp.c[1] = -ce1 * se0; sp.c[2] = -se1;
                sp.eL[0] = se0;      sp.eL[1] = ce0;       sp.eL[2] = 0;
                sp.eD[0] = se1 * ce0; sp.eD[1] = -se1 * se0; sp.eD[2] = ce1;
                sp.au = 2500 * S * (s.u1 - s.u0) / R;
                sp.av = 2500 * S * (s.v1 - s.v0) / R;
                sp.u0 = s.u0; sp.u1 = s.u1; sp.v0 = s.v0; sp.v1 = s.v1;
                if (!(sp.au > 0) || !(sp.av > 0)) continue;
                layer->sprites.push_back(sp);
            }
            layer->build();
            sky_out->stars = layer;
            res.layers.push_back("stars");
            res.starFile = sky.starFile;
            res.starAtlas = stars->atlas;
            res.warnings.push_back("stars: twinkle tw UNPROVEN (vertex RNG, Time); baked with tw = 0 "
                                   "(gw2-sky.md §8.4)");
        }
    }

    // §9: texture sky cards, in card order, with this mode's attribute set.
    bool flag2 = false;
    for (size_t i = 0; i < sky.cards.size(); ++i) {
        const Extractor::MapSkyCard& card = sky.cards[i];
        const Extractor::MapSkyCardAttr& at = day ? card.day : card.night;
        const std::string ci = "sky cards: card " + std::to_string(i);
        if (card.materialFile) {   // §9.6
            res.warnings.push_back(ci + " (material fileId " + std::to_string(card.materialFile) +
                                   ") is a material card, UNPROVEN for baking (gw2-sky.md §9.6); not baked");
            continue;
        }
        if (!at.texture || at.density == 0) continue;   // nothing drawn (§9.5)
        // §9.4: h = saturate(... + minHaze) lifts the card toward FogColorFar,
        // which is UNPROVEN; the reduced form would draw it un-hazed.
        if (at.minHaze > 0) {
            res.warnings.push_back(ci + " minHaze > 0: haze-dominated, FogColorFar UNPROVEN (gw2-sky.md §9.4); "
                                   "not baked");
            continue;
        }
        if (card.flags & 0x8) {   // §9.2: aims at `location` from the camera
            res.warnings.push_back(ci + " aims at its location (flag 8), which needs the camera; "
                                   "not baked (gw2-sky.md §9.2)");
            continue;
        }
        if (card.flags & 0x80) {   // §9.4: hidden while camera.z < level
            res.warnings.push_back(ci + " depends on the UNPROVEN `level` (flag 0x80); not baked "
                                   "(gw2-sky.md §9.4/§9.5)");
            continue;
        }
        auto it = textures.find(at.texture);
        if (it == textures.end() || !image_ok(it->second)) {
            res.warnings.push_back(ci + " texture " + std::to_string(at.texture) +
                                   " missing or not decoded; not baked");
            continue;
        }
        if (card.flags & 0x2) flag2 = true;
        // §9.2: az radians (0 = east, toward north), lat = latitude * pi/2; spin = 0.
        Card k;
        const float az = at.azimuth, lat = at.latitude * kPi / 2;
        const float ca = std::cos(az), sa = std::sin(az), cl = std::cos(lat), sl = std::sin(lat);
        k.c[0] = cl * ca;  k.c[1] = cl * sa;  k.c[2] = -sl;
        k.eL[0] = -sa;     k.eL[1] = ca;      k.eL[2] = 0;
        k.eD[0] = sl * ca; k.eD[1] = sl * sa; k.eD[2] = cl;
        // §9.3: half-size 1000 at distance F = F0, scale clamped to >= 1e-6.
        k.tu = 1000 * std::max(at.scale[0], 1e-6f) / F0;
        k.tv = 1000 * std::max(at.scale[1], 1e-6f) / F0;
        std::copy(std::begin(at.textureUV), std::end(at.textureUV), k.uv);
        k.brightness = at.brightness;
        k.density = at.density;
        k.tex = std::make_shared<const Image>(it->second);
        sky_out->cards.push_back(std::move(k));
        if (std::find(res.cardTextures.begin(), res.cardTextures.end(), at.texture) == res.cardTextures.end())
            res.cardTextures.push_back(at.texture);
    }
    if (!sky_out->cards.empty()) {
        res.layers.push_back("cards");
        res.warnings.push_back("sky cards: haze, sun-light and cardFade terms UNPROVEN (FogColorFar, SH, sun "
                               "colour); reduced form rgb = T.rgb*brightness, a = T.a*density "
                               "(gw2-sky.md §9.4/§9.5)");
        if (flag2)
            res.warnings.push_back("sky cards: flag-2 cards hide while program C is on, which is UNPROVEN; "
                                   "baked as shown (gw2-sky.md §9.4)");
    }
    if (res.layers.size() > 1) {
        std::string order = "hemicube";
        for (size_t i = 1; i < res.layers.size(); ++i) order += "->" + res.layers[i];
        res.warnings.push_back("layer order: " + order + " inferred from blend states, UNPROVEN (gw2-sky.md §7)");
    }

    res.radiance = [sky_out](const float dir[3]) { return sky_out->radiance(dir); };
    res.ok = true;
    return res;
}

} // namespace castlemist::exportgltf::sky
