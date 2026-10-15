/// @file
/// @brief The world export's manifest.json v1 (world_manifest.h; spec
///        docs/superpowers/specs/2026-10-15-world-export-design.md
///        "manifest.json (version 1)", D3, D5, D7).

#include "castlemist/exportgltf/world_manifest.h"

#include "castlemist/exportgltf/world_cell.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/world/frame.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain_bake.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

/// Replaces every non-finite number under @p v with null, one warning per path.
void scrub_non_finite(json& v, const std::string& path, std::vector<std::string>& warnings) {
    if (v.is_number_float()) {
        if (!std::isfinite(v.get<double>())) {
            v = nullptr;
            warnings.push_back("manifest: non-finite number at " + path + " written as null");
        }
    } else if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it)
            scrub_non_finite(it.value(), path.empty() ? it.key() : path + "." + it.key(), warnings);
    } else if (v.is_array()) {
        for (size_t k = 0; k < v.size(); ++k) scrub_non_finite(v[k], path + "[" + std::to_string(k) + "]", warnings);
    }
}

json model_json(const ManifestModel& m) {
    json uv = json::array();
    if (m.uvScroll.is_array())
        for (json e : m.uvScroll) {
            if (e.is_object() && !e.contains("unit")) e["unit"] = "per second, UNPROVEN";
            uv.push_back(std::move(e));
        }
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
              {"uvScroll", uv},
              {"status", m.ok ? "ok" : "failed"}};
    if (!m.ok || !m.error.empty()) j["error"] = m.error;
    return j;
}

json terrain_json(const cw::Terrain& t, const cw::ChunkRegion& r) {
    if (!t.present || t.chunks.empty()) return {{"present", false}};
    const cw::TerrainChunk& k = t.chunks.front();
    json region = r.whole() ? json{0, 0, t.chunksX - 1, t.chunksY - 1} : json{r.cx0, r.cy0, r.cx1, r.cy1};
    return {{"present", true},
            {"chunksX", t.chunksX},
            {"chunksY", t.chunksY},
            {"chunkSize", {k.rect[2] - k.rect[0], k.rect[3] - k.rect[1]}},
            {"segments", std::max(0, k.samples - 1)},
            {"region", region}};
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
    for (int k = 0; k < 3; ++k) local[12 + k] -= cellOrigin[k];
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

std::vector<ManifestSetting> world_export_settings(const WorldFolderOptions& o) {
    const std::string kU = "UNPROVEN", kC = "castlemist's choice", kM = "measured";
    return {
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
        {"cellMaxTriangles", o.budget.maxTriangles, kC, "spec D1; VRChat states no per-area limit"},
        {"cellMaxTextureMB", o.budget.maxTextureMB, kC, "spec D1; VRChat states no per-area limit"},
        {"ambient", "fillColor * min(fillIntensity, 1)", kC, "spec D5 (raw intensities, flat ambient)"},
        {"unityAxisConversion", "ModelImporter.bakeAxisConversion = true: east +X, north +Z, up +Y", kM,
         "frame probe on Unity 6000.0.42f1 (world export Task 1); Unity 2022.3 not run"},
    };
}

json world_light_json(const castlemist::model::Extractor::MapEnvLight& l) {
    float uf[3], bd[3];
    cw::map_to_unity(l.sunDir, uf);
    cw::map_to_blender(l.sunDir, bd);
    for (int k = 0; k < 3; ++k) { uf[k] = -uf[k]; bd[k] = -bd[k]; }
    const float a = std::min(l.fillIntensity, 1.0f);
    const float ambient[3] = {l.fillColor[0] * a, l.fillColor[1] * a, l.fillColor[2] * a};
    return {{"present", l.present},
            {"presetIndex", l.presetIndex},
            {"sun",
             {{"toSun", vec(l.sunDir, 3)},
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
    const WorldFolderOptions& o = in.opts ? *in.opts : kDefaults;

    json man;
    man["format"] = "castlemist-world";
    man["version"] = kWorldManifestVersion;
    man["castlemist"] = CASTLEMIST_VERSION_STRING;
    man["map"] = {{"fileId", s.mapFileId}, {"bounds", s.hasBounds ? vec(s.bounds, 4) : json(nullptr)}};

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
    for (const ManifestSetting& st : world_export_settings(o)) {
        settings[st.name] = {{"value", st.value}, {"status", st.status}, {"source", st.source}};
        if (st.status == "UNPROVEN")
            settingWarnings.push_back("settings: " + st.name + " = " + st.value.dump() + " is UNPROVEN (" +
                                      st.source + ")");
    }
    man["settings"] = settings;
    man["terrain"] = terrain_json(s.terrain, o.region);

    std::unordered_map<uint32_t, size_t> modelPos;   // scene index -> position in models[]
    json models = json::array();
    size_t modelsFailed = 0;
    for (size_t k = 0; k < in.models.size(); ++k) {
        const ManifestModel& m = in.models[k];
        modelPos.emplace(m.index, k);
        if (!m.ok) ++modelsFailed;
        models.push_back(model_json(m));
    }
    man["models"] = models;

    size_t props = 0, written = 0, skippedTrs = 0, skippedModel = 0, chunks = 0, collisionInstances = 0;
    bool flatWater = false;
    const bool hasPreset = cw::water_has_preset(s);
    json cells = json::array();
    for (const ManifestCell& mc : in.cells) {
        if (!mc.cell) continue;
        const cw::Cell& c = *mc.cell;
        if (!c.all) chunks += size_t(std::max(0, c.w)) * size_t(std::max(0, c.h));
        if (o.collision) collisionInstances += c.collision.size();
        json water = nullptr;
        if (!mc.water.empty()) {
            water = {{"texture", mc.water}, {"z", s.water.planeZ}};
        } else if (s.water.hasPlane && !hasPreset && build_cell_water(s, c).vertices() > 0) {
            water = {{"texture", nullptr}, {"z", s.water.planeZ}, {"note", kFlatWaterNote}};
            flatWater = true;
        }
        json instances = json::array();
        for (uint32_t pi : c.props) {
            ++props;
            const auto mp = pi < s.props.size() ? modelPos.find(s.props[pi].model) : modelPos.end();
            if (mp == modelPos.end() || !in.models[mp->second].ok) { ++skippedModel; continue; }
            const cw::PropInstance& p = s.props[pi];
            UnityTrs u{};
            if (!unity_trs(p.world, c.origin, u)) { ++skippedTrs; continue; }
            const std::string clip = in.clipOf ? in.clipOf(pi) : std::string();
            instances.push_back({{"prop", pi},
                                 {"model", mp->second},
                                 {"group", p.group},
                                 {"m", vec(p.world, 16)},
                                 {"unity", {{"t", vec(u.t, 3)}, {"r", vec(u.r, 4)}, {"s", u.s}}},
                                 {"clip", str_or_null(clip)}});
            ++written;
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
             {"instances", instances}});
    }
    man["cells"] = cells;
    man["water"] = water_json(s);
    man["sky"] = in.sky.is_object() ? in.sky : json(nullptr);
    man["light"] = in.light.is_object() ? in.light : world_light_json(s.environment.light);

    json spawn = nullptr;
    if (in.spawn.is_object()) {
        spawn = in.spawn;
        if (!spawn.contains("unity") && spawn.contains("map") && spawn["map"].is_array() && spawn["map"].size() == 3) {
            float mp[3], up[3];
            for (size_t k = 0; k < 3; ++k) mp[k] = spawn["map"][k].is_number() ? spawn["map"][k].get<float>() : NAN;
            cw::map_to_unity(mp, up);
            spawn["unity"] = vec(up, 3);
        }
    }
    man["spawn"] = spawn;

    // The probe sits at the spawn, or at the centre of the exported cells with z 0.
    json probeMap;
    if (spawn.is_object() && spawn.contains("map")) {
        probeMap = spawn["map"];
    } else {
        float lo[2] = {0, 0}, hi[2] = {0, 0};
        bool any = false;
        for (const ManifestCell& mc : in.cells) {
            if (!mc.cell || mc.cell->all) continue;
            const float* r = mc.cell->rect;
            if (!any) { lo[0] = r[0]; lo[1] = r[1]; hi[0] = r[2]; hi[1] = r[3]; any = true; }
            lo[0] = std::min(lo[0], r[0]); lo[1] = std::min(lo[1], r[1]);
            hi[0] = std::max(hi[0], r[2]); hi[1] = std::max(hi[1], r[3]);
        }
        if (!any && s.hasBounds) { lo[0] = s.bounds[0]; lo[1] = s.bounds[1]; hi[0] = s.bounds[2]; hi[1] = s.bounds[3]; }
        probeMap = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.0f};
    }
    float pm[3] = {NAN, NAN, NAN}, pu[3];
    if (probeMap.is_array() && probeMap.size() == 3)
        for (size_t k = 0; k < 3; ++k) pm[k] = probeMap[k].is_number() ? probeMap[k].get<float>() : NAN;
    cw::map_to_unity(pm, pu);
    man["frameProbe"] = {{"glb", "probe.glb"}, {"fbx", nullptr}, {"map", probeMap}, {"unity", vec(pu, 3)},
                         {"expect", frame_probe_expect()}};

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

    std::vector<std::string> warnings = s.warnings;
    warnings.insert(warnings.end(), in.warnings.begin(), in.warnings.end());
    warnings.insert(warnings.end(), settingWarnings.begin(), settingWarnings.end());
    if (flatWater) warnings.push_back(kFlatWaterNote);
    if (skippedTrs)
        warnings.push_back("props: " + std::to_string(skippedTrs) +
                           " placements with a non-uniform or degenerate transform skipped");
    if (skippedModel)
        warnings.push_back("props: " + std::to_string(skippedModel) + " placements of failed or missing models skipped");
    scrub_non_finite(man, "", warnings);
    man["warnings"] = warnings;
    return man;
}

} // namespace castlemist::exportgltf
