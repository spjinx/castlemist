/// @file
/// @brief ModelJoint hierarchy -> glTF nodes + skin, and granny::Anim clips
///        -> glTF animations. No Euler conversion anywhere: glTF rotation is
///        already a plain quaternion, so GW2's own quaternions (bind pose and
///        every sampled keyframe) are copied straight through.

#include "internal.h"

#include "castlemist/native/granny_anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace castlemist::exportgltf {

using nlohmann::json;

namespace {

constexpr int kFloat = 5126;

} // namespace

std::vector<JointExportInfo> write_skeleton(GltfWriter& w, const ModelPreview& model,
                                            const std::string& namePrefix) {
    std::vector<JointExportInfo> out(model.joints.size());

    // Pass 1: create every joint's node (translation/rotation/scale copied
    // straight from GW2's own bind values, no conversion).
    for (size_t i = 0; i < model.joints.size(); ++i) {
        const ModelJoint& j = model.joints[i];
        std::string name = sanitize_name(j.name.empty() ? (namePrefix + "_joint" + std::to_string(i)) : j.name);
        json node{
            {"name", name},
            {"translation", {j.localPos[0], j.localPos[1], j.localPos[2]}},
            {"rotation", {j.localQuat[0], j.localQuat[1], j.localQuat[2], j.localQuat[3]}},
            {"scale", {j.localScale[0], j.localScale[4], j.localScale[8]}},
        };
        out[i].nodeIndex = w.add_node(std::move(node));
    }

    // Pass 2: wire up parent/child links now that every joint's node index is
    // known (works regardless of whether parents precede children in the array).
    for (size_t i = 0; i < model.joints.size(); ++i) {
        int parent = model.joints[i].parent;
        if (parent >= 0) w.add_child(out[static_cast<size_t>(parent)].nodeIndex, out[i].nodeIndex);
    }

    return out;
}

int write_skin(GltfWriter& w, const ModelPreview& model, const std::vector<JointExportInfo>& joints) {
    std::vector<int> jointNodes;
    std::vector<float> inverseBind;
    jointNodes.reserve(model.joints.size());
    inverseBind.reserve(model.joints.size() * 16);

    for (size_t i = 0; i < model.joints.size(); ++i) {
        jointNodes.push_back(joints[i].nodeIndex);
        // glTF wants the inverse bind matrix directly -- GW2's ModelJoint::invWorld
        // already *is* exactly that (the documented model->bone bind matrix the
        // renderer itself uses to skin), so no inversion is needed here at all.
        Mat4 invW;
        for (int k = 0; k < 16; ++k) invW.m[k] = model.joints[i].invWorld[k];
        std::array<float, 16> flat = flatten_column_major(invW);
        inverseBind.insert(inverseBind.end(), flat.begin(), flat.end());
    }

    int ibmAcc = w.add_accessor(inverseBind.data(), inverseBind.size() * sizeof(float), kFloat, "MAT4",
                                model.joints.size(), 0);

    json skin{{"joints", jointNodes}, {"inverseBindMatrices", ibmAcc}};
    return w.add_skin(std::move(skin));
}

void write_animations(GltfWriter& w, const ModelPreview& model, const std::vector<JointExportInfo>& joints, int fps) {
    if (!model.hasAnimation || model.animClips.empty() || joints.empty()) return;
    if (fps < 1) fps = 30;

    for (size_t clipIdx = 0; clipIdx < model.animClips.size(); ++clipIdx) {
        const castlemist::granny::Anim& clip = model.animClips[clipIdx];
        if (!clip.valid || clip.duration <= 0.0f) continue;

        std::unordered_map<std::string, size_t> trackByName;
        for (size_t i = 0; i < clip.tracks.size(); ++i) trackByName[clip.tracks[i].name] = i;

        int frameCount = std::max(1, static_cast<int>(std::floor(static_cast<double>(clip.duration) * fps)) + 1);
        std::vector<float> sampleTimes(static_cast<size_t>(frameCount));
        for (int f = 0; f < frameCount; ++f) {
            sampleTimes[static_cast<size_t>(f)] = std::min(clip.duration, static_cast<float>(f) / static_cast<float>(fps));
        }
        int timesAcc = w.add_accessor(sampleTimes.data(), sampleTimes.size() * sizeof(float), kFloat, "SCALAR",
                                      sampleTimes.size(), 0);

        json channels = json::array();
        json samplers = json::array();

        for (size_t ji = 0; ji < model.joints.size(); ++ji) {
            const ModelJoint& j = model.joints[ji];
            auto found = trackByName.find(j.name);
            if (found == trackByName.end()) continue; // no track: node keeps its static bind-pose T/R/S
            const castlemist::granny::Track& track = clip.tracks[found->second];

            std::vector<float> tvals, rvals, svals;
            tvals.reserve(static_cast<size_t>(frameCount) * 3);
            rvals.reserve(static_cast<size_t>(frameCount) * 4);
            svals.reserve(static_cast<size_t>(frameCount) * 3);

            for (int f = 0; f < frameCount; ++f) {
                float pos[3] = {j.localPos[0], j.localPos[1], j.localPos[2]};
                float quat[4] = {j.localQuat[0], j.localQuat[1], j.localQuat[2], j.localQuat[3]};
                float ss[9]; std::memcpy(ss, j.localScale, sizeof(ss));
                castlemist::granny::sample(track.pos, sampleTimes[static_cast<size_t>(f)], pos, 3);
                castlemist::granny::sample(track.ori, sampleTimes[static_cast<size_t>(f)], quat, 4);
                castlemist::granny::sample(track.sca, sampleTimes[static_cast<size_t>(f)], ss, 9);
                tvals.push_back(pos[0]); tvals.push_back(pos[1]); tvals.push_back(pos[2]);
                rvals.push_back(quat[0]); rvals.push_back(quat[1]); rvals.push_back(quat[2]); rvals.push_back(quat[3]);
                svals.push_back(ss[0]); svals.push_back(ss[4]); svals.push_back(ss[8]);
            }

            auto add_channel = [&](const char* path, const std::vector<float>& values, const char* type) {
                int valAcc = w.add_accessor(values.data(), values.size() * sizeof(float), kFloat, type,
                                            static_cast<size_t>(frameCount), 0);
                samplers.push_back(json{{"input", timesAcc}, {"output", valAcc}, {"interpolation", "LINEAR"}});
                int samplerIdx = static_cast<int>(samplers.size()) - 1;
                channels.push_back(json{{"sampler", samplerIdx},
                                        {"target", {{"node", joints[ji].nodeIndex}, {"path", path}}}});
            };
            add_channel("translation", tvals, "VEC3");
            add_channel("rotation", rvals, "VEC4");
            add_channel("scale", svals, "VEC3");
        }

        if (!channels.empty()) {
            std::string clipName = sanitize_name(clip.name.empty() ? ("Clip_" + std::to_string(clipIdx)) : clip.name);
            w.add_animation(json{{"name", clipName}, {"channels", channels}, {"samplers", samplers}});
        }
    }
}

} // namespace castlemist::exportgltf
