#include "castlemist/ripper/piece_export.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <vector>

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/ripper/atlas.h"
#include "castlemist/ripper/dye.h"
#include "internal.h"

namespace castlemist::ripper {
namespace {

using detail::add_texture;
using detail::bake_part;
using detail::BakedTextures;
using detail::is_skin_mesh;
using detail::load_model;
using detail::to_image;

PieceExportResult skipped(std::string reason) {
    PieceExportResult r;
    r.status = "skipped";
    r.reason = std::move(reason);
    return r;
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

    std::optional<BakedTextures> baked = bake_part(dat, fd, piece.dyes);
    if (!baked) return skipped("texture failed to decode");
    const ModelTextureCPU& base = baked->base;
    const std::optional<ModelTextureCPU>& normal = baked->normal;
    const int dyed = baked->dyed, undyed = baked->undyed;

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
        info.skin = is_skin_mesh(*model, mesh);
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
