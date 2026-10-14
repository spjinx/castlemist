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
#include "world_props.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace castlemist::render {

/// One uploaded instanced mesh (a prop model, a collision mesh or the water quad).
struct MeshGPU {
    Microsoft::WRL::ComPtr<ID3D11Buffer> vb, ib;
    std::vector<std::pair<uint32_t, uint32_t>> lodRanges;   ///< (first index, count)
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

/// Immutable vertex + index buffers for one prepared mesh. False (and @p out
/// untouched) for a not-ok or empty mesh or a failed buffer.
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

/// One frame's instanced draw: cull_into() for each layer (opaque first,
/// translucent last), then draw().
class InstancedBatch {
public:
    /// Drops last frame's buckets.
    void clear() { buckets_.clear(); }
    /// Culls @p insts (distance, then frustum sphere), picks a LOD by screen
    /// size as render_fly does, and appends them to this frame's buckets.
    /// Returns how many survived.
    int cull_into(const std::vector<InstCPU>& insts, const std::vector<MeshGPU>& meshes, const float* tint,
                  float alpha, const WorldCamera& cam, float farD, const float planes[6][4]);
    /// Uploads every bucketed instance in one Map and issues one
    /// DrawIndexedInstanced per bucket; translucent buckets switch to alpha
    /// blending without depth writes. Adds to @p tris; returns draw calls.
    int draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris);

private:
    struct Bucket {
        const MeshGPU* mesh;
        int lod;
        const float* tint;
        float alpha;
        std::vector<const InstCPU*> insts;
    };
    bool ensure_capacity(ID3D11Device* dev, uint32_t need);

    std::vector<Bucket> buckets_;   ///< rebuilt every frame
    std::vector<int> bucketOf_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> instVB_;
    uint32_t instCap_ = 0;
};

} // namespace castlemist::render
