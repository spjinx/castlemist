#include "castlemist/ripper/thumbnail.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace castlemist::ripper {
namespace {

float dot(const std::array<float, 3>& a, float x, float y, float z) { return a[0] * x + a[1] * y + a[2] * z; }

float wrap01(float t) { return t - std::floor(t); }

std::array<float, 3> normalized(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f)
        for (float& c : v) c /= l;
    return v;
}

std::array<float, 3> cross(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

} // namespace

ThumbnailView view_along(std::array<float, 3> forward) {
    ThumbnailView v;
    v.forward = normalized(forward);
    const std::array<float, 3> world_up{0, 0, -1};
    const float d = world_up[0] * v.forward[0] + world_up[1] * v.forward[1] + world_up[2] * v.forward[2];
    v.up = normalized({world_up[0] - d * v.forward[0], world_up[1] - d * v.forward[1], world_up[2] - d * v.forward[2]});
    v.right = cross(v.up, v.forward);
    return v;
}

ThumbnailView front_view() { return view_along({0, 1, 0}); }

ThumbnailView side_view() { return view_along({-1, 0, 0}); }

ThumbnailView three_quarter_top_view() {
    const float yaw = 0.785398f, pitch = 0.610865f;  // 45 degrees around, 35 down
    return view_along({std::cos(pitch) * std::sin(yaw), std::cos(pitch) * std::cos(yaw), std::sin(pitch)});
}

ImageRgba render_thumbnail(const ModelPreview& model, int size, const ThumbnailView& view) {
    ImageRgba img{size, size, std::vector<uint8_t>(static_cast<size_t>(size) * size * 4)};
    for (size_t i = 0; i < img.px.size(); i += 4)
        for (int c = 0; c < 4; ++c) img.px[i + c] = view.background[static_cast<size_t>(c)];

    // Frame: the bounding box of every vertex, projected.
    float x0 = std::numeric_limits<float>::max(), x1 = -x0, y0 = x0, y1 = -x0;
    for (const ModelMeshCPU& m : model.meshes)
        for (const GVertex& v : m.vertices) {
            const float x = dot(view.right, v.px, v.py, v.pz), y = dot(view.up, v.px, v.py, v.pz);
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
    if (x1 <= x0 || y1 <= y0) return img;
    const float span = std::max(x1 - x0, y1 - y0);
    const float scale = size * (1 - 2 * view.margin) / span;
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    auto to_px = [&](float x, float y) {
        return std::array<float, 2>{size / 2.0f + (x - cx) * scale, size / 2.0f - (y - cy) * scale};
    };

    std::vector<float> depth(static_cast<size_t>(size) * size, std::numeric_limits<float>::max());
    // Light from the camera, a little above and to the left.
    std::array<float, 3> light{};
    for (int k = 0; k < 3; ++k) light[k] = -view.forward[k] * 0.8f + view.up[k] * 0.45f - view.right[k] * 0.3f;
    const float ll = std::sqrt(light[0] * light[0] + light[1] * light[1] + light[2] * light[2]);
    for (float& c : light) c /= ll;

    for (const ModelMeshCPU& m : model.meshes) {
        const ModelTextureCPU* tex = nullptr;
        if (m.materialIndex < model.materials.size()) {
            const int t = model.materials[m.materialIndex].diffuseTex;
            if (t >= 0 && static_cast<size_t>(t) < model.textures.size() && model.textures[static_cast<size_t>(t)].width > 0)
                tex = &model.textures[static_cast<size_t>(t)];
        }
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const GVertex* v[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const uint32_t idx = m.indices[i + static_cast<size_t>(k)];
                if (idx >= m.vertices.size()) { ok = false; break; }
                v[k] = &m.vertices[idx];
            }
            if (!ok) continue;
            std::array<float, 2> p[3];
            float z[3];
            for (int k = 0; k < 3; ++k) {
                p[k] = to_px(dot(view.right, v[k]->px, v[k]->py, v[k]->pz), dot(view.up, v[k]->px, v[k]->py, v[k]->pz));
                z[k] = dot(view.forward, v[k]->px, v[k]->py, v[k]->pz);
            }
            const float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
            if (std::fabs(area) < 1e-6f) continue;
            const int bx0 = std::max(0, static_cast<int>(std::floor(std::min({p[0][0], p[1][0], p[2][0]}))));
            const int bx1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({p[0][0], p[1][0], p[2][0]}))));
            const int by0 = std::max(0, static_cast<int>(std::floor(std::min({p[0][1], p[1][1], p[2][1]}))));
            const int by1 = std::min(size - 1, static_cast<int>(std::ceil(std::max({p[0][1], p[1][1], p[2][1]}))));
            for (int y = by0; y <= by1; ++y)
                for (int x = bx0; x <= bx1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((p[1][0] - px) * (p[2][1] - py) - (p[2][0] - px) * (p[1][1] - py)) / area;
                    const float w1 = ((p[2][0] - px) * (p[0][1] - py) - (p[0][0] - px) * (p[2][1] - py)) / area;
                    const float w2 = 1 - w0 - w1;
                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                    const float zz = w0 * z[0] + w1 * z[1] + w2 * z[2];
                    float& zb = depth[static_cast<size_t>(y) * size + x];
                    if (zz >= zb) continue;
                    std::array<float, 4> col{200, 200, 200, 255};
                    if (tex) {
                        const float u = wrap01(w0 * v[0]->u + w1 * v[1]->u + w2 * v[2]->u);
                        const float vv = wrap01(w0 * v[0]->v + w1 * v[1]->v + w2 * v[2]->v);
                        const int tx = std::min(tex->width - 1, static_cast<int>(u * tex->width));
                        const int ty = std::min(tex->height - 1, static_cast<int>(vv * tex->height));
                        const uint8_t* t = tex->rgba.data() + (static_cast<size_t>(ty) * tex->width + tx) * 4;
                        if (t[3] < 64) continue;  // the export's alpha cut-out
                        col = {float(t[0]), float(t[1]), float(t[2]), 255};
                    }
                    // Normal (two-sided), lambert + ambient.
                    float nx = w0 * v[0]->nx + w1 * v[1]->nx + w2 * v[2]->nx;
                    float ny = w0 * v[0]->ny + w1 * v[1]->ny + w2 * v[2]->ny;
                    float nz = w0 * v[0]->nz + w1 * v[1]->nz + w2 * v[2]->nz;
                    const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                    float shade = 0.75f;
                    if (nl > 1e-6f) {
                        nx /= nl; ny /= nl; nz /= nl;
                        shade = 0.35f + 0.75f * std::fabs(light[0] * nx + light[1] * ny + light[2] * nz);
                    }
                    zb = zz;
                    uint8_t* d = img.px.data() + (static_cast<size_t>(y) * size + x) * 4;
                    for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>(std::clamp(col[static_cast<size_t>(c)] * shade, 0.0f, 255.0f));
                    d[3] = 255;
                }
        }
    }
    return img;
}

} // namespace castlemist::ripper
