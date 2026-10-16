#include "castlemist/world/terrain_bake.h"

#include "castlemist/world/cells.h"
#include "castlemist/world/surface_rules.h"

#include "chunk_grid.h"

#include <algorithm>
#include <cmath>

namespace castlemist::world {

namespace {

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

uint8_t to_byte(float v) {
    return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

BlendTexel solid_texel(const uint8_t b[4]) {
    BlendTexel t{};
    for (int c = 0; c < 4; ++c) t.rgba[c] = b[c] / 255.0f;
    return t;
}

/// Bilinear terrain height at map (x, y); found = false off the terrain. @p g is
/// detail::make_grid(t), built once per pass (review M5: it was rebuilt, with an
/// allocation, for every texel, and the chunk was taken from a dense row-major
/// index; now it is the chunk at (cx, cy) or none).
float terrain_z(const Terrain& t, const detail::Grid& g, float x, float y, bool& found) {
    found = false;
    if (!t.present || t.chunks.empty() || g.nx <= 0 || g.ny <= 0) return 0;
    int cx, cy;
    detail::point_chunk(g, x, y, cx, cy);
    const TerrainChunk* cp = g.get(cx, cy);
    if (!cp) return 0;
    const TerrainChunk& c = *cp;
    const int n = c.samples;
    if (n < 2 || c.heights.size() < static_cast<size_t>(n) * n) return 0;
    const float w = c.rect[2] - c.rect[0], h = c.rect[3] - c.rect[1];
    if (!(w > 0) || !(h > 0)) return 0;
    const float gx = std::clamp((x - c.rect[0]) / w, 0.0f, 1.0f) * (n - 1);
    const float gy = std::clamp((c.rect[3] - y) / h, 0.0f, 1.0f) * (n - 1);
    const int i0 = std::min(static_cast<int>(gx), n - 2), j0 = std::min(static_cast<int>(gy), n - 2);
    const float fx = gx - i0, fy = gy - j0;
    auto H = [&](int i, int j) { return c.heights[static_cast<size_t>(j) * n + i]; };
    found = true;
    return (H(i0, j0) * (1 - fx) + H(i0 + 1, j0) * fx) * (1 - fy) +
           (H(i0, j0 + 1) * (1 - fx) + H(i0 + 1, j0 + 1) * fx) * fy;
}

} // namespace

int ground_mip_level(int groundWidth, float repeats, int texelsPerChunk, int mipCount) {
    if (mipCount <= 1 || groundWidth <= 0 || texelsPerChunk <= 0 || !(repeats > 0)) return 0;
    const double l = std::log2(static_cast<double>(repeats) * groundWidth / texelsPerChunk);
    const long m = std::lround(l);
    return static_cast<int>(std::clamp<long>(m, 0, mipCount - 1));
}

float linear_to_srgb(float c) {
    if (!(c > 0)) return 0.0f;
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

BlendTexel sample_bilinear(const CappedTexture& t, int mip, float u, float v, bool wrap, bool srgb) {
    BlendTexel out{};
    if (t.mips.empty()) return out;
    mip = std::clamp(mip, 0, static_cast<int>(t.mips.size()) - 1);
    const int w = std::max(1, t.width >> mip), h = std::max(1, t.height >> mip);
    const std::vector<uint8_t>& px = t.mips[static_cast<size_t>(mip)];
    if (px.size() < static_cast<size_t>(w) * h * 4) return out;
    const float fu = u * w - 0.5f, fv = v * h - 0.5f;
    const float flu = std::floor(fu), flv = std::floor(fv);
    const float tx = fu - flu, ty = fv - flv;
    auto addr = [&](int i, int n) {
        if (wrap) { i %= n; return i < 0 ? i + n : i; }
        return std::clamp(i, 0, n - 1);
    };
    const int x0 = addr(static_cast<int>(flu), w), x1 = addr(static_cast<int>(flu) + 1, w);
    const int y0 = addr(static_cast<int>(flv), h), y1 = addr(static_cast<int>(flv) + 1, h);
    auto fetch = [&](int x, int y, int c) {
        const float b = px[(static_cast<size_t>(y) * w + x) * 4 + c] / 255.0f;
        return (srgb && c < 3) ? srgb_to_linear(b) : b;
    };
    for (int c = 0; c < 4; ++c) {
        const float top = fetch(x0, y0, c) * (1 - tx) + fetch(x1, y0, c) * tx;
        const float bot = fetch(x0, y1, c) * (1 - tx) + fetch(x1, y1, c) * tx;
        out.rgba[c] = top * (1 - ty) + bot * ty;
    }
    return out;
}

bool bake_chunk_albedo(const TerrainChunk& c, const TextureLookup& tex, const BakeSettings& s,
                       std::vector<uint8_t>& rgbOut) {
    const int T = std::max(1, s.texelsPerChunk);
    rgbOut.assign(static_cast<size_t>(T) * T * 3, 128);
    const TerrainMaterial& m = c.material;
    if (!m.resolved) return false;

    auto get = [&](uint32_t id) -> const CappedTexture* {
        if (id == 0 || !tex) return nullptr;
        const CappedTexture* t = tex(id);
        return (t && !t->mips.empty()) ? t : nullptr;
    };
    const CappedTexture* ground[4] = {};
    int mip[4] = {};
    bool any = false;
    for (int k = 0; k < 4; ++k) {
        if (static_cast<size_t>(k) < m.textureFileIds.size()) ground[k] = get(m.textureFileIds[k]);
        if (ground[k]) {
            any = true;
            mip[k] = ground_mip_level(ground[k]->width, s.groundRepeats, T, static_cast<int>(ground[k]->mips.size()));
        }
    }
    if (!any) return false;
    const CappedTexture* picker = get(m.pickerFileId);
    const CappedTexture* modx = get(m.picker2FileId);
    const BlendTexel pickerSolid = solid_texel(m.pickerSolid), modxSolid = solid_texel(m.picker2Solid);

    for (int j = 0; j < T; ++j)
        for (int i = 0; i < T; ++i) {
            const float sx = (i + 0.5f) / T, ty = (j + 0.5f) / T;
            float puv[2], guv[2];
            page_uv(m, sx, ty, puv);
            ground_uv(sx, ty, s.groundRepeats, guv);
            TerrainBlendInput in;
            in.picker = picker ? sample_bilinear(*picker, 0, puv[0], puv[1], false, false) : pickerSolid;
            in.modx = modx ? sample_bilinear(*modx, 0, puv[0], puv[1], false, false) : modxSolid;
            for (int k = 0; k < 4; ++k) {
                in.present[k] = ground[k] != nullptr;
                if (ground[k]) in.ground[k] = sample_bilinear(*ground[k], mip[k], guv[0], guv[1], true, s.srgbColour);
            }
            float alb[3], w[4];
            terrain_blend(in, alb, w);
            uint8_t* o = &rgbOut[(static_cast<size_t>(j) * T + i) * 3];
            for (int ch = 0; ch < 3; ++ch) {
                const float a = std::clamp(alb[ch], 0.0f, 1.0f);
                o[ch] = to_byte(s.srgbColour ? linear_to_srgb(a) : a);
            }
        }
    return true;
}

bool water_has_preset(const WorldScene& s) {
    const int pi = s.environment.light.presetIndex;
    return pi >= 0 && static_cast<size_t>(pi) < s.environment.water.size();
}

bool water_below_plane(const WorldScene& s, const float rect[4], int texelsW, int texelsH) {
    if (texelsW <= 0 || texelsH <= 0) return false;
    const float planeZ = s.water.planeZ;
    const detail::Grid g = detail::make_grid(s.terrain);
    for (int j = 0; j < texelsH; ++j)
        for (int i = 0; i < texelsW; ++i) {
            const float x = rect[0] + (i + 0.5f) / texelsW * (rect[2] - rect[0]);
            const float y = rect[3] - (j + 0.5f) / texelsH * (rect[3] - rect[1]);
            bool found;
            const float z = terrain_z(s.terrain, g, x, y, found);
            if (found && z > planeZ) return true;
        }
    return false;
}

bool bake_cell_water(const WorldScene& s, const float rect[4], int texelsW, int texelsH, bool srgbColour,
                     std::vector<uint8_t>& rgbaOut) {
    if (!water_has_preset(s) || !water_below_plane(s, rect, texelsW, texelsH)) return false;
    const int pi = s.environment.light.presetIndex;
    const auto& preset = s.environment.water[static_cast<size_t>(pi)];
    float sh[3], dp[3];
    water_colour(preset.surfaceShallowColor, sh);
    water_colour(preset.surfaceDeepColor, dp);
    if (srgbColour)
        for (int k = 0; k < 3; ++k) { sh[k] = srgb_to_linear(sh[k]); dp[k] = srgb_to_linear(dp[k]); }
    const float planeZ = s.water.planeZ;
    const detail::Grid g = detail::make_grid(s.terrain);

    std::vector<uint8_t> out(static_cast<size_t>(texelsW) * texelsH * 4);
    for (int j = 0; j < texelsH; ++j)
        for (int i = 0; i < texelsW; ++i) {
            const float x = rect[0] + (i + 0.5f) / texelsW * (rect[2] - rect[0]);
            const float y = rect[3] - (j + 0.5f) / texelsH * (rect[3] - rect[1]);
            bool found;
            const float z = terrain_z(s.terrain, g, x, y, found);
            const float tz = found ? z : planeZ;
            const float mix = water_depth_mix(tz, planeZ, preset.depthAttenuation);
            uint8_t* o = &out[(static_cast<size_t>(j) * texelsW + i) * 4];
            for (int k = 0; k < 3; ++k) {
                const float v = sh[k] + (dp[k] - sh[k]) * mix;
                o[k] = to_byte(srgbColour ? linear_to_srgb(v) : v);
            }
            o[3] = static_cast<uint8_t>(std::lround(kFlatWaterAlpha * 255.0f));
        }
    rgbaOut = std::move(out);
    return true;
}

} // namespace castlemist::world
