#include "internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
        // Cloth proxies often carry normals opposite to their winding. The winding
        // is right (it faces out, as in game); flip the normals to match it so
        // they light correctly, and engines that cull back faces (Unity) show them.
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
            for (GVertex& v : mesh.vertices) {
                v.nx = -v.nx; v.ny = -v.ny; v.nz = -v.nz;
                v.bx = -v.bx; v.by = -v.by; v.bz = -v.bz;  // keep the tangent frame right-handed
            }
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

// Marks (value 1) every atlas pixel the meshes' UV triangles cover, wrapped
// UVs; skin meshes (sampling the body's region) are skipped unless asked for.
std::vector<uint8_t> uv_coverage(const ModelPreview& m, int w, int h, bool skin_meshes) {
    std::vector<uint8_t> cov(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
    for (const ModelMeshCPU& mesh : m.meshes) {
        if (!skin_meshes && is_skin_mesh(m, mesh)) continue;
        for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
            float px[3], py[3];
            bool ok = true;
            for (int k = 0; k < 3; ++k) {
                const uint32_t vi = mesh.indices[t + k];
                if (vi >= mesh.vertices.size()) { ok = false; break; }
                px[k] = wrap_uv(mesh.vertices[vi].u) * static_cast<float>(w);
                py[k] = wrap_uv(mesh.vertices[vi].v) * static_cast<float>(h);
            }
            if (!ok) continue;
            const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
            if (std::fabs(area) < 1e-6f) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({px[0], px[1], px[2]}))));
            const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max({px[0], px[1], px[2]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({py[0], py[1], py[2]}))));
            const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max({py[0], py[1], py[2]}))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float cx = static_cast<float>(x) + 0.5f, cy = static_cast<float>(y) + 0.5f;
                    const float a = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                    const float b = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                    const float c = 1.0f - a - b;
                    if (a >= -0.01f && b >= -0.01f && c >= -0.01f) cov[static_cast<size_t>(y) * w + x] = 1;
                }
        }
    }
    return cov;
}

std::vector<character::ManifestDye> undergarment_dyes() {
    std::vector<character::ManifestDye> dyes;
    for (int slot = 0; slot < 4; ++slot) {
        character::ManifestDye d;
        d.slot = slot;
        d.color_id = 1;
        d.color_name = "Dye Remover";
        d.material = "cloth";
        d.shift = character::DyeShift{15.0f, 1.25f, 38.0f, 0.28125f, 1.4453125f};  // the API's cloth shift
        d.known = true;
        dyes.push_back(d);
    }
    return dyes;
}

void place_texture(AtlasRegion& region, const ImageRgba& tex, float scale, const std::vector<uint8_t>& coverage) {
    const float w = scale * static_cast<float>(tex.w), h = scale * static_cast<float>(tex.h);
    const uint32_t cx = region.ax, cy = region.ay;
    uint32_t fx = cx, fy = cy;
    fit_anchor(fx, fy, w, h);
    const std::pair<uint32_t, uint32_t> candidates[] = {{cx, cy}, {fx, fy}, {fx, cy}, {cx, fy}, {0, 0}};
    const int n = static_cast<int>(kAtlasSize);
    // Score: of the atlas pixels the piece samples, how many land on painted texels.
    auto score = [&](uint32_t ax, uint32_t ay) {
        size_t hit = 0;
        for (const composite::BlitRect& r : region.rects)
            for (uint32_t y = r.y0; y < r.y1 && y < static_cast<uint32_t>(n); y += 2)
                for (uint32_t x = r.x0; x < r.x1 && x < static_cast<uint32_t>(n); x += 2) {
                    if (!coverage[static_cast<size_t>(y) * n + x] || x < ax || y < ay) continue;
                    const int tx = static_cast<int>(static_cast<float>(x - ax) / scale);
                    const int ty = static_cast<int>(static_cast<float>(y - ay) / scale);
                    if (tx >= tex.w || ty >= tex.h) continue;
                    const uint8_t* p = tex.px.data() + (static_cast<size_t>(ty) * tex.w + tx) * 4;
                    if (p[3] > 8 && (p[0] > 4 || p[1] > 4 || p[2] > 4)) ++hit;
                }
        return hit;
    };
    size_t best = score(cx, cy);  // the rects' corner wins ties
    for (const auto& [ax, ay] : candidates) {
        const size_t s = score(ax, ay);
        if (s > best + best / 20) {  // clearly better
            best = s;
            region.ax = ax;
            region.ay = ay;
        }
    }
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
