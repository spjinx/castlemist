/// @file
/// @brief The Map window's sky bake and mode choice (see world_sky.h).

#include "castlemist/render/world_sky.h"

#include "castlemist/exportgltf/sky_bake.h"
#include "castlemist/exportgltf/sky_project.h"

#include <algorithm>
#include <cctype>

namespace castlemist::render {

namespace sky = castlemist::exportgltf::sky;

bool parse_world_sky_time(const std::string& text, WorldSkyTime& out) {
    std::string t;
    for (char c : text) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (t == "day") out = WorldSkyTime::Day;
    else if (t == "night") out = WorldSkyTime::Night;
    else if (t == "off") out = WorldSkyTime::Off;
    else return false;
    return true;
}

const char* world_sky_time_name(WorldSkyTime t) {
    switch (t) {
    case WorldSkyTime::Day: return "day";
    case WorldSkyTime::Night: return "night";
    default: return "off";
    }
}

int world_sky_mode(const std::vector<int>& baked, WorldSkyTime t, bool* fellBack) {
    if (fellBack) *fellBack = false;
    if (t == WorldSkyTime::Off) return -1;
    const int want = t == WorldSkyTime::Night ? 1 : 0, other = 1 - want;
    auto has = [&](int m) { return std::find(baked.begin(), baked.end(), m) != baked.end(); };
    if (has(want)) return want;
    if (has(other)) {
        if (fellBack) *fellBack = true;
        return other;
    }
    return -1;
}

std::vector<WorldSkyImage> bake_world_sky(const sky::SkyInputs& in, int width, std::vector<std::string>& warnings) {
    std::vector<WorldSkyImage> out;
    if (!in.sky.present) {
        warnings.push_back("sky: the map has no sky (no env chunk or no dataGlobal)");
        return out;
    }
    width = std::max(16, width & ~1);
    for (size_t mode = 0; mode < 2; ++mode) {
        if (mode >= in.sky.modes.size()) {
            warnings.push_back("sky: no " + sky::mode_name(mode) + " mode in the map's sky");
            continue;
        }
        if (mode == 1 && !out.empty() && out[0].mode == 0 && sky::same_sky_mode(in.sky, 0, 1)) {
            WorldSkyImage copy = out[0];
            copy.mode = 1;
            out.push_back(std::move(copy));
            continue;
        }
        const sky::BakeResult b =
            sky::make_sky_sampler(in.sky, mode, in.textures, in.stars.present ? &in.stars : nullptr);
        for (const std::string& w : b.warnings) warnings.push_back("sky " + sky::mode_name(mode) + ": " + w);
        if (!b.ok) continue;
        sky::Image img = sky::render_equirect(b.radiance, width, width / 2);
        WorldSkyImage w;
        w.mode = static_cast<int>(mode);
        w.width = img.width;
        w.height = img.height;
        w.rgba = std::move(img.rgba);
        out.push_back(std::move(w));
    }
    return out;
}

void world_sky_horizon(const WorldSkyImage& img, float band, float rgb[3]) {
    rgb[0] = rgb[1] = rgb[2] = 0;
    if (img.width <= 0 || img.height <= 0 || img.rgba.size() < static_cast<size_t>(img.width) * img.height * 4) return;
    const int mid = img.height / 2;
    const int half = std::max(1, static_cast<int>(band * static_cast<float>(img.height) * 0.5f));
    const int y0 = std::max(0, mid - half), y1 = std::min(img.height, mid + half);
    double sum[3] = {0, 0, 0};
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < img.width; ++x) {
            const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
            for (int k = 0; k < 3; ++k) sum[k] += p[k];
            ++n;
        }
    if (n == 0) return;
    for (int k = 0; k < 3; ++k) rgb[k] = static_cast<float>(sum[k] / (255.0 * static_cast<double>(n)));
}

} // namespace castlemist::render
