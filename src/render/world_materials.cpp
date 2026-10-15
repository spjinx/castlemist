/// @file
/// @brief WorldView's texture cache (world_materials.h) and the scene's
///        terrain/water texture list (world_prep.h: world_texture_ids).

#include "world_materials.h"

#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_prep.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace castlemist::render {

void TextureCache::add(ID3D11Device* dev, const CappedTexture& t) {
    if (!dev || t.fileId == 0) return;
    Entry& e = map_[t.fileId];
    if (e.state != State::Unknown) return;   // once per fileId
    auto fail = [&] {
        e = Entry{};
        e.state = State::Failed;
        ++failed_;
    };
    const size_t levels = t.mips.size();
    if (levels == 0 || t.width <= 0 || t.height <= 0 || levels > D3D11_REQ_MIP_LEVELS) return fail();
    std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
    size_t bytes = 0;
    for (size_t i = 0; i < levels; ++i) {
        const int w = std::max(1, t.width >> i), h = std::max(1, t.height >> i);
        const size_t want = static_cast<size_t>(w) * h * 4;
        if (t.mips[i].size() != want) return fail();
        init[i].pSysMem = t.mips[i].data();
        init[i].SysMemPitch = static_cast<UINT>(w) * 4;
        bytes += want;
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(t.width);
    td.Height = static_cast<UINT>(t.height);
    td.MipLevels = static_cast<UINT>(levels);
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, init.data(), &e.tex))) return fail();
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = static_cast<UINT>(levels);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(dev->CreateShaderResourceView(e.tex.Get(), &sd, &e.unorm))) return fail();
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (FAILED(dev->CreateShaderResourceView(e.tex.Get(), &sd, &e.srgb))) return fail();
    e.state = State::Ready;
    ++ready_;
    bytes_ += bytes;
}

ID3D11ShaderResourceView* TextureCache::srv(uint32_t fileId, bool srgb) const {
    const auto it = map_.find(fileId);
    if (it == map_.end() || it->second.state != State::Ready) return nullptr;
    return srgb ? it->second.srgb.Get() : it->second.unorm.Get();
}

TextureCache::State TextureCache::state(uint32_t fileId) const {
    const auto it = map_.find(fileId);
    return it == map_.end() ? State::Unknown : it->second.state;
}

void TextureCache::clear() {
    map_.clear();
    ready_ = failed_ = 0;
    bytes_ = 0;
}

std::vector<uint32_t> world_texture_ids(const castlemist::world::WorldScene& s, const WorldMaterialSettings& cfg) {
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    auto take = [&](uint32_t id) {
        if (id != 0 && seen.insert(id).second) out.push_back(id);
    };
    auto take4 = [&](const std::vector<uint32_t>& v) {
        for (size_t k = 0; k < 4 && k < v.size(); ++k) take(v[k]);
    };
    for (const castlemist::world::TerrainChunk& c : s.terrain.chunks) {
        take4(c.material.textureFileIds);
        take4(c.material.normalFileIds);
        take(c.material.pickerFileId);
        take(c.material.picker2FileId);
    }
    const int pi = s.environment.light.presetIndex;
    const int ti = cfg.waterBumpTextureIndex;
    if (pi >= 0 && static_cast<size_t>(pi) < s.environment.water.size() && ti >= 0) {
        const std::vector<uint32_t>& tex = s.environment.water[static_cast<size_t>(pi)].textureFileIds;
        if (static_cast<size_t>(ti) < tex.size()) take(tex[static_cast<size_t>(ti)]);
    }
    return out;
}

} // namespace castlemist::render
