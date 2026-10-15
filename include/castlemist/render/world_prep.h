/// @file
/// @brief The off-UI-thread half of WorldView's model upload: a streamed
///        model's preview and its prepared (flattened, AO-baked) mesh.
///
/// No Direct3D here: the map window's load workers include this, call
/// prepare_world_model_shared, and post the result; WorldView::add_models
/// then only creates the GPU buffers.
/// @ingroup render
#pragma once

#include <cstdint>
#include <memory>
#include <utility>

struct ModelPreview;   // castlemist/extract/model_types.h

namespace castlemist::render {

struct PreparedModel;   // src/render/world_props.h (internal)

/// @brief One streamed model, ready for WorldView::add_models.
///
/// Not an aggregate on purpose: with a constructor that needs all three, a
/// braced `{index, preview}` pair cannot convert to it, so the pair overload of
/// WorldView::add_models stays unambiguous for `add_models({{0u, cube}})`.
struct WorldModelLoad {
    WorldModelLoad() = default;
    WorldModelLoad(uint32_t m, std::shared_ptr<const ModelPreview> p, std::shared_ptr<const PreparedModel> q)
        : model(m), preview(std::move(p)), prep(std::move(q)) {}
    uint32_t model = 0;                            ///< index into WorldScene::models
    std::shared_ptr<const ModelPreview> preview;   ///< picking, materials, joints, clips; null = failed
    std::shared_ptr<const PreparedModel> prep;     ///< null or !ok = failed
};

/// @brief prepare_world_model (flatten, AO bake, MatVtx, per-submesh ranges,
///        SkinVtx when @p withSkin) on a shared, immutable result. Touches no
///        Direct3D and no shared mutable state: safe on any thread.
std::shared_ptr<const PreparedModel> prepare_world_model_shared(const ModelPreview& m, bool withSkin);

} // namespace castlemist::render
