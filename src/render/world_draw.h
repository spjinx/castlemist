/// @file
/// @brief WorldView's instanced draw (props, collision, water): the uploaded
///        meshes, the placed instances, the per-frame cull into (mesh, LOD)
///        buckets and the one-Map-per-frame instance stream.
///
/// Follows the fly view's render_fly (map_fly.cpp): distance + frustum sphere
/// cull, LOD by screen size, buckets by (mesh, LOD), one dynamic instance
/// buffer, one DrawIndexedInstanced per bucket.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include "castlemist/render/world_view.h"
#include "castlemist/world/world_scene.h"

#include "detail/math.h"
#include "world_materials.h"
#include "world_props.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace castlemist::render {

/// One prop material as the material pass draws it: prop_materials()'s
/// diffuse plus its alpha mode for each combination of the texture's
/// (hasCutout, looksLikeEffect, darkCoverage) flags, so the draw resolves the
/// mode from the texture once it is Ready without calling the export's rules
/// per frame.
struct PropMatGPU {
    PropMaterial m;
    WorldAlpha alphaBy[8] = {};   ///< [hasCutout | looksLikeEffect << 1 | darkCoverage << 2]
};

/// alphaBy's index for a texture's flags.
inline int prop_alpha_index(const TextureCache::Info& i) {
    return (i.hasCutout ? 1 : 0) | (i.looksLikeEffect ? 2 : 0) | (i.darkCoverage ? 4 : 0);
}

/// One uploaded instanced mesh (a prop model, a collision mesh or the water quad).
struct MeshGPU {
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb, ib;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vbSkin;   ///< SkinVtx stream; null unless prepared withSkin
    UINT stride = sizeof(MapVtx);   ///< vb's stride: sizeof(MatVtx) for a prop model, sizeof(MapVtx) otherwise
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;   ///< (first index, count)
    std::vector<std::vector<SubRange>> subRanges;           ///< per LOD (prop models only)
    std::vector<PropMatGPU> materials;                      ///< by SubRange::material (prop models only)
    float center[3] = {0, 0, 0};   ///< model-space sphere
    float radius = 0;
    bool ok = false;
};

/// One placed instance. `ready` once its mesh is uploaded (props stream in).
struct InstCPU {
    uint32_t mesh = 0;
    uint32_t source = 0;   ///< index in the scene's list (props: what pick() returns)
    Mat4 world{};          ///< row-vector (p' = p W), from colmajor_to_rowvec
    float scale = 1;       ///< max_column_length of the instance matrix
    float centerW[3] = {0, 0, 0};
    float radiusW = 0;
    bool ready = false;
};

/// Places @p in's world sphere from its mesh: centre through the matrix,
/// radius times the largest column length.
void place(InstCPU& in, const MeshGPU& m);

/// All 16 floats finite.
bool finite16(const float w[16]);

/// Immutable vertex + index buffers for one prepared mesh: `matVerts`
/// (stride 32) when non-empty, else `verts` (stride 16), plus `skinVerts`
/// when present. False (and @p out untouched) for a not-ok or empty mesh or
/// a failed buffer.
bool upload_mesh(ID3D11Device* dev, const PreparedModel& p, MeshGPU& out);

/// Prepares every collision mesh across cores and uploads it, then places
/// each instance whose mesh uploaded and whose matrix is finite.
void upload_collision(ID3D11Device* dev, const castlemist::world::Collision& c, std::vector<MeshGPU>& meshes,
                      std::vector<InstCPU>& insts);

/// The instanced pipeline objects draw() binds (owned by WorldView::Impl).
struct InstPipeline {
    ID3D11InputLayout* il;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11BlendState* bsAlpha;            ///< translucent buckets
    ID3D11DepthStencilState* dssNoWrite;  ///< translucent buckets: tested, not written
    ID3D11DepthStencilState* dss;         ///< restored after the translucent buckets
};

/// Size of world_shaders::kPropMat's MatCB (register b1), for the owner's buffer.
inline constexpr UINT kMatCBBytes = 32;

/// What draw_materials() binds: kPropMat for textured sub-ranges, the
/// InstPipeline (kInstanced, the AO look) for the rest.
struct PropMatPipeline {
    InstPipeline ao;
    ID3D11InputLayout* il;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11Buffer* cbMat;              ///< MatCB, b1, written once per textured sub-range
    ID3D11SamplerState* sampler;      ///< s0, wraps
    /// Pass 2's blend state per WorldAlpha (null for Opaque, Clip, Unmapped).
    ID3D11BlendState* const* blendOf;
    const TextureCache* textures;
    bool srgb;                        ///< WorldMaterialSettings::srgbColourTextures
    double time = 0;                  ///< WorldView::time(): the material clock
    float scrollUnitSeconds = 1;      ///< WorldMaterialSettings::uvScrollUnitSeconds
};

/// One frame's instanced draw: cull_into() for each layer (opaque first,
/// translucent last), then draw(); or, for the props' material pass,
/// upload() then draw_materials() for pass 1 and later pass 2.
class InstancedBatch {
public:
    /// Drops last frame's buckets.
    void clear() {
        buckets_.clear();
        first_.clear();
    }
    /// Culls @p insts (distance, then frustum sphere), picks a LOD by screen
    /// size as render_fly does, and appends them to this frame's buckets.
    /// Returns how many survived.
    int cull_into(const std::vector<InstCPU>& insts, const std::vector<MeshGPU>& meshes, const float* tint,
                  float alpha, const WorldCamera& cam, float farD, const float planes[6][4]);
    /// Uploads every bucketed instance in one Map and issues one
    /// DrawIndexedInstanced per bucket; translucent buckets switch to alpha
    /// blending without depth writes. Adds to @p tris; returns draw calls.
    int draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris);
    /// Writes every bucketed instance in one Map (draw() calls it; the
    /// material pass calls it once before its two passes). False when there
    /// is nothing to draw or the buffer could not be written.
    bool upload(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    /// The props' material pass over the uploaded buckets (spec
    /// PropMaterialPass): one DrawIndexedInstanced per SubRange of the
    /// bucket's LOD. @p pass 1 draws the Opaque and Clip sub-ranges and those
    /// in the AO look (no diffuse, UV2+, texture pending or failed, or an
    /// Unmapped blend; counted in @p untextured), AO ones first; @p pass 2 the
    /// blended ones (each with its WorldAlpha's blend state),
    /// depth writes off, buckets ordered by blend_back_to_front on their
    /// nearest instance (no per-instance sort: an approximation). A LOD with
    /// no sub-ranges draws its whole range in the AO look. Leaves the blend
    /// and depth state at opaque. Adds to @p tris; returns draw calls.
    int draw_materials(ID3D11DeviceContext* ctx, const PropMatPipeline& pl, int pass, size_t& tris,
                       int& untextured);

private:
    struct Bucket {
        const MeshGPU* mesh;
        int lod;
        const float* tint;
        float alpha;
        std::vector<const InstCPU*> insts;
        float nearD2;   ///< squared distance of the nearest instance's centre
    };
    bool ensure_capacity(ID3D11Device* dev, uint32_t need);
    void bind_streams(ID3D11DeviceContext* ctx, size_t b) const;

    std::vector<Bucket> buckets_;   ///< rebuilt every frame
    std::vector<uint32_t> first_;   ///< per bucket: its first instance in instVB_ (upload())
    std::vector<int> bucketOf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> instVB_;
    uint32_t instCap_ = 0;
};

} // namespace castlemist::render
