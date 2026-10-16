/// @file
/// @brief WorldView's Shader shading: the game's material shaders on the
///        Map window's device (see world_game.h).

#include "world_game.h"

#include "detail/shaders.h"   // kNormalGBuffer, kLightAccum (the main window's light pre-pass)
#include "detail/state.h"     // kFarSceneDepth, CB

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace castlemist::render {

using Microsoft::WRL::ComPtr;

namespace {

/// FNV-1a over the bytes, then the size: the program cache's key.
uint64_t hash_bytes(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h ^ (static_cast<uint64_t>(b.size()) * 0x9E3779B97F4A7C15ull);
}

ComPtr<ID3D11ShaderResourceView> solid_srv(ID3D11Device* dev, DXGI_FORMAT fmt, const void* texel, UINT pitch,
                                           bool cube) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = 1;
    td.ArraySize = cube ? 6 : 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    D3D11_SUBRESOURCE_DATA sd[6];
    for (auto& d : sd) d = {texel, pitch, pitch};
    ComPtr<ID3D11Texture2D> t;
    ComPtr<ID3D11ShaderResourceView> v;
    D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
    vd.Format = fmt;
    if (cube) {
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        vd.TextureCube.MipLevels = 1;
    } else {
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        vd.Texture2D.MipLevels = 1;
    }
    if (FAILED(dev->CreateTexture2D(&td, sd, &t)) || FAILED(dev->CreateShaderResourceView(t.Get(), &vd, &v)))
        return nullptr;
    return v;
}

/// The bgfx uniform layout of a row-vector Mat4 (scene.cpp's pM): transposed.
void put_matrix(float out[16], const Mat4& m) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = m.m[j * 4 + i];
}

} // namespace

GameDyn game_dyn_of(const std::string& n) {
    static const char* const kNames[] = {"World", "WorldViewProjection", "WorldView", "ViewProjection", "View",
                                         "CameraPosition", "ScreenDims", "Time"};
    for (int k = 0; k < static_cast<int>(GameDyn::Count); ++k)
        if (n == kNames[k]) return static_cast<GameDyn>(k);
    return GameDyn::Count;
}

bool WorldGamePass::create(ID3D11Device* dev) {
    // The stand-ins build_one_game_mat binds: GW2_GLOBALLIT's default grey
    // (110) for the light buffer and other globals, a grey (64) env cube, a
    // white shadow map (fully lit) and a far scene depth.
    const uint8_t grey[4] = {110, 110, 110, 255}, cube[4] = {64, 64, 64, 255}, white[4] = {255, 255, 255, 255};
    const float farZ = kFarSceneDepth;
    grey_ = solid_srv(dev, DXGI_FORMAT_R8G8B8A8_UNORM, grey, 4, false);
    envCube_ = solid_srv(dev, DXGI_FORMAT_R8G8B8A8_UNORM, cube, 4, true);
    white_ = solid_srv(dev, DXGI_FORMAT_R8G8B8A8_UNORM, white, 4, false);
    farDepth_ = solid_srv(dev, DXGI_FORMAT_R32_FLOAT, &farZ, 4, false);
    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sm.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sm, &sampler_))) return false;
    return grey_ && envCube_ && white_ && farDepth_;
}

void WorldGamePass::reset(size_t models) {
    models_.clear();
    models_.resize(models);
    built_ = failed_ = lightStandIns_ = 0;
}

bool WorldGamePass::add(uint32_t m, std::shared_ptr<const ModelPreview> game) {
    if (m >= models_.size() || models_[m].game || models_[m].built) return false;
    if (!game) {   // the trace failed: nothing to draw with
        models_[m].built = true;
        models_[m].state = GameModelWorld::State::NoShader;
        ++failed_;
        return true;
    }
    models_[m].game = std::move(game);
    models_[m].state = GameModelWorld::State::Waiting;
    return true;
}

bool WorldGamePass::needs_build(uint32_t m, const ModelPreview* preview) const {
    return m < models_.size() && models_[m].game && !models_[m].built && preview;
}

GameModelWorld::State WorldGamePass::state(uint32_t m, const ModelPreview* preview, const TextureCache& tex) {
    if (m >= models_.size()) return GameModelWorld::State::NoData;
    GameModelWorld& g = models_[m];
    if (g.state == GameModelWorld::State::NoShader) return g.state;
    if (!g.built) return g.game ? GameModelWorld::State::Waiting : GameModelWorld::State::NoData;
    if (!preview) return GameModelWorld::State::Waiting;
    if (!g.texturesSettled) {
        for (const GameMatWorld& mat : g.mats)
            for (int k = 0; k < 16; ++k)
                if (mat.ok && mat.texFile[k] && tex.state(mat.texFile[k]) == TextureCache::State::Unknown)
                    return GameModelWorld::State::Waiting;
        g.texturesSettled = true;
        g.state = GameModelWorld::State::Ready;
    }
    return g.state;
}

bool WorldGamePass::build_mat(ID3D11Device* dev, const GameMaterial& c, const ModelPreview& game, const GameVals& vals,
                              GameMatWorld& g) {
    if (!c.ok || c.vsDXBC.empty() || c.psDXBC.empty()) return false;
    const uint64_t hv = hash_bytes(c.vsDXBC), hp = hash_bytes(c.psDXBC);
    ComPtr<ID3D11VertexShader>& vs = vs_[hv];
    if (!vs && FAILED(dev->CreateVertexShader(c.vsDXBC.data(), c.vsDXBC.size(), nullptr, &vs))) {
        vs_.erase(hv);
        return false;
    }
    ComPtr<ID3D11PixelShader>& ps = ps_[hp];
    if (!ps && FAILED(dev->CreatePixelShader(c.psDXBC.data(), c.psDXBC.size(), nullptr, &ps))) {
        ps_.erase(hp);
        return false;
    }
    ComPtr<ID3D11InputLayout>& il = il_[hv];
    if (!il) {
        std::vector<std::string> names;
        std::vector<D3D11_INPUT_ELEMENT_DESC> el;
        if (!game_input_layout(c.vsDXBC, names, el) ||
            FAILED(dev->CreateInputLayout(el.data(), static_cast<UINT>(el.size()), c.vsDXBC.data(), c.vsDXBC.size(),
                                          &il))) {
            il_.erase(hv);
            return false;
        }
    }
    g.vs = vs.Get();
    g.ps = ps.Get();
    g.il = il.Get();

    auto blend_of = [&](uint64_t key, const D3D11_BLEND_DESC& d) -> ID3D11BlendState* {
        ComPtr<ID3D11BlendState>& b = blend_[key];
        if (!b && FAILED(dev->CreateBlendState(&d, &b))) {
            blend_.erase(key);
            return nullptr;
        }
        return b.Get();
    };
    // Keys: the opaque state depends only on the write mask; the blended one
    // on the blend fields (bits 12..35) and the mask.
    const UINT8 mask = game_write_mask(c.shaderPassFlags);
    g.opaqueBlend = blend_of(0xF000000000000000ull | mask, game_opaque_blend_desc(c.shaderPassFlags));
    D3D11_BLEND_DESC bd;
    if (game_blend_desc(c.renderState, c.shaderPassFlags, bd))
        g.blend = blend_of((c.renderState & 0x0000000FFFFFF000ull) | (static_cast<uint64_t>(mask) << 40), bd);
    if (!g.opaqueBlend) return false;
    g.depthWrite = (c.shaderPassFlags & 0x0040u) == 0;
    g.cutout = c.prepassCutout;

    for (const GameSamplerCPU& s : c.samplers) {
        if (s.slot < 0 || s.slot >= 16) continue;
        const int k = s.slot;
        if (s.global != 0) {
            g.global[k] = static_cast<int8_t>(s.global);
            if (s.global == 1) g.lightStandIn = true;
        } else if (s.gameTex >= 0 && static_cast<size_t>(s.gameTex) < game.textures.size() &&
                   game.textures[static_cast<size_t>(s.gameTex)].fileId) {
            g.texFile[k] = game.textures[static_cast<size_t>(s.gameTex)].fileId;
        } else {
            g.global[k] = 1;   // the composited atlas (fileId 0) or no texture: grey, never white
        }
    }

    auto plan = [&](const std::vector<GameShaderUniform>& u, uint32_t cb, const std::vector<GameConstOverride>& k,
                    std::vector<uint8_t>& base, std::vector<GameMatWorld::Dyn>& dyn) {
        base.assign(game_cb_size(u, cb), 0);
        game_fill_cb(base, u, vals, k);
        for (const GameShaderUniform& x : u) {
            if (x.type == 0) continue;
            const GameDyn d = game_dyn_of(x.name);
            if (d == GameDyn::Count) continue;
            const int vec4s = std::clamp(x.vec4s, 0, 4);
            if (x.byteOff < 0 || static_cast<size_t>(x.byteOff) + static_cast<size_t>(vec4s) * 16 > base.size()) continue;
            dyn.push_back({static_cast<uint16_t>(x.byteOff), d, static_cast<uint8_t>(vec4s)});
        }
    };
    plan(c.vsUniforms, c.vsConstBuf, c.vsConsts, g.vsBase, g.vsDyn);
    plan(c.psUniforms, c.psConstBuf, c.psConsts, g.psBase, g.psDyn);
    g.ok = true;
    return true;
}

void WorldGamePass::build(ID3D11Device* dev, uint32_t m, const ModelPreview& preview, const GameVals& vals) {
    if (m >= models_.size()) return;
    GameModelWorld& gm = models_[m];
    if (gm.built || !gm.game) return;
    gm.built = true;
    gm.state = GameModelWorld::State::NoShader;
    const ModelPreview& game = *gm.game;

    // Materials by index, as build_game_materials sizes them.
    uint32_t maxIdx = 0;
    for (const auto& x : preview.materials) maxIdx = std::max(maxIdx, x.index);
    for (const auto& x : game.gameMaterials) maxIdx = std::max(maxIdx, x.index);
    gm.mats.assign(static_cast<size_t>(maxIdx) + 1, GameMatWorld{});
    for (const GameMaterial& c : game.gameMaterials)
        if (c.index < gm.mats.size()) build_mat(dev, c, game, vals, gm.mats[c.index]);

    // LOD0 of every mesh, as upload_scene_model concatenates them.
    std::vector<GVertex> verts;
    std::vector<uint32_t> idx;
    bool allOk = true;
    for (const ModelMeshCPU& mesh : preview.meshes) {
        if (mesh.indices.empty() || mesh.vertices.empty()) continue;
        const uint32_t base = static_cast<uint32_t>(verts.size());
        GameModelWorld::Sub sub;
        sub.first = static_cast<uint32_t>(idx.size());
        sub.mat = mesh.materialIndex;
        verts.insert(verts.end(), mesh.vertices.begin(), mesh.vertices.end());
        for (uint32_t i : mesh.indices)
            if (i < mesh.vertices.size()) idx.push_back(base + i);
        sub.count = static_cast<uint32_t>(idx.size()) - sub.first;
        if (sub.count == 0) continue;
        if (sub.mat >= gm.mats.size() || !gm.mats[sub.mat].ok) allOk = false;
        gm.subs.push_back(sub);
    }
    if (!allOk || gm.subs.empty()) {
        gm.mats.clear();
        gm.subs.clear();
        ++failed_;
        return;
    }
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(GVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{verts.data(), 0, 0};
    bool bufOk = SUCCEEDED(dev->CreateBuffer(&bd, &sd, &gm.vb));
    bd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    bufOk = bufOk && SUCCEEDED(dev->CreateBuffer(&bd, &sd, &gm.ib));
    if (!bufOk) {
        gm.mats.clear();
        gm.subs.clear();
        gm.vb.Reset();
        gm.ib.Reset();
        ++failed_;
        return;
    }
    for (const GameMatWorld& x : gm.mats) gm.lightStandIn = gm.lightStandIn || (x.ok && x.lightStandIn);
    for (const GameModelWorld::Sub& s : gm.subs) lightStandIns_ += gm.mats[s.mat].lightStandIn ? 1 : 0;
    gm.state = GameModelWorld::State::Waiting;   // until its textures settle (state())
    gm.game.reset();   // the CPU copy is not needed once built (the window keeps its own)
    ++built_;
}

bool WorldGamePass::create_prepass(ID3D11Device* dev) {
    auto blob = [](const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& out) {
        ComPtr<ID3DBlob> err;
        return SUCCEEDED(D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, 0, 0, &out, &err));
    };
    ComPtr<ID3DBlob> nv, np, lv, lp;
    if (!blob(shaders::kNormalGBuffer, "VSNrm", "vs_5_0", nv) || !blob(shaders::kNormalGBuffer, "PSNrm", "ps_5_0", np) ||
        !blob(shaders::kLightAccum, "VSFS", "vs_5_0", lv) || !blob(shaders::kLightAccum, "PSLight", "ps_5_0", lp))
        return false;
    // GVertex: position, normal, UV0 (the cutout's texture coordinate).
    const D3D11_INPUT_ELEMENT_DESC el[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(CB);
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &nrmCb_))) return false;
    bd.ByteWidth = 24 * sizeof(float);   // shRed, shGreen, shBlue, shSun, shSunColor, encParams
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &lightCb_))) return false;
    return SUCCEEDED(dev->CreateVertexShader(nv->GetBufferPointer(), nv->GetBufferSize(), nullptr, &nrmVs_)) &&
           SUCCEEDED(dev->CreatePixelShader(np->GetBufferPointer(), np->GetBufferSize(), nullptr, &nrmPs_)) &&
           SUCCEEDED(dev->CreateVertexShader(lv->GetBufferPointer(), lv->GetBufferSize(), nullptr, &lightVs_)) &&
           SUCCEEDED(dev->CreatePixelShader(lp->GetBufferPointer(), lp->GetBufferSize(), nullptr, &lightPs_)) &&
           SUCCEEDED(dev->CreateInputLayout(el, 3, nv->GetBufferPointer(), nv->GetBufferSize(), &nrmIl_));
}

bool WorldGamePass::resize_prepass(ID3D11Device* dev, int w, int h) {
    nrmRtv_.Reset();
    nrmSrv_.Reset();
    lightRtv_.Reset();
    lightSrv_.Reset();
    lightValid_ = false;
    if (w <= 0 || h <= 0) return false;
    // As device.cpp's create_targets: RGBA8 normals, an RGBA16F light buffer.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> n, l;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &n)) ||
        FAILED(dev->CreateRenderTargetView(n.Get(), nullptr, &nrmRtv_)) ||
        FAILED(dev->CreateShaderResourceView(n.Get(), nullptr, &nrmSrv_)))
        return false;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &l)) &&
           SUCCEEDED(dev->CreateRenderTargetView(l.Get(), nullptr, &lightRtv_)) &&
           SUCCEEDED(dev->CreateShaderResourceView(l.Get(), nullptr, &lightSrv_));
}

void WorldGamePass::light_prepass(ID3D11DeviceContext* ctx, const std::vector<std::pair<uint32_t, const Mat4*>>& insts,
                                  ID3D11DepthStencilView* dsv, ID3D11DepthStencilState* dss, ID3D11RasterizerState* rs,
                                  const TextureCache& tex, const GameVals& vals, int w, int h) {
    lightValid_ = false;
    if (!nrmVs_ || !nrmRtv_ || !lightRtv_ || !dsv) return;
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    // Pass 1: world normals of every drawn opaque submesh (lighting.cpp
    // render_scene_light_prepass), with the material's own prepass cutout.
    const float nclr[4] = {0.5f, 0.5f, 0.5f, 0.0f};
    ctx->ClearRenderTargetView(nrmRtv_.Get(), nclr);
    ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ctx->OMSetRenderTargets(1, nrmRtv_.GetAddressOf(), dsv);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs);
    ctx->OMSetDepthStencilState(dss, 0);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->IASetInputLayout(nrmIl_.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(nrmVs_.Get(), nullptr, 0);
    ctx->PSSetShader(nrmPs_.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, nrmCb_.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, nrmCb_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    const UINT stride = sizeof(GVertex), offset = 0;
    for (const auto& [m, world] : insts) {
        if (m >= models_.size() || models_[m].state != GameModelWorld::State::Ready) continue;
        const GameModelWorld& gm = models_[m];
        CB cb{};
        cb.mvp = mul(*world, frame_.viewProj);
        cb.model = *world;
        ctx->IASetVertexBuffers(0, 1, gm.vb.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(gm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        for (const GameModelWorld::Sub& s : gm.subs) {
            const GameMatWorld& g = gm.mats[s.mat];
            if (g.blend) continue;   // translucent surfaces read the buffer, they do not write it
            ID3D11ShaderResourceView* dif = g.texFile[0] ? tex.srv(g.texFile[0], false) : nullptr;
            cb.hasTex = (dif && g.cutout) ? 1.0f : 0.0f;
            cb.cutout = g.cutout ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (FAILED(ctx->Map(nrmCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
            std::memcpy(ms.pData, &cb, sizeof cb);
            ctx->Unmap(nrmCb_.Get(), 0);
            ctx->PSSetShaderResources(0, 1, &dif);
            ctx->DrawIndexed(s.count, s.first, 0);
        }
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);

    // Pass 2: the env rig's SH diffuse per pixel (kLightAccum) into the light buffer.
    float sh[24] = {};
    auto put = [&](int i, const char* n) {
        auto it = vals.find(n);
        if (it == vals.end()) return;
        for (size_t k = 0; k < 4 && k < it->second.size(); ++k) sh[i * 4 + static_cast<int>(k)] = it->second[k];
    };
    put(0, "shRed");
    put(1, "shGreen");
    put(2, "shBlue");
    put(3, "shSun");
    put(4, "shSunColor");
    sh[20] = kLightBufferEncode;
    D3D11_MAPPED_SUBRESOURCE lms;
    if (SUCCEEDED(ctx->Map(lightCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lms))) {
        std::memcpy(lms.pData, sh, sizeof sh);
        ctx->Unmap(lightCb_.Get(), 0);
    }
    ctx->OMSetRenderTargets(1, lightRtv_.GetAddressOf(), nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(lightVs_.Get(), nullptr, 0);
    ctx->PSSetShader(lightPs_.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, lightCb_.GetAddressOf());
    ctx->PSSetShaderResources(0, 1, nrmSrv_.GetAddressOf());
    ctx->Draw(3, 0);
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    lightValid_ = true;
}

bool WorldGamePass::has_blended(uint32_t m) const {
    if (m >= models_.size()) return false;
    for (const GameModelWorld::Sub& s : models_[m].subs)
        if (models_[m].mats[s.mat].blend) return true;
    return false;
}

ID3D11Buffer* WorldGamePass::cbuffer(ID3D11Device* dev, size_t bytes, bool vs) {
    auto& pool = vs ? vsCb_ : psCb_;
    ComPtr<ID3D11Buffer>& b = pool[bytes];
    if (!b) {
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.ByteWidth = static_cast<UINT>(bytes);
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &b))) {
            pool.erase(bytes);
            return nullptr;
        }
    }
    return b.Get();
}

void WorldGamePass::write_cb(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const std::vector<uint8_t>& base,
                             const std::vector<GameMatWorld::Dyn>& dyn) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    uint8_t* p = static_cast<uint8_t*>(ms.pData);
    std::memcpy(p, base.data(), base.size());
    for (const GameMatWorld::Dyn& d : dyn) {
        const int v = static_cast<int>(d.var);
        const float* src = v <= static_cast<int>(GameDyn::WorldView) ? inst_[v] : frame_.v[v];
        std::memcpy(p + d.off, src, static_cast<size_t>(d.vec4s) * 16);
    }
    ctx->Unmap(b, 0);
}

void WorldGamePass::bind_common(ID3D11DeviceContext* ctx) {
    ID3D11SamplerState* ss[16];
    for (auto& s : ss) s = sampler_.Get();
    ctx->PSSetSamplers(0, 16, ss);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void WorldGamePass::set_instance(const Mat4& world) {
    put_matrix(inst_[0], world);
    put_matrix(inst_[1], mul(world, frame_.viewProj));
    put_matrix(inst_[2], mul(world, frame_.view));
}

void WorldGamePass::bind_material(ID3D11DeviceContext* ctx, const GameMatWorld& g, ID3D11Buffer* vcb,
                                  ID3D11Buffer* pcb, const TextureCache& tex) {
    ctx->IASetInputLayout(g.il);
    ctx->VSSetShader(g.vs, nullptr, 0);
    ctx->PSSetShader(g.ps, nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, &vcb);
    ctx->PSSetConstantBuffers(0, 1, &pcb);
    ID3D11ShaderResourceView* srv[16] = {};
    for (int k = 0; k < 16; ++k) {
        if (g.texFile[k]) {
            srv[k] = tex.srv(g.texFile[k], false);
            if (!srv[k]) srv[k] = grey_.Get();   // a failed decode: grey, as the extraction does
        } else {
            switch (g.global[k]) {
            case 1: srv[k] = lightValid_ ? lightSrv_.Get() : grey_.Get(); break;
            case 2: srv[k] = envCube_.Get(); break;
            case 3: srv[k] = white_.Get(); break;
            case 4: srv[k] = farDepth_.Get(); break;
            default: break;
            }
        }
    }
    ctx->PSSetShaderResources(0, 16, srv);
}

int WorldGamePass::draw_opaque_group(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t m,
                                     const std::vector<const Mat4*>& worlds, const TextureCache& tex,
                                     ID3D11DepthStencilState* dss, size_t& tris) {
    if (m >= models_.size() || worlds.empty()) return 0;
    GameModelWorld& gm = models_[m];
    if (gm.state != GameModelWorld::State::Ready) return 0;
    const UINT stride = sizeof(GVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, gm.vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(gm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->OMSetDepthStencilState(dss, 0);
    auto perInstance = [](const std::vector<GameMatWorld::Dyn>& dyn) {
        for (const GameMatWorld::Dyn& d : dyn)
            if (d.var <= GameDyn::WorldView) return true;
        return false;
    };
    int draws = 0;
    for (const GameModelWorld::Sub& s : gm.subs) {
        const GameMatWorld& g = gm.mats[s.mat];
        if (g.blend) continue;
        ID3D11Buffer* vcb = cbuffer(dev, g.vsBase.size(), true);
        ID3D11Buffer* pcb = cbuffer(dev, g.psBase.size(), false);
        if (!vcb || !pcb) continue;
        bind_material(ctx, g, vcb, pcb, tex);
        ctx->OMSetBlendState(g.opaqueBlend, nullptr, 0xffffffff);
        const bool vsInst = perInstance(g.vsDyn), psInst = perInstance(g.psDyn);
        for (size_t i = 0; i < worlds.size(); ++i) {
            set_instance(*worlds[i]);
            if (vsInst || i == 0) write_cb(ctx, vcb, g.vsBase, g.vsDyn);
            if (psInst || i == 0) write_cb(ctx, pcb, g.psBase, g.psDyn);
            ctx->DrawIndexed(s.count, s.first, 0);
            tris += s.count / 3;
            ++draws;
        }
    }
    return draws;
}

int WorldGamePass::draw_instance(ID3D11Device* dev, ID3D11DeviceContext* ctx, uint32_t m, const Mat4& world,
                                 bool blendedPass, const TextureCache& tex, ID3D11DepthStencilState* dss,
                                 ID3D11DepthStencilState* dssNoWrite, size_t& tris) {
    if (m >= models_.size()) return 0;
    GameModelWorld& gm = models_[m];
    if (gm.state != GameModelWorld::State::Ready) return 0;
    set_instance(world);
    const UINT stride = sizeof(GVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, gm.vb.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(gm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    int draws = 0;
    for (const GameModelWorld::Sub& s : gm.subs) {
        const GameMatWorld& g = gm.mats[s.mat];
        if ((g.blend != nullptr) != blendedPass) continue;
        ID3D11Buffer* vcb = cbuffer(dev, g.vsBase.size(), true);
        ID3D11Buffer* pcb = cbuffer(dev, g.psBase.size(), false);
        if (!vcb || !pcb) continue;
        write_cb(ctx, vcb, g.vsBase, g.vsDyn);
        write_cb(ctx, pcb, g.psBase, g.psDyn);
        bind_material(ctx, g, vcb, pcb, tex);
        if (blendedPass) {
            ctx->OMSetBlendState(g.blend, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(g.depthWrite ? dss : dssNoWrite, 0);
        } else {
            ctx->OMSetBlendState(g.opaqueBlend, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(dss, 0);
        }
        ctx->DrawIndexed(s.count, s.first, 0);
        tris += s.count / 3;
        ++draws;
    }
    return draws;
}

} // namespace castlemist::render
