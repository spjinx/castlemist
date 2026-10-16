/// @file
/// @brief The world export's frame probe: three axis bars written as a .glb,
///        and the directions each importer must measure them at.
///
/// Spec docs/superpowers/specs/2026-10-15-world-export-design.md, D6
/// "Frame probe". Every export writes `probe.glb`; the Blender script
/// (`tools/blender/castlemist_world.py --check-probe`) and the Unity importer
/// (`tools/unity/Castlemist/Editor/FrameProbe.cs`) import it, measure the
/// bars and compare against frame_probe_expect(). It is the one automated
/// proof that glb -> Blender -> FBX -> Unity lands on world/frame.h's
/// map_to_unity.
///
/// Check rule (the same in Python and C#):
///  1. each bar's world-space bounds centre minus the probe root's world
///     position, normalised;
///  2. a bar passes when its dot with the expected direction is > 0.99;
///  3. the extents along the three directions are in the ratio 3 : 2 : 1
///     within 1 %.
/// @ingroup exportgltf

#pragma once

#include <string>

#include "nlohmann/json.hpp"

namespace castlemist::exportgltf {

/// @brief Bar lengths in map units: east, north, up.
inline constexpr float kProbeLengths[3] = {300.0f, 200.0f, 100.0f};
/// @brief Bar cross-section side in map units (each other axis in [-10, 10]).
inline constexpr float kProbeThickness = 20.0f;

/// @brief Writes the probe: three boxes from the origin under one root node
///        `GW2_MapToGltf` carrying map_to_gltf_root(1) (world_export.h), all
///        in map space (left-handed, +X east, +Y north, up = -Z):
///        "Probe_East" x in [0, 300], "Probe_North" y in [0, 200],
///        "Probe_Up" z in [-100, 0]; each other axis in [-10, 10]. One unlit
///        material per bar (red, green, blue).
/// @param glbPathUtf8 Destination, UTF-8. Parent directories are created.
/// @param error       When non-null, receives the reason on failure.
bool write_frame_probe(const std::string& glbPathUtf8, std::string* error = nullptr);

/// @brief The directions the bars must be measured at, from world/frame.h:
///        {"unity":{"east":[1,0,0],"north":[0,0,1],"up":[0,1,0]},
///         "blender":{"east":[1,0,0],"north":[0,1,0],"up":[0,0,1]},
///         "lengths":[3,2,1]}
nlohmann::json frame_probe_expect();

} // namespace castlemist::exportgltf
