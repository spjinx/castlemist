/// @file
/// @brief Sky sampler from docs/research/gw2-sky.md: base hemicube (§2, §5),
///        stars (§8) and texture sky cards (§9).

#include "castlemist/exportgltf/sky_bake.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace castlemist::exportgltf::sky {

using castlemist::model::Extractor;

const char* input_source_name(InputSource s) {
    switch (s) {
    case InputSource::Measured: return "measured";
    case InputSource::Derived: return "derived from static code";
    default: return "UNPROVEN guess";
    }
}

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

/// Bilinear lookup with wrap (D3D WRAP addressing), for tiling layers.
Rgba lookup_wrap(const Image& img, float u, float v) {
    const float fx = u * img.width - 0.5f, fy = v * img.height - 0.5f;
    const float x0f = std::floor(fx), y0f = std::floor(fy);
    const float tx = fx - x0f, ty = fy - y0f;
    auto wrap = [](long long i, int n) { return static_cast<int>(((i % n) + n) % n); };
    const long long x0 = static_cast<long long>(x0f), y0 = static_cast<long long>(y0f);
    const int xs[2] = {wrap(x0, img.width), wrap(x0 + 1, img.width)};
    const int ys[2] = {wrap(y0, img.height), wrap(y0 + 1, img.height)};
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

float frac(float x) { return x - std::floor(x); }
float smooth01(float f) { return f * f * (3 - 2 * f); }

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
    /// edge overlap visible; on top of this inset the face boundaries are
    /// cross-faded (kFadeTexels, warned in sky.json).
    static float inset(const Image& img) { return 1.4f / img.width; }

    /// Half-width of the cross-fade band at each face boundary, in texels of
    /// the face's own texture. Not from the game (it draws the overlap as-is):
    /// the user's choice of a seamless export.
    static constexpr float kFadeTexels = 2.0f;

    /// Face lookups, gw2-sky.md §2.
    Rgba side_upper(const Image& img, float u, float v) const {   // faces 1 (east) / 3 (west)
        v = std::clamp(v, inset(img), 0.5f);
        return lookup(img, u, v, 0, img.height / 2 - 1);
    }
    Rgba side_lower(const Image& img, float u, float v) const {   // faces 0 (north) / 2 (south)
        v = std::clamp(v, 0.5f, 1.0f - inset(img));
        return lookup(img, u, v, img.height / 2, img.height - 1);
    }

    enum Face { East, West, North, South, Top };

    /// One face's §2 formula at direction (x, y, up), also a little past the
    /// face's own edges (the lookups clamp), so a neighbour can be sampled at
    /// the same direction. Below the horizon the side faces repeat their
    /// horizon row (U = 0).
    Rgba sample(Face f, float x, float y, float up) const {
        if (f == Top) {
            const float a = inset(t), b = 1.0f - a;
            const float X = x / up, Y = y / up;
            return lookup(t, a + (b - a) * (1 + X) / 2, a + (b - a) * (1 + Y) / 2);
        }
        if (f == East || f == West) {          // upper halves
            const float ax = std::fabs(x), Y = y / ax, U = up > 0 ? up / ax : 0;
            const Image& img = f == East ? ne : sw;
            const float a = inset(img), b = 1.0f - a;
            const float u = f == East ? a + (b - a) * (1 - Y) / 2    // face 1, east
                                      : a + (b - a) * (1 + Y) / 2;   // face 3, west
            return side_upper(img, u, a + (0.5f - a) * (1 - U));
        }
        const float ay = std::fabs(y), X = x / ay, U = up > 0 ? up / ay : 0;   // lower halves, rotated 180°
        const Image& img = f == North ? ne : sw;
        const float a = inset(img), b = 1.0f - a;
        const float u = f == North ? a + (b - a) * (1 - X) / 2       // face 0, north
                                   : a + (b - a) * (1 + X) / 2;      // face 2, south
        return side_lower(img, u, 0.5f + (b - 0.5f) * U);
    }

    /// §2 inverse projection: GW2 sky direction -> premultiplied texel colour
    /// (rgb * a). Near a face boundary the owning face and its neighbour are
    /// both sampled at d and blended linearly by the distance to the edge
    /// (texels of the owner), weight 0.5 on the edge itself.
    Rgba base(const float d[3]) const {
        float x = d[0], y = d[1];
        const float up = -d[2];
        float ax = std::fabs(x), ay = std::fabs(y);
        if (ax == 0 && ay == 0 && up <= 0) {   // straight down: every side row is the
            x = 1;                             // same horizon stretch, pick any face
            ax = 1;
        }
        const float m = std::max(ax, ay);

        // Owner face, and each boundary within the band: (neighbour, distance in texels).
        Face own;
        struct Edge { Face f; float dist; };
        Edge edges[3];
        int ne_ = 0;
        auto edge = [&](Face f, float dist) {
            if (dist < kFadeTexels) edges[ne_++] = Edge{f, std::max(dist, 0.0f)};
        };
        if (up > 0 && up >= m) {               // top cap, face 4, texture T
            own = Top;
            const float k = (1 - 2 * inset(t)) * t.width / 2;   // texels per unit of X / Y
            const float X = x / up, Y = y / up;
            if (std::fabs(X) >= std::fabs(Y)) edge(X > 0 ? East : West, (1 - std::fabs(X)) * k);
            else                              edge(Y > 0 ? North : South, (1 - std::fabs(Y)) * k);
            // the second-nearest edge too, for the corners
            if (std::fabs(X) >= std::fabs(Y)) edge(Y > 0 ? North : South, (1 - std::fabs(Y)) * k);
            else                              edge(X > 0 ? East : West, (1 - std::fabs(X)) * k);
        } else if (ax >= ay) {                 // east / west
            own = x > 0 ? East : West;
            const Image& img = x > 0 ? ne : sw;
            const float a = inset(img);
            const float Y = y / ax;
            edge(Y > 0 ? North : South, (1 - std::fabs(Y)) * (1 - 2 * a) * img.width / 2);
            if (up > 0) edge(Top, (1 - up / ax) * (0.5f - a) * img.height);
        } else {                               // north / south
            own = y > 0 ? North : South;
            const Image& img = y > 0 ? ne : sw;
            const float a = inset(img);
            const float X = x / ay;
            edge(X > 0 ? East : West, (1 - std::fabs(X)) * (1 - 2 * a) * img.width / 2);
            if (up > 0) edge(Top, (1 - up / ay) * (0.5f - a) * img.height);
        }

        auto premul = [](Rgba c) { return Rgba{c.r * c.a, c.g * c.a, c.b * c.a, c.a}; };
        Rgba out = premul(sample(own, x, y, up));
        if (ne_ == 0) return out;
        // Neighbour weight 0.5 at the edge falling to 0 at kFadeTexels inside;
        // the owner keeps the product of what each edge leaves it.
        float w[3], sum = 0, keep = 1;
        for (int i = 0; i < ne_; ++i) {
            w[i] = 0.5f * (1 - edges[i].dist / kFadeTexels);
            sum += w[i];
            keep *= 1 - w[i];
        }
        Rgba acc{out.r * keep, out.g * keep, out.b * keep, out.a * keep};
        for (int i = 0; i < ne_; ++i) {
            if (w[i] <= 0) continue;
            const float k = (1 - keep) * w[i] / sum;
            const Rgba c = premul(sample(edges[i].f, x, y, up));
            acc.r += c.r * k;
            acc.g += c.g * k;
            acc.b += c.b * k;
            acc.a += c.a * k;
        }
        return acc;
    }

    /// §5 with LightIntensity and HazeDensity left out: tex.rgb * tex.a *
    /// Brightness, before the 8-bit clamp (§8.5 adds the stars to this).
    void radiance(const float g[3], float out[3]) const {
        const Rgba c = base(g);   // already rgb * a
        out[0] = c.r * brightness;
        out[1] = c.g * brightness;
        out[2] = c.b * brightness;
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

// ---- approximate sky (gw2-sky.md amendment 2026-10-17) -----------------

/// The sun-glow / SH lighting term shared by the hemicube (§5), the cards
/// (§9.4) and the clouds (§10.4): `col = T + w (G - T)`, with g taken from
/// @p v and the ambient from (-v, 1).
struct Glow {
    bool on = false;
    float sunClr[3] = {1, 1, 1}, sunDir[3] = {0, 0, 1}, sh[3][4] = {};

    void apply(const float T[3], const float v[3], float w, float out[3]) const {
        if (!on || w == 0) {
            for (int k = 0; k < 3; ++k) out[k] = T[k];
            return;
        }
        const float s = dot3(sunDir, v) * 0.25f + 0.75f;
        const float g = ((1 - v[2]) + sunDir[2] * v[2]) * s * s;
        const float lum = T[0] * 0.3f + T[1] * 0.59f + T[2] * 0.11f;
        for (int k = 0; k < 3; ++k) {
            const float amb = -v[0] * sh[k][0] - v[1] * sh[k][1] - v[2] * sh[k][2] + sh[k][3];
            const float C = amb + (sunClr[k] - amb) * g;
            const float X = C * T[k] + lum * lum * (T[k] + C - 2 * C * T[k]);
            const float M = 0.5f * X + 0.5f * T[k];
            const float G = M + (X - M) * g;
            out[k] = T[k] + w * (G - T[k]);
        }
    }
};

/// One layer drawn over the hemicube in the approximate sky.
struct Overlay {
    virtual ~Overlay() = default;
    virtual void blend(const float d[3], float dst[3]) const = 0;
};

/// Where direction @p d hits a card quad: its 0..1 quad coordinates (s left
/// to right, r top to bottom) and texture uv (§9.5); false on a miss.
bool card_hit(const Card& k, const float d[3], float& u, float& v) {
    const float x = dot3(d, k.c);
    if (x <= 0) return false;
    const float a = dot3(d, k.eL) / x, b = dot3(d, k.eD) / x;
    if (std::fabs(a) > k.tu || std::fabs(b) > k.tv) return false;
    const float s = (1 - a / k.tu) / 2, r = (1 + b / k.tv) / 2;
    u = k.uv[0] + (k.uv[1] - k.uv[0]) * s;
    v = (1 - k.uv[2]) + ((1 - k.uv[3]) - (1 - k.uv[2])) * r;
    return true;
}

/// A texture card with §9.4 in full: sun glow and haze toward FogColorFar.
struct LitCard : Overlay {
    Card geo;                    // placement, UVs, texture, brightness, density
    Glow glow;
    float lightW = 0;            // card.lightIntensity * sky LightIntensity
    float fog[3] = {0, 0, 0};
    float hazeBottom = 0, hazeFalloff = 0, hazeW = 0, minHaze = 0, cardFade = 1;

    void blend(const float d[3], float dst[3]) const override {
        float u, v;
        if (!card_hit(geo, d, u, v)) return;
        const Rgba T = lookup(*geo.tex, u, v);
        const float t3[3] = {T.r, T.g, T.b};
        float col[3];
        glow.apply(t3, d, lightW, col);   // §9.4: g and the ambient from d
        const float e = saturate(-d[2]);
        const float f = saturate((e - hazeBottom) / hazeFalloff);
        const float h = saturate((1 - smooth01(f)) * hazeW + minHaze);
        const float al = std::clamp(T.a * geo.density * cardFade, 0.0f, 1.0f);
        for (int k = 0; k < 3; ++k) {
            const float rgb = (col[k] + (fog[k] - col[k]) * h) * geo.brightness;
            dst[k] = rgb * al + dst[k] * (1 - al);
        }
    }
};

/// A material card drawn with the transcribed pixel shader of AMAT 1262294
/// (PS 12, "flow fire"): two flow maps distort a noise texture, which picks
/// a colour between colorl and colorh, masked by a second flow-distorted
/// texture; then the §9.4 haze (skypara) and density / brightness (skyparb).
struct FlowFireCard : Overlay {
    Card geo;                                          // placement (uv = textures[0].textureUV)
    std::shared_ptr<const Image> t[5];                 // t0..t4 = material textures[0..4]
    float colorl[4] = {}, colorh[4] = {}, fireduv[4] = {}, flowspd[4] = {}, flowuv[4] = {}, flowbuv[4] = {},
          firedfl[4] = {}, ramprmp[4] = {}, firesfl[4] = {}, opacity[4] = {};
    float fog[3] = {0, 0, 0};
    float skypara[4] = {}, skyparb[4] = {};
    float time = 0, alphaRef = 0;

    /// Scale and offset of one animated UV set:
    /// (|2 frac(T c.y) - 1| (c.w - c.z) + c.z, frac(T c.x)).
    void anim(const float c[4], float& scale, float& off) const {
        scale = std::fabs(frac(time * c[1]) * 2 - 1) * (c[3] - c[2]) + c[2];
        off = frac(time * c[0]);
    }

    void blend(const float d[3], float dst[3]) const override {
        float u, v;
        if (!card_hit(geo, d, u, v)) return;
        const float v0[4] = {u, v, u, v};   // TEXCOORD1 / TEXCOORD0 through identity TexTransforms
        float sc, off;
        anim(flowuv, sc, off);
        const Rgba f2 = lookup_wrap(*t[2], v0[0] * sc, v0[1] * sc + off);
        anim(flowbuv, sc, off);
        const Rgba f3 = lookup_wrap(*t[3], v0[0] * sc, v0[1] * sc + off);
        const float flow[2] = {(f2.r * 2 - 1) + (f3.r * 2 - 1), (f2.g * 2 - 1) + (f3.g * 2 - 1)};
        const float fd[2] = {flow[0] * firedfl[0], flow[1] * firedfl[0]};
        anim(fireduv, sc, off);
        const float base[2] = {v0[0] * sc, v0[1] * sc + off};
        const float ph1 = frac(flowspd[0] * time - 0.5f), ph2 = frac(time * flowspd[0]);
        const float A = lookup_wrap(*t[1], fd[0] * ph1 + base[0], fd[1] * ph1 + base[1]).g;
        const float B = lookup_wrap(*t[1], fd[0] * ph2 + base[0], fd[1] * ph2 + base[1]).g;
        const float w2 = std::fabs(ph2 * 2 - 1);
        const float n = B + w2 * (A - B);
        const float k = smooth01(saturate((n + 0.05f) * 2.03593f));
        float col[4];
        for (int c = 0; c < 3; ++c) col[c] = (colorl[c] + n * (colorh[c] - colorl[c])) * k * 3.24f;
        col[3] = k * 1.8f;
        const float m = lookup_wrap(*t[4], v0[2], v0[3]).r * (ramprmp[1] - ramprmp[0]) + ramprmp[0];
        const float dm[2] = {flow[0] * m * firesfl[0], flow[1] * m * firesfl[0]};
        const float A0 = lookup_wrap(*t[0], dm[0] * ph1 + v0[2], dm[1] * ph1 + v0[3]).g;
        const float B0 = lookup_wrap(*t[0], dm[0] * ph2 + v0[2], dm[1] * ph2 + v0[3]).g;
        const float mask = B0 + w2 * (A0 - B0);
        const float al = mask * col[3] * opacity[0];
        if (al * skyparb[0] - alphaRef < 0) return;   // discard
        const float e = saturate(-d[2]);
        const float f = saturate((e - skypara[0]) * (1.0f / skypara[1]));
        const float h = saturate((1 - smooth01(f)) * skypara[2] + skypara[3]);
        const float outA = std::clamp(al * skyparb[0], 0.0f, 1.0f);
        for (int c = 0; c < 3; ++c) {
            const float rgb = mask * col[c] * opacity[0];
            const float o = (rgb + (fog[c] - rgb) * h) * skyparb[1];
            dst[c] = o * outA + dst[c] * (1 - outA);
        }
    }
};

/// One cloud layer, §10.5: a horizontal square at z = -altitude, half-size
/// F m, centred under the camera, scroll 0, soft-particle factor 1.
struct CloudLayer : Overlay {
    std::shared_ptr<const Image> tex;
    Glow glow;
    float camZ = 0, planeZ = 0, Sw = 1, uvScale = 1, depth = 0, density = 0, haze = 0, lightW = 0,
          brightness = 1, fadeEnd = 0, fadeWidth = 0, cloudFade = 1;
    float fog[3] = {0, 0, 0};
    // VS 1645 fog: min((1 - fz) (near.w - far.w) + far.w, fz) * haze, with
    // fz = saturate((viewZ - d0) / (d1 - d0)); viewZ = s along this direction.
    bool fogDist = false;
    float d0 = 0, d1 = 0, nearW = 1, farW = 1;

    void blend(const float d[3], float dst[3]) const override {
        if (d[2] == 0) return;
        const float s = (planeZ - camZ) / d[2];
        if (!(s > 0)) return;
        const float xl = s * d[0] / Sw, yl = s * d[1] / Sw;
        if (std::fabs(xl) > 0.5f || std::fabs(yl) > 0.5f) return;
        const float rr = std::min(std::sqrt(xl * xl + yl * yl) / 0.5f, 1.0f);
        const float va = 1 - rr * rr * rr * rr;
        const float u = (xl + 0.5f) * uvScale, v = (yl + 0.5f) * uvScale;   // cam.xy = 0, scroll 0
        const float n[3] = {-d[0], -d[1], -d[2]};
        const float a0 = lookup_wrap(*tex, u, v).a;
        const Rgba T = lookup_wrap(*tex, u + (a0 - 0.5f) * depth * n[0], v + (a0 - 0.5f) * depth * n[1]);
        const float t3[3] = {T.r, T.g, T.b};
        float col[3];
        glow.apply(t3, n, lightW, col);   // §10.4: as §9.4 with d replaced by n
        float vf = saturate((std::fabs(camZ - planeZ) - fadeEnd) / (fadeEnd + fadeWidth));
        vf *= vf;
        const float al = std::clamp(va * density * cloudFade * T.a * vf, 0.0f, 1.0f);
        const float fz = fogDist && d1 > d0 ? saturate((s - d0) / (d1 - d0)) : 1.0f;
        const float fogAmt = std::min((1 - fz) * (nearW - farW) + farW, fz) * haze;
        for (int k = 0; k < 3; ++k) {
            const float rgb = (col[k] + (fog[k] - col[k]) * fogAmt) * brightness;
            dst[k] = rgb * al + dst[k] * (1 - al);
        }
    }
};

/// Every baked layer of one mode, composed in the §7 order the blend states
/// suggest: hemicube (opaque) -> stars (additive) -> cards (alpha).
struct Composite {
    Hemicube base;
    std::shared_ptr<const StarLayer> stars;
    std::vector<Card> cards;
    // Approximate sky only: the base's haze and glow, then every card and
    // cloud layer as an overlay (cards in card order, then clouds farthest first).
    bool approx = false;
    Glow baseGlow;
    float baseLightW = 0, fog[3] = {0, 0, 0}, hazeBottom = 0, hazeFalloff = 0, hazeW = 0;
    std::vector<std::shared_ptr<const Overlay>> overlays;

    Rgb radiance(const float unity[3]) const {
        float g[3], c[3];
        unity_to_gw2(unity, g);
        if (approx) return approx_radiance(g);
        base.radiance(g, c);
        if (stars) stars->add(g, c);
        for (const Card& k : cards) k.blend(g, c);
        // §5: the honest 8-bit mapping is clamp(c, 0, 1).
        return Rgb{std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 0.0f, 1.0f), std::clamp(c[2], 0.0f, 1.0f)};
    }

    /// §5 in full (sun glow, horizon haze), then stars, then the overlays.
    Rgb approx_radiance(const float g[3]) const {
        float T[3], c[3];
        base.radiance(g, T);   // tex.rgb * tex.a * Brightness
        const float n[3] = {-g[0], -g[1], -g[2]};   // PS 1617: n = -view
        baseGlow.apply(T, n, baseLightW, c);
        float f = saturate((std::max(n[2], 0.0f) - hazeBottom) / hazeFalloff);
        f = (1 - smooth01(f)) * hazeW;
        for (int k = 0; k < 3; ++k) c[k] += (fog[k] - c[k]) * f;
        if (stars) stars->add(g, c);
        for (const auto& o : overlays) o->blend(g, c);
        return Rgb{std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 0.0f, 1.0f), std::clamp(c[2], 0.0f, 1.0f)};
    }
};

Rgb black(const float*) { return Rgb{}; }

/// GW2 Token (base-23, gw2-uniform-hash.md): a material constant's name.
std::string token_name(uint32_t token) {
    static const char kAlpha[] = "abcdefghiklmnopvrstuwxy";
    uint32_t v = token - 0x30000000u;
    std::string out;
    while (v) {
        out += kAlpha[v % 23];
        v /= 23;
    }
    return out;
}

/// §9.2 / §9.3 placement of one card attribute set at distance F.
Card place_card(const Extractor::MapSkyCardAttr& at, float F) {
    Card k;
    const float az = at.azimuth, lat = at.latitude * kPi / 2;
    const float ca = std::cos(az), sa = std::sin(az), cl = std::cos(lat), sl = std::sin(lat);
    k.c[0] = cl * ca;  k.c[1] = cl * sa;  k.c[2] = -sl;
    k.eL[0] = -sa;     k.eL[1] = ca;      k.eL[2] = 0;
    k.eD[0] = sl * ca; k.eD[1] = sl * sa; k.eD[2] = cl;
    k.tu = 1000 * std::max(at.scale[0], 1e-6f) / F;
    k.tv = 1000 * std::max(at.scale[1], 1e-6f) / F;
    std::copy(std::begin(at.textureUV), std::end(at.textureUV), k.uv);
    k.brightness = at.brightness;
    k.density = at.density;
    return k;
}

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", v);
    return b;
}

/// The approximate sky's additions to one mode (gw2-sky.md amendment
/// 2026-10-17): the hemicube's haze and sun glow, every card it can draw
/// (texture cards with §9.4 in full, the flow-fire material card), then the
/// cloud layers (§10.5), farthest first.
void add_approx(Composite& out, BakeResult& res, const Extractor::MapSky& sky, size_t modeIndex,
                const TextureMap& textures, const ApproxInputs& in) {
    const bool day = modeIndex % 2 == 0;
    const Extractor::MapSkyParams& p = sky.params;
    const float F0 = sky.skyDistance;
    out.approx = true;
    res.warnings.push_back("approximate sky: layers the proven bake leaves out are drawn with the labelled "
                           "inputs below (gw2-sky.md amendment 2026-10-17)");
    for (const InputLabel& l : in.labels)
        res.warnings.push_back("approx input: " + l.input + " = " + l.value + " [" + input_source_name(l.source) +
                               "] " + l.note);

    Glow glow;
    glow.on = in.haveSun;
    for (int k = 0; k < 3; ++k) {
        glow.sunClr[k] = in.sunColor[k];
        glow.sunDir[k] = in.sunDir[k];
        for (int j = 0; j < 4; ++j) glow.sh[k][j] = in.sh[k][j];
    }
    const float fog[3] = {in.fogFar[0], in.fogFar[1], in.fogFar[2]};
    const float HB = day ? p.dayHazeBottom : p.nightHazeBottom;
    const float HF = day ? p.dayHazeFalloff : p.nightHazeFalloff;
    const float HD = day ? p.dayHazeDensity : p.nightHazeDensity;
    const float LI = day ? p.dayLightIntensity : p.nightLightIntensity;
    const float hazeW = in.haveFog ? HD : 0.0f;   // without FogColorFar no haze at all

    // §5: the base pass's own haze and glow.
    out.baseGlow = glow;
    out.baseLightW = LI;
    for (int k = 0; k < 3; ++k) out.fog[k] = fog[k];
    out.hazeBottom = HB;
    out.hazeFalloff = HF;
    out.hazeW = hazeW;
    // "haze" and "sun glow" belong to the base pass: listed right after "base".
    auto afterBase = std::find(res.layers.begin(), res.layers.end(), std::string("base"));
    size_t pos = afterBase == res.layers.end() ? res.layers.size() : static_cast<size_t>(afterBase - res.layers.begin()) + 1;
    if (HD != 0) {
        if (in.haveFog) res.layers.insert(res.layers.begin() + static_cast<long>(pos++), "haze");
        else res.warnings.push_back("haze: no FogColorFar for this mode; not baked");
    }
    if (LI != 0) {
        if (in.haveSun) res.layers.insert(res.layers.begin() + static_cast<long>(pos++), "sun glow");
        else res.warnings.push_back("sun glow: no sun colour / SH inputs; not baked");
    }

    // §9: every card in card order, texture and material cards alike.
    size_t drawnCards = 0;
    for (size_t i = 0; i < sky.cards.size(); ++i) {
        const Extractor::MapSkyCard& card = sky.cards[i];
        const Extractor::MapSkyCardAttr& at = day ? card.day : card.night;
        const std::string ci = "sky cards: card " + std::to_string(i);
        if (at.density == 0) continue;   // alpha 0 everywhere
        if (card.flags & 0x8) {
            res.warnings.push_back(ci + " aims at its location (flag 8), which needs the camera; not baked");
            continue;
        }
        if (card.flags & 0x80) {
            res.warnings.push_back(ci + " depends on the UNPROVEN `level` (flag 0x80); not baked");
            continue;
        }
        Card geo = place_card(at, F0);
        if (card.materialFile) {
            if (card.materialFile != kFlowFireMaterial) {
                res.warnings.push_back(ci + " (material fileId " + std::to_string(card.materialFile) +
                                       ") has no transcribed shader; not baked");
                continue;
            }
            auto ff = std::make_shared<FlowFireCard>();
            bool texOk = card.materialTextures.size() >= 5;
            for (int k = 0; texOk && k < 5; ++k) {
                auto it = textures.find(card.materialTextures[k].fileId);
                if (it == textures.end() || !image_ok(it->second)) texOk = false;
                else ff->t[k] = std::make_shared<const Image>(it->second);
            }
            if (!texOk) {
                res.warnings.push_back(ci + " (flow-fire material): a material texture is missing or not decoded; "
                                            "not baked");
                continue;
            }
            std::copy(std::begin(card.materialTextures[0].textureUV), std::end(card.materialTextures[0].textureUV),
                      geo.uv);
            ff->geo = geo;
            for (const auto& c : card.materialConstants) {
                const std::string nm = token_name(c.token);
                float* dst = nm == "colorl" ? ff->colorl : nm == "colorh" ? ff->colorh : nm == "fireduv" ? ff->fireduv
                           : nm == "flowspd" ? ff->flowspd : nm == "flowuv" ? ff->flowuv : nm == "flowbuv" ? ff->flowbuv
                           : nm == "firedfl" ? ff->firedfl : nm == "ramprmp" ? ff->ramprmp : nm == "firesfl" ? ff->firesfl
                           : nm == "opacity" ? ff->opacity : nullptr;
                if (dst) std::copy(std::begin(c.value), std::end(c.value), dst);
            }
            for (int k = 0; k < 3; ++k) ff->fog[k] = fog[k];
            // §9.4: the card draw sets skypara / skyparb itself (the tokens are
            // in the reserved table 0x1427f1550, so the material's own are not used).
            ff->skypara[0] = HB;
            ff->skypara[1] = HF;
            ff->skypara[2] = in.haveFog ? at.hazeDensity * HD : 0.0f;
            ff->skypara[3] = in.haveFog ? at.minHaze : 0.0f;
            ff->skyparb[0] = at.density * in.cardFade;
            ff->skyparb[1] = at.brightness;
            ff->time = in.time;
            ff->alphaRef = in.alphaRef;
            out.overlays.push_back(ff);
            ++drawnCards;
            res.warnings.push_back(ci + " (material " + std::to_string(card.materialFile) +
                                   "): drawn with the transcribed PS 12 of that AMAT; quad UVs taken as "
                                   "textures[0].textureUV with identity TexTransforms and t_i = textures[i] "
                                   "(UNPROVEN: the material quad 0x140ca7740 and the sky path's effect choice "
                                   "0x140aab550 were not traced)");
            continue;
        }
        if (!at.texture) continue;
        auto it = textures.find(at.texture);
        if (it == textures.end() || !image_ok(it->second)) {
            res.warnings.push_back(ci + " texture " + std::to_string(at.texture) + " missing or not decoded; not baked");
            continue;
        }
        auto lc = std::make_shared<LitCard>();
        geo.tex = std::make_shared<const Image>(it->second);
        lc->geo = geo;
        lc->glow = glow;
        lc->lightW = at.lightIntensity * LI;
        for (int k = 0; k < 3; ++k) lc->fog[k] = fog[k];
        lc->hazeBottom = HB;
        lc->hazeFalloff = HF;
        lc->hazeW = in.haveFog ? at.hazeDensity * HD : 0.0f;
        lc->minHaze = in.haveFog ? at.minHaze : 0.0f;
        lc->cardFade = in.cardFade;
        out.overlays.push_back(lc);
        ++drawnCards;
        if (std::find(res.cardTextures.begin(), res.cardTextures.end(), at.texture) == res.cardTextures.end())
            res.cardTextures.push_back(at.texture);
        if (at.minHaze > 0 && !in.haveFog)
            res.warnings.push_back(ci + " minHaze > 0 but no FogColorFar: drawn un-hazed");
    }
    if (drawnCards) res.layers.push_back("cards");

    // §10: cloud layers, attribute set 0 (day) / 1 (night); the third set
    // (camera.z > level) is not used.
    struct Pending { float dist; std::shared_ptr<CloudLayer> layer; };
    std::vector<Pending> clouds;
    const size_t slot = day ? 0 : 1;
    for (size_t i = 0; i < sky.clouds.size(); ++i) {
        const Extractor::MapSkyCloudLayer& L = sky.clouds[i];
        const std::string li = "clouds: layer " + std::to_string(i);
        if (L.attributes.size() <= slot) {
            res.warnings.push_back(li + " has no attribute set " + std::to_string(slot) + "; not baked");
            continue;
        }
        auto it = textures.find(L.texture);
        if (!L.texture || it == textures.end() || !image_ok(it->second)) {
            res.warnings.push_back(li + " texture " + std::to_string(L.texture) + " missing or not decoded; not baked");
            continue;
        }
        const Extractor::MapSkyCloudAttr& A = L.attributes[slot];
        auto cl = std::make_shared<CloudLayer>();
        cl->tex = std::make_shared<const Image>(it->second);
        cl->glow = glow;
        const float m = 0.5925f + 0.4075f * L.extent;   // §10.2
        cl->Sw = 2 * F0 * m;
        cl->uvScale = m * L.scale;
        cl->camZ = in.cameraZ;
        cl->planeZ = -L.altitude;
        cl->depth = L.depth;
        cl->density = A.density;
        // §10.4 fog, distance term from the haze's distRange (GrSetFog's FogParam0).
        cl->haze = in.haveFog ? A.haze : 0.0f;
        cl->fogDist = in.haveFogDist;
        cl->d0 = in.fogDist[0];
        cl->d1 = in.fogDist[1];
        cl->nearW = in.fogNearW;
        cl->farW = in.fogFar[3];
        cl->lightW = A.lightIntensity * LI;
        cl->brightness = A.brightness;
        cl->fadeEnd = L.attributes.back().fadeEnd;   // §10.1: the last set's fade values win
        cl->fadeWidth = L.attributes.back().fadeWidth;
        cl->cloudFade = in.cloudFade;
        for (int k = 0; k < 3; ++k) cl->fog[k] = fog[k];
        clouds.push_back({std::fabs(cl->planeZ - cl->camZ), cl});
    }
    std::stable_sort(clouds.begin(), clouds.end(), [](const Pending& a, const Pending& b) { return a.dist > b.dist; });
    for (const Pending& c : clouds) out.overlays.push_back(c.layer);
    if (!clouds.empty()) {
        res.layers.push_back("clouds");
        res.warnings.push_back("clouds: drawn farthest first (order UNPROVEN), scroll 0 (the load state), camera "
                               "x/y 0, soft-particle factor 1 (no scene depth), attribute set " +
                               std::to_string(slot) + " (the camera.z > level set not used)");
    }
}

} // namespace

BakeResult make_sky_sampler(const Extractor::MapSky& sky, size_t modeIndex, const TextureMap& textures,
                            const Extractor::MapStars* stars, const ApproxInputs* approx) {
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
    if (!approx) {
        if (!sky.clouds.empty()) res.warnings.push_back("clouds: UNPROVEN (gw2-sky.md §4); not baked");
        if ((day ? p.dayHazeDensity : p.nightHazeDensity) != 0)
            res.warnings.push_back("haze: inputs UNPROVEN (FogColorFar, gw2-sky.md §4/§5); not baked");
        if ((day ? p.dayLightIntensity : p.nightLightIntensity) != 0)
            res.warnings.push_back("sun glow: inputs UNPROVEN (sun colour, SH, gw2-sky.md §4/§5); not baked");
    }

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
    res.warnings.push_back("seams cross-faded over 2 texels; the game draws the authored overlap as-is "
                           "(gw2-sky.md §2)");

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

    // Approximate sky: base haze / glow, every card it can draw, the clouds.
    if (approx) add_approx(*sky_out, res, sky, modeIndex, textures, *approx);

    // §9: texture sky cards, in card order, with this mode's attribute set.
    bool flag2 = false;
    for (size_t i = 0; !approx && i < sky.cards.size(); ++i) {
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
    if (!approx && !sky_out->cards.empty()) {
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
