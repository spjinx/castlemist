#include "castlemist/ripper/dye.h"

#include <algorithm>
#include <cmath>

namespace castlemist::ripper {
namespace {

constexpr double kPi = 3.14159265358979323846;

ColorMatrix identity() {
    ColorMatrix m{};
    for (int i = 0; i < 4; ++i) m[i][i] = 1;
    return m;
}

ColorMatrix mul(const ColorMatrix& a, const ColorMatrix& b) {
    ColorMatrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

uint8_t clamp_byte(double v) { return static_cast<uint8_t>(std::clamp(static_cast<int>(v), 0, 255)); }

} // namespace

ColorMatrix dye_matrix(const character::DyeShift& s) {
    const double brightness = s.brightness / 128.0;
    const double contrast = s.contrast;
    const double hue = s.hue * kPi / 180.0;
    ColorMatrix m = identity();
    if (brightness != 0 || contrast != 1) {
        const double t = 128 * (2 * brightness + 1 - contrast);
        m = mul(ColorMatrix{{{contrast, 0, 0, t}, {0, contrast, 0, t}, {0, 0, contrast, t}, {0, 0, 0, 1}}}, m);
    }
    if (hue != 0 || s.saturation != 1 || s.lightness != 1) {
        m = mul(ColorMatrix{{{0.707107, 0.0, -0.707107, 0},
                             {-0.408248, 0.816497, -0.408248, 0},
                             {0.577350, 0.577350, 0.577350, 0},
                             {0, 0, 0, 1}}},
                m);
        const double c = std::cos(hue) * s.saturation, sn = std::sin(hue) * s.saturation;
        m = mul(ColorMatrix{{{c, sn, 0, 0}, {-sn, c, 0, 0}, {0, 0, s.lightness, 0}, {0, 0, 0, 1}}}, m);
        m = mul(ColorMatrix{{{0.707107, -0.408248, 0.577350, 0},
                             {0, 0.816497, 0.577350, 0},
                             {-0.707107, -0.408248, 0.577350, 0},
                             {0, 0, 0, 1}}},
                m);
    }
    return m;
}

std::array<uint8_t, 3> apply_dye(const ColorMatrix& m, std::array<uint8_t, 3> rgb) {
    const double v[4] = {double(rgb[2]), double(rgb[1]), double(rgb[0]), 1.0};  // BGR
    double o[3];
    for (int i = 0; i < 3; ++i) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * v[3];
    return {clamp_byte(o[2]), clamp_byte(o[1]), clamp_byte(o[0])};
}

void bake_dyes(std::vector<uint8_t>& rgba, int w, int h, const std::array<const std::vector<uint8_t>*, 4>& masks,
               const std::array<std::optional<ColorMatrix>, 4>& dyes) {
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (rgba.size() < n * 4) return;
    for (size_t p = 0; p < n; ++p) {
        uint8_t* px = rgba.data() + p * 4;
        const std::array<uint8_t, 3> base = {px[0], px[1], px[2]};
        double out[3] = {double(px[0]), double(px[1]), double(px[2])};
        for (size_t i = 0; i < 4; ++i) {
            if (!masks[i] || !dyes[i] || masks[i]->size() < n * 4) continue;
            const uint8_t* mk = masks[i]->data() + p * 4;
            const double wgt = (mk[0] + mk[1] + mk[2]) / (3.0 * 255.0);
            if (wgt <= 0) continue;
            const std::array<uint8_t, 3> dyed = apply_dye(*dyes[i], base);
            for (int c = 0; c < 3; ++c) out[c] += (dyed[c] - out[c]) * wgt;
        }
        for (int c = 0; c < 3; ++c) px[c] = clamp_byte(out[c] + 0.5);
    }
}

} // namespace castlemist::ripper
