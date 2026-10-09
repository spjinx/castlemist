#ifndef CASTLEMIST_RIPPER_ARMOR_PREVIEW_H
#define CASTLEMIST_RIPPER_ARMOR_PREVIEW_H

// Character armor leaves its materials' `diffuse` and `normal` slots empty
// (fileId 0): the game fills them at runtime with the character's composited
// 1024x1024 armor atlas, so a lone armor model previews flat white wherever it
// samples that atlas. This rebuilds the piece's share of the atlas -- its
// Composite base texture with dyes baked in, blitted into its blit rect, and
// its normal map likewise -- so a model viewer can show the armor as the game
// paints it.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "castlemist/character/manifest.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/format/content_map.h"
#include "castlemist/native/gw2dat.h"

namespace castlemist::ripper {

/// The stand-in for the runtime atlas, ready to bind where a material's
/// `diffuse` / `normal` slot names fileId 0. Laid out as the full atlas when the
/// piece sits in blit rects (its UVs address the atlas directly); the piece's
/// own texture when it is self-textured.
struct ArmorPreviewTextures {
    ModelTextureCPU diffuse;
    std::optional<ModelTextureCPU> normal;
    int dyed_channels = 0;
    std::array<bool, 4> channels{};  // which dye channels the piece has a mask for
};

/// The dye materials, in the order a colour stores its shifts (and the API's).
inline constexpr const char* kDyeMaterials[4] = {"cloth", "leather", "metal", "fur"};
constexpr uint32_t kDyeRemover = 1;

/// One dye channel's colour: a dye (API colour id) and the material the
/// channel's shift is taken for. The skin decides the material in game; the
/// archive doesn't say which, so it is picked alongside the dye.
struct DyeChoice {
    uint32_t color_id = kDyeRemover;
    int material = 0;  // index into kDyeMaterials
};

/// The palette that lists every dye (content-map uid 82, 643 colours), or
/// nullptr while the content map isn't loaded.
const cmap::Palette* dye_palette();

/// What a dye looks like on @p material: its shift applied to the dye
/// palette's base colour.
std::array<uint8_t, 3> dye_swatch(const cmap::Palette& palette, const cmap::PaletteColor& color, int material);

/// Channel choices as the dyes build_armor_preview() bakes. A dye the content
/// map doesn't know (or no map) falls back to Dye Remover's cloth shift.
std::vector<character::ManifestDye> preview_dyes(const std::array<DyeChoice, 4>& choices);

/// The dyes a lone armor piece previews in: Dye Remover on every channel,
/// the colour the game shows on a channel nobody dyed.
std::vector<character::ManifestDye> default_preview_dyes();

/// Builds the atlas stand-in for the model at archive row @p mft_index when it
/// is a Composite armor mesh (any race/gender, base or overlap mesh); nullopt
/// when it isn't one, or when the model doesn't sample the atlas at all. The
/// Composite file is parsed once per archive and kept. Only safe on the thread
/// that owns @p dat.
std::optional<ArmorPreviewTextures> build_armor_preview(Gw2Dat& dat, uint32_t mft_index, const ModelPreview& model,
                                                        const std::vector<character::ManifestDye>& dyes);

/// Points every material slot that samples the runtime atlas (reconstruction
/// diffuse/normal and the game shader's samplers) at @p tex, appended to the
/// model's textures. Idempotent: a model already carrying them is left alone.
void apply_armor_preview(ModelPreview& model, const ArmorPreviewTextures& tex);

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_ARMOR_PREVIEW_H
