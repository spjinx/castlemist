/// @file
/// @brief A map's environment light rig (MapEnvRig), as the renderer's game lighting takes it.
/// @ingroup extract

#pragma once

#include <cstdint>
#include <vector>

#include "castlemist/extract/model_types.h"

/// @addtogroup extract
/// @{

/// @brief A map's real environment light rig, from the `env` chunk's day presets.
///
/// The chunk stores `PackMapEnvDataLightV*` records (byte3 colour, float
/// intensity, float3 direction). The highest-intensity light is taken as the
/// sun; the rest are collapsed into one intensity-weighted fill/sky ambient.
/// The renderer feeds this into the game lighting shaders so a model is lit by
/// the map's own light instead of a hardcoded stand-in.
///
/// When @ref present is false the renderer uses its default rig.
struct MapEnvRig {
    bool  present = false;                      ///< A usable rig was parsed.
    float sunDir[3] = {0.45f, 0.80f, -0.40f};   ///< Normalized, points *toward* the sun (-Z is up).
    float sunColor[3] = {1.00f, 0.91f, 0.75f};  ///< Linear RGB, 0..1.
    float sunIntensity = 1.0f;                  ///< Sun strength.
    float fillColor[3] = {0.49f, 0.68f, 0.97f}; ///< Intensity-weighted mean of the non-sun lights.
    float fillIntensity = 0.3f;                 ///< Summed fill intensity (drives ambient strength).
    int   lightCount = 0;                       ///< Lights found in the chunk.
};

/// @}
