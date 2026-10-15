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

/// world_shaders::kPropMat's MatCB (register b1).
struct MatCB {
    float uvOffset[2]; float uvSet; float alphaMode;
    float textured;    float _pad[3];
};
static_assert(sizeof(MatCB) == kMatCBBytes, "the owner creates MatCB's buffer with kMatCBBytes");

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
    const bool mat = !p.matVerts.empty();
    const size_t nv = mat ? p.matVerts.size() : p.verts.size();
    if (!dev || !p.ok || nv == 0 || p.indices.empty()) return false;
    if (!p.skinVerts.empty() && p.skinVerts.size() != nv) return false;
    MeshGPU m;
    m.stride = mat ? static_cast<UINT>(sizeof(MatVtx)) : static_cast<UINT>(sizeof(MapVtx));
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(nv * m.stride);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{mat ? static_cast<const void*>(p.matVerts.data()) : p.verts.data(), 0, 0};
    if (FAILED(dev->CreateBuffer(&bd, &sd, &m.vb))) return false;
    if (!p.skinVerts.empty()) {
        bd.ByteWidth = static_cast<UINT>(p.skinVerts.size() * sizeof(SkinVtx));
        sd.pSysMem = p.skinVerts.data();
        if (FAILED(dev->CreateBuffer(&bd, &sd, &m.vbSkin))) return false;
    }
    bd.ByteWidth = static_cast<UINT>(p.indices.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = p.indices.data();
    if (FAILED(dev->CreateBuffer(&bd, &sd, &m.ib))) return false;
    m.lodRanges = p.lodRanges;
    m.subRanges = p.subRanges;
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
            buckets_.push_back({&m, lod, tint, alpha, {}, d2});
        }
        Bucket& bk = buckets_[static_cast<size_t>(bucketOf_[key])];
        bk.insts.push_back(&in);
        bk.nearD2 = std::min(bk.nearD2, d2);
        ++visible;
    }
    return visible;
}

bool InstancedBatch::upload(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    first_.clear();
    uint32_t total = 0;
    for (const Bucket& b : buckets_) total += static_cast<uint32_t>(b.insts.size());
    if (total == 0 || !ensure_capacity(dev, total)) return false;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(instVB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
    auto* dst = static_cast<InstGPU*>(ms.pData);
    first_.assign(buckets_.size(), 0);
    uint32_t off = 0;
    for (size_t b = 0; b < buckets_.size(); ++b) {
        first_[b] = off;
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
    return true;
}

void InstancedBatch::bind_streams(ID3D11DeviceContext* ctx, size_t b) const {
    const Bucket& bk = buckets_[b];
    ID3D11Buffer* vbs[] = {bk.mesh->vb.Get(), instVB_.Get()};
    const UINT strides[] = {bk.mesh->stride, sizeof(InstGPU)};
    const UINT offsets[] = {0, first_[b] * static_cast<UINT>(sizeof(InstGPU))};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->IASetIndexBuffer(bk.mesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
}

int InstancedBatch::draw(ID3D11Device* dev, ID3D11DeviceContext* ctx, const InstPipeline& pl, size_t& tris) {
    if (!upload(dev, ctx)) return 0;
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
        bind_streams(ctx, b);
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

int InstancedBatch::draw_materials(ID3D11DeviceContext* ctx, const PropMatPipeline& pl, int pass, size_t& tris,
                                   int& untextured) {
    if (buckets_.empty() || first_.size() != buckets_.size()) return 0;   // upload() failed or did not run
    // Sub-range @p r's diffuse view and alpha mode; null when it draws in the
    // AO look (no diffuse, UV2+, texture pending or failed).
    auto resolve = [&](const MeshGPU& m, const SubRange& r, WorldAlpha& a) -> ID3D11ShaderResourceView* {
        if (r.count == 0 || r.material >= m.materials.size()) return nullptr;
        const PropMatGPU& pm = m.materials[r.material];
        if (!pm.m.textured) return nullptr;
        ID3D11ShaderResourceView* srv = pl.textures->srv(pm.m.diffuseFileId, pl.srgb);
        if (!srv) return nullptr;
        const TextureCache::Info inf = pl.textures->info(pm.m.diffuseFileId);
        a = pm.alphaBy[(inf.hasCutout ? 1 : 0) | (inf.looksLikeEffect ? 2 : 0)];
        return srv;
    };
    auto lod_ok = [](const Bucket& bk) {
        return bk.lod >= 0 && bk.lod < static_cast<int>(bk.mesh->lodRanges.size()) &&
               bk.mesh->lodRanges[static_cast<size_t>(bk.lod)].second > 0;
    };
    auto subs_of = [](const Bucket& bk) -> const std::vector<SubRange>* {
        const auto lod = static_cast<size_t>(bk.lod);
        return lod < bk.mesh->subRanges.size() ? &bk.mesh->subRanges[lod] : nullptr;
    };
    int draws = 0;
    auto issue = [&](const Bucket& bk, uint32_t first, uint32_t count) {
        ctx->DrawIndexedInstanced(count, static_cast<UINT>(bk.insts.size()), first, 0, 0);
        ++draws;
        tris += static_cast<size_t>(count / 3) * bk.insts.size();
    };
    bool boundMat = false;
    auto use_mat = [&] {
        if (boundMat) return;
        ctx->IASetInputLayout(pl.il);
        ctx->VSSetShader(pl.vs, nullptr, 0);
        ctx->PSSetShader(pl.ps, nullptr, 0);
        ctx->VSSetConstantBuffers(1, 1, &pl.cbMat);
        ctx->PSSetConstantBuffers(1, 1, &pl.cbMat);
        ctx->PSSetSamplers(0, 1, &pl.sampler);
        boundMat = true;
    };
    // One MatCB write, one SRV bind and one draw per textured sub-range.
    auto draw_textured = [&](const Bucket& bk, const SubRange& r, ID3D11ShaderResourceView* srv, WorldAlpha a) {
        const PropMatGPU& pm = bk.mesh->materials[r.material];
        MatCB c{};
        c.uvSet = static_cast<float>(pm.m.uvSet);
        c.alphaMode = static_cast<float>(static_cast<int>(a));
        c.textured = 1.0f;
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(pl.cbMat, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
        std::memcpy(ms.pData, &c, sizeof c);
        ctx->Unmap(pl.cbMat, 0);
        ctx->PSSetShaderResources(0, 1, &srv);
        issue(bk, r.first, r.count);
    };

    if (pass == 1) {
        // The AO look first, with kInstanced exactly as Geometry draws it.
        ctx->IASetInputLayout(pl.ao.il);
        ctx->VSSetShader(pl.ao.vs, nullptr, 0);
        ctx->PSSetShader(pl.ao.ps, nullptr, 0);
        for (size_t b = 0; b < buckets_.size(); ++b) {
            const Bucket& bk = buckets_[b];
            if (!lod_ok(bk)) continue;
            const std::vector<SubRange>* subs = subs_of(bk);
            bool bound = false;
            if (!subs || subs->empty()) {   // no per-submesh ranges: the whole LOD
                const auto range = bk.mesh->lodRanges[static_cast<size_t>(bk.lod)];
                bind_streams(ctx, b);
                issue(bk, range.first, range.second);
                ++untextured;
                continue;
            }
            for (const SubRange& r : *subs) {
                WorldAlpha a = WorldAlpha::Opaque;
                if (r.count == 0 || resolve(*bk.mesh, r, a)) continue;
                if (!bound) { bind_streams(ctx, b); bound = true; }
                issue(bk, r.first, r.count);
                ++untextured;
            }
        }
        // Then the textured opaque and clip sub-ranges.
        for (size_t b = 0; b < buckets_.size(); ++b) {
            const Bucket& bk = buckets_[b];
            const std::vector<SubRange>* subs = subs_of(bk);
            if (!lod_ok(bk) || !subs) continue;
            bool bound = false;
            for (const SubRange& r : *subs) {
                WorldAlpha a = WorldAlpha::Opaque;
                ID3D11ShaderResourceView* srv = resolve(*bk.mesh, r, a);
                if (!srv || (a != WorldAlpha::Opaque && a != WorldAlpha::Clip)) continue;
                use_mat();
                if (!bound) { bind_streams(ctx, b); bound = true; }
                draw_textured(bk, r, srv, a);
            }
        }
    } else if (pass == 2) {
        auto blended = [&](const Bucket& bk) {
            const std::vector<SubRange>* subs = subs_of(bk);
            if (!lod_ok(bk) || !subs) return false;
            for (const SubRange& r : *subs) {
                WorldAlpha a = WorldAlpha::Opaque;
                if (resolve(*bk.mesh, r, a) && (a == WorldAlpha::Blend || a == WorldAlpha::Additive)) return true;
            }
            return false;
        };
        std::vector<size_t> which;
        std::vector<float> dist;
        for (size_t b = 0; b < buckets_.size(); ++b)
            if (blended(buckets_[b])) {
                which.push_back(b);
                dist.push_back(std::sqrt(buckets_[b].nearD2));
            }
        if (!which.empty()) {
            use_mat();
            ctx->OMSetDepthStencilState(pl.ao.dssNoWrite, 0);
            int curBlend = -1;   // 0 SrcAlpha / InvSrcAlpha, 1 SrcAlpha / One
            for (size_t k : blend_back_to_front(dist)) {
                const size_t b = which[k];
                const Bucket& bk = buckets_[b];
                bind_streams(ctx, b);
                for (const SubRange& r : *subs_of(bk)) {
                    WorldAlpha a = WorldAlpha::Opaque;
                    ID3D11ShaderResourceView* srv = resolve(*bk.mesh, r, a);
                    if (!srv || (a != WorldAlpha::Blend && a != WorldAlpha::Additive)) continue;
                    const int want = a == WorldAlpha::Additive ? 1 : 0;
                    if (want != curBlend) {
                        ctx->OMSetBlendState(want ? pl.bsAdditive : pl.ao.bsAlpha, nullptr, 0xffffffff);
                        curBlend = want;
                    }
                    draw_textured(bk, r, srv, a);
                }
            }
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(pl.ao.dss, 0);
        }
    }
    if (boundMat) {
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
    }
    return draws;
}

} // namespace castlemist::render
