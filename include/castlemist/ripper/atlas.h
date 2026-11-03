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

constexpr size_t kNoSlot = static_cast<size_t>(-1);

/// The slots a composite piece type owns (2026-11-02, from the Mad Prince's textures): 9 coat -> 0, 12 leggings
/// -> 1, 8 boots -> 2, 3, 10 gloves -> 4, 5, 11 helm -> 6, 13 aquatic helm -> 7, 14 shoulders -> 8. Empty for
/// any other type.
std::vector<size_t> armor_slots(uint8_t type);

/// The slot a hideFlags / skinFlags bit names: bits 0..10 are slots 0..10, 11..14 the chest / feet / hands /
/// legs skin (slots 12..15), 16 the ears (slot 11). kNoSlot for any other bit.
size_t flag_slot(int bit);

/// Face, ears and the bare body: slots a piece paints only through its cut mask.
bool is_skin_slot(size_t slot);

struct PieceRegions {
    AtlasRegion paint;  // drawn as the piece draws (its own slots and the ones its hideFlags take)
    AtlasRegion skin;   // drawn through its cut mask
    AtlasRegion over;   // skin its hideFlags name: drawn over it by its alpha
};

/// Where a piece's texture goes by the blit set: laid out from the top-left of every slot it names -- its own,
/// the ones its hideFlags take and the skin its hide / skinFlags name -- it paints its own slots and the slots
/// its hideFlags take, less `lost` (taken by another piece); the skin its hideFlags name over the skin by its
/// alpha (`over`), and skin named by skinFlags alone through its cut mask (`skin`). All empty when the
/// set has no rectIndex, the type is not armour, or a `tw` x `th` texture at `scale` does not reach every rect.
PieceRegions piece_regions(const composite::BlitRectSet& set, uint8_t type, uint32_t hide_flags, uint32_t skin_flags,
                           const std::vector<size_t>& lost, int tw, int th, float scale);

/// `region` split into its rects that are the face slot's and the rest; both keep its anchor.
std::pair<AtlasRegion, AtlasRegion> split_face(const AtlasRegion& region, const composite::BlitRectSet& set);

/// `tex` with its alpha replaced by `mask`'s red (nearest-resized to `tex` when the sizes differ).
ImageRgba with_mask_alpha(const ImageRgba& tex, const ImageRgba& mask);

/// Atlas UV -> UV within the cropped block: u' = (u*1024 - x0) / (x1 - x0), same for v.
void remap_uv(float& u, float& v, const composite::BlitRect& rect);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ATLAS_H
