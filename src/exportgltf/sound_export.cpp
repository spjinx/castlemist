/// @file
/// @brief sound_sidecar_json / write_model_sounds: a model's AMSP sound script as
///        `<stem>_sounds.json` and its sound files (see sound_export.h).
#include "castlemist/exportgltf/sound_export.h"

#include "castlemist/native/gw2_audio.hpp"
#include "castlemist/world/anim.h"

#include "internal.h"
#include "long_path.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace castlemist::exportgltf {

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace cm = castlemist::model;

namespace {

std::string tok(uint64_t t) { return std::to_string(t); }

bool write_bytes(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream out(long_path(p), std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    out.close();   // the final buffered flush can fail (disk full): check after it
    return !out.fail();
}

json envelope_json(const cm::SoundEnvelope& e) {
    json pts = json::array();
    for (const auto& [o, v] : e.points) pts.push_back({o, v});
    return {{"inputType", e.inputType}, {"inputScale", e.inputScale}, {"inputOffset", e.inputOffset},
            {"outputScale", e.outputScale}, {"outputOffset", e.outputOffset}, {"points", pts}};
}

json param_json(const cm::SoundParam& p) {
    return {{"value", p.value}, {"type", p.type},
            {"random", p.type == 1 ? json{p.randomMin, p.randomMax} : json(nullptr)},
            {"envelope", p.hasEnvelope ? envelope_json(p.envelope) : json(nullptr)}};
}

} // namespace

json sound_sidecar_json(const ModelPreview& model, const cm::SoundScript& script, uint32_t scriptId,
                        const std::map<uint32_t, std::string>& audioPaths) {
    json sounds = json::array();
    for (const cm::MetaSound& m : script.sounds) {
        json files = json::array();
        for (const cm::SoundFile& f : m.files) {
            const auto it = audioPaths.find(f.fileId);
            files.push_back({{"fileId", f.fileId}, {"path", it != audioPaths.end() ? json(it->second) : json(nullptr)},
                             {"weight", f.weight}, {"volume", f.volume}, {"language", tok(f.language)}});
        }
        sounds.push_back({{"id", tok(m.name)}, {"name", cm::detokenizeName64(m.name)},
                          {"category", cm::detokenizeName64(m.category)}, {"files", files},
                          {"volume", param_json(m.volume)}, {"pitch", param_json(m.pitch)},
                          {"rolloff", m.hasAttenuation && m.attenuationVolume.hasEnvelope
                                          ? envelope_json(m.attenuationVolume.envelope) : json(nullptr)},
                          {"fadeIn", m.fadeIn}, {"fadeOut", m.fadeOut},
                          {"playLength", {m.playLength[0], m.playLength[1]}},
                          {"replayDelay", {m.replayDelay[0], m.replayDelay[1]}},
                          {"initialDelay", {m.initialDelay[0], m.initialDelay[1]}},
                          {"loopMode", m.loopMode}, {"loopCount", m.loopCount}, {"playbackMode", m.playbackMode},
                          {"fileIterateMode", m.fileIterateMode}, {"flags", m.flags}});
    }
    json triggers = json::array();
    for (const cm::SoundTrigger& t : script.triggers) {
        const int c = castlemist::world::resolve_prop_clip(model.animClipTokens, t.name);
        json markers = json::array();
        for (const cm::SoundMarker& mk : t.markers) markers.push_back({{"sound", tok(mk.cue)}, {"time", mk.time}, {"type", mk.type}});
        triggers.push_back({{"id", tok(t.name)}, {"name", cm::detokenizeName64(t.name)},
                            {"clip", c >= 0 && static_cast<size_t>(c) < model.animClips.size()
                                         ? json(model.animClips[static_cast<size_t>(c)].name) : json(nullptr)},
                            {"markers", markers}});
    }
    json handlers = json::array();
    for (uint64_t h : script.handlers) handlers.push_back(cm::detokenizeName64(h));
    return {{"script", scriptId}, {"volume", script.volume}, {"sounds", sounds}, {"triggers", triggers}, {"handlers", handlers}};
}

std::string write_model_sounds(const ModelPreview& model, const std::function<std::vector<uint8_t>(uint32_t)>& readFile,
                               const json& tpl, const std::string& folderUtf8, const std::string& stem,
                               std::vector<std::string>& warnings) {
    if (!model.soundScript) return {};
    const std::vector<uint8_t> bytes = readFile(model.soundScript);
    cm::SoundScript script;
    try {
        if (!bytes.empty()) script = cm::Extractor(bytes, tpl).parseSoundScript();
    } catch (const std::exception& e) {
        warnings.push_back("sound script " + std::to_string(model.soundScript) + ": " + e.what());
        return {};
    }
    if (!script.present) {
        warnings.push_back("sound script " + std::to_string(model.soundScript) + " did not read");
        return {};
    }
    if (script.sounds.empty()) return {};
    const fs::path folder = fs::u8path(folderUtf8);
    std::map<uint32_t, std::string> paths;
    std::set<uint32_t> seen;
    for (const cm::MetaSound& m : script.sounds)
        for (const cm::SoundFile& f : m.files) {
            if (!seen.insert(f.fileId).second) continue;
            const std::vector<uint8_t> a = readFile(f.fileId);
            const std::vector<castlemist::audio::Clip> clips = a.empty() ? std::vector<castlemist::audio::Clip>()
                                                                         : castlemist::audio::extract(a.data(), a.size());
            const castlemist::audio::Clip* c = nullptr;
            for (const auto& k : clips)
                if (!k.data.empty() && k.codec != castlemist::audio::CODEC_UNKNOWN) { c = &k; break; }
            if (!c) {
                warnings.push_back("sound file " + std::to_string(f.fileId) + " has no MP3 / Ogg / WAV audio");
                continue;
            }
            const char* ext = c->codec == castlemist::audio::CODEC_MP3 ? ".mp3"
                            : c->codec == castlemist::audio::CODEC_OGG ? ".ogg"
                            : c->codec == castlemist::audio::CODEC_WAV ? ".wav" : ".flac";
            const std::string rel = "sounds/" + std::to_string(f.fileId) + ext;
            std::error_code ec;
            make_dirs(folder / "sounds", ec);
            if (ec || !write_bytes(folder / "sounds" / (std::to_string(f.fileId) + ext), c->data)) {
                warnings.push_back("cannot write " + rel);
                continue;
            }
            paths[f.fileId] = rel;
        }
    const std::string name = stem + "_sounds.json";
    const std::string text = sound_sidecar_json(model, script, model.soundScript, paths).dump(1);
    if (!write_bytes(folder / name, std::vector<uint8_t>(text.begin(), text.end()))) {
        warnings.push_back("cannot write " + name);
        return {};
    }
    return name;
}

} // namespace castlemist::exportgltf
