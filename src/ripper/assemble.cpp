#include "castlemist/ripper/assemble.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "castlemist/exportgltf/gltf_export.h"
#include "castlemist/extract/entry_extractor.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/dye.h"
#include "castlemist/ripper/face_morphs.h"
#include "castlemist/ripper/skeleton_merge.h"
#include "castlemist/ripper/thumbnail.h"
#include "castlemist/ripper/vrchat.h"
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
    int pattern_slot = -1;                 // which mask of a skin pattern is this part's (chest 0, face 1, ...)
    bool undergarment = false;             // the race's underwear: own colours, no skin tint
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

    // Thumbnails (one look, many builds): bare parts' baked textures by token.
    std::map<uint64_t, std::optional<detail::BakedTextures>> preview_bakes;

    // Parts' models by fileId: thumbnails rebuild the same head many times.
    std::map<uint32_t, std::optional<ModelPreview>> models;
    std::optional<ModelPreview> model(uint32_t file_id) {
        auto it = models.find(file_id);
        if (it == models.end()) it = models.emplace(file_id, detail::load_model(dat, file_id)).first;
        return it->second;
    }

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
    bool body_parts = true;             // false: no bare-body parts at all (head thumbnails)
    bool preview = false;               // thumbnails: reduced textures, no normal maps
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

// VRChat: a bare-body part stays (nothing is dropped under armor) and gets a
// UV1 tile for Poiyomi's UV Tile Discard -- tiles count from the bottom-left,
// (0,0) (1,0) (2,0) (3,0), then the row above: head and everything else (0,0),
// chest (1,0), legs (2,0), hands (3,0), feet (0,1), undergarments (1,1). glTF's V runs down (Blender
// and Unity flip it on import), so a tile row up is a V step down here.
void vrchat_body_part(ModelPreview& out, size_t first, const std::string& part, const std::set<std::string>&) {
    struct Info { const char* part; float tu, tv; };
    static const Info kParts[] = {{"body chest", 1, 0},      {"body legs", 2, 0},           {"body hands", 3, 0},
                                  {"body feet", 0, 1},       {"undergarment top", 1, 1},    {"undergarment bottom", 1, 1}};
    float tu = 0, tv = 0;
    for (const Info& i : kParts)
        if (part == i.part) tu = i.tu, tv = i.tv;
    for (size_t mi = first; mi < out.meshes.size(); ++mi) {
        ModelMeshCPU& mesh = out.meshes[mi];
        mesh.exportUv1 = true;
        for (GVertex& v : mesh.vertices) {
            // Kept just inside the tile, so an edge texel can't land in the next one.
            v.uv1[0][0] = std::clamp(v.u - std::floor(v.u), 1e-4f, 1 - 1e-4f) + tu;
            v.uv1[0][1] = std::clamp(v.v - std::floor(v.v), 1e-4f, 1 - 1e-4f) - tv;
        }
    }
}

// The layers an armor piece's material adds over its base colour (its
// mask_tex / decal_tex, by sampler role) and whether it glows.
struct PieceLayers {
    uint32_t mask = 0;   // R = metal, G = gloss, B = silk / sheen, A = glow (atlas layout)
    uint32_t decal = 0;  // a detail layer: highlights / pattern over the base colour
    bool glow = false;   // the material has glow constants (glow, glowcm): mask A lights up
};
PieceLayers piece_layers(const ModelPreview& m) {
    PieceLayers l;
    for (const ModelMaterialCPU& mat : m.materials) {
        for (const auto& ex : mat.extraTextures) {
            if (ex.role == "mask" && !l.mask) l.mask = ex.fileId;
            if (ex.role == "decal" && !l.decal) l.decal = ex.fileId;
        }
        for (const auto& c : mat.namedConstants)
            if (c.first == "glow" || c.first == "glowcm") l.glow = true;
    }
    return l;
}

// Calls f(index of the RGBA pixel) for every atlas pixel inside the region's rects.
template <class F>
void for_region(const AtlasRegion& region, int w, int h, F f) {
    for (const composite::BlitRect& r : region.rects)
        for (uint32_t y = r.y0; y < r.y1 && y < static_cast<uint32_t>(h); ++y)
            for (uint32_t x = r.x0; x < r.x1 && x < static_cast<uint32_t>(w); ++x)
                f((static_cast<size_t>(y) * static_cast<size_t>(w) + x) * 4);
}

// Builds one output model on the race skeleton; appends to the report.
ModelPreview build(Context& ctx, const CharacterManifest& manifest, const AssemblyOptions& opt, const Selection& sel,
                   AssemblyReport& rep) {
    const composite::CompositeRace* race = ctx.race;
    auto report = [&](std::string name, std::string status, std::string reason, uint32_t mesh) {
        rep.parts.push_back({std::move(name), std::move(status), std::move(reason), mesh, sel.file});
    };

    // ---- the part list: body, head, armor ---------------------------------------
    std::vector<Part> parts;
    if (sel.body_parts && opt.skin_style >= 0 && static_cast<size_t>(opt.skin_style) < race->skin_styles.size()) {
        const auto& style = race->skin_styles[static_cast<size_t>(opt.skin_style)];
        const char* names[4] = {"body chest", "body feet", "body hands", "body legs"};
        const char* covered_by[4] = {"Coat", "Boots", "Gloves", "Leggings"};
        for (int k = 0; k < 4; ++k) {
            const composite::CompositeFileData* fd = ctx.entry(style[k]);
            if (!fd) {
                if (sel.body_meshes) report(names[k], "dropped", "no body part for " + ctx.race_key, 0);
                continue;
            }
            constexpr int kPatternSlot[4] = {0, 2, 3, 4};  // chest, feet, hands, legs
            Part body{names[k], fd, nullptr, sel.body_meshes, true, {}, false, kPatternSlot[k]};
            if (sel.body_meshes && sel.hide_body_under_armor && sel.worn.count(covered_by[k])) {
                body.keep_mesh = false;
                body.hidden_reason = std::string("under the ") + covered_by[k];
            }
            if (!sel.body_meshes) body.hidden_reason = "texture only";
            parts.push_back(body);
        }
    }
    if (sel.head) {
        auto head_part = [&](const char* name, const std::vector<uint64_t>& list, int index, int pattern_slot) {
            if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
            if (const composite::CompositeFileData* fd = ctx.entry(list[static_cast<size_t>(index)]))
                parts.push_back(Part{name, fd, nullptr, true, false, {}, false, pattern_slot});
        };
        head_part("face", race->faces, opt.face, 1);
        head_part("ears", race->ears, opt.ears, 5);
        head_part("hair", race->hair_styles, opt.hair, -1);
        if (sel.hair_under_helm_scalp_only && sel.worn.count("Helm") && !parts.empty() && parts.back().name == "hair")
            parts.back().scalp_only = true;
    }
    // The undergarments (a top for female races, a bottom for all): where no
    // coat / leggings covers them, as in game -- and always when nothing is
    // hidden under armor (VRChat, the separate body), so the avatar is dressed
    // with its armor toggled off. Painted before the armor, like the body.
    if (sel.body_parts && sel.body_meshes && sel.head) {
        const std::pair<const char*, std::pair<uint64_t, const char*>> under[] = {
            {"undergarment top", {composite::kUndergarmentTopToken, "Coat"}},
            {"undergarment bottom", {composite::kUndergarmentBottomToken, "Leggings"}}};
        for (const auto& [name, what] : under) {
            const composite::CompositeFileData* fd = ctx.entry(what.first);
            if (!fd || (sel.hide_body_under_armor && sel.worn.count(what.second))) continue;
            Part u{name, fd, nullptr, true, false, {}};
            u.undergarment = true;
            parts.push_back(u);
        }
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
    ImageRgba normal;
    if (!sel.preview) normal = ImageRgba{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    for (size_t i = 0; i < normal.px.size(); i += 4) {
        normal.px[i] = 128;
        normal.px[i + 1] = 128;
        normal.px[i + 2] = 255;
        normal.px[i + 3] = 255;
    }
    std::vector<size_t> atlas_meshes;  // indices into out.meshes that sample the atlas
    // Armor surface maps on the same layout, each piece only inside its own
    // rects (a set's mask covers the whole set; the other pieces' parts of it
    // are left out): glTF metallic-roughness (G = roughness, B = metal) and a
    // detail layer. Bare skin: not metal, fairly rough.
    ImageRgba metal_rough{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 255)};
    for (size_t i = 0; i < metal_rough.px.size(); i += 4) {
        metal_rough.px[i + 1] = 180;
        metal_rough.px[i + 2] = 0;
    }
    std::optional<ImageRgba> detail_map;
    bool any_armor = false;
    // Glow, in the glow colour on the same atlas layout: the skin pattern's
    // masks, and a sylvari's own glow on the face and hair (their model
    // materials' mask_tex -- greyscale spots and veins, sampled by UV0).
    const bool sylvari = ctx.race_key.rfind("Sylvari", 0) == 0;
    const bool glowing = opt.glow_rgb && opt.glow_intensity > 0;
    const bool pattern_glowing = glowing && opt.pattern >= 0 &&
                                 static_cast<size_t>(opt.pattern) < race->skin_patterns.size();
    ImageRgba emissive{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
    bool any_glow = false;
    // A greyscale mask lit in the glow colour.
    auto glow_image = [&](const ModelTextureCPU& mask) {
        ImageRgba glow{mask.width, mask.height, std::vector<uint8_t>(mask.rgba.size())};
        const float k = std::clamp(opt.glow_intensity, 0.0f, 1.0f) / 255.0f;
        for (size_t t = 0; t + 3 < mask.rgba.size(); t += 4) {
            const float w = mask.rgba[t] * k;  // greyscale: R = G = B
            for (int c = 0; c < 3; ++c) glow.px[t + c] = static_cast<uint8_t>((*opt.glow_rgb)[c] * w + 0.5f);
            glow.px[t + 3] = 255;
        }
        return glow;
    };
    // The glow mask of a sylvari face / hair model (0 = none): the mask_tex of
    // the material drawing most of it (hair: the strands, not the scalp under
    // them, whose mask is laid out for the scalp's texture).
    auto own_glow_mask = [&](const ModelPreview& m) -> uint32_t {
        std::map<uint32_t, size_t> tris;
        for (const ModelMeshCPU& mesh : m.meshes) tris[mesh.materialIndex] += mesh.indices.size() / 3;
        uint32_t best = 0;
        size_t most = 0;
        for (const ModelMaterialCPU& mat : m.materials)
            for (const auto& ex : mat.extraTextures)
                if (ex.role == "mask" && ex.fileId && tris[mat.index] > most) {
                    most = tris[mat.index];
                    best = ex.fileId;
                }
        return best;
    };

    // An armor piece just painted into the atlas: what its base colour's alpha
    // is (cut-out holes vs a specular / gloss map -- character armor is alpha
    // tested, and the alpha of an opaque piece carries its shine), its metal /
    // gloss / glow mask and detail layer, into the surface atlases. Returns
    // the workflow, for the report.
    auto armor_surface = [&](const ModelPreview& model, const detail::BakedTextures& baked,
                             const AtlasRegion& region, bool skin_meshes) -> std::string {
        (void)baked;
        any_armor = true;
        const PieceLayers layers = piece_layers(model);
        // Holes are counted only where the piece's own triangles sample.
        const std::vector<uint8_t> used = detail::uv_coverage(model, kAtlas, kAtlas, skin_meshes);
        // Masks and decals are sampled by the atlas UVs: they span the whole
        // atlas (a set's mask holds every piece of the set), clipped to the rects.
        AtlasRegion whole = region;
        whole.ax = whole.ay = 0;
        // Alpha: below kHole is a hole; the rest is shine, the texel opaque.
        constexpr uint8_t kHole = 16;
        size_t total = 0, holes = 0, lo = 255, hi = 0;
        for_region(region, kAtlas, kAtlas, [&](size_t i) {
            uint8_t* p = diffuse.px.data() + i;
            const bool sampled = used[i / 4] != 0;
            total += sampled;
            if (p[3] < kHole) {
                holes += sampled;
                p[3] = 0;
                return;
            }
            lo = std::min<size_t>(lo, p[3]);
            hi = std::max<size_t>(hi, p[3]);
            // Shine -> roughness (full shine 0.4, none 1.0); not metal.
            metal_rough.px[i + 1] = static_cast<uint8_t>(255 - p[3] * 6 / 10);
            metal_rough.px[i + 2] = 0;
            p[3] = 255;
        });
        std::string wf;
        ModelTextureCPU mask;
        if (layers.mask && decode_texture_full(ctx.dat, layers.mask, mask) && mask.width > 0) {
            // Masked PBR: metal and gloss from the mask; glow from its alpha.
            ImageRgba m{mask.width, mask.height, std::move(mask.rgba)};
            ImageRgba tmp{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
            blit(tmp, m, whole, static_cast<float>(kAtlas) / static_cast<float>(m.w));
            size_t metal = 0, glow = 0;
            for_region(region, kAtlas, kAtlas, [&](size_t i) {
                const uint8_t* t = tmp.px.data() + i;
                metal_rough.px[i + 1] = static_cast<uint8_t>(255 - t[1]);
                metal_rough.px[i + 2] = t[0];
                if (t[0] > 127) ++metal;
                if (layers.glow && t[3] > 8 && diffuse.px[i + 3]) {
                    ++glow;
                    for (int c = 0; c < 3; ++c)
                        emissive.px[i + c] = static_cast<uint8_t>(diffuse.px[i + c] * t[3] / 255);
                    emissive.px[i + 3] = 255;
                }
            });
            if (glow) any_glow = true;
            wf = metal ? "metallic (mask: metal R, gloss G)" : "masked gloss (mask: gloss G, no metal)";
            if (glow) wf += ", glow (mask A)";
        } else {
            wf = "legacy (base colour + normal only)";
        }
        if (layers.decal) {
            ModelTextureCPU dec;
            if (decode_texture_full(ctx.dat, layers.decal, dec) && dec.width > 0) {
                if (!detail_map)
                    detail_map = ImageRgba{kAtlas, kAtlas, std::vector<uint8_t>(static_cast<size_t>(kAtlas) * kAtlas * 4, 0)};
                blit(*detail_map, ImageRgba{dec.width, dec.height, std::move(dec.rgba)}, whole,
                     static_cast<float>(kAtlas) / static_cast<float>(dec.width));
                wf += ", detail layer (decal)";
            }
        }
        if (total) {
            if (holes * 200 > total) wf += "; alpha = cut-out holes + shine";
            else if (hi > lo + 16) wf += "; alpha = shine (specular), opaque";
            else wf += "; alpha unused, opaque";
        }
        return wf;
    };

    // VRChat: the body and hair get the atlas as it is before any armor paints
    // over it (a helm takes the hair's rect, gloves the hands'...), so with the
    // armor toggled off they still show their own textures.
    std::optional<ImageRgba> body_diffuse, body_normal, body_emissive;
    bool body_glow = false;
    for (const Part& part : parts) {
        if (opt.vrchat && part.piece && !body_diffuse) {
            body_diffuse = diffuse;
            body_normal = normal;
            body_emissive = emissive;
            body_glow = any_glow;
        }
        std::optional<ModelPreview> model = ctx.model(part.fd->mesh_base);
        if (!model) {
            report(part.name, "dropped", "model failed to load", part.fd->mesh_base);
            continue;
        }
        // A piece's overlap mesh: the part reaching onto a neighbouring slot
        // (gauntlets up the forearm, greaves up the shin). Same texture.
        if (part.piece && part.fd->mesh_overlap)
            if (std::optional<ModelPreview> ov = ctx.model(part.fd->mesh_overlap)) merge_into(*model, *ov);
        // Armor keeps its own dyes; bare-body parts, face, ears and the scalp
        // take the skin colour, and hair its colours on its dye channels.
        std::vector<character::ManifestDye> look_dyes;
        std::optional<ColorMatrix> skin;
        if (!part.piece && !part.undergarment) {
            // Hair texels outside its dye masks are authored accents, not skin.
            if (opt.skin_tint && part.name != "hair") skin = dye_matrix(*opt.skin_tint);
            if (part.name == "hair") {
                // Sylvari hair: channel 1 (a mask over the whole style) takes the
                // skin colour and channel 2 the hair colour -- leaves it leaves
                // out (striped fronds, accents) stay skin-toned, as in game.
                // Other races: hair colour, then the second hair colour.
                const bool sylvari_hair = ctx.race_key.rfind("Sylvari", 0) == 0;
                const std::optional<character::DyeShift> tints[2] = {
                    sylvari_hair ? opt.skin_tint : opt.hair_tint, sylvari_hair ? opt.hair_tint : opt.hair_tint2};
                for (int ch = 0; ch < 2; ++ch)
                    if (tints[ch]) {
                        character::ManifestDye d;
                        d.slot = ch;
                        d.shift = tints[ch];
                        look_dyes.push_back(d);
                    }
            }
        }
        // A skin pattern rides on dye channel 1 (bare parts have no dye masks of
        // their own), the iris on channel 2 (sylvari faces: the cut mask is it).
        composite::CompositeFileData fd = *part.fd;
        uint32_t pattern_mask = 0;
        if (!part.piece && part.pattern_slot >= 0 && opt.pattern >= 0 &&
            static_cast<size_t>(opt.pattern) < race->skin_patterns.size())
            pattern_mask = race->skin_patterns[static_cast<size_t>(opt.pattern)][static_cast<size_t>(part.pattern_slot)];
        if (pattern_mask && opt.pattern_tint) {
            fd.mask_dye = {pattern_mask, 0, 0, 0};
            character::ManifestDye d;
            d.slot = 0;
            d.shift = opt.pattern_tint;
            look_dyes.push_back(d);
        }
        if (!part.piece && part.name == "face" && sylvari && opt.eye_tint && part.fd->mask_cut) {
            fd.mask_dye[1] = part.fd->mask_cut;
            character::ManifestDye d;
            d.slot = 1;
            d.shift = opt.eye_tint;
            look_dyes.push_back(d);
        }
        std::string workflow;  // armor: how its surface is made (for the report)
        std::optional<detail::BakedTextures> baked;
        if (sel.preview) {
            auto it = ctx.preview_bakes.find(part.fd->token);
            if (it == ctx.preview_bakes.end())
                it = ctx.preview_bakes.emplace(part.fd->token, detail::bake_part(ctx.dat, fd, look_dyes, skin, true)).first;
            baked = it->second;
        } else {
            baked = detail::bake_part(ctx.dat, fd,
                                      part.piece          ? part.piece->dyes
                                      : part.undergarment ? detail::undergarment_dyes()
                                                          : look_dyes,
                                      skin);
        }
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
        if (!region.rects.empty())
            detail::place_texture(region, detail::to_image(baked->base), baked->scale,
                                  detail::uv_coverage(*model, kAtlas, kAtlas, part.skin_meshes_are_part));
        const bool in_atlas = !region.rects.empty();
        if (in_atlas) {
            // Hair and bald-scalp layers paint over the face's region the way the
            // game composites them (by their alpha), and so do the undergarments
            // over the bare body where their rects share it; everything else replaces.
            const BlitMode mode = part.name == "hair" || part.undergarment ? BlitMode::Over : BlitMode::Replace;
            blit(diffuse, detail::to_image(baked->base), region, baked->scale, mode);
            if (baked->normal && mode == BlitMode::Replace)
                blit(normal, detail::to_image(*baked->normal), region, baked->normal_scale);
            // Glow masks, from the region's top-left. A skin pattern's mask
            // covers the part's texture footprint; a sylvari's own glow mask
            // (face, hair, ears) covers the width of the part's atlas rects,
            // uniformly scaled -- not the texture, which can reach past them
            // (the face's is 512 wide in a 384-wide rect, the hair's the whole
            // atlas). Armor painted over a region puts out the glow beneath it.
            auto blit_glow = [&](uint32_t mask_file, BlitMode m, bool own) {
                ModelTextureCPU mask;
                if (!mask_file || !decode_texture_full(ctx.dat, mask_file, mask) || mask.width <= 0) return;
                int fw = static_cast<int>(std::lround(baked->scale * static_cast<float>(baked->base.width)));
                int fh = static_cast<int>(std::lround(baked->scale * static_cast<float>(baked->base.height)));
                if (own) {
                    uint32_t x1 = region.ax;
                    for (const composite::BlitRect& r : region.rects) x1 = std::max(x1, r.x1);
                    fw = static_cast<int>(x1 - region.ax);
                    fh = static_cast<int>(std::lround(static_cast<float>(fw) * static_cast<float>(mask.height) /
                                                      static_cast<float>(mask.width)));
                }
                if (fw <= 0 || fh <= 0) return;
                blit(emissive, resize_bilinear(glow_image(mask), fw, fh), region, 1.0f, m);
                any_glow = true;
            };
            if (part.piece) {
                ImageRgba dark{baked->base.width, baked->base.height,
                               std::vector<uint8_t>(static_cast<size_t>(baked->base.width) * baked->base.height * 4, 0)};
                for (size_t t = 3; t < dark.px.size(); t += 4) dark.px[t] = 255;
                blit(emissive, dark, region, baked->scale);
                workflow = armor_surface(*model, *baked, region, part.skin_meshes_are_part);
            }
            if (pattern_glowing && pattern_mask) blit_glow(pattern_mask, BlitMode::Replace, false);
            if (glowing && sylvari && !part.piece && !part.undergarment)
                blit_glow(own_glow_mask(*model), BlitMode::Add, true);
        } else if (glowing && sylvari && !part.piece && !part.undergarment && part.keep_mesh) {
            // Self-textured hair: its glow on its own UVs.
            ModelTextureCPU mask;
            if (uint32_t f = own_glow_mask(*model); f && decode_texture_full(ctx.dat, f, mask) && mask.width > 0) {
                int e = detail::add_texture(*model, glow_image(mask), 0, false);
                for (ModelMaterialCPU& mat : model->materials) mat.emissiveTex = e;
            }
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
        // Which avatar piece these meshes belong to: the bare body and head,
        // the hair, or the armor slot (vrchat merges per piece).
        const std::string group = part.piece          ? part.piece->slot
                                  : part.undergarment ? "Undergarments"
                                  : part.name == "hair" ? "Hair"
                                                        : "Body";
        for (size_t mi = first; mi < out.meshes.size(); ++mi) out.meshes[mi].meshName = group;
        if (opt.vrchat) vrchat_body_part(out, first, part.name, sel.worn);
        report(part.name, "used", part.scalp_only ? "scalp only, strands under the Helm" : workflow, part.fd->mesh_base);
    }

    // One material for everything that samples the atlas.
    if (!atlas_meshes.empty()) {
        int d = detail::add_texture(out, diffuse, 0, false);
        int n = sel.preview ? -1 : detail::add_texture(out, normal, 0, true);
        ModelMaterialCPU mat;
        mat.index = static_cast<uint32_t>(out.materials.size());
        mat.materialName = "CharacterAtlas";
        mat.diffuseTex = d;
        mat.normalTex = n;
        if (any_glow) mat.emissiveTex = detail::add_texture(out, emissive, 0, false);
        if (any_armor && !sel.preview) mat.metalRoughTex = detail::add_texture(out, metal_rough, 0, false);
        if (detail_map && !sel.preview)
            mat.extraTextures.push_back({detail::add_texture(out, *detail_map, 0, false), 0, 0, "decal"});
        out.materials.push_back(mat);
        for (size_t mi : atlas_meshes) out.meshes[mi].materialIndex = mat.index;
        if (opt.vrchat) {  // Body and Hair on the pre-armor atlas
            if (!body_diffuse) {
                body_diffuse = diffuse;
                body_normal = normal;
            }
            ModelMaterialCPU body = mat;
            body.metalRoughTex = -1;  // bare skin and hair
            body.extraTextures.clear();
            body.index = static_cast<uint32_t>(out.materials.size());
            body.materialName = "BodyAtlas";
            body.diffuseTex = detail::add_texture(out, *body_diffuse, 0, false);
            body.normalTex = sel.preview ? -1 : detail::add_texture(out, *body_normal, 0, true);
            if (body_emissive && body_glow) body.emissiveTex = detail::add_texture(out, *body_emissive, 0, false);
            else if (!body_glow && body_emissive) body.emissiveTex = -1;
            out.materials.push_back(body);
            for (size_t mi : atlas_meshes)
                if (out.meshes[mi].meshName == "Body" || out.meshes[mi].meshName == "Hair" ||
                    out.meshes[mi].meshName == "Undergarments")
                    out.meshes[mi].materialIndex = body.index;
        }
    }

    // ---- back item: merges onto the skeleton by joint name ------------------------
    if (sel.back) {
        if (const ManifestPiece* back = find_slot(manifest, "Backpack"); back && !back->file_ids.empty()) {
            std::optional<ModelPreview> m = detail::load_model(ctx.dat, back->file_ids[0]);
            if (m && !m->joints.empty()) {
                // Its own rig hung from the shared root (Mawdrey, wings, ...) goes
                // onto the back holster; one skinned to body bones (capes) merges
                // by name.
                const size_t back_first = out.meshes.size();
                if (back_is_self_rigged(*m, out) && attach_skinned(out, *m, kBackStow.weapon, kBackStow.body)) {
                    report("Backpack", "used", std::string("on ") + kBackStow.body, back->file_ids[0]);
                } else {
                    merge_into(out, *m);
                    report("Backpack", "used", "", back->file_ids[0]);
                }
                for (size_t mi = back_first; mi < out.meshes.size(); ++mi) out.meshes[mi].meshName = "Backpack";
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
        } else if (const size_t wfirst = out.meshes.size(); attach_rigid(out, *w.model, w.at->weapon, w.at->body)) {
            for (size_t mi = wfirst; mi < out.meshes.size(); ++mi) out.meshes[mi].meshName = slot;
            report(slot, "used", std::string("on ") + w.at->body + (w.shares.empty() ? "" : " (shared with " + w.shares + ")"),
                   file);
        } else {
            report(slot, "dropped", "no matching attach point", file);
        }
    }
    // Face-detail blend shapes on whatever the face rig moves (face, ears,
    // hair, a helm); thumbnails skip them.
    if (opt.face_morphs && !sel.preview) add_face_morphs(out, opt.face_sliders);
    return out;
}

bool write_glb(ModelPreview model, const std::string& path, const AssemblyOptions& opt, AssemblyReport& rep) {
    detail::mirror_x(model);  // GW2 is left-handed: un-mirror for glTF
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

std::vector<ImageRgba> look_thumbnails(const CharacterManifest& manifest, const std::string& dat_path, LookPart part,
                                       const AssemblyOptions& options, int size, std::string* error) {
    AssemblyReport rep;
    AssemblyOptions opt = options;
    opt.weapons = WeaponPlacement::None;
    std::unique_ptr<Context> ctx = open_context(manifest, dat_path, opt, rep);
    if (!ctx) {
        if (error) *error = rep.error;
        return {};
    }
    const std::vector<uint64_t>& list =
        part == LookPart::Face ? ctx->race->faces : part == LookPart::Hair ? ctx->race->hair_styles : ctx->race->ears;
    if (part == LookPart::Ears) opt.hair = -1;  // the ears unhidden by hair
    Selection sel;
    sel.body_parts = false;
    sel.preview = true;
    sel.hair_under_helm_scalp_only = false;
    std::vector<ImageRgba> out;
    for (size_t i = 0; i < list.size(); ++i) {
        (part == LookPart::Face ? opt.face : part == LookPart::Hair ? opt.hair : opt.ears) = static_cast<int>(i);
        AssemblyReport r;
        ModelPreview head = build(*ctx, manifest, opt, sel, r);
        // Faces head-on, ears from the side, hair three-quarter from above.
        out.push_back(render_thumbnail(head, size, part == LookPart::Face   ? front_view()
                                                   : part == LookPart::Ears ? side_view()
                                                                            : three_quarter_top_view()));
    }
    return out;
}

std::vector<ImageRgba> pattern_thumbnails(const CharacterManifest& manifest, const std::string& dat_path,
                                          std::array<uint8_t, 3> skin_rgb, std::array<uint8_t, 3> pattern_rgb, int size,
                                          std::string* error) {
    AssemblyReport rep;
    AssemblyOptions opt;
    opt.weapons = WeaponPlacement::None;
    std::unique_ptr<Context> ctx = open_context(manifest, dat_path, opt, rep);
    if (!ctx) {
        if (error) *error = rep.error;
        return {};
    }
    std::vector<ImageRgba> out;
    for (const auto& files : ctx->race->skin_patterns) {
        ModelTextureCPU mask;
        ImageRgba img{size, size / 2, std::vector<uint8_t>(static_cast<size_t>(size) * (size / 2) * 4, 255)};
        if (files[0] && decode_texture_full(ctx->dat, files[0], mask) && mask.width > 0) {
            const ImageRgba m = resize_bilinear(ImageRgba{mask.width, mask.height, mask.rgba}, img.w, img.h);
            for (size_t t = 0; t + 3 < img.px.size(); t += 4) {
                const float w = m.px[t] / 255.0f;
                for (int c = 0; c < 3; ++c)
                    img.px[t + c] = static_cast<uint8_t>(skin_rgb[c] + (pattern_rgb[c] - skin_rgb[c]) * w + 0.5f);
            }
        }
        out.push_back(std::move(img));
    }
    return out;
}

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
    if (opt.vrchat) {  // the whole body and hair: armor toggles in-game, so nothing is cut
        sel.hide_body_under_armor = false;
        sel.hair_under_helm_scalp_only = false;
    }
    ModelPreview model = build(*ctx, manifest, opt, sel, rep);
    if (opt.vrchat) {
        rep.physbone_chains = make_vrchat_ready(model);
        write_vrchat_maps(model, glb_path);
    }
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
