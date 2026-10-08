/// @file
/// @brief Terrain geometry (docs/research/gw2-world-frame.md §3) and
///        materials (§4).

#include "castlemist/world/terrain.h"

#include "castlemist/native/cmp_decompress_method0.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace castlemist::world {

TerrainLayout terrain_layout(uint32_t dimX, uint32_t dimY, uint32_t vertsPerChunkSide, size_t heightCount) {
    TerrainLayout l;
    if (dimX == 0 || dimY == 0 || heightCount == 0) {
        l.why = "empty trn (dims " + std::to_string(dimX) + "x" + std::to_string(dimY) + ", " +
                std::to_string(heightCount) + " samples)";
        return l;
    }

    // §3.2. With the field, segments = verticesPerChunkSide and each chunk
    // stores (segments + 3)^2 samples (after spjinx/t3d TerrainRenderer.ts
    // getChunkResolution). Without it, solve for the one segment count the
    // dims and the sample count agree on (dims = chunks * segments, proven on
    // every map that has the field) -- T3D instead assumes 32 there.
    int segments = 0;
    if (vertsPerChunkSide > 0) {
        segments = (int)vertsPerChunkSide;
    } else {
        int found = 0;
        for (uint32_t s = 1; s <= dimX && s <= dimY; ++s) {
            if (dimX % s || dimY % s) continue;
            if ((size_t)(dimX / s) * (dimY / s) * (s + 3) * (s + 3) != heightCount) continue;
            if (!found) segments = (int)s;
            ++found;
        }
        if (found != 1) {
            l.why = "no verticesPerChunkSide and " + std::to_string(found) + " segment counts fit dims " +
                    std::to_string(dimX) + "x" + std::to_string(dimY) + " and " + std::to_string(heightCount) +
                    " samples";
            return l;
        }
    }
    const size_t stored = (size_t)segments + 3, perChunk = stored * stored;
    if (heightCount % perChunk) {
        l.why = std::to_string(heightCount) + " samples are not a whole number of " + std::to_string(stored) + "x" +
                std::to_string(stored) + " chunks";
        return l;
    }
    const size_t count = heightCount / perChunk;

    // Chunk grid: chunksX = sqrt(dims[0] * count / dims[1]), chunksY = count / chunksX
    // (after spjinx/t3d TerrainRenderer.ts parseNumChunks), required to be whole.
    const double cxF = std::sqrt((double)dimX * (double)count / (double)dimY);
    const long cx = std::lround(cxF);
    if (cx <= 0 || std::fabs(cxF - (double)cx) > 1e-9 || count % (size_t)cx) {
        l.why = std::to_string(count) + " chunks do not form a grid with aspect " + std::to_string(dimX) + ":" +
                std::to_string(dimY);
        return l;
    }
    l.chunksX = (int)cx;
    l.chunksY = (int)(count / (size_t)cx);
    l.segments = segments;
    l.stored = (int)stored;
    l.ok = true;
    return l;
}

Terrain build_terrain(const castlemist::model::Extractor::MapTerrain& t, std::vector<std::string>& warnings) {
    Terrain out;
    if (t.heights.empty() || t.dimX == 0 || t.dimY == 0) {
        warnings.push_back("terrain: no trn height samples; map has no terrain");
        return out;
    }
    // §2: the terrain is placed by parm.rect; without it there is nothing to place it by.
    if (!t.hasRect) {
        warnings.push_back("terrain: no parm rect; terrain left out (no rect is invented)");
        return out;
    }
    const float X0 = t.rect[0], Y0 = t.rect[1], X1 = t.rect[2], Y1 = t.rect[3];
    if (!(X1 > X0) || !(Y1 > Y0)) {
        warnings.push_back("terrain: parm rect is empty or inverted; terrain left out");
        return out;
    }
    const TerrainLayout l = terrain_layout(t.dimX, t.dimY, t.vertsPerChunkSide, t.heights.size());
    if (!l.ok) {
        warnings.push_back("terrain: " + l.why + "; terrain left out");
        return out;
    }

    out.present = true;
    out.chunksX = l.chunksX;
    out.chunksY = l.chunksY;
    out.chunks.reserve((size_t)l.chunksX * l.chunksY);
    const int S = l.stored, n = l.segments + 1;
    for (int cy = 0; cy < l.chunksY; ++cy) {
        for (int cx = 0; cx < l.chunksX; ++cx) {
            TerrainChunk ch;
            ch.cx = cx;
            ch.cy = cy;
            // §3.3: cx runs east from rect[0], cy runs south from rect[3]. On
            // the test maps this is where spjinx/t3d TerrainRenderer.ts
            // (chunk position, odd/even branch) puts every chunk.
            ch.rect[0] = X0 + (X1 - X0) * (float)cx / (float)l.chunksX;
            ch.rect[2] = X0 + (X1 - X0) * (float)(cx + 1) / (float)l.chunksX;
            ch.rect[3] = Y1 - (Y1 - Y0) * (float)cy / (float)l.chunksY;
            ch.rect[1] = Y1 - (Y1 - Y0) * (float)(cy + 1) / (float)l.chunksY;
            ch.samples = n;
            // §3.1: the chunk's own samples are the inner (segments+1)^2; the
            // outer ring is dropped (after spjinx/t3d TerrainRenderer.ts, chunk
            // vertex loop). Row 0 = north edge, column 0 = west edge, as stored.
            ch.heights.resize((size_t)n * n);
            const float* blk = &t.heights[((size_t)cy * l.chunksX + cx) * (size_t)S * S];
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) ch.heights[(size_t)j * n + i] = blk[(size_t)(j + 1) * S + (i + 1)];
            out.chunks.push_back(std::move(ch));
        }
    }
    return out;
}

float terrain_height_at(const Terrain& t, float x, float y, bool* inside) {
    if (inside) *inside = false;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!t.present || t.chunksX <= 0 || t.chunksY <= 0 || t.chunks.size() != (size_t)t.chunksX * t.chunksY)
        return nan;
    const TerrainChunk& nw = t.chunks.front();
    const TerrainChunk& se = t.chunks.back();
    const float X0 = nw.rect[0], Y1 = nw.rect[3], X1 = se.rect[2], Y0 = se.rect[1];
    if (!(x >= X0 && x <= X1 && y >= Y0 && y <= Y1)) return nan;

    // The chunk holding (x, y); a point on a shared edge may go either way,
    // the heights there are equal (§3.1).
    int cx = (int)std::floor((x - X0) / (X1 - X0) * (float)t.chunksX);
    int cy = (int)std::floor((Y1 - y) / (Y1 - Y0) * (float)t.chunksY);
    cx = cx < 0 ? 0 : (cx >= t.chunksX ? t.chunksX - 1 : cx);
    cy = cy < 0 ? 0 : (cy >= t.chunksY ? t.chunksY - 1 : cy);
    const TerrainChunk& c = t.chunks[(size_t)cy * t.chunksX + cx];
    const int n = c.samples;
    if (n < 2 || c.heights.size() != (size_t)n * n) return nan;

    // §3.4: bilinear over the chunk's samples; column i at x0 + i*w/(n-1),
    // row j at y1 - j*h/(n-1).
    const int seg = n - 1;
    float u = (x - c.rect[0]) / (c.rect[2] - c.rect[0]) * (float)seg;
    float v = (c.rect[3] - y) / (c.rect[3] - c.rect[1]) * (float)seg;
    u = u < 0 ? 0 : (u > (float)seg ? (float)seg : u);
    v = v < 0 ? 0 : (v > (float)seg ? (float)seg : v);
    int i0 = (int)u, j0 = (int)v;
    if (i0 >= seg) i0 = seg - 1;
    if (j0 >= seg) j0 = seg - 1;
    const float tx = u - (float)i0, ty = v - (float)j0;
    auto H = [&](int i, int j) { return c.heights[(size_t)j * n + i]; };
    const float a = H(i0, j0) + (H(i0 + 1, j0) - H(i0, j0)) * tx;
    const float b = H(i0, j0 + 1) + (H(i0 + 1, j0 + 1) - H(i0, j0 + 1)) * tx;
    if (inside) *inside = true;
    return a + (b - a) * ty;
}

namespace {

/// GW2 Token decode: base-23 over "abcdefghiklmnopvrstuwxy" after subtracting
/// 0x30000000 (the rule src/extract/game_shader.cpp decode_token documents,
/// verified there against the client). §4.1: the terrain texture tokens
/// decode to "color".."colord", "blend", "modx", "normal".."normald", "ramp".
std::string token_name(uint32_t token) {
    static const char* kAlpha = "abcdefghiklmnopvrstuwxy";
    uint32_t v = token - 0x30000000u;
    std::string out;
    while (v) { out.push_back(kAlpha[v % 23]); v /= 23; }
    return out;
}

/// "base" -> 0, "baseb" -> 1, "basec" -> 2, "based" -> 3; -1 otherwise.
int slot_of(const std::string& name, const char* base) {
    const size_t n = std::strlen(base);
    if (name.compare(0, n, base) != 0) return -1;
    if (name.size() == n) return 0;
    if (name.size() == n + 1 && name[n] >= 'b' && name[n] <= 'd') return name[n] - 'a';
    return -1;
}

std::string chunk_name(size_t i, const TerrainChunk& c) {
    return "chunk " + std::to_string(i) + " (" + std::to_string(c.cx) + ", " + std::to_string(c.cy) + ")";
}

using Ex = castlemist::model::Extractor;

/// The PIMG file's PGTB table, or `present = false` with a warning.
Ex::MapPagedImage read_paged_image(uint32_t fileId, Gw2Dat& dat, const nlohmann::json& tpl,
                                   std::vector<std::string>& warnings) {
    Ex::MapPagedImage pimg;
    const std::string what = "terrain materials: pagedImage " + std::to_string(fileId);
    if (!fileId) {
        warnings.push_back("terrain materials: trn names no pagedImage; no blend pages");
        return pimg;
    }
    const uint32_t base = get_by_base_id(dat, fileId);
    if (base == 0 || base > dat.mft_data_list.size()) {
        warnings.push_back(what + " is not in the dat; no blend pages");
        return pimg;
    }
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);   // I/O failure propagates
    try {
        std::vector<uint8_t> bytes = e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
        pimg = Ex(bytes, tpl).parsePagedImage();
    } catch (const std::exception& ex) {
        warnings.push_back(what + " unreadable (" + ex.what() + "); no blend pages");
        return Ex::MapPagedImage{};
    }
    if (!pimg.present) warnings.push_back(what + " has no PGTB table; no blend pages");
    return pimg;
}

} // namespace

void resolve_terrain_materials(Terrain& t, const castlemist::model::Extractor::MapTerrainMaterials& m, Gw2Dat& dat,
                               const nlohmann::json& tpl, std::vector<std::string>& warnings) {
    if (!t.present) return;
    if (!m.present) {
        warnings.push_back("terrain materials: trn has no materials; terrain left untextured");
        return;
    }
    // §4.1: materials[i] belongs to chunk i, i = cy * chunksX + cx, the same
    // index Terrain::chunks uses (§3).
    if (m.chunks.size() != t.chunks.size()) {
        warnings.push_back("terrain materials: " + std::to_string(m.chunks.size()) + " chunk materials for " +
                           std::to_string(t.chunks.size()) + " terrain chunks; terrain left untextured");
        return;
    }

    // §4.2: the paged image (PIMG) holding the blend pages, by (layer, page x, page y).
    const Ex::MapPagedImage pimg = read_paged_image(m.pimgFileId, dat, tpl, warnings);
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, const Ex::MapPagedImage::Page*> pages;
    uint32_t pagesX = 0, pagesY = 0;
    for (const auto& p : pimg.strippedPages) {
        if (p.layer > 1) continue;
        pages[{p.layer, p.coord[0], p.coord[1]}] = &p;
        pagesX = std::max(pagesX, p.coord[0] + 1);
        pagesY = std::max(pagesY, p.coord[1] + 1);
    }
    // Chunks per page side: the one n whose ceil(chunks / n) is the page grid
    // on both axes (4 on every test map, §4.2) -- derived, not assumed.
    int perPage = 0;
    if (!pages.empty()) {
        int found = 0;
        for (int n = 1; n <= std::max(t.chunksX, t.chunksY); ++n)
            if ((uint32_t)((t.chunksX + n - 1) / n) == pagesX && (uint32_t)((t.chunksY + n - 1) / n) == pagesY) {
                if (!found) perPage = n;
                ++found;
            }
        if (found != 1) {
            warnings.push_back("terrain materials: page grid " + std::to_string(pagesX) + "x" + std::to_string(pagesY) +
                               " fits " + std::to_string(found) + " chunks-per-page values for " +
                               std::to_string(t.chunksX) + "x" + std::to_string(t.chunksY) + " chunks; no blend pages");
            perPage = 0;
        }
    }

    std::set<std::string> dropped;   // tokens bound but not kept (§4.1)
    size_t droppedCount = 0;
    for (size_t i = 0; i < t.chunks.size(); ++i) {
        TerrainChunk& ch = t.chunks[i];
        const auto& c = m.chunks[i];
        TerrainMaterial mat;
        mat.materialFileId = c.materialFileId;
        for (int k = 0; k < 3; ++k) mat.tiling[k] = c.tiling[k];
        mat.uvScale = 0;   // §4.4: UNPROVEN
        mat.textureFileIds.assign(4, 0);
        mat.normalFileIds.assign(4, 0);
        mat.pickerScale = perPage ? 1.0f / (float)perPage : 0.0f;
        bool ok = true, anyColour = false;
        for (size_t j = 0; j < c.texIndices.size(); ++j) {
            const uint32_t k = c.texIndices[j];
            if (k >= m.texFiles.size()) {
                warnings.push_back("terrain materials: " + chunk_name(i, ch) + " texIndexArray[" + std::to_string(j) +
                                   "] = " + std::to_string(k) + " is past texFileArray (" +
                                   std::to_string(m.texFiles.size()) + " entries); chunk left unresolved");
                ok = false;
                continue;
            }
            const auto& tex = m.texFiles[k];
            const std::string name = token_name(tex.token);
            if (int s = slot_of(name, "color"); s >= 0) {
                mat.textureFileIds[(size_t)s] = tex.fileId;
                anyColour = anyColour || tex.fileId != 0;
            } else if (int n = slot_of(name, "normal"); n >= 0) {
                mat.normalFileIds[(size_t)n] = tex.fileId;
            } else if (name == "blend" || name == "modx") {
                // §4.2: a page reference: no filename, coord in chunks, the PIMG layer.
                const uint32_t layer = name == "blend" ? 0u : 1u;
                if (tex.layer != layer || tex.coord[0] != (uint32_t)ch.cx || tex.coord[1] != (uint32_t)ch.cy) {
                    warnings.push_back("terrain materials: " + chunk_name(i, ch) + " '" + name + "' names layer " +
                                       std::to_string(tex.layer) + " coord (" + std::to_string(tex.coord[0]) + ", " +
                                       std::to_string(tex.coord[1]) + "), not its own; page left out");
                    continue;
                }
                if (!perPage) continue;
                const uint32_t px = tex.coord[0] / (uint32_t)perPage, py = tex.coord[1] / (uint32_t)perPage;
                auto it = pages.find({layer, px, py});
                if (it == pages.end()) {
                    warnings.push_back("terrain materials: " + chunk_name(i, ch) + " has no layer " +
                                       std::to_string(layer) + " page (" + std::to_string(px) + ", " +
                                       std::to_string(py) + ")");
                    continue;
                }
                (layer == 0 ? mat.pickerFileId : mat.picker2FileId) = it->second->fileId;
                if (!it->second->fileId)
                    std::memcpy(layer == 0 ? mat.pickerSolid : mat.picker2Solid, it->second->solidColor, 4);
                // §4.3: page-image UV, u from the west column, v from the north (first) row.
                mat.pickerOffset[0] = (float)(tex.coord[0] % (uint32_t)perPage) / (float)perPage;
                mat.pickerOffset[1] = (float)(tex.coord[1] % (uint32_t)perPage) / (float)perPage;
            } else {
                dropped.insert(name.empty() ? std::to_string(tex.token) : name);
                ++droppedCount;
            }
        }
        if (ok && !anyColour)
            warnings.push_back("terrain materials: " + chunk_name(i, ch) +
                               " binds no colour texture; chunk left unresolved");
        mat.resolved = ok && anyColour;
        ch.material = std::move(mat);
    }
    if (droppedCount) {
        std::string names;
        for (const auto& n : dropped) names += (names.empty() ? "" : ", ") + n;
        warnings.push_back("terrain materials: " + std::to_string(droppedCount) +
                           " chunk texture bindings not kept (tokens: " + names + ")");
    }
}

} // namespace castlemist::world
