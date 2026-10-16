/// @file
/// @brief WorldView's instanced draw (props, collision, water): the uploaded
///        meshes, the placed instances, the per-frame cull into (mesh, LOD)
///        buckets and the one-Map-per-frame instance stream.
///
/// Follows the main window's old mesh-only map view: distance + frustum sphere
/// cull, LOD by screen size, buckets by (mesh, LOD), one dynamic instance
/// buffer, one DrawIndexedInstanced per bucket.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include "castlemist/render/world_foliage_grid.h"
#include "castlemist/render/world_view.h"
#include "castlemist/world/world_scene.h"

#include "detail/math.h"
#include "world_materials.h"
#include "world_props.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <unordered_map>
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
    uint64_t animSequence = 0;   ///< props: PropInstance::animSequence (0 = none)
    /// Props: the clip of its model it plays (spec SkinnedPropPass), set by
    /// add_models when animSequence resolves and the model has a skin
    /// stream; -1 = static (drawn in bind pose).
    int clip = -1;
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
/// Size of world_shaders::kPropSkinned's SkinCB (register b2): the bucket's
/// palette base and bone count.
inline constexpr UINT kSkinCBBytes = 16;

/// Which props a cull takes: All ignores InstCPU::clip (Geometry, collision,
/// water: everything static); Static only clip < 0; Skinned only clip >= 0,
/// bucketed by (mesh, clip, LOD).
enum class CullSkin { All, Static, Skinned };

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
    /// Skinned batches only: SkinCB (b2, VS), written per bucket with its
    /// palette base; the skin stream binds to slot 2. Null for static batches.
    ID3D11Buffer* cbSkin = nullptr;
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
    /// size, and appends them to this frame's buckets. With @p skip
    /// (index-aligned with @p insts), an instance whose entry is non-zero is
    /// left out (Shader shading draws it with the game's shaders).
    /// Returns how many survived.
    int cull_into(const std::vector<InstCPU>& insts, const std::vector<MeshGPU>& meshes, const float* tint,
                  float alpha, const WorldCamera& cam, float farD, const float planes[6][4],
                  CullSkin which = CullSkin::All, const std::vector<uint8_t>* skip = nullptr);
    /// Skinned batches: the distinct (mesh index, clip) of this frame's
    /// buckets, in first-seen order (one pose each, spec SkinnedPropPass).
    std::vector<std::pair<uint32_t, int>> palette_keys();
    /// Skinned batches: palette_keys()[k]'s palette starts at bone @p base[k]
    /// of the frame's palette buffer and holds @p count[k] bones.
    void set_palettes(const std::vector<uint32_t>& base, const std::vector<uint32_t>& count);
    /// Uploads every bucketed instance in one Map and issues one
    /// DrawIndexedInstanced per bucket; translucent buckets switch to alpha
    /// blending without depth writes. Adds to @p tris; returns draw calls.
    int draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris);
    /// draw()'s second half: the DrawIndexedInstanced per bucket over what
    /// upload() wrote (nothing when it did not run or failed).
    int draw_uploaded(ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris);
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

protected:
    struct Bucket {
        const MeshGPU* mesh;
        int lod;
        const float* tint;   ///< props: the AO look's albedo; null for foliage
        float alpha;
        std::vector<const Mat4*> worlds;   ///< each instance's row-vector world
        std::vector<float> fade;           ///< FoliageBatch only: per instance, aligned with worlds
        float nearD2;   ///< squared distance of the nearest instance's centre
        int clip = -1;               ///< skinned buckets: the clip; -1 static
        uint32_t meshIndex = 0;
        int paletteKey = -1;         ///< index into palette_keys()
        uint32_t paletteBase = 0, boneCount = 0;
    };
    bool ensure_capacity(ID3D11Device* dev, uint32_t need);
    /// Slots 0 and 1; for a skinned bucket with @p cbSkin also the skin
    /// stream (slot 2) and its SkinCB.
    void bind_streams(ID3D11DeviceContext* ctx, size_t b, ID3D11Buffer* cbSkin = nullptr) const;

    /// The instance stream's stride (InstGPU and FoliageInstGPU are both 64 bytes).
    static constexpr UINT kInstStride = 64;

    std::vector<Bucket> buckets_;   ///< rebuilt every frame
    std::vector<uint32_t> first_;   ///< per bucket: its first instance in instVB_ (upload())
    std::vector<int> bucketOf_;
    std::unordered_map<uint64_t, int> skinBucketOf_;   ///< (mesh, clip, LOD) -> bucket, CullSkin::Skinned
    Microsoft::WRL::ComPtr<ID3D11Buffer> instVB_;
    uint32_t instCap_ = 0;
};

/// The foliage pass's instance stream entry (spec D6): the three world
/// columns of the row-vector world matrix, then the fade (x; yzw unused).
struct FoliageInstGPU {
    float r0[4], r1[4], r2[4];
    float fade, _pad[3];
};
static_assert(sizeof(FoliageInstGPU) == 64, "the foliage input layout hardcodes a 64-byte stride");

/// The zone instances' draw (spec D6 "Draw"): InstancedBatch's buckets by
/// (mesh, LOD), its upload-once / draw-per-bucket pattern and its material
/// pass (draw_materials: cutout and blend as the props' alpha modes), over
/// its own 64-byte stream (FoliageInstGPU) that carries a fade instead of a
/// tint. The pixel shaders discard where fade < bayer4x4(pixel).
class FoliageBatch : public InstancedBatch {
public:
    /// Buckets the survivors of foliage_cull by (mesh, LOD), the LOD by
    /// screen size as the props' rule. @p worlds and @p meshOf are per zone
    /// instance (the view's CPU copy); @p centre and radius come from the
    /// grid's spheres. An instance whose mesh is not uploaded is skipped.
    /// Returns how many were bucketed.
    int cull_into(const std::vector<FoliageVisible>& vis, const FoliageGrid& g, const std::vector<Mat4>& worlds,
                  const std::vector<uint32_t>& meshOf, const std::vector<MeshGPU>& meshes, const WorldCamera& cam);
    /// Writes every bucketed instance (FoliageInstGPU) in one Map. False with
    /// nothing to draw or on failure.
    bool upload(ID3D11Device* dev, ID3D11DeviceContext* ctx);
    /// Geometry shading: upload() then one DrawIndexedInstanced per bucket
    /// with @p pl (the foliage AO pipeline). Adds to @p tris; returns draws.
    int draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris);
};

} // namespace castlemist::render
