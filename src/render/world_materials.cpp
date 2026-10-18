/// @file
/// @brief WorldView's texture cache (world_materials.h) and the scene's
///        terrain/water texture list (world_prep.h: world_texture_ids).

#include "world_materials.h"

#include "castlemist/exportgltf/blend_mode.h"
#include "castlemist/exportgltf/shader_profiles.h"
#include "castlemist/extract/model_types.h"
#include "castlemist/render/world_prep.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
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
    e.info.hasCutout = t.hasCutout;
    e.info.looksLikeEffect = t.looksLikeEffect;
    e.info.darkCoverage = dark_coverage(t.mips[0]);
    ++ready_;
    bytes_ += bytes;
}

ID3D11ShaderResourceView* TextureCache::srv(uint32_t fileId, bool srgb) const {
    const auto it = map_.find(fileId);
    if (it == map_.end() || it->second.state != State::Ready) return nullptr;
    return srgb ? it->second.srgb.Get() : it->second.unorm.Get();
}

TextureCache::Info TextureCache::info(uint32_t fileId) const {
    const auto it = map_.find(fileId);
    return (it == map_.end() || it->second.state != State::Ready) ? Info{} : it->second.info;
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
    std::vector<uint32_t> out = castlemist::world::terrain_texture_ids(s.terrain);
    std::unordered_set<uint32_t> seen(out.begin(), out.end());
    auto take = [&](uint32_t id) {
        if (id != 0 && seen.insert(id).second) out.push_back(id);
    };
    const int pi = s.environment.light.presetIndex;
    const int ti = cfg.waterBumpTextureIndex;
    if (pi >= 0 && static_cast<size_t>(pi) < s.environment.water.size() && ti >= 0) {
        const std::vector<uint32_t>& tex = s.environment.water[static_cast<size_t>(pi)].textureFileIds;
        if (static_cast<size_t>(ti) < tex.size()) take(tex[static_cast<size_t>(ti)]);
    }
    return out;
}

std::vector<std::string> material_setting_warnings(const WorldMaterialSettings& s) {
    std::vector<std::string> out;
    char b[320];
    auto line = [&](const char* fmt, auto... v) {
        std::snprintf(b, sizeof b, fmt, v...);
        out.emplace_back(b);
    };
    const char* on = "on";
    const char* off = "off";
    line("groundRepeatsPerChunk = %g: UNPROVEN, ground repeats per chunk; T3D's guess, not in the shader "
         "(note \xc2\xa7" "1.6)",
         static_cast<double>(s.groundRepeatsPerChunk));
    line("uvScrollUnitSeconds = %g: UNPROVEN, offset = frac(scroll * t / unit) (note \xc2\xa7" "4)",
         static_cast<double>(s.uvScrollUnitSeconds));
    line("terrainNormalMaps = %s: UNPROVEN sign, tangent +X east, bitangent -Y (increasing t, south) "
         "(note \xc2\xa7" "1.2)",
         s.terrainNormalMaps ? on : off);
    line("terrainSpecPower = %g: castlemist's choice, forward stand-in for the light buffer's L.a "
         "(note \xc2\xa7" "1.4)",
         static_cast<double>(s.terrainSpecPower));
    out.emplace_back("terrain shSunColor.w = 1: UNPROVEN, the rim term's sun weight (note \xc2\xa7" "1.2)");
    line("maxTextureSize = %d: castlemist's choice, a memory bound (spec, not in the note)", s.maxTextureSize);
    line("waterBumpTextureIndex = %d: UNPROVEN beyond AMAT 187551 (note \xc2\xa7" "6)", s.waterBumpTextureIndex);
    line("waterTileUnit = %g: UNPROVEN, units of bumpTile/bumpSpeed/bumpAngle: uv = R(bumpAngle, radians) "
         "* xy * bumpTile / waterTileUnit + (bumpSpeed * t, 0) (note \xc2\xa7" "6)",
         static_cast<double>(s.waterTileUnit));
    out.emplace_back("water colours: UNPROVEN byte order, surfaceShallowColor/surfaceDeepColor bytes 0,1,2 read as "
                     "R,G,B / 255 (the light and haze colours are B,G,R in the client; the water import is not checked) "
                     "(note \xc2\xa7" "6)");
    out.emplace_back("water bump: castlemist's choice, textureFileIds[waterBumpTextureIndex] read as UNORM xy*2-1, "
                     "N = normalize((n0.xy + n1.xy) * bumpAmount * waterBumpStrengthScale, -1); no scene depth, "
                     "colour copy or reflection; normal xy sign and sun-direction convention sign UNPROVEN (note \xc2\xa7" "6)");
    line("waterSpecPower = %g: castlemist's choice (note \xc2\xa7" "6)", static_cast<double>(s.waterSpecPower));
    line("waterBumpAngleDegrees = %s: UNPROVEN, bumpAngle0/1 read as degrees (stored 143.3 / 61.7 on Queensdale); "
         "no shader transcription settles the unit (note \xc2\xa7" "6)",
         s.waterBumpAngleDegrees ? on : off);
    line("waterBumpStrengthScale = %g: castlemist's choice, bump normal xy gain = bumpAmount * scale (stored "
         "bumpAmount 23.7 -> ~18 degree rms tilt); bumpAmount's unit UNPROVEN (note \xc2\xa7" "6)",
         static_cast<double>(s.waterBumpStrengthScale));
    line("srgbColourTextures = %s: castlemist's choice, colour textures decode sRGB to linear before the "
         "blend (spec, not in the note)",
         s.srgbColourTextures ? on : off);
    out.emplace_back("all animated props start at t = 0 and loop: UNPROVEN (note \xc2\xa7" "5)");
    out.emplace_back("pickerSolid byte order: UNPROVEN, solid pages use the stored bytes as RGBA (note \xc2\xa7" "1.6)");
    out.emplace_back("water preset k pairs with lighting preset k: UNPROVEN (note \xc2\xa7" "6)");
    out.emplace_back("dark decals: castlemist's choice, a Clip texture whose visible texels (alpha >= 64) are all black "
                     "(<= 16) blends on its alpha instead; castlemist's own rule, measured on AMAT 58246's decals in "
                     "sub-project 3, not from the research note or the game's shader");
    out.emplace_back("water alpha: castlemist's choice, alpha = lerp(0.45, 1, fresnel) on the preset-driven water "
                     "(flat water uses 0.45); not in the dat (note \xc2\xa7" "6)");
    out.emplace_back("terrain uvData recorded, not animated: UNPROVEN target and units (note \xc2\xa7" "1.5)");
    return out;
}

namespace eg = castlemist::exportgltf;

WorldAlpha world_alpha_mode(const ModelMaterialCPU& mat, bool diffuseHasCutout, bool diffuseLooksLikeEffect,
                            bool diffuseDarkCoverage) {
    const bool isEffect = mat.isEffect || (!mat.hasRenderState && diffuseLooksLikeEffect);
    const eg::BlendInfo b = eg::decode_blend(mat.renderState, mat.hasRenderState, isEffect,
                                             eg::alpha_tested(eg::profile_for(mat, mat.renderState)));
    if (!mat.hasRenderState) {   // material_export.cpp: cutout first, then effect
        // A clip that keeps only black texels can only be a darkening decal
        // (ruling P7: Queensdale's AMAT 58246 decals, rgb 0 under soft alpha,
        // drew as solid black splats): blend it on its alpha instead.
        if (diffuseHasCutout) return diffuseDarkCoverage ? WorldAlpha::Blend : WorldAlpha::Clip;
        return b.preset == eg::BlendPreset::Additive ? WorldAlpha::Additive : WorldAlpha::Opaque;
    }
    // Each mode below is the preset's exact bgfx factors (decode_blend's
    // table); a blend with no preset (Custom) or one whose factors vary
    // (SoftAdditive: One/InvSrcColor or InvDstColor/One) is not guessed.
    switch (b.preset) {
    case eg::BlendPreset::Opaque: return WorldAlpha::Opaque;
    case eg::BlendPreset::Cutout: return WorldAlpha::Clip;
    case eg::BlendPreset::Fade:
    case eg::BlendPreset::TransClipping: return WorldAlpha::Blend;          // SrcAlpha / InvSrcAlpha
    case eg::BlendPreset::Additive:                                         // SrcAlpha or One / One
        return b.srcRgb == 2 ? WorldAlpha::AdditiveOne : WorldAlpha::Additive;
    case eg::BlendPreset::Transparent: return WorldAlpha::Premultiplied;    // One / InvSrcAlpha
    case eg::BlendPreset::Multiplicative: return WorldAlpha::Multiply;      // DstColor / Zero
    case eg::BlendPreset::Multiplicative2x: return WorldAlpha::Multiply2x;  // DstColor / SrcColor
    default: return WorldAlpha::Unmapped;
    }
}

void uv_scroll_offset(const ModelTexTransformCPU& t, double seconds, float unitSeconds, float out[2]) {
    out[0] = out[1] = 0;
    if (!(unitSeconds > 0)) return;
    for (int k = 0; k < 2; ++k) {
        const double x = static_cast<double>(t.scroll[k]) * seconds / static_cast<double>(unitSeconds);
        out[k] = static_cast<float>(x - std::floor(x));
    }
}

std::vector<PropMaterial> prop_materials(const ModelPreview& m, int* stillEntries) {
    std::vector<PropMaterial> out(m.materials.size());
    for (size_t i = 0; i < m.materials.size(); ++i) {
        const ModelMaterialCPU& mat = m.materials[i];
        if (stillEntries)
            for (const ModelTexTransformCPU& t : mat.texTransforms)
                if (!tex_transform_animates(t)) ++*stillEntries;
        if (mat.diffuseTex < 0 || static_cast<size_t>(mat.diffuseTex) >= m.textures.size() || mat.diffuseUv > 1)
            continue;
        out[i].diffuseFileId = m.textures[static_cast<size_t>(mat.diffuseTex)].fileId;
        out[i].uvSet = mat.diffuseUv;
        out[i].textured = true;
        for (size_t k = 0; k < mat.texTransforms.size(); ++k) {   // the diffuse's own UV set only
            const ModelTexTransformCPU& t = mat.texTransforms[k];
            if (t.uvIndex != mat.diffuseUv || !tex_transform_animates(t)) continue;
            out[i].scroll = static_cast<int>(k);
            out[i].scrollRate[0] = t.scroll[0];
            out[i].scrollRate[1] = t.scroll[1];
            break;
        }
    }
    return out;
}

std::vector<size_t> blend_back_to_front(const std::vector<float>& nearestDistance) {
    std::vector<size_t> order(nearestDistance.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return nearestDistance[a] > nearestDistance[b]; });
    return order;
}

void water_bump_uv(float x, float y, float tile, float speed, float angle, double seconds, float tileUnit,
                   float uv[2]) {
    // UNPROVEN units (note §6): tile repeats per tileUnit map units, speed in
    // UV per second along the rotated u axis, angle in radians. The game's
    // wtrprmv/d/e are client-filled; this is castlemist's reading of the
    // preset's bumpTile / bumpSpeed / bumpAngle.
    const double c = std::cos(static_cast<double>(angle)), s = std::sin(static_cast<double>(angle));
    const double k = tileUnit > 0 ? static_cast<double>(tile) / static_cast<double>(tileUnit) : 0.0;
    uv[0] = static_cast<float>((c * x - s * y) * k + static_cast<double>(speed) * seconds);
    uv[1] = static_cast<float>((s * x + c * y) * k);
}

float water_bump_angle(float stored, bool degrees) {
    // UNPROVEN (note §6, ruling P10): Queensdale's 143.3 / 61.7 read as degrees.
    return degrees ? static_cast<float>(static_cast<double>(stored) * 3.14159265358979323846 / 180.0) : stored;
}

float water_bump_strength(float bumpAmount, float scale) {
    const float g = bumpAmount * scale;
    return (std::isfinite(g) && g > 0) ? g : 0.0f;
}

double clip_time(double seconds, float duration) {
    if (!(duration > 1e-6f) || !std::isfinite(seconds) || !std::isfinite(duration)) return 0.0;
    // gw2bgfx_view.cpp's wrap: loop rather than clamp (the sampler does not loop).
    const double d = duration;
    double t = std::fmod(seconds, d);
    if (t < 0.0) t += d;
    return t < d ? t : 0.0;   // -tiny + d can round to d
}

void pose_palette(const PoseRig& rig, double seconds, std::vector<float>& palette) {
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const size_t n = rig.bones.size();
    palette.resize(n * 16);
    std::vector<castlemist::granny::PoseXform> world;
    const float t = static_cast<float>(clip_time(seconds, rig.clip ? rig.clip->duration : 0.0f));
    castlemist::granny::composePose(rig.bones, rig.clip, t, world, rig.clip ? &rig.trackByName : nullptr);
    for (size_t b = 0; b < n; ++b) {
        float* m = palette.data() + b * 16;
        castlemist::granny::skinMatrix(rig.bones[b], world[b], m);
        bool finite = true;
        for (int k = 0; k < 16; ++k) finite = finite && std::isfinite(m[k]);
        if (!finite) std::copy(kIdentity, kIdentity + 16, m);
    }
}

} // namespace castlemist::render
