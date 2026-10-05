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

void add_cloth_meshes(ModelPreview& m) {
    // In-game the cloth pieces ARE drawn: hair fronds, cape tails, skirt flaps
    // hang from the static mesh as simulated cloth (the frond style: a 402-vert
    // static mesh plus a 357-vert cloth piece reaching further down). Export
    // them as ordinary meshes in their authored rest shape. They carry no skin
    // weights (the simulation drives them), so each vertex follows the nearest
    // vertex of the model's skinned meshes.
    if (m.clothPieces.empty()) return;
    std::vector<const GVertex*> skinned;
    for (const ModelMeshCPU& mesh : m.meshes)
        if (mesh.hasSkin)
            for (const GVertex& v : mesh.vertices) skinned.push_back(&v);
    for (const ClothPieceCPU& cp : m.clothPieces) {
        if (cp.vertices.empty() || cp.indices.size() < 3) continue;
        ModelMeshCPU mesh;
        mesh.vertices = cp.vertices;
        mesh.indices = cp.indices;
        mesh.materialIndex = cp.materialIndex;
        mesh.vertexCount = static_cast<uint32_t>(cp.vertices.size());
        mesh.meshName = "cloth";
        mesh.hasSkin = !skinned.empty();
        for (GVertex& v : mesh.vertices) {
            const GVertex* best = nullptr;
            float best_d = 0;
            for (const GVertex* s : skinned) {
                const float dx = s->px - v.px, dy = s->py - v.py, dz = s->pz - v.pz;
                const float d = dx * dx + dy * dy + dz * dz;
                if (!best || d < best_d) best = s, best_d = d;
            }
            if (!best) continue;
            for (int k = 0; k < 4; ++k) {
                v.bidx[k] = best->bidx[k];
                v.bwt[k] = best->bwt[k];
            }
        }
        // Cloth proxies are often wound opposite to their normals; engines that
        // cull back faces (Unity) would hide them. Match the winding to the normals.
        long agree = 0;
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const GVertex* t[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                if (mesh.indices[i + k] >= mesh.vertices.size()) { ok = false; break; }
                t[k] = &mesh.vertices[mesh.indices[i + k]];
            }
            if (!ok) continue;
            const float ax = t[1]->px - t[0]->px, ay = t[1]->py - t[0]->py, az = t[1]->pz - t[0]->pz;
            const float bx = t[2]->px - t[0]->px, by = t[2]->py - t[0]->py, bz = t[2]->pz - t[0]->pz;
            const float gx = ay * bz - az * by, gy = az * bx - ax * bz, gz = ax * by - ay * bx;
            const float nx = t[0]->nx + t[1]->nx + t[2]->nx, ny = t[0]->ny + t[1]->ny + t[2]->ny,
                        nz = t[0]->nz + t[1]->nz + t[2]->nz;
            agree += (gx * nx + gy * ny + gz * nz) >= 0 ? 1 : -1;
        }
        if (agree < 0)
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) std::swap(mesh.indices[i + 1], mesh.indices[i + 2]);
        m.totalVerts += static_cast<uint32_t>(mesh.vertices.size());
        m.totalTris += static_cast<uint32_t>(mesh.indices.size() / 3);
        m.meshes.push_back(std::move(mesh));
    }
}

std::optional<ModelPreview> load_model(Gw2Dat& dat, uint32_t file_id) {
    try {
        // Against the open dat: extract_entry() would reopen it for every model.
        if (auto m = load_model_by_fileid(dat, file_id)) {
            ModelPreview out = *m;
            add_cloth_meshes(out);
            return out;
        }
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

void mirror_x(ModelPreview& model) {
    for (ModelMeshCPU& mesh : model.meshes) {
        for (GVertex& v : mesh.vertices) {
            v.px = -v.px;
            v.nx = -v.nx;
            v.tx = -v.tx;
            v.bx = -v.bx;
        }
        auto flip = [](std::vector<uint32_t>& idx) {
            for (size_t i = 0; i + 2 < idx.size(); i += 3) std::swap(idx[i + 1], idx[i + 2]);
        };
        flip(mesh.indices);
        for (auto& lod : mesh.lodIndices) flip(lod);
        for (MorphTargetCPU& t : mesh.morphs)
            for (size_t i = 0; i < t.delta.size(); i += 3) t.delta[i] = -t.delta[i];
    }
    for (ModelJoint& j : model.joints) {
        j.pos[0] = -j.pos[0];
        j.localPos[0] = -j.localPos[0];
        j.localQuat[1] = -j.localQuat[1];
        j.localQuat[2] = -j.localQuat[2];
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if ((r == 0) != (c == 0)) j.localScale[r * 3 + c] = -j.localScale[r * 3 + c];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if ((r == 0) != (c == 0)) j.invWorld[r * 4 + c] = -j.invWorld[r * 4 + c];
    }
    model.animClips.clear();
    model.animClipBank.clear();
    model.hasAnimation = false;
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
