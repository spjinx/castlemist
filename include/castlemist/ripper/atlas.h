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
#include <utility>
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

/// A UV coordinate folded into [0,1] the way the game's repeat sampler sees it.
/// GW2 mirrors a garment's halves by shifting one half's UVs by -1 (Angler Vest:
/// u from -0.99 to 0.37), so raw UVs fall outside every atlas rect. Values
/// already in [0,1] -- including a rect's 1.0 edge -- are returned unchanged.
float wrap_uv(float u);

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

/// The block of `tex` the rect shows: its top-left (rect width / scale) x
/// (rect height / scale) texels, transparent where `tex` is smaller. `scale` is
/// atlas pixels per texel: 2 for the usual reduced texture, 1 for a
/// full-resolution copy.
ImageRgba crop_piece(const ImageRgba& tex, const composite::BlitRect& rect, float scale = 2.0f);

/// Nearest-neighbour resize (dye masks that come at another resolution than
/// the base texture are brought to its size rather than dropped).
ImageRgba resize_nearest(const ImageRgba& src, int w, int h);

/// Bilinear resize (bringing a base texture up to its sharper dye masks' size).
ImageRgba resize_bilinear(const ImageRgba& src, int w, int h);

/// Where one part's texture lands in the full character atlas: the rects its
/// UV points fall in, and the anchor (their common top-left) the texture is
/// drawn from at 2x.
struct AtlasRegion {
    std::vector<composite::BlitRect> rects;
    uint32_t ax = 0, ay = 0;
};

/// The rects of `set` containing the (already wrapped) UV points, ignoring
/// rects hit by fewer than 1% of them (stray vertices on a border); anchor =
/// their top-left. Empty when no rect is hit.
AtlasRegion region_for(const composite::BlitRectSet& set, const std::vector<std::pair<float, float>>& uvs);

/// Where a texture `w` x `h` atlas pixels big starts for rects anchored at
/// (x, y): there, moved back as far as it takes to stay inside the atlas -- a
/// texture bigger than its rects is laid out for a larger block (Baggy Cargo
/// Pants: 512 wide for rects from x 640; Devout Gloves: the whole atlas for a
/// 128 px rect).
void fit_anchor(uint32_t& x, uint32_t& y, float w, float h);

/// How a part's texture meets what is already in the atlas.
enum class BlitMode {
    Replace,  // copy RGBA (armor and body: their alpha is a cut-out)
    Over,     // blend RGB by the texture's alpha, alpha = the larger of the two
              // (hair and scalp layers painted over the face, as the game
              // composites them; strands outside the face keep their coverage)
    Add,      // add RGB, alpha = the larger of the two (glow layers summing)
};

/// Draws `tex` into the atlas from the region's anchor at `scale` atlas pixels
/// per texel (2 = the usual reduced texture, 1 = a full-resolution copy),
/// bilinearly filtered, writing only pixels inside the region's rects.
void blit(ImageRgba& atlas, const ImageRgba& tex, const AtlasRegion& region, float scale = 2.0f,
          BlitMode mode = BlitMode::Replace);

/// The blit set's slot that holds the face (rectIndex order: coat, leggings, ?, boots, gloves, ?, helm, aquatic
/// helm, shoulders, face, hair, ears, then chest / feet / hands / legs skin; matched 2026-11-02 against the rects
/// measured in section 3 of docs/research/gw2-armor-skins-and-dyes.md).
constexpr size_t kFaceSlot = 9;
/// fileData skinFlags / hideFlags bits (same layout: 9 face, 10 hair, 11 chest, 12 feet, 13 hands, 14 legs).
constexpr uint32_t kSkinFlagFace = 0x200;
constexpr uint32_t kHideFlagHair = 0x400;

/// The rects of slot `slot`: rect_index holds each slot's first rect, then the rect count. Empty when out of range.
std::vector<composite::BlitRect> slot_rects(const composite::BlitRectSet& set, size_t slot);

/// For a piece whose skinFlags name the face (helms): its texture is laid out from the top-left its own rects
/// share with the face slot's. When a `tw` x `th` texture at `scale` reaches the whole face rect from there,
/// moves `own`'s anchor to that corner and returns the face region (same anchor); else returns empty and
/// leaves `own` as it is.
AtlasRegion face_overlay(AtlasRegion& own, const composite::BlitRectSet& set, int tw, int th, float scale);

/// `region` split into its rects that are the face slot's and the rest; both keep its anchor.
std::pair<AtlasRegion, AtlasRegion> split_face(const AtlasRegion& region, const composite::BlitRectSet& set);

/// `tex` with its alpha replaced by `mask`'s red (nearest-resized to `tex` when the sizes differ).
ImageRgba with_mask_alpha(const ImageRgba& tex, const ImageRgba& mask);

/// Atlas UV -> UV within the cropped block: u' = (u*1024 - x0) / (x1 - x0), same for v.
void remap_uv(float& u, float& v, const composite::BlitRect& rect);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ATLAS_H
