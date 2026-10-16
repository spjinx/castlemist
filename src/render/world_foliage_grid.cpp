/// @file
/// @brief FoliageGrid (spec D6): tiles over the map, fade, spheres and the
///        per-frame cull. Pure; no Direct3D.

#include "castlemist/render/world_foliage_grid.h"

#include "castlemist/world/world_scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace castlemist::render {

float foliage_fade(float d, float n, float f) {
    if (!(d < f)) return 0.0f;   // d >= f (and NaN)
    if (n >= f || d <= n) return 1.0f;
    return (f - d) / (f - n);
}

float bayer4x4(int px, int py) {
    static constexpr int kM[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    return (static_cast<float>(kM[py & 3][px & 3]) + 0.5f) / 16.0f;
}

FoliageGrid build_foliage_grid(const castlemist::world::WorldScene& s, float tileUnits) {
    using castlemist::world::ZoneInstance;
    FoliageGrid g;
    g.tileUnits = (tileUnits > 0 && std::isfinite(tileUnits)) ? tileUnits : kFoliageTileUnits;
    const std::vector<ZoneInstance>& zi = s.zones.instances;
    const std::vector<castlemist::world::ZoneLayer>& layers = s.zones.layers;
    const size_t n = zi.size();

    // The extent: the parm rect, else the instances' translations.
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool have = false;
    if (s.hasBounds) {
        x0 = std::min(s.bounds[0], s.bounds[2]); x1 = std::max(s.bounds[0], s.bounds[2]);
        y0 = std::min(s.bounds[1], s.bounds[3]); y1 = std::max(s.bounds[1], s.bounds[3]);
        have = std::isfinite(x0) && std::isfinite(x1) && std::isfinite(y0) && std::isfinite(y1);
    }
    if (!have) {
        for (const ZoneInstance& z : zi) {
            const float x = z.world[12], y = z.world[13];
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            if (!have) { x0 = x1 = x; y0 = y1 = y; have = true; continue; }
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
    }
    g.x0 = x0;
    g.y0 = y0;
    auto tiles_over = [&](float span) {
        const double t = std::ceil(static_cast<double>(span) / g.tileUnits);
        return static_cast<int>(std::clamp(t, 1.0, 65536.0));
    };
    g.tilesX = tiles_over(x1 - x0);
    g.tilesY = tiles_over(y1 - y0);
    g.tiles.resize(static_cast<size_t>(g.tilesX) * g.tilesY);

    // An unresolved layer's fade: the map's largest type-3 fadeRange (by far
    // value; UNPROVEN `zoneFade`). With no type-3 layer, the largest of any
    // layer (castlemist's choice); with no layer at all, [0, 0] (never drawn).
    float fbN = 0, fbF = 0;
    bool fb = false;
    for (int pass = 0; pass < 2 && !fb; ++pass)
        for (const auto& L : layers) {
            if (pass == 0 && L.type != 3) continue;
            if (!std::isfinite(L.fadeRange[1]) || !std::isfinite(L.fadeRange[0])) continue;
            if (!fb || L.fadeRange[1] > fbF) { fbN = L.fadeRange[0]; fbF = L.fadeRange[1]; fb = true; }
        }

    g.tileOf.resize(n);
    g.fadeNear.resize(n);
    g.fadeFar.resize(n);
    g.sphere.assign(n * 4, 0.0f);
    g.collider.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const ZoneInstance& z = zi[i];
        auto cell = [&](float v, float o, int count) {
            const float t = (v - o) / g.tileUnits;
            if (!std::isfinite(t)) return 0;
            return static_cast<int>(std::clamp(std::floor(t), 0.0f, static_cast<float>(count - 1)));
        };
        const int tx = cell(z.world[12], g.x0, g.tilesX), ty = cell(z.world[13], g.y0, g.tilesY);
        const uint32_t t = static_cast<uint32_t>(ty * g.tilesX + tx);
        g.tileOf[i] = t;
        float fn = fbN, ff = fbF;
        if (z.layer < layers.size()) {
            fn = layers[z.layer].fadeRange[0];
            ff = layers[z.layer].fadeRange[1];
        }
        g.fadeNear[i] = fn;
        g.fadeFar[i] = ff;
        g.sphere[i * 4 + 3] = -1.0f;
        g.collider[i] = z.source == castlemist::world::ZoneSource::Collider ? 1 : 0;
        FoliageTile& tile = g.tiles[t];
        tile.instances.push_back(static_cast<uint32_t>(i));
        if (std::isfinite(ff)) tile.farMax = std::max(tile.farMax, ff);
    }
    // Indices were pushed in order, so a stable sort by model gives (model, index).
    for (FoliageTile& tile : g.tiles)
        std::stable_sort(tile.instances.begin(), tile.instances.end(),
                         [&](uint32_t a, uint32_t b) { return zi[a].model < zi[b].model; });
    return g;
}

void foliage_set_sphere(FoliageGrid& g, uint32_t i, const float centre[3], float radius) {
    if (i >= g.tileOf.size() || static_cast<size_t>(i) * 4 + 3 >= g.sphere.size()) return;
    if (!(radius >= 0) || !std::isfinite(radius)) return;
    for (int k = 0; k < 3; ++k)
        if (!std::isfinite(centre[k])) return;
    const uint32_t t = g.tileOf[i];
    if (t >= g.tiles.size()) return;
    float* sp = &g.sphere[static_cast<size_t>(i) * 4];
    for (int k = 0; k < 3; ++k) sp[k] = centre[k];
    sp[3] = radius;
    FoliageTile& tile = g.tiles[t];
    for (int k = 0; k < 3; ++k) {
        const float lo = centre[k] - radius, hi = centre[k] + radius;
        tile.aabbMin[k] = tile.hasAabb ? std::min(tile.aabbMin[k], lo) : lo;
        tile.aabbMax[k] = tile.hasAabb ? std::max(tile.aabbMax[k], hi) : hi;
    }
    tile.hasAabb = true;
}

void foliage_cull(const FoliageGrid& g, const float cam[3], const float planes[6][4], float distanceScale,
                  bool collidersOnly, uint32_t maxVisible, std::vector<FoliageVisible>& out, FoliageCullStats& st) {
    out.clear();
    st = FoliageCullStats{};
    const float scale = (distanceScale > 0 && std::isfinite(distanceScale)) ? distanceScale : 1.0f;
    const size_t n = g.tileOf.size();
    if (g.fadeNear.size() < n || g.fadeFar.size() < n || g.sphere.size() < n * 4 || g.collider.size() < n) return;

    // Step 1: tiles by AABB distance and the frustum (p-vertex test), nearest first.
    std::vector<std::pair<float, uint32_t>> kept;   // (distance to the AABB, tile)
    for (uint32_t t = 0; t < g.tiles.size(); ++t) {
        const FoliageTile& tile = g.tiles[t];
        if (!tile.hasAabb || tile.instances.empty()) continue;
        float d2 = 0;
        for (int k = 0; k < 3; ++k) {
            const float e = std::max({tile.aabbMin[k] - cam[k], 0.0f, cam[k] - tile.aabbMax[k]});
            d2 += e * e;
        }
        const float reach = tile.farMax * scale;
        if (d2 > reach * reach) continue;
        bool outside = false;
        for (int p = 0; p < 6 && !outside; ++p) {
            float v = planes[p][3];
            for (int k = 0; k < 3; ++k)
                v += planes[p][k] * (planes[p][k] >= 0 ? tile.aabbMax[k] : tile.aabbMin[k]);
            outside = v < 0;
        }
        if (outside) continue;
        kept.emplace_back(std::sqrt(d2), t);
    }
    std::sort(kept.begin(), kept.end());
    st.tiles = static_cast<int>(kept.size());

    // Step 2: instances by fade and sphere; the cap, nearest tile first.
    for (const auto& [td, t] : kept) {
        for (uint32_t i : g.tiles[t].instances) {
            if (i >= n) continue;
            if (collidersOnly && !g.collider[i]) continue;
            const float* sp = &g.sphere[static_cast<size_t>(i) * 4];
            if (!(sp[3] >= 0)) continue;   // its model has not arrived
            const float dx = sp[0] - cam[0], dy = sp[1] - cam[1], dz = sp[2] - cam[2];
            const float fade =
                foliage_fade(std::sqrt(dx * dx + dy * dy + dz * dz), g.fadeNear[i] * scale, g.fadeFar[i] * scale);
            if (!(fade > 0)) continue;
            bool outside = false;
            for (int p = 0; p < 6 && !outside; ++p)
                outside = planes[p][0] * sp[0] + planes[p][1] * sp[1] + planes[p][2] * sp[2] + planes[p][3] < -sp[3];
            if (outside) continue;
            if (out.size() >= maxVisible) { ++st.capped; continue; }
            out.push_back({i, fade});
        }
    }
}

} // namespace castlemist::render
