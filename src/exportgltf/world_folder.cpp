/// @file
/// @brief export_world_folder: the world export's orchestrator
///        (docs/superpowers/specs/2026-10-15-world-export-design.md,
///        "Order of work", "Output folder", "Error handling").
#include "castlemist/exportgltf/world_folder.h"

#include "castlemist/exportgltf/vrchat_export.h"
#include "castlemist/exportgltf/world_cell.h"
#include "castlemist/exportgltf/world_export.h"
#include "castlemist/exportgltf/world_foliage.h"
#include "castlemist/exportgltf/world_lods.h"
#include "castlemist/exportgltf/world_probe.h"
#include "castlemist/exportgltf/world_unity_terrain.h"
#include "castlemist/world/anim.h"
#include "castlemist/world/cells.h"
#include "castlemist/world/dat_read.h"
#include "castlemist/world/surface_rules.h"
#include "castlemist/world/terrain.h"
#include "castlemist/world/terrain_bake.h"
#include "castlemist/world/walk.h"

#include "internal.h"
#include "long_path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace castlemist::exportgltf {

namespace {

namespace fs = std::filesystem;
namespace cw = castlemist::world;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

fs::path u8path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string utf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

/// The first fatal error of a pool (dat I/O, a failed write); later ones are dropped.
struct Fatal {
    std::mutex mu;
    std::string error;
    std::atomic<bool> stop{false};
    const std::atomic<bool>* cancel = nullptr;   ///< WorldFolderOptions::cancel
    void set(const std::string& e) {
        std::lock_guard<std::mutex> lk(mu);
        if (error.empty()) error = e;
        stop = true;
    }
    /// True once a fatal error or the cancel flag stops the export.
    bool stopped() {
        if (!stop && cancel && cancel->load()) set("export cancelled");
        return stop;
    }
};

/// Runs body(i) for i in [0, n) on min(threads, n) workers popping an atomic
/// counter. Each thread body is under a catch-all: a world::DatIoError, or any
/// other exception the body does not handle itself, is fatal and stops the pool.
template <class F>
void run_pool(size_t n, unsigned threads, Fatal& fatal, F&& body) {
    if (n == 0) return;
    const unsigned workers = static_cast<unsigned>(std::max<size_t>(1, std::min<size_t>(threads, n)));
    std::atomic<size_t> next{0};
    auto work = [&] {
        try {
            for (size_t i; !fatal.stopped() && (i = next.fetch_add(1)) < n;) body(i);
        } catch (const cw::DatIoError& e) {
            fatal.set(e.what());
        } catch (const std::exception& e) {
            fatal.set(std::string("export: ") + e.what());
        } catch (...) {
            fatal.set("export: unknown exception");
        }
    };
    if (workers == 1) { work(); return; }
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned t = 0; t < workers; ++t) pool.emplace_back(work);
    for (auto& th : pool) th.join();
}

bool write_bytes(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(long_path(p), std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    f.close();   // the final buffered flush can fail (disk full): check after it
    return !f.fail();
}

uint64_t capped_bytes(int w, int h) { return uint64_t(std::max(w, 0)) * uint64_t(std::max(h, 0)) * 4 * 4 / 3; }

/// The model stage's result for one needed model (kept by model index).
struct ModelOut {
    ManifestModel mm;
    std::vector<std::string> warnings;
    std::vector<uint32_t> textures;                    ///< distinct texture fileIds
    std::unordered_map<uint32_t, uint64_t> texBytes;   ///< RGBA8 + mips at the cap
    std::vector<uint64_t> clipTokens, allTokens;       ///< animClipTokens, animationTokens
    bool empty = false;                                ///< loaded, but no drawable meshes
};

/// Fills the preview's textures from the capped decode, measures it, writes
/// models/<fileId>/ and releases it. A null load or a failed write fails the
/// model only; world::DatIoError reaches the pool (fatal).
void export_one_model(const cw::WorldScene& scene, uint32_t mi, ModelSource& src, const fs::path& dir,
                      const WorldFolderOptions& opts, const std::shared_ptr<SharedImagePool>& pool, ModelOut& out) {
    const uint32_t fileId = scene.models[mi].fileId;
    ManifestModel& mm = out.mm;
    mm.index = mi;
    mm.fileId = fileId;
    std::shared_ptr<ModelPreview> m = src.load(mi, fileId);
    if (!m) { mm.error = "model did not load"; return; }
    mm.meshes = static_cast<int>(m->meshes.size());
    mm.materialCount = static_cast<int>(m->materials.size());
    mm.wind = model_wind(*m);   // world-foliage spec D7: recorded, not acted on (D10)
    if (m->meshes.empty()) {   // empty, not broken: counted apart (review M2)
        mm.error = "This model has no drawable meshes to export.";
        out.empty = true;
        return;
    }
    try {
        // Textures: mip 0 of the capped decode, as the map window draws them.
        std::vector<std::shared_ptr<const CappedTexture>> capped(m->textures.size());
        std::set<uint32_t> ids;
        for (size_t i = 0; i < m->textures.size(); ++i) {
            ModelTextureCPU& t = m->textures[i];
            std::shared_ptr<const CappedTexture> c = src.texture(t.fileId, opts.maxTextureSize);
            const bool ok = c && !c->mips.empty() && c->width > 0 && c->height > 0 &&
                            c->mips[0].size() >= size_t(c->width) * size_t(c->height) * 4;
            t.rgba.clear();
            if (ok) {
                t.rgba = c->mips[0];
                t.width = c->width;
                t.height = c->height;
                t.hasCutout = c->hasCutout;   // measured on the pixels, which a header-only load cannot
                capped[i] = c;
            }
            if (t.fileId != 0 && ids.insert(t.fileId).second) {
                out.textures.push_back(t.fileId);
                if (ok) out.texBytes[t.fileId] = capped_bytes(c->width, c->height);
            }
        }
        for (const auto& [id, b] : out.texBytes) mm.textureBytes += b;

        for (const ModelMeshCPU& mesh : m->meshes) mm.triangles += mesh.indices.size() / 3;
        mm.lods = model_lods_json(*m);   // Task 15b: levels, switch distances, triangles per level
        for (const auto& clip : m->animClips) mm.clips.push_back(clip.name);
        out.clipTokens = m->animClipTokens;
        out.allTokens = m->animationTokens;

        for (ModelMaterialCPU& mat : m->materials) {
            const bool hasDiffuse = mat.diffuseTex >= 0 && static_cast<size_t>(mat.diffuseTex) < m->textures.size();
            const CappedTexture* d = hasDiffuse ? capped[static_cast<size_t>(mat.diffuseTex)].get() : nullptr;
            // No render state (a wantGame = false load): the pixel guesses the
            // map window makes (world_alpha_mode, src/render/world_materials.h).
            if (!mat.hasRenderState && d) {
                if (!mat.isEffect && d->looksLikeEffect) mat.isEffect = true;   // model_preview.cpp's map-path rule
                if (d->hasCutout && cw::dark_coverage(d->mips[0])) mm.darkDecalMaterials.push_back(material_name(mat));
            }
            // uvScroll: prop_materials' rule (world_materials.h): a diffuse on UV0/UV1, and
            // the first texTransforms entry on its own UV set that animates.
            if (!hasDiffuse || mat.diffuseUv > 1) continue;
            for (const ModelTexTransformCPU& tt : mat.texTransforms) {
                if (tt.uvIndex != mat.diffuseUv || !cw::tex_transform_animates(tt)) continue;
                mm.uvScroll.push_back({{"material", material_name(mat)},
                                       {"uvIndex", tt.uvIndex},
                                       {"scroll", {tt.scroll[0], tt.scroll[1]}}});
                break;
            }
        }

        const std::string id = std::to_string(fileId);
        const fs::path folder = dir / "models" / id;
        VrchatFolderOptions vo;
        vo.gltf.rootMatrix = map_to_gltf_root(1.0);
        vo.gltf.imageDirUtf8 = utf8(folder / "Images");   // generated images (bakes) only
        vo.gltf.imageUriPrefix = "Images/";
        vo.gltf.sharedImages = pool;                      // dat textures: <out>/textures/<fileId>.png (W8)
        vo.gltf.sharedImageUriPrefix = "../../textures/"; // from models/<fileId>/<fileId>.glb
        vo.gltf.exportLods = true;                        // Task 15b: <node>_LOD1 / _LOD2 (world_lods.h)
        const VrchatFolderResult r = write_vrchat_folder(*m, utf8(folder), id, fileId, vo);
        m.reset();   // the preview is released once written
        for (const std::string& w : r.warnings) out.warnings.push_back("models: " + id + ": " + w);
        if (!r.ok) {
            mm.error = r.error.empty() ? "VRChat folder not written" : r.error;
            return;
        }
        mm.ok = true;
        mm.folder = "models/" + id;
        mm.glb = mm.folder + "/" + id + ".glb";
        mm.materials = mm.folder + "/materials.json";
    } catch (const cw::DatIoError&) {
        throw;
    } catch (const std::exception& e) {
        mm.ok = false;
        mm.error = std::string("model export failed: ") + e.what();
    }
}

/// One written cell (kept by cell index); `written` false = omitted.
struct CellOut {
    bool written = false;
    ManifestCell mc;
    size_t unbaked = 0;
    std::vector<std::string> warnings;
    size_t foliageSkipped = 0, foliageFailedModel = 0, foliageScatter = 0, foliageDensity = 0;
    std::vector<uint32_t> foliageModels;   ///< scene.models indices with a written record
    /// The cell's records (models as manifest positions), written to foliage/<id>.bin after
    /// the unityTerrain stage, which sets their bit 3 (world-foliage spec D7).
    std::vector<FoliageRecord> records;
    std::vector<uint8_t> albedoRgb;        ///< the baked albedo, kept for the unityTerrain tiles
};

/// What the cell stage needs to write the foliage records (world-foliage spec D7).
struct FoliageCtx {
    FoliageExport mode = FoliageExport::Off;
    std::vector<uint32_t> manifestPos;   ///< scene.models index -> models[] position, UINT32_MAX = not exported
    std::vector<char> modelOk;           ///< by scene.models index
    std::vector<uint64_t> triangles;     ///< by scene.models index (LOD0, as measured)
};

/// The cell's records (sorted, models as manifest positions), with their counts.
std::vector<FoliageRecord> cell_foliage_kept(const cw::WorldScene& s, const cw::Cell& c, const FoliageCtx& f,
                                             CellOut& out) {
    std::vector<FoliageRecord> kept;
    if (f.mode == FoliageExport::Off || c.foliage.empty()) return kept;
    const std::vector<FoliageRecord> recs = cell_foliage_records(s, c, f.mode, out.foliageSkipped);
    kept.reserve(recs.size());
    for (FoliageRecord r : recs) {
        const uint32_t mi = r.model;
        if (mi >= f.manifestPos.size() || f.manifestPos[mi] == UINT32_MAX || !f.modelOk[mi]) {
            ++out.foliageFailedModel;
            continue;
        }
        r.model = f.manifestPos[mi];
        const uint16_t source = uint16_t((r.flags >> kFoliageSourceShift) & 3u);
        if (source == uint16_t(cw::ZoneSource::Scatter)) ++out.foliageScatter;
        if (source == uint16_t(cw::ZoneSource::Density)) ++out.foliageDensity;
        if (r.flags & kFoliageFlagCollider) ++out.mc.colliderTrees;
        out.mc.foliageTriangles += f.triangles[mi];
        if (out.foliageModels.empty() || out.foliageModels.back() != mi) out.foliageModels.push_back(mi);
        kept.push_back(r);
        ++out.mc.foliageRecords;
    }
    return kept;
}

std::vector<uint8_t> foliage_bytes(const std::vector<FoliageRecord>& recs) {
    std::vector<uint8_t> bytes(recs.size() * kFoliageRecordBytes);
    for (size_t i = 0; i < recs.size(); ++i) encode_foliage_record(recs[i], bytes.data() + i * kFoliageRecordBytes);
    return bytes;
}

void export_one_cell(const cw::WorldScene& s, const cw::Cell& c, const cw::TextureLookup& lookup,
                     const std::unordered_map<int64_t, size_t>& chunkAt, const fs::path& dir,
                     const WorldFolderOptions& opts, const FoliageCtx& fol, Fatal& fatal, CellOut& out) {
    const CellMesh terrain = build_cell_terrain(s.terrain, c, &out.warnings);
    const CellMesh water = build_cell_water(s, c, opts.waterTexelsPerChunk);
    const std::vector<CellMesh> colliders = opts.collision ? build_cell_colliders(s, c) : std::vector<CellMesh>{};
    out.records = cell_foliage_kept(s, c, fol, out);
    if (terrain.vertices() == 0 && water.vertices() == 0 && colliders.empty() && c.props.empty() && out.records.empty())
        return;

    const fs::path cellsDir = dir / "cells";
    CellImages img;
    out.mc.cell = &c;
    out.mc.glb = "cells/" + c.id + ".glb";

    if (terrain.vertices() > 0) {
        const int T = opts.texelsPerChunk, W = c.w * T, H = c.h * T;
        std::vector<uint8_t> rgb(size_t(W) * size_t(H) * 3, 128);
        cw::BakeSettings bs;
        bs.texelsPerChunk = T;
        bs.groundRepeats = opts.groundRepeats;
        bs.srgbColour = opts.srgbColour;
        std::vector<uint8_t> chunk;
        for (int y = c.cy0; y < c.cy0 + c.h; ++y)
            for (int x = c.cx0; x < c.cx0 + c.w; ++x) {
                const auto it = chunkAt.find(int64_t(y) * (int64_t(1) << 32) + x);
                if (it == chunkAt.end()) continue;   // no chunk there: stays grey, not a chunk to count
                if (!cw::bake_chunk_albedo(s.terrain.chunks[it->second], lookup, bs, chunk)) ++out.unbaked;
                if (chunk.size() < size_t(T) * T * 3) continue;
                for (int j = 0; j < T; ++j)
                    std::copy_n(chunk.begin() + ptrdiff_t(size_t(j) * T * 3), size_t(T) * 3,
                                rgb.begin() + ptrdiff_t(((size_t(y - c.cy0) * T + j) * W + size_t(x - c.cx0) * T) * 3));
            }
        const std::vector<uint8_t> png = encode_png_pixels(W, H, 3, rgb);
        const std::string name = c.id + "_albedo.png";
        if (png.empty() || !write_bytes(cellsDir / name, png)) {
            fatal.set("cannot write " + utf8(cellsDir / name));
            return;
        }
        img.albedoUri = name;
        out.mc.albedo = "cells/" + name;
        out.mc.albedoW = W;
        out.mc.albedoH = H;
        if (opts.unityTerrain) out.albedoRgb = std::move(rgb);   // the unityTerrain tiles assemble it
    }

    if (water.vertices() > 0 && cw::water_has_preset(s)) {
        const int WT = opts.waterTexelsPerChunk;
        std::vector<uint8_t> rgba;
        if (cw::bake_cell_water(s, c.rect, c.w * WT, c.h * WT, opts.srgbColour, rgba)) {
            const std::string name = c.id + "_water.png";
            const std::vector<uint8_t> png = encode_png_pixels(c.w * WT, c.h * WT, 4, rgba);
            if (png.empty() || !write_bytes(cellsDir / name, png)) {
                fatal.set("cannot write " + utf8(cellsDir / name));
                return;
            }
            img.waterUri = name;
            out.mc.water = "cells/" + name;
        }
    }

    // The cell's foliage records are written after the unityTerrain stage (it sets bit 3).

    for (const CellMesh& m : colliders) out.mc.colliderTriangles += m.idx.size() / 3;
    out.mc.colliderParts = colliders.size();
    std::string err;
    if (!write_cell_gltf(utf8(cellsDir / (c.id + ".glb")), c, terrain, water.vertices() ? &water : nullptr, colliders,
                         img, &err)) {
        fatal.set("cannot write " + utf8(cellsDir / (c.id + ".glb")) + ": " + err);
        return;
    }
    out.written = true;
}

/// The output folder rule (world_folder.h).
bool prepare_dir(const fs::path& d, const std::string& dirUtf8, bool overwrite, std::string& error) {
    std::error_code ec;
    const fs::path ld = long_path(d, true);   // the clear walks children that may be past MAX_PATH
    if (fs::exists(ld, ec)) {
        if (!fs::is_directory(ld, ec)) { error = "output folder not empty: " + dirUtf8; return false; }
        const bool empty = fs::directory_iterator(ld, ec) == fs::directory_iterator();
        if (ec) { error = "cannot read output folder " + dirUtf8 + ": " + ec.message(); return false; }
        if (!empty) {
            if (!overwrite) { error = "output folder not empty: " + dirUtf8; return false; }
            if (!is_world_export_folder(dirUtf8)) {
                error = "output folder not empty: " + dirUtf8 +
                        (fs::exists(ld / "manifest.json", ec)
                             ? " (its manifest.json is not a castlemist world export, so it is not replaced)"
                             : " (no manifest.json: not a previous world export, so it is not replaced)");
                return false;
            }
            // manifest.json first: an interrupted clear never leaves a manifest
            // describing a folder that is half gone.
            fs::remove(ld / "manifest.json", ec);
            if (ec) { error = "cannot clear " + utf8(d / "manifest.json") + ": " + ec.message(); return false; }
            std::vector<fs::path> entries;
            for (auto it = fs::directory_iterator(ld, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
                entries.push_back(it->path());
            if (ec) { error = "cannot read output folder " + dirUtf8 + ": " + ec.message(); return false; }
            for (const fs::path& p : entries)
                if (!remove_tree(p, ec)) {
                    error = "cannot clear " + utf8(d / p.filename()) + ": " + ec.message();
                    return false;
                }
        }
    }
    make_dirs(d / "cells", ec);
    if (!ec) make_dirs(d / "models", ec);
    if (!ec) make_dirs(d / "textures", ec);   // the manifest names it even when empty
    if (ec) { error = "cannot create " + dirUtf8 + ": " + ec.message(); return false; }
    return true;
}

/// Folder size by kind (WorldFolderReport::bytes).
std::map<std::string, uint64_t> folder_bytes(const fs::path& d) {
    std::map<std::string, uint64_t> b = {{"prop-textures", 0}, {"terrain", 0}, {"geometry", 0}, {"sky", 0}, {"foliage", 0},
                                          {"unity-terrain", 0}};
    const fs::path ld = long_path(d, true);
    std::error_code ec;
    // The first folder below @p d, by string: lexically_relative does not split the
    // extended-length `\\?\C:\...` paths walk_files gives (libstdc++), so every top
    // came out empty and prop-textures / terrain were always 0.
    using Str = fs::path::string_type;
    const Str base = ld.native();
    const fs::path::value_type seps[] = {'\\', '/', 0};
    walk_files(d, ec, [&](const fs::path& f, uint64_t n) {
        const Str& w = f.native();
        std::string top;
        if (w.size() > base.size() + 1 && w.compare(0, base.size(), base) == 0 &&
            (w[base.size()] == '\\' || w[base.size()] == '/')) {
            const Str rest = w.substr(base.size() + 1);
            const size_t cut = rest.find_first_of(seps);
            if (cut != Str::npos) top = utf8(fs::path(rest.substr(0, cut)));
        }
        const std::string ext = utf8(f.extension());
        if (top == "sky") b["sky"] += n;
        else if (ext == ".glb" || ext == ".fbx") b["geometry"] += n;
        else if (ext == ".png" && (top == "models" || top == "textures")) b["prop-textures"] += n;
        else if (ext == ".png" && top == "cells") b["terrain"] += n;
        else if (ext == ".bin" && top == "foliage") b["foliage"] += n;
        else if (top == "terrain") b["unity-terrain"] += n;
    });
    return b;
}

/// The manifest's sky block from what write_skybox wrote: the first mode
/// written in its own folder is the default; its equirect when baked.
json sky_block(const fs::path& dir) {
    std::ifstream in(long_path(dir / "sky" / "sky.json"), std::ios::binary);
    if (!in) return nullptr;
    json sj;
    try { sj = json::parse(in); } catch (...) { return nullptr; }
    if (!sj.contains("modes") || !sj["modes"].is_array()) return nullptr;
    for (const json& m : sj["modes"]) {
        if (!m.is_object() || !m.value("aliasOf", json()).is_null()) continue;
        const bool baked = m.value("baked", false), skybox = m.value("skybox", false);
        if (!baked && !skybox) continue;
        const std::string name = m.value("name", std::string());
        if (name.empty()) continue;
        return {{"json", "sky/sky.json"},
                {"default", name},
                {"equirect", baked ? json("sky/" + name + "/baked/equirect.png") : json(nullptr)}};
    }
    return nullptr;
}

} // namespace

namespace {

/// The export itself; may throw (export_world_folder catches). @p manifestWritten
/// is set once manifest.json exists, so a later exception can remove it.
WorldFolderReport export_world_folder_impl(const cw::WorldScene& scene, ModelSource& src, const sky::SkyInputs* sky,
                                           const std::string& dirUtf8, const WorldFolderOptions& opts,
                                           bool& manifestWritten) {
    WorldFolderReport rep;
    rep.dir = dirUtf8;
    auto t = Clock::now();
    auto lap = [&](const char* stage) {
        const auto now = Clock::now();
        rep.seconds[stage] = std::chrono::duration<double>(now - t).count();
        t = now;
    };
    if (opts.texelsPerChunk < 1 || opts.waterTexelsPerChunk < 1 || opts.maxTextureSize < 1) {
        rep.error = "texelsPerChunk, waterTexelsPerChunk and maxTextureSize must be positive";
        return rep;
    }
    const fs::path dir = u8path(dirUtf8);
    const unsigned threads = opts.threads > 0 ? static_cast<unsigned>(opts.threads)
                                              : std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::string> warnings;
    Fatal fatal;
    fatal.cancel = opts.cancel;

    // 1. The cells the region keeps, without stats (no splits): their props name the needed models.
    //    A region that keeps nothing is refused before the folder is touched.
    std::vector<uint32_t> needed;
    {
        const cw::CellPlan first = cw::partition_cells(scene, cw::CellStats{}, opts.budget, opts.region);
        if (!opts.region.whole() && first.cells.empty()) {
            const cw::ChunkRegion& g = opts.region;
            rep.error = "region " + std::to_string(g.cx0) + "," + std::to_string(g.cy0) + "," + std::to_string(g.cx1) +
                        "," + std::to_string(g.cy1) + " keeps no cells (the terrain is " +
                        std::to_string(scene.terrain.chunksX) + " x " + std::to_string(scene.terrain.chunksY) +
                        " chunks)";
            return rep;
        }
        std::vector<char> use(scene.models.size(), 0);
        for (const cw::Cell& c : first.cells)
            for (uint32_t pi : c.props)
                if (pi < scene.props.size() && scene.props[pi].model < use.size()) use[scene.props[pi].model] = 1;
        // Zone-only models are used too (world-foliage spec D2, D7): the models of the
        // zone instances in the kept cells that the foliage mode keeps.
        if (opts.foliage != FoliageExport::Off)
            for (const cw::Cell& c : first.cells)
                for (uint32_t zi : c.foliage) {
                    if (zi >= scene.zones.instances.size()) continue;
                    const cw::ZoneInstance& z = scene.zones.instances[zi];
                    if (opts.foliage == FoliageExport::Colliders && z.source != cw::ZoneSource::Collider) continue;
                    if (z.model < use.size()) use[z.model] = 1;
                }
        for (uint32_t i = 0; i < use.size(); ++i) if (use[i]) needed.push_back(i);
    }
    if (!prepare_dir(dir, dirUtf8, opts.overwrite, rep.error)) return rep;
    lap("world-cells");

    // 2. The model stage; dat textures go once into the shared pool (W8).
    const auto pool = std::make_shared<SharedImagePool>(utf8(dir / "textures"));
    std::vector<ModelOut> mouts(needed.size());
    run_pool(needed.size(), threads, fatal,
             [&](size_t k) { export_one_model(scene, needed[k], src, dir, opts, pool, mouts[k]); });
    src.release_textures();   // W9: the models are written; their textures are not needed again
    lap("models");
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }

    // 3. partition_cells from the measurements.
    cw::CellStats st;
    st.modelTriangles.assign(scene.models.size(), 0);
    st.modelTextures.assign(scene.models.size(), {});
    std::vector<const ModelOut*> outOf(scene.models.size(), nullptr);
    for (const ModelOut& o : mouts) {
        outOf[o.mm.index] = &o;
        if (!o.mm.ok) continue;
        st.modelTriangles[o.mm.index] = o.mm.triangles;
        st.modelTextures[o.mm.index] = o.textures;
        for (const auto& [id, b] : o.texBytes) st.textureBytes[id] = b;
    }
    st.bakedBytesPerChunk = uint64_t(opts.texelsPerChunk) * opts.texelsPerChunk * 3 +
                            uint64_t(opts.waterTexelsPerChunk) * opts.waterTexelsPerChunk * 4;
    const cw::CellPlan plan = cw::partition_cells(scene, st, opts.budget, opts.region);
    warnings.insert(warnings.end(), plan.warnings.begin(), plan.warnings.end());
    if (plan.propsOutsideRegion)
        warnings.push_back("cells: " + std::to_string(plan.propsOutsideRegion) +
                           " placements and collision instances outside the region left out");
    if (plan.foliageOutsideRegion && opts.foliage != FoliageExport::Off)
        warnings.push_back("foliage: " + std::to_string(plan.foliageOutsideRegion) +
                           " zone instances outside the region left out");
    for (const ModelOut& o : mouts) {
        warnings.insert(warnings.end(), o.warnings.begin(), o.warnings.end());
        if (!o.mm.ok) warnings.push_back("models: " + std::to_string(o.mm.fileId) + " failed: " + o.mm.error);
    }

    // 4. The kept cells' terrain textures, once each.
    std::unordered_map<int64_t, size_t> chunkAt;   // (cy << 32) + cx -> terrain.chunks index
    for (size_t i = 0; i < scene.terrain.chunks.size(); ++i)
        chunkAt.emplace(int64_t(scene.terrain.chunks[i].cy) * (int64_t(1) << 32) + scene.terrain.chunks[i].cx, i);
    std::vector<size_t> keptChunks;
    for (const cw::Cell& c : plan.cells) {
        if (c.all) continue;
        for (int y = c.cy0; y < c.cy0 + c.h; ++y)
            for (int x = c.cx0; x < c.cx0 + c.w; ++x) {
                const auto it = chunkAt.find(int64_t(y) * (int64_t(1) << 32) + x);
                if (it != chunkAt.end()) keptChunks.push_back(it->second);
            }
    }
    std::sort(keptChunks.begin(), keptChunks.end());
    const std::vector<uint32_t> texIds =
        scene.terrain.present ? cw::terrain_texture_ids(scene.terrain, &keptChunks) : std::vector<uint32_t>{};
    std::vector<std::shared_ptr<const CappedTexture>> terrainTex(texIds.size());
    run_pool(texIds.size(), threads, fatal,
             [&](size_t k) { terrainTex[k] = src.texture(texIds[k], opts.maxTextureSize); });
    src.release_textures();   // W9: terrainTex holds them until the cells are baked
    lap("terrain-textures");
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }
    std::unordered_map<uint32_t, const CappedTexture*> texById;
    for (size_t k = 0; k < texIds.size(); ++k)
        if (terrainTex[k] && !terrainTex[k]->mips.empty()) texById.emplace(texIds[k], terrainTex[k].get());
    const cw::TextureLookup lookup = [&texById](uint32_t id) -> const CappedTexture* {
        const auto it = texById.find(id);
        return it == texById.end() ? nullptr : it->second;
    };

    // 5. Bake and write each cell, and its foliage records (world-foliage spec D7).
    FoliageCtx fol;
    fol.mode = scene.zones.present ? opts.foliage : FoliageExport::Off;
    fol.manifestPos.assign(scene.models.size(), UINT32_MAX);
    fol.modelOk.assign(scene.models.size(), 0);
    fol.triangles.assign(scene.models.size(), 0);
    for (size_t k = 0; k < mouts.size(); ++k) {
        const uint32_t mi = mouts[k].mm.index;
        fol.manifestPos[mi] = static_cast<uint32_t>(k);
        fol.modelOk[mi] = mouts[k].mm.ok ? 1 : 0;
        fol.triangles[mi] = mouts[k].mm.triangles;
    }
    if (fol.mode != FoliageExport::Off) {
        bool any = false;
        for (const cw::Cell& c : plan.cells) any = any || !c.foliage.empty();
        std::error_code ec;
        if (any) make_dirs(dir / "foliage", ec);
        if (ec) {
            rep.error = "cannot create " + utf8(dir / "foliage") + ": " + ec.message();
            return rep;
        }
    }
    std::vector<CellOut> couts(plan.cells.size());
    run_pool(plan.cells.size(), threads, fatal, [&](size_t k) {
        export_one_cell(scene, plan.cells[k], lookup, chunkAt, dir, opts, fol, fatal, couts[k]);
    });
    lap("cells");
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }

    // 5b. The unityTerrain tiles (world-foliage spec D7), before the records are written:
    //     the Terrain trees get flag bit 3 here.
    json unityTerrain = nullptr;
    if (opts.unityTerrain && scene.terrain.present) {
        std::vector<std::vector<FoliageRecord>> recs(plan.cells.size());
        std::vector<std::vector<uint8_t>> albedo(plan.cells.size());
        for (size_t k = 0; k < couts.size(); ++k) {
            if (!couts[k].written) continue;
            recs[k] = std::move(couts[k].records);
            albedo[k] = std::move(couts[k].albedoRgb);
        }
        std::vector<ManifestModel> mm;
        mm.reserve(mouts.size());
        for (const ModelOut& o : mouts) mm.push_back(o.mm);
        UnityTerrainInput ui;
        ui.scene = &scene;
        ui.plan = &plan;
        ui.cellAlbedoRgb = &albedo;
        ui.texelsPerChunk = opts.texelsPerChunk;
        ui.records = &recs;
        ui.models = &mm;
        std::string err;
        unityTerrain = write_unity_terrain(ui, utf8(dir), warnings, &err);
        if (!err.empty()) { rep.error = err; return rep; }
        for (size_t k = 0; k < couts.size(); ++k) {
            couts[k].albedoRgb = {};
            if (couts[k].written) couts[k].records = std::move(recs[k]);
        }
        lap("unity-terrain");
    }

    // 5c. Each written cell's foliage records (world-foliage spec D7), only when it has any.
    run_pool(couts.size(), threads, fatal, [&](size_t k) {
        CellOut& c = couts[k];
        if (!c.written || c.records.empty()) return;
        const std::string name = plan.cells[k].id + ".bin";
        if (!write_bytes(dir / "foliage" / name, foliage_bytes(c.records))) {
            fatal.set("cannot write " + utf8(dir / "foliage" / name));
            return;
        }
        c.mc.foliageFile = "foliage/" + name;
        c.records = {};
    });
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }
    std::vector<ManifestCell> mcells;
    size_t unbaked = 0, folSkipped = 0, folFailed = 0, folScatter = 0, folDensity = 0;
    std::vector<char> folUsed(mouts.size(), 0);   // models[] positions with a written record
    for (const CellOut& c : couts) {
        warnings.insert(warnings.end(), c.warnings.begin(), c.warnings.end());
        unbaked += c.unbaked;
        folSkipped += c.foliageSkipped;
        folFailed += c.foliageFailedModel;
        if (!c.written) continue;
        mcells.push_back(c.mc);
        folScatter += c.foliageScatter;
        folDensity += c.foliageDensity;
        for (uint32_t mi : c.foliageModels) folUsed[fol.manifestPos[mi]] = 1;
    }
    if (folSkipped)
        warnings.push_back("foliage: " + std::to_string(folSkipped) + " placements with a degenerate transform skipped");
    if (folFailed)
        warnings.push_back("foliage: " + std::to_string(folFailed) + " placements of failed or missing models skipped");
    if (unbaked) warnings.push_back("terrain: " + std::to_string(unbaked) + " chunks with no usable colour texture baked grey");
    texById.clear();
    terrainTex.clear();

    // 6. The sky.
    json skyJson = nullptr;
    if (!opts.sky) {
        // not asked for: sky null, nothing to say
    } else if (!sky) {
        warnings.push_back("sky: no sky inputs; sky: null");
    } else {
        sky::SkyExportOptions so;
        so.approximate = opts.skyApproximate;
        const sky::SkyExportReport sr = sky::write_skybox(*sky, utf8(dir), "sky", so);
        if (!sr.ok && sr.error.rfind("cannot ", 0) == 0) { rep.error = "sky: " + sr.error; return rep; }
        for (const std::string& w : sr.warnings) warnings.push_back("sky: " + w);
        if (sr.ok) skyJson = sky_block(dir);
        if (!sr.ok) warnings.push_back("sky: " + sr.error + "; sky: null");
        else if (skyJson.is_null()) warnings.push_back("sky: sky.json names no written mode; sky: null");
    }
    lap("sky");
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }

    // 7. The probe and the spawn: a ray down (+Z) from above the region centre.
    std::string err;
    if (!write_frame_probe(utf8(dir / "probe.glb"), &err)) { rep.error = "cannot write probe.glb: " + err; return rep; }
    json spawn = nullptr;
    {
        float lo[2] = {0, 0}, hi[2] = {0, 0};
        bool any = false;
        for (const ManifestCell& mc : mcells) {
            const cw::Cell& c = *mc.cell;
            if (!any) { lo[0] = c.rect[0]; lo[1] = c.rect[1]; hi[0] = c.rect[2]; hi[1] = c.rect[3]; any = true; }
            lo[0] = std::min(lo[0], c.rect[0]); lo[1] = std::min(lo[1], c.rect[1]);
            hi[0] = std::max(hi[0], c.rect[2]); hi[1] = std::max(hi[1], c.rect[3]);
        }
        if (!any && scene.hasBounds) { lo[0] = scene.bounds[0]; lo[1] = scene.bounds[1]; hi[0] = scene.bounds[2]; hi[1] = scene.bounds[3]; }
        const cw::CollisionIndex ci = cw::CollisionIndex::build(scene, true);
        if (!ci.empty()) {
            float blo[3], bhi[3];
            ci.bounds(blo, bhi);
            const float origin[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), blo[2] - 1000.f};
            const float down[3] = {0, 0, 1};
            if (auto hit = ci.raycast(origin, down, bhi[2] - blo[2] + 2000.f))
                spawn = {{"map", {hit->point[0], hit->point[1], hit->point[2]}}};
        }
        if (spawn.is_null()) warnings.push_back("spawn: no ground under the region centre; spawn: null");
    }

    // 8. The manifest, last (not once cancelled).
    if (fatal.stopped()) { rep.error = fatal.error; return rep; }
    size_t animated = 0, resolved = 0, failedDecode = 0;
    std::unordered_map<uint32_t, std::string> clipOf;
    for (const ManifestCell& mc : mcells)
        for (uint32_t pi : mc.cell->props) {
            if (pi >= scene.props.size() || scene.props[pi].animSequence == 0) continue;
            ++animated;
            const cw::PropInstance& p = scene.props[pi];
            const ModelOut* o = p.model < outOf.size() ? outOf[p.model] : nullptr;
            if (!o || !o->mm.ok) continue;
            const int c = cw::resolve_prop_clip(o->clipTokens, p.animSequence);
            if (c >= 0 && static_cast<size_t>(c) < o->mm.clips.size()) {
                ++resolved;
                clipOf[pi] = o->mm.clips[static_cast<size_t>(c)];
            } else if (std::find(o->allTokens.begin(), o->allTokens.end(), p.animSequence) != o->allTokens.end()) {
                ++failedDecode;   // the clip exists but did not decode (sub-project 3's split)
            }
        }

    WorldManifestInput in;
    in.scene = &scene;
    WorldFolderOptions manOpts = opts;
    manOpts.region = plan.region;   // what was written: the request snapped to whole blocks
    in.opts = &manOpts;
    for (const ModelOut& o : mouts) in.models.push_back(o.mm);
    in.cells = mcells;
    in.sky = skyJson;
    in.spawn = spawn;
    in.counts = {{"chunksUnbaked", unbaked}, {"animated", animated}, {"animatedResolved", resolved},
                 {"clipsFailedDecode", failedDecode},
                 {"foliage", {{"scatter", folScatter}, {"density", folDensity}, {"skipped", folSkipped},
                              {"failedModel", folFailed}}}};
    if (fol.mode != FoliageExport::Off) in.foliage = foliage_manifest_json(scene, in.models, folUsed);
    in.unityTerrain = unityTerrain;
    in.warnings = warnings;
    in.textures = {{"dir", "textures"}, {"count", pool->written()}};
    in.clipOf = [&clipOf](uint32_t pi) {
        const auto it = clipOf.find(pi);
        return it == clipOf.end() ? std::string() : it->second;
    };
    const json man = world_manifest_json(in);
    {
        const fs::path tmp = dir / "manifest.json.tmp";
        {
            std::ofstream f(long_path(tmp), std::ios::binary);
            f << man.dump(2) << '\n';
            f.close();   // check after the final flush (disk full), never before
            if (f.fail()) { rep.error = "cannot write " + utf8(tmp); return rep; }
        }
        std::error_code ec;
        fs::rename(long_path(tmp), long_path(dir / "manifest.json"), ec);
        if (ec) { rep.error = "cannot rename " + utf8(tmp) + ": " + ec.message(); return rep; }
        manifestWritten = true;
    }
    lap("manifest");

    const json& cn = man["counts"];
    rep.ok = true;
    rep.manifest = utf8(dir / "manifest.json");
    rep.cells = cn.value("cells", size_t(0));
    rep.modelsFailed = cn.value("modelsFailed", size_t(0));
    rep.modelsWritten = cn.value("models", size_t(0)) - rep.modelsFailed;
    rep.propsWritten = cn.value("propsWritten", size_t(0));
    rep.propsSkipped = cn.value("propsSkipped", size_t(0));
    rep.chunksUnbaked = unbaked;
    rep.animated = animated;
    rep.animatedResolved = resolved;
    rep.clipsFailedDecode = failedDecode;
    for (const ModelOut& o : mouts)
        if (o.empty) ++rep.modelsEmpty;
    rep.warnings = man["warnings"].get<std::vector<std::string>>();
    if (cn.contains("foliage")) rep.foliage = cn["foliage"];
    rep.bytes = folder_bytes(dir);
    return rep;
}

} // namespace

WorldFolderReport export_world_folder(const cw::WorldScene& scene, ModelSource& src, const sky::SkyInputs* sky,
                                      const std::string& dirUtf8, const WorldFolderOptions& opts) {
    bool manifestWritten = false;
    std::string error;
    try {
        return export_world_folder_impl(scene, src, sky, dirUtf8, opts, manifestWritten);
    } catch (const std::exception& e) {
        error = std::string("export: ") + e.what();
    } catch (...) {
        error = "export: unknown exception";
    }
    // Never throws (spec "Error handling"); a failed export leaves no manifest.json.
    if (manifestWritten) {
        std::error_code ec;
        fs::remove(long_path(u8path(dirUtf8) / "manifest.json"), ec);
    }
    WorldFolderReport rep;
    rep.dir = dirUtf8;
    rep.error = error;
    return rep;
}

bool is_world_export_folder(const std::string& dirUtf8) {
    std::error_code ec;
    const fs::path p = long_path(u8path(dirUtf8) / "manifest.json");
    if (!fs::is_regular_file(p, ec)) return false;
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    const json j = json::parse(in, nullptr, false);
    return !j.is_discarded() && j.is_object() && j.contains("format") && j["format"] == "castlemist-world";
}

json report_json(const WorldFolderReport& r) {
    return json{{"ok", r.ok},
                {"error", r.error.empty() ? json(nullptr) : json(r.error)},
                {"dir", r.dir},
                {"manifest", r.manifest.empty() ? json(nullptr) : json(r.manifest)},
                {"counts",
                 {{"cells", r.cells},
                  {"modelsWritten", r.modelsWritten},
                  {"modelsFailed", r.modelsFailed},
                  {"modelsEmpty", r.modelsEmpty},
                  {"propsWritten", r.propsWritten},
                  {"propsSkipped", r.propsSkipped},
                  {"chunksUnbaked", r.chunksUnbaked},
                  {"animated", r.animated},
                  {"animatedResolved", r.animatedResolved},
                  {"clipsFailedDecode", r.clipsFailedDecode},
                  {"foliage", r.foliage}}},
                {"seconds", r.seconds},
                {"bytes", r.bytes},
                {"warnings", r.warnings}};
}

} // namespace castlemist::exportgltf
