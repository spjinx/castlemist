/// @file
/// @brief The Map window's sky: which baked mode a time of day shows, and
///        the bake itself (exportgltf's sky sampler, no Direct3D).
/// @ingroup render
#pragma once

#include <string>
#include <vector>

#include "castlemist/exportgltf/sky_export.h"
#include "castlemist/render/world_prep.h"

namespace castlemist::render {

/// @brief "day", "night" or "off" (any case) into @p out; false (and @p out
///        untouched) for anything else.
bool parse_world_sky_time(const std::string& text, WorldSkyTime& out);
/// @brief Lower-case name of @p t ("day", "night", "off").
const char* world_sky_time_name(WorldSkyTime t);

/// @brief "approx" / "proven".
const char* world_sky_kind_name(WorldSkyKind k);
/// @brief GW2_MAPWIN_SKY: words separated by commas or spaces, each a time
///        ("day", "night", "off") or a kind ("approx", "proven"), any case.
///        Sets only what the text names; false (both untouched) for an empty
///        text or an unknown word.
bool parse_world_sky(const std::string& text, WorldSkyTime& time, WorldSkyKind& kind);

/// @brief The sky mode to draw for @p t from the modes in @p baked: Day ->
///        0, Night -> 1; when that one was not baked, the other (and
///        @p fellBack, when non-null, is set true); -1 for Off or when
///        neither was baked.
int world_sky_mode(const std::vector<int>& baked, WorldSkyTime t, bool* fellBack = nullptr);

/// @brief Bakes sky modes 0 and 1 of @p in (load_sky_textures filled its
///        textures) as @p width x @p width/2 equirects with
///        make_sky_sampler and render_equirect. A mode that is missing, has
///        no hemicube, or does not bake is left out, with a line in
///        @p warnings; mode 1 repeating mode 0 (same_sky_mode) reuses its
///        pixels. @p approximate bakes the approximate sky (approx_inputs;
///        its warnings carry every input's label, and night is baked on its
///        own since its haze and light slots differ). Read-only on @p in;
///        safe on any thread.
std::vector<WorldSkyImage> bake_world_sky(const castlemist::exportgltf::sky::SkyInputs& in, int width,
                                          std::vector<std::string>& warnings, bool approximate = false);

/// @brief The mean colour of the rows within @p band (fraction of the height)
///        of the horizon (the middle row) of an equirect, 0..1 display
///        values; {0, 0, 0} for an empty image.
void world_sky_horizon(const WorldSkyImage& img, float band, float rgb[3]);

} // namespace castlemist::render
