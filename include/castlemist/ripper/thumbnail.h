#ifndef CASTLEMIST_RIPPER_THUMBNAIL_H
#define CASTLEMIST_RIPPER_THUMBNAIL_H

// A tiny CPU renderer for picker thumbnails: a model drawn textured (its
// materials' diffuse textures, alpha-tested like the export's MASK), lit by one
// light, framed to fill the image. Enough to tell faces, hair styles and ears
// apart at a glance -- not a preview renderer.

#include <array>

#include "castlemist/extract/model_types.h"
#include "castlemist/ripper/atlas.h"

namespace castlemist::ripper {

struct ThumbnailView {
    /// Camera basis in model space: `right` and `up` span the image, the camera
    /// looks along `forward` (into the screen). Default: a GW2 character's front
    /// (models stand along -Z and face -Y).
    std::array<float, 3> right{1, 0, 0}, up{0, 0, -1}, forward{0, 1, 0};
    float margin = 0.06f;                          // of the image, each side
    std::array<uint8_t, 4> background{118, 118, 128, 255};  // mid grey: dark hair stays visible
};

/// Renders every mesh of `model` into a `size` x `size` RGBA image.
ImageRgba render_thumbnail(const ModelPreview& model, int size, const ThumbnailView& view = {});

} // namespace castlemist::ripper

#endif // CASTLEMIST_RIPPER_THUMBNAIL_H
