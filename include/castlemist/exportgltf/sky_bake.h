/// @file
/// @brief Sky sampler: turns one GW2 sky mode plus its decoded textures into a
///        radiance callback over Unity directions.
/// @ingroup exportgltf
///
/// Implements only what `docs/research/gw2-sky.md` proves: the base hemicube
/// (NE / SW / T, §2) with the sky pixel shader's brightness maths (§5). Every
/// layer that note marks UNPROVEN is left out and named in `warnings`.

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
    std::vector<std::string> layers;       ///< layers actually baked ("base")
    std::vector<std::string> warnings;     ///< layers left out and why (UNPROVEN, missing texture)
    bool ok = false;                       ///< false when the mode's base hemicube is missing
};

/// Sampler for sky mode @p modeIndex of @p sky. Directions passed to the
/// returned radiance are Unity directions (left-handed, +Y up, +Z north).
BakeResult make_sky_sampler(const castlemist::model::Extractor::MapSky& sky, size_t modeIndex,
                            const TextureMap& textures);

/// GW2 sky space (X east, Y north, Z down; gw2-sky.md §1) to Unity.
void gw2_to_unity(const float gw2[3], float unity[3]);
/// Inverse of gw2_to_unity.
void unity_to_gw2(const float unity[3], float gw2[3]);

} // namespace castlemist::exportgltf::sky
