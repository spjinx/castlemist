// SPDX-License-Identifier: MIT
//
// measure_terrain -- the measurements behind docs/research/gw2-world-frame.md
// §3.1-§3.3 (which samples are a chunk's own, the chunk grid, the chunk
// placement). Prints numbers only; nothing from the dat is written anywhere.
//
//   measure_terrain <Gw2.dat> <template.json> <mapFileId>...
//
// Built with -DCASTLEMIST_WORLD_MEASURE=ON (tools/world/README.md). Per map it
// reads the map packfile with castlemist's own parser (Extractor::parseTerrain,
// Extractor::parseMapProps) and reports:
//   1. shared edges: for horizontal neighbours A = (cx, cy), B = (cx+1, cy),
//      how many A[row][seg+k] == B[row][k] (k = 0, 1, 2) over every stored row;
//      the same for vertical neighbours (A[seg+k][col] == B[k][col]) and
//      reversed (A[k][col] == B[seg+k][col]); every mismatching x pair; and,
//      as a control, x neighbours under column-major chunk order;
//   2. per placement hypothesis, over every prop parseMapProps returns (props
//      outside the rect skipped): |z - h| with h bilinear over that
//      hypothesis's grid -- median, p25, share under 16 and under 64 units;
//   3. for the proven layout, the share of props under 16 units when the props
//      are shifted by s samples along x or y (s = -64 .. 64; s = -32, -16, -1,
//      +1, +16, +32 printed, plus the best s), and how many props are more
//      than 500 units above / below the terrain under them.

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/gw2model.hpp"
#include "castlemist/world/dat_read.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using castlemist::model::Extractor;
using json = nlohmann::json;

namespace {

struct Grid {
    int cX, cY, seg, S;
    float r[4];
    const std::vector<float>* h;
    float at(int c, int row, int col) const { return (*h)[(size_t)c * S * S + (size_t)row * S + col]; }
};

// A placement hypothesis: which stored index a chunk's own samples start at,
// whether chunk rows (cy) and sample rows run north -> south, and an east-west mirror.
struct Hyp {
    const char* name;
    int off;
    bool northFirst;
    bool rowsNorthFirst;
    bool mirrorX;
};

bool height(const Grid& g, const Hyp& H, float x, float y, float& out) {
    float cdx = (g.r[2] - g.r[0]) / g.cX, cdy = (g.r[3] - g.r[1]) / g.cY;
    float lx = x - g.r[0];
    if (H.mirrorX) lx = (g.r[2] - g.r[0]) - lx;
    float ly = H.northFirst ? (g.r[3] - y) : (y - g.r[1]);   // distance along the chunk-index direction
    if (lx < 0 || ly < 0 || lx > g.r[2] - g.r[0] || ly > g.r[3] - g.r[1]) return false;
    int cx = std::min(g.cX - 1, (int)(lx / cdx)), cy = std::min(g.cY - 1, (int)(ly / cdy));
    float u = (lx - cx * cdx) / cdx * g.seg;   // column, from the chunk's first column
    float v = (ly - cy * cdy) / cdy * g.seg;   // row along the chunk direction
    if (H.rowsNorthFirst != H.northFirst) v = g.seg - v;
    int c0 = std::min(g.seg - 1, (int)u), r0 = std::min(g.seg - 1, (int)v);
    float tx = u - c0, ty = v - r0;
    int c = cy * g.cX + cx;
    auto S = [&](int r, int cc) { return g.at(c, r + H.off, cc + H.off); };
    float a = S(r0, c0) + (S(r0, c0 + 1) - S(r0, c0)) * tx;
    float b = S(r0 + 1, c0) + (S(r0 + 1, c0 + 1) - S(r0 + 1, c0)) * tx;
    out = a + (b - a) * ty;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: measure_terrain <Gw2.dat> <template.json> <mapFileId>...\n");
        return 2;
    }
    json tpl;
    {
        std::ifstream f(argv[2]);
        f >> tpl;
    }
    Gw2Dat dat;
    load_dat_file(dat, argv[1]);
    for (int a = 3; a < argc; ++a) {
        const uint32_t id = (uint32_t)std::stoul(argv[a]);
        auto bytes = castlemist::world::read_file_bytes(dat, id);
        if (!bytes) {
            std::cout << "=== map " << id << ": not in the dat\n";
            continue;
        }
        Extractor ex(*bytes, tpl);
        auto t = ex.parseTerrain();
        auto props = ex.parseMapProps();
        std::cout << "=== map " << id << "\n";
        std::cout << "dims " << t.dimX << " x " << t.dimY << "  verticesPerChunkSide " << t.vertsPerChunkSide
                  << "  heights " << t.heights.size() << "  rect " << t.rect[0] << " " << t.rect[1] << " "
                  << t.rect[2] << " " << t.rect[3] << " hasRect " << t.hasRect << "  props " << props.size() << "\n";
        // Solve count = dimX*dimY*(seg+3)^2/seg^2 for an integer seg dividing both dims.
        int seg = 0;
        for (uint32_t s = 1; s <= 512; ++s) {
            if (t.dimX % s || t.dimY % s) continue;
            size_t n = (size_t)(t.dimX / s) * (t.dimY / s) * (s + 3) * (s + 3);
            if (n == t.heights.size()) {
                std::cout << "  solves with seg " << s << "\n";
                if (!seg) seg = (int)s;
            }
        }
        if (!seg) {
            std::cout << "  no seg solves\n";
            continue;
        }
        Grid g{(int)t.dimX / seg, (int)t.dimY / seg, seg, seg + 3, {t.rect[0], t.rect[1], t.rect[2], t.rect[3]},
               &t.heights};
        double t3dChunks = std::sqrt((double)t.dimX * (g.cX * g.cY) / t.dimY);
        std::cout << "  chunks " << g.cX << " x " << g.cY << " (T3D sqrt rule " << t3dChunks << ")  stored " << g.S
                  << "\n";
        for (int k = 0; k < 3; ++k) {
            size_t eq = 0, n = 0;
            double mx = 0;
            for (int cy = 0; cy < g.cY; ++cy)
                for (int cx = 0; cx + 1 < g.cX; ++cx)
                    for (int r = 0; r < g.S; ++r) {
                        float A = g.at(cy * g.cX + cx, r, seg + k), B = g.at(cy * g.cX + cx + 1, r, k);
                        ++n;
                        eq += (A == B);
                        mx = std::max(mx, (double)std::fabs(A - B));
                    }
            std::cout << "  x-neigh A[.][seg+" << k << "]==B[.][" << k << "]: " << eq << "/" << n << " max|d| " << mx
                      << "\n";
        }
        for (int k = 0; k < 3; ++k) {
            size_t eq = 0, n = 0, eq2 = 0;
            double mx = 0, mx2 = 0;
            for (int cy = 0; cy + 1 < g.cY; ++cy)
                for (int cx = 0; cx < g.cX; ++cx)
                    for (int c = 0; c < g.S; ++c) {
                        float A = g.at(cy * g.cX + cx, seg + k, c), B = g.at((cy + 1) * g.cX + cx, k, c);
                        ++n;
                        eq += (A == B);
                        mx = std::max(mx, (double)std::fabs(A - B));
                        float A2 = g.at(cy * g.cX + cx, k, c), B2 = g.at((cy + 1) * g.cX + cx, seg + k, c);
                        eq2 += (A2 == B2);
                        mx2 = std::max(mx2, (double)std::fabs(A2 - B2));
                    }
            std::cout << "  y-neigh A[seg+" << k << "][.]==B[" << k << "][.]: " << eq << "/" << n << " max|d| " << mx
                      << "   reversed A[" << k << "]==B[seg+" << k << "]: " << eq2 << "/" << n << " max|d| " << mx2
                      << "\n";
        }
        for (int cy = 0; cy < g.cY; ++cy)
            for (int cx = 0; cx + 1 < g.cX; ++cx)
                for (int r = 0; r < g.S; ++r)
                    for (int k = 0; k < 3; ++k) {
                        float A = g.at(cy * g.cX + cx, r, seg + k), B = g.at(cy * g.cX + cx + 1, r, k);
                        if (A != B)
                            std::printf("  mismatch x-neigh chunk (%d,%d)|(%d,%d) row %d k %d: %.4f vs %.4f\n", cx, cy,
                                        cx + 1, cy, r, k, A, B);
                    }
        {
            size_t eq = 0, n = 0;
            for (int cy = 0; cy < g.cY; ++cy)
                for (int cx = 0; cx + 1 < g.cX; ++cx)
                    for (int r = 0; r < g.S; ++r) {
                        float A = g.at(cx * g.cY + cy, r, seg + 1), B = g.at((cx + 1) * g.cY + cy, r, 1);
                        ++n;
                        eq += (A == B);
                    }
            std::cout << "  control: column-major chunk order x-neigh equal " << eq << "/" << n << "\n";
        }
        const Hyp hyps[] = {
            {"T3D: inner ring, chunks+rows north-first", 1, true, true, false},
            {"offset 0 (castlemist old), north-first", 0, true, true, false},
            {"offset 2, north-first", 2, true, true, false},
            {"south-first (chunks+rows), inner", 1, false, false, false},
            {"chunks north, rows south, inner", 1, true, false, false},
            {"chunks south, rows north, inner", 1, false, true, false},
            {"mirror X, north-first, inner", 1, true, true, true},
            {"old map_scene.cpp: south-first, offset 0", 0, false, false, false},
        };
        for (const auto& H : hyps) {
            std::vector<double> ad;
            size_t within16 = 0, within64 = 0;
            for (const auto& p : props) {
                float h;
                if (!height(g, H, p.pos[0], p.pos[1], h)) continue;
                double d = std::fabs(p.pos[2] - h);
                ad.push_back(d);
                within16 += d < 16;
                within64 += d < 64;
            }
            if (&H == &hyps[0]) {
                // Shift scan: the proven placement translated by s samples; does s = 0 win?
                float cdy = (g.r[3] - g.r[1]) / g.cY, cdx = (g.r[2] - g.r[0]) / g.cX;
                for (int axis = 0; axis < 2; ++axis) {
                    int best = 0;
                    double bestf = -1, f0 = 0;
                    std::printf("  shift scan %s (step = 1 sample):", axis ? "y" : "x");
                    for (int s = -64; s <= 64; ++s) {
                        float step = (axis ? cdy : cdx) / g.seg;
                        size_t w = 0, n = 0;
                        for (const auto& p : props) {
                            float h;
                            float x = p.pos[0] - (axis ? 0 : s * step), y = p.pos[1] - (axis ? s * step : 0);
                            if (!height(g, H, x, y, h)) continue;
                            ++n;
                            w += std::fabs(p.pos[2] - h) < 16;
                        }
                        double f = n ? 100.0 * w / n : 0;
                        if (s == 0) f0 = f;
                        if (s == -32 || s == -16 || s == 16 || s == 32 || s == -1 || s == 1)
                            std::printf(" [%+d:%.1f%%]", s, f);
                        if (f > bestf) {
                            bestf = f;
                            best = s;
                        }
                    }
                    std::printf("  best %+d samples (%.1f%%), at 0: %.1f%%\n", best, bestf, f0);
                }
                size_t above = 0, below = 0;
                for (const auto& p : props) {
                    float h;
                    if (!height(g, H, p.pos[0], p.pos[1], h)) continue;
                    double d = p.pos[2] - h;
                    if (d < -500) ++above;
                    else if (d > 500) ++below;
                }
                std::printf("  props >500 above terrain (z-h<-500): %zu, >500 below: %zu\n", above, below);
            }
            if (ad.empty()) continue;
            std::sort(ad.begin(), ad.end());
            std::printf("  %-44s n %zu  median|z-h| %8.1f  p25 %7.1f  <16: %5.1f%%  <64: %5.1f%%\n", H.name, ad.size(),
                        ad[ad.size() / 2], ad[ad.size() / 4], 100.0 * within16 / ad.size(),
                        100.0 * within64 / ad.size());
        }
    }
    return 0;
}
