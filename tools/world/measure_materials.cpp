// SPDX-License-Identifier: MIT
//
// measure_materials -- the measurements behind docs/research/gw2-world-frame.md
// §4.1-§4.4 (terrain materials, the paged image and the orientation of a
// chunk's sub-rect in its page). Prints numbers only; nothing from the dat is
// written anywhere.
//
//   measure_materials <Gw2.dat> <template.json> <mapFileId>...
//
// Built with -DCASTLEMIST_WORLD_MEASURE=ON (tools/world/README.md). Per map it
// calls parseTerrain + build_terrain (the chunk grid), parseTerrainMaterials
// and, on the map's PIMG file, parsePagedImage, and reports:
//   1. the decoded token sequence of every chunk's loResMaterial.texIndexArray,
//      what kind of entry each token names (filename or page reference), and
//      how many indices are out of range;
//   2. for every page reference, whether coord == (i % chunksX, i / chunksX)
//      and whether layer is 0 for "blend", 1 for "modx";
//   3. whether hiResMaterial.texIndexArray == loResMaterial.texIndexArray, the
//      (hi, lo) materialFile pairs, the tiling bytes, non-null uvData;
//   4. the PIMG layers, the layer-0/1 page grid, every n with
//      ceil(chunks / n) = page grid, pages without a filename, and the
//      chunk-layer pairs whose page has no file (with the first n that fits);
//   5. the seam test: every page of layers 0 and 1 decoded (atex, mip 0,
//      RGBA8); for each pair of pages adjacent in coord, the mean absolute RGBA
//      difference between page A's last row (column) and page B's first
//      ("last->first"), and between A's first and B's last ("first->last");
//      as a baseline, between adjacent rows inside a page (every 7th row).

#include "castlemist/native/gw2_atex.hpp"
#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/dat_read.h"
#include "castlemist/world/terrain.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using Ex = castlemist::model::Extractor;

namespace {

/// GW2 Token decode (base 23 after 0x30000000), the rule of
/// src/extract/game_shader.cpp decode_token.
std::string tok(uint32_t t) {
    static const char* A = "abcdefghiklmnopvrstuwxy";
    uint32_t v = t - 0x30000000u;
    std::string o;
    while (v) {
        o.push_back(A[v % 23]);
        v /= 23;
    }
    return o;
}

std::vector<uint8_t> bytes(Gw2Dat& dat, uint32_t id) {
    auto b = castlemist::world::read_file_bytes(dat, id);
    return b ? std::move(*b) : std::vector<uint8_t>{};
}

struct Img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

Img decode(Gw2Dat& dat, uint32_t id) {
    Img im;
    auto d = bytes(dat, id);
    if (d.empty()) return im;
    try {
        auto t = castlemist::atex::parse(d.data(), d.size());
        auto i = castlemist::atex::decode(t, 0);
        im.w = i.width;
        im.h = i.height;
        im.px = i.rgba;
    } catch (const std::exception&) {
    }
    return im;
}

/// Mean absolute RGBA difference between row ra of a and row rb of b.
double rowDiff(const Img& a, int ra, const Img& b, int rb) {
    double s = 0;
    for (int x = 0; x < a.w; ++x)
        for (int c = 0; c < 4; ++c) s += std::fabs((double)a.px[(ra * a.w + x) * 4 + c] - b.px[(rb * b.w + x) * 4 + c]);
    return s / (a.w * 4);
}

/// Mean absolute RGBA difference between column ca of a and column cb of b.
double colDiff(const Img& a, int ca, const Img& b, int cb) {
    double s = 0;
    for (int y = 0; y < a.h; ++y)
        for (int c = 0; c < 4; ++c) s += std::fabs((double)a.px[(y * a.w + ca) * 4 + c] - b.px[(y * b.w + cb) * 4 + c]);
    return s / (a.h * 4);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: measure_materials <Gw2.dat> <template.json> <mapFileId>...\n");
        return 2;
    }
    nlohmann::json tpl;
    {
        std::ifstream tf(argv[2]);
        tf >> tpl;
    }
    Gw2Dat dat;
    load_dat_file(dat, argv[1]);
    for (int ai = 3; ai < argc; ++ai) {
        const uint32_t id = (uint32_t)std::stoul(argv[ai]);
        auto mb = bytes(dat, id);
        if (mb.empty()) {
            std::printf("== map %u: not in the dat\n", id);
            continue;
        }
        Ex ex(mb, tpl);
        std::vector<std::string> w;
        auto T = castlemist::world::build_terrain(ex.parseTerrain(), w);
        auto M = ex.parseTerrainMaterials();
        const int X = T.chunksX, Y = T.chunksY;
        std::printf("== map %u: chunks %dx%d, materials %zu, texFiles %zu, pimg %u\n", id, X, Y, M.chunks.size(),
                    M.texFiles.size(), M.pimgFileId);
        if (!X || !Y) continue;

        // 1-3: tokens, page references, hi/lo, tiling, uvData.
        std::map<std::string, int> seqs;
        std::map<size_t, int> lens;
        int oob = 0, coordOk = 0, coordBad = 0, layerOk = 0, layerBad = 0, hiEq = 0, uv = 0;
        std::map<std::pair<uint32_t, uint32_t>, int> matPairs;
        std::map<std::string, int> tilings;
        std::map<std::string, std::set<std::string>> tokKinds;   // token -> {"file", "page(...)"}
        for (size_t i = 0; i < M.chunks.size(); ++i) {
            const auto& c = M.chunks[i];
            const int cx = (int)(i % X), cy = (int)(i / X);
            lens[c.texIndices.size()]++;
            std::string s;
            for (uint32_t k : c.texIndices) {
                if (k >= M.texFiles.size()) {
                    ++oob;
                    s += "OOB,";
                    continue;
                }
                const auto& t = M.texFiles[k];
                const std::string tn = tok(t.token);
                s += tn + ",";
                tokKinds[tn].insert(t.fileId ? "file"
                                             : "page(flags=" + std::to_string(t.flags) +
                                                   ",layer=" + std::to_string(t.layer) + ")");
                if (!t.fileId) {
                    (t.coord[0] == (uint32_t)cx && t.coord[1] == (uint32_t)cy) ? ++coordOk : ++coordBad;
                    const bool lok = (tn == "blend" && t.layer == 0) || (tn == "modx" && t.layer == 1);
                    lok ? ++layerOk : ++layerBad;
                }
            }
            seqs[s]++;
            hiEq += c.hiTexIndices == c.texIndices;
            matPairs[{c.hiMaterialFileId, c.materialFileId}]++;
            std::string ti;
            for (int k = 0; k < c.tilingCount; ++k) ti += std::to_string(c.tiling[k]) + " ";
            tilings[ti]++;
            uv += c.hasUvData;
        }
        for (const auto& [k, v] : lens) std::printf("  texIndices length %zu: %d chunks\n", k, v);
        for (const auto& [k, v] : seqs) std::printf("  token sequence %s : %d\n", k.c_str(), v);
        for (const auto& [k, v] : tokKinds) {
            std::printf("  token %s ->", k.c_str());
            for (const auto& s : v) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        std::printf("  out-of-range indices %d; page-ref coord==(cx,cy) %d / %d; layer matches token %d / %d\n", oob,
                    coordOk, coordOk + coordBad, layerOk, layerOk + layerBad);
        std::printf("  hi texIndices == lo: %d / %zu; uvData non-null: %d\n", hiEq, M.chunks.size(), uv);
        for (const auto& [k, v] : matPairs) std::printf("  (hi, lo) materialFile (%u, %u): %d\n", k.first, k.second, v);
        for (const auto& [k, v] : tilings) std::printf("  tiling [%s]: %d\n", k.c_str(), v);

        // 4: the paged image.
        auto pb = bytes(dat, M.pimgFileId);
        if (pb.empty()) {
            std::printf("  pimg unreadable\n");
            continue;
        }
        Ex pex(pb, tpl);
        auto P = pex.parsePagedImage();
        for (size_t l = 0; l < P.layers.size(); ++l) {
            const uint32_t f = P.layers[l].strippedFormat;
            const char fc[5] = {(char)f, (char)(f >> 8), (char)(f >> 16), (char)(f >> 24), 0};
            std::printf("  layer %zu: strippedDims %ux%u format %s\n", l, P.layers[l].strippedDims[0],
                        P.layers[l].strippedDims[1], fc);
        }
        uint32_t mx[2] = {0, 0}, my[2] = {0, 0};
        int zeroFile = 0, nonzeroSolid = 0;
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> pages;
        for (const auto& p : P.strippedPages) {
            if (p.layer > 1) continue;
            mx[p.layer] = std::max(mx[p.layer], p.coord[0]);
            my[p.layer] = std::max(my[p.layer], p.coord[1]);
            pages[{p.layer, p.coord[0], p.coord[1]}] = p.fileId;
            zeroFile += p.fileId == 0;
            nonzeroSolid += (p.solidColor[0] | p.solidColor[1] | p.solidColor[2] | p.solidColor[3]) != 0;
        }
        std::printf("  stripped pages %zu; layer0 grid %ux%u, layer1 grid %ux%u; filename 0: %d; non-zero "
                    "solidColor: %d\n",
                    P.strippedPages.size(), mx[0] + 1, my[0] + 1, mx[1] + 1, my[1] + 1, zeroFile, nonzeroSolid);
        int perPage = 0;
        for (int n = 1; n <= 16; ++n)
            if ((X + n - 1) / n == (int)mx[0] + 1 && (Y + n - 1) / n == (int)my[0] + 1) {
                std::printf("  chunks per page side n = %d fits the grid\n", n);
                if (!perPage) perPage = n;
            }
        if (!perPage) continue;
        int missing = 0;
        for (int cy = 0; cy < Y; ++cy)
            for (int cx = 0; cx < X; ++cx)
                for (uint32_t l = 0; l < 2; ++l) {
                    auto it = pages.find({l, (uint32_t)(cx / perPage), (uint32_t)(cy / perPage)});
                    if (it == pages.end() || !it->second) ++missing;
                }
        std::printf("  chunk-layer pairs without a page file (n=%d): %d\n", perPage, missing);

        // 5: the seam test, per layer.
        for (uint32_t l = 0; l < 2; ++l) {
            std::map<std::pair<uint32_t, uint32_t>, Img> im;
            for (const auto& [k, f] : pages)
                if (std::get<0>(k) == l) im[{std::get<1>(k), std::get<2>(k)}] = decode(dat, f);
            double vA = 0, vB = 0, hA = 0, hB = 0, inner = 0;
            int nv = 0, nh = 0, ni = 0, w = 0, h = 0;
            for (const auto& [c, a] : im) {
                if (!a.w) continue;
                w = a.w;
                h = a.h;
                for (int r = 0; r + 1 < a.h; r += 7) {
                    inner += rowDiff(a, r, a, r + 1);
                    ++ni;
                }
                auto s = im.find({c.first, c.second + 1});
                if (s != im.end() && s->second.w == a.w && s->second.h == a.h) {
                    vA += rowDiff(a, a.h - 1, s->second, 0);            // rows north -> south
                    vB += rowDiff(a, 0, s->second, s->second.h - 1);    // rows south -> north
                    ++nv;
                }
                auto e = im.find({c.first + 1, c.second});
                if (e != im.end() && e->second.w == a.w && e->second.h == a.h) {
                    hA += colDiff(a, a.w - 1, e->second, 0);
                    hB += colDiff(a, 0, e->second, e->second.w - 1);
                    ++nh;
                }
            }
            std::printf("  layer %u (%dx%d px): mean |adjacent rows inside a page| %.2f (%d); vertical seams %d: "
                        "last->first %.2f, first->last %.2f; horizontal seams %d: last->first %.2f, first->last %.2f\n",
                        l, w, h, ni ? inner / ni : 0, ni, nv, nv ? vA / nv : 0, nv ? vB / nv : 0, nh,
                        nh ? hA / nh : 0, nh ? hB / nh : 0);
        }
    }
    return 0;
}
