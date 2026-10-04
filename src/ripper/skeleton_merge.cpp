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

} // namespace

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
    const Mat t = mul(load(weapon.joints[static_cast<size_t>(wj)].invWorld),
                      inverse(load(dst.joints[static_cast<size_t>(bj)].invWorld)));
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

    const uint32_t mat_off = append_materials(dst, weapon);
    for (ModelMeshCPU mesh : weapon.meshes) {
        mesh.materialIndex += mat_off;
        mesh.hasSkin = true;
        for (GVertex& v : mesh.vertices) {
            point(v.px, v.py, v.pz);
            direction(v.nx, v.ny, v.nz);
            direction(v.tx, v.ty, v.tz);
            direction(v.bx, v.by, v.bz);
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
