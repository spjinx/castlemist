#include "castlemist/ripper/assemble.h"

#include <algorithm>
#include <array>
#include <exception>
#include <optional>
#include <set>

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/skeleton_merge.h"
#include "internal.h"

namespace castlemist::ripper {
namespace {

using character::CharacterManifest;
using character::ManifestPiece;

constexpr int kAtlas = 1024;

// One composite-backed part of the character (body, head or armor).
struct Part {
    std::string name;
    const composite::CompositeFileData* fd = nullptr;
    const ManifestPiece* piece = nullptr;  // dyes come from here (armor only)
    bool keep_mesh = true;                 // false: texture into the atlas only
    bool skin_meshes_are_part = false;     // body parts: their "skin" meshes ARE the part
    std::string hidden_reason;
};

bool is_armor(const ManifestPiece& p) {
    return p.skin_type == "Armor" || (p.skin_type.empty() && !p.weight_class.empty());
}

bool aquatic(const std::string& slot) { return slot.find("Aquatic") != std::string::npos; }

const ManifestPiece* find_slot(const CharacterManifest& m, const std::string& slot) {
    for (const ManifestPiece& p : m.pieces)
        if (p.slot == slot && p.status != character::PieceStatus::NoSkin) return &p;
    return nullptr;
}

// Weapon stow joint -> the race skeleton's holster joint.
struct AttachPair {
    const char* weapon;
    const char* body;
};
constexpr AttachPair kStow[] = {{"actionpoint:RStowBack", "actionpoint:RHolsterBack"},
                                {"actionpoint:LStowBack", "actionpoint:LHolsterBack"},
                                {"actionpoint:RStowHip", "actionpoint:RHolsterHip"},
                                {"actionpoint:LStowHip", "actionpoint:LHolsterHip"}};
constexpr AttachPair kRightHand{"actionpoint:RGripHand", "actionpoint:RightHand"};
constexpr AttachPair kLeftHand{"actionpoint:LGripHand", "actionpoint:LeftHand"};

bool has_joint(const ModelPreview& m, const char* name) {
    return std::any_of(m.joints.begin(), m.joints.end(), [&](const ModelJoint& j) { return j.name == name; });
}

} // namespace

const char* to_string(WeaponPlacement w) {
    switch (w) {
    case WeaponPlacement::Stowed: return "stowed";
    case WeaponPlacement::Hands: return "hands";
    case WeaponPlacement::None: return "none";
    }
    return "stowed";
}

AssemblyReport assemble_character(const CharacterManifest& manifest, const std::string& dat_path,
                                  const std::string& glb_path, const AssemblyOptions& opt) {
    AssemblyReport rep;
    Gw2Dat dat;
    try {
        load_dat_file(dat, dat_path);
    } catch (const std::exception& e) {
        rep.error = std::string("cannot open the dat: ") + e.what();
        return rep;
    }
    std::optional<composite::Composite> comp = load_composite(dat);
    if (!comp) {
        rep.error = "Composite file not found in this dat (open the index DB, or rebuild it)";
        return rep;
    }
    const std::string race_key = manifest.race + manifest.gender;
    const composite::CompositeRace* race = comp->race(race_key);
    if (!race) {
        rep.error = "unknown race/gender '" + race_key + "'";
        return rep;
    }
    auto entry = [&](uint64_t token) -> const composite::CompositeFileData* {
        auto it = race->file_data.find(token);
        return it == race->file_data.end() ? nullptr : &it->second;
    };

    // ---- the part list: body, head, armor ---------------------------------------
    std::vector<Part> parts;
    std::set<std::string> worn;  // armor slots that resolved to a composite entry
    std::vector<Part> armor_parts;
    for (const ManifestPiece& p : manifest.pieces) {
        if (!is_armor(p) || aquatic(p.slot) || p.status == character::PieceStatus::NoSkin) continue;
        const composite::CompositeFileData* fd = p.skin_token ? entry(p.skin_token) : nullptr;
        if (!fd) {
            rep.parts.push_back({p.slot, "dropped", "no appearance for " + race_key, 0});
            continue;
        }
        armor_parts.push_back({p.slot, fd, &p, true, false, {}});
        worn.insert(p.slot);
    }

    if (opt.skin_style >= 0 && static_cast<size_t>(opt.skin_style) < race->skin_styles.size()) {
        const auto& style = race->skin_styles[static_cast<size_t>(opt.skin_style)];
        const char* names[4] = {"body chest", "body feet", "body hands", "body legs"};
        const char* covered_by[4] = {"Coat", "Boots", "Gloves", "Leggings"};
        for (int k = 0; k < 4; ++k) {
            Part body{names[k], entry(style[k]), nullptr, !worn.count(covered_by[k]), true, {}};
            if (!body.keep_mesh) body.hidden_reason = std::string("under the ") + covered_by[k];
            if (body.fd) parts.push_back(body);
            else rep.parts.push_back({names[k], "dropped", "no body part for " + race_key, 0});
        }
    }
    auto head_part = [&](const char* name, const std::vector<uint64_t>& list, int index, bool keep,
                         const char* why_hidden) {
        if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
        const composite::CompositeFileData* fd = entry(list[static_cast<size_t>(index)]);
        if (!fd) return;
        Part p{name, fd, nullptr, keep, false, keep ? std::string() : std::string(why_hidden)};
        parts.push_back(p);
    };
    head_part("face", race->faces, opt.face, true, "");
    head_part("ears", race->ears, 0, true, "");
    head_part("hair", race->hair_styles, opt.hair, !worn.count("Helm"), "under the Helm");
    parts.insert(parts.end(), armor_parts.begin(), armor_parts.end());  // armor paints over the body

    // ---- textures into the atlas, meshes onto the skeleton -------------------------
    ModelPreview character;
    ImageRgba diffuse{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    ImageRgba normal{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    for (size_t i = 0; i < normal.px.size(); i += 4) {
        normal.px[i] = 128;
        normal.px[i + 1] = 128;
        normal.px[i + 2] = 255;
        normal.px[i + 3] = 255;
    }
    std::vector<size_t> atlas_meshes;  // indices into character.meshes that sample the atlas

    for (const Part& part : parts) {
        std::optional<ModelPreview> model = detail::load_model(dat, part.fd->mesh_base);
        if (!model) {
            rep.parts.push_back({part.name, "dropped", "model failed to load", part.fd->mesh_base});
            continue;
        }
        std::vector<character::ManifestDye> no_dyes;
        std::optional<detail::BakedTextures> baked =
            detail::bake_part(dat, *part.fd, part.piece ? part.piece->dyes : no_dyes);
        if (!baked) {
            rep.parts.push_back({part.name, "dropped", "texture failed to decode", part.fd->mesh_base});
            continue;
        }
        const composite::BlitRectSet* set =
            part.fd->blit_set < comp->blit_sets.size() ? &comp->blit_sets[part.fd->blit_set] : nullptr;

        // The part's own meshes decide where its texture goes; a garment's
        // "skin" meshes sample the body's region instead.
        std::vector<std::pair<float, float>> uvs;
        for (const ModelMeshCPU& mesh : model->meshes) {
            if (!part.skin_meshes_are_part && detail::is_skin_mesh(*model, mesh)) continue;
            auto w = detail::wrapped_uvs(mesh);
            uvs.insert(uvs.end(), w.begin(), w.end());
        }
        AtlasRegion region = set ? region_for(*set, uvs) : AtlasRegion{};
        const bool in_atlas = !region.rects.empty();
        if (in_atlas) {
            blit(diffuse, detail::to_image(baked->base), region);
            if (baked->normal) blit(normal, detail::to_image(*baked->normal), region);
        }

        if (!part.keep_mesh) {
            rep.parts.push_back({part.name, "hidden", part.hidden_reason, part.fd->mesh_base});
            continue;
        }
        if (!in_atlas) {
            // Self-textured (e.g. some hair styles): its own baked texture on its own UVs.
            int d = detail::add_texture(*model, detail::to_image(baked->base), part.fd->texture_base, false);
            int n = baked->normal ? detail::add_texture(*model, detail::to_image(*baked->normal), part.fd->texture_normal, true) : -1;
            for (ModelMaterialCPU& mat : model->materials) {
                mat.diffuseTex = d;
                mat.normalTex = n;
                mat.extraTextures.clear();
            }
        } else {
            for (ModelMeshCPU& mesh : model->meshes)
                for (GVertex& v : mesh.vertices) {
                    v.u = wrap_uv(v.u);
                    v.v = wrap_uv(v.v);
                }
        }
        const size_t first = merge_into(character, *model);
        if (in_atlas)
            for (size_t mi = first; mi < character.meshes.size(); ++mi) atlas_meshes.push_back(mi);
        rep.parts.push_back({part.name, "used", "", part.fd->mesh_base});
    }
    if (character.joints.empty()) {
        rep.error = "no part of the character could be loaded";
        return rep;
    }

    // One material for everything that samples the atlas.
    {
        int d = detail::add_texture(character, diffuse, 0, false);
        int n = detail::add_texture(character, normal, 0, true);
        ModelMaterialCPU mat;
        mat.index = static_cast<uint32_t>(character.materials.size());
        mat.materialName = "CharacterAtlas";
        mat.diffuseTex = d;
        mat.normalTex = n;
        character.materials.push_back(mat);
        for (size_t mi : atlas_meshes) character.meshes[mi].materialIndex = mat.index;
    }

    // ---- back item: merges onto the skeleton by joint name ------------------------
    if (const ManifestPiece* back = find_slot(manifest, "Backpack"); back && !back->file_ids.empty()) {
        std::optional<ModelPreview> m = detail::load_model(dat, back->file_ids[0]);
        if (m && !m->joints.empty()) {
            merge_into(character, *m);
            rep.parts.push_back({"Backpack", "used", "", back->file_ids[0]});
        } else {
            rep.parts.push_back({"Backpack", "dropped", "model failed to load", back->file_ids[0]});
        }
    }

    // ---- weapons (set A) ------------------------------------------------------------
    for (const char* slot : {"WeaponA1", "WeaponA2"}) {
        const ManifestPiece* w = find_slot(manifest, slot);
        if (!w || w->file_ids.empty()) continue;
        if (opt.weapons == WeaponPlacement::None) {
            rep.parts.push_back({slot, "dropped", "weapons: none", w->file_ids[0]});
            continue;
        }
        std::optional<ModelPreview> m = detail::load_model(dat, w->file_ids[0]);
        if (!m) {
            rep.parts.push_back({slot, "dropped", "model failed to load", w->file_ids[0]});
            continue;
        }
        std::optional<AttachPair> at;
        if (opt.weapons == WeaponPlacement::Hands) {
            const AttachPair hand = std::string(slot) == "WeaponA2" ? kLeftHand : kRightHand;
            if (has_joint(*m, hand.weapon)) at = hand;
            else if (has_joint(*m, kRightHand.weapon)) at = kRightHand;  // off-hands often only carry RGripHand
        }
        if (!at)
            for (const AttachPair& p : kStow)
                if (has_joint(*m, p.weapon)) { at = p; break; }
        if (at && attach_rigid(character, *m, at->weapon, at->body))
            rep.parts.push_back({slot, "used", std::string("on ") + at->body, w->file_ids[0]});
        else
            rep.parts.push_back({slot, "dropped", "no matching attach point", w->file_ids[0]});
    }

    // GW2 characters stand along -Z in inches; the avatar should stand along
    // glTF +Y in metres, still facing +Z: -90 about X, then 180 about the
    // forward axis, and inches -> metres.
    exportgltf::GltfExportOptions gopts;
    gopts.rootRotation = {0.0, -0.70710678, 0.70710678, 0.0};
    gopts.rootScale = 0.0254;
    exportgltf::GltfExportResult g = exportgltf::export_model_gltf(character, glb_path, gopts);
    if (!g.ok) {
        rep.error = "glTF export failed: " + g.error;
        return rep;
    }
    rep.joints = character.joints.size();
    rep.ok = true;
    return rep;
}

} // namespace castlemist::ripper
