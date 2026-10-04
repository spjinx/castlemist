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
    uint32_t base = get_by_base_id(dat, file_id);
    if (base == 0 || base > dat.mft_data_list.size()) return std::nullopt;
    try {
        ExtractedEntry e = extract_entry(dat.file_info.file_path, dat.mft_data_list[base - 1]);
        if (!e.model) return std::nullopt;
        return *e.model;
    } catch (const std::exception&) {
        return std::nullopt;
    }
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
                                       const std::vector<character::ManifestDye>& manifest_dyes) {
    BakedTextures out;
    if (!decode_texture_rgba(dat, fd.texture_base, out.base)) return std::nullopt;
    std::array<std::vector<uint8_t>, 4> masks;
    std::array<const std::vector<uint8_t>*, 4> mask_px{};
    std::array<std::optional<ColorMatrix>, 4> dyes{};
    for (size_t i = 0; i < 4; ++i) {
        ModelTextureCPU m;
        if (!fd.mask_dye[i] || !decode_texture_rgba(dat, fd.mask_dye[i], m)) continue;
        if (m.width != out.base.width || m.height != out.base.height)  // resize rather than drop the channel
            m.rgba = resize_nearest(to_image(m), out.base.width, out.base.height).px;
        masks[i] = std::move(m.rgba);
        mask_px[i] = &masks[i];
    }
    for (const character::ManifestDye& d : manifest_dyes) {
        if (!d.shift || d.slot < 0 || d.slot >= 4) continue;
        dyes[static_cast<size_t>(d.slot)] = dye_matrix(*d.shift);
        (mask_px[static_cast<size_t>(d.slot)] ? out.dyed : out.undyed)++;
    }
    bake_dyes(out.base.rgba, out.base.width, out.base.height, mask_px, dyes);
    if (fd.texture_normal) {
        ModelTextureCPU n;
        if (decode_texture_rgba(dat, fd.texture_normal, n)) out.normal = std::move(n);
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
