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
std::optional<BakedTextures> bake_part(Gw2Dat& dat, const composite::CompositeFileData& fd,
                                       const std::vector<character::ManifestDye>& dyes);

/// The mesh's material is the body skin ("Skin", "SylvariSkin1", ...).
bool is_skin_mesh(const ModelPreview& m, const ModelMeshCPU& mesh);

/// The mesh's UVs wrapped into [0,1] (mirrored halves sit at u-1).
std::vector<std::pair<float, float>> wrapped_uvs(const ModelMeshCPU& mesh);

} // namespace castlemist::ripper::detail

#endif // CASTLEMIST_RIPPER_INTERNAL_H
