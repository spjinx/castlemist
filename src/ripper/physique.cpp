#include "castlemist/ripper/physique.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "castlemist/native/gw2model.hpp"
#include "castlemist/ripper/skeleton_merge.h"

namespace castlemist::ripper {
namespace {

using Mat = std::array<double, 16>;  // row-major, row vectors: p' = p * M (as ModelJoint)

constexpr double kPi = 3.14159265358979323846;

Mat identity() {
    Mat m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}

Mat load(const float* m) {
    Mat r;
    for (int i = 0; i < 16; ++i) r[i] = m[i];
    return r;
}

Mat mul(const Mat& a, const Mat& b) {  // a applied first, then b
    Mat r{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k) r[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col];
    return r;
}

Mat inverse(Mat a) {  // Gauss-Jordan with partial pivoting
    Mat inv = identity();
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r * 4 + c]) > std::fabs(a[pivot * 4 + c])) pivot = r;
        if (std::fabs(a[pivot * 4 + c]) < 1e-12) return identity();
        for (int k = 0; k < 4; ++k) {
            std::swap(a[c * 4 + k], a[pivot * 4 + k]);
            std::swap(inv[c * 4 + k], inv[pivot * 4 + k]);
        }
        const double d = a[c * 4 + c];
        for (int k = 0; k < 4; ++k) {
            a[c * 4 + k] /= d;
            inv[c * 4 + k] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double f = a[r * 4 + c];
            for (int k = 0; k < 4; ++k) {
                a[r * 4 + k] -= f * a[c * 4 + k];
                inv[r * 4 + k] -= f * inv[c * 4 + k];
            }
        }
    }
    return inv;
}

Mat rotation(double ax, double ay, double az) {  // degrees, x then y then z
    auto rx = identity(), ry = identity(), rz = identity();
    const double a = ax * kPi / 180, b = ay * kPi / 180, c = az * kPi / 180;
    rx[5] = std::cos(a), rx[6] = std::sin(a), rx[9] = -std::sin(a), rx[10] = std::cos(a);
    ry[0] = std::cos(b), ry[2] = -std::sin(b), ry[8] = std::sin(b), ry[10] = std::cos(b);
    rz[0] = std::cos(c), rz[1] = std::sin(c), rz[4] = -std::sin(c), rz[5] = std::cos(c);
    return mul(mul(rx, ry), rz);
}

// One joint's deltas at the preset's weights, in its own frame.
struct Delta {
    double rot[3] = {0, 0, 0}, scale[3] = {0, 0, 0}, offset[3] = {0, 0, 0};
    bool any = false;
};

// "bone:ClavicleL" <-> "bone:ClavicleR"; empty when the name has no side.
std::string twin(const std::string& name) {
    if (name.empty()) return {};
    const char c = name.back();
    if (c == 'L') return name.substr(0, name.size() - 1) + 'R';
    if (c == 'R') return name.substr(0, name.size() - 1) + 'L';
    return {};
}

} // namespace

size_t apply_physique(ModelPreview& model, const composite::BoneScalePreset& preset) {
    const size_t n = model.joints.size();
    if (n == 0) return 0;
    std::vector<Delta> deltas(n);
    auto joint_of = [&](uint64_t token) -> int {
        for (size_t j = 0; j < n; ++j)
            if (castlemist::model::tokenizeBoneName(model.joints[j].name) == token) return static_cast<int>(j);
        return -1;
    };
    auto add = [&](int j, const composite::BoneScaleSub& s, double f) {
        if (j < 0) return;
        Delta& d = deltas[static_cast<size_t>(j)];
        for (int k = 0; k < 3; ++k) {
            d.rot[k] += f * s.values[static_cast<size_t>(k)];
            d.scale[k] += f * s.values[static_cast<size_t>(3 + k)];
            d.offset[k] += f * s.values[static_cast<size_t>(6 + k)];
        }
        d.any = true;
    };
    for (const composite::BoneScaleGroup& g : preset.groups) {
        if (g.weight == 0) continue;
        for (const composite::BoneScaleSub& s : g.subs) {
            const double f = std::clamp(static_cast<double>(g.weight), static_cast<double>(s.min), static_cast<double>(s.max));
            const int j = joint_of(s.bone);
            add(j, s, f);
            if ((s.flag & 2) && j >= 0) {
                const std::string other = twin(model.joints[static_cast<size_t>(j)].name);
                for (size_t k = 0; k < n && !other.empty(); ++k)
                    if (model.joints[k].name == other) add(static_cast<int>(k), s, f);
            }
        }
    }

    // World (bone -> model) and inverse per joint.
    std::vector<Mat> inv_world(n), world(n);
    for (size_t j = 0; j < n; ++j) {
        inv_world[j] = load(model.joints[j].invWorld);
        world[j] = inverse(inv_world[j]);
    }
    // Rigid motion (rotation + offset about the joint, in its frame), carried
    // down the hierarchy; the skinning delta adds the joint's own scale first.
    std::vector<std::optional<Mat>> rigid(n);
    std::vector<Mat> skin(n);
    auto rigid_of = [&](auto&& self, size_t j) -> const Mat& {
        if (rigid[j]) return *rigid[j];
        const Delta& d = deltas[j];
        Mat own = identity();
        if (d.any) {
            Mat local = rotation(d.rot[0], d.rot[1], d.rot[2]);
            local[12] = d.offset[0], local[13] = d.offset[1], local[14] = d.offset[2];
            own = mul(mul(inv_world[j], local), world[j]);
        }
        const int p = model.joints[j].parent;
        rigid[j] = (p >= 0 && static_cast<size_t>(p) < n && static_cast<size_t>(p) != j) ? mul(own, self(self, static_cast<size_t>(p)))
                                                                                        : own;
        return *rigid[j];
    };
    size_t moved = 0;
    for (size_t j = 0; j < n; ++j) {
        const Delta& d = deltas[j];
        Mat s = identity();
        s[0] = 1 + d.scale[0], s[5] = 1 + d.scale[1], s[10] = 1 + d.scale[2];
        skin[j] = mul(mul(mul(inv_world[j], s), world[j]), rigid_of(rigid_of, j));
        if (d.any) ++moved;
    }
    if (!moved) return 0;
    // Normals and tangents: the inverse transpose of each delta's 3x3.
    std::vector<Mat> skin_n(n);
    for (size_t j = 0; j < n; ++j) {
        const Mat inv = inverse(skin[j]);
        Mat t{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) t[r * 4 + c] = inv[c * 4 + r];
        skin_n[j] = t;
    }

    auto xform = [](const Mat& m, const float* v, double w, double* out, bool point) {
        for (int c = 0; c < 3; ++c)
            out[c] += w * (v[0] * m[0 * 4 + c] + v[1] * m[1 * 4 + c] + v[2] * m[2 * 4 + c] + (point ? m[12 + c] : 0.0));
    };
    for (ModelMeshCPU& mesh : model.meshes) {
        if (!mesh.hasSkin) continue;
        for (GVertex& v : mesh.vertices) {
            double p[3] = {0, 0, 0}, nn[3] = {0, 0, 0}, t[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            const float pos[3] = {v.px, v.py, v.pz}, nor[3] = {v.nx, v.ny, v.nz}, tan[3] = {v.tx, v.ty, v.tz},
                        bit[3] = {v.bx, v.by, v.bz};
            double total = 0;
            for (int k = 0; k < 4; ++k) {
                const double w = v.bwt[k];
                if (w <= 0 || v.bidx[k] >= n) continue;
                total += w;
                xform(skin[v.bidx[k]], pos, w, p, true);
                xform(skin_n[v.bidx[k]], nor, w, nn, false);
                xform(skin[v.bidx[k]], tan, w, t, false);
                xform(skin[v.bidx[k]], bit, w, b, false);
            }
            if (total <= 0) continue;
            auto norm = [](double* a) {
                const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                if (l > 1e-12) a[0] /= l, a[1] /= l, a[2] /= l;
            };
            norm(nn), norm(t), norm(b);
            v.px = static_cast<float>(p[0] / total), v.py = static_cast<float>(p[1] / total), v.pz = static_cast<float>(p[2] / total);
            v.nx = static_cast<float>(nn[0]), v.ny = static_cast<float>(nn[1]), v.nz = static_cast<float>(nn[2]);
            v.tx = static_cast<float>(t[0]), v.ty = static_cast<float>(t[1]), v.tz = static_cast<float>(t[2]);
            v.bx = static_cast<float>(b[0]), v.by = static_cast<float>(b[1]), v.bz = static_cast<float>(b[2]);
        }
    }
    // The bind pose follows the rigid part (bones keep unit scale).
    for (size_t j = 0; j < n; ++j) {
        const Mat w = mul(world[j], *rigid[j]);
        const Mat iw = inverse(w);
        for (int k = 0; k < 16; ++k) model.joints[j].invWorld[k] = static_cast<float>(iw[k]);
        for (int k = 0; k < 3; ++k) model.joints[j].pos[k] = static_cast<float>(w[12 + k]);
    }
    for (size_t j = 0; j < n; ++j) {
        const std::array<float, 7> l = local_from_bind(model, j);
        for (int k = 0; k < 3; ++k) model.joints[j].localPos[k] = l[static_cast<size_t>(k)];
        for (int k = 0; k < 4; ++k) model.joints[j].localQuat[k] = l[static_cast<size_t>(3 + k)];
    }
    return moved;
}

} // namespace castlemist::ripper
