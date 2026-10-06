#ifndef CASTLEMIST_RIPPER_INTERNAL_H
#define CASTLEMIST_RIPPER_INTERNAL_H

// Building blocks shared by piece export and character assembly.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "castlemist/character/manifest.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/format/composite.h"
#include "castlemist/native/gw2dat.h"
#include "castlemist/ripper/atlas.h"
#include "castlemist/ripper/dye.h"

namespace castlemist::ripper::detail {

/// A model by fileId through the extract layer (external skeleton resolved).
std::optional<ModelPreview> load_model(Gw2Dat& dat, uint32_t file_id);

ImageRgba to_image(const ModelTextureCPU& t);

/// Appends a baked texture to the model; returns its index.
int add_texture(ModelPreview& m, const ImageRgba& im, uint32_t file_id, bool is_normal);

/// A composite entry's textures, dyes baked in (dye slot i -> mask i).
struct BakedTextures {
    ModelTextureCPU base;
    std::optional<ModelTextureCPU> normal;
    int dyed = 0, undyed = 0;
    float scale = 2.0f;         // atlas pixels per base texel (2 = reduced copy, 1 = full-resolution copy)
    float normal_scale = 2.0f;  // the same for the normal map
};
// `rest`: the colour for texels no dye mask covers (a character's skin colour
// on the bare body, face, ears and scalp); none leaves them as authored.
std::optional<BakedTextures> bake_part(Gw2Dat& dat, const composite::CompositeFileData& fd,
                                       const std::vector<character::ManifestDye>& dyes,
                                       const std::optional<ColorMatrix>& rest = std::nullopt,
                                       bool preview = false);  // reduced textures, no normal map (thumbnails)

/// The mesh's material is the body skin ("Skin", "SylvariSkin1", ...).
// GW2 model space is left-handed (Z down, X/Y east/north); glTF is
// right-handed, so a rotation alone exports a mirror image ("Left" bones on the
// character's right). Reflects X in place: vertices, normals, tangent frames,
// blend-shape deltas, triangle winding, and the skeleton's bind data (S*M*S).
// Animation clips are dropped (not mirrored).
void mirror_x(ModelPreview& model);

bool is_skin_mesh(const ModelPreview& m, const ModelMeshCPU& mesh);

/// Marks (value 1) every atlas pixel (w x h) the meshes' UV triangles cover,
/// UVs wrapped; skin meshes (sampling the body's region) skipped unless asked for.
std::vector<uint8_t> uv_coverage(const ModelPreview& m, int w, int h, bool skin_meshes);

/// Where a part's texture starts in the atlas. Usually at its rects' corner;
/// a texture laid out for a larger block (Baggy Cargo Pants: 512 wide for rects
/// from x 640) starts further back. Picks, among the corner, the corner moved
/// back inside the atlas, and the atlas origin, the placement whose painted
/// texels cover clearly more of what the piece samples (`coverage`, from
/// uv_coverage); the corner when none does.
void place_texture(AtlasRegion& region, const ImageRgba& tex, float scale, const std::vector<uint8_t>& coverage);

/// The undergarments' dyes: every channel Dye Remover (colour 1) on cloth,
/// the colour the game shows on a channel nobody dyed.
std::vector<character::ManifestDye> undergarment_dyes();

/// The mesh's UVs wrapped into [0,1] (mirrored halves sit at u-1).
std::vector<std::pair<float, float>> wrapped_uvs(const ModelMeshCPU& mesh);

} // namespace castlemist::ripper::detail

#endif // CASTLEMIST_RIPPER_INTERNAL_H
