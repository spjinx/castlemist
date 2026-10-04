#include "castlemist/ripper/piece_export.h"

#include <algorithm>
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
    r.glb_path = g.glbPath.empty() ? glb_path : g.glbPath;
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
    std::array<ModelTextureCPU, 4> masks;
    std::array<const std::vector<uint8_t>*, 4> mask_px{};
    std::array<std::optional<ColorMatrix>, 4> dyes{};
    for (size_t i = 0; i < 4; ++i) {
        if (fd.mask_dye[i] && decode_texture_rgba(dat, fd.mask_dye[i], masks[i]) && masks[i].width == base.width &&
            masks[i].height == base.height)
            mask_px[i] = &masks[i].rgba;
    }
    for (const character::ManifestDye& d : piece.dyes)
        if (d.shift && d.slot >= 0 && d.slot < 4) dyes[static_cast<size_t>(d.slot)] = dye_matrix(*d.shift);
    bake_dyes(base.rgba, base.width, base.height, mask_px, dyes);

    std::optional<ModelTextureCPU> normal;
    if (fd.texture_normal) {
        ModelTextureCPU n;
        if (decode_texture_rgba(dat, fd.texture_normal, n)) normal = std::move(n);
    }

    if (fd.blit_set >= ctx.comp->blit_sets.size()) return skipped("no mesh inside an atlas rect");
    const composite::BlitRectSet& set = ctx.comp->blit_sets[fd.blit_set];

    // The armor meshes are the ones whose UVs sit inside one atlas rect; the
    // rest (the Skin material) sample the body texture and stay untextured.
    std::optional<composite::BlitRect> rect;
    std::vector<size_t> armor_meshes;
    for (size_t mi = 0; mi < model->meshes.size(); ++mi) {
        const auto& verts = model->meshes[mi].vertices;
        if (verts.empty()) continue;
        float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
        for (const auto& v : verts) {
            u0 = std::min(u0, v.u); u1 = std::max(u1, v.u);
            v0 = std::min(v0, v.v); v1 = std::max(v1, v.v);
        }
        auto r = piece_rect(set, u0, v0, u1, v1);
        if (!r) continue;
        if (!rect) rect = r;
        if (r->x0 == rect->x0 && r->y0 == rect->y0) armor_meshes.push_back(mi);
    }
    if (!rect) return skipped("no mesh inside an atlas rect");

    int diffuse_idx = add_texture(*model, crop_piece(to_image(base), *rect), fd.texture_base, false);
    int normal_idx = normal ? add_texture(*model, crop_piece(to_image(*normal), *rect), fd.texture_normal, true) : -1;
    for (size_t mi : armor_meshes) {
        ModelMeshCPU& mesh = model->meshes[mi];
        for (auto& v : mesh.vertices) remap_uv(v.u, v.v, *rect);
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
    const bool armor = piece.skin_type == "Armor";
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
