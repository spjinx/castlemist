/// @file
/// @brief write_vrchat_folder: one model -> `<Name>.glb`, Poiyomi-named PNGs
///        and materials.json (spec "Output folder" and "4. materials.json").

#include "castlemist/exportgltf/vrchat_export.h"

#include "internal.h"
#include "long_path.h"

#include "castlemist/exportgltf/vrchat_maps.h"

#include <algorithm>
#include <cctype>
#include <cstring>
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

namespace {

// A material constant's token32 name: GW2's base-23 Token (docs/research/gw2-uniform-hash.md).
std::string constant_token_name(uint32_t token) {
    static const char kAlpha[] = "abcdefghiklmnopvrstuwxy";
    std::string s;
    for (uint32_t v = token - 0x30000000u; v; v /= 23) s += kAlpha[v % 23];
    return s;
}

// The constants clip `c` animates on material `mat` (materials.json "constAnims"): each one's
// value sampled at kConstAnimFps over the clip, or its initial value alone when it has no track
// (the clip holds it). Keyed by the clip's glTF animation name (skeleton_anim_export.cpp).
constexpr float kConstAnimFps = 30.0f;
nlohmann::json const_anims_json(const ModelPreview& model, const ModelMaterialCPU& mat) {
    nlohmann::json out = nlohmann::json::array();
    for (size_t c = 0; c < model.animClips.size() && c < model.animClipConsts.size(); ++c) {
        const castlemist::granny::Anim& clip = model.animClips[c];
        const std::string clipName = sanitize_name(clip.name.empty() ? ("Clip_" + std::to_string(c)) : clip.name);
        for (const ModelMatConstAnimCPU& k : model.animClipConsts[c]) {
            if (k.materialId != mat.materialId) continue;
            const int dim = k.hasTrack && k.curve.dim > 0 ? std::min(k.curve.dim, 4) : 4;
            nlohmann::json times = nlohmann::json::array(), values = nlohmann::json::array();
            const int n = k.hasTrack ? std::max(2, static_cast<int>(std::ceil(clip.duration * kConstAnimFps)) + 1) : 1;
            for (int i = 0; i < n; ++i) {
                const float t = n > 1 ? clip.duration * static_cast<float>(i) / static_cast<float>(n - 1) : 0.0f;
                float v[16] = {k.initial[0], k.initial[1], k.initial[2], k.initial[3]};
                if (k.hasTrack) castlemist::granny::sample(k.curve, t, v, dim);
                times.push_back(t);
                values.push_back({v[0], v[1], v[2], v[3]});
            }
            out.push_back({{"clip", clipName},
                           {"duration", clip.duration},
                           {"constant", constant_token_name(k.constToken)},
                           {"initial", {k.initial[0], k.initial[1], k.initial[2], k.initial[3]}},
                           {"animated", k.hasTrack},
                           {"times", std::move(times)},
                           {"values", std::move(values)}});
        }
    }
    return out;
}

}  // namespace
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
    std::ofstream out(long_path(p), std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();   // the final flush can fail: check after it
    return !out.fail();
}

}  // namespace

std::vector<uint8_t> complete_dds_cube_mips(const std::vector<uint8_t>& dds) {
    // DDS_HEADER (after "DDS "): flags @8, height @12, width @16, mipMapCount @28,
    // fourCC @84, caps2 @112; data from 128. GW2's cubes stop at 4 x 4 (512: 8 of
    // 10 levels), which Unity's DDS importer refuses ("Invalid mipCount").
    auto u32 = [&dds](size_t o) {
        uint32_t v = 0;
        std::memcpy(&v, dds.data() + o, 4);
        return v;
    };
    if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4) != 0 || !(u32(112) & 0x200u)) return dds;
    const uint32_t h = u32(12), w = u32(16), have = std::max<uint32_t>(1, u32(28));
    const std::string fourcc(reinterpret_cast<const char*>(dds.data()) + 84, 4);
    const size_t block = fourcc == "DXT1" ? 8 : (fourcc == "DXT3" || fourcc == "DXT5") ? 16 : 0;
    if (block == 0 || w == 0 || h == 0 || have == 1) return dds;
    uint32_t full = 1;
    while ((std::max(w, h) >> full) > 0) ++full;
    if (have >= full) return dds;
    auto level_bytes = [&](uint32_t l) {
        const size_t lw = std::max<uint32_t>(1, w >> l), lh = std::max<uint32_t>(1, h >> l);
        return ((lw + 3) / 4) * ((lh + 3) / 4) * block;
    };
    size_t face = 0;
    for (uint32_t l = 0; l < have; ++l) face += level_bytes(l);
    if (dds.size() < 128 + 6 * face) return dds;
    std::vector<uint8_t> out(dds.begin(), dds.begin() + 128);
    const uint32_t mips = full;
    std::memcpy(out.data() + 28, &mips, 4);
    for (int f = 0; f < 6; ++f) {
        const uint8_t* src = dds.data() + 128 + static_cast<size_t>(f) * face;
        out.insert(out.end(), src, src + face);
        // The missing levels (2 x 2, 1 x 1: one block each) repeat the smallest block.
        const uint8_t* last = src + face - level_bytes(have - 1);
        for (uint32_t l = have; l < full; ++l) out.insert(out.end(), last, last + level_bytes(l));
    }
    return out;
}

namespace {

/// The GW2 cloth that uses material @p i (ModelPreview::clothPieces), for the castlemist shaders'
/// wave: {pieces, wind, gravity, drag, rigidness} of the windiest piece, or null without one.
json cloth_json(const ModelPreview& model, size_t i) {
    const ClothPieceCPU* best = nullptr;
    int pieces = 0;
    for (const ClothPieceCPU& cp : model.clothPieces) {
        if (cp.materialIndex != i || cp.lockCount == 0) continue;
        ++pieces;
        if (!best || cp.wind > best->wind) best = &cp;
    }
    if (!best) return json();
    return json{{"pieces", pieces}, {"wind", best->wind}, {"gravity", best->gravity},
                {"drag", best->drag}, {"rigidness", best->rigidness}};
}

/// Writes one material's maps under `Textures/` and builds their JSON.
class MapWriter {
public:
    MapWriter(const fs::path& folder, std::string matFile, std::vector<std::string>& warnings,
              SharedImagePool* pool = nullptr, std::string poolPrefix = {})
        : folder_(folder), matFile_(std::move(matFile)), warnings_(warnings), pool_(pool),
          poolPrefix_(std::move(poolPrefix)) {}

    /// The map's JSON ({file, uv, fileId, source}), or null when it is not
    /// present or its PNG could not be written (with a warning). With a pool
    /// (Task 15d) the file is the pool's "<sha1>_<suffix>.png", shared by every
    /// map of the export with the same bytes and role; an extra layer's
    /// (@p extra) is "<sha1>_x-<lower-case role>.png", so it never meets a
    /// Poiyomi role's name ("projector" and "Projector" are one file on Windows).
    json write(const MapSlot& slot, const std::string& suffix, bool extra = false) {
        if (!slot.present) return json();
        const std::vector<uint8_t> png = encode_png(slot.tex);
        std::string rel;
        if (pool_) {
            std::string err;
            const std::string leaf = pool_->ensure_content(png, extra ? "_x-" + lower_ascii(suffix) : "_" + suffix, &err);
            if (leaf.empty()) {
                warnings_.push_back(suffix + " map (" + slot.source + ") could not be written to " + poolPrefix_ +
                                    ": left out" + (err.empty() ? "" : " (" + err + ")"));
                return json();
            }
            rel = poolPrefix_ + leaf;
        } else {
            const std::string leaf = matFile_ + " - " + unique_name(suffix, suffixes_) + ".png";
            rel = "Textures/" + leaf;
            if (png.empty() || !write_file(folder_ / "Textures" / u8path(leaf), png)) {
                warnings_.push_back(suffix + " map (" + slot.source + ") could not be written to " +
                                    rel + ": left out");
                return json();
            }
        }
        return json{{"file", rel},
                    {"uv", slot.uv},
                    {"fileId", slot.fileId ? json(slot.fileId) : json()},
                    {"source", slot.source}};
    }

    /// A DDS cubemap layer, written as it is stored ("<sha1>_x-<role>.dds" in the
    /// pool, else "Textures/<Mat> - <role>.dds"): {file, uv, fileId, role}, or null.
    json write_cube(const std::vector<uint8_t>& dds, const ModelMaterialCPU::ExtraTexture& t) {
        const std::string role = safe_file_name(t.role.empty() ? "cube" : t.role);
        std::string rel;
        if (pool_) {
            std::string err;
            const std::string leaf = pool_->ensure_content(dds, "_x-" + lower_ascii(role), &err, ".dds");
            if (leaf.empty()) {
                warnings_.push_back(role + " cubemap (fileId " + std::to_string(t.fileId) + ") could not be written to " +
                                    poolPrefix_ + ": left out" + (err.empty() ? "" : " (" + err + ")"));
                return json();
            }
            rel = poolPrefix_ + leaf;
        } else {
            const std::string leaf = matFile_ + " - " + unique_name(role, suffixes_) + ".dds";
            rel = "Textures/" + leaf;
            if (!write_file(folder_ / "Textures" / u8path(leaf), dds)) {
                warnings_.push_back(role + " cubemap (fileId " + std::to_string(t.fileId) + ") could not be written to " +
                                    rel + ": left out");
                return json();
            }
        }
        return json{{"file", rel}, {"uv", t.uvIndex}, {"fileId", t.fileId}, {"role", t.role}};
    }

private:
    fs::path folder_;
    std::string matFile_;
    std::vector<std::string>& warnings_;
    std::set<std::string> suffixes_;
    SharedImagePool* pool_;
    std::string poolPrefix_;
};

/// Task 15d: the Poiyomi side of a material's key -- its materials.json entry
/// without the display fields (name, fileName), the per-model ones
/// (usedByMeshes, warnings) and, from the world export, `world` (dark decal,
/// uvScroll). Re-parsed as nlohmann::json, so the keys are sorted.
std::string key_input(const json& entry, const nlohmann::json& world) {
    nlohmann::json j = nlohmann::json::parse(entry.dump(-1, ' ', false, json::error_handler_t::replace));
    for (const char* k : {"name", "fileName", "usedByMeshes", "warnings"}) j.erase(k);
    j["world"] = world;
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

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
    return write_vrchat_folder(model, folderUtf8, name, modelFileId, VrchatFolderOptions{});
}

VrchatFolderResult write_vrchat_folder(const ModelPreview& model, const std::string& folderUtf8,
                                       const std::string& name, uint32_t modelFileId,
                                       const VrchatFolderOptions& opts) {
    VrchatFolderResult r;
    if (model.meshes.empty() && !model.hasEffects()) {
        r.error = "This model has no drawable meshes to export.";
        return r;
    }

    const fs::path folder = u8path(folderUtf8);
    std::error_code ec;
    if (opts.generatedMaps) make_dirs(folder, ec);   // the maps go to the pool (Task 15d)
    else make_dirs(folder / "Textures", ec);
    if (ec) {
        r.error = "Could not create '" + utf8(opts.generatedMaps ? folder : folder / "Textures") + "': " + ec.message();
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

        MapWriter mw(folder, matFile, warnings, opts.generatedMaps.get(), opts.generatedMapsPrefix);
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
        json alphaMask = mw.write(maps.alphaMask, "AlphaMask");
        // An opacity mask has no cutoff: null (not -1), the schema's "not applicable".
        if (!alphaMask.is_null())
            alphaMask["cutoff"] = maps.alphaMaskCutoff >= 0.0f ? json(maps.alphaMaskCutoff) : json();
        jm["alphaMask"] = std::move(alphaMask);
        json extras = json::array();
        for (const MaterialMaps::Extra& x : maps.extras) {
            json e = mw.write(x.slot, safe_file_name(x.role.empty() ? "extra" : x.role), true);
            if (e.is_null()) continue;
            e["role"] = x.role;
            e["use"] = x.use.empty() ? json() : json(x.use);
            extras.push_back(std::move(e));
        }
        jm["extras"] = std::move(extras);
        json cubemaps = json::array();
        for (const ModelMaterialCPU::ExtraTexture& t : mat.cubeTextures) {
            auto it = model.cubeMaps.find(t.fileId);
            if (it == model.cubeMaps.end()) continue;
            json c = mw.write_cube(complete_dds_cube_mips(it->second), t);
            if (!c.is_null()) cubemaps.push_back(std::move(c));
        }
        jm["cubemaps"] = std::move(cubemaps);
        json constants = json::array();
        for (const MaterialMaps::LayerConstant& c : maps.layerConstants)
            constants.push_back({{"role", c.role}, {"uv", c.uv}, {"rgba", {c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]}}});
        jm["layerConstants"] = std::move(constants);

        json rim;
        if (maps.rim.present) {
            const std::string ch = maps.rim.maskSource.substr(maps.rim.maskSource.find('.') + 1);
            json mask;
            if (maps.rim.mask.present) {
                json f = mw.write(maps.rim.mask, "RimMask");
                if (!f.is_null())
                    mask = json{{"file", f["file"]}, {"constant", json()}, {"uv", f["uv"]},
                                {"channel", ch}};
            } else if (maps.rim.maskConstant) {
                mask = json{{"file", json()}, {"constant", *maps.rim.maskConstant},
                            {"uv", json()}, {"channel", ch}};
            }
            rim = json{{"color", rgb_json(maps.rim.color)},
                       {"colorSpace", "sRGB"},
                       {"mask", std::move(mask)},
                       {"scroll", maps.rim.scroll ? json(*maps.rim.scroll) : json()}};
        }
        json projector;
        if (maps.projector.present || maps.projectorConstant) {
            json f = mw.write(maps.projector, "Projector");
            const json falloff = maps.projectorFalloff
                                     ? json::array({(*maps.projectorFalloff)[0], (*maps.projectorFalloff)[1]})
                                     : json();
            if (!f.is_null()) {
                projector = json{{"file", f["file"]}, {"uv", f["uv"]}, {"fileId", f["fileId"]},
                                 {"falloff", falloff}, {"coverage", "saturate(2a)"}};
            } else if (maps.projectorConstant) {
                const auto& c = *maps.projectorConstant;
                projector = json{{"file", json()}, {"uv", json()}, {"fileId", json()},
                                 {"constant", json::array({c[0], c[1], c[2], c[3]})},
                                 {"falloff", falloff}, {"coverage", "saturate(2a)"}};
            }
        }
        const bool hasEmission = maps.emissionMap.present || maps.emissionMask.present ||
                                 maps.emissionBaked.present;
        json gw2 = json::object();
        for (const auto& [n, v] : mat.namedConstants) gw2[n] = v;
        // The whole float4 of each constant (a shader may read .y-.w: 1507908's stepcol.w).
        json gw2Vectors = json::object();
        for (const auto& [n, v] : mat.namedConstantVectors) gw2Vectors[n] = {v[0], v[1], v[2], v[3]};
        // MODL texTransforms as stored: what the VS's TexTransform<uv> animates per UV set
        // (scroll in UV per settings.uvScrollUnitSeconds, rotate read as radians per second;
        // both UNPROVEN, gw2-world-materials.md).
        json texTransforms = json::array();
        for (const ModelTexTransformCPU& t : mat.texTransforms)
            texTransforms.push_back({{"uv", t.uvIndex},
                                     {"scroll", {t.scroll[0], t.scroll[1]}},
                                     {"scrollFreq", {t.scrollFreq[0], t.scrollFreq[1]}},
                                     {"scale", {t.scale[0], t.scale[1]}},
                                     {"scaleFreq", {t.scaleFreq[0], t.scaleFreq[1]}},
                                     {"rotate", t.rotate},
                                     {"flipbook", {{"columns", t.columns}, {"rows", t.rows},
                                                   {"count", t.count}, {"fps", t.fps}}},
                                     {"flags", t.flags}});

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
            // Only shaders clipping on the diffuse alpha were disassembled to this
            // threshold; a cutout layer's 0.5 lives in maps.alphaMask.cutoff.
            {"alphaCutoffIsDefault", !profile.clips},
            {"renderQueueOffset", mat.sortLayer},  // draw order from the material's sort layer
            {"sortOrder", mat.sortOrder},           // raw
            {"cull", mat.isEffect ? "Off" : "Back"},
            {"unlit", profile.unlit},  // the game applies no lighting (fog only)
            {"maps", std::move(jm)},
            {"emission", hasEmission ? json{{"color", rgb_json(maps.emissionColor)},
                                                  {"colorSpace", "sRGB"},
                                                  {"strength", maps.emissionStrength}}
                                     : json()},
            {"rim", std::move(rim)},
            {"projector", std::move(projector)},
            {"specularTint", maps.specularTint ? rgb_json(*maps.specularTint) : json()},
            {"reflectionTint", maps.reflectionTint ? rgb_json(*maps.reflectionTint) : json()},
            {"gw2", std::move(gw2)},
            {"gw2Vectors", std::move(gw2Vectors)},
            {"texTransforms", std::move(texTransforms)},
            // GW2 animates some constants per clip (ModelAnimationData.matConstAnim).
            {"constAnims", const_anims_json(model, mat)},
            {"cloth", cloth_json(model, i)},
            {"warnings", warnings},
        };
        materials.push_back(std::move(m));
        for (const std::string& w : warnings) r.warnings.push_back(matName + ": " + w);
    }

    const fs::path glb = folder / u8path(modelName + ".glb");
    GltfExportOptions gopts = opts.gltf;
    if (opts.materialKeys) {
        gopts.materialKeyInputs.clear();
        for (size_t i = 0; i < materials.size(); ++i)
            gopts.materialKeyInputs.push_back(
                key_input(materials[i], i < opts.materialKeyExtras.size() ? opts.materialKeyExtras[i] : nlohmann::json()));
    }
    const GltfExportResult g = export_model_gltf(model, utf8(glb), gopts);
    if (!g.ok) {
        r.error = g.error;
        return r;
    }
    if (opts.materialKeys && g.materialKeys.size() == materials.size())
        for (size_t i = 0; i < materials.size(); ++i) {
            materials[i]["key"] = g.materialKeys[i];
            r.materialKeys.push_back({g.materialKeys[i], materials[i]["name"].get<std::string>(),
                                      materials[i]["usedByMeshes"].get<bool>()});
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
