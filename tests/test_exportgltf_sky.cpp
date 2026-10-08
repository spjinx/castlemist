/// @file
/// @brief Tests for the sky export: projection writers (face table, equirect,
///        PNG output).

#include "test_framework.h"

#include "castlemist/exportgltf/sky_project.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Static so this test's private decoder never clashes with the copy the format
// layer builds (that one has no stdio, so stbi_info on a path is not in it).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace castlemist::exportgltf::sky;

namespace {

constexpr Face kFaces[] = {Face::PX, Face::NX, Face::PY, Face::NY, Face::PZ, Face::NZ};

float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float angle_between(const float a[3], const float b[3]) {
    float d = dot3(a, b) / std::sqrt(dot3(a, a) * dot3(b, b));
    return std::acos(std::fmax(-1.0f, std::fmin(1.0f, d)));
}

/// Colour that encodes the direction itself, so a pixel decodes back to it.
Rgb encode_dir(const float d[3]) {
    return Rgb{(d[0] + 1) * 0.5f, (d[1] + 1) * 0.5f, (d[2] + 1) * 0.5f};
}

void decode_pixel(const Image& img, int x, int y, float out[3]) {
    const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    for (int i = 0; i < 3; ++i) out[i] = p[i] / 255.0f * 2.0f - 1.0f;
}

/// Texel direction at position @p i along one edge of a face.
enum class Edge { Left, Right, Top, Bottom };
void edge_dir(Face f, Edge e, int i, int size, float out[3]) {
    switch (e) {
    case Edge::Left:   face_texel_dir(f, 0, i, size, out); break;
    case Edge::Right:  face_texel_dir(f, size - 1, i, size, out); break;
    case Edge::Top:    face_texel_dir(f, i, 0, size, out); break;
    case Edge::Bottom: face_texel_dir(f, i, size - 1, size, out); break;
    }
}

} // namespace

CM_TEST(skyproject, face_centres_point_forward) {
    for (Face f : kFaces) {
        float d[3];
        face_texel_dir(f, 63, 63, 128, d);
        const FaceBasis& b = face_basis(f);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(d[i], b.forward[i], 0.01f);
    }
}

CM_TEST(skyproject, axis_marker_lands_on_its_face) {
    for (Face axis : kFaces) {
        const float* fwd = face_basis(axis).forward;
        Radiance r = [fwd](const float d[3]) {
            return dot3(d, fwd) > 0.99f ? Rgb{1, 0, 0} : Rgb{};
        };
        for (Face f : kFaces) {
            Image img = render_face(r, f, 64);
            CHECK_EQ(img.width, 64);
            CHECK_EQ(img.height, 64);
            int red = 0;
            for (size_t p = 0; p < img.rgba.size(); p += 4) red += img.rgba[p] > 0 ? 1 : 0;
            if (f == axis) {
                CHECK_EQ(int(img.rgba[(32 * 64 + 32) * 4]), 255);
                CHECK(red > 0);
            } else {
                CHECK_EQ(red, 0);
            }
        }
    }
}

CM_TEST(skyproject, neighbouring_face_edges_agree) {
    constexpr int kSize = 64;
    struct Seam { Face a; Edge ea; Face b; Edge eb; };
    const Seam seams[] = {
        {Face::PZ, Edge::Right,  Face::PX, Edge::Left},
        {Face::PZ, Edge::Top,    Face::PY, Edge::Bottom},
        {Face::PZ, Edge::Bottom, Face::NY, Edge::Top},
        {Face::PX, Edge::Right,  Face::NZ, Edge::Left},
        {Face::NZ, Edge::Right,  Face::NX, Edge::Left},
        {Face::NX, Edge::Right,  Face::PZ, Edge::Left},
    };
    Radiance r = encode_dir;
    for (const Seam& s : seams) {
        Image ia = render_face(r, s.a, kSize);
        Image ib = render_face(r, s.b, kSize);
        for (int i = 0; i < kSize; ++i) {
            float da[3], db[3];
            edge_dir(s.a, s.ea, i, kSize, da);
            edge_dir(s.b, s.eb, i, kSize, db);
            CHECK(angle_between(da, db) < 2.0f / kSize);
        }
        // The pixels agree too, not just the directions.
        float pa[3], pb[3];
        int ax = s.ea == Edge::Right ? kSize - 1 : s.ea == Edge::Left ? 0 : kSize / 2;
        int ay = s.ea == Edge::Bottom ? kSize - 1 : s.ea == Edge::Top ? 0 : kSize / 2;
        int bx = s.eb == Edge::Right ? kSize - 1 : s.eb == Edge::Left ? 0 : kSize / 2;
        int by = s.eb == Edge::Bottom ? kSize - 1 : s.eb == Edge::Top ? 0 : kSize / 2;
        decode_pixel(ia, ax, ay, pa);
        decode_pixel(ib, bx, by, pb);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(pa[i], pb[i], 0.05f);
    }
}

CM_TEST(skyproject, equirect_known_directions) {
    Image img = render_equirect(encode_dir, 512, 256);
    CHECK_EQ(img.width, 512);
    CHECK_EQ(img.height, 256);
    float d[3];
    decode_pixel(img, 256, 128, d);
    CHECK_NEAR(d[0], 1.0f, 0.02f); CHECK_NEAR(d[1], 0.0f, 0.02f); CHECK_NEAR(d[2], 0.0f, 0.02f);
    decode_pixel(img, 384, 128, d);
    CHECK_NEAR(d[0], 0.0f, 0.02f); CHECK_NEAR(d[1], 0.0f, 0.02f); CHECK_NEAR(d[2], -1.0f, 0.02f);
    decode_pixel(img, 128, 128, d);
    CHECK_NEAR(d[0], 0.0f, 0.02f); CHECK_NEAR(d[1], 0.0f, 0.02f); CHECK_NEAR(d[2], 1.0f, 0.02f);
    for (int x = 0; x < 512; x += 37) {
        decode_pixel(img, x, 0, d);
        CHECK_NEAR(d[1], 1.0f, 0.02f);
        decode_pixel(img, x, 255, d);
        CHECK_NEAR(d[1], -1.0f, 0.02f);
    }
}

CM_TEST(skyproject, png_round_trip) {
    Image img;
    img.width = 8;
    img.height = 4;
    img.rgba.assign(8 * 4 * 4, 200);
    auto dir = std::filesystem::temp_directory_path() / "cm_test_skyproject" / "nested";
    std::filesystem::remove_all(dir.parent_path());
    std::string path = (dir / "out.png").string();
    std::string error;
    CHECK(write_png(img, path, error));
    CHECK(error.empty());
    int w = 0, h = 0, comp = 0;
    CHECK(stbi_info(path.c_str(), &w, &h, &comp) != 0);
    CHECK_EQ(w, 8);
    CHECK_EQ(h, 4);
    CHECK_EQ(comp, 4);
    std::filesystem::remove_all(dir.parent_path());
}
