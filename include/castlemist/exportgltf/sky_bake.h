/// @file
/// @brief Sky sampler: turns one GW2 sky mode plus its decoded textures into a
///        radiance callback over Unity directions.
/// @ingroup exportgltf
///
/// Implements what `docs/research/gw2-sky.md` proves: the base hemicube
/// (NE / SW / T, §2) with the sky pixel shader's brightness maths (§5), the
/// star field (§8, twinkle at tw = 0) and texture sky cards (§9, the reduced
/// form without the haze / sun-light terms), composed hemicube -> stars ->
/// cards (§7, an order inferred from the blend states). Every term or layer
/// the note marks UNPROVEN is left out or reduced and named in `warnings`.

#pragma once

#include "castlemist/exportgltf/sky_project.h"
#include "castlemist/native/gw2model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace castlemist::exportgltf::sky {

/// fileId -> decoded RGBA8 image (row 0 = top, i.e. texture v = 0).
using TextureMap = std::unordered_map<uint32_t, Image>;

struct BakeResult {
    Radiance radiance;                     ///< always callable; black when !ok. Owns what it needs.
    std::vector<std::string> layers;       ///< layers actually baked: "base", "stars", "cards"
    std::vector<std::string> warnings;     ///< layers left out and why (UNPROVEN, missing texture)
    bool ok = false;                       ///< false when the mode's base hemicube is missing
    uint32_t starFile = 0, starAtlas = 0;  ///< star sources baked (0 when "stars" is not in layers)
    std::vector<uint32_t> cardTextures;    ///< card textures baked, in card order, no repeats
};

/// How an approximate-sky input is known (gw2-sky.md amendment 2026-10-17).
enum class InputSource {
    Measured,   ///< read from the map data, the data path itself proven
    Derived,    ///< from the map data through a path read in the client's static code
    Guess,      ///< UNPROVEN: no proven source; a stated stand-in
};
/// "measured", "derived from static code", "UNPROVEN guess".
const char* input_source_name(InputSource s);

/// One labelled input of the approximate sky.
struct InputLabel {
    std::string input;     ///< e.g. "FogColorFar"
    InputSource source = InputSource::Guess;
    std::string value;     ///< the value used, printable
    std::string note;      ///< where it comes from
};

/// The inputs the proven-only bake leaves out, for the approximate sky
/// (gw2-sky.md amendment 2026-10-17). With them, make_sky_sampler also bakes
/// the hemicube's horizon haze and sun glow, haze-dominated and lit texture
/// cards, the material cards it has a transcribed shader for, and the cloud
/// layers. Every input carries a label.
struct ApproxInputs {
    bool haveFog = false;
    float fogFar[4] = {0, 0, 0, 1};        ///< FogColorFar, RGBA 0..1
    bool haveSun = false;                  ///< sun colour/direction and SH below are set
    float sunColor[3] = {1, 1, 1};         ///< sunClr.rgb
    float sunDir[3] = {0, 0, 1};           ///< sunDir.xyz, GW2 axes, normalized: the direction the light travels
    float sh[3][4] = {};                   ///< shRed, shGreen, shBlue
    float cameraZ = 0;                     ///< GW2 z of the camera (up = -z), for the cloud planes
    float cardFade = 1, cloudFade = 1;     ///< EnvEnvironment card / cloud fades
    float time = 0;                        ///< the `Time` uniform for material cards
    float alphaRef = 0;                    ///< the `AlphaRef` uniform for material cards
    std::vector<InputLabel> labels;        ///< one per input, copied into the bake's warnings
};

/// Sampler for sky mode @p modeIndex of @p sky. Directions passed to the
/// returned radiance are Unity directions (left-handed, +Y up, +Z north).
/// @p stars is the parsed `starFile` (Extractor::parseStars); its atlas is
/// looked up in @p textures by `stars->atlas`. Sizes use `sky.skyDistance`
/// (F0, gw2-sky.md §7.1). Without @p approx the bake is the proven-only one;
/// with it, the approximate sky (ApproxInputs).
BakeResult make_sky_sampler(const castlemist::model::Extractor::MapSky& sky, size_t modeIndex,
                            const TextureMap& textures,
                            const castlemist::model::Extractor::MapStars* stars = nullptr,
                            const ApproxInputs* approx = nullptr);

/// The AMAT whose sky-card pixel shader the approximate sky transcribes
/// (Mad King's "flow fire" card, PS 12 of AMAT 1262294). Material cards with
/// any other material stay unbaked.
inline constexpr uint32_t kFlowFireMaterial = 1262294;

/// GW2 sky space (X east, Y north, Z down; gw2-sky.md §1) to Unity.
void gw2_to_unity(const float gw2[3], float unity[3]);
/// Inverse of gw2_to_unity.
void unity_to_gw2(const float unity[3], float gw2[3]);

} // namespace castlemist::exportgltf::sky
