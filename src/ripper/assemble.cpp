#include "castlemist/ripper/assemble.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/skeleton_merge.h"
#include "internal.h"

namespace castlemist::ripper {
namespace {

namespace fs = std::filesystem;
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
    bool scalp_only = false;               // hair under a helm: keep its skin (scalp) meshes, drop the strands
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
constexpr AttachPair kBackStow{"actionpoint:CStowBack", "actionpoint:CHolsterBack"};
constexpr AttachPair kRightHand{"actionpoint:RGripHand", "actionpoint:RightHand"};
constexpr AttachPair kLeftHand{"actionpoint:LGripHand", "actionpoint:LeftHand"};

bool has_joint(const ModelPreview& m, const char* name) {
    return std::any_of(m.joints.begin(), m.joints.end(), [&](const ModelJoint& j) { return j.name == name; });
}

// A back item rigged on its own: it has the back stow point and shares no
// weighted bone with the body skeleton except the root.
bool back_is_self_rigged(const ModelPreview& back, const ModelPreview& body) {
    if (!has_joint(back, kBackStow.weapon)) return false;
    std::vector<bool> weighted(back.joints.size(), false);
    for (const ModelMeshCPU& mesh : back.meshes)
        for (const GVertex& v : mesh.vertices)
            for (int k = 0; k < 4; ++k)
                if (v.bwt[k] > 0 && v.bidx[k] < weighted.size()) weighted[v.bidx[k]] = true;
    for (size_t i = 0; i < back.joints.size(); ++i)
        if (weighted[i] && back.joints[i].parent >= 0 && has_joint(body, back.joints[i].name.c_str())) return false;
    return true;
}

std::string sanitize(std::string s) {
    for (char& c : s)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) c = '_';
    return s.empty() ? "piece" : s;
}

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string to_utf8(const fs::path& p) {
    std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The open dat, the Composite and the character's race, shared by every output.
struct Context {
    Gw2Dat dat;
    composite::Composite comp;
    const composite::CompositeRace* race = nullptr;
    std::string race_key;
    ModelPreview skeleton;  // the race skeleton alone (no meshes), seeding every output

    // Every weapon of the kit, loaded once and placed once for all outputs (so
    // separate files don't stack two weapons on one holster).
    struct Weapon {
        const ManifestPiece* piece = nullptr;
        std::optional<ModelPreview> model;
        std::optional<AttachPair> at;
        std::string shares;  // the earlier slot already on that holster
    };
    std::map<std::string, Weapon> weapons;

    const composite::CompositeFileData* entry(uint64_t token) const {
        auto it = race->file_data.find(token);
        return it == race->file_data.end() ? nullptr : &it->second;
    }
};

// Loads every weapon of the kit and gives each its attach point: the active
// set in the hands when asked, the rest spread over the holsters.
void plan_weapons(Context& ctx, const CharacterManifest& manifest, const AssemblyOptions& opt) {
    std::vector<std::string> stowed_slots;
    std::vector<std::vector<std::string>> stow_joints;
    for (const char* slot : kWeaponSlots) {
        const ManifestPiece* piece = find_slot(manifest, slot);
        if (!piece || piece->file_ids.empty()) continue;
        Context::Weapon& w = ctx.weapons[slot];
        w.piece = piece;
        if (opt.weapons == WeaponPlacement::None) continue;
        w.model = detail::load_model(ctx.dat, piece->file_ids[0]);
        if (!w.model) continue;
        const std::string s = slot;
        if (opt.weapons == WeaponPlacement::Hands && (s == "WeaponA1" || s == "WeaponA2")) {
            const AttachPair hand = s == "WeaponA2" ? kLeftHand : kRightHand;
            if (has_joint(*w.model, hand.weapon)) w.at = hand;
            else if (has_joint(*w.model, kRightHand.weapon)) w.at = kRightHand;  // off-hands often only carry RGripHand
            if (w.at) continue;
        }
        std::vector<std::string> own;
        for (const AttachPair& p : kStow)
            if (has_joint(*w.model, p.weapon)) own.push_back(p.weapon);
        stowed_slots.push_back(s);
        stow_joints.push_back(std::move(own));
    }
    const std::vector<std::string> chosen = choose_holsters(stowed_slots, stow_joints);
    std::map<std::string, std::string> holder;  // body holster -> first slot on it
    for (size_t i = 0; i < stowed_slots.size(); ++i) {
        Context::Weapon& w = ctx.weapons[stowed_slots[i]];
        for (const AttachPair& p : kStow)
            if (chosen[i] == p.weapon) w.at = p;
        if (!w.at) continue;
        auto [it, fresh] = holder.emplace(w.at->body, stowed_slots[i]);
        if (!fresh) w.shares = it->second;
    }
}

std::unique_ptr<Context> open_context(const CharacterManifest& manifest, const std::string& dat_path,
                                      const AssemblyOptions& opt, AssemblyReport& rep) {
    auto ctx = std::make_unique<Context>();
    try {
        load_dat_file(ctx->dat, dat_path);
    } catch (const std::exception& e) {
        rep.error = std::string("cannot open the dat: ") + e.what();
        return nullptr;
    }
    std::optional<composite::Composite> comp = load_composite(ctx->dat);
    if (!comp) {
        rep.error = "Composite file not found in this dat (open the index DB, or rebuild it)";
        return nullptr;
    }
    ctx->comp = std::move(*comp);
    ctx->race_key = manifest.race + manifest.gender;
    ctx->race = ctx->comp.race(ctx->race_key);
    if (!ctx->race) {
        rep.error = "unknown race/gender '" + ctx->race_key + "'";
        return nullptr;
    }
    // The race skeleton, taken from the bare chest (every body part and armor
    // piece of the race shares it).
    if (opt.skin_style >= 0 && static_cast<size_t>(opt.skin_style) < ctx->race->skin_styles.size()) {
        if (const auto* chest = ctx->entry(ctx->race->skin_styles[static_cast<size_t>(opt.skin_style)][0]))
            if (auto m = detail::load_model(ctx->dat, chest->mesh_base)) {
                ctx->skeleton.joints = m->joints;
                ctx->skeleton.skeletonVersion = m->skeletonVersion;
                ctx->skeleton.skeletonType = m->skeletonType;
                ctx->skeleton.externalSkeletonRef = m->externalSkeletonRef;
            }
    }
    if (ctx->skeleton.joints.empty()) {
        rep.error = "the race skeleton could not be loaded";
        return nullptr;
    }
    plan_weapons(*ctx, manifest, opt);
    return ctx;
}

// What goes into one output file.
struct Selection {
    bool body_meshes = true;            // false: bare-body textures only (armor skin patches need them)
    bool hide_body_under_armor = true;  // combined: drop the bare part an armor piece covers
    bool head = true;
    bool hair_under_helm_scalp_only = true;
    std::vector<const ManifestPiece*> armor;  // pieces whose meshes go in
    std::set<std::string> worn;               // every armor slot the character wears
    bool back = false;
    std::vector<std::string> weapons;         // slots
    bool drop_shared_holster = false;         // combined: no two weapons on one holster
    std::string file;                         // separate mode: recorded on the report's parts
};

// Builds one output model on the race skeleton; appends to the report.
ModelPreview build(Context& ctx, const CharacterManifest& manifest, const AssemblyOptions& opt, const Selection& sel,
                   AssemblyReport& rep) {
    const composite::CompositeRace* race = ctx.race;
    auto report = [&](std::string name, std::string status, std::string reason, uint32_t mesh) {
        rep.parts.push_back({std::move(name), std::move(status), std::move(reason), mesh, sel.file});
    };

    // ---- the part list: body, head, armor ---------------------------------------
    std::vector<Part> parts;
    if (opt.skin_style >= 0 && static_cast<size_t>(opt.skin_style) < race->skin_styles.size()) {
        const auto& style = race->skin_styles[static_cast<size_t>(opt.skin_style)];
        const char* names[4] = {"body chest", "body feet", "body hands", "body legs"};
        const char* covered_by[4] = {"Coat", "Boots", "Gloves", "Leggings"};
        for (int k = 0; k < 4; ++k) {
            const composite::CompositeFileData* fd = ctx.entry(style[k]);
            if (!fd) {
                if (sel.body_meshes) report(names[k], "dropped", "no body part for " + ctx.race_key, 0);
                continue;
            }
            Part body{names[k], fd, nullptr, sel.body_meshes, true, {}};
            if (sel.body_meshes && sel.hide_body_under_armor && sel.worn.count(covered_by[k])) {
                body.keep_mesh = false;
                body.hidden_reason = std::string("under the ") + covered_by[k];
            }
            if (!sel.body_meshes) body.hidden_reason = "texture only";
            parts.push_back(body);
        }
    }
    if (sel.head) {
        auto head_part = [&](const char* name, const std::vector<uint64_t>& list, int index) {
            if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
            if (const composite::CompositeFileData* fd = ctx.entry(list[static_cast<size_t>(index)]))
                parts.push_back(Part{name, fd, nullptr, true, false, {}});
        };
        head_part("face", race->faces, opt.face);
        head_part("ears", race->ears, 0);
        head_part("hair", race->hair_styles, opt.hair);
        if (sel.hair_under_helm_scalp_only && sel.worn.count("Helm") && !parts.empty() && parts.back().name == "hair")
            parts.back().scalp_only = true;
    }
    for (const ManifestPiece* p : sel.armor) {
        const composite::CompositeFileData* fd = p->skin_token ? ctx.entry(p->skin_token) : nullptr;
        if (!fd) {
            report(p->slot, "dropped", "no appearance for " + ctx.race_key, 0);
            continue;
        }
        parts.push_back(Part{p->slot, fd, p, true, false, {}});  // armor paints over the body
    }

    // ---- textures into the atlas, meshes onto the skeleton -------------------------
    ModelPreview out = ctx.skeleton;
    ImageRgba diffuse{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    ImageRgba normal{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    for (size_t i = 0; i < normal.px.size(); i += 4) {
        normal.px[i] = 128;
        normal.px[i + 1] = 128;
        normal.px[i + 2] = 255;
        normal.px[i + 3] = 255;
    }
    std::vector<size_t> atlas_meshes;  // indices into out.meshes that sample the atlas

    for (const Part& part : parts) {
        std::optional<ModelPreview> model = detail::load_model(ctx.dat, part.fd->mesh_base);
        if (!model) {
            report(part.name, "dropped", "model failed to load", part.fd->mesh_base);
            continue;
        }
        std::vector<character::ManifestDye> no_dyes;
        std::optional<detail::BakedTextures> baked =
            detail::bake_part(ctx.dat, *part.fd, part.piece ? part.piece->dyes : no_dyes);
        if (!baked) {
            report(part.name, "dropped", "texture failed to decode", part.fd->mesh_base);
            continue;
        }
        const composite::BlitRectSet* set =
            part.fd->blit_set < ctx.comp.blit_sets.size() ? &ctx.comp.blit_sets[part.fd->blit_set] : nullptr;

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
            // Hair and bald-scalp layers paint over the face's region the way the
            // game composites them (by their alpha); everything else replaces.
            const BlitMode mode = part.name == "hair" ? BlitMode::Over : BlitMode::Replace;
            blit(diffuse, detail::to_image(baked->base), region, baked->scale, mode);
            if (baked->normal && mode == BlitMode::Replace)
                blit(normal, detail::to_image(*baked->normal), region, baked->normal_scale);
        }

        if (!part.keep_mesh) {
            if (sel.body_meshes) report(part.name, "hidden", part.hidden_reason, part.fd->mesh_base);
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
        if (part.scalp_only) {
            std::vector<ModelMeshCPU> scalp;
            for (ModelMeshCPU& mesh : model->meshes)
                if (detail::is_skin_mesh(*model, mesh)) scalp.push_back(std::move(mesh));
            model->meshes = std::move(scalp);
        }
        const size_t first = merge_into(out, *model);
        if (in_atlas)
            for (size_t mi = first; mi < out.meshes.size(); ++mi) atlas_meshes.push_back(mi);
        report(part.name, "used", part.scalp_only ? "scalp only, strands under the Helm" : "", part.fd->mesh_base);
    }

    // One material for everything that samples the atlas.
    if (!atlas_meshes.empty()) {
        int d = detail::add_texture(out, diffuse, 0, false);
        int n = detail::add_texture(out, normal, 0, true);
        ModelMaterialCPU mat;
        mat.index = static_cast<uint32_t>(out.materials.size());
        mat.materialName = "CharacterAtlas";
        mat.diffuseTex = d;
        mat.normalTex = n;
        out.materials.push_back(mat);
        for (size_t mi : atlas_meshes) out.meshes[mi].materialIndex = mat.index;
    }

    // ---- back item: merges onto the skeleton by joint name ------------------------
    if (sel.back) {
        if (const ManifestPiece* back = find_slot(manifest, "Backpack"); back && !back->file_ids.empty()) {
            std::optional<ModelPreview> m = detail::load_model(ctx.dat, back->file_ids[0]);
            if (m && !m->joints.empty()) {
                // Its own rig hung from the shared root (Mawdrey, wings, ...) goes
                // onto the back holster; one skinned to body bones (capes) merges
                // by name.
                if (back_is_self_rigged(*m, out) && attach_skinned(out, *m, kBackStow.weapon, kBackStow.body)) {
                    report("Backpack", "used", std::string("on ") + kBackStow.body, back->file_ids[0]);
                } else {
                    merge_into(out, *m);
                    report("Backpack", "used", "", back->file_ids[0]);
                }
            } else {
                report("Backpack", "dropped", "model failed to load", back->file_ids[0]);
            }
        }
    }

    // ---- weapons ----------------------------------------------------------------------
    for (const std::string& slot : sel.weapons) {
        auto it = ctx.weapons.find(slot);
        if (it == ctx.weapons.end()) continue;
        const Context::Weapon& w = it->second;
        const uint32_t file = w.piece->file_ids[0];
        if (opt.weapons == WeaponPlacement::None) {
            report(slot, "dropped", "weapons: none", file);
        } else if (!w.model) {
            report(slot, "dropped", "model failed to load", file);
        } else if (!w.at) {
            report(slot, "dropped", "no matching attach point", file);
        } else if (!w.shares.empty() && sel.drop_shared_holster) {
            report(slot, "dropped", std::string(w.at->body) + " is taken by " + w.shares, file);
        } else if (attach_rigid(out, *w.model, w.at->weapon, w.at->body)) {
            report(slot, "used", std::string("on ") + w.at->body + (w.shares.empty() ? "" : " (shared with " + w.shares + ")"),
                   file);
        } else {
            report(slot, "dropped", "no matching attach point", file);
        }
    }
    return out;
}

bool write_glb(const ModelPreview& model, const std::string& path, const AssemblyOptions& opt, AssemblyReport& rep) {
    // GW2 characters stand along -Z; the avatar should stand along glTF +Y,
    // still facing +Z: -90 about X, then 180 about the forward axis. Units stay
    // GW2's (inches) unless asked for metres.
    exportgltf::GltfExportOptions gopts;
    gopts.rootRotation = {0.0, -0.70710678, 0.70710678, 0.0};
    gopts.rootScale = opt.metres ? 0.0254 : 1.0;
    exportgltf::GltfExportResult g = exportgltf::export_model_gltf(model, path, gopts);
    if (!g.ok) rep.error = "glTF export failed: " + g.error;
    return g.ok;
}

std::set<std::string> worn_slots(Context& ctx, const CharacterManifest& m) {
    std::set<std::string> worn;
    for (const ManifestPiece& p : m.pieces)
        if (is_armor(p) && !aquatic(p.slot) && p.status != character::PieceStatus::NoSkin && p.skin_token &&
            ctx.entry(p.skin_token))
            worn.insert(p.slot);
    return worn;
}

std::vector<const ManifestPiece*> armor_pieces(const CharacterManifest& m) {
    std::vector<const ManifestPiece*> out;
    for (const ManifestPiece& p : m.pieces)
        if (is_armor(p) && !aquatic(p.slot) && p.status != character::PieceStatus::NoSkin) out.push_back(&p);
    return out;
}

} // namespace

const char* const kWeaponSlots[6] = {"WeaponA1", "WeaponA2", "WeaponB1", "WeaponB2", "WeaponAquaticA", "WeaponAquaticB"};

std::vector<std::string> choose_holsters(const std::vector<std::string>& slots,
                                         const std::vector<std::vector<std::string>>& stow_joints) {
    // The body holster a weapon stow joint goes to.
    auto holster = [](const std::string& joint) -> std::string {
        for (const AttachPair& p : kStow)
            if (joint == p.weapon) return p.body;
        return joint;
    };
    auto off_hand = [](const std::string& slot) { return slot.back() == '2' || slot.back() == 'B'; };
    const size_t n = slots.size();
    std::vector<size_t> pick(n, 0), best;
    long best_score = -1;
    // At most 6 weapons x 4 stow points: try every assignment.
    while (true) {
        long unshared = 0, priority = 0, side = 0;
        std::set<std::string> taken;
        for (size_t i = 0; i < n; ++i) {
            if (stow_joints[i].empty()) continue;
            const std::string& j = stow_joints[i][pick[i]];
            if (taken.insert(holster(j)).second) {
                ++unshared;
                priority += 1L << (n - i);
            }
            const bool right = j.rfind("actionpoint:R", 0) == 0;
            side += right != off_hand(slots[i]);
        }
        const long score = unshared * 1000000 + priority * 100 + side;
        if (score > best_score) {
            best_score = score;
            best = pick;
        }
        size_t i = 0;
        for (; i < n; ++i) {
            if (++pick[i] < std::max<size_t>(stow_joints[i].size(), 1)) break;
            pick[i] = 0;
        }
        if (i == n) break;
    }
    std::vector<std::string> out(n);
    for (size_t i = 0; i < n; ++i)
        if (!stow_joints[i].empty()) out[i] = stow_joints[i][best[i]];
    return out;
}

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
    std::unique_ptr<Context> ctx = open_context(manifest, dat_path, opt, rep);
    if (!ctx) return rep;
    Selection sel;
    sel.armor = armor_pieces(manifest);
    sel.worn = worn_slots(*ctx, manifest);
    sel.back = true;
    sel.weapons.assign(std::begin(kWeaponSlots), std::end(kWeaponSlots));
    sel.drop_shared_holster = true;
    ModelPreview model = build(*ctx, manifest, opt, sel, rep);
    if (!write_glb(model, glb_path, opt, rep)) return rep;
    rep.joints = model.joints.size();
    rep.ok = true;
    return rep;
}

AssemblyReport assemble_character_separate(const CharacterManifest& manifest, const std::string& dat_path,
                                           const std::string& out_dir, const AssemblyOptions& opt) {
    AssemblyReport rep;
    std::unique_ptr<Context> ctx = open_context(manifest, dat_path, opt, rep);
    if (!ctx) return rep;
    std::error_code ec;
    fs::create_directories(from_utf8(out_dir), ec);
    if (ec) {
        rep.error = "cannot create " + out_dir + ": " + ec.message();
        return rep;
    }
    auto write = [&](const ModelPreview& m, const std::string& file) {
        return write_glb(m, to_utf8(from_utf8(out_dir) / from_utf8(file)), opt, rep);
    };

    // The body: everything bare, the head with its full hair.
    Selection body;
    body.hide_body_under_armor = false;
    body.hair_under_helm_scalp_only = false;
    body.file = "body.glb";
    ModelPreview body_model = build(*ctx, manifest, opt, body, rep);
    if (!write(body_model, body.file)) return rep;
    rep.joints = body_model.joints.size();

    // Each piece on its own, on the same skeleton.
    int n = 0;
    auto piece_file = [&](const std::string& slot, const std::string& skin) {
        char prefix[8];
        std::snprintf(prefix, sizeof prefix, "%02d_", ++n);
        return prefix + sanitize(slot) + "_" + sanitize(skin) + ".glb";
    };
    for (const ManifestPiece* p : armor_pieces(manifest)) {
        Selection s;
        s.body_meshes = false;  // bare-body textures still go in: the piece's skin patches sample them
        s.head = false;
        s.armor = {p};
        s.file = piece_file(p->slot, p->skin_name);
        ModelPreview m = build(*ctx, manifest, opt, s, rep);
        if (m.meshes.empty()) continue;  // dropped (reported)
        if (!write(m, s.file)) return rep;
    }
    if (const ManifestPiece* back = find_slot(manifest, "Backpack"); back && !back->file_ids.empty()) {
        Selection s;
        s.body_meshes = false;
        s.head = false;
        s.back = true;
        s.file = piece_file("Backpack", back->skin_name);
        ModelPreview m = build(*ctx, manifest, opt, s, rep);
        if (!m.meshes.empty() && !write(m, s.file)) return rep;
    }
    for (const char* slot : kWeaponSlots) {
        const ManifestPiece* w = find_slot(manifest, slot);
        if (!w || w->file_ids.empty()) continue;
        Selection s;
        s.body_meshes = false;
        s.head = false;
        s.weapons = {slot};
        s.file = piece_file(slot, w->skin_name);
        ModelPreview m = build(*ctx, manifest, opt, s, rep);
        if (!m.meshes.empty() && !write(m, s.file)) return rep;
    }
    rep.ok = true;
    return rep;
}

} // namespace castlemist::ripper
