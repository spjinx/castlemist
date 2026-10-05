#include "castlemist/ripper/atlas.h"

#include <algorithm>
#include <cmath>
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

float wrap_uv(float u) {
    if (u >= 0.0f && u <= 1.0f) return u;
    return u - std::floor(u);
}

ImageRgba crop_piece(const ImageRgba& tex, const composite::BlitRect& rect, float scale) {
    ImageRgba out;
    out.w = static_cast<int>(static_cast<float>(rect.x1 - rect.x0) / scale);
    out.h = static_cast<int>(static_cast<float>(rect.y1 - rect.y0) / scale);
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

AtlasRegion region_for(const composite::BlitRectSet& set, const std::vector<std::pair<float, float>>& uvs) {
    std::vector<size_t> hits(set.rects.size(), 0);
    for (const auto& [u, v] : uvs) {
        const float x = u * kAtlasSize, y = v * kAtlasSize;
        for (size_t i = 0; i < set.rects.size(); ++i) {
            const composite::BlitRect& r = set.rects[i];
            if (x >= r.x0 && x <= r.x1 && y >= r.y0 && y <= r.y1) {
                ++hits[i];
                break;  // first rect wins on a shared border
            }
        }
    }
    AtlasRegion out;
    const size_t min_hits = std::max<size_t>(1, uvs.size() / 100);
    for (size_t i = 0; i < set.rects.size(); ++i) {
        if (hits[i] < min_hits) continue;
        const composite::BlitRect& r = set.rects[i];
        if (out.rects.empty()) {
            out.ax = r.x0;
            out.ay = r.y0;
        }
        out.ax = std::min(out.ax, r.x0);
        out.ay = std::min(out.ay, r.y0);
        out.rects.push_back(r);
    }
    return out;
}

void blit(ImageRgba& atlas, const ImageRgba& tex, const AtlasRegion& region, float scale, BlitMode mode) {
    if (tex.w <= 0 || tex.h <= 0 || scale <= 0) return;
    auto texel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, tex.w - 1);
        y = std::clamp(y, 0, tex.h - 1);
        return static_cast<float>(tex.px[(static_cast<size_t>(y) * tex.w + x) * 4 + c]);
    };
    for (const composite::BlitRect& r : region.rects) {
        for (uint32_t y = std::max(r.y0, region.ay); y < r.y1 && y < static_cast<uint32_t>(atlas.h); ++y) {
            const float fy = (static_cast<float>(y - region.ay) + 0.5f) / scale - 0.5f;
            if (fy > static_cast<float>(tex.h) - 0.5f) continue;  // past the texture: leave the atlas alone
            const int y0 = static_cast<int>(std::floor(fy));
            const float ty = fy - static_cast<float>(y0);
            for (uint32_t x = std::max(r.x0, region.ax); x < r.x1 && x < static_cast<uint32_t>(atlas.w); ++x) {
                const float fx = (static_cast<float>(x - region.ax) + 0.5f) / scale - 0.5f;
                if (fx > static_cast<float>(tex.w) - 0.5f) continue;
                const int x0 = static_cast<int>(std::floor(fx));
                const float tx = fx - static_cast<float>(x0);
                uint8_t* dst = atlas.px.data() + (static_cast<size_t>(y) * atlas.w + x) * 4;
                float s[4];
                for (int c = 0; c < 4; ++c) {
                    const float top = texel(x0, y0, c) * (1 - tx) + texel(x0 + 1, y0, c) * tx;
                    const float bottom = texel(x0, y0 + 1, c) * (1 - tx) + texel(x0 + 1, y0 + 1, c) * tx;
                    s[c] = top * (1 - ty) + bottom * ty;
                }
                if (mode == BlitMode::Over) {
                    const float a = s[3] / 255.0f;
                    for (int c = 0; c < 3; ++c)
                        dst[c] = static_cast<uint8_t>(std::clamp(dst[c] + (s[c] - dst[c]) * a + 0.5f, 0.0f, 255.0f));
                    // Coverage adds up: hair strands outside the face's islands
                    // keep their own alpha, the face keeps its own under them.
                    dst[3] = static_cast<uint8_t>(std::max<float>(dst[3], std::clamp(s[3] + 0.5f, 0.0f, 255.0f)));
                } else if (mode == BlitMode::Add) {
                    for (int c = 0; c < 3; ++c)
                        dst[c] = static_cast<uint8_t>(std::clamp(dst[c] + s[c] + 0.5f, 0.0f, 255.0f));
                    dst[3] = static_cast<uint8_t>(std::max<float>(dst[3], std::clamp(s[3] + 0.5f, 0.0f, 255.0f)));
                } else {
                    for (int c = 0; c < 4; ++c) dst[c] = static_cast<uint8_t>(std::clamp(s[c] + 0.5f, 0.0f, 255.0f));
                }
            }
        }
    }
}

ImageRgba resize_bilinear(const ImageRgba& src, int w, int h) {
    ImageRgba out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 0)};
    if (src.w <= 0 || src.h <= 0) return out;

    // Edge-aligned: the first/last texel centres land on the first/last pixels.
    const float sx = w > 1 && src.w > 1 ? static_cast<float>(w - 1) / static_cast<float>(src.w - 1) : 1.0f;
    const float sy = h > 1 && src.h > 1 ? static_cast<float>(h - 1) / static_cast<float>(src.h - 1) : 1.0f;
    auto texel = [&](int x, int y, int c) {
        x = std::clamp(x, 0, src.w - 1);
        y = std::clamp(y, 0, src.h - 1);
        return static_cast<float>(src.px[(static_cast<size_t>(y) * src.w + x) * 4 + c]);
    };
    for (int y = 0; y < h; ++y) {
        const float fy = static_cast<float>(y) / sy;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / sx;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                const float top = texel(x0, y0, c) * (1 - tx) + texel(x0 + 1, y0, c) * tx;
                const float bottom = texel(x0, y0 + 1, c) * (1 - tx) + texel(x0 + 1, y0 + 1, c) * tx;
                out.px[(static_cast<size_t>(y) * w + x) * 4 + c] =
                    static_cast<uint8_t>(std::clamp(top * (1 - ty) + bottom * ty + 0.5f, 0.0f, 255.0f));
            }
        }
    }

    return out;
}

void remap_uv(float& u, float& v, const composite::BlitRect& rect) {
    u = (u * kAtlasSize - static_cast<float>(rect.x0)) / static_cast<float>(rect.x1 - rect.x0);
    v = (v * kAtlasSize - static_cast<float>(rect.y0)) / static_cast<float>(rect.y1 - rect.y0);
}

} // namespace castlemist::ripper
