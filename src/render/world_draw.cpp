/// @file
/// @brief WorldView's instanced draw: mesh upload, cull into buckets, one
///        instance stream per frame.

#include "world_draw.h"

#include "detail/mesh_bake.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

namespace {

/// Per-instance stream entry (the fly view's MapInstGPU): the three world
/// columns of the row-vector world matrix, then rgb tint + alpha.
struct InstGPU {
    float r0[4], r1[4], r2[4], tint[4];
};
static_assert(sizeof(InstGPU) == 64, "the instance input layout hardcodes a 64-byte stride");

} // namespace

void place(InstCPU& in, const MeshGPU& m) {
    const Vec3 c = transformPoint({m.center[0], m.center[1], m.center[2]}, in.world);
    in.centerW[0] = c.x; in.centerW[1] = c.y; in.centerW[2] = c.z;
    in.radiusW = m.radius * in.scale;
    in.ready = std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z) && std::isfinite(in.radiusW);
}

bool finite16(const float w[16]) {
    for (int k = 0; k < 16; ++k)
        if (!std::isfinite(w[k])) return false;
    return true;
}

bool upload_mesh(ID3D11Device* dev, const PreparedModel& p, MeshGPU& out) {
    if (!dev || !p.ok || p.verts.empty() || p.indices.empty()) return false;
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(p.verts.size() * sizeof(MapVtx));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{p.verts.data(), 0, 0};
    MeshGPU m;
    if (FAILED(dev->CreateBuffer(&bd, &sd, &m.vb))) return false;
    bd.ByteWidth = static_cast<UINT>(p.indices.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = p.indices.data();
    if (FAILED(dev->CreateBuffer(&bd, &sd, &m.ib))) return false;
    m.lodRanges = p.lodRanges;
    std::memcpy(m.center, p.center, sizeof m.center);
    m.radius = p.radius;
    m.ok = true;
    out = std::move(m);
    return true;
}

void upload_collision(ID3D11Device* dev, const castlemist::world::Collision& C, std::vector<MeshGPU>& meshes,
                      std::vector<InstCPU>& insts) {
    meshes.clear();
    insts.clear();
    {
        std::vector<PreparedModel> prep(C.meshes.size());
        parallel_for(C.meshes.size(), [&](size_t i) { prep[i] = prepare_collision_mesh(C.meshes[i]); });
        meshes.resize(C.meshes.size());
        for (size_t i = 0; i < prep.size(); ++i) upload_mesh(dev, prep[i], meshes[i]);
    }
    for (const auto& ci : C.instances) {
        if (ci.mesh >= meshes.size() || !meshes[ci.mesh].ok || !finite16(ci.world)) continue;
        InstCPU in;
        in.mesh = ci.mesh;
        in.world = colmajor_to_rowvec(ci.world);
        in.scale = max_column_length(ci.world);
        place(in, meshes[ci.mesh]);
        if (in.ready) insts.push_back(in);
    }
}

bool InstancedBatch::ensure_capacity(ID3D11Device* dev, uint32_t need) {
    if (instVB_ && instCap_ >= need) return true;
    const uint32_t cap = std::max<uint32_t>(need + need / 2, 4096);
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = cap * sizeof(InstGPU);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ComPtr<ID3D11Buffer> nb;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &nb))) return false;
    instVB_ = nb;
    instCap_ = cap;
    return true;
}

int InstancedBatch::cull_into(const std::vector<InstCPU>& insts, const std::vector<MeshGPU>& meshes,
                              const float* tint, float alpha, const WorldCamera& cam, float farD,
                              const float planes[6][4]) {
    bucketOf_.assign(meshes.size() * 4, -1);
    int visible = 0;
    for (const InstCPU& in : insts) {
        if (!in.ready || in.mesh >= meshes.size()) continue;   // model not streamed in yet
        const MeshGPU& m = meshes[in.mesh];
        if (!m.ok) continue;
        const float dx = in.centerW[0] - cam.pos[0], dy = in.centerW[1] - cam.pos[1],
                    dz = in.centerW[2] - cam.pos[2];
        const float d2 = dx * dx + dy * dy + dz * dz, reach = farD + in.radiusW;
        if (d2 > reach * reach) continue;   // one compare; rejects most of a big map
        bool out = false;
        for (int p = 0; p < 6 && !out; ++p)
            out = planes[p][0] * in.centerW[0] + planes[p][1] * in.centerW[1] + planes[p][2] * in.centerW[2] +
                      planes[p][3] < -in.radiusW;
        if (out) continue;
        const int maxLod = static_cast<int>(m.lodRanges.size()) - 1;
        int lod = 0;
        if (maxLod > 0) {
            const float sr = in.radiusW / std::sqrt(std::max(d2, 1e-4f));   // ~ tan(angular radius)
            lod = (sr > 0.20f) ? 0 : (sr > 0.06f ? 1 : (sr > 0.02f ? 2 : 3));
            lod = std::min(lod, maxLod);
        }
        const size_t key = static_cast<size_t>(in.mesh) * 4 + static_cast<size_t>(lod);
        if (bucketOf_[key] < 0) {
            bucketOf_[key] = static_cast<int>(buckets_.size());
            buckets_.push_back({&m, lod, tint, alpha, {}});
        }
        buckets_[static_cast<size_t>(bucketOf_[key])].insts.push_back(&in);
        ++visible;
    }
    return visible;
}

int InstancedBatch::draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris) {
    uint32_t total = 0;
    for (const Bucket& b : buckets_) total += static_cast<uint32_t>(b.insts.size());
    if (total == 0 || !ensure_capacity(dev, total)) return 0;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(instVB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return 0;
    auto* dst = static_cast<InstGPU*>(ms.pData);
    std::vector<uint32_t> first(buckets_.size());
    uint32_t off = 0;
    for (size_t b = 0; b < buckets_.size(); ++b) {
        first[b] = off;
        for (const InstCPU* in : buckets_[b].insts) {
            // Column c of the row-vector W (W[r][c] = m[r*4+c]), so that
            // world_c = dot((p,1), column c) -- the fly view's pack_inst.
            const float* m = in->world.m;
            InstGPU& o = dst[off++];
            float* cols[3] = {o.r0, o.r1, o.r2};
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 4; ++r) cols[c][r] = m[r * 4 + c];
            for (int k = 0; k < 3; ++k) o.tint[k] = buckets_[b].tint[k];
            o.tint[3] = buckets_[b].alpha;
        }
    }
    ctx->Unmap(instVB_.Get(), 0);

    ctx->IASetInputLayout(pl.il);
    ctx->VSSetShader(pl.vs, nullptr, 0);
    ctx->PSSetShader(pl.ps, nullptr, 0);
    bool blending = false;
    int draws = 0;
    for (size_t b = 0; b < buckets_.size(); ++b) {
        const Bucket& bk = buckets_[b];
        if (bk.lod >= static_cast<int>(bk.mesh->lodRanges.size())) continue;
        const auto range = bk.mesh->lodRanges[static_cast<size_t>(bk.lod)];
        if (range.second == 0) continue;
        if (bk.alpha < 1.0f && !blending) {
            ctx->OMSetBlendState(pl.bsAlpha, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(pl.dssNoWrite, 0);
            blending = true;
        }
        ID3D11Buffer* vbs[] = {bk.mesh->vb.Get(), instVB_.Get()};
        const UINT strides[] = {sizeof(MapVtx), sizeof(InstGPU)};
        const UINT offsets[] = {0, first[b] * static_cast<UINT>(sizeof(InstGPU))};
        ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
        ctx->IASetIndexBuffer(bk.mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexedInstanced(range.second, static_cast<UINT>(bk.insts.size()), range.first, 0, 0);
        ++draws;
        tris += static_cast<size_t>(range.second / 3) * bk.insts.size();
    }
    if (blending) {
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(pl.dss, 0);
    }
    return draws;
}

} // namespace castlemist::render
