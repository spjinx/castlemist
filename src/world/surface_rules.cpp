/// @file
/// @brief Pure surface rules (surface_rules.h), moved from render/world_materials.cpp.

#include "castlemist/world/surface_rules.h"

#include <cmath>

namespace castlemist::world {

bool dark_coverage(const std::vector<uint8_t>& rgba) {
    const size_t n = rgba.size() / 4;
    const size_t step = n > 65536 ? n / 65536 : 1;   // as compute_alpha_cutout: ~64k samples
    size_t passing = 0;
    for (size_t p = 0; p < n; p += step) {
        const uint8_t* t = &rgba[p * 4];
        if (t[3] < 64) continue;   // discarded by the clip (alpha < 0.25)
        if (t[0] > 16 || t[1] > 16 || t[2] > 16) return false;
        ++passing;
    }
    return passing > 0;
}

bool tex_transform_animates(const ModelTexTransformCPU& t) {
    return t.columns == 1 && t.rows == 1 && t.count == 1 && t.scrollFreq[0] == 0 && t.scrollFreq[1] == 0 &&
           (t.scroll[0] != 0 || t.scroll[1] != 0) && t.scale[0] == 0 && t.scale[1] == 0 && t.scaleFreq[0] == 0 && t.scaleFreq[1] == 0 && t.rotate == 0 &&
           t.rotate2 == 0;
}

void water_colour(const uint8_t stored[4], float rgb[3]) {
    // UNPROVEN (note §6): bytes 0, 1, 2 as R, G, B. The client reads the env
    // light and haze colours B, G, R (parseMapEnv; final review I1); the water
    // import has not been checked the same way, so this is left as it was.
    for (int k = 0; k < 3; ++k) rgb[k] = static_cast<float>(stored[k]) / 255.0f;
}

float water_depth_mix(float terrainZ, float planeZ, float depthAttenuation) {
    if (!(depthAttenuation > 0) || !std::isfinite(depthAttenuation)) return 0.0f;
    const float t = (terrainZ - planeZ) / depthAttenuation;
    if (!(t > 0)) return 0.0f;   // also NaN
    return t < 1 ? t : 1.0f;
}

} // namespace castlemist::world
