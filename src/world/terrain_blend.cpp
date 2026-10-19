#include "castlemist/world/terrain_blend.h"

namespace castlemist::world {

void terrain_blend(const TerrainBlendInput& in, float albedoOut[3], float weightsOut[4]) {
    // Picker channel -> ground slot: R colord(3), G colorc(2), B colorb(1), A color(0).
    float w[4] = {in.picker.rgba[3], in.picker.rgba[2], in.picker.rgba[1], in.picker.rgba[0]};
    bool any = false;
    for (int i = 0; i < 4; ++i) {
        if (!in.present[i]) w[i] = 0;
        if (w[i] != 0) any = true;
    }
    if (!any) {
        int s = 0;
        if (!in.present[0])
            for (int i = 1; i < 4; ++i)
                if (in.present[i]) { s = i; break; }
        w[s] = 1.0f / 255.0f;
    }
    float k[4], d = 0;
    for (int i = 0; i < 4; ++i) {
        k[i] = w[i] * (in.ground[i].rgba[3] + 0.01f);
        d += k[i];
    }
    for (int i = 0; i < 4; ++i) {
        k[i] /= d;
        weightsOut[i] = k[i];
    }
    float a[3];
    for (int c = 0; c < 3; ++c) {
        a[c] = 0;
        for (int i = 0; i < 4; ++i) a[c] += k[i] * in.ground[i].rgba[c];
    }
    const float lum = 0.30f * a[0] + 0.59f * a[1] + 0.11f * a[2];
    for (int c = 0; c < 3; ++c) {
        float v = lum + (a[c] - lum) * (2 * in.modx.rgba[1]);
        albedoOut[c] = 2 * in.modx.rgba[0] * v;
    }
}

void page_uv(const TerrainMaterial& m, float s, float t, float uv[2]) {
    uv[0] = m.pickerOffset[0] + m.pickerScale * s;
    uv[1] = m.pickerOffset[1] + m.pickerScale * t;
}

void ground_uv(float s, float t, float repeats, float uv[2]) {
    uv[0] = s * repeats;
    uv[1] = t * repeats;
}

} // namespace castlemist::world
