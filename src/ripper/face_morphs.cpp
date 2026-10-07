#include "castlemist/ripper/face_morphs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
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
        // Eyes -- small amounts: the eye region is a few units across, and a
        // brow or lid moved by more than a fraction of that reads as droopy.
        {"Eyebrow Placement", {{"bone:BrowInnerL", {1, 1, 1}, 0, {0, 0, 0.08 * up}},
                               {"bone:BrowInnerR", {1, 1, 1}, 0, {0, 0, 0.08 * up}},
                               {"bone:BrowOuterL", {1, 1, 1}, 0, {0, 0, 0.08 * up}},
                               {"bone:BrowOuterR", {1, 1, 1}, 0, {0, 0, 0.08 * up}}}},
        {"Eyebrow Angle", {{"bone:BrowOuterL", {1, 1, 1}, 0, {0, 0, 0.08 * up}},
                           {"bone:BrowOuterR", {1, 1, 1}, 0, {0, 0, 0.08 * up}},
                           {"bone:BrowInnerL", {1, 1, 1}, 0, {0, 0, 0.04 * -up}},
                           {"bone:BrowInnerR", {1, 1, 1}, 0, {0, 0, 0.04 * -up}}}},
        {"Brow Size", {{"bone:BrowInnerL", {1.1, 1.1, 1.1}},
                       {"bone:BrowInnerR", {1.1, 1.1, 1.1}},
                       {"bone:BrowOuterL", {1.1, 1.1, 1.1}},
                       {"bone:BrowOuterR", {1.1, 1.1, 1.1}}}},
        {"Iris Size", {{"bone:EyeL", {1.08, 1.08, 1.08}}, {"bone:EyeR", {1.08, 1.08, 1.08}}}},
        {"Eyelid Shape", {{"bone:EyelidUpperL", {1.05, 1, 1}, 3}, {"bone:EyelidUpperR", {1.05, 1, 1}, 3},
                          {"bone:EyelidBotmL", {1.05, 1, 1}, 2}, {"bone:EyelidBotmR", {1.05, 1, 1}, 2}}},
        {"Eye Openness", {{"bone:EyelidUpperL", {1, 1, 1}, 7}, {"bone:EyelidUpperR", {1, 1, 1}, 7},
                          {"bone:EyelidBotmL", {1, 1, 1}, -5}, {"bone:EyelidBotmR", {1, 1, 1}, -5}}},
        {"Eye Width", {{"bone:EyeParentL", {1.06, 1, 1}}, {"bone:EyeParentR", {1.06, 1, 1}}}},
        {"Eye Size", {{"bone:EyeParentL", {1.06, 1.06, 1.06}}, {"bone:EyeParentR", {1.06, 1.06, 1.06}}}},
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

namespace {

// A shape key: every joint's effective deformation (its own edit, then its
// parents'), and whether it moves at all.
struct Key {
    std::string name;
    float weight;
    std::vector<Mat> eff;
    std::vector<bool> moved;
};

struct Rig {
    const ModelPreview& model;
    std::unordered_map<std::string, int> by_name;
    double centre_x = 0;
};

std::optional<Rig> rig_of(const ModelPreview& model) {
    Rig r{model, {}, 0};
    for (size_t i = 0; i < model.joints.size(); ++i) r.by_name.emplace(model.joints[i].name, static_cast<int>(i));
    const auto head = r.by_name.find("bone:Head");
    if (head == r.by_name.end()) return std::nullopt;
    r.centre_x = model.joints[static_cast<size_t>(head->second)].pos[0];
    return r;
}

Key make_key(const Rig& rig, std::string name, float weight, const std::vector<Edit>& edits, int sign) {
    const ModelPreview& model = rig.model;
    Key k{std::move(name), weight, std::vector<Mat>(model.joints.size(), identity()),
          std::vector<bool>(model.joints.size(), false)};
    std::vector<Mat> own(model.joints.size(), identity());
    std::vector<bool> has(model.joints.size(), false);
    for (const Edit& e : edits) {
        auto it = rig.by_name.find(e.joint);
        if (it == rig.by_name.end()) continue;
        const size_t ji = static_cast<size_t>(it->second);
        own[ji] = mul(own[ji], edit_matrix(e, model.joints[ji], rig.centre_x, sign));
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
    return k;
}

// Bakes the keys through the skin weights into morph targets on every skinned
// mesh they move (appended to existing targets). Returns meshes touched.
size_t bake_keys(ModelPreview& model, const std::vector<Key>& keys) {
    size_t touched = 0;
    for (ModelMeshCPU& mesh : model.meshes) {
        if (!mesh.hasSkin) continue;
        bool added = false;
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
            if (any) {
                mesh.morphs.push_back(std::move(t));
                added = true;
            }
        }
        touched += added;
    }
    return touched;
}

// VRChat's own shape keys: blinks for eye tracking and the 15 visemes it
// drives from the voice (Avatar Descriptor > LipSync > Viseme Blend Shape).
// Approximations from the face rig: the jaw opens by turning bone:Jaw about
// its pivot (negative = the chin goes down), the corners move in or out, the
// lips push forward.
struct NamedKey {
    const char* name;
    std::vector<Edit> edits;
};

std::vector<Edit> mouth(double jaw_deg, double corners_out, double lips_fwd, double press = 0) {
    constexpr double fwd = -1, up = -1;
    std::vector<Edit> e = {{"bone:Jaw", {1, 1, 1}, -jaw_deg},
                           {"bone:MouthL", {1, 1, 1}, 0, {0, 0, 0}, corners_out},
                           {"bone:MouthR", {1, 1, 1}, 0, {0, 0, 0}, corners_out}};
    if (lips_fwd != 0 || press != 0) {
        e.push_back({"bone:MouthTop", {1, 1, 1}, 0, {0, lips_fwd * fwd, press * -up}});
        e.push_back({"bone:MouthBot", {1, 1, 1}, 0, {0, lips_fwd * fwd, press * up}});
        e.push_back({"bone:LipMidTopL", {1, 1, 1}, 0, {0, lips_fwd * 0.8 * fwd, press * -up}});
        e.push_back({"bone:LipMidTopR", {1, 1, 1}, 0, {0, lips_fwd * 0.8 * fwd, press * -up}});
        e.push_back({"bone:LipMidBotL", {1, 1, 1}, 0, {0, lips_fwd * 0.8 * fwd, press * up}});
        e.push_back({"bone:LipMidBotR", {1, 1, 1}, 0, {0, lips_fwd * 0.8 * fwd, press * up}});
    }
    return e;
}

const std::vector<NamedKey>& vrchat_keys() {
    static const std::vector<NamedKey> k = {
        {"Blink", {{"bone:EyelidUpperL", {1, 1, 1}, -30}, {"bone:EyelidUpperR", {1, 1, 1}, -30},
                   {"bone:EyelidBotmL", {1, 1, 1}, 8}, {"bone:EyelidBotmR", {1, 1, 1}, 8}}},
        {"Blink_L", {{"bone:EyelidUpperL", {1, 1, 1}, -30}, {"bone:EyelidBotmL", {1, 1, 1}, 8}}},
        {"Blink_R", {{"bone:EyelidUpperR", {1, 1, 1}, -30}, {"bone:EyelidBotmR", {1, 1, 1}, 8}}},
        {"vrc.v_aa", mouth(14, 0.05, 0)},
        {"vrc.v_ch", mouth(6, -0.10, 0.08)},
        {"vrc.v_dd", mouth(8, 0, 0)},
        {"vrc.v_e", mouth(7, 0.15, 0)},
        {"vrc.v_ff", mouth(3, 0.04, 0, 0.04)},
        {"vrc.v_ih", mouth(5, 0.10, 0)},
        {"vrc.v_kk", mouth(7, 0.05, 0)},
        {"vrc.v_nn", mouth(5, 0, 0)},
        {"vrc.v_oh", mouth(11, -0.18, 0.10)},
        {"vrc.v_ou", mouth(6, -0.25, 0.15)},
        {"vrc.v_pp", mouth(0, 0, 0.02, 0.06)},
        {"vrc.v_rr", mouth(5, -0.08, 0.04)},
        {"vrc.v_ss", mouth(3, 0.12, 0)},
        {"vrc.v_th", mouth(5, 0, 0.05)},
    };
    return k;
}

} // namespace

size_t add_face_morphs(ModelPreview& model, const std::map<std::string, float>& values) {
    const std::optional<Rig> rig = rig_of(model);
    if (!rig) return 0;
    std::vector<Key> keys;
    for (const Slider& sl : sliders()) {
        auto v = values.find(sl.name);
        const float value = v == values.end() ? 0.5f : std::clamp(v->second, 0.0f, 1.0f);
        keys.push_back(make_key(*rig, std::string(sl.name) + "+", std::max(0.0f, (value - 0.5f) * 2), sl.edits, +1));
        keys.push_back(make_key(*rig, std::string(sl.name) + "-", std::max(0.0f, (0.5f - value) * 2), sl.edits, -1));
    }
    return bake_keys(model, keys);
}

size_t add_vrchat_face_keys(ModelPreview& model) {
    const std::optional<Rig> rig = rig_of(model);
    if (!rig) return 0;
    std::vector<Key> keys;
    for (const NamedKey& nk : vrchat_keys()) keys.push_back(make_key(*rig, nk.name, 0, nk.edits, +1));
    return bake_keys(model, keys);
}

const std::vector<std::string>& vrchat_face_key_names() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        for (const NamedKey& k : vrchat_keys()) n.push_back(k.name);
        return n;
    }();
    return names;
}

} // namespace castlemist::ripper
