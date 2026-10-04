#include "castlemist/ripper/piece_export.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/ripper/atlas.h"
#include "castlemist/ripper/dye.h"

namespace castlemist::ripper {
namespace {

PieceExportResult skipped(std::string reason) {
    PieceExportResult r;
    r.status = "skipped";
    r.reason = std::move(reason);
    return r;
}

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

// A real alpha cut-out: a minority of texels below the game's 0.25 threshold.
bool alpha_cuts(const ImageRgba& im) {
    size_t n = im.px.size() / 4, low = 0;
    for (size_t i = 0; i < n; ++i) low += im.px[i * 4 + 3] < 64;
    return n && low > n / 100 && low < n * 95 / 100;
}

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

PieceExportResult write(const ModelPreview& model, const std::string& glb_path, PieceExportResult r) {
    exportgltf::GltfExportResult g = exportgltf::export_model_gltf(model, glb_path);
    if (!g.ok) return skipped("glTF export failed: " + g.error);
    r.ok = true;
    r.glb_path = glb_path;  // UTF-8, as the caller gave it
    return r;
}

PieceExportResult export_armor(const PieceContext& ctx, const character::ManifestPiece& piece,
                               const composite::CompositeFileData& fd, const std::string& glb_path) {
    Gw2Dat& dat = *ctx.dat;
    std::optional<ModelPreview> model = load_model(dat, fd.mesh_base);
    if (!model) return skipped("model failed to load");

    ModelTextureCPU base;
    if (!decode_texture_rgba(dat, fd.texture_base, base)) return skipped("texture failed to decode");

    // Dyes: manifest dye slot i -> mask i, each with its own color matrix.
    std::array<std::vector<uint8_t>, 4> masks;
    std::array<const std::vector<uint8_t>*, 4> mask_px{};
    std::array<std::optional<ColorMatrix>, 4> dyes{};
    for (size_t i = 0; i < 4; ++i) {
        ModelTextureCPU m;
        if (!fd.mask_dye[i] || !decode_texture_rgba(dat, fd.mask_dye[i], m)) continue;
        if (m.width != base.width || m.height != base.height)  // resize rather than drop the channel
            m.rgba = resize_nearest(to_image(m), base.width, base.height).px;
        masks[i] = std::move(m.rgba);
        mask_px[i] = &masks[i];
    }
    int dyed = 0, undyed = 0;
    for (const character::ManifestDye& d : piece.dyes) {
        if (!d.shift || d.slot < 0 || d.slot >= 4) continue;
        dyes[static_cast<size_t>(d.slot)] = dye_matrix(*d.shift);
        (mask_px[static_cast<size_t>(d.slot)] ? dyed : undyed)++;
    }
    bake_dyes(base.rgba, base.width, base.height, mask_px, dyes);

    std::optional<ModelTextureCPU> normal;
    if (fd.texture_normal) {
        ModelTextureCPU n;
        if (decode_texture_rgba(dat, fd.texture_normal, n)) normal = std::move(n);
    }

    if (fd.blit_set >= ctx.comp->blit_sets.size()) return skipped("no mesh inside an atlas rect");
    const composite::BlitRectSet& set = ctx.comp->blit_sets[fd.blit_set];

    // The armor meshes are the non-Skin ones whose UVs sit inside the chosen
    // atlas rect; the rest (the Skin material) sample the body texture and stay
    // untextured.
    std::vector<MeshUvInfo> infos;
    for (const auto& mesh : model->meshes) {
        MeshUvInfo info;
        info.verts = mesh.vertices.size();
        info.umin = info.vmin = 1e9f;
        info.umax = info.vmax = -1e9f;
        for (const auto& v : mesh.vertices) {
            const float u = wrap_uv(v.u), vv = wrap_uv(v.v);  // mirrored halves sit at u-1
            info.umin = std::min(info.umin, u); info.umax = std::max(info.umax, u);
            info.vmin = std::min(info.vmin, vv); info.vmax = std::max(info.vmax, vv);
        }
        if (mesh.materialIndex < model->materials.size()) {
            std::string name = model->materials[mesh.materialIndex].materialName;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
            info.skin = name.find("skin") != std::string::npos;  // "Skin", "SylvariSkin1", ...
        }
        infos.push_back(info);
    }
    std::optional<composite::BlitRect> rect = choose_armor_rect(set, infos);
    std::vector<size_t> armor_meshes;
    if (rect) {
        for (size_t mi = 0; mi < infos.size(); ++mi) {
            if (infos[mi].skin || infos[mi].verts == 0) continue;
            auto r = piece_rect(set, infos[mi].umin, infos[mi].vmin, infos[mi].umax, infos[mi].vmax);
            if (r && r->x0 == rect->x0 && r->y0 == rect->y0 && r->x1 == rect->x1 && r->y1 == rect->y1)
                armor_meshes.push_back(mi);
        }
    }
    if (!rect) return skipped("no mesh inside an atlas rect");

    int diffuse_idx = add_texture(*model, crop_piece(to_image(base), *rect), fd.texture_base, false);
    int normal_idx = normal ? add_texture(*model, crop_piece(to_image(*normal), *rect), fd.texture_normal, true) : -1;
    for (size_t mi : armor_meshes) {
        ModelMeshCPU& mesh = model->meshes[mi];
        for (auto& v : mesh.vertices) {
            v.u = wrap_uv(v.u);
            v.v = wrap_uv(v.v);
            remap_uv(v.u, v.v, *rect);
        }
        if (mesh.materialIndex < model->materials.size()) {
            ModelMaterialCPU& mat = model->materials[mesh.materialIndex];
            mat.diffuseTex = diffuse_idx;
            mat.normalTex = normal_idx;
            mat.diffuseUv = mat.normalUv = 0;
            mat.extraTextures.clear();  // their UVs addressed the atlas, which no longer exists
        }
    }

    PieceExportResult r;
    r.status = "armor";
    r.dyed_channels = dyed;
    r.undyed_channels = undyed;
    r.mesh = fd.mesh_base;
    r.texture_base = fd.texture_base;
    return write(*model, glb_path, r);
}

} // namespace

PieceExportResult export_piece(const PieceContext& ctx, const character::ManifestPiece& piece,
                               const std::string& glb_path) {
    if (piece.status == character::PieceStatus::NoSkin || piece.skin_id == 0) return skipped("no skin");

    // Armor goes through the Composite. The skin type decides, not the token:
    // weapons carry unrelated data in the same field (skin 11871 has 1 there).
    // With no API skin type, a token the Composite knows still counts as armor.
    // Manifests saved before skin_type existed still mark armor by its weight class.
    const bool armor = piece.skin_type == "Armor" || (piece.skin_type.empty() && !piece.weight_class.empty());
    if (piece.skin_token != 0 && (armor || piece.skin_type.empty())) {
        const composite::CompositeRace* race = ctx.comp ? ctx.comp->race(ctx.race_key) : nullptr;
        if (race) {
            auto it = race->file_data.find(piece.skin_token);
            if (it != race->file_data.end()) return export_armor(ctx, piece, it->second, glb_path);
        }
        if (armor) return skipped("no appearance for " + ctx.race_key);
    }
    if (armor) return skipped("no appearance token (rebuild the content map)");

    if (piece.file_ids.empty()) return skipped("no skin");
    std::optional<ModelPreview> model = load_model(*ctx.dat, piece.file_ids[0]);
    if (!model) return skipped("model failed to load");
    PieceExportResult r;
    r.status = "model";
    r.mesh = piece.file_ids[0];
    return write(*model, glb_path, r);
}

} // namespace castlemist::ripper
