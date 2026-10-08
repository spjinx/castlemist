/// @file
/// @brief Sky sampler from docs/research/gw2-sky.md (base hemicube only).

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

struct Rgba { float r = 0, g = 0, b = 0, a = 0; };

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

/// The three base textures of one mode and its Brightness.
struct Hemicube {
    Image ne, sw, t;
    float brightness = 0;

    /// Face lookups, gw2-sky.md §2. Inset e = 1/W (measured, not derived).
    Rgba side_upper(const Image& img, float u, float v) const {   // faces 1 (east) / 3 (west)
        const float a = 1.0f / img.width;
        v = std::clamp(v, a, 0.5f);
        return lookup(img, u, v, 0, img.height / 2 - 1);
    }
    Rgba side_lower(const Image& img, float u, float v) const {   // faces 0 (north) / 2 (south)
        const float b = 1.0f - 1.0f / img.width;
        v = std::clamp(v, 0.5f, b);
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
            const float a = 1.0f / t.width, b = 1.0f - a;
            const float X = x / up, Y = y / up;
            return lookup(t, a + (b - a) * (1 + X) / 2, a + (b - a) * (1 + Y) / 2, 0, t.height - 1);
        }
        // Side face; below the horizon the skirt repeats the horizon row.
        const float U = up > 0 ? up / m : 0;   // 0 at horizon .. 1 at cube edge
        if (ax >= ay) {                        // east / west, upper halves
            const float Y = y / ax;
            const Image& img = x > 0 ? ne : sw;
            const float a = 1.0f / img.width, b = 1.0f - a;
            const float u = x > 0 ? a + (b - a) * (1 - Y) / 2    // face 1, east
                                  : a + (b - a) * (1 + Y) / 2;   // face 3, west
            return side_upper(img, u, a + (0.5f - a) * (1 - U));
        }
        const float X = x / ay;                // north / south, lower halves (rotated 180°)
        const Image& img = y > 0 ? ne : sw;
        const float a = 1.0f / img.width, b = 1.0f - a;
        const float u = y > 0 ? a + (b - a) * (1 - X) / 2        // face 0, north
                              : a + (b - a) * (1 + X) / 2;       // face 2, south
        return side_lower(img, u, 0.5f + (b - 0.5f) * U);
    }

    /// §5 with LightIntensity and HazeDensity left out:
    /// radiance = clamp(tex.rgb * tex.a * Brightness, 0, 1).
    Rgb radiance(const float unity[3]) const {
        float g[3];
        unity_to_gw2(unity, g);
        const Rgba c = base(g);
        const float k = c.a * brightness;
        return Rgb{std::clamp(c.r * k, 0.0f, 1.0f), std::clamp(c.g * k, 0.0f, 1.0f),
                   std::clamp(c.b * k, 0.0f, 1.0f)};
    }
};

Rgb black(const float*) { return Rgb{}; }

} // namespace

BakeResult make_sky_sampler(const Extractor::MapSky& sky, size_t modeIndex, const TextureMap& textures) {
    BakeResult res;
    res.radiance = black;
    const std::string mode = "mode " + std::to_string(modeIndex);

    if (modeIndex >= sky.modes.size()) {
        res.warnings.push_back(mode + ": not in the map's sky; nothing baked");
        return res;
    }
    // gw2-sky.md §3: modes 0/2 use the day* parameters, 1/3 the night* ones.
    // Nothing is known about a mode past 3.
    if (modeIndex > 3) {
        res.warnings.push_back(mode + ": parameter set UNPROVEN (gw2-sky.md §3); nothing baked");
        return res;
    }
    const bool day = modeIndex % 2 == 0;
    const Extractor::MapSkyParams& p = sky.params;
    const Extractor::MapSkyMode& m = sky.modes[modeIndex];

    // §4: everything but the base hemicube is left out. Warn for each such
    // layer this map (and this mode's parameter set) actually has.
    if (sky.starFile) res.warnings.push_back("stars: UNPROVEN (gw2-sky.md §4); not baked");
    if (!sky.clouds.empty()) res.warnings.push_back("clouds: UNPROVEN (gw2-sky.md §4); not baked");
    if (!sky.cards.empty()) res.warnings.push_back("sky cards: UNPROVEN (gw2-sky.md §4); not baked");
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
            it->second.rgba.size() < static_cast<size_t>(it->second.width) * it->second.height * 4) {
            res.warnings.push_back("base: " + std::string(name) + " texture " + std::to_string(id) +
                                   " missing or not decoded; " + mode + " not baked");
            return false;
        }
        out = it->second;
        return true;
    };
    auto cube = std::make_shared<Hemicube>();
    bool have = find(m.ne, "NE", cube->ne);
    have = find(m.sw, "SW", cube->sw) && have;
    have = find(m.top, "T", cube->t) && have;
    if (!have) return res;

    // §5: Brightness = dayBrightness (modes 0, 2) / nightBrightness (modes 1, 3).
    cube->brightness = day ? p.dayBrightness : p.nightBrightness;
    res.radiance = [cube](const float dir[3]) { return cube->radiance(dir); };
    res.layers.push_back("base");
    res.ok = true;
    return res;
}

} // namespace castlemist::exportgltf::sky
