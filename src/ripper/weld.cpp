#include "castlemist/ripper/weld.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace castlemist::ripper {

size_t weld_normals(ModelPreview& m, const std::vector<size_t>& meshes, float eps, float max_angle_deg) {
    struct Ref {
        size_t mesh, vert;
    };
    auto key = [&](const GVertex& g) {
        auto q = [&](float f) { return static_cast<int64_t>(std::llround(f / eps)) & 0x1FFFFF; };
        return (q(g.px) << 42) | (q(g.py) << 21) | q(g.pz);
    };
    std::unordered_map<int64_t, std::vector<Ref>> at;
    for (size_t mi : meshes) {
        if (mi >= m.meshes.size()) continue;
        const std::vector<GVertex>& vs = m.meshes[mi].vertices;
        for (size_t vi = 0; vi < vs.size(); ++vi) at[key(vs[vi])].push_back({mi, vi});
    }
    const float cos_max = std::cos(max_angle_deg * 3.14159265f / 180.0f);
    size_t changed = 0;
    for (auto& [k, refs] : at) {
        if (refs.size() < 2) continue;
        std::vector<std::array<float, 3>> out(refs.size());
        for (size_t a = 0; a < refs.size(); ++a) {
            const GVertex& va = m.meshes[refs[a].mesh].vertices[refs[a].vert];
            float sx = 0, sy = 0, sz = 0;
            for (const Ref& r : refs) {
                const GVertex& vb = m.meshes[r.mesh].vertices[r.vert];
                if (va.nx * vb.nx + va.ny * vb.ny + va.nz * vb.nz < cos_max) continue;
                sx += vb.nx;
                sy += vb.ny;
                sz += vb.nz;
            }
            const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
            out[a] = len > 1e-6f ? std::array<float, 3>{sx / len, sy / len, sz / len}
                                 : std::array<float, 3>{va.nx, va.ny, va.nz};
        }
        for (size_t a = 0; a < refs.size(); ++a) {
            GVertex& va = m.meshes[refs[a].mesh].vertices[refs[a].vert];
            if (std::fabs(va.nx - out[a][0]) + std::fabs(va.ny - out[a][1]) + std::fabs(va.nz - out[a][2]) > 1e-5f) ++changed;
            va.nx = out[a][0];
            va.ny = out[a][1];
            va.nz = out[a][2];
        }
    }
    return changed;
}

} // namespace castlemist::ripper
