/// @file
/// @brief write_vrchat_folder: one model -> `<Name>.glb`, Poiyomi-named PNGs
///        and materials.json (spec "Output folder" and "4. materials.json").

#include "castlemist/exportgltf/vrchat_export.h"

#include "internal.h"

#include "castlemist/exportgltf/vrchat_maps.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CASTLEMIST_VERSION_STRING
#define CASTLEMIST_VERSION_STRING "unknown"
#endif

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
// Insertion order, so materials.json reads in the spec's field order.
using json = nlohmann::ordered_json;

namespace {

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// `base`, or `base_2`, `base_3`, ... -- unique among `taken` ignoring ASCII
/// case (Windows file names are case-insensitive). Records the result.
std::string unique_name(const std::string& base, std::set<std::string>& taken) {
    std::string name = base;
    for (int n = 2; taken.count(lower_ascii(name)); ++n) name = base + "_" + std::to_string(n);
    taken.insert(lower_ascii(name));
    return name;
}

std::optional<float> named_constant(const ModelMaterialCPU& mat, const char* name) {
    for (const auto& [n, v] : mat.namedConstants)
        if (n == name) return v;
    return std::nullopt;
}

json opt_json(const std::optional<float>& v) { return v ? json(*v) : json(); }

json rgb_json(const std::array<float, 3>& c) { return {c[0], c[1], c[2]}; }

bool write_file(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/// Writes one material's maps under `Textures/` and builds their JSON.
class MapWriter {
public:
    MapWriter(const fs::path& folder, std::string matFile, std::vector<std::string>& warnings)
        : folder_(folder), matFile_(std::move(matFile)), warnings_(warnings) {}

    /// The map's JSON ({file, uv, fileId, source}), or null when it is not
    /// present or its PNG could not be written (with a warning).
    json write(const MapSlot& slot, const std::string& suffix) {
        if (!slot.present) return json();
        const std::string leaf = matFile_ + " - " + unique_name(suffix, suffixes_) + ".png";
        const std::string rel = "Textures/" + leaf;
        const std::vector<uint8_t> png = encode_png(slot.tex);
        if (png.empty() || !write_file(folder_ / "Textures" / u8path(leaf), png)) {
            warnings_.push_back(suffix + " map (" + slot.source + ") could not be written to " +
                                rel + ": left out");
            return json();
        }
        return json{{"file", rel},
                    {"uv", slot.uv},
                    {"fileId", slot.fileId ? json(slot.fileId) : json()},
                    {"source", slot.source}};
    }

private:
    fs::path folder_;
    std::string matFile_;
    std::vector<std::string>& warnings_;
    std::set<std::string> suffixes_;
};

json blend_json(const ModelMaterialCPU& mat, const BlendInfo& b) {
    if (!mat.hasRenderState) return json();  // no game blend word: nothing raw to record
    return json{{"srcRgb", b.srcRgb}, {"dstRgb", b.dstRgb}, {"srcA", b.srcA}, {"dstA", b.dstA},
                {"op", b.eqRgb},      {"eqRgb", b.eqRgb},   {"eqA", b.eqA}};
}

json panning_json(const ModelMaterialCPU& mat) {
    const auto u = named_constant(mat, "intscru");
    const auto v = named_constant(mat, "intscrv");
    if (!u && !v) return json();
    return json{{"u", opt_json(u)}, {"v", opt_json(v)}, {"unit", "gw2-raw"}};
}

}  // namespace

std::string safe_file_name(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || u == 0x7F || std::string_view("/\\:*?\"<>|").find(c) !=
                                                     std::string_view::npos;
        out.push_back(bad ? '_' : c);
    }
    const auto keep = [](char c) { return c != ' ' && c != '.'; };
    const auto first = std::find_if(out.begin(), out.end(), keep);
    const auto last = std::find_if(out.rbegin(), out.rend(), keep).base();
    out = first < last ? std::string(first, last) : std::string();
    if (out.empty()) return "_";

    // CON, PRN, AUX, NUL, COM1-9, LPT1-9 are devices on Windows, with any extension.
    const std::string stem = lower_ascii(out.substr(0, out.find('.')));
    static const char* const kReserved[] = {"con", "prn", "aux", "nul"};
    bool reserved = std::find(std::begin(kReserved), std::end(kReserved), stem) != std::end(kReserved);
    if (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
        stem[3] >= '1' && stem[3] <= '9')
        reserved = true;
    if (reserved) out += "_";
    return out;
}

VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                       const std::string& name, uint32_t modelFileId) {
    VrchatFolderResult r;
    if (model.meshes.empty()) {
        r.error = "This model has no drawable meshes to export.";
        return r;
    }

    const fs::path folder = u8path(folderUtf8);
    std::error_code ec;
    fs::create_directories(folder / "Textures", ec);
    if (ec) {
        r.error = "Could not create '" + utf8(folder / "Textures") + "': " + ec.message();
        return r;
    }
    r.folder = utf8(folder);
    const std::string modelName = safe_file_name(name);

    // Per-material triangle use: materials no mesh draws with (most blended
    // weapon materials draw the particle effects) stay in the JSON, unused.
    std::vector<size_t> triangles(model.materials.size(), 0);
    for (const ModelMeshCPU& m : model.meshes)
        if (m.materialIndex < triangles.size()) triangles[m.materialIndex] += m.indices.size() / 3;

    std::set<std::string> takenNames;
    json materials = json::array();
    for (size_t i = 0; i < model.materials.size(); ++i) {
        const ModelMaterialCPU& mat = model.materials[i];
        const std::string matName = material_name(mat);
        const std::string matFile = unique_name(safe_file_name(matName), takenNames);
        const MaterialShading shading = material_shading(mat);
        const ShaderProfile& profile = *shading.profile;
        const BlendInfo& blend = shading.blend;
        MaterialMaps maps = build_material_maps(model, mat, blend, profile);

        std::vector<std::string> warnings = std::move(maps.warnings);
        if (!mat.hasRenderState)
            warnings.push_back(std::string("no game shader: preset ") +
                               poiyomi_preset_name(blend.preset) +
                               " from the effect flag, blend factors unknown");
        else if (!blend.exact)
            warnings.push_back(std::string("blend is not an exact Poiyomi preset: nearest '") +
                               poiyomi_preset_name(blend.nearest) +
                               "', use the raw factors in \"blend\" (Advanced Blending)");

        MapWriter mw(folder, matFile, warnings);
        json jm{{"baseColor", mw.write(maps.baseColor, "BaseColor")},
                {"normal", mw.write(maps.normal, "Normal")}};
        json packed = mw.write(maps.packed, "Packed");
        if (!packed.is_null())
            packed["sources"] = {{"metal", maps.metalSource},
                                 {"smooth", maps.smoothSource},
                                 {"reflection", maps.reflectionSource},
                                 {"specular", maps.specularSource}};
        jm["packed"] = std::move(packed);
        json emissionMap = mw.write(maps.emissionMap, "EmissionMap");
        if (!emissionMap.is_null()) emissionMap["panning"] = panning_json(mat);
        jm["emissionMap"] = std::move(emissionMap);
        jm["emissionMask"] = mw.write(maps.emissionMask, "EmissionMask");
        jm["emissionBaked"] = mw.write(maps.emissionBaked, "Emission");
        json distortion = mw.write(maps.distortion, "Distortion");
        if (!distortion.is_null()) distortion["strength"] = opt_json(named_constant(mat, "gloptrb"));
        jm["distortion"] = std::move(distortion);
        json decal = mw.write(maps.decal, "Decal");
        if (!decal.is_null()) decal["mode"] = maps.decalMode;
        jm["decal"] = std::move(decal);
        json decalMask = mw.write(maps.decalMask, "DecalMask");
        if (!decalMask.is_null()) decalMask["channel"] = maps.decalMaskChannel;
        jm["decalMask"] = std::move(decalMask);
        json extras = json::array();
        for (const MaterialMaps::Extra& x : maps.extras) {
            json e = mw.write(x.slot, safe_file_name(x.role.empty() ? "extra" : x.role));
            if (e.is_null()) continue;
            e["role"] = x.role;
            e["use"] = x.use.empty() ? json() : json(x.use);
            extras.push_back(std::move(e));
        }
        jm["extras"] = std::move(extras);

        const bool hasEmission = maps.emissionMap.present || maps.emissionMask.present ||
                                 maps.emissionBaked.present;
        json gw2 = json::object();
        for (const auto& [n, v] : mat.namedConstants) gw2[n] = v;

        json m{
            {"name", matName},
            {"fileName", matFile},
            {"amat", mat.materialFile ? json(mat.materialFile) : json()},
            {"profile", profile.name},
            {"usedByMeshes", triangles[i] > 0},
            {"renderPreset", poiyomi_preset_name(blend.preset)},
            {"preset", poiyomi_preset_name(blend.nearest)},
            {"mode", poiyomi_mode_value(blend)},
            {"exact", blend.exact},
            {"blend", blend_json(mat, blend)},
            {"alphaCutoff", kAlphaCutoff},
            // Only clipping shaders were disassembled to this threshold.
            {"alphaCutoffIsDefault", !profile.clips},
            {"renderQueueOffset", mat.sortLayer},  // draw order from the material's sort layer
            {"sortOrder", mat.sortOrder},           // raw
            {"cull", mat.isEffect ? "Off" : "Back"},
            {"maps", std::move(jm)},
            {"emission", hasEmission ? json{{"color", rgb_json(maps.emissionColor)},
                                                  {"strength", maps.emissionStrength}}
                                     : json()},
            {"specularTint", maps.specularTint ? rgb_json(*maps.specularTint) : json()},
            {"reflectionTint", maps.reflectionTint ? rgb_json(*maps.reflectionTint) : json()},
            {"gw2", std::move(gw2)},
            {"warnings", warnings},
        };
        materials.push_back(std::move(m));
        for (const std::string& w : warnings) r.warnings.push_back(matName + ": " + w);
    }

    const fs::path glb = folder / u8path(modelName + ".glb");
    const GltfExportResult g = export_model_gltf(model, utf8(glb));
    if (!g.ok) {
        r.error = g.error;
        return r;
    }
    r.glb = utf8(glb);
    r.particlesJson = g.particlesJsonPath;

    json animations = json::array();
    for (const auto& clip : model.animClips) animations.push_back(clip.name);

    const json doc{
        {"castlemist", CASTLEMIST_VERSION_STRING},
        {"model", modelFileId},
        {"poiyomi", "10"},
        {"materials", std::move(materials)},
        {"animations", std::move(animations)},
        {"particles", g.particlesJsonPath.empty() ? json() : json(utf8(u8path(g.particlesJsonPath).filename()))},
    };
    const fs::path jsonPath = folder / "materials.json";
    const std::string text = doc.dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    if (!write_file(jsonPath, std::vector<uint8_t>(text.begin(), text.end()))) {
        r.error = "Could not write '" + utf8(jsonPath) + "'.";
        return r;
    }
    r.materialsJson = utf8(jsonPath);
    r.materials = model.materials.size();
    r.clips = model.animClips.size();
    r.ok = true;
    return r;
}

}  // namespace castlemist::exportgltf
