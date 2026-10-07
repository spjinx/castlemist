#include "castlemist/ripper/skeleton_merge.h"

#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace castlemist::ripper {
namespace {

using Mat = std::array<double, 16>;  // row-major, row vectors: p' = p * M

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

// General 4x4 inverse (Gauss-Jordan with partial pivoting).
Mat inverse(Mat a) {
    Mat inv{};
    for (int i = 0; i < 4; ++i) inv[i * 4 + i] = 1;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r * 4 + c]) > std::fabs(a[pivot * 4 + c])) pivot = r;
        if (std::fabs(a[pivot * 4 + c]) < 1e-12) return inv;  // singular: leave as identity
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

int find_joint(const ModelPreview& m, const std::string& name) {
    for (size_t i = 0; i < m.joints.size(); ++i)
        if (m.joints[i].name == name) return static_cast<int>(i);
    return -1;
}

// Appends src's textures/materials (indices shifted) and returns the material offset.
uint32_t append_materials(ModelPreview& dst, const ModelPreview& src) {
    const int tex_off = static_cast<int>(dst.textures.size());
    const uint32_t mat_off = static_cast<uint32_t>(dst.materials.size());
    dst.textures.insert(dst.textures.end(), src.textures.begin(), src.textures.end());
    for (ModelMaterialCPU m : src.materials) {
        if (m.diffuseTex >= 0) m.diffuseTex += tex_off;
        if (m.normalTex >= 0) m.normalTex += tex_off;
        for (auto& ex : m.extraTextures)
            if (ex.texIndex >= 0) ex.texIndex += tex_off;
        m.index += mat_off;
        dst.materials.push_back(std::move(m));
    }
    return mat_off;
}

void add_totals(ModelPreview& dst, const ModelMeshCPU& mesh) {
    dst.totalVerts += static_cast<uint32_t>(mesh.vertices.size());
    dst.totalTris += static_cast<uint32_t>(mesh.indices.size() / 3);
}

// src model space -> dst model space, so src's `sj` lands on dst's `bj` (bind pose).
Mat attach_transform(const ModelPreview& src, int sj, const ModelPreview& dst, int bj) {
    return mul(load(src.joints[static_cast<size_t>(sj)].invWorld),
               inverse(load(dst.joints[static_cast<size_t>(bj)].invWorld)));
}

void transform_vertex(const Mat& t, GVertex& v) {
    auto point = [&](float& x, float& y, float& z) {
        const double nx = x * t[0] + y * t[4] + z * t[8] + t[12];
        const double ny = x * t[1] + y * t[5] + z * t[9] + t[13];
        const double nz = x * t[2] + y * t[6] + z * t[10] + t[14];
        x = static_cast<float>(nx); y = static_cast<float>(ny); z = static_cast<float>(nz);
    };
    auto direction = [&](float& x, float& y, float& z) {
        double nx = x * t[0] + y * t[4] + z * t[8];
        double ny = x * t[1] + y * t[5] + z * t[9];
        double nz = x * t[2] + y * t[6] + z * t[10];
        const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1e-12) { nx /= len; ny /= len; nz /= len; }
        x = static_cast<float>(nx); y = static_cast<float>(ny); z = static_cast<float>(nz);
    };
    point(v.px, v.py, v.pz);
    direction(v.nx, v.ny, v.nz);
    direction(v.tx, v.ty, v.tz);
    direction(v.bx, v.by, v.bz);
}

// Sets a joint's bind local TRS from a row-vector local matrix (p_parent = p_bone * L).
// That is glTF's column-vector T*R*S transposed: row i of L's 3x3 is R's column i times s_i.
void set_local(ModelJoint& j, const Mat& l) {
    double r[3][3], s[3];  // r[row][col], column-vector form
    for (int i = 0; i < 3; ++i) {
        s[i] = std::sqrt(l[i * 4] * l[i * 4] + l[i * 4 + 1] * l[i * 4 + 1] + l[i * 4 + 2] * l[i * 4 + 2]);
        for (int k = 0; k < 3; ++k) r[k][i] = s[i] > 1e-12 ? l[i * 4 + k] / s[i] : (k == i ? 1.0 : 0.0);
    }
    double q[4];  // x y z w
    const double tr = r[0][0] + r[1][1] + r[2][2];
    if (tr > 0) {
        const double w = std::sqrt(1 + tr) * 2;
        q[3] = w / 4; q[0] = (r[2][1] - r[1][2]) / w; q[1] = (r[0][2] - r[2][0]) / w; q[2] = (r[1][0] - r[0][1]) / w;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const double w = std::sqrt(1 + r[0][0] - r[1][1] - r[2][2]) * 2;
        q[3] = (r[2][1] - r[1][2]) / w; q[0] = w / 4; q[1] = (r[0][1] + r[1][0]) / w; q[2] = (r[0][2] + r[2][0]) / w;
    } else if (r[1][1] > r[2][2]) {
        const double w = std::sqrt(1 + r[1][1] - r[0][0] - r[2][2]) * 2;
        q[3] = (r[0][2] - r[2][0]) / w; q[0] = (r[0][1] + r[1][0]) / w; q[1] = w / 4; q[2] = (r[1][2] + r[2][1]) / w;
    } else {
        const double w = std::sqrt(1 + r[2][2] - r[0][0] - r[1][1]) * 2;
        q[3] = (r[1][0] - r[0][1]) / w; q[0] = (r[0][2] + r[2][0]) / w; q[1] = (r[1][2] + r[2][1]) / w; q[2] = w / 4;
    }
    for (int i = 0; i < 4; ++i) j.localQuat[i] = static_cast<float>(q[i]);
    for (int i = 0; i < 3; ++i) j.localPos[i] = static_cast<float>(l[12 + i]);
    for (int i = 0; i < 9; ++i) j.localScale[i] = 0;
    for (int i = 0; i < 3; ++i) j.localScale[i * 4] = static_cast<float>(s[i]);
}

} // namespace

std::array<float, 7> local_from_bind(const ModelPreview& m, size_t joint) {
    const ModelJoint& j = m.joints[joint];
    Mat l = inverse(load(j.invWorld));
    if (j.parent >= 0) l = mul(l, load(m.joints[static_cast<size_t>(j.parent)].invWorld));
    ModelJoint tmp;
    set_local(tmp, l);
    return {tmp.localPos[0], tmp.localPos[1], tmp.localPos[2],
            tmp.localQuat[0], tmp.localQuat[1], tmp.localQuat[2], tmp.localQuat[3]};
}

bool attach_skinned(ModelPreview& dst, const ModelPreview& src, const std::string& src_joint,
                    const std::string& body_joint) {
    const int sj = find_joint(src, src_joint);
    const int bj = find_joint(dst, body_joint);
    if (sj < 0 || bj < 0) return false;
    const Mat t = attach_transform(src, sj, dst, bj);
    const Mat t_inv = inverse(t);

    // src joints dst already has (the shared root) collapse onto the body joint;
    // the rest are appended with their bind moved by t, roots re-parented to it.
    std::unordered_map<std::string, int> in_dst;
    for (size_t i = 0; i < dst.joints.size(); ++i) in_dst.emplace(dst.joints[i].name, static_cast<int>(i));
    std::vector<int> remap(src.joints.size(), bj);
    std::vector<bool> appended(src.joints.size(), false);
    for (size_t i = 0; i < src.joints.size(); ++i) {
        if (in_dst.count(src.joints[i].name)) continue;
        ModelJoint j = src.joints[i];
        const int sp = j.parent;
        const bool parent_appended = sp >= 0 && static_cast<size_t>(sp) < i && appended[static_cast<size_t>(sp)];
        const Mat inv_world = mul(t_inv, load(j.invWorld));  // new model -> old model -> bone
        for (int k = 0; k < 16; ++k) j.invWorld[k] = static_cast<float>(inv_world[k]);
        const Mat world = inverse(inv_world);
        for (int k = 0; k < 3; ++k) j.pos[k] = static_cast<float>(world[12 + k]);
        if (parent_appended) {
            j.parent = remap[static_cast<size_t>(sp)];  // local bind unchanged: both moved by t
        } else {
            j.parent = bj;
            set_local(j, mul(world, load(dst.joints[static_cast<size_t>(bj)].invWorld)));
        }
        remap[i] = static_cast<int>(dst.joints.size());
        appended[i] = true;
        dst.joints.push_back(std::move(j));
    }

    const uint32_t mat_off = append_materials(dst, src);
    for (ModelMeshCPU mesh : src.meshes) {
        mesh.materialIndex += mat_off;
        const bool rigid = !mesh.hasSkin;
        mesh.hasSkin = true;
        for (GVertex& v : mesh.vertices) {
            transform_vertex(t, v);
            if (rigid) {
                v.bidx[0] = static_cast<uint32_t>(bj);
                v.bidx[1] = v.bidx[2] = v.bidx[3] = 0;
                v.bwt[0] = 1;
                v.bwt[1] = v.bwt[2] = v.bwt[3] = 0;
            } else {
                for (uint32_t& b : v.bidx)
                    if (b < remap.size()) b = static_cast<uint32_t>(remap[b]);
            }
        }
        add_totals(dst, mesh);
        dst.meshes.push_back(std::move(mesh));
    }
    return true;
}

size_t merge_into(ModelPreview& dst, const ModelPreview& src) {
    const size_t first = dst.meshes.size();
    if (dst.joints.empty()) {
        dst.joints = src.joints;
        dst.skeletonVersion = src.skeletonVersion;
        dst.skeletonType = src.skeletonType;
        dst.externalSkeletonRef = src.externalSkeletonRef;
    }

    // src joint index -> dst joint index, appending the ones dst lacks in src
    // order (so a parent is always mapped before its children).
    std::unordered_map<std::string, int> by_name;
    for (size_t i = 0; i < dst.joints.size(); ++i) by_name.emplace(dst.joints[i].name, static_cast<int>(i));
    std::vector<int> remap(src.joints.size(), 0);
    for (size_t i = 0; i < src.joints.size(); ++i) {
        auto it = by_name.find(src.joints[i].name);
        if (it != by_name.end()) {
            remap[i] = it->second;
            continue;
        }
        ModelJoint j = src.joints[i];
        j.parent = (j.parent >= 0 && static_cast<size_t>(j.parent) < i) ? remap[static_cast<size_t>(j.parent)] : -1;
        remap[i] = static_cast<int>(dst.joints.size());
        by_name.emplace(j.name, remap[i]);
        dst.joints.push_back(std::move(j));
    }

    const uint32_t mat_off = append_materials(dst, src);
    for (ModelMeshCPU mesh : src.meshes) {
        mesh.materialIndex += mat_off;
        if (!remap.empty())
            for (GVertex& v : mesh.vertices)
                for (uint32_t& b : v.bidx)
                    if (b < remap.size()) b = static_cast<uint32_t>(remap[b]);
        add_totals(dst, mesh);
        dst.meshes.push_back(std::move(mesh));
    }
    return first;
}

bool attach_rigid(ModelPreview& dst, const ModelPreview& weapon, const std::string& weapon_joint,
                  const std::string& body_joint) {
    const int wj = find_joint(weapon, weapon_joint);
    const int bj = find_joint(dst, body_joint);
    if (wj < 0 || bj < 0) return false;

    // Weapon model space -> weapon point space -> body model space.
    const Mat t = attach_transform(weapon, wj, dst, bj);

    const uint32_t mat_off = append_materials(dst, weapon);
    for (ModelMeshCPU mesh : weapon.meshes) {
        mesh.materialIndex += mat_off;
        mesh.hasSkin = true;
        for (GVertex& v : mesh.vertices) {
            transform_vertex(t, v);
            v.bidx[0] = static_cast<uint32_t>(bj);
            v.bidx[1] = v.bidx[2] = v.bidx[3] = 0;
            v.bwt[0] = 1;
            v.bwt[1] = v.bwt[2] = v.bwt[3] = 0;
        }
        add_totals(dst, mesh);
        dst.meshes.push_back(std::move(mesh));
    }
    return true;
}

} // namespace castlemist::ripper
