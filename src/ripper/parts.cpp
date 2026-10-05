#include "internal.h"

#include <algorithm>
#include <cctype>
#include <exception>

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/ripper/dye.h"

namespace castlemist::ripper::detail {
namespace {

// A real alpha cut-out: a minority of texels below the game's 0.25 threshold.
bool alpha_cuts(const ImageRgba& im) {
    size_t n = im.px.size() / 4, low = 0;
    for (size_t i = 0; i < n; ++i) low += im.px[i * 4 + 3] < 64;
    return n && low > n / 100 && low < n * 95 / 100;
}

} // namespace

std::optional<ModelPreview> load_model(Gw2Dat& dat, uint32_t file_id) {
    try {
        // Against the open dat: extract_entry() would reopen it for every model.
        if (auto m = load_model_by_fileid(dat, file_id)) return *m;
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

ImageRgba to_image(const ModelTextureCPU& t) { return ImageRgba{t.width, t.height, t.rgba}; }

int add_texture(ModelPreview& m, const ImageRgba& im, uint32_t file_id, bool is_normal) {
    ModelTextureCPU t;
    t.fileId = file_id;
    t.width = im.w;
    t.height = im.h;
    t.rgba = im.px;
    t.fmt = is_normal ? "baked normal" : "baked diffuse";
    t.channels = is_normal ? "RGB" : "RGBA";
    t.isNormal = is_normal;
    t.hasCutout = !is_normal && alpha_cuts(im);
    m.textures.push_back(std::move(t));
    return static_cast<int>(m.textures.size() - 1);
}

std::optional<BakedTextures> bake_part(Gw2Dat& dat, const composite::CompositeFileData& fd,
                                       const std::vector<character::ManifestDye>& manifest_dyes,
                                       const std::optional<ColorMatrix>& rest, bool preview) {
    BakedTextures out;
    // The highest resolution GW2 ships: a texture's full-size copy when there is
    // one. The Composite names the reduced entry, which the atlas draws at 2x;
    // a copy twice that size draws at 1x.
    auto scale_of = [](int exact_w, int used_w) { return used_w > 0 && exact_w > 0 ? 2.0f * exact_w / used_w : 2.0f; };
    int exact_w = 0;
    auto decode = [&](uint32_t id, ModelTextureCPU& t, int* w) {
        if (!preview) return decode_texture_full(dat, id, t, w);
        if (!decode_texture_rgba(dat, id, t)) return false;
        if (w) *w = t.width;
        return true;
    };
    if (!decode(fd.texture_base, out.base, &exact_w)) return std::nullopt;
    out.scale = scale_of(exact_w, out.base.width);
    std::array<ModelTextureCPU, 4> decoded;
    std::array<bool, 4> have{};
    int target_w = out.base.width, target_h = out.base.height;
    for (size_t i = 0; i < 4; ++i) {
        if (!fd.mask_dye[i] || !decode(fd.mask_dye[i], decoded[i], nullptr)) continue;
        have[i] = true;
        if (decoded[i].width > target_w) {  // a sharper mask: bake at its resolution
            target_w = decoded[i].width;
            target_h = decoded[i].height;
        }
    }
    if (target_w != out.base.width) {
        out.scale *= static_cast<float>(out.base.width) / static_cast<float>(target_w);
        ImageRgba up = resize_bilinear(to_image(out.base), target_w, target_h);
        out.base.width = up.w;
        out.base.height = up.h;
        out.base.rgba = std::move(up.px);
    }
    std::array<std::vector<uint8_t>, 4> masks;
    std::array<const std::vector<uint8_t>*, 4> mask_px{};
    std::array<std::optional<ColorMatrix>, 4> dyes{};
    for (size_t i = 0; i < 4; ++i) {
        if (!have[i]) continue;
        ModelTextureCPU& m = decoded[i];
        if (m.width != out.base.width || m.height != out.base.height)  // resize rather than drop the channel
            m.rgba = resize_bilinear(to_image(m), out.base.width, out.base.height).px;
        masks[i] = std::move(m.rgba);
        mask_px[i] = &masks[i];
    }
    for (const character::ManifestDye& d : manifest_dyes) {
        if (!d.shift || d.slot < 0 || d.slot >= 4) continue;
        dyes[static_cast<size_t>(d.slot)] = dye_matrix(*d.shift);
        (mask_px[static_cast<size_t>(d.slot)] ? out.dyed : out.undyed)++;
    }
    bake_dyes(out.base.rgba, out.base.width, out.base.height, mask_px, dyes, rest);
    if (fd.texture_normal && !preview) {
        ModelTextureCPU n;
        int normal_exact_w = 0;
        if (decode_texture_full(dat, fd.texture_normal, n, &normal_exact_w)) {
            out.normal_scale = scale_of(normal_exact_w, n.width);
            out.normal = std::move(n);
        }
    }
    return out;
}

bool is_skin_mesh(const ModelPreview& m, const ModelMeshCPU& mesh) {
    if (mesh.materialIndex >= m.materials.size()) return false;
    std::string name = m.materials[mesh.materialIndex].materialName;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    return name.find("skin") != std::string::npos;
}

std::vector<std::pair<float, float>> wrapped_uvs(const ModelMeshCPU& mesh) {
    std::vector<std::pair<float, float>> out;
    out.reserve(mesh.vertices.size());
    for (const GVertex& v : mesh.vertices) out.push_back({wrap_uv(v.u), wrap_uv(v.v)});
    return out;
}

} // namespace castlemist::ripper::detail
