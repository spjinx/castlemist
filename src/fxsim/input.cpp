/// @file
/// @brief SimInput from a ModelPreview (see sim.h).

#include "castlemist/fxsim/sim.h"

namespace castlemist::fxsim {

SimInput make_input(const ModelPreview& model) {
    SimInput in;
    in.clouds = model.clouds;
    auto jointPos = [&](int j, fx::Vec3& out) {
        if (j < 0 || static_cast<size_t>(j) >= model.joints.size()) return false;
        const float* p = model.joints[static_cast<size_t>(j)].pos;
        out = {p[0], p[1], p[2]};
        return true;
    };
    for (size_t ci = 0; ci < model.clouds.size(); ++ci) {
        const ParticleCloudCPU& c = model.clouds[ci];
        for (uint32_t ei : c.emitterIndices) {
            if (ei >= model.emitters.size()) {
                in.warnings.push_back("cloud " + std::to_string(ci) + ": emitter index " + std::to_string(ei) +
                                      " past " + std::to_string(model.emitters.size()) + " emitters");
                continue;
            }
            EmitterIn e;
            e.e = model.emitters[ei];
            e.cloud = static_cast<int>(ci);
            if (!jointPos(e.e.boneJoint, e.origin)) jointPos(c.boneJoint, e.origin);
            in.emitters.push_back(std::move(e));
        }
    }
    return in;
}

}  // namespace castlemist::fxsim
