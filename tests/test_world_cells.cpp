/// @file
/// @brief Tests for cell partition and bucketing (world/cells.h).

#include "test_framework.h"

#include "castlemist/world/cells.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace castlemist::world;

namespace {

constexpr float kX0 = -15360.f, kY1 = 39936.f, kC = 3072.f;

WorldScene grid(int nx, int ny, float pickerScale = 0.25f) {
    WorldScene s;
    s.terrain.present = true;
    s.terrain.chunksX = nx; s.terrain.chunksY = ny;
    for (int cy = 0; cy < ny; ++cy)
        for (int cx = 0; cx < nx; ++cx) {
            TerrainChunk c;
            c.cx = cx; c.cy = cy; c.samples = 33;
            c.rect[0] = kX0 + cx * kC; c.rect[2] = kX0 + (cx + 1) * kC;
            c.rect[3] = kY1 - cy * kC; c.rect[1] = kY1 - (cy + 1) * kC;
            c.material.pickerScale = pickerScale;
            s.terrain.chunks.push_back(c);
        }
    return s;
}

// Prop at the centre of chunk (cx, cy).
void add_prop(WorldScene& s, int cx, int cy, uint32_t model = 0) {
    PropInstance p;
    p.model = model;
    p.world[0] = p.world[5] = p.world[10] = p.world[15] = 1;
    p.world[12] = kX0 + (cx + 0.5f) * kC;
    p.world[13] = kY1 - (cy + 0.5f) * kC;
    s.props.push_back(p);
}

void add_prop_at(WorldScene& s, float x, float y) {
    add_prop(s, 0, 0);
    s.props.back().world[12] = x; s.props.back().world[13] = y;
}

bool has_id(const CellPlan& p, const std::string& id) {
    return std::any_of(p.cells.begin(), p.cells.end(), [&](const Cell& c) { return c.id == id; });
}

const Cell* by_id(const CellPlan& p, const std::string& id) {
    for (const auto& c : p.cells) if (c.id == id) return &c;
    return nullptr;
}

size_t appearances(const CellPlan& p, uint32_t prop) {
    size_t n = 0;
    for (const auto& c : p.cells) n += size_t(std::count(c.props.begin(), c.props.end(), prop));
    return n;
}

} // namespace

CM_TEST(cells, block_size_from_pages) {
    CHECK_EQ(cell_block_chunks(grid(10, 25).terrain, 4), 4);
    CHECK_EQ(cell_block_chunks(grid(10, 25, 0.5f).terrain, 4), 2);
    CHECK_EQ(cell_block_chunks(grid(10, 25, 0.f).terrain, 4), 4);
}

CM_TEST(cells, edge_blocks) {
    const auto s = grid(10, 25);
    const auto p = partition_cells(s, {}, {});
    CHECK_EQ(p.cells.size(), size_t(21));
    CHECK(has_id(p, "c8_0_2x4"));
    CHECK(has_id(p, "c0_24_4x1"));
    CHECK(has_id(p, "c8_24_2x1"));
    CHECK(p.cells[0].id == "c0_0_4x4");
}

CM_TEST(cells, tiles_region_exactly) {
    const auto s = grid(10, 25);
    const auto p = partition_cells(s, {}, {});
    int total = 0;
    std::vector<int> seen(250, 0);
    for (const auto& c : p.cells) {
        total += c.w * c.h;
        CHECK_EQ(c.cx0 / 4, (c.cx0 + c.w - 1) / 4);
        CHECK_EQ(c.cy0 / 4, (c.cy0 + c.h - 1) / 4);
        for (int y = c.cy0; y < c.cy0 + c.h; ++y)
            for (int x = c.cx0; x < c.cx0 + c.w; ++x) ++seen[size_t(y) * 10 + x];
    }
    CHECK_EQ(total, 250);
    CHECK(std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; }));
}

CM_TEST(cells, heavy_block_splits_to_quadrants) {
    auto s = grid(4, 4);
    s.models.resize(1);
    add_prop(s, 0, 0); add_prop(s, 3, 3);
    CellStats st; st.modelTriangles = {600000};
    const auto p = partition_cells(s, st, {});
    CHECK_EQ(p.cells.size(), size_t(4));
    for (const char* id : {"c0_0_2x2", "c2_0_2x2", "c0_2_2x2", "c2_2_2x2"}) {
        const Cell* c = by_id(p, id);
        CHECK(c != nullptr);
        if (c) CHECK(c->split);
    }
    CHECK_EQ(by_id(p, "c0_0_2x2")->triangles, uint64_t(4 * 2048 + 600000));
}

CM_TEST(cells, over_budget_1x1_warns) {
    auto s = grid(4, 4);
    s.models.resize(1);
    for (int i = 0; i < 3; ++i) add_prop(s, 0, 0);
    CellStats st; st.modelTriangles = {600000};
    const auto p = partition_cells(s, st, {});
    const Cell* c = by_id(p, "c0_0_1x1");
    CHECK(c != nullptr);
    if (c) { CHECK(c->overBudget); CHECK_EQ(c->props.size(), size_t(3)); }
    int n = 0;
    for (const auto& w : p.warnings) if (w.find("c0_0_1x1") != std::string::npos) ++n;
    CHECK_EQ(n, 1);
    // Every prop lands exactly once even after the splits.
    for (uint32_t i = 0; i < 3; ++i) CHECK_EQ(appearances(p, i), size_t(1));
}

CM_TEST(cells, shared_edge_one_cell) {
    auto s = grid(10, 25);
    s.models.resize(1);
    add_prop_at(s, kX0 + 4 * kC, kY1 - 100.f);
    add_prop_at(s, -20000.f, kY1 - 100.f);
    const auto p = partition_cells(s, {}, {});
    CHECK_EQ(appearances(p, 0), size_t(1));
    const Cell* c = by_id(p, "c4_0_4x4");
    CHECK(c != nullptr);
    if (c) CHECK(std::count(c->props.begin(), c->props.end(), 0u) == 1);
    CHECK_EQ(appearances(p, 1), size_t(1));
    for (const auto& cell : p.cells)
        if (std::count(cell.props.begin(), cell.props.end(), 1u)) CHECK_EQ(cell.cx0, 0);
}

CM_TEST(cells, texture_budget_counts_distinct) {
    auto s = grid(4, 4);
    s.models.resize(1);
    add_prop(s, 0, 0); add_prop(s, 1, 1);
    CellStats st;
    st.modelTriangles = {10};
    st.modelTextures = {{7}};
    st.textureBytes[7] = 1u << 20;
    st.bakedBytesPerChunk = 1u << 20;
    const auto p = partition_cells(s, st, {});
    CHECK_EQ(p.cells.size(), size_t(1));
    CHECK_EQ(p.cells[0].textureMB, 1.0 + 16.0);
}

CM_TEST(cells, region_filters) {
    auto s = grid(10, 25);
    s.models.resize(1);
    add_prop(s, 5, 9);     // inside
    add_prop(s, 0, 0);     // outside
    add_prop(s, 9, 24);    // outside
    ChunkRegion r; r.cx0 = 4; r.cy0 = 8; r.cx1 = 7; r.cy1 = 11;
    const auto p = partition_cells(s, {}, {}, r);
    CHECK_EQ(p.cells.size(), size_t(1));
    CHECK(p.cells[0].id == "c4_8_4x4");
    CHECK_EQ(p.propsOutsideRegion, size_t(2));
    CHECK_EQ(p.cells[0].props.size(), size_t(1));
}

CM_TEST(cells, collision_bucketed_once) {
    auto s = grid(10, 25);
    for (int i = 0; i < 5; ++i) {
        CollisionInstance ci;
        ci.world[12] = -30000.f + 9000.f * float(i);   // some outside rect, clamped
        ci.world[13] = 50000.f - 12000.f * float(i);
        s.collision.instances.push_back(ci);
    }
    const auto p = partition_cells(s, {}, {});
    size_t n = 0;
    for (const auto& c : p.cells) n += c.collision.size();
    CHECK_EQ(n, size_t(5));
}

CM_TEST(cells, no_terrain_one_cell) {
    WorldScene s;
    s.models.resize(1);
    add_prop_at(s, 10.f, 20.f); add_prop_at(s, 30.f, 60.f);
    const auto p = partition_cells(s, {}, {});
    CHECK_EQ(p.cells.size(), size_t(1));
    CHECK(p.cells[0].id == "c_all");
    CHECK(p.cells[0].all);
    CHECK_EQ(p.cells[0].props.size(), size_t(2));
    CHECK(std::find(p.warnings.begin(), p.warnings.end(),
                       "cells: no terrain; one cell c_all holds every placement") != p.warnings.end());
}
