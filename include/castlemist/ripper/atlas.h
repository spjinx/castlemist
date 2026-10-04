#ifndef CASTLEMIST_RIPPER_ATLAS_H
#define CASTLEMIST_RIPPER_ATLAS_H

// The character atlas: GW2 composites every armor piece's textures into one
// 1024x1024 image, each into its own blit rect, and armor UVs address that
// atlas directly. A piece texture is drawn at 2x, anchored at its rect's
// top-left and clipped to the rect (docs/research/gw2-armor-skins-and-dyes.md
// section 3). For a standalone piece export we cut that block back out and
// remap the UVs to it.

#include <cstdint>
#include <optional>
#include <vector>

#include "castlemist/format/composite.h"

namespace castlemist::ripper {

constexpr float kAtlasSize = 1024.0f;

struct ImageRgba {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // w*h RGBA8
};

/// The rect of `set` containing the UV box (atlas px, 2 px tolerance), or nullopt.
std::optional<composite::BlitRect> piece_rect(const composite::BlitRectSet& set, float umin, float vmin, float umax,
                                              float vmax);

/// One mesh, as rect selection sees it: its UV box, vertex count, and whether
/// its material is the body `Skin` (which samples the body texture, not the armor).
struct MeshUvInfo {
    float umin = 0, vmin = 0, umax = 0, vmax = 0;
    size_t verts = 0;
    bool skin = false;
};

/// The piece's rect: among non-Skin meshes whose UV box fits a rect, the rect
/// holding the most vertices. nullopt when none fits.
std::optional<composite::BlitRect> choose_armor_rect(const composite::BlitRectSet& set,
                                                     const std::vector<MeshUvInfo>& meshes);

/// The block of `tex` the rect shows: its top-left (rect width/2) x (rect height/2)
/// texels, transparent where `tex` is smaller.
ImageRgba crop_piece(const ImageRgba& tex, const composite::BlitRect& rect);

/// Nearest-neighbour resize (dye masks that come at another resolution than
/// the base texture are brought to its size rather than dropped).
ImageRgba resize_nearest(const ImageRgba& src, int w, int h);

/// Atlas UV -> UV within the cropped block: u' = (u*1024 - x0) / (x1 - x0), same for v.
void remap_uv(float& u, float& v, const composite::BlitRect& rect);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ATLAS_H
