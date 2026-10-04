#include "castlemist/ripper/atlas.h"

#include <algorithm>
#include <cstring>

namespace castlemist::ripper {

std::optional<composite::BlitRect> piece_rect(const composite::BlitRectSet& set, float umin, float vmin, float umax,
                                              float vmax) {
    constexpr float kTol = 2.0f;
    const float x0 = umin * kAtlasSize, y0 = vmin * kAtlasSize, x1 = umax * kAtlasSize, y1 = vmax * kAtlasSize;
    for (const composite::BlitRect& r : set.rects) {
        if (x0 >= r.x0 - kTol && x1 <= r.x1 + kTol && y0 >= r.y0 - kTol && y1 <= r.y1 + kTol) return r;
    }
    return std::nullopt;
}

std::optional<composite::BlitRect> choose_armor_rect(const composite::BlitRectSet& set,
                                                     const std::vector<MeshUvInfo>& meshes) {
    std::optional<composite::BlitRect> best;
    size_t best_verts = 0;
    for (const MeshUvInfo& m : meshes) {
        if (m.skin || m.verts == 0) continue;
        auto r = piece_rect(set, m.umin, m.vmin, m.umax, m.vmax);
        if (!r) continue;
        size_t total = 0;  // every non-Skin vertex that lives in this rect
        for (const MeshUvInfo& o : meshes) {
            if (o.skin || o.verts == 0) continue;
            auto ro = piece_rect(set, o.umin, o.vmin, o.umax, o.vmax);
            if (ro && ro->x0 == r->x0 && ro->y0 == r->y0 && ro->x1 == r->x1 && ro->y1 == r->y1) total += o.verts;
        }
        if (total > best_verts) {
            best_verts = total;
            best = r;
        }
    }
    return best;
}

ImageRgba crop_piece(const ImageRgba& tex, const composite::BlitRect& rect) {
    ImageRgba out;
    out.w = static_cast<int>(rect.x1 - rect.x0) / 2;
    out.h = static_cast<int>(rect.y1 - rect.y0) / 2;
    out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    const int cw = std::min(out.w, tex.w), ch = std::min(out.h, tex.h);
    for (int y = 0; y < ch; ++y)
        std::memcpy(out.px.data() + static_cast<size_t>(y) * out.w * 4, tex.px.data() + static_cast<size_t>(y) * tex.w * 4,
                    static_cast<size_t>(cw) * 4);
    return out;
}

ImageRgba resize_nearest(const ImageRgba& src, int w, int h) {
    ImageRgba out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 0)};
    if (src.w <= 0 || src.h <= 0) return out;
    for (int y = 0; y < h; ++y) {
        const int sy = static_cast<int>(static_cast<int64_t>(y) * src.h / h);
        for (int x = 0; x < w; ++x) {
            const int sx = static_cast<int>(static_cast<int64_t>(x) * src.w / w);
            std::memcpy(out.px.data() + (static_cast<size_t>(y) * w + x) * 4,
                        src.px.data() + (static_cast<size_t>(sy) * src.w + sx) * 4, 4);
        }
    }
    return out;
}

void remap_uv(float& u, float& v, const composite::BlitRect& rect) {
    u = (u * kAtlasSize - static_cast<float>(rect.x0)) / static_cast<float>(rect.x1 - rect.x0);
    v = (v * kAtlasSize - static_cast<float>(rect.y0)) / static_cast<float>(rect.y1 - rect.y0);
}

} // namespace castlemist::ripper
