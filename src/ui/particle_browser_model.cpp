/// @file
/// @brief The Particle Browser model (see detail/particle_browser.h).

#include "detail/particle_browser.h"
#include "detail/texture_gallery.h"

#include "castlemist/fx/fx_math.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace castlemist::ui::particles {

namespace {

constexpr char kIndexMagic[4] = {'C', 'M', 'F', 'X'};
constexpr uint32_t kIndexVersion = 1;

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}
std::string pair(const float p[2], const char* unit = "") {
    std::string s = p[0] == p[1] ? num(p[0]) : num(p[0]) + ".." + num(p[1]);
    return *unit ? s + " " + unit : s;
}
std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", v);
    return b;
}

const char* preset_name(uint32_t id) {
    switch (id) {
        case 0: return "constant 1";
        case 1: return "t";
        case 2: case 6: case 7: case 8: return "bump (0 at both ends)";
        case 3: case 4: case 5: return "bump, lifted";
        case 9: return "t^2";
        case 10: return "t^4";
        case 11: return "ease (ends at 1)";
        case 12: case 13: case 14: return "ease, lifted";
        case 15: return "tent^2";
        case 16: return "tent^4";
        case 17: return "tent (1 - |2t - 1|)";
        default: return "outside the client's table: constant 1";
    }
}

const char* alignment_meaning(uint8_t fileType) {
    switch (fx::plane_alignment_case(fileType)) {
        case 0: return "faces alignmentDir (a fixed plane)";
        case 1: return "camera-facing";
        case 2: return "faces its motion";
        case 3: return "faces away from its motion";
        case 4: return "along its on-screen motion";
        case 5: return "axis billboard along its motion";
        default: return "past the client's table";
    }
}

const char* spawn_shape_meaning(uint8_t s) {
    switch (s) {
        case 0: return "disc, radius min..max";
        case 1: return "line along X (max long, min thick)";
        case 2: return "mesh surface";
        case 3: return "flat rectangle (half-sizes max x min)";
        case 4: return "sphere, radius min..max";
        case 5: return "cylinder (ring at min, up to max high)";
        case 6: return "mesh";
        default: return "unknown";
    }
}

}  // namespace

bool save_index(const std::filesystem::path& path, const std::string& fingerprint, uint32_t scannedUpTo,
                const std::vector<ModelRow>& rows) {
    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        f.write(kIndexMagic, 4);
        u32(kIndexVersion);
        u32(static_cast<uint32_t>(fingerprint.size()));
        f.write(fingerprint.data(), static_cast<std::streamsize>(fingerprint.size()));
        u32(scannedUpTo);
        u32(static_cast<uint32_t>(rows.size()));
        for (const ModelRow& r : rows) f.write(reinterpret_cast<const char*>(&r), sizeof r);
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

bool load_index(const std::filesystem::path& path, const std::string& fingerprint, uint32_t& scannedUpTo,
                std::vector<ModelRow>& rows) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    char magic[4];
    uint32_t version = 0, fpLen = 0, upTo = 0, count = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&version), 4);
    f.read(reinterpret_cast<char*>(&fpLen), 4);
    if (!f || std::memcmp(magic, kIndexMagic, 4) != 0 || version != kIndexVersion || fpLen > 4096) return false;
    std::string fp(fpLen, '\0');
    f.read(fp.data(), fpLen);
    f.read(reinterpret_cast<char*>(&upTo), 4);
    f.read(reinterpret_cast<char*>(&count), 4);
    if (!f || fp != fingerprint || count > (1u << 22)) return false;
    std::vector<ModelRow> out(count);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count * sizeof(ModelRow)));
    if (!f) return false;
    scannedUpTo = upTo;
    rows = std::move(out);
    return true;
}

std::vector<size_t> filter_rows(const std::vector<ModelRow>& rows, const ModelFilter& f) {
    gallery::IdQuery q;
    std::string err;
    const bool searching = !f.search.empty();
    if (searching && !gallery::parse_id_query(f.search, q, err)) return {};
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const ModelRow& r = rows[i];
        if (f.streak && !r.streaks) continue;
        if (f.lightning && !r.lightning) continue;
        if (f.mesh && !r.meshEmitters) continue;
        if (f.burst && !r.bursts) continue;
        if (f.tracks && !r.clipTracks) continue;
        if (f.alignCase >= 0 && !((r.alignMask >> f.alignCase) & 1)) continue;
        if (searching && !q.contains(r.fileId)) continue;
        out.push_back(i);
    }
    return out;
}

std::vector<size_t> filter_effects(const std::vector<castlemist::skillfx::EffectRow>& rows, int cls,
                                   const std::string& search) {
    gallery::IdQuery q;
    std::string err;
    const bool byId = !search.empty() && gallery::parse_id_query(search, q, err);
    std::string needle = search;
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::vector<size_t> out;
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        if (cls >= 0 && static_cast<int>(castlemist::skillfx::effect_class_of_kind(r.kind)) != cls) continue;
        if (byId) {
            if (!q.contains(r.uid) && !(r.model && q.contains(r.model))) continue;
        } else if (!needle.empty()) {
            std::string code = r.codename;
            for (char& c : code) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (code.find(needle) == std::string::npos) continue;
        }
        out.push_back(i);
    }
    return out;
}

std::vector<FieldLine> describe_emitter(const ParticleEmitterCPU& e) {
    std::vector<FieldLine> l;
    auto add = [&](const char* g, const char* label, std::string value, std::string meaning = {}) {
        l.push_back({g, label, std::move(value), std::move(meaning)});
    };
    // spawn
    add("spawn", "spawnPeriod", num(e.spawnPeriod) + " s", "a group every period while visible");
    add("spawn", "spawnProbability", num(e.spawnProbability) + " %", "chance each period fires");
    add("spawn", "spawnGroupSize", pair(e.spawnGroupSize), "particles per group");
    add("spawn", "spawnShape", std::to_string(e.spawnShape), spawn_shape_meaning(e.spawnShape));
    add("spawn", "spawnRadius", pair(e.spawnRadius));
    add("spawn", "lifetime", pair(e.lifetime, "s"));
    // motion
    add("motion", "velocity dir", pair(e.velocity[0]) + ", " + pair(e.velocity[1]) + ", " + pair(e.velocity[2]),
        "normalised, then times the speed");
    add("motion", "velocity speed", pair(e.velocity[3]));
    add("motion", "acceleration dir", pair(e.acceleration[0]) + ", " + pair(e.acceleration[1]) + ", " +
        pair(e.acceleration[2]));
    add("motion", "acceleration", pair(e.acceleration[3]), "plus the cloud's; pos += v dt + a dt^2/2");
    add("motion", "drag", num(e.drag), "v *= 1 - (emitter + cloud drag) dt");
    if (e.extra.present) add("motion", "velocityInherit", pair(e.extra.velocityInherit), "of the emitter's own motion");
    // colour
    add("colour", "colorPeriod", num(e.colorPeriod), "life fractions; ping-pongs begin <-> end");
    add("colour", "colorFalloff", pair(e.colorFalloff));
    add("colour", "opacity", e.opacityCurve.empty() ? "preset " + std::to_string(e.opacityCurvePreset)
                                                    : std::to_string(e.opacityCurve.size()) + " keys",
        e.opacityCurve.empty() ? preset_name(e.opacityCurvePreset) : "stored curve, clamped 0..1");
    // size
    add("size", "scaleInitial X", pair(e.scaleInitial[0]), "half-size");
    add("size", "scaleInitial Y", pair(e.scaleInitial[1]), (e.flags & 0x10) ? "unused: Y size = X" : "half-size");
    add("size", "scaleChange", pair(e.scaleChange[0]) + ", " + pair(e.scaleChange[1]), "per second");
    add("size", "scale", e.scaleCurve.empty() ? "preset " + std::to_string(e.scaleCurvePreset)
                                              : std::to_string(e.scaleCurve.size()) + " keys",
        e.scaleCurve.empty() ? preset_name(e.scaleCurvePreset) : "stored curve, clamped 0..1");
    // sprite
    if (e.hasPlane) {
        add("sprite", "alignmentType", std::to_string(e.alignmentType), alignment_meaning(e.alignmentType));
        add("sprite", "rotationInitial", pair(e.rotationInitial, "deg"));
        add("sprite", "rotationChange", pair(e.rotationChange, "deg/s"));
        add("sprite", "rotationDrag", num(e.rotationDrag), "spin slows and stops, never reverses");
        add("sprite", "texCoordRect", num(e.texCoordRect[0]) + ", " + num(e.texCoordRect[1]) + " .. " +
            num(e.texCoordRect[2]) + ", " + num(e.texCoordRect[3]), "u0, v0 .. u1, v1 (v from the bottom)");
    }
    if (e.hasMesh) add("sprite", "mesh", std::to_string(e.meshFileId), "a mesh particle, not a sprite");
    if (e.flipbook.present)
        add("flipbook", "flipbook", std::to_string(e.flipbook.columns) + "x" + std::to_string(e.flipbook.rows) + ", " +
            std::to_string(e.flipbook.count) + " frames from " + std::to_string(e.flipbook.start),
            e.flipbook.fps > 0 ? num(e.flipbook.fps) + " fps, looping" : "spread over the life");
    // wind
    if (e.windInfluence) add("wind", "windInfluence", std::to_string(e.windInfluence), "x 100/255 of the environment wind");
    // flags
    add("flags", "emitterFlags", hex(e.emitterFlags),
        (e.emitterFlags & 0x8) ? "burst: one group when the bone turns visible" : "");
    std::string fm;
    auto note = [&](uint32_t bit, const char* text) {
        if (e.flags & bit) fm += (fm.empty() ? "" : "; ") + std::string(text);
    };
    note(0x4, "no emitter transform at spawn");
    note(0x10, "Y size = X");
    note(0x80, "opacity preset inverted");
    note(0x200, "scale preset inverted");
    note(0x400, "scale preset reversed");
    note(0x1000, "random spin direction");
    add("flags", "flags", hex(e.flags), fm);
    return l;
}

}  // namespace castlemist::ui::particles
