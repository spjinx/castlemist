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

// The rect's block of a texture that starts at atlas (ax, ay) (see
// detail::place_texture: not always the rect's corner).
ImageRgba crop_at(const ImageRgba& tex, const composite::BlitRect& rect, float scale, uint32_t ax, uint32_t ay) {
    const int ox = std::min(tex.w, static_cast<int>(static_cast<float>(rect.x0 - std::min(ax, rect.x0)) / scale));
    const int oy = std::min(tex.h, static_cast<int>(static_cast<float>(rect.y0 - std::min(ay, rect.y0)) / scale));
    if (ox == 0 && oy == 0) return crop_piece(tex, rect, scale);
    ImageRgba shifted{tex.w - ox, tex.h - oy, {}};
    shifted.px.resize(static_cast<size_t>(shifted.w) * shifted.h * 4);
    for (int y = 0; y < shifted.h; ++y)
        std::copy_n(tex.px.data() + (static_cast<size_t>(y + oy) * tex.w + ox) * 4, static_cast<size_t>(shifted.w) * 4,
                    shifted.px.data() + static_cast<size_t>(y) * shifted.w * 4);
    return crop_piece(shifted, rect, scale);
}

PieceExportResult write(ModelPreview model, const std::string& glb_path, PieceExportResult r) {
    detail::mirror_x(model);  // GW2 is left-handed: un-mirror for glTF
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
    if (!rect) {
        // A piece spread over several rects (Baggy Cargo Pants, Carapace
        // Vestments): the block covering every rect its meshes touch, its
        // texture anchored at their top-left as when it is composited.
        std::vector<std::pair<float, float>> uvs;
        for (size_t mi = 0; mi < infos.size(); ++mi) {
            if (infos[mi].skin || infos[mi].verts == 0) continue;
            for (const auto& v : model->meshes[mi].vertices) uvs.emplace_back(wrap_uv(v.u), wrap_uv(v.v));
        }
        const AtlasRegion region = region_for(set, uvs);
        if (!region.rects.empty()) {
            composite::BlitRect box = region.rects.front();
            box.x0 = region.ax;
            box.y0 = region.ay;
            for (const composite::BlitRect& r : region.rects) {
                box.x1 = std::max(box.x1, r.x1);
                box.y1 = std::max(box.y1, r.y1);
            }
            rect = box;
            for (size_t mi = 0; mi < infos.size(); ++mi)
                if (!infos[mi].skin && infos[mi].verts != 0) armor_meshes.push_back(mi);
        }
    }
    if (!rect) return skipped("no mesh inside an atlas rect");

    // Where the texture starts, the way the atlas places it; the normal map,
    // the same block at its own resolution.
    AtlasRegion placed{{*rect}, rect->x0, rect->y0};
    detail::place_texture(placed, to_image(base), baked->scale,
                          detail::uv_coverage(*model, static_cast<int>(kAtlasSize), static_cast<int>(kAtlasSize), false));
    int diffuse_idx =
        add_texture(*model, crop_at(to_image(base), *rect, baked->scale, placed.ax, placed.ay), fd.texture_base, false);
    int normal_idx = normal ? add_texture(*model, crop_at(to_image(*normal), *rect, baked->normal_scale, placed.ax, placed.ay),
                                          fd.texture_normal, true)
                            : -1;
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

PieceExportResult export_undergarment(const PieceContext& ctx, uint64_t token, const std::string& glb_path) {
    const composite::CompositeRace* race = ctx.comp ? ctx.comp->race(ctx.race_key) : nullptr;
    if (!race) return skipped("no appearance for " + ctx.race_key);
    auto it = race->file_data.find(token);
    if (it == race->file_data.end()) return skipped("none for " + ctx.race_key);
    character::ManifestPiece undergarment;
    undergarment.dyes = detail::undergarment_dyes();
    PieceExportResult r = export_armor(ctx, undergarment, it->second, glb_path);
    if (r.ok) r.status = "undergarment";
    return r;
}

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
