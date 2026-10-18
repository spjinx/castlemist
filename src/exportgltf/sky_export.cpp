/// @file
/// @brief Skybox export: dat stage (load_sky_inputs) and pure writing stage
///        (write_skybox, sky.json). Sky maths: docs/research/gw2-sky.md.

#include "castlemist/exportgltf/sky_export.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/dds.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "long_path.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_set>

namespace castlemist::exportgltf::sky {

using castlemist::model::Extractor;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

/// gw2-sky.md §6: stored cube order E, W, N, S, B, T -> Unity face, written
/// as stored (no rotation, no flip).
constexpr Face kCubeFace[6] = {Face::PX, Face::NX, Face::PZ, Face::NZ, Face::NY, Face::PY};
constexpr const char* kCubeName[6] = {"E", "W", "N", "S", "B", "T"};
constexpr Face kFaces[6] = {Face::PX, Face::NX, Face::PY, Face::NY, Face::PZ, Face::NZ};

bool has_content(const Extractor::MapSkyMode& m) { return m.hasPanorama() || m.hasCube(); }

bool any_cube(const Extractor::MapSkyMode& m) {
    return std::any_of(std::begin(m.cube), std::end(m.cube), [](uint32_t f) { return f != 0; });
}

bool same_card_attr(const Extractor::MapSkyCardAttr& a, const Extractor::MapSkyCardAttr& b) {
    return a.texture == b.texture && a.azimuth == b.azimuth && a.latitude == b.latitude &&
           a.density == b.density && a.hazeDensity == b.hazeDensity && a.minHaze == b.minHaze &&
           a.lightIntensity == b.lightIntensity && a.brightness == b.brightness &&
           std::equal(std::begin(a.scale), std::end(a.scale), std::begin(b.scale)) &&
           std::equal(std::begin(a.textureUV), std::end(a.textureUV), std::begin(b.textureUV));
}

/// gw2-sky.md §3: modes 0/2 use the day attribute set (sky params, stars,
/// cards), 1/3 the night set. Two modes from different sets bake alike only
/// when the day and night values the bake reads agree.
bool same_attribute_set(const Extractor::MapSky& s, size_t i, size_t j) {
    if (i % 2 == j % 2) return true;
    const Extractor::MapSkyParams& p = s.params;
    if (p.dayBrightness != p.nightBrightness || p.dayStarDensity != p.nightStarDensity ||
        p.dayHazeBottom != p.nightHazeBottom || p.dayHazeFalloff != p.nightHazeFalloff ||
        p.dayHazeDensity != p.nightHazeDensity || p.dayLightIntensity != p.nightLightIntensity)
        return false;
    return std::all_of(s.cards.begin(), s.cards.end(),
                       [](const Extractor::MapSkyCard& c) { return same_card_attr(c.day, c.night); });
}

/// Mode @p j repeats mode @p i: same textures and the same attribute set.
bool same_mode(const Extractor::MapSky& s, size_t i, size_t j) {
    const Extractor::MapSkyMode &a = s.modes[i], &b = s.modes[j];
    return a.ne == b.ne && a.sw == b.sw && a.top == b.top &&
           std::equal(std::begin(a.cube), std::end(a.cube), std::begin(b.cube)) && same_attribute_set(s, i, j);
}

} // namespace

bool same_sky_mode(const Extractor::MapSky& s, size_t i, size_t j) {
    return i < s.modes.size() && j < s.modes.size() && same_mode(s, i, j);
}

namespace {

json id_or_null(uint32_t id) { return id ? json(id) : json(nullptr); }

json mode_sources(const Extractor::MapSky& s, const Extractor::MapSkyMode& m) {
    json cube = nullptr;
    if (any_cube(m)) {
        cube = json::object();
        for (int k = 0; k < 6; ++k) cube[kCubeName[k]] = id_or_null(m.cube[k]);
    }
    json clouds = json::array();
    for (const auto& l : s.clouds)
        if (l.texture) clouds.push_back(l.texture);
    // Stars and cards are listed as the bake used them (baked_sources).
    return json{{"ne", id_or_null(m.ne)},   {"sw", id_or_null(m.sw)}, {"top", id_or_null(m.top)},
                {"cube", cube},             {"stars", nullptr},       {"clouds", clouds},
                {"cards", json::array()}};
}

/// The star file / atlas and card textures a bake actually used.
void baked_sources(const BakeResult& b, json& sources) {
    if (b.starFile) sources["stars"] = json{{"file", b.starFile}, {"atlas", b.starAtlas}};
    sources["cards"] = b.cardTextures;
}

/// Decompressed bytes of the entry @p fileId names; empty if it has none.
std::vector<uint8_t> file_bytes(Gw2Dat& dat, uint32_t fileId) {
    const uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base > dat.mft_data_list.size()) return {};
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    return e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
}

json sun_json(const Extractor::MapEnvLight& l) {
    float u[3];
    gw2_to_unity(l.sunDir, u);
    return json{{"direction", {u[0], u[1], u[2]}},
                {"color", {l.sunColor[0], l.sunColor[1], l.sunColor[2]}},
                {"intensity", l.sunIntensity}};
}

json unity_slots() {
    json j = json::object();
    for (Face f : kFaces) j[face_file(f)] = face_unity_slot(f);
    return j;
}

void push_unique(std::vector<std::string>& v, const std::string& s) {
    if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
}

bool image_ok(const Image& img) {
    return img.width > 0 && img.height > 0 &&
           img.rgba.size() >= static_cast<size_t>(img.width) * img.height * 4;
}

} // namespace

std::string mode_name(size_t index) {
    // gw2-sky.md §3: mode 0 is day, mode 1 night; 2 and 3 are UNPROVEN.
    if (index == 0) return "day";
    if (index == 1) return "night";
    return "mode" + std::to_string(index);
}

namespace {

std::string fmt3(const float* v, int n) {
    std::string s = "(";
    char b[32];
    for (int i = 0; i < n; ++i) {
        std::snprintf(b, sizeof b, "%s%.4g", i ? ", " : "", v[i]);
        s += b;
    }
    return s + ")";
}

std::string pct(double x) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f%%", x * 100);
    return b;
}

std::string block_name(int zone) { return zone < 0 ? "dataGlobal" : "dataLocalArray[" + std::to_string(zone) + "]"; }

} // namespace

/// The approximate sky of modes @p i and @p j reads the same haze, light and
/// cloud attribute values (day/night slots differ only by mode parity).
bool same_approx_slots(const SkyInputs& in, size_t i, size_t j) {
    if (i % 2 == j % 2) return true;
    const ApproxInputs a = approx_inputs(in, i), b = approx_inputs(in, j);
    auto eq = [](const float* x, const float* y, int n) { return std::equal(x, x + n, y); };
    if (a.haveFog != b.haveFog || a.haveSun != b.haveSun || !eq(a.fogFar, b.fogFar, 4) ||
        !eq(a.sunColor, b.sunColor, 3) || !eq(a.sunDir, b.sunDir, 3) || !eq(&a.sh[0][0], &b.sh[0][0], 12))
        return false;
    for (const auto& L : in.sky.clouds) {
        if (L.attributes.size() < 2) continue;
        const auto &x = L.attributes[0], &y = L.attributes[1];
        if (x.brightness != y.brightness || x.density != y.density || x.haze != y.haze ||
            x.lightIntensity != y.lightIntensity)
            return false;
    }
    return true;
}

EnvBlockChoice choose_env_block(const Extractor::MapSky& s) {
    EnvBlockChoice c;
    const double w = static_cast<double>(s.mapRect[2]) - s.mapRect[0], h = static_cast<double>(s.mapRect[3]) - s.mapRect[1];
    const double area = w > 0 && h > 0 ? w * h : 0;
    if (s.zones.empty() || area <= 0) {
        c.note = s.zones.empty() ? "dataGlobal: the map has no per-zone env blocks"
                                 : "dataGlobal: no parm.rect to weigh the zones against";
        return c;
    }
    double zonesArea = 0;
    int best = -1;
    for (size_t i = 0; i < s.zones.size(); ++i) {
        zonesArea += s.zones[i].area;
        if (best < 0 || s.zones[i].area > s.zones[static_cast<size_t>(best)].area) best = static_cast<int>(i);
    }
    const double globalCover = std::max(0.0, area - zonesArea);
    const double bestCover = s.zones[static_cast<size_t>(best)].area;
    if (bestCover > globalCover) c.zone = best;
    c.note = block_name(c.zone) + ": the block covering most of parm.rect by shape area (dataGlobal " +
             pct(globalCover / area) + ", largest zone " + block_name(best) + " " + pct(bestCover / area) +
             "); the game blends the blocks by camera position, so this is one static stand-in "
             "(UNPROVEN as what a given camera sees)";
    return c;
}

ApproxInputs approx_inputs(const SkyInputs& in, size_t mode) {
    const Extractor::MapSky& s = in.sky;
    ApproxInputs a;
    const size_t slot = mode % 2;   // gw2-sky.md §3: modes 0/2 day (t = 1), 1/3 night (t = 0)
    const char* slotName = slot ? "night" : "day";

    // FogColorFar: haze[slot].farColor of the chosen block (dataGlobal's when
    // the zone has none of its own, as 0x140c60340 falls back to the parent).
    const EnvBlockChoice block = choose_env_block(s);
    a.labels.push_back({"env block", InputSource::Guess, block_name(block.zone), block.note});
    const Extractor::MapHaze* haze = nullptr;
    int hazeBlock = -1;
    if (block.zone >= 0 && s.zones[static_cast<size_t>(block.zone)].haze.size() > slot) {
        haze = &s.zones[static_cast<size_t>(block.zone)].haze[slot];
        hazeBlock = block.zone;
    } else if (s.haze.size() > slot) {
        haze = &s.haze[slot];
    }
    if (haze) {
        a.haveFog = true;
        for (int k = 0; k < 4; ++k) a.fogFar[k] = haze->farColor[k];
        a.haveFogDist = true;
        a.fogNearW = haze->nearColor[3];
        a.fogDist[0] = haze->distRange[0];
        a.fogDist[1] = haze->distRange[1];
        a.labels.push_back({"FogColorFar", InputSource::Derived, fmt3(a.fogFar, 4),
                            block_name(hazeBlock) + " haze[" + std::to_string(slot) + "] (" + slotName +
                                ").farColor, bytes B,G,R,A: haze import 0x140c889e0 -> env slot 0x140c619f0 -> "
                                "per-frame pick 0x140c60340 (slot 0 at t = 1, 1 at t = 0) -> fog set 0x140c64960 "
                                "-> GrSetFog 0x140ab2c20, whose stores follow VSCalculateFog's uniform order "
                                "(the upload to the FogColorFar register not traced)"});
    } else {
        a.labels.push_back({"FogColorFar", InputSource::Guess, "none",
                            "no haze in slot " + std::to_string(slot) + "; haze not baked"});
    }

    // Sun colour / direction and SH: dataGlobal lighting[slot] (the same
    // day/night/alternate slots, 0x140c60690). Their path into EnvContext
    // +0x4e4..0x4f8 and the SH globals is not traced.
    const Extractor::MapEnvLight* light =
        s.lighting.size() > slot && s.lighting[slot].present ? &s.lighting[slot] : nullptr;
    if (light) {
        a.haveSun = true;
        // parseMapEnv already gives R, G, B (the file's B, G, R reordered at the source).
        const float* clr = light->sunColor;
        const float* fill = light->fillColor;
        float len = std::sqrt(light->sunDir[0] * light->sunDir[0] + light->sunDir[1] * light->sunDir[1] +
                              light->sunDir[2] * light->sunDir[2]);
        if (!(len > 0)) len = 1;
        for (int k = 0; k < 3; ++k) {
            a.sunColor[k] = clr[k];
            a.sunDir[k] = light->sunDir[k] / len;
        }
        const float amb = std::clamp(light->fillIntensity * 0.30f, 0.12f, 0.55f);
        for (int k = 0; k < 3; ++k) a.sh[k][3] = fill[k] * amb;
        a.labels.push_back({"sunClr.rgb", InputSource::Guess, fmt3(a.sunColor, 3),
                            std::string("dataGlobal lighting[") + std::to_string(slot) +
                                "] dominant light colour as parseMapEnv reads it (bytes B, G, R: the lighting import "
                                "puts byte 2 in red, Gw2-64.exe 2026-10-14 0x140c8f60e); the writer of "
                                "EnvContext+0x4e4 is not traced"});
        a.labels.push_back({"sunDir", InputSource::Guess, fmt3(a.sunDir, 3),
                            "the same light's direction (the way it travels), as stored"});
        const float shw[3] = {a.sh[0][3], a.sh[1][3], a.sh[2][3]};
        a.labels.push_back({"shRed/shGreen/shBlue", InputSource::Guess, fmt3(shw, 3) + " constant",
                            "fill lights' colour x clamp(0.3 x fill intensity, 0.12, 0.55), castlemist's world-view "
                            "stand-in; the engine's SH globals are not traced"});
    } else {
        a.labels.push_back({"sunClr / sunDir / SH", InputSource::Guess, "none",
                            std::string("no dataGlobal lighting[") + std::to_string(slot) + "]; sun glow not baked"});
    }

    // Camera height for the cloud planes.
    if (in.hasCameraZ) {
        a.cameraZ = in.cameraZ;
        a.labels.push_back({"camera z", InputSource::Guess, fmt3(&a.cameraZ, 1),
                            in.cameraNote.empty() ? "given by the caller" : in.cameraNote});
    } else if (s.hasGroundZ) {
        a.cameraZ = s.groundZ;
        a.labels.push_back({"camera z", InputSource::Guess, fmt3(&a.cameraZ, 1),
                            "the terrain's median height (trn.heightMapArray), a ground-level stand-in camera"});
    } else {
        a.cameraZ = 0;
        a.labels.push_back({"camera z", InputSource::Guess, "0", "no terrain; the map origin"});
    }
    a.labels.push_back({"cardFade, cloudFade", InputSource::Guess, "1",
                        "EnvEnvironment+0xa40 / +0x970 steady values, expected 1 (gw2-sky.md §9.4, §10.3)"});
    a.labels.push_back({"Time", InputSource::Guess, "0", "material cards' Time uniform; 0 = their first frame"});
    a.labels.push_back({"AlphaRef", InputSource::Guess, "0", "material cards' alpha-test reference"});
    if (a.haveFogDist)
        a.labels.push_back({"FogParam0.xy, FogColorNearMinusFar.w", InputSource::Derived,
                            fmt3(a.fogDist, 2) + ", near.w " + fmt3(&a.fogNearW, 1),
                            "the same haze's distRange and nearColor.w: GrSetFog 0x140ab2c20 stores "
                            "(1/(d1-d0), -d0/(d1-d0)) and near - far, which the clouds' VS 1645 reads as "
                            "fz = saturate(viewZ FogParam0.x + FogParam0.y) (store-to-register link not traced)"});
    return a;
}

SkyInputs load_sky_inputs(Gw2Dat& dat, const std::vector<uint8_t>& mapBytes, const json& tpl,
                          uint32_t mapFileId, bool approx) {
    SkyInputs in;
    in.mapFileId = mapFileId;
    try {
        Extractor ex(mapBytes, tpl);
        in.sky = ex.parseMapSky();
        in.daySun = ex.parseMapEnv();   // auto: the brightest preset
    } catch (const std::exception& e) {
        in.sky = {};
        in.decodeWarnings.push_back(std::string("map ") + std::to_string(mapFileId) +
                                    ": env chunk unreadable (" + e.what() + ")");
        return in;
    }
    load_sky_textures(dat, tpl, in, approx);
    return in;
}

void load_sky_textures(Gw2Dat& dat, const json& tpl, SkyInputs& in, bool approx) {
    if (!in.sky.present) return;

    // Every distinct fileId the sky names, in a stable order.
    std::vector<uint32_t> ids;
    std::unordered_set<uint32_t> seen;
    auto add = [&](uint32_t id) {
        if (id && seen.insert(id).second) ids.push_back(id);
    };
    for (const auto& m : in.sky.modes) {
        add(m.ne);
        add(m.sw);
        add(m.top);
        for (uint32_t f : m.cube) add(f);
    }
    for (const auto& l : in.sky.clouds) add(l.texture);
    for (const auto& c : in.sky.cards) {
        add(c.day.texture);
        add(c.night.texture);
        // The approximate sky's flow-fire material card samples these.
        if (approx && c.materialFile == kFlowFireMaterial)
            for (const auto& t : c.materialTextures) add(t.fileId);
    }

    for (uint32_t id : ids) {
        ModelTextureCPU t;
        bool ok = false;
        try {
            ok = decode_texture_rgba(dat, id, t);
        } catch (const std::exception&) {
            ok = false;
        }
        Image img{t.width, t.height, std::move(t.rgba)};
        if (ok && image_ok(img))
            in.textures.emplace(id, std::move(img));
        else
            in.decodeWarnings.push_back("fileId " + std::to_string(id) + ": not a decodable texture");
    }

    // gw2-sky.md §8.2: starFile is a PF packfile (chunk STAR), not a texture.
    // Its atlas is the chunk's own filename -- the reference the game loads
    // (0x140a763f0) -- and a plain DDS the ATEX decoder does not read.
    if (const uint32_t sf = in.sky.starFile) {
        try {
            std::vector<uint8_t> bytes = file_bytes(dat, sf);
            if (!bytes.empty()) in.stars = Extractor(bytes, tpl).parseStars();
        } catch (const std::exception&) {
            in.stars = {};
        }
        if (!in.stars.present) {
            in.decodeWarnings.push_back("fileId " + std::to_string(sf) + ": not a STAR packfile");
        } else if (const uint32_t at = in.stars.atlas) {
            Image img;
            try {
                std::vector<uint8_t> bytes = file_bytes(dat, at);
                uint32_t w = 0, h = 0;
                if (castlemist::dds::decode_rgba8(bytes.data(), bytes.size(), w, h, img.rgba)) {
                    img.width = static_cast<int>(w);
                    img.height = static_cast<int>(h);
                } else {
                    ModelTextureCPU t;   // not a plain DDS: try the texture decoder
                    if (decode_texture_rgba(dat, at, t)) img = Image{t.width, t.height, std::move(t.rgba)};
                }
            } catch (const std::exception&) {
                img = {};
            }
            if (image_ok(img))
                in.textures.emplace(at, std::move(img));
            else
                in.decodeWarnings.push_back("fileId " + std::to_string(at) + ": star atlas not decodable");
        }
    }
}

SkyExportReport write_skybox(const SkyInputs& in, const std::string& parentDir, const std::string& name,
                             const SkyExportOptions& opt) {
    SkyExportReport rep;
    const Extractor::MapSky& s = in.sky;
    if (!s.present || std::none_of(s.modes.begin(), s.modes.end(), has_content)) {
        rep.error = "no sky";
        return rep;
    }

    const fs::path folder = fs::path(parentDir) / name;
    rep.folder = folder.string();
    std::error_code ec;
    remove_tree(folder, ec);   // overwrite: nothing stale survives (remove_all spins past MAX_PATH)
    if (ec) {
        rep.error = "cannot clear " + rep.folder + ": " + ec.message();
        return rep;
    }
    make_dirs(folder, ec);
    if (ec) {
        rep.error = "cannot create " + rep.folder + ": " + ec.message();
        return rep;
    }

    std::vector<std::string> top;   // sky.json top-level warnings
    for (const std::string& w : in.decodeWarnings) push_unique(top, w);
    bool wroteMode23 = false;

    const int eqW = std::max(2, opt.equirectWidth), eqH = std::max(1, eqW / 2);
    const int faceSize = std::max(1, opt.faceSize);

    json modes = json::array();
    std::vector<std::string> modeWarnings;   // every mode's warnings, for the report
    for (size_t j = 0; j < s.modes.size(); ++j) {
        const Extractor::MapSkyMode& m = s.modes[j];
        const std::string mname = mode_name(j);
        json mj = {{"name", mname}, {"aliasOf", nullptr}, {"sources", mode_sources(s, m)},
                   {"skyDistance", s.skyDistance}};
        // "sun" is the env lighting rig's sun light (parseMapEnv), not the sun sky card:
        // the game places cards by their own azimuth/latitude (gw2-sky.md §9.2), so the two need not coincide.
        mj["sun"] = (j == 0 && in.daySun.present) ? sun_json(in.daySun) : json(nullptr);

        if (has_content(m)) {
            size_t alias = j;
            for (size_t i = 0; i < j && alias == j; ++i)
                if (has_content(s.modes[i]) && same_mode(s, i, j) &&
                    (!opt.approximate || same_approx_slots(in, i, j)))
                    alias = i;
            if (alias != j) {
                mj["aliasOf"] = mode_name(alias);
                modes.push_back(std::move(mj));
                continue;
            }
        }

        std::vector<std::string> warns;
        bool skybox = false, baked = false;
        std::vector<std::string> layers;
        const fs::path dir = folder / mname;
        auto write = [&](const Image& img, const fs::path& p) {
            std::string err;
            if (write_png(img, p.string(), err)) return true;
            push_unique(warns, mname + ": " + p.filename().string() + " not written: " + err);
            return false;
        };

        // Raw cube, as stored (gw2-sky.md §6).
        if (m.hasCube()) {
            const Image* faces[6] = {};
            bool all = true;
            for (int k = 0; k < 6; ++k) {
                auto it = in.textures.find(m.cube[k]);
                if (it == in.textures.end() || !image_ok(it->second)) {
                    push_unique(warns, mname + ": cube face " + kCubeName[k] + " (fileId " +
                                           std::to_string(m.cube[k]) +
                                           ") missing or not decoded; skybox not written");
                    all = false;
                } else {
                    faces[k] = &it->second;
                }
            }
            if (all) {
                skybox = true;
                for (int k = 0; k < 6; ++k)
                    skybox = write(*faces[k], dir / "skybox" / (std::string(face_file(kCubeFace[k])) + ".png")) &&
                             skybox;
            }
        } else if (any_cube(m)) {
            push_unique(warns, mname + ": cube sky incomplete (a face is null); skybox not written");
        }

        // Bake: hemicube (gw2-sky.md §2, §5), stars (§8), texture cards (§9).
        if (m.hasPanorama()) {
            ApproxInputs ai;
            if (opt.approximate) ai = approx_inputs(in, j);
            BakeResult b = make_sky_sampler(s, j, in.textures, &in.stars, opt.approximate ? &ai : nullptr);
            if (opt.approximate) {
                json labels = json::array();
                for (const InputLabel& l : ai.labels)
                    labels.push_back({{"input", l.input}, {"source", input_source_name(l.source)},
                                      {"value", l.value}, {"note", l.note}});
                mj["approxInputs"] = labels;
            }
            for (const std::string& w : b.warnings) push_unique(warns, w);
            if (b.ok) {
                baked_sources(b, mj["sources"]);
                baked = write(render_equirect(b.radiance, eqW, eqH), dir / "baked" / "equirect.png");
                for (Face f : kFaces)
                    baked = write(render_face(b.radiance, f, faceSize),
                                  dir / "baked" / (std::string(face_file(f)) + ".png")) &&
                            baked;
                if (baked) layers = b.layers;
            }
        } else if (m.hasCube()) {
            push_unique(warns, mname + ": cube sky only (no NE/SW/T hemicube); nothing baked");
        } else if (m.ne || m.sw || m.top) {
            push_unique(warns, mname + ": hemicube incomplete (NE/SW/T has a null); nothing baked");
        }

        if (skybox || baked) {
            ++rep.modesWritten;
            if (skybox) ++rep.rawSkyboxes;
            if (j >= 2) wroteMode23 = true;
        }
        mj["baked"] = baked;
        mj["layers"] = layers;
        mj["skybox"] = skybox;
        mj["unitySlots"] = unity_slots();
        mj["warnings"] = warns;
        for (const std::string& w : warns) push_unique(modeWarnings, w);
        modes.push_back(std::move(mj));
    }

    if (wroteMode23)
        push_unique(top, "mode2/mode3: the second factor that selects them is UNPROVEN (gw2-sky.md §3)");

    if (opt.approximate)
        push_unique(top, "approximate sky: haze, sun glow, haze-dominated and material cards and clouds are baked "
                         "with the labelled inputs in each mode's approxInputs (gw2-sky.md amendment 2026-10-17)");
    json sky = {{"map", in.mapFileId}, {"envVersion", s.envVersion}, {"approximate", opt.approximate},
                {"modes", modes}, {"warnings", top}};
    {
        std::ofstream f(long_path(folder / "sky.json"), std::ios::binary);
        f << sky.dump(2) << '\n';
        if (!f) push_unique(top, "sky.json not written");
    }

    rep.warnings = top;
    for (const std::string& w : modeWarnings) push_unique(rep.warnings, w);
    rep.ok = rep.modesWritten > 0;
    if (!rep.ok) rep.error = "no sky mode could be written";
    return rep;
}

json report_json(const SkyExportReport& r) {
    return json{{"ok", r.ok},
                {"error", r.error.empty() ? json(nullptr) : json(r.error)},
                {"folder", r.folder},
                {"modesWritten", r.modesWritten},
                {"rawSkyboxes", r.rawSkyboxes},
                {"warnings", r.warnings}};
}

} // namespace castlemist::exportgltf::sky
