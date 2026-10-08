/// @file
/// @brief Skybox export: dat stage (load_sky_inputs) and pure writing stage
///        (write_skybox, sky.json). Sky maths: docs/research/gw2-sky.md.

#include "castlemist/exportgltf/sky_export.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/dds.h"
#include "castlemist/native/cmp_decompress_method0.hpp"

#include <algorithm>
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

SkyInputs load_sky_inputs(Gw2Dat& dat, const std::vector<uint8_t>& mapBytes, const json& tpl,
                          uint32_t mapFileId) {
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
    if (!in.sky.present) return in;

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
    return in;
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
    fs::remove_all(folder, ec);   // overwrite: nothing stale survives
    if (ec) {
        rep.error = "cannot clear " + rep.folder + ": " + ec.message();
        return rep;
    }
    fs::create_directories(folder, ec);
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
                if (has_content(s.modes[i]) && same_mode(s, i, j)) alias = i;
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
            BakeResult b = make_sky_sampler(s, j, in.textures, &in.stars);
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

    json sky = {{"map", in.mapFileId}, {"envVersion", s.envVersion}, {"modes", modes}, {"warnings", top}};
    {
        std::ofstream f(folder / "sky.json", std::ios::binary);
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
