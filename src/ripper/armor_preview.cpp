#include "castlemist/ripper/armor_preview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "castlemist/ripper/atlas.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/dye.h"
#include "castlemist/ripper/look.h"
#include "internal.h"

namespace castlemist::ripper {

namespace {

constexpr int kAtlas = 1024;

/// The Composite file of one archive, indexed by armor mesh fileId. Parsed on
/// first use and kept: it is a few MB and the same for every armor model.
struct CompositeIndex {
    bool loaded = false;  // tried, whether or not the archive had one
    std::string dat_path;
    std::optional<composite::Composite> comp;
    std::unordered_map<uint32_t, std::pair<const composite::CompositeFileData*, const composite::BlitRectSet*>> by_mesh;
};

const CompositeIndex& composite_index(Gw2Dat& dat) {
    static CompositeIndex idx;
    if (idx.loaded && idx.dat_path == dat.file_info.file_path) return idx;
    idx = CompositeIndex{};
    idx.loaded = true;
    idx.dat_path = dat.file_info.file_path;
    idx.comp = load_composite(dat);
    if (!idx.comp) return idx;
    for (const composite::CompositeRace& race : idx.comp->races)
        for (const auto& [token, fd] : race.file_data) {
            if (fd.type < 8) continue;  // bare body, face, hair, ears: not armor
            const composite::BlitRectSet* set =
                fd.blit_set < idx.comp->blit_sets.size() ? &idx.comp->blit_sets[fd.blit_set] : nullptr;
            for (uint32_t mesh : {fd.mesh_base, fd.mesh_overlap})
                if (mesh) idx.by_mesh.try_emplace(mesh, &fd, set);
        }
    return idx;
}

// A real alpha cut-out: a minority of texels below the game's 0.25 threshold
// (the same test the piece export applies).
bool alpha_cuts(const ModelTextureCPU& t) {
    size_t n = t.rgba.size() / 4, low = 0;
    for (size_t i = 0; i < n; ++i) low += t.rgba[i * 4 + 3] < 64;
    return n && low > n / 100 && low < n * 95 / 100;
}

ModelTextureCPU to_texture(ImageRgba im, uint32_t file_id, bool is_normal, bool cutout) {
    ModelTextureCPU t;
    t.fileId = file_id;
    t.width = im.w;
    t.height = im.h;
    t.rgba = std::move(im.px);
    t.fmt = is_normal ? "baked normal" : "baked diffuse";
    t.channels = is_normal ? "RGB" : "RGBA";
    t.isNormal = is_normal;
    t.hasCutout = cutout;
    return t;
}

/// The atlas colour around a UV point (3x3 texels, wrapped), RGB in 0..1.
std::array<float, 3> swatch(const ModelTextureCPU& atlas, float u, float v) {
    std::array<float, 3> c{};
    const int cx = static_cast<int>(u * static_cast<float>(atlas.width));
    const int cy = static_cast<int>(v * static_cast<float>(atlas.height));
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const int x = ((cx + dx) % atlas.width + atlas.width) % atlas.width;
            const int y = ((cy + dy) % atlas.height + atlas.height) % atlas.height;
            const uint8_t* p = &atlas.rgba[(static_cast<size_t>(y) * atlas.width + x) * 4];
            for (int k = 0; k < 3; ++k) c[k] += p[k] / (255.0f * 9.0f);
        }
    return c;
}

/// A `dyedot` material (spikes, emblems, trims added over armor) maps its own
/// grey detail texture and reads the atlas only at up to three swatch points
/// -- its dyedot / dyedotb / dyedotc constants, small squares painted into the
/// piece's base texture -- to colour it where its dyedotmask's R / G / B say.
/// The game shader does that per pixel; the reconstruction shader cannot, so
/// the colours are baked into a copy of the detail texture, which becomes the
/// material's diffuse on the detail's own UV set. False when @p mat isn't one.
bool is_dyedot(const ModelMaterialCPU& mat) {
    for (const auto& c : mat.namedConstantVectors)
        if (c.first == "dyedot" || c.first == "dyedotb" || c.first == "dyedotc") return true;
    return false;
}

bool bake_dyedot(ModelPreview& model, ModelMaterialCPU& mat, const ModelTextureCPU& atlas) {
    std::array<std::optional<std::array<float, 3>>, 3> dots;
    for (const auto& [name, v] : mat.namedConstantVectors) {
        const int ch = name == "dyedot" ? 0 : name == "dyedotb" ? 1 : name == "dyedotc" ? 2 : -1;
        if (ch >= 0) dots[static_cast<size_t>(ch)] = swatch(atlas, v[0], v[1]);
    }
    if (!dots[0] && !dots[1] && !dots[2]) return false;
    const int detail = mat.diffuseTex;  // the detail layer, what the area heuristic picked
    int mask = -1;
    for (const auto& ex : mat.extraTextures)
        if (ex.role == "dyedotmask" && ex.uvIndex == mat.diffuseUv) mask = ex.texIndex;
    if (detail < 0 || mask < 0) return false;

    ModelTextureCPU out = model.textures[static_cast<size_t>(detail)];
    ImageRgba m = detail::to_image(model.textures[static_cast<size_t>(mask)]);
    if (m.w != out.width || m.h != out.height) m = resize_bilinear(m, out.width, out.height);
    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
        for (size_t ch = 0; ch < 3; ++ch) {
            if (!dots[ch]) continue;
            const float w = m.px[i + ch] / 255.0f;
            if (w <= 0.0f) continue;
            // The detail is grey around 0.66: it shades the swatch colour.
            for (size_t k = 0; k < 3; ++k) {
                const float d = out.rgba[i + k] / 255.0f;
                const float dyed = std::min(1.0f, (*dots[ch])[k] * d * 1.5f);
                out.rgba[i + k] = static_cast<uint8_t>(std::lround(255.0f * (d + (dyed - d) * w)));
            }
        }
    }
    out.fileId = 0xC0000000u | mat.index;  // synthetic: the glTF writer dedups textures by fileId
    out.fmt = "baked dyedot";
    mat.diffuseTex = static_cast<int>(model.textures.size());
    model.textures.push_back(std::move(out));
    return true;
}

bool samples_atlas(const ModelPreview& model) {
    for (const ModelMaterialCPU& mat : model.materials)
        if (mat.atlasDiffuseUv >= 0 || mat.atlasNormalUv >= 0) return true;
    for (const GameMaterial& gm : model.gameMaterials)
        for (const GameSamplerCPU& s : gm.samplers)
            if (s.atlas) return true;
    return false;
}

} // namespace

std::vector<character::ManifestDye> default_preview_dyes() { return detail::undergarment_dyes(); }

const cmap::Palette* dye_palette() {
    constexpr uint32_t kEveryDyePalette = 82;  // docs/research/gw2-armor-skins-and-dyes.md section 6
    return cmap::built() ? cmap::palette(kEveryDyePalette) : nullptr;
}

std::array<uint8_t, 3> dye_swatch(const cmap::Palette& palette, const cmap::PaletteColor& color, int material) {
    if (color.materials.empty()) return palette.base;
    const size_t m = std::min(static_cast<size_t>(std::max(material, 0)), color.materials.size() - 1);
    return apply_dye(dye_matrix(to_dye_shift(color.materials[m])), palette.base);
}

std::vector<character::ManifestDye> preview_dyes(const std::array<DyeChoice, 4>& choices) {
    std::vector<character::ManifestDye> dyes = default_preview_dyes();
    const cmap::Palette* palette = dye_palette();
    for (size_t slot = 0; slot < 4; ++slot) {
        character::ManifestDye& d = dyes[slot];
        const DyeChoice& c = choices[slot];
        const int m = std::clamp(c.material, 0, 3);
        d.material = kDyeMaterials[m];
        if (!palette) continue;
        for (const cmap::PaletteColor& pc : palette->colors) {
            if (pc.id != c.color_id || pc.materials.empty()) continue;
            d.color_id = pc.id;
            d.color_name = color_name(pc.id);
            d.shift = to_dye_shift(pc.materials[std::min(static_cast<size_t>(m), pc.materials.size() - 1)]);
            d.rgb = dye_swatch(*palette, pc, m);
            break;
        }
    }
    return dyes;
}

std::optional<ArmorPreviewTextures> build_armor_preview(Gw2Dat& dat, uint32_t mft_index, const ModelPreview& model,
                                                        const std::vector<character::ManifestDye>& dyes) {
    if (!samples_atlas(model)) return std::nullopt;
    const CompositeIndex& idx = composite_index(dat);
    const composite::CompositeFileData* fd = nullptr;
    const composite::BlitRectSet* set = nullptr;
    for (uint32_t file_id : get_by_file_id(dat, mft_index + 1)) {
        auto it = idx.by_mesh.find(file_id);
        if (it == idx.by_mesh.end()) continue;
        fd = it->second.first;
        set = it->second.second;
        break;
    }
    if (!fd) return std::nullopt;

    std::optional<detail::BakedTextures> baked = detail::bake_part(dat, *fd, dyes);
    if (!baked) return std::nullopt;
    ArmorPreviewTextures out;
    out.dyed_channels = baked->dyed;
    for (size_t i = 0; i < 4; ++i) out.channels[i] = fd->mask_dye[i] != 0;
    const bool cutout = alpha_cuts(baked->base);

    // Where the atlas puts the piece: the rects the UVs of the meshes that
    // sample it fall in -- leaving out `dyedot` materials, which map their own
    // detail texture over the whole 0..1 UV0 range and read the atlas only at
    // swatch points; their UVs would drag the anchor to the atlas origin.
    std::vector<std::pair<float, float>> uvs;
    for (const ModelMeshCPU& mesh : model.meshes) {
        if (mesh.materialIndex >= model.materials.size()) continue;
        const ModelMaterialCPU& mat = model.materials[mesh.materialIndex];
        if (mat.atlasDiffuseUv < 0 || is_dyedot(mat)) continue;
        auto w = detail::wrapped_uvs(mesh);
        uvs.insert(uvs.end(), w.begin(), w.end());
    }
    AtlasRegion region = set ? region_for(*set, uvs) : AtlasRegion{};
    if (region.rects.empty()) {
        // Self-textured: the piece's own texture on its own UVs.
        out.diffuse = to_texture(detail::to_image(baked->base), fd->texture_base, false, cutout);
        if (baked->normal) out.normal = to_texture(detail::to_image(*baked->normal), fd->texture_normal, true, false);
        return out;
    }
    detail::place_texture(region, detail::to_image(baked->base), baked->scale,
                          detail::uv_coverage(model, kAtlas, kAtlas, false));
    if (std::getenv("GW2_TEXDBG"))
        std::fprintf(stderr, "[armor] mesh=%u texture=%u '%s' rects=%zu anchor=(%u,%u) scale=%.2f\n", fd->mesh_base,
                     fd->texture_base, set->name.c_str(), region.rects.size(), region.ax, region.ay, baked->scale);
    // The piece is alone in this atlas, so its whole texture is drawn, not just
    // what its rects keep: the swatches a `dyedot` material reads can sit
    // outside them, and so can texels a filtered edge samples.
    auto footprint = [&](const ModelTextureCPU& t, float scale) {
        const auto ext = [&](uint32_t from, int px) {
            return std::min<uint32_t>(kAtlas, from + static_cast<uint32_t>(std::lround(scale * static_cast<float>(px))));
        };
        return AtlasRegion{{composite::BlitRect{region.ax, region.ay, ext(region.ax, t.width), ext(region.ay, t.height)}},
                           region.ax, region.ay};
    };
    // Opaque mid-grey around the piece, so a stray texel reads as surface
    // rather than a hole.
    ImageRgba diffuse{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 128)};
    for (size_t i = 3; i < diffuse.px.size(); i += 4) diffuse.px[i] = 255;
    blit(diffuse, detail::to_image(baked->base), footprint(baked->base, baked->scale), baked->scale);
    out.diffuse = to_texture(std::move(diffuse), fd->texture_base, false, cutout);
    if (baked->normal) {
        // Flat tangent-space normal (0, 0, 1) around the piece.
        ImageRgba normal{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 255)};
        for (size_t i = 0; i < normal.px.size(); i += 4) normal.px[i] = normal.px[i + 1] = 128;
        blit(normal, detail::to_image(*baked->normal), footprint(*baked->normal, baked->normal_scale), baked->normal_scale);
        out.normal = to_texture(std::move(normal), fd->texture_normal, true, false);
    }
    return out;
}

void apply_armor_preview(ModelPreview& model, const ArmorPreviewTextures& tex) {
    for (const ModelTextureCPU& t : model.textures)
        if (t.fileId == tex.diffuse.fileId && t.fmt == tex.diffuse.fmt) return;  // already applied
    const int diffuse = static_cast<int>(model.textures.size());
    model.textures.push_back(tex.diffuse);
    int normal = -1;
    if (tex.normal) {
        normal = static_cast<int>(model.textures.size());
        model.textures.push_back(*tex.normal);
    }
    // Particle clouds draw their sprites with their material's reconstruction
    // diffuse: those keep their own texture (a flame, a glow), not atlas patches.
    std::set<uint32_t> sprite_mats;
    for (const ParticleCloudCPU& c : model.clouds) sprite_mats.insert(c.materialIndex);
    for (size_t mi = 0; mi < model.materials.size(); ++mi) {
        ModelMaterialCPU& mat = model.materials[mi];
        if (sprite_mats.count(static_cast<uint32_t>(mi))) continue;
        if (mat.atlasDiffuseUv >= 0 && bake_dyedot(model, mat, tex.diffuse)) continue;
        if (mat.atlasDiffuseUv >= 0) {
            // The atlas is the base colour; whatever the area heuristic picked
            // instead (a tiling detail layer on another UV set) stays a layer.
            if (mat.diffuseTex >= 0 && mat.diffuseTex != diffuse) {
                ModelMaterialCPU::ExtraTexture ex;
                ex.texIndex = mat.diffuseTex;
                ex.uvIndex = mat.diffuseUv;
                ex.fileId = model.textures[static_cast<size_t>(mat.diffuseTex)].fileId;
                bool listed = false;
                for (const auto& e : mat.extraTextures) listed |= e.texIndex == ex.texIndex;
                if (!listed) mat.extraTextures.push_back(ex);
            }
            mat.diffuseTex = diffuse;
            mat.diffuseUv = static_cast<uint8_t>(mat.atlasDiffuseUv);
        }
        if (mat.atlasNormalUv >= 0 && normal >= 0) {
            mat.normalTex = normal;
            mat.normalUv = static_cast<uint8_t>(mat.atlasNormalUv);
        }
    }
    for (GameMaterial& gm : model.gameMaterials)
        for (GameSamplerCPU& s : gm.samplers) {
            const int t = s.atlas == 1 ? diffuse : s.atlas == 2 ? normal : -1;
            if (t < 0) continue;
            s.gameTex = t;
            s.global = 0;
        }
}

} // namespace castlemist::ripper
