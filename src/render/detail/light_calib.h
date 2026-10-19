/// @file
/// @brief The measured light calibration for textured game materials: the
///        sun colour and the SH-ambient DC that a map's env rig produces.
///
/// Shared by the Shader-mode material rig (game_material.cpp) and WorldView's
/// textured shaders (kTerrainMat, kPropMat; ruling P6). The numbers are
/// solved for the game's own light buffer (sample1 RT 6826): a fully lit
/// surface near the measured p90 (0.32 + 0.75 = 1.07) and the shadow side
/// on the measured p1 (~0.32). See game_material.cpp for the derivation.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <algorithm>

namespace castlemist::render {

/// Luminance of the default warm sun colour (MapEnvRig::sunColor).
inline constexpr float kCalibSunLum = 0.919f;
/// Luminance of the default cool fill colour (MapEnvRig::fillColor).
inline constexpr float kCalibFillLum = 0.656f;

/// @brief The calibrated sun colour and ambient DC for an env rig.
///
/// sun     = sunColor  * 0.75 / kCalibSunLum * clamp(sunIntensity, 0, 3) * lit
/// ambient = fillColor * clamp(fillIntensity * (0.32 / kCalibFillLum) / 0.3, 0.30, 0.90) * lit
inline void calibrated_light(const float sunColor[3], float sunIntensity, const float fillColor[3],
                             float fillIntensity, float lit, float sun[3], float ambient[3]) {
    const float sMag = (0.75f / kCalibSunLum) * std::clamp(sunIntensity, 0.0f, 3.0f) * lit;
    const float fi = std::clamp(fillIntensity * (0.32f / kCalibFillLum) / 0.3f, 0.30f, 0.90f) * lit;
    for (int k = 0; k < 3; ++k) {
        sun[k] = sunColor[k] * sMag;
        ambient[k] = fillColor[k] * fi;
    }
}

} // namespace castlemist::render
