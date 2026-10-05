#include "castlemist/ripper/face_morphs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace castlemist::ripper {
namespace {

using Mat = std::array<double, 16>;  // row-major, row vectors: p' = p * M

Mat identity() {
    Mat m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}

Mat mul(const Mat& a, const Mat& b) {  // a first, then b
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

Mat translate(double x, double y, double z) {
    Mat m = identity();
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

Mat scale(double x, double y, double z) {
    Mat m{};
    m[0] = x; m[5] = y; m[10] = z; m[15] = 1;
    return m;
}

Mat rotate_x(double degrees) {  // positive moves a point in front (-Y) upward (-Z)
    const double a = degrees * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
    Mat m = identity();
    // row vector: y' = y*c - z*s, z' = y*s + z*c
    m[5] = c; m[6] = s;
    m[9] = -s; m[10] = c;
    return m;
}

// One bone's part in a slider, at the slider's "+" end. Model axes: X = the
// character's left, -Y = forward (the face), -Z = up. `out` moves away from the
// face's centre line (mirrored for left / right bones).
struct Edit {
    const char* joint;
    std::array<double, 3> scale{1, 1, 1};
    double rot_x = 0;                     // degrees, about the bone's pivot
    std::array<double, 3> move{0, 0, 0};  // model units (GW2 inches)
    double out = 0;
};

struct Slider {
    const char* name;
    std::vector<Edit> edits;
};

const std::vector<Slider>& sliders() {
    constexpr double up = -1, fwd = -1;  // -Z is up, -Y is forward
    static const std::vector<Slider> k = {
        // Head shape
        {"Cheeks", {{"bone:CheekUpperL", {1.15, 1.15, 1.15}, 0, {0, 0.12 * fwd, 0}, 0.22},
                    {"bone:CheekUpperR", {1.15, 1.15, 1.15}, 0, {0, 0.12 * fwd, 0}, 0.22},
                    {"bone:CheekLowerL", {1.15, 1.15, 1.15}, 0, {0, 0.10 * fwd, 0}, 0.25},
                    {"bone:CheekLowerR", {1.15, 1.15, 1.15}, 0, {0, 0.10 * fwd, 0}, 0.25}}},
        {"Jaw Width", {{"bone:Jaw", {1.18, 1, 1}},
                       {"bone:CheekLowerL", {1, 1, 1}, 0, {0, 0, 0}, 0.15},
                       {"bone:CheekLowerR", {1, 1, 1}, 0, {0, 0, 0}, 0.15}}},
        {"Chin Length", {{"bone:Chin", {1, 1, 1.1}, 0, {0, 0.05 * fwd, 0.35 * -up}}}},
        {"Head Width", {{"bone:Head", {1.1, 1, 1}}}},
        {"Head Size", {{"bone:Head", {1.1, 1.1, 1.1}}}},
        // Mouth
        {"Upper Lip Fullness", {{"bone:MouthTop", {1.2, 1.3, 1.3}, 0, {0, 0.1 * fwd, 0}},
                                {"bone:LipMidTopL", {1.2, 1.3, 1.3}, 0, {0, 0.08 * fwd, 0}},
                                {"bone:LipMidTopR", {1.2, 1.3, 1.3}, 0, {0, 0.08 * fwd, 0}}}},
        {"Lower Lip Fullness", {{"bone:MouthBot", {1.2, 1.3, 1.3}, 0, {0, 0.1 * fwd, 0}},
                                {"bone:LipMidBotL", {1.2, 1.3, 1.3}, 0, {0, 0.08 * fwd, 0}},
                                {"bone:LipMidBotR", {1.2, 1.3, 1.3}, 0, {0, 0.08 * fwd, 0}}}},
        {"Mouth Width", {{"bone:MouthL", {1, 1, 1}, 0, {0, 0, 0}, 0.25},
                         {"bone:MouthR", {1, 1, 1}, 0, {0, 0, 0}, 0.25},
                         {"bone:LipMidTopL", {1, 1, 1}, 0, {0, 0, 0}, 0.12},
                         {"bone:LipMidTopR", {1, 1, 1}, 0, {0, 0, 0}, 0.12},
                         {"bone:LipMidBotL", {1, 1, 1}, 0, {0, 0, 0}, 0.12},
                         {"bone:LipMidBotR", {1, 1, 1}, 0, {0, 0, 0}, 0.12}}},
        // Nose (one bone)
        {"Nose Bridge Width", {{"bone:Nose", {1.15, 1, 1.05}}}},
        {"Nose Width at Base", {{"bone:Nose", {1.25, 1, 0.95}}}},
        {"Nose Bridge Height", {{"bone:Nose", {1, 1.1, 1}, 0, {0, 0.18 * fwd, 0}}}},
        {"Nose Height", {{"bone:Nose", {1, 1, 1}, 0, {0, 0, 0.25 * up}}}},
        {"Nose Length", {{"bone:Nose", {1, 1.2, 1.1}, 0, {0, 0.1 * fwd, 0.05 * -up}}}},
        // Eyes
        {"Eyebrow Placement", {{"bone:BrowInnerL", {1, 1, 1}, 0, {0, 0, 0.2 * up}},
                               {"bone:BrowInnerR", {1, 1, 1}, 0, {0, 0, 0.2 * up}},
                               {"bone:BrowOuterL", {1, 1, 1}, 0, {0, 0, 0.2 * up}},
                               {"bone:BrowOuterR", {1, 1, 1}, 0, {0, 0, 0.2 * up}}}},
        {"Eyebrow Angle", {{"bone:BrowOuterL", {1, 1, 1}, 0, {0, 0, 0.2 * up}},
                           {"bone:BrowOuterR", {1, 1, 1}, 0, {0, 0, 0.2 * up}},
                           {"bone:BrowInnerL", {1, 1, 1}, 0, {0, 0, 0.1 * -up}},
                           {"bone:BrowInnerR", {1, 1, 1}, 0, {0, 0, 0.1 * -up}}}},
        {"Brow Size", {{"bone:BrowInnerL", {1.25, 1.25, 1.25}},
                       {"bone:BrowInnerR", {1.25, 1.25, 1.25}},
                       {"bone:BrowOuterL", {1.25, 1.25, 1.25}},
                       {"bone:BrowOuterR", {1.25, 1.25, 1.25}}}},
        {"Iris Size", {{"bone:EyeL", {1.2, 1.2, 1.2}}, {"bone:EyeR", {1.2, 1.2, 1.2}}}},
        {"Eyelid Shape", {{"bone:EyelidUpperL", {1.12, 1, 1}, 6}, {"bone:EyelidUpperR", {1.12, 1, 1}, 6},
                          {"bone:EyelidBotmL", {1.12, 1, 1}, 4}, {"bone:EyelidBotmR", {1.12, 1, 1}, 4}}},
        {"Eye Openness", {{"bone:EyelidUpperL", {1, 1, 1}, 14}, {"bone:EyelidUpperR", {1, 1, 1}, 14},
                          {"bone:EyelidBotmL", {1, 1, 1}, -10}, {"bone:EyelidBotmR", {1, 1, 1}, -10}}},
        {"Eye Width", {{"bone:EyeParentL", {1.15, 1, 1}}, {"bone:EyeParentR", {1.15, 1, 1}}}},
        {"Eye Size", {{"bone:EyeParentL", {1.12, 1.12, 1.12}}, {"bone:EyeParentR", {1.12, 1.12, 1.12}}}},
    };
    return k;
}

// The edit at sign s (+1 / -1): scales invert, rotations and moves negate.
Mat edit_matrix(const Edit& e, const ModelJoint& j, double centre_x, int s) {
    const double cx = j.pos[0], cy = j.pos[1], cz = j.pos[2];
    auto sc = [&](double v) { return s > 0 ? v : 1.0 / v; };
    const double side = j.pos[0] >= centre_x ? 1.0 : -1.0;
    Mat m = translate(-cx, -cy, -cz);
    m = mul(m, scale(sc(e.scale[0]), sc(e.scale[1]), sc(e.scale[2])));
    m = mul(m, rotate_x(s * e.rot_x));
    m = mul(m, translate(cx + s * (e.move[0] + side * e.out), cy + s * e.move[1], cz + s * e.move[2]));
    return m;
}

} // namespace

const std::vector<std::string>& face_slider_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const Slider& s : sliders()) n.push_back(s.name);
        return n;
    }();
    return names;
}

size_t add_face_morphs(ModelPreview& model, const std::map<std::string, float>& values) {
    std::unordered_map<std::string, int> by_name;
    for (size_t i = 0; i < model.joints.size(); ++i) by_name.emplace(model.joints[i].name, static_cast<int>(i));
    const auto head = by_name.find("bone:Head");
    if (head == by_name.end()) return 0;
    const double centre_x = model.joints[static_cast<size_t>(head->second)].pos[0];

    // For each shape key: every joint's effective deformation (its own edit,
    // then its parents'), and whether it moves at all.
    struct Key {
        std::string name;
        float weight;
        std::vector<Mat> eff;
        std::vector<bool> moved;
    };
    std::vector<Key> keys;
    for (const Slider& sl : sliders()) {
        auto v = values.find(sl.name);
        const float value = v == values.end() ? 0.5f : std::clamp(v->second, 0.0f, 1.0f);
        for (int s : {+1, -1}) {
            Key k{std::string(sl.name) + (s > 0 ? "+" : "-"),
                  s > 0 ? std::max(0.0f, (value - 0.5f) * 2) : std::max(0.0f, (0.5f - value) * 2),
                  std::vector<Mat>(model.joints.size(), identity()), std::vector<bool>(model.joints.size(), false)};
            std::vector<Mat> own(model.joints.size(), identity());
            std::vector<bool> has(model.joints.size(), false);
            for (const Edit& e : sl.edits) {
                auto it = by_name.find(e.joint);
                if (it == by_name.end()) continue;
                const size_t ji = static_cast<size_t>(it->second);
                own[ji] = mul(own[ji], edit_matrix(e, model.joints[ji], centre_x, s));
                has[ji] = true;
            }
            std::vector<int> done(model.joints.size(), 0);
            std::function<void(size_t)> resolve = [&](size_t ji) {
                if (done[ji]) return;
                done[ji] = 1;
                const int p = model.joints[ji].parent;
                Mat parent = identity();
                bool parent_moved = false;
                if (p >= 0 && static_cast<size_t>(p) < model.joints.size() && static_cast<size_t>(p) != ji) {
                    resolve(static_cast<size_t>(p));
                    parent = k.eff[static_cast<size_t>(p)];
                    parent_moved = k.moved[static_cast<size_t>(p)];
                }
                k.eff[ji] = mul(own[ji], parent);
                k.moved[ji] = has[ji] || parent_moved;
            };
            for (size_t ji = 0; ji < model.joints.size(); ++ji) resolve(ji);
            keys.push_back(std::move(k));
        }
    }

    size_t touched = 0;
    for (ModelMeshCPU& mesh : model.meshes) {
        if (!mesh.hasSkin) continue;
        std::vector<MorphTargetCPU> targets;
        for (const Key& k : keys) {
            MorphTargetCPU t{k.name, std::vector<float>(mesh.vertices.size() * 3, 0.0f), k.weight};
            bool any = false;
            for (size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
                const GVertex& v = mesh.vertices[vi];
                double sum = 0, out[3] = {0, 0, 0};
                bool moves = false;
                for (int b = 0; b < 4; ++b) {
                    if (v.bwt[b] <= 0 || v.bidx[b] >= model.joints.size()) continue;
                    if (k.moved[v.bidx[b]]) moves = true;
                    const Mat& m = k.eff[v.bidx[b]];
                    const double w = v.bwt[b];
                    out[0] += w * (v.px * m[0] + v.py * m[4] + v.pz * m[8] + m[12]);
                    out[1] += w * (v.px * m[1] + v.py * m[5] + v.pz * m[9] + m[13]);
                    out[2] += w * (v.px * m[2] + v.py * m[6] + v.pz * m[10] + m[14]);
                    sum += w;
                }
                if (!moves || sum <= 0) continue;
                const double d[3] = {out[0] / sum - v.px, out[1] / sum - v.py, out[2] / sum - v.pz};
                if (std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(d[2]) < 1e-6) continue;
                for (int c = 0; c < 3; ++c) t.delta[vi * 3 + c] = static_cast<float>(d[c]);
                any = true;
            }
            if (any) targets.push_back(std::move(t));
        }
        if (!targets.empty()) {
            mesh.morphs = std::move(targets);
            ++touched;
        }
    }
    return touched;
}

} // namespace castlemist::ripper
