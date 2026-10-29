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
#include "long_path.h"

#include "castlemist/exportgltf/world_lods.h"
#include "castlemist/native/gw2model.hpp"

#include <filesystem>
#include <fstream>

#include "castlemist/fxsim/sim.h"
#include "castlemist/fxsim/stats.h"

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

json pair_json(const float p[2]) { return json::array({p[0], p[1]}); }
json pairs3_json(const float p[3][2]) { return json::array({pair_json(p[0]), pair_json(p[1]), pair_json(p[2])}); }

json emitter_json(const ParticleEmitterCPU& e, const ModelPreview& model) {
    json j;
    j["spawnShape"] = e.spawnShape;
    j["spawnPeriodSec"] = e.spawnPeriod;
    j["spawnProbability"] = e.spawnProbability;
    j["spawnGroupSize"] = {e.spawnGroupSize[0], e.spawnGroupSize[1]};
    j["spawnRadius"] = {e.spawnRadius[0], e.spawnRadius[1]};
    j["lifetimeSec"] = {e.lifetime[0], e.lifetime[1]};
    j["drag"] = e.drag;
    // velocity / acceleration: (dirX, dirY, dirZ, magnitude), each {base, span} (gw2-particle-system:
    // normalize(dir) * magnitude); transform: the emitter's 3x4 rows, emitter -> cloud bone.
    for (const char* k : {"velocity", "acceleration"}) {
        const float (*v)[2] = k[0] == 'v' ? e.velocity : e.acceleration;
        j[k] = json::array();
        for (int r = 0; r < 4; ++r) j[k].push_back({v[r][0], v[r][1]});
    }
    if (e.hasTransform) j["transform"] = std::vector<float>(e.transform, e.transform + 12);
    j["emitterFlags"] = e.emitterFlags;
    j["flags"] = e.flags;
    j["rotationDrag"] = e.rotationDrag;
    j["colorBegin"] = {json::array({e.colorBegin[0][0], e.colorBegin[0][1], e.colorBegin[0][2], e.colorBegin[0][3]}),
                        json::array({e.colorBegin[1][0], e.colorBegin[1][1], e.colorBegin[1][2], e.colorBegin[1][3]})};
    j["colorEnd"] = {json::array({e.colorEnd[0][0], e.colorEnd[0][1], e.colorEnd[0][2], e.colorEnd[0][3]}),
                      json::array({e.colorEnd[1][0], e.colorEnd[1][1], e.colorEnd[1][2], e.colorEnd[1][3]})};
    // colorPeriod: in fractions of the life (0x140ad3a30), not seconds; offset: world units along
    // the camera ray (0x140ad11b0). docs/research/gw2-particle-system.md, 2026-10-23.
    j["colorPeriod"] = e.colorPeriod;
    j["offset"] = e.offset;
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
    // 2026-10-28: the V70 fields (absent on older emitters, never zero-filled) and the whole
    // mesh settings, every pair [min, max] as stored. docs/superpowers/specs/2026-10-28-effect-decode-design.md.
    const ParticleEmitterExtraCPU& x = e.extra;
    if (x.present) {
        j["velocityInherit"] = pair_json(x.velocityInherit);
        j["velocityDistRange"] = pair_json(x.velocityDistRange);
        j["velocityDistSpeed"] = pair_json(x.velocityDistSpeed);
        j["accelerationDistRange"] = pair_json(x.accelerationDistRange);
        j["accelerationDistSpeed"] = pair_json(x.accelerationDistSpeed);
        j["affinity"] = x.affinity;
    }
    if (x.mesh.present) {
        const MeshEmitterCPU& m = x.mesh;
        j["mesh"] = {{"fileId", m.fileId}, {"alignment", m.alignment}, {"bankThreshold", m.bankThreshold},
                     {"bankScale", m.bankScale}, {"obstacleRadius", m.obstacleRadius},
                     {"rotation", pairs3_json(m.rotation)}, {"scaleInitial", pairs3_json(m.scaleInitial)},
                     {"scaleChange", pairs3_json(m.scaleChange)}, {"spin", pairs3_json(m.spin)},
                     {"rotationDrag", {m.rotationDrag[0], m.rotationDrag[1], m.rotationDrag[2]}}};
    }
    return j;
}

const char* track_target_name(EffectTrackTarget t) {
    switch (t) {
        case EffectTrackTarget::Cloud: return "cloud";
        case EffectTrackTarget::Light: return "light";
        case EffectTrackTarget::Streak: return "streak";
        case EffectTrackTarget::Lightning: return "lightning";
        case EffectTrackTarget::Wind: return "wind";
    }
    return "unknown";
}

} // namespace

std::string write_particle_sidecar(const ModelPreview& model, const std::vector<int>& glMaterialIndices,
                                   const std::string& glbPath) {
    if (!model.hasEffects() && !model.hasMeshVis()) return {};

    json doc;
    doc["clouds"] = json::array();
    for (const auto& c : model.clouds) {
        json cj;
        cj["materialIndex"] = c.materialIndex;   // the model material: materials.json's entry at this index
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
        if (!c.obstacleIndices.empty()) cj["obstacleIndices"] = c.obstacleIndices;
        doc["clouds"].push_back(std::move(cj));
    }

    // Per clip, the visibility keys of the bones the clouds and emitters sit on
    // (gw2model AnimClip::visTracks): GW2 spawns from an emitter while its bone is
    // visible and fires a burst emitter on each hidden -> visible change.
    std::set<std::string> fxBones;
    for (const auto& c : model.clouds) fxBones.insert(bone_name(model, c.boneJoint));
    for (const auto& e : model.emitters) fxBones.insert(bone_name(model, e.boneJoint));
    fxBones.erase(std::string());
    std::map<uint64_t, std::string> byToken;
    for (const std::string& b : fxBones) byToken[castlemist::model::tokenizeBoneName(b)] = b;
    // boneVis (2026-10-25): the same keys for every named bone, effect or not (a model's mesh
    // sets, e.g. 84108's "good" / "demolish", switch with them).
    std::map<uint64_t, std::string> allByToken;
    for (const auto& j : model.joints)
        if (!j.name.empty()) allByToken[castlemist::model::tokenizeBoneName(j.name)] = j.name;
    doc["clips"] = json::array();
    for (size_t i = 0; i < model.animClips.size(); ++i) {
        json cj{{"name", model.animClips[i].name}, {"duration", model.animClips[i].duration}};
        json vis = json::object(), boneVis = json::object();
        if (i < model.animClipVis.size())
            for (const auto& [tok, keys] : model.animClipVis[i]) {
                const auto it = byToken.find(tok);
                if (it != byToken.end()) vis[it->second] = keys;
                const auto all = allByToken.find(tok);
                if (all != allByToken.end()) boneVis[all->second] = keys;
            }
        cj["vis"] = vis;
        cj["boneVis"] = boneVis;
        if (i < model.animClipEffectTracks.size() && !model.animClipEffectTracks[i].empty()) {
            json tracks = json::array();
            for (const auto& t : model.animClipEffectTracks[i]) {
                json tj{{"target", track_target_name(t.target)}, {"type", t.type}, {"trackGroup", t.trackGroup},
                        {"vectorTrack", t.vectorTrack},
                        {"initial", {t.initial[0], t.initial[1], t.initial[2], t.initial[3]}}};
                const std::string bone = bone_name(model, t.boneJoint);
                if (!bone.empty()) tj["bone"] = bone;
                tracks.push_back(std::move(tj));
            }
            cj["effectTracks"] = std::move(tracks);
        }
        doc["clips"].push_back(std::move(cj));
    }

    // meshVis (2026-10-25): each mesh node (mesh_node_name; its "_LOD<k>" nodes share it) whose
    // visBone is a named bone, with that bone: clips[].boneVis then show and hide the mesh.
    const std::string glbStem = std::filesystem::path(glbPath).stem().string();
    doc["meshVis"] = json::object();
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        const auto it = allByToken.find(model.meshes[i].visBone);
        if (model.meshes[i].visBone && it != allByToken.end()) doc["meshVis"][mesh_node_name(model, i, glbStem)] = it->second;
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

    // 2026-10-28: streaks, obstacles and effect winds, fields as stored (colours packed).
    auto bone_or_skip = [&](json& j, int joint) {
        const std::string b = bone_name(model, joint);
        if (!b.empty()) j["bone"] = b;
    };
    doc["streaks"] = json::array();
    for (const auto& st : model.streaks) {
        json sj{{"materialIndex", st.materialIndex}, {"anchorIndices", st.anchorIndices}, {"flags", st.flags},
                {"velocity", {st.velocity[0], st.velocity[1], st.velocity[2]}},
                {"acceleration", {st.acceleration[0], st.acceleration[1], st.acceleration[2]}},
                {"jitter", st.jitter}, {"noise", st.noise}, {"spawnDist", st.spawnDist},
                {"texScale", st.texScale}, {"wind", st.wind}};
        if (st.materialIndex < glMaterialIndices.size() && glMaterialIndices[st.materialIndex] >= 0)
            sj["gltfMaterial"] = glMaterialIndices[st.materialIndex];
        bone_or_skip(sj, st.boneJoint);
        doc["streaks"].push_back(std::move(sj));
    }
    doc["streakAnchors"] = json::array();
    for (const auto& an : model.streakAnchors) {
        json aj{{"colorStart", an.colorStart}, {"colorEnd", an.colorEnd}, {"falloff", an.falloff},
                {"lifetime", an.lifetime}, {"flags", an.flags}, {"texV", an.texV}};
        bone_or_skip(aj, an.boneJoint);
        doc["streakAnchors"].push_back(std::move(aj));
    }
    doc["obstacles"] = json::array();
    for (const auto& ob : model.obstacles) {
        json oj{{"type", ob.type}, {"response", ob.response}, {"flags", ob.flags}, {"affinity", ob.affinity},
                {"dragCoef", ob.dragCoef}, {"gravityCoef", ob.gravityCoef}, {"length", ob.length},
                {"width", ob.width}, {"height", ob.height}, {"radius", ob.radius}};
        if (ob.hasTransform) oj["transform"] = std::vector<float>(ob.transform, ob.transform + 12);
        bone_or_skip(oj, ob.boneJoint);
        doc["obstacles"].push_back(std::move(oj));
    }
    doc["effectWinds"] = json::array();
    for (const auto& w : model.effectWinds) {
        json wj{{"intensity", w.intensity}, {"nearDistance", w.nearDistance}, {"farDistance", w.farDistance}};
        bone_or_skip(wj, w.boneJoint);
        doc["effectWinds"].push_back(std::move(wj));
    }

    // castlemist's reference statistics for the same effects (sub-project D: a Unity port is
    // measured against these): fxsim, seed 1, 4 s at 1/60 s.
    if (model.hasEffects()) {
        fxsim::Sim sim(fxsim::make_input(model), 1u, true);
        for (int i = 0; i < 240; ++i) sim.step(1.0f / 60.0f);
        json stats = fxsim::stats_json(sim.stats());
        stats["seed"] = 1;
        stats["dt"] = 1.0 / 60.0;
        stats["seconds"] = 4;
        stats["approximations"] = sim.approximations();
        const std::string statsPath = glbPath.substr(0, glbPath.find_last_of('.')) + "_fxstats.json";
        std::ofstream sout(long_path(std::filesystem::path(std::u8string(statsPath.begin(), statsPath.end()))));
        if (sout) sout << stats.dump(1);
    }

    std::string jsonPath = glbPath.substr(0, glbPath.find_last_of('.')) + "_particles.json";
    std::ofstream out(long_path(std::filesystem::path(std::u8string(jsonPath.begin(), jsonPath.end()))));  // UTF-8
    if (!out) return {};
    out << doc.dump(2);
    if (!out) return {};
    return jsonPath;
}

} // namespace castlemist::exportgltf
