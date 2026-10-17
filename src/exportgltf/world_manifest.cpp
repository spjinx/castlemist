/// @file
/// @brief The world export's manifest.json v1 (world_manifest.h; spec
///        docs/superpowers/specs/2026-10-15-world-export-design.md
///        "manifest.json (version 1)", D3, D5, D7).

#include "castlemist/exportgltf/world_manifest.h"

#include "castlemist/exportgltf/world_cell.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/exportgltf/world_unity_terrain.h"
#include "castlemist/world/frame.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain_bake.h"
#include "castlemist/world/zone_scatter.h"
#include "long_path.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

#ifndef CASTLEMIST_VERSION_STRING
#define CASTLEMIST_VERSION_STRING "unknown"
#endif

namespace castlemist::exportgltf {

using nlohmann::json;
namespace cw = castlemist::world;

namespace {

bool finite_all(const float* v, size_t n) {
    for (size_t k = 0; k < n; ++k)
        if (!std::isfinite(v[k])) return false;
    return true;
}

json vec(const float* v, size_t n) {
    json a = json::array();
    for (size_t k = 0; k < n; ++k) a.push_back(v[k]);
    return a;
}

json str_or_null(const std::string& s) { return s.empty() ? json(nullptr) : json(s); }

/// Rows of the 3x3 matrix of a frame.h vector conversion (row i = output axis i).
json conversion_rows(void (*conv)(const float*, float*)) {
    float cols[3][3];
    for (int j = 0; j < 3; ++j) {
        float e[3] = {0, 0, 0};
        e[j] = 1;
        conv(e, cols[j]);
    }
    json rows = json::array();
    for (int i = 0; i < 3; ++i) rows.push_back({int(cols[0][i]), int(cols[1][i]), int(cols[2][i])});
    return rows;
}

/// The JSON path of the first non-finite number under @p v, or "" when every number is finite.
std::string first_non_finite(const json& v, const std::string& path) {
    if (v.is_number_float()) return std::isfinite(v.get<double>()) ? std::string() : (path.empty() ? "<root>" : path);
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            std::string p = first_non_finite(it.value(), path.empty() ? it.key() : path + "." + it.key());
            if (!p.empty()) return p;
        }
    } else if (v.is_array()) {
        for (size_t k = 0; k < v.size(); ++k) {
            std::string p = first_non_finite(v[k], path + "[" + std::to_string(k) + "]");
            if (!p.empty()) return p;
        }
    }
    return {};
}

/// Spec W6: an optional block holding a non-finite number is written as null,
/// with a warning naming the field. Returns the block (or null).
json finite_or_null(json block, const std::string& name, std::vector<std::string>& warnings) {
    const std::string bad = first_non_finite(block, name);
    if (bad.empty()) return block;
    warnings.push_back("manifest: non-finite number at " + bad + "; " + name + " written as null");
    return nullptr;
}

/// Three finite numbers from a JSON array, or false.
bool finite_vec3(const json& a, float out[3]) {
    if (!a.is_array() || a.size() != 3) return false;
    for (size_t k = 0; k < 3; ++k) {
        if (!a[k].is_number()) return false;
        out[k] = a[k].get<float>();
        if (!std::isfinite(out[k])) return false;
    }
    return true;
}

/// uvScroll entries with only the schema's keys; `unit` is always the const.
/// An entry without a string material, a non-negative integer uvIndex and two
/// finite scroll numbers is dropped with a warning.
json uv_scroll_json(const ManifestModel& m, std::vector<std::string>& warnings) {
    json uv = json::array();
    if (!m.uvScroll.is_array()) return uv;
    for (const json& e : m.uvScroll) {
        const bool ok = e.is_object() && e.contains("material") && e["material"].is_string() && e.contains("uvIndex") &&
                        e["uvIndex"].is_number_integer() && e["uvIndex"].get<int64_t>() >= 0 && e.contains("scroll") &&
                        e["scroll"].is_array() && e["scroll"].size() == 2 && e["scroll"][0].is_number() &&
                        e["scroll"][1].is_number() && std::isfinite(e["scroll"][0].get<double>()) &&
                        std::isfinite(e["scroll"][1].get<double>());
        if (!ok) {
            warnings.push_back("models: fileId " + std::to_string(m.fileId) + ": a malformed uvScroll entry dropped");
            continue;
        }
        uv.push_back({{"material", e["material"]}, {"uvIndex", e["uvIndex"]}, {"scroll", e["scroll"]},
                      {"unit", "per second, UNPROVEN"}});
    }
    return uv;
}

json model_json(const ManifestModel& m, std::vector<std::string>& warnings) {
    json j = {{"index", m.index},
              {"fileId", m.fileId},
              {"folder", str_or_null(m.folder)},
              {"glb", str_or_null(m.glb)},
              {"fbx", nullptr},
              {"materials", str_or_null(m.materials)},
              {"triangles", m.triangles},
              {"textureBytes", m.textureBytes},
              {"clips", m.clips},
              {"darkDecalMaterials", m.darkDecalMaterials},
              {"uvScroll", uv_scroll_json(m, warnings)},
              {"status", m.ok ? "ok" : "failed"}};
    if (!m.ok || !m.error.empty()) j["error"] = m.error;
    return j;
}

json terrain_json(const cw::Terrain& t, const cw::ChunkRegion& r, std::vector<std::string>& warnings) {
    if (!t.present || t.chunks.empty()) return {{"present", false}};
    const cw::TerrainChunk& k = t.chunks.front();
    json region = r.whole() ? json{0, 0, t.chunksX - 1, t.chunksY - 1} : json{r.cx0, r.cy0, r.cx1, r.cy1};
    json j = {{"present", true},
              {"chunksX", t.chunksX},
              {"chunksY", t.chunksY},
              {"segments", std::max(0, k.samples - 1)},
              {"region", region}};
    const float size[2] = {k.rect[2] - k.rect[0], k.rect[3] - k.rect[1]};
    if (finite_all(size, 2))
        j["chunkSize"] = vec(size, 2);
    else
        warnings.push_back("manifest: non-finite number at terrain.chunkSize; chunkSize left out");
    return j;
}

json water_json(const cw::WorldScene& s) {
    if (!s.water.hasPlane) return {{"plane", false}};
    json w = {{"plane", true}, {"z", s.water.planeZ}};
    if (cw::water_has_preset(s)) {
        const int pi = s.environment.light.presetIndex;
        const auto& p = s.environment.water[size_t(pi)];
        auto colour = [](const uint8_t stored[4]) {
            float rgb[3];
            cw::water_colour(stored, rgb);   // bytes 0..2 as RGB, order UNPROVEN (materials note §6)
            return json{rgb[0], rgb[1], rgb[2], float(stored[3]) / 255.0f};
        };
        w["preset"] = pi;
        w["shallow"] = colour(p.surfaceShallowColor);
        w["deep"] = colour(p.surfaceDeepColor);
        w["depthAttenuation"] = p.depthAttenuation;
    } else {
        w["preset"] = nullptr;
        w["flatColour"] = {cw::kFlatWaterTint[0], cw::kFlatWaterTint[1], cw::kFlatWaterTint[2], cw::kFlatWaterAlpha};
        w["note"] = kFlatWaterNote;
    }
    return w;
}

} // namespace

bool unity_trs(const float mapWorld[16], const float cellOrigin[3], UnityTrs& out) {
    if (!finite_all(mapWorld, 16) || !finite_all(cellOrigin, 3)) return false;
    float local[16], U[16];
    std::copy(mapWorld, mapWorld + 16, local);
    for (int k = 0; k < 3; ++k) local[12 + k] = float(double(mapWorld[12 + k]) - double(cellOrigin[k]));
    cw::map_matrix_to_unity(local, U);
    if (!finite_all(U, 16)) return false;
    // An affine matrix: bottom row (0, 0, 0, 1).
    if (std::fabs(U[3]) > 1e-6f || std::fabs(U[7]) > 1e-6f || std::fabs(U[11]) > 1e-6f ||
        std::fabs(U[15] - 1.0f) > 1e-6f)
        return false;

    // Column-major: element (row r, col c) is U[c * 4 + r].
    double col[3][3];
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) col[c][r] = U[c * 4 + r];
    auto dot = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const double len[3] = {std::sqrt(dot(col[0], col[0])), std::sqrt(dot(col[1], col[1])),
                           std::sqrt(dot(col[2], col[2]))};
    const double s = len[0];
    if (!(s > 0) || !std::isfinite(s)) return false;
    constexpr double kTol = 1e-4;
    for (int c = 1; c < 3; ++c)
        if (std::fabs(len[c] - s) > kTol * s) return false;           // non-uniform
    if (std::fabs(dot(col[0], col[1])) > kTol * s * s || std::fabs(dot(col[0], col[2])) > kTol * s * s ||
        std::fabs(dot(col[1], col[2])) > kTol * s * s)
        return false;                                                // shear
    const double det = col[0][0] * (col[1][1] * col[2][2] - col[2][1] * col[1][2]) -
                       col[1][0] * (col[0][1] * col[2][2] - col[2][1] * col[0][2]) +
                       col[2][0] * (col[0][1] * col[1][2] - col[1][1] * col[0][2]);
    if (!(det > 0)) return false;                                    // mirror (or degenerate)

    // R = U3 / s, R[r][c]; Shepperd's method picks the largest of w, x, y, z.
    double R[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) R[r][c] = col[c][r] / s;
    const double tr = R[0][0] + R[1][1] + R[2][2];
    double x, y, z, w;
    if (tr >= R[0][0] && tr >= R[1][1] && tr >= R[2][2]) {
        w = 0.5 * std::sqrt(std::max(0.0, 1.0 + tr));
        const double f = 0.25 / w;
        x = (R[2][1] - R[1][2]) * f; y = (R[0][2] - R[2][0]) * f; z = (R[1][0] - R[0][1]) * f;
    } else if (R[0][0] >= R[1][1] && R[0][0] >= R[2][2]) {
        x = 0.5 * std::sqrt(std::max(0.0, 1.0 + R[0][0] - R[1][1] - R[2][2]));
        const double f = 0.25 / x;
        w = (R[2][1] - R[1][2]) * f; y = (R[0][1] + R[1][0]) * f; z = (R[0][2] + R[2][0]) * f;
    } else if (R[1][1] >= R[2][2]) {
        y = 0.5 * std::sqrt(std::max(0.0, 1.0 + R[1][1] - R[0][0] - R[2][2]));
        const double f = 0.25 / y;
        w = (R[0][2] - R[2][0]) * f; x = (R[0][1] + R[1][0]) * f; z = (R[1][2] + R[2][1]) * f;
    } else {
        z = 0.5 * std::sqrt(std::max(0.0, 1.0 + R[2][2] - R[0][0] - R[1][1]));
        const double f = 0.25 / z;
        w = (R[1][0] - R[0][1]) * f; x = (R[0][2] + R[2][0]) * f; y = (R[1][2] + R[2][1]) * f;
    }
    if (w < 0) { x = -x; y = -y; z = -z; w = -w; }
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 0) || !std::isfinite(n)) return false;
    UnityTrs t{};
    for (int k = 0; k < 3; ++k) t.t[k] = U[12 + k];
    t.r[0] = float(x / n); t.r[1] = float(y / n); t.r[2] = float(z / n); t.r[3] = float(w / n);
    t.s = float(s);
    if (!finite_all(t.t, 3) || !finite_all(t.r, 4) || !std::isfinite(t.s)) return false;
    out = t;
    return true;
}

std::vector<ManifestSetting> world_export_settings(const WorldFolderOptions& o, const cw::WorldScene* scene) {
    const std::string kU = "UNPROVEN", kC = "castlemist's choice", kM = "measured";
    std::vector<ManifestSetting> v = {
        {"groundRepeatsPerChunk", o.groundRepeats, kU, "T3D's guess, docs/research/gw2-world-materials.md \xc2\xa7" "1.6"},
        {"srgbColourTextures", o.srgbColour, kC, "the map window's srgbColourTextures (sub-project 3)"},
        {"pickerSolidByteOrder", "as stored", kU, "docs/research/gw2-world-frame.md \xc2\xa7" "4.2"},
        {"waterPresetPairing", "preset k with lighting preset k", kU,
         "docs/research/gw2-world-materials.md \xc2\xa7" "6"},
        {"waterArea", "whole cell, depth-tested", kU, "docs/research/gw2-world-frame.md \xc2\xa7" "6.2"},
        {"waterAlpha", cw::kFlatWaterAlpha, kC, "the map window's flat-water alpha (world/surface_rules.h)"},
        {"uvScrollUnitSeconds", 1, kU, "docs/research/gw2-world-materials.md \xc2\xa7" "4"},
        {"darkDecalRule", true, kC,
         "castlemist's own rule (sub-project 3 amendments); it can misfire on black cutout props"},
        {"blendModes", "the VRChat export's renderPreset and raw factors (exact: false where approximated)", kC,
         "exportgltf/blend_mode.h; the map window's exact-factor table (sub-project 3 P7/P8) governs only the renderer"},
        {"collisionHullSelection", "animations[last]", kU, "docs/research/gw2-world-frame.md \xc2\xa7" "7.1"},
        {"unityScale", o.unityScale, kU, "gltf_export.h:57 'GW2 inches'; spec D7"},
        {"clipPhase", "all at t = 0, looping", kU, "docs/research/gw2-world-materials.md \xc2\xa7" "5"},
        {"texelsPerChunk", o.texelsPerChunk, kC, "spec D3 (allowed 64, 128, 256, 512)"},
        {"waterTexelsPerChunk", o.waterTexelsPerChunk, kC, "spec D3"},
        {"maxTextureSize", o.maxTextureSize, kC, "the map window's texture cap (sub-project 3); spec Performance"},
        {"cellMaxChunks", o.budget.maxChunks, kC, "spec D1 (or 1/pickerScale)"},
        {"cellMaxTriangles", o.budget.maxTriangles, kC,
         "spec D1 budget rule: Queensdale p90 block rounded up to 500 000 (world export Task 13); VRChat states no per-area limit"},
        {"cellMaxTextureMB", o.budget.maxTextureMB, kC, "spec D1; VRChat states no per-area limit"},
        {"ambient", "fillColor * min(fillIntensity, 1)", kC, "spec D5 (raw intensities, flat ambient)"},
        {"unityAxisConversion", "ModelImporter.bakeAxisConversion = true: east +X, north +Z, up +Y", kM,
         "frame probe on Unity 6000.0.42f1 (world export Task 1) and 2022.3.22f1 (Task 12); bake off fails it, on passes"},
    };
    // World foliage (docs/superpowers/specs/2026-10-16-world-foliage-design.md D11; the
    // terrain rows come with the unityTerrain block).
    const std::string F = "docs/superpowers/specs/2026-10-16-world-foliage-design.md ";
    const cw::ZoneScatterMode mode = scene ? scene->zones.mode : cw::ZoneScatterMode::Colliders;
    const bool colliders = mode == cw::ZoneScatterMode::Colliders;
    const char* scatter = mode == cw::ZoneScatterMode::Exact            ? "exact"
                          : mode == cw::ZoneScatterMode::DensityMatched ? "densityMatched"
                                                                        : "colliders";
    // The D5 rule rows. Colliders (phase 1): each collider entry at its own stored transform.
    // exact: the client's rule (docs/research/gw2-terrain-to-unity.md §1.3, 2026-10-16); its
    // single-instance path (type 3) reproduces 4750 / 4750 colliders, so those rows are
    // measured, and the tiling path's rows are UNPROVEN (ruling S3). densityMatched: UNPROVEN.
    const bool exact = mode == cw::ZoneScatterMode::Exact;
    const std::string R = "docs/research/gw2-terrain-to-unity.md Â§" "1.3 (2026-10-16), ";
    auto rule = [&](const char* name, const std::string& exactValue, const std::string& exactSource) {
        if (colliders)
            return ManifestSetting{name, "the collider entry's own transform (no scatter rule in force)", kM,
                                   F + "D5; phase 1 places the havk zoneModels entries only"};
        if (exact) return ManifestSetting{name, exactValue, kM, R + exactSource + "; 4750 / 4750 colliders"};
        return ManifestSetting{name, "the D5 step 4 rule (castlemist's own)", kU, F + "D5 step 4"};
    };
    const std::vector<ManifestSetting> foliage = {
        {"zoneGrid", "48 units, origin parm (x0, y0), rows north; cell p of vertRect at vertex (vr0 + p mod w, vr1 + p / w)",
         kM, F + "M2, with " + R + "the client decode (0x140cd0410 / 0x140cd0660, 0x140cc8150)"},
        {"zoneModelSlot", "(flag >> 1) & 7 into modelArray", kM, F + "M3"},
        {"zoneFlagBit0", "selects the path: 1 = tiling (0x140cc1900), 0 = single instance (0x140cc1f50)", kM,
         R + "read in the code (0x140cc23c0); set on every type-1/2 cell, clear on every type-3 cell"},
        {"zoneTreeLink", "anchor Â±" "24, then geometry vote", kM,
         F + "D3 (anchor candidates are type-3 placements, ruling S1)"},
        {"zoneVisualTransform", "client_world_matrix(translate, rotate, scale)", kU, F + "D4"},
        {"zoneScatter", scatter, mode == cw::ZoneScatterMode::DensityMatched ? kU : kM,
         exact ? R + "reproduces 4750 / 4750 colliders (single-instance path); the tiling path is UNPROVEN, "
                     "see zoneTiling"
               : F + "D5"},
        {"zonePlacementMultiplicity",
         exact ? "type 3: 1, plus the subModel instance; types 1-2: irange(1, T) by the tiling path"
               : "1 instance per placement",
         kU, F + "M2 (measured for type 3; UNPROVEN for types 1-2 and the subModel)"},
        rule("zoneJitter", "P + (24 - 48 fB(), 24 - 48 fB()) unless pack flag 0x1; Park-Miller 48271 reseeded per cell",
             "0x140cc2880, seed table 0x142592280"),
        rule("zoneYaw", "(rotRange.z0 + fB() (z1 - z0)) * pi / 180", "0x140cc2af0"),
        rule("zoneScale", "scaleRange[0] + roundEven((s - s0) / (99/1280)) (99/1280), s uniform", "0x140cc1f50, 0x141e449c4"),
        rule("zoneTilt",
             "per model flag: random from rotRange x/y, or the ground normal (terrain plane NW,NE,SW when fx <= fy, else "
             "NE,SW,SE; collideData under zoneFlags bit 1), x0.35 with pack flag 0x4; else upright",
             "0x140cc2af0"),
        rule("zoneZ", "terrain triangle (split NE-SW) + zOffset; collideData.zPos under zoneFlags bit 1",
             "0x140cc2880, 0x140cbc110"),
    };
    std::vector<ManifestSetting> exactRows;
    if (exact)
        exactRows = {
            {"zoneTiling", "types 1-2: pattern irange(0, 24), count irange(1, T), offsets from the 2/3/4 tables, "
                           "no jitter when count > 1",
             kU, R + "transcribed from 0x140cc1900 (tables 0x141e44210 / 0x141e443a0 / 0x141e44600); no collider checks it"},
            {"zoneGraphicsQuality", 2, kU, R + "the tiling count and subModel input (getter 0x140c58c10 not read); ruling S3"},
            {"zoneGroundcoverOffset", "+(24, 24) from the vertex on the tiling path", kU, R + "0x140cc1900; ruling S3"},
            {"zoneUnprovenBranches",
             "clamp to parm rect inset 1, zone matrix = identity, water test (pack 0x1000), random z (0x10 + 0x800), "
             "yaw from normal (0x200), 90Â° snap (0x200000), subModel second instance",
             kU, R + "transcribed; none of the 4750 colliders takes them"},
        };
    const std::vector<ManifestSetting> foliageTail = {
        {"zoneProbability", "applied offline (not re-applied)", kU, F + "D5"},
        {"zoneTint", "hslRanges / hslOffset not applied", kC, F + "D11"},
        {"zoneFade", "fadeRange = camera distance, map units", kU, F + "D6"},
        {"zoneFadeQualityScale", cw::zone_fade_quality_scale(cw::zone_scatter_rule_in_force().graphicsQuality), kU,
         R + "the client's fadeRange x {0.85, 1, 1.5, 2}[quality] at the quality of zoneGraphicsQuality (2); the Map "
             "Window applies it, the layers' fadeRange and the models' fadeFar here are the stored values"},
        {"zoneLayerTypeTarget", "1-2 detail, 3 tree", kC,
         F + "D11, from measured sizes (docs/research/gw2-terrain-to-unity.md \xc2\xa7" "1.3)"},
        {"foliageWind", "none", kC, F + "D10"},
        {"foliagePatchUnits", 1536, kC, F + "D11"},
        {"foliageTileUnits", 1536, kC, F + "D11 (the Map Window's foliage tiles)"},
        {"foliageThinHash", "fmix32(placement)", kC, F + "D9"},
        {"lodCullFovDeg", 60, kC, F + "D11; the VRChat FOV is UNPROVEN"},
    };
    v.insert(v.end(), foliage.begin(), foliage.end());
    v.insert(v.end(), exactRows.begin(), exactRows.end());
    v.insert(v.end(), foliageTail.begin(), foliageTail.end());
    return v;
}

json world_light_json(const castlemist::model::Extractor::MapEnvLight& l) {
    // The env chunk's direction is the one the light travels (map space, up = -Z;
    // every measured day preset has z > 0, Task 13), so it maps straight to a
    // Unity light's forward and a Blender sun's -Z; toward the sun is its negation.
    float uf[3], bd[3], toSun[3];
    cw::map_to_unity(l.sunDir, uf);
    cw::map_to_blender(l.sunDir, bd);
    for (int k = 0; k < 3; ++k) toSun[k] = -l.sunDir[k];
    const float a = std::min(l.fillIntensity, 1.0f);
    const float ambient[3] = {l.fillColor[0] * a, l.fillColor[1] * a, l.fillColor[2] * a};
    return {{"present", l.present},
            {"presetIndex", l.presetIndex},
            {"sun",
             {{"toSun", vec(toSun, 3)},
              {"unityForward", vec(uf, 3)},
              {"blenderDirection", vec(bd, 3)},
              {"color", vec(l.sunColor, 3)},
              {"intensity", l.sunIntensity}}},
            {"fill", {{"color", vec(l.fillColor, 3)}, {"intensity", l.fillIntensity}}},
            {"ambient", vec(ambient, 3)}};
}

json world_manifest_json(const WorldManifestInput& in) {
    static const cw::WorldScene kNoScene;
    static const WorldFolderOptions kDefaults;
    const cw::WorldScene& s = in.scene ? *in.scene : kNoScene;
    std::vector<std::string> notes;   // the writer's own warnings, after the input's

    // Options are numbers the user can set; a non-finite one falls back to its
    // default so every settings value stays finite (ruling W6).
    WorldFolderOptions o = in.opts ? *in.opts : kDefaults;
    auto finite_option = [&](float& v, float def, const char* name) {
        if (std::isfinite(v)) return;
        notes.push_back(std::string("manifest: option ") + name + " is not finite; the default is written");
        v = def;
    };
    finite_option(o.unityScale, kDefaults.unityScale, "unityScale");
    finite_option(o.groundRepeats, kDefaults.groundRepeats, "groundRepeats");
    if (!std::isfinite(o.budget.maxTextureMB)) {
        notes.push_back("manifest: option budget.maxTextureMB is not finite; the default is written");
        o.budget.maxTextureMB = kDefaults.budget.maxTextureMB;
    }

    json man;
    man["format"] = "castlemist-world";
    man["version"] = kWorldManifestVersion;
    man["castlemist"] = CASTLEMIST_VERSION_STRING;
    const bool boundsOk = s.hasBounds && finite_all(s.bounds, 4);
    if (s.hasBounds && !boundsOk) notes.push_back("manifest: non-finite number at map.bounds; bounds written as null");
    man["map"] = {{"fileId", s.mapFileId}, {"bounds", boundsOk ? vec(s.bounds, 4) : json(nullptr)}};

    const auto root = map_to_gltf_root(1);
    man["frame"] = {
        {"space", "gw2-map"}, {"handedness", "left"}, {"east", "+X"}, {"north", "+Y"}, {"up", "-Z"},
        {"toUnity", conversion_rows(&cw::map_to_unity)},
        {"toBlender", conversion_rows(&cw::map_to_blender)},
        {"gltfRoot", vec(root.data(), 16)},
        {"units",
         {{"perMetre", nullptr},
          {"status", "UNPROVEN"},
          {"source", "docs/research/gw2-world-frame.md \xc2\xa7" "1.3"},
          {"unityScale",
           {{"value", o.unityScale}, {"status", "UNPROVEN"}, {"source", "gltf_export.h:57 'GW2 inches'"}}}}}};

    std::vector<std::string> settingWarnings;
    json settings = json::object();
    std::vector<ManifestSetting> settingRows = world_export_settings(o, &s);
    if (in.unityTerrain.is_object()) {   // the terrain rows come with the block (world-foliage spec D11)
        const std::vector<ManifestSetting> ut = unity_terrain_settings();
        settingRows.insert(settingRows.end(), ut.begin(), ut.end());
    }
    for (const ManifestSetting& st : settingRows) {
        assert(first_non_finite(st.value, st.name).empty());   // code constants and the finite options above
        settings[st.name] = {{"value", st.value}, {"status", st.status}, {"source", st.source}};
        if (st.status == "UNPROVEN")
            settingWarnings.push_back("settings: " + st.name + " = " + st.value.dump() + " is UNPROVEN (" +
                                      st.source + ")");
    }
    man["settings"] = settings;
    man["terrain"] = terrain_json(s.terrain, o.region, notes);

    std::unordered_map<uint32_t, size_t> modelPos;   // scene index -> position in models[]
    json models = json::array();
    size_t modelsFailed = 0;
    for (size_t k = 0; k < in.models.size(); ++k) {
        const ManifestModel& m = in.models[k];
        modelPos.emplace(m.index, k);
        if (!m.ok) ++modelsFailed;
        models.push_back(model_json(m, notes));
    }
    man["models"] = models;

    size_t props = 0, written = 0, skippedTrs = 0, skippedModel = 0, skippedCell = 0, chunks = 0,
           collisionInstances = 0;
    bool flatWater = false, waterZWarned = false;
    const bool hasPreset = cw::water_has_preset(s);
    const bool planeZOk = std::isfinite(s.water.planeZ);
    float lo[2] = {0, 0}, hi[2] = {0, 0};   // the kept cells' extent, for the probe
    bool anyRect = false;
    json cells = json::array();
    const bool foliageOn = in.foliage.is_object();
    uint64_t foliageRecords = 0, foliageColliderTrees = 0;
    std::vector<char> propModel(in.models.size(), 0);   // models[] positions a written cell's prop uses
    for (const ManifestCell& mc : in.cells) {
        if (!mc.cell) continue;
        const cw::Cell& c = *mc.cell;
        // Ruling W6: a cell whose rect, origin or budget is not finite is dropped.
        const char* bad = !finite_all(c.rect, 4)        ? "rect"
                          : !finite_all(c.origin, 3)    ? "origin"
                          : !std::isfinite(c.textureMB) ? "budget.textureMB"
                                                        : nullptr;
        if (bad) {
            props += c.props.size();
            skippedCell += c.props.size();
            notes.push_back("cells: " + c.id + " dropped: non-finite number in " + bad);
            continue;
        }
        if (!c.all) {
            chunks += size_t(std::max(0, c.w)) * size_t(std::max(0, c.h));
            if (!anyRect) { lo[0] = c.rect[0]; lo[1] = c.rect[1]; hi[0] = c.rect[2]; hi[1] = c.rect[3]; anyRect = true; }
            lo[0] = std::min(lo[0], c.rect[0]); lo[1] = std::min(lo[1], c.rect[1]);
            hi[0] = std::max(hi[0], c.rect[2]); hi[1] = std::max(hi[1], c.rect[3]);
        }
        if (o.collision) collisionInstances += c.collision.size();
        json water = nullptr;
        const bool flat = mc.water.empty() && s.water.hasPlane && !hasPreset && build_cell_water(s, c, o.waterTexelsPerChunk).vertices() > 0;
        if ((!mc.water.empty() || flat) && !planeZOk) {
            if (!waterZWarned) notes.push_back("manifest: non-finite number at water.z; cell water written as null");
            waterZWarned = true;
        } else if (!mc.water.empty()) {
            water = {{"texture", mc.water}, {"z", s.water.planeZ}};
        } else if (flat) {
            water = {{"texture", nullptr}, {"z", s.water.planeZ}, {"note", kFlatWaterNote}};
            flatWater = true;
        }
        json instances = json::array();
        for (uint32_t pi : c.props) {
            ++props;
            const auto mp = pi < s.props.size() ? modelPos.find(s.props[pi].model) : modelPos.end();
            if (mp != modelPos.end()) propModel[mp->second] = 1;
            if (mp == modelPos.end() || !in.models[mp->second].ok) { ++skippedModel; continue; }
            const cw::PropInstance& p = s.props[pi];
            UnityTrs u{};
            if (!unity_trs(p.world, c.origin, u)) { ++skippedTrs; continue; }   // also every non-finite m
            const std::string clip = in.clipOf ? in.clipOf(pi) : std::string();
            instances.push_back({{"prop", pi},
                                 {"model", mp->second},
                                 {"group", p.group},
                                 {"m", vec(p.world, 16)},
                                 {"unity", {{"t", vec(u.t, 3)}, {"r", vec(u.r, 4)}, {"s", u.s}}},
                                 {"clip", str_or_null(clip)}});
            ++written;
        }
        json foliage = nullptr;
        if (foliageOn && !mc.foliageFile.empty()) {
            foliage = {{"file", mc.foliageFile}, {"records", mc.foliageRecords}, {"colliderTrees", mc.colliderTrees},
                       {"triangles", mc.foliageTriangles}};
            foliageRecords += mc.foliageRecords;
            foliageColliderTrees += mc.colliderTrees;
        }
        cells.push_back(
            {{"id", c.id},
             {"chunks", c.all ? json(nullptr) : json{c.cx0, c.cy0, c.cx0 + c.w - 1, c.cy0 + c.h - 1}},
             {"rect", vec(c.rect, 4)},
             {"origin", vec(c.origin, 3)},
             {"glb", mc.glb},
             {"fbx", nullptr},
             {"albedo", str_or_null(mc.albedo)},
             {"albedoSize", mc.albedo.empty() ? json(nullptr) : json{mc.albedoW, mc.albedoH}},
             {"water", water},
             {"colliderParts", mc.colliderParts},
             {"colliderTriangles", mc.colliderTriangles},
             {"budget", {{"triangles", c.triangles}, {"textureMB", c.textureMB}, {"split", c.split},
                         {"overBudget", c.overBudget}}},
             {"instances", instances},
             {"foliage", foliage}});
    }
    man["cells"] = cells;
    // World foliage (world-foliage spec "manifest.json additions"): the block as given,
    // null when a stored layer value is not finite (ruling W6, warned).
    json foliageBlock = foliageOn ? finite_or_null(in.foliage, "foliage", notes) : json(nullptr);
    if (foliageBlock.is_object()) man["foliage"] = foliageBlock;
    // unityTerrain (world-foliage spec D7): the block as given, left out when not finite (warned).
    const json unityTerrain =
        in.unityTerrain.is_object() ? finite_or_null(in.unityTerrain, "unityTerrain", notes) : json(nullptr);
    if (unityTerrain.is_object()) man["unityTerrain"] = unityTerrain;
    man["water"] = finite_or_null(water_json(s), "water", notes);

    // sky: only the schema's keys (json, default: strings; equirect: string or null).
    json sky = nullptr;
    if (in.sky.is_object()) {
        const json& k = in.sky;
        const bool ok = k.contains("json") && k["json"].is_string() && k.contains("default") && k["default"].is_string() &&
                        (!k.contains("equirect") || k["equirect"].is_string() || k["equirect"].is_null());
        if (ok) {
            sky = {{"json", k["json"]}, {"default", k["default"]}};
            if (k.contains("equirect")) sky["equirect"] = k["equirect"];
        } else {
            notes.push_back("manifest: sky lacks a string json / default; sky written as null");
        }
    }
    man["sky"] = sky;
    // textures (optional, ruling W8): only the schema's keys; a malformed block is left out.
    if (in.textures.is_object()) {
        const json& t = in.textures;
        if (t.contains("dir") && t["dir"].is_string() && t.contains("count") && t["count"].is_number_integer() &&
            t["count"].get<int64_t>() >= 0)
            man["textures"] = {{"dir", t["dir"]}, {"count", t["count"]}};
        else
            notes.push_back("manifest: textures lacks a string dir / count; textures left out");
    }
    man["light"] = finite_or_null(in.light.is_object() ? in.light : world_light_json(s.environment.light), "light", notes);

    // spawn: only map (three finite numbers) and unity = map_to_unity(map), always recomputed.
    json spawn = nullptr;
    float spawnMap[3];
    const bool spawnOk = in.spawn.is_object() && in.spawn.contains("map") && finite_vec3(in.spawn["map"], spawnMap);
    if (spawnOk) {
        float up[3];
        cw::map_to_unity(spawnMap, up);
        spawn = {{"map", vec(spawnMap, 3)}, {"unity", vec(up, 3)}};
    } else if (in.spawn.is_object()) {
        notes.push_back("manifest: spawn.map is not three finite numbers; spawn written as null");
    }
    man["spawn"] = spawn;

    // The probe sits at the spawn, or at the centre of the kept cells (else the bounds) with z 0.
    float pm[3] = {0, 0, 0};
    if (spawnOk) {
        std::copy(spawnMap, spawnMap + 3, pm);
    } else {
        if (!anyRect && boundsOk) { lo[0] = s.bounds[0]; lo[1] = s.bounds[1]; hi[0] = s.bounds[2]; hi[1] = s.bounds[3]; }
        pm[0] = 0.5f * (lo[0] + hi[0]);
        pm[1] = 0.5f * (lo[1] + hi[1]);
    }
    float pu[3];
    cw::map_to_unity(pm, pu);
    man["frameProbe"] = finite_or_null({{"glb", "probe.glb"}, {"fbx", nullptr}, {"map", vec(pm, 3)}, {"unity", vec(pu, 3)},
                                        {"expect", frame_probe_expect()}},
                                       "frameProbe", notes);

    auto given = [&](const char* key) -> uint64_t {
        if (in.counts.is_object() && in.counts.contains(key) && in.counts[key].is_number_integer() &&
            in.counts[key].get<int64_t>() >= 0)
            return in.counts[key].get<uint64_t>();
        return 0;
    };
    man["counts"] = {{"props", props},
                     {"propsWritten", written},
                     {"propsSkipped", props - written},
                     {"models", in.models.size()},
                     {"modelsFailed", modelsFailed},
                     {"collisionInstances", collisionInstances},
                     {"cells", cells.size()},
                     {"chunks", chunks},
                     {"chunksUnbaked", given("chunksUnbaked")},
                     {"animated", given("animated")},
                     {"animatedResolved", given("animatedResolved")},
                     {"clipsFailedDecode", given("clipsFailedDecode")}};
    if (foliageOn) {
        const json gf = in.counts.is_object() && in.counts.contains("foliage") && in.counts["foliage"].is_object()
                            ? in.counts["foliage"] : json::object();
        auto fgiven = [&](const char* key) -> uint64_t {
            if (gf.contains(key) && gf[key].is_number_integer() && gf[key].get<int64_t>() >= 0) return gf[key].get<uint64_t>();
            return 0;
        };
        // modelsFoliageOnly: foliage models that no written prop instance uses.
        uint64_t foliageOnly = 0;
        if (foliageBlock.is_object())
            for (const json& fm : foliageBlock["models"]) {
                const size_t k = fm["model"].get<size_t>();
                if (k < propModel.size() && !propModel[k]) ++foliageOnly;
            }
        man["counts"]["foliage"] = {{"records", foliageRecords},
                                    {"colliderTrees", foliageColliderTrees},
                                    {"scatter", fgiven("scatter")},
                                    {"density", fgiven("density")},
                                    {"modelsFoliageOnly", foliageOnly},
                                    {"skipped", fgiven("skipped")},
                                    {"failedModel", fgiven("failedModel")}};
        if (unityTerrain.is_object()) {
            uint64_t terrainTrees = 0, detailRecords = 0;
            for (const json& t : unityTerrain["tiles"]) {
                if (t["trees"].is_object()) terrainTrees += t["trees"]["records"].get<uint64_t>();
                for (const json& d : t["details"]) detailRecords += d["records"].get<uint64_t>();
            }
            man["counts"]["foliage"]["terrainTrees"] = terrainTrees;
            man["counts"]["foliage"]["detailRecords"] = detailRecords;
        }
    }

    std::vector<std::string> warnings = s.warnings;
    warnings.insert(warnings.end(), in.warnings.begin(), in.warnings.end());
    warnings.insert(warnings.end(), settingWarnings.begin(), settingWarnings.end());
    warnings.insert(warnings.end(), notes.begin(), notes.end());
    if (flatWater) warnings.push_back(kFlatWaterNote);
    if (skippedTrs)
        warnings.push_back("props: " + std::to_string(skippedTrs) +
                           " placements with a non-uniform or degenerate transform skipped");
    if (skippedModel)
        warnings.push_back("props: " + std::to_string(skippedModel) + " placements of failed or missing models skipped");
    if (skippedCell)
        warnings.push_back("props: " + std::to_string(skippedCell) + " placements in dropped cells skipped");
    man["warnings"] = warnings;
    assert(first_non_finite(man, "").empty());
    return man;
}

size_t set_manifest_fbx(const std::string& exportDirUtf8) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(std::u8string(exportDirUtf8.begin(), exportDirUtf8.end()));
    json man;
    {
        std::ifstream f(long_path(dir / "manifest.json"), std::ios::binary);
        if (!f) return 0;
        man = json::parse(f, nullptr, false);
    }
    if (man.is_discarded() || !man.is_object()) return 0;
    size_t set = 0;
    // `glb` is relative to the folder; the .fbx beside it has the same stem.
    auto fill = [&](json& block) {
        if (!block.is_object() || !block.contains("glb") || !block["glb"].is_string()) return;
        const std::string glb = block["glb"].get<std::string>();
        const fs::path rel = fs::path(std::u8string(glb.begin(), glb.end())).replace_extension(".fbx");
        std::error_code ec;
        if (fs::is_regular_file(long_path(dir / rel), ec)) {
            const std::u8string u = rel.generic_u8string();
            block["fbx"] = std::string(u.begin(), u.end());
            ++set;
        } else {
            block["fbx"] = nullptr;
        }
    };
    for (const char* list : {"cells", "models"})
        if (man.contains(list) && man[list].is_array())
            for (json& b : man[list]) fill(b);
    if (man.contains("frameProbe")) fill(man["frameProbe"]);

    const fs::path tmp = dir / "manifest.json.tmp";
    {
        std::ofstream f(long_path(tmp), std::ios::binary);
        f << man.dump(2) << '\n';
        f.close();
        if (f.fail()) {
            std::error_code ec;
            fs::remove(long_path(tmp), ec);
            return 0;
        }
    }
    std::error_code ec;
    fs::rename(long_path(tmp), long_path(dir / "manifest.json"), ec);
    if (ec) {
        fs::remove(long_path(tmp), ec);
        return 0;
    }
    return set;
}

} // namespace castlemist::exportgltf
