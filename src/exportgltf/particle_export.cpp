/// @file
/// @brief Sidecar JSON for a model's baked particle effects.
///
/// glTF has no concept of GW2's billboard emitter system (spawn shapes,
/// atlas sub-rects, flipbooks, curve-driven opacity/scale), so
/// `write_particle_sidecar` writes a companion `<stem>_particles.json` next
/// to the .glb with everything already decoded in
/// `ModelPreview::clouds`/`emitters`/`effectLights` (see
/// castlemist/extract/model_types.h and the gw2-particle-system research
/// note) -- enough to rebuild each emitter in Unity as a mask + scrolling-
/// emission plane the way Poiyomi Pro sets one up: which already-embedded
/// glTF material/texture the emitter's cloud renders with, the atlas
/// `texCoordRect` that picks one sprite out of it, the flipbook grid, the
/// opacity/scale curves, spawn shape/lifetime/color range, bone attach and a
/// `isDistortion` flag for heat-haze/refraction emitters that need a
/// refraction shader path instead of additive emission.
///
/// NOT covered: GW2's real UV pan/scroll speed for the emission and
/// distortion maps. That lives in the game's own DXBC shader code / AMAT
/// uniform constants, which castlemist's material reconstruction does not
/// decode (see gw2mcp-server.md's "Game-DXBC path ... rejected as
/// multi-week"), so scroll speed/direction stays tune-by-eye in Unity same
/// as the rest of this manual workflow.

#include "internal.h"

#include <fstream>

namespace castlemist::exportgltf {

using nlohmann::json;

namespace {

// Mirrors castlemist::render::fxIsDistortion (src/render/particles.cpp),
// duplicated rather than shared: exportgltf has no dependency on the render
// layer (D3D/bgfx), and this heuristic is three lines. GW2 refraction/
// heat-haze emitters store a flat surface normal (~0,0,1 tilting to ~1,0,*)
// as their "colour" and perturb the framebuffer instead of emitting light --
// additive rendering (or an emissive Unity material) draws them as giant
// colored streaks, so a downstream importer needs to know to route these to
// a distortion/refraction path instead.
bool is_distortion_emitter(const ParticleEmitterCPU& e) {
    float r = e.colorBegin[0][0], g = e.colorBegin[0][1], b = e.colorBegin[0][2];
    float er = e.colorEnd[0][0], eg = e.colorEnd[0][1];
    bool beginNormal = (b > 0.8f && r < 0.2f && g < 0.2f);
    bool endNormal = (er > 0.8f && eg < 0.2f);
    return beginNormal && endNormal;
}

json curve_json(const std::vector<std::pair<float, float>>& keys) {
    json arr = json::array();
    for (const auto& [t, v] : keys) arr.push_back({{"t", t}, {"value", v}});
    return arr;
}

std::string bone_name(const ModelPreview& model, int boneJoint) {
    if (boneJoint >= 0 && static_cast<size_t>(boneJoint) < model.joints.size())
        return model.joints[static_cast<size_t>(boneJoint)].name;
    return {};
}

json emitter_json(const ParticleEmitterCPU& e, const ModelPreview& model) {
    json j;
    j["spawnShape"] = e.spawnShape;
    j["spawnPeriodSec"] = e.spawnPeriod;
    j["spawnProbability"] = e.spawnProbability;
    j["spawnGroupSize"] = {e.spawnGroupSize[0], e.spawnGroupSize[1]};
    j["spawnRadius"] = {e.spawnRadius[0], e.spawnRadius[1]};
    j["lifetimeSec"] = {e.lifetime[0], e.lifetime[1]};
    j["drag"] = e.drag;
    j["colorBegin"] = {json::array({e.colorBegin[0][0], e.colorBegin[0][1], e.colorBegin[0][2], e.colorBegin[0][3]}),
                        json::array({e.colorBegin[1][0], e.colorBegin[1][1], e.colorBegin[1][2], e.colorBegin[1][3]})};
    j["colorEnd"] = {json::array({e.colorEnd[0][0], e.colorEnd[0][1], e.colorEnd[0][2], e.colorEnd[0][3]}),
                      json::array({e.colorEnd[1][0], e.colorEnd[1][1], e.colorEnd[1][2], e.colorEnd[1][3]})};
    j["colorPeriodSec"] = e.colorPeriod;
    j["colorFalloff"] = {e.colorFalloff[0], e.colorFalloff[1]};
    j["isDistortion"] = is_distortion_emitter(e);
    j["texCoordRect"] = {e.texCoordRect[0], e.texCoordRect[1], e.texCoordRect[2], e.texCoordRect[3]};
    j["hasPlane"] = e.hasPlane;
    j["hasMesh"] = e.hasMesh;
    if (e.hasMesh) j["meshFileId"] = e.meshFileId;
    j["alignmentType"] = e.alignmentType;
    j["alignmentDir"] = {e.alignmentDir[0], e.alignmentDir[1], e.alignmentDir[2]};
    j["rotationInitial"] = {e.rotationInitial[0], e.rotationInitial[1]};
    j["rotationChange"] = {e.rotationChange[0], e.rotationChange[1]};
    j["scaleInitial"] = {json::array({e.scaleInitial[0][0], e.scaleInitial[0][1]}),
                          json::array({e.scaleInitial[1][0], e.scaleInitial[1][1]})};
    j["scaleChange"] = {json::array({e.scaleChange[0][0], e.scaleChange[0][1]}),
                         json::array({e.scaleChange[1][0], e.scaleChange[1][1]})};
    if (!e.opacityCurve.empty()) j["opacityCurve"] = curve_json(e.opacityCurve);
    else if (e.opacityCurvePreset) j["opacityCurvePreset"] = e.opacityCurvePreset;
    if (!e.scaleCurve.empty()) j["scaleCurve"] = curve_json(e.scaleCurve);
    else if (e.scaleCurvePreset) j["scaleCurvePreset"] = e.scaleCurvePreset;
    if (e.flipbook.present) {
        j["flipbook"] = {{"columns", e.flipbook.columns}, {"rows", e.flipbook.rows},
                          {"count", e.flipbook.count}, {"start", e.flipbook.start}, {"fps", e.flipbook.fps}};
    }
    std::string bone = bone_name(model, e.boneJoint);
    if (!bone.empty()) j["bone"] = bone;
    j["windInfluence"] = e.windInfluence;
    if (e.spawnWindSpeed[0] != 0 || e.spawnWindSpeed[1] != 0)
        j["spawnWindSpeed"] = {e.spawnWindSpeed[0], e.spawnWindSpeed[1]};
    if (e.spawnWindEmit[0] != 0 || e.spawnWindEmit[1] != 0)
        j["spawnWindEmit"] = {e.spawnWindEmit[0], e.spawnWindEmit[1]};
    return j;
}

} // namespace

std::string write_particle_sidecar(const ModelPreview& model, const std::vector<int>& glMaterialIndices,
                                   const std::string& glbPath) {
    if (!model.hasEffects()) return {};

    json doc;
    doc["clouds"] = json::array();
    for (const auto& c : model.clouds) {
        json cj;
        if (c.materialIndex < glMaterialIndices.size() && glMaterialIndices[c.materialIndex] >= 0)
            cj["gltfMaterial"] = glMaterialIndices[c.materialIndex];
        cj["velocity"] = {c.velocity[0], c.velocity[1], c.velocity[2]};
        cj["acceleration"] = {c.acceleration[0], c.acceleration[1], c.acceleration[2]};
        cj["drag"] = c.drag;
        std::string bone = bone_name(model, c.boneJoint);
        if (!bone.empty()) cj["bone"] = bone;
        cj["emitters"] = json::array();
        for (uint32_t ei : c.emitterIndices)
            if (ei < model.emitters.size()) cj["emitters"].push_back(emitter_json(model.emitters[ei], model));
        doc["clouds"].push_back(std::move(cj));
    }

    doc["effectLights"] = json::array();
    for (const auto& l : model.effectLights) {
        json lj{{"color", {l.color[0], l.color[1], l.color[2]}},
                {"intensity", l.intensity},
                {"nearDistance", l.nearDistance},
                {"farDistance", l.farDistance}};
        std::string bone = bone_name(model, l.boneJoint);
        if (!bone.empty()) lj["bone"] = bone;
        else lj["pos"] = {l.pos[0], l.pos[1], l.pos[2]};
        doc["effectLights"].push_back(std::move(lj));
    }

    std::string jsonPath = glbPath.substr(0, glbPath.find_last_of('.')) + "_particles.json";
    std::ofstream out(jsonPath);
    if (!out) return {};
    out << doc.dump(2);
    if (!out) return {};
    return jsonPath;
}

} // namespace castlemist::exportgltf
