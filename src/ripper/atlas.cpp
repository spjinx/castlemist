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

void fit_anchor(uint32_t& x, uint32_t& y, float w, float h) {
    auto fit = [](uint32_t& a, float size) {
        const float room = kAtlasSize - size;
        if (static_cast<float>(a) > room) a = room > 0 ? static_cast<uint32_t>(room) : 0;
    };
    fit(x, w);
    fit(y, h);
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

std::vector<composite::BlitRect> slot_rects(const composite::BlitRectSet& set, size_t slot) {
    if (slot + 1 >= set.rect_index.size()) return {};
    const size_t a = set.rect_index[slot], b = set.rect_index[slot + 1];
    if (a > b || b > set.rects.size()) return {};
    return {set.rects.begin() + static_cast<std::ptrdiff_t>(a), set.rects.begin() + static_cast<std::ptrdiff_t>(b)};
}

std::vector<size_t> armor_slots(uint8_t type) {
    switch (type) {
        case 9: return {0};
        case 12: return {1};
        case 8: return {2, 3};
        case 10: return {4, 5};
        case 11: return {6};
        case 13: return {7};
        case 14: return {8};
        default: return {};
    }
}

size_t flag_slot(int bit) {
    if (bit >= 0 && bit <= 10) return static_cast<size_t>(bit);
    if (bit >= 11 && bit <= 14) return static_cast<size_t>(bit + 1);
    if (bit == 16) return 11;
    return kNoSlot;
}

bool is_skin_slot(size_t slot) { return slot == kFaceSlot || slot == 11 || (slot >= 12 && slot <= 15); }

PieceRegions piece_regions(const composite::BlitRectSet& set, uint8_t type, uint32_t hide_flags, uint32_t skin_flags,
                           const std::vector<size_t>& lost, int tw, int th, float scale) {
    const std::vector<size_t> own = armor_slots(type);
    if (own.empty() || set.rect_index.empty() || tw <= 0 || th <= 0) return {};
    auto is_lost = [&](size_t s) { return std::find(lost.begin(), lost.end(), s) != lost.end(); };
    std::vector<size_t> named = own, paint, skin, over;
    for (size_t s : own)
        if (!is_lost(s)) paint.push_back(s);
    for (int bit = 0; bit < 32; ++bit) {
        const bool hide = (hide_flags >> bit) & 1u, sk = (skin_flags >> bit) & 1u;
        const size_t s = flag_slot(bit);
        if ((!hide && !sk) || s == kNoSlot || std::find(named.begin(), named.end(), s) != named.end()) continue;
        named.push_back(s);
        if (is_lost(s)) continue;
        // Slots its hideFlags take: its own paint; skin over the skin by its alpha (the Mad Prince's coat is
        // clear over the chest, which keeps his neck). Skin named by skinFlags alone: through the cut mask
        // (his helm's face paint).
        if (hide && is_skin_slot(s)) over.push_back(s);
        else if (hide) paint.push_back(s);
        else if (is_skin_slot(s)) skin.push_back(s);
    }
    bool any = false;
    uint32_t ax = 0, ay = 0;
    for (size_t s : named)
        for (const composite::BlitRect& r : slot_rects(set, s)) {
            ax = any ? std::min(ax, r.x0) : r.x0;
            ay = any ? std::min(ay, r.y0) : r.y0;
            any = true;
        }
    if (!any) return {};
    PieceRegions out{{{}, ax, ay}, {{}, ax, ay}, {{}, ax, ay}};
    for (size_t s : paint) for (const composite::BlitRect& r : slot_rects(set, s)) out.paint.rects.push_back(r);
    for (size_t s : skin) for (const composite::BlitRect& r : slot_rects(set, s)) out.skin.rects.push_back(r);
    for (size_t s : over) for (const composite::BlitRect& r : slot_rects(set, s)) out.over.rects.push_back(r);
    const float x1 = static_cast<float>(ax) + scale * static_cast<float>(tw);
    const float y1 = static_cast<float>(ay) + scale * static_cast<float>(th);
    for (const AtlasRegion* g : {&out.paint, &out.skin, &out.over})
        for (const composite::BlitRect& r : g->rects)
            if (static_cast<float>(r.x1) > x1 + 0.5f || static_cast<float>(r.y1) > y1 + 0.5f) return {};
    return out;
}

std::pair<AtlasRegion, AtlasRegion> split_face(const AtlasRegion& region, const composite::BlitRectSet& set) {
    const std::vector<composite::BlitRect> face = slot_rects(set, kFaceSlot);
    AtlasRegion f{{}, region.ax, region.ay}, rest{{}, region.ax, region.ay};
    for (const composite::BlitRect& r : region.rects) {
        const bool is_face = std::any_of(face.begin(), face.end(), [&](const composite::BlitRect& q) {
            return q.x0 == r.x0 && q.y0 == r.y0 && q.x1 == r.x1 && q.y1 == r.y1;
        });
        (is_face ? f : rest).rects.push_back(r);
    }
    return {f, rest};
}

ImageRgba with_mask_alpha(const ImageRgba& tex, const ImageRgba& mask) {
    ImageRgba out = tex;
    if (mask.w <= 0 || mask.h <= 0) return out;
    const ImageRgba m = mask.w == tex.w && mask.h == tex.h ? mask : resize_nearest(mask, tex.w, tex.h);
    for (size_t i = 0; i + 3 < out.px.size() && i < m.px.size(); i += 4) out.px[i + 3] = m.px[i];
    return out;
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
