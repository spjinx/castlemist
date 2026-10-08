/// @file
/// @brief Tests for the sky export: projection writers (face table, equirect,
///        PNG output), the sky sampler (docs/research/gw2-sky.md) and the
///        skybox export folder.

#include "test_framework.h"

#include "castlemist/exportgltf/sky_bake.h"
#include "castlemist/exportgltf/sky_export.h"
#include "castlemist/exportgltf/sky_project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

// ---------------------------------------------------------------------------
// Sky sampler (gw2-sky.md). Synthetic textures only, no dat.
// ---------------------------------------------------------------------------

namespace {

using MapSky = castlemist::model::Extractor::MapSky;
using MapSkyMode = castlemist::model::Extractor::MapSkyMode;

constexpr uint32_t kNE = 11, kSW = 12, kT = 13;
constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

enum class Tex { NE, SW, T };

/// One sky mode with a panorama (NE/SW/T = 11/12/13), brightness 1.
MapSky one_mode_sky() {
    MapSky sky;
    sky.present = true;
    MapSkyMode m;
    m.ne = kNE; m.sw = kSW; m.top = kT;
    sky.modes.push_back(m);
    sky.params.dayBrightness = 1.0f;
    sky.params.nightBrightness = 1.0f;
    return sky;
}

Image solid(int size, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    Image img;
    img.width = img.height = size;
    img.rgba.resize(static_cast<size_t>(size) * size * 4);
    for (size_t p = 0; p < img.rgba.size(); p += 4) {
        img.rgba[p] = r; img.rgba[p + 1] = g; img.rgba[p + 2] = b; img.rgba[p + 3] = a;
    }
    return img;
}

/// GW2 sky direction of texture coordinate (u, v), straight from the vertex
/// table in gw2-sky.md §2 (the forward mapping, written independently of the
/// sampler's inverse). Unnormalised; R = 1. Inset e = 1.4/W, the seamless
/// inset §2 measured (the game's own is 25/F0).
void forward_dir(Tex t, float u, float v, int width, float out[3]) {
    const float a = 1.4f / width, b = 1.0f - a;
    const float s = (u - a) / (b - a);            // 0..1 across the face
    if (t == Tex::T) {                            // face 4, z = -R
        out[0] = 2 * s - 1;
        out[1] = 2 * (v - a) / (b - a) - 1;
        out[2] = -1;
        return;
    }
    if (v < 0.5f) {                               // faces 1 (east) / 3 (west), upright
        const float tt = (v - a) / (0.5f - a);    // 0 at zenith edge, 1 at horizon
        out[2] = -(1 - tt);
        if (t == Tex::NE) { out[0] = 1;  out[1] = 1 - 2 * s; }
        else              { out[0] = -1; out[1] = 2 * s - 1; }
    } else {                                      // faces 0 (north) / 2 (south), rotated 180°
        const float tt = (v - 0.5f) / (b - 0.5f); // 0 at horizon, 1 at zenith edge
        out[2] = -tt;
        if (t == Tex::NE) { out[1] = 1;  out[0] = 1 - 2 * s; }
        else              { out[1] = -1; out[0] = 2 * s - 1; }
    }
}

void normalize3(float d[3]) {
    float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) d[i] /= l;
}

/// gw2-sky.md §1, written out here so the tests don't lean on the code under test.
void to_unity(const float gw2[3], float u[3]) { u[0] = gw2[0]; u[1] = -gw2[2]; u[2] = gw2[1]; }
void to_gw2(const float u[3], float g[3]) { g[0] = u[0]; g[1] = u[2]; g[2] = -u[1]; }

/// Unity direction at compass azimuth @p az (0 = north, 90 = east) and
/// elevation @p el above the horizon, both in degrees.
void unity_dir(float az, float el, float out[3]) {
    float a = az * kDegToRad, e = el * kDegToRad;
    out[0] = std::cos(e) * std::sin(a);
    out[1] = std::sin(e);
    out[2] = std::cos(e) * std::cos(a);
}

/// Texture whose every texel encodes (as colour) the GW2 direction §2 gives it.
Image direction_texture(Tex t, int size) {
    Image img = solid(size, 0, 0, 0, 255);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float d[3];
            forward_dir(t, (x + 0.5f) / size, (y + 0.5f) / size, size, d);
            normalize3(d);
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * size + x) * 4];
            for (int i = 0; i < 3; ++i)
                p[i] = static_cast<uint8_t>(std::lround((d[i] + 1) * 0.5f * 255));
        }
    return img;
}

bool has_warning(const BakeResult& r, const std::string& needle) {
    for (const std::string& w : r.warnings)
        if (w.find(needle) != std::string::npos && w.find("UNPROVEN") != std::string::npos) return true;
    return false;
}

float max_channel(const Rgb& c) { return std::max({c.r, c.g, c.b}); }

/// The note every baked mode carries about its seam cross-fade (gw2-sky.md §2).
bool is_seam_note(const std::string& w) { return w.rfind("seams cross-faded", 0) == 0; }

/// Warnings other than the seam note.
std::vector<std::string> other_warnings(const BakeResult& r) {
    std::vector<std::string> out;
    for (const std::string& w : r.warnings)
        if (!is_seam_note(w)) out.push_back(w);
    return out;
}

bool any_warning(const BakeResult& r, const std::string& a, const std::string& b = std::string()) {
    for (const std::string& w : r.warnings)
        if (w.find(a) != std::string::npos && (b.empty() || w.find(b) != std::string::npos)) return true;
    return false;
}

} // namespace

CM_TEST(skybake, gw2_to_unity_up_is_plus_y) {
    struct Case { float g[3]; float u[3]; };
    const Case cases[] = {
        {{0, 0, -1}, {0, 1, 0}},   // up
        {{0, 0, 1}, {0, -1, 0}},   // down
        {{0, 1, 0}, {0, 0, 1}},    // north
        {{1, 0, 0}, {1, 0, 0}},    // east
        {{0, -1, 0}, {0, 0, -1}},  // south
        {{-1, 0, 0}, {-1, 0, 0}},  // west
    };
    for (const Case& c : cases) {
        float u[3];
        gw2_to_unity(c.g, u);
        for (int i = 0; i < 3; ++i) CHECK_NEAR(u[i], c.u[i], 1e-6f);
    }
}

CM_TEST(skybake, missing_base_is_not_ok) {
    float up[3] = {0, 1, 0};
    {   // NE listed but not decoded
        MapSky sky = one_mode_sky();
        TextureMap tex;
        tex[kSW] = solid(8, 255, 255, 255, 255);
        tex[kT] = solid(8, 255, 255, 255, 255);
        BakeResult r = make_sky_sampler(sky, 0, tex);
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.warnings.empty());
        CHECK(std::find(r.layers.begin(), r.layers.end(), "base") == r.layers.end());
        CHECK(r.radiance != nullptr);
        CHECK_EQ(max_channel(r.radiance(up)), 0.0f);
    }
    {   // cube-only mode (map 3264516): no panorama at all
        MapSky sky;
        sky.present = true;
        MapSkyMode m;
        for (uint32_t i = 0; i < 6; ++i) m.cube[i] = 100 + i;
        sky.modes.push_back(m);
        BakeResult r = make_sky_sampler(sky, 0, TextureMap{});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.warnings.empty());
        CHECK(r.radiance != nullptr);
    }
    {   // mode index past the end
        MapSky sky = one_mode_sky();
        BakeResult r = make_sky_sampler(sky, 3, TextureMap{});
        CHECK_FALSE(r.ok);
        CHECK_FALSE(r.warnings.empty());
    }
    {   // all present -> ok, base baked
        MapSky sky = one_mode_sky();
        TextureMap tex;
        tex[kNE] = solid(8, 255, 255, 255, 255);
        tex[kSW] = solid(8, 255, 255, 255, 255);
        tex[kT] = solid(8, 255, 255, 255, 255);
        BakeResult r = make_sky_sampler(sky, 0, tex);
        CHECK(r.ok);
        CHECK(r.layers == std::vector<std::string>{"base"});
        // gw2-sky.md §2: the seam cross-fade is a choice, and says so.
        CHECK_EQ(r.warnings.size(), size_t(1));
        CHECK(any_warning(r, "seams cross-faded over 2 texels", "authored overlap as-is (gw2-sky.md §2)"));
        CHECK_NEAR(r.radiance(up).r, 1.0f, 1e-6f);
    }
}

CM_TEST(skybake, brightness_follows_mode) {
    // gw2-sky.md §5: radiance = clamp(tex.rgb * tex.a * Brightness, 0, 1);
    // Brightness = dayBrightness for modes 0/2, nightBrightness for 1/3.
    MapSky sky = one_mode_sky();
    sky.modes.resize(4, sky.modes[0]);
    sky.params.dayBrightness = 1.5f;
    sky.params.nightBrightness = 0.5f;
    TextureMap tex;
    tex[kNE] = solid(8, 128, 128, 128, 255);
    tex[kSW] = solid(8, 128, 128, 128, 255);
    tex[kT] = solid(8, 128, 128, 128, 255);
    const float grey = 128.0f / 255.0f;
    const float expected[4] = {grey * 1.5f, grey * 0.5f, grey * 1.5f, grey * 0.5f};
    float dirs[3][3] = {{0, 1, 0}, {0.6f, 0.3f, 0.74f}, {-0.2f, -0.9f, 0.3f}};
    for (auto& d : dirs) normalize3(d);
    for (size_t mode = 0; mode < 4; ++mode) {
        BakeResult r = make_sky_sampler(sky, mode, tex);
        CHECK(r.ok);
        for (auto& d : dirs) {
            Rgb c = r.radiance(d);
            CHECK_NEAR(c.r, expected[mode], 1e-4f);
            CHECK_NEAR(c.g, expected[mode], 1e-4f);
            CHECK_NEAR(c.b, expected[mode], 1e-4f);
        }
    }
    // Alpha premultiplies; the result is clamped to 1.
    tex[kNE] = solid(8, 128, 128, 128, 128);
    tex[kSW] = solid(8, 128, 128, 128, 128);
    tex[kT] = solid(8, 128, 128, 128, 128);
    float up[3] = {0, 1, 0};
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK_NEAR(r.radiance(up).r, grey * grey * 1.5f, 1e-4f);
    sky.params.dayBrightness = 4.0f;
    tex[kT] = solid(8, 255, 255, 255, 255);
    r = make_sky_sampler(sky, 0, tex);
    CHECK_EQ(r.radiance(up).r, 1.0f);
}

CM_TEST(skybake, panorama_seams_are_continuous) {
    // Every texel encodes the GW2 direction §2's vertex table gives it, so the
    // sampler must return (about) the encoded query direction everywhere above
    // the horizon, with no jump at the seams between the five hemicube faces,
    // and repeat the horizon row below the horizon.
    constexpr int kSize = 128;
    MapSky sky = one_mode_sky();
    TextureMap tex;
    tex[kNE] = direction_texture(Tex::NE, kSize);
    tex[kSW] = direction_texture(Tex::SW, kSize);
    tex[kT] = direction_texture(Tex::T, kSize);
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK(r.ok);
    float worst_step = 0, worst_err = 0, worst_below = 0;
    for (int az = 0; az < 360; ++az) {
        for (int el = -30; el <= 89; ++el) {
            float d[3], dn[3], de[3];
            unity_dir(float(az), float(el), d);
            unity_dir(float(az + 1), float(el), dn);
            unity_dir(float(az), float(el + 1), de);
            Rgb c = r.radiance(d), cn = r.radiance(dn), ce = r.radiance(de);
            worst_step = std::max({worst_step, std::fabs(c.r - cn.r), std::fabs(c.g - cn.g),
                                   std::fabs(c.b - cn.b), std::fabs(c.r - ce.r),
                                   std::fabs(c.g - ce.g), std::fabs(c.b - ce.b)});
            if (el >= 0) {
                float g[3];
                to_gw2(d, g);
                worst_err = std::max({worst_err, std::fabs(c.r - (g[0] + 1) * 0.5f),
                                      std::fabs(c.g - (g[1] + 1) * 0.5f),
                                      std::fabs(c.b - (g[2] + 1) * 0.5f)});
            } else {
                // §2: below the horizon the skirt repeats the horizon row.
                float h[3];
                unity_dir(float(az), 0.0f, h);
                Rgb ch = r.radiance(h);
                worst_below = std::max({worst_below, std::fabs(c.r - ch.r),
                                        std::fabs(c.g - ch.g), std::fabs(c.b - ch.b)});
            }
        }
    }
    CHECK(worst_step < 0.05f);
    CHECK(worst_err < 0.03f);
    CHECK(worst_below < 1e-6f);
}

namespace {

/// Like direction_texture, but the colour is 0.5 + 0.4 d + @p offset, so
/// neighbouring faces deliberately disagree by the offset difference.
Image offset_direction_texture(Tex t, int size, float offset) {
    Image img = solid(size, 0, 0, 0, 255);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            float d[3];
            forward_dir(t, (x + 0.5f) / size, (y + 0.5f) / size, size, d);
            normalize3(d);
            uint8_t* p = &img.rgba[(static_cast<size_t>(y) * size + x) * 4];
            for (int i = 0; i < 3; ++i)
                p[i] = static_cast<uint8_t>(std::lround(std::clamp(0.5f + 0.4f * d[i] + offset, 0.0f, 1.0f) * 255));
        }
    return img;
}

/// Equirect seam metric (as seam2.py): the largest colour step next to a seam
/// over the median step a few pixels either side. Vertical seams at Unity
/// azimuth 45/135/225/315 deg (elevation 5..40), top-cap seams at elevation 45
/// deg over the side-face centres (azimuth 0/90/180/270).
float worst_seam_ratio(const Image& eq) {
    const int W = eq.width, H = eq.height;
    auto px = [&](int x, int y) {
        x = ((x % W) + W) % W;
        return &eq.rgba[(static_cast<size_t>(y) * W + x) * 4];
    };
    auto diff = [](const uint8_t* a, const uint8_t* b) {
        return (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2])) / 3.0f;
    };
    auto col_x = [&](float azDeg) {
        const float phi = azDeg * kDegToRad;
        float u = (phi + 3.14159265f / 2) / (2 * 3.14159265f);
        u -= std::floor(u);
        return static_cast<int>(std::lround(u * W));
    };
    auto median = [](std::vector<float> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    float worst = 0;
    const int r0 = static_cast<int>((90 - 40) / 180.0f * H), r1 = static_cast<int>((90 - 5) / 180.0f * H);
    for (float az : {45.0f, 135.0f, 225.0f, 315.0f}) {
        const int c = col_x(az);
        auto step = [&](int k) {
            float s = 0;
            for (int y = r0; y < r1; ++y) s += diff(px(c + k, y), px(c + k - 1, y));
            return s / (r1 - r0);
        };
        std::vector<float> ref;
        for (int k = 4; k <= 24; ++k) { ref.push_back(step(k)); ref.push_back(step(-k)); }
        worst = std::max(worst, std::max({step(-1), step(0), step(1)}) / median(ref));
    }
    const int r = static_cast<int>(std::lround((90 - 45) / 180.0f * H));
    for (float az : {0.0f, 90.0f, 180.0f, 270.0f}) {
        const int c = col_x(az);
        auto step = [&](int k) {
            float s = 0;
            for (int x = c - 20; x <= c + 20; ++x) s += diff(px(x, r + k), px(x, r + k - 1));
            return s / 41;
        };
        std::vector<float> ref;
        for (int k = 4; k <= 24; ++k) { ref.push_back(step(k)); ref.push_back(step(-k)); }
        worst = std::max(worst, std::max({step(-1), step(0), step(1)}) / median(ref));
    }
    return worst;
}

} // namespace

CM_TEST(skybake, mismatched_face_edges_are_cross_faded) {
    // The textures' authored edges disagree (§2 seam table); the bake
    // cross-fades them over 2 texels either side of each face boundary.
    constexpr int kSize = 32;
    MapSky sky = one_mode_sky();
    TextureMap tex;
    tex[kNE] = offset_direction_texture(Tex::NE, kSize, 0.006f);
    tex[kSW] = offset_direction_texture(Tex::SW, kSize, -0.006f);
    tex[kT] = offset_direction_texture(Tex::T, kSize, 0.0f);
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK(r.ok);
    const float ratio = worst_seam_ratio(render_equirect(r.radiance, 512, 256));
    std::printf("      worst seam step / in-face step: %.2f\n", ratio);
    CHECK(ratio < 1.5f);
    CHECK(any_warning(r, "seams cross-faded over 2 texels", "gw2-sky.md §2"));
}

CM_TEST(skybake, panorama_texel_maps_to_its_direction) {
    // One bright texel at a time; the radiance at the direction §2 gives that
    // texel is bright, and everything more than 10 degrees away is dark.
    constexpr int kSize = 64;
    struct Probe { Tex t; int x, y; };
    const Probe probes[] = {
        {Tex::NE, 20, 10}, {Tex::NE, 55, 25},   // east (upper half)
        {Tex::NE, 40, 50},                      // north (lower half, rotated)
        {Tex::SW, 10, 20},                      // west (upper half)
        {Tex::SW, 50, 45}, {Tex::SW, 30, 40},   // south (lower half, rotated)
        {Tex::T, 32, 32}, {Tex::T, 5, 58},      // top cap
    };
    std::vector<std::array<float, 3>> sphere;   // Fibonacci sphere of test directions
    constexpr int kN = 4000;
    for (int i = 0; i < kN; ++i) {
        float y = 1 - 2 * (i + 0.5f) / kN, rr = std::sqrt(1 - y * y);
        float phi = i * 2.39996323f;
        sphere.push_back({rr * std::cos(phi), y, rr * std::sin(phi)});
    }
    for (const Probe& p : probes) {
        MapSky sky = one_mode_sky();
        TextureMap tex;
        tex[kNE] = solid(kSize, 0, 0, 0, 255);
        tex[kSW] = solid(kSize, 0, 0, 0, 255);
        tex[kT] = solid(kSize, 0, 0, 0, 255);
        uint32_t id = p.t == Tex::NE ? kNE : p.t == Tex::SW ? kSW : kT;
        uint8_t* px = &tex[id].rgba[(static_cast<size_t>(p.y) * kSize + p.x) * 4];
        px[0] = px[1] = px[2] = 255;
        BakeResult r = make_sky_sampler(sky, 0, tex);
        CHECK(r.ok);
        float g[3], u[3];
        forward_dir(p.t, (p.x + 0.5f) / kSize, (p.y + 0.5f) / kSize, kSize, g);
        normalize3(g);
        to_unity(g, u);
        CHECK(r.radiance(u).r > 0.9f);
        int stray = 0;
        for (const auto& s : sphere)
            if (angle_between(s.data(), u) > 10.0f * kDegToRad && max_channel(r.radiance(s.data())) > 0.01f)
                ++stray;
        CHECK_EQ(stray, 0);
    }
}

// ---- stars (gw2-sky.md §8) and texture sky cards (§9) ----

namespace {

using MapStars = castlemist::model::Extractor::MapStars;
using MapStar = castlemist::model::Extractor::MapStar;
using MapSkyCard = castlemist::model::Extractor::MapSkyCard;

constexpr uint32_t kStarFile = 500, kAtlas = 501, kCardDay = 600, kCardNight = 601;
constexpr float kPi = 3.14159265358979323846f;

/// One mode, black opaque hemicube, day and night star density 1, no haze.
MapSky black_sky(TextureMap& tex) {
    MapSky sky = one_mode_sky();
    sky.modes.resize(4, sky.modes[0]);
    tex[kNE] = solid(8, 0, 0, 0, 255);
    tex[kSW] = solid(8, 0, 0, 0, 255);
    tex[kT] = solid(8, 0, 0, 0, 255);
    return sky;
}

/// §8.3 star centre, GW2 space: (cos e1 cos e0, -cos e1 sin e0, -sin e1).
void star_centre(float e0, float e1, float g[3]) {
    g[0] = std::cos(e1) * std::cos(e0);
    g[1] = -std::cos(e1) * std::sin(e0);
    g[2] = -std::sin(e1);
}

/// §9.2 card centre, GW2 space: (cos lat cos az, cos lat sin az, -sin lat).
void card_centre(float az, float lat, float g[3]) {
    g[0] = std::cos(lat) * std::cos(az);
    g[1] = std::cos(lat) * std::sin(az);
    g[2] = -std::sin(lat);
}

/// Unity direction at tangent offsets (@p a, @p b) from GW2 centre @p c along
/// GW2 axes @p eL / @p eD: the gnomonic point c + a*eL + b*eD, normalised.
void offset_dir(const float c[3], const float eL[3], const float eD[3], float a, float b, float u[3]) {
    float g[3];
    for (int i = 0; i < 3; ++i) g[i] = c[i] + a * eL[i] + b * eD[i];
    normalize3(g);
    to_unity(g, u);
}

/// A star field of one star with atlas rect u0..u1, v0..v1.
MapStars one_star(float e0, float e1, float scale, float u0 = 0, float u1 = 0.25f, float v0 = 0,
                  float v1 = 0.25f) {
    MapStars st;
    st.present = true;
    st.scale = scale;
    st.atlas = kAtlas;
    st.stars.push_back(MapStar{e0, e1, u0, u1, v0, v1});
    return st;
}

/// Sky with one texture card (day and night attributes alike).
MapSky card_sky(TextureMap& tex, float az, float latStored, float scale) {
    MapSky sky = black_sky(tex);
    MapSkyCard c;
    c.day.texture = kCardDay;
    c.day.azimuth = az;
    c.day.latitude = latStored;
    c.day.scale[0] = c.day.scale[1] = scale;
    c.day.density = 1;
    c.day.brightness = 1;
    c.day.textureUV[0] = 0; c.day.textureUV[1] = 1; c.day.textureUV[2] = 1; c.day.textureUV[3] = 0;
    c.night = c.day;
    sky.cards.push_back(c);
    return sky;
}

} // namespace

CM_TEST(skybake, star_lights_its_direction) {
    TextureMap tex;
    MapSky sky = black_sky(tex);
    sky.starFile = kStarFile;
    sky.params.dayStarDensity = 1;
    tex[kAtlas] = solid(16, 128, 128, 128, 255);
    const float e0 = 0.7f, e1 = 0.4f;
    MapStars st = one_star(e0, e1, 0.125f);
    BakeResult r = make_sky_sampler(sky, 0, tex, &st);
    CHECK(r.ok);
    CHECK(r.layers == (std::vector<std::string>{"base", "stars"}));
    float g[3], u[3];
    star_centre(e0, e1, g);
    to_unity(g, u);
    // §8.4 with tw = 0: add = 2 T.rgb^2 * att * StarDensity, att = 1 (no haze).
    const float t = 128.0f / 255.0f;
    CHECK_NEAR(r.radiance(u).r, 2 * t * t, 1e-3f);
    // 2 degrees away, in elevation and in azimuth: nothing.
    star_centre(e0, e1 + 2 * kDegToRad, g);
    to_unity(g, u);
    CHECK_EQ(max_channel(r.radiance(u)), 0.0f);
    star_centre(e0 + 2 * kDegToRad / std::cos(e1), e1, g);
    to_unity(g, u);
    CHECK_EQ(max_channel(r.radiance(u)), 0.0f);
    // §8.4: the twinkle phase is not reproducible; the bake says it used tw = 0.
    CHECK(any_warning(r, "stars:", "tw"));
}

CM_TEST(skybake, star_size_scales_with_one_over_f0) {
    TextureMap tex;
    tex[kAtlas] = solid(16, 255, 255, 255, 255);
    const float e0 = -0.3f, e1 = 0.9f;
    float c[3], eL[3], eD[3];
    star_centre(e0, e1, c);
    eL[0] = std::sin(e0); eL[1] = std::cos(e0); eL[2] = 0;          // §8.3 eL
    eD[0] = std::sin(e1) * std::cos(e0); eD[1] = -std::sin(e1) * std::sin(e0); eD[2] = std::cos(e1);
    for (float f0 : {24576.0f, 49152.0f}) {
        MapSky sky = black_sky(tex);
        sky.skyDistance = f0;
        sky.starFile = kStarFile;
        sky.params.dayStarDensity = 1;
        MapStars st = one_star(e0, e1, 0.5f, 0.0f, 0.5f, 0.0f, 0.25f);
        BakeResult r = make_sky_sampler(sky, 0, tex, &st);
        const float R = 0.5f * f0;
        const float au = 2500 * 0.5f * 0.5f / R, av = 2500 * 0.5f * 0.25f / R;
        float u[3];
        for (float k : {0.95f, -0.95f}) {
            offset_dir(c, eL, eD, k * au, 0, u);
            CHECK(r.radiance(u).r > 0.5f);
            offset_dir(c, eL, eD, 0, k * av, u);
            CHECK(r.radiance(u).r > 0.5f);
        }
        for (float k : {1.05f, -1.05f}) {
            offset_dir(c, eL, eD, k * au, 0, u);
            CHECK_EQ(r.radiance(u).r, 0.0f);
            offset_dir(c, eL, eD, 0, k * av, u);
            CHECK_EQ(r.radiance(u).r, 0.0f);
        }
    }
}

CM_TEST(skybake, star_sprite_is_upright_and_unmirrored) {
    // §8.3: +eL (north at e0 = 0) is the u0 side, +eD (down) the v1 side.
    TextureMap tex;
    MapSky sky = black_sky(tex);
    sky.starFile = kStarFile;
    sky.params.dayStarDensity = 1;
    Image atlas = solid(16, 0, 0, 0, 255);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            uint8_t* p = &atlas.rgba[(static_cast<size_t>(y) * 16 + x) * 4];
            p[0] = x < 8 ? 255 : 0;   // red: left half (u < 0.5)
            p[1] = y < 8 ? 255 : 0;   // green: top half (v < 0.5)
        }
    tex[kAtlas] = atlas;
    MapStars st = one_star(0, 0, 1.0f, 0, 1, 0, 1);
    BakeResult r = make_sky_sampler(sky, 0, tex, &st);
    const float au = 2500.0f / (0.5f * sky.skyDistance);
    float c[3] = {1, 0, 0}, eL[3] = {0, 1, 0}, eD[3] = {0, 0, 1}, u[3];
    offset_dir(c, eL, eD, 0.6f * au, -0.6f * au, u);   // north and up: u0, v0
    Rgb nw = r.radiance(u);
    CHECK(nw.r > 0.5f);
    CHECK(nw.g > 0.5f);
    offset_dir(c, eL, eD, -0.6f * au, 0.6f * au, u);   // south and down: u1, v1
    Rgb se = r.radiance(u);
    CHECK_EQ(se.r, 0.0f);
    CHECK_EQ(se.g, 0.0f);
}

CM_TEST(skybake, stars_add_to_the_base_with_haze_attenuation) {
    TextureMap tex;
    MapSky sky = black_sky(tex);
    tex[kNE] = solid(8, 77, 77, 77, 255);
    tex[kSW] = solid(8, 77, 77, 77, 255);
    tex[kT] = solid(8, 77, 77, 77, 255);
    tex[kAtlas] = solid(16, 100, 100, 100, 255);
    sky.starFile = kStarFile;
    sky.params.nightStarDensity = 0.5f;
    const float e0 = 2.0f, e1 = 0.5f;
    MapStars st = one_star(e0, e1, 0.125f);
    float g[3], u[3];
    star_centre(e0, e1, g);
    to_unity(g, u);
    const float base = 77.0f / 255.0f, t = 100.0f / 255.0f, add = 2 * t * t;
    BakeResult r = make_sky_sampler(sky, 1, tex, &st);   // night: night* values
    CHECK_NEAR(r.radiance(u).r, base + add * 0.5f, 1e-3f);
    // §8.4: att = 1 - (1 - f^2 (3 - 2f)) HazeDensity, f = saturate((|d.z| - HazeBottom) / HazeFalloff).
    sky.params.nightHazeDensity = 1.0f;
    sky.params.nightHazeBottom = 0.0f;
    sky.params.nightHazeFalloff = 1.0f;
    r = make_sky_sampler(sky, 1, tex, &st);
    const float f = std::sin(e1), att = 1 - (1 - f * f * (3 - 2 * f));
    CHECK_NEAR(r.radiance(u).r, base + add * 0.5f * att, 1e-3f);
    // Day density 0: the game hides the mesh; no stars layer for day.
    BakeResult d = make_sky_sampler(sky, 0, tex, &st);
    CHECK(d.layers == std::vector<std::string>{"base"});
    CHECK_NEAR(d.radiance(u).r, base, 1e-4f);
}

CM_TEST(skybake, sky_card_sits_at_azimuth_latitude) {
    // §9.2: az radians (0 = east, toward north), latitude stored 0..1 x pi/2;
    // §9.3: half-angle atan(1000 scale / F), F = F0.
    TextureMap tex;
    const float az = 1.0f, latStored = 0.3f, scale = 2.0f;
    MapSky sky = card_sky(tex, az, latStored, scale);
    constexpr int kTex = 16;
    tex[kCardDay] = solid(kTex, 255, 255, 255, 255);
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK(r.ok);
    CHECK(r.layers == (std::vector<std::string>{"base", "cards"}));
    const float lat = latStored * kPi / 2;
    float c[3], u[3];
    card_centre(az, lat, c);
    to_unity(c, u);
    CHECK_NEAR(r.radiance(u).r, 1.0f, 1e-4f);
    // Not at the mirrored azimuth or the unscaled latitude.
    card_centre(-az, lat, c);
    to_unity(c, u);
    CHECK_EQ(r.radiance(u).r, 0.0f);
    card_centre(az, latStored, c);
    to_unity(c, u);
    CHECK_EQ(r.radiance(u).r, 0.0f);
    // Edge within half a texel of tan = 1000 scale / F0.
    card_centre(az, lat, c);
    const float eL[3] = {-std::sin(az), std::cos(az), 0};
    const float eD[3] = {std::sin(lat) * std::cos(az), std::sin(lat) * std::sin(az), std::cos(lat)};
    const float tu = 1000 * scale / sky.skyDistance, texel = 2 * tu / kTex;
    for (float s : {1.0f, -1.0f}) {
        offset_dir(c, eL, eD, s * (tu - 0.5f * texel), 0, u);
        CHECK_NEAR(r.radiance(u).r, 1.0f, 1e-4f);
        offset_dir(c, eL, eD, s * (tu + 0.5f * texel), 0, u);
        CHECK_EQ(r.radiance(u).r, 0.0f);
        offset_dir(c, eL, eD, 0, s * (tu - 0.5f * texel), u);
        CHECK_NEAR(r.radiance(u).r, 1.0f, 1e-4f);
        offset_dir(c, eL, eD, 0, s * (tu + 0.5f * texel), u);
        CHECK_EQ(r.radiance(u).r, 0.0f);
    }
    CHECK(any_warning(r, "sky cards:", "UNPROVEN"));
    CHECK(any_warning(r, "layer order", "UNPROVEN"));
}

CM_TEST(skybake, sky_card_texture_uv_crop_and_orientation) {
    // §9.3: textureUV = (uLeft, uRight, 1 - vTop, 1 - vBottom); (0,1,1,0) is the
    // whole texture upright and unmirrored (left = +eL, top = up).
    TextureMap tex;
    const float az = -2.0f, latStored = 0.5f;
    MapSky sky = card_sky(tex, az, latStored, 1.0f);
    Image quad = solid(16, 0, 0, 0, 255);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
            uint8_t* p = &quad.rgba[(static_cast<size_t>(y) * 16 + x) * 4];
            const bool right = x >= 8, bottom = y >= 8;
            p[0] = (!right && !bottom) ? 255 : 0;   // top-left red
            p[1] = (right && !bottom) ? 255 : 0;    // top-right green
            p[2] = (!right && bottom) ? 255 : 0;    // bottom-left blue
        }
    tex[kCardDay] = quad;
    const float lat = latStored * kPi / 2;
    float c[3], u[3];
    card_centre(az, lat, c);
    const float eL[3] = {-std::sin(az), std::cos(az), 0};
    const float eD[3] = {std::sin(lat) * std::cos(az), std::sin(lat) * std::sin(az), std::cos(lat)};
    const float tu = 1000.0f / sky.skyDistance;
    {
        BakeResult r = make_sky_sampler(sky, 0, tex);
        offset_dir(c, eL, eD, 0.5f * tu, -0.5f * tu, u);    // left, up
        Rgb tl = r.radiance(u);
        CHECK(tl.r > 0.9f && tl.g < 0.1f && tl.b < 0.1f);
        offset_dir(c, eL, eD, -0.5f * tu, -0.5f * tu, u);   // right, up
        Rgb tr = r.radiance(u);
        CHECK(tr.g > 0.9f && tr.r < 0.1f);
        offset_dir(c, eL, eD, 0.5f * tu, 0.5f * tu, u);     // left, down
        Rgb bl = r.radiance(u);
        CHECK(bl.b > 0.9f && bl.r < 0.1f);
    }
    // (0.5, 1, 1, 0.5): u 0.5..1, v 0..0.5 -> the top-right (green) quadrant fills the card.
    sky.cards[0].day.textureUV[0] = 0.5f;
    sky.cards[0].day.textureUV[1] = 1.0f;
    sky.cards[0].day.textureUV[2] = 1.0f;
    sky.cards[0].day.textureUV[3] = 0.5f;
    BakeResult r = make_sky_sampler(sky, 0, tex);
    for (float a : {0.8f, 0.0f, -0.8f})
        for (float b : {0.8f, 0.0f, -0.8f}) {
            offset_dir(c, eL, eD, a * tu, b * tu, u);
            Rgb p = r.radiance(u);
            CHECK(p.g > 0.9f && p.r < 0.1f && p.b < 0.1f);
        }
}

CM_TEST(skybake, sky_card_alpha_blends_over_the_base) {
    // §9.4/§9.5 reduced form: rgb = T.rgb * brightness, a = T.a * density,
    // dst = rgb a + dst (1 - a) (SRC_ALPHA, INV_SRC_ALPHA).
    TextureMap tex;
    const float az = 0.4f, latStored = 0.2f;
    MapSky sky = card_sky(tex, az, latStored, 1.0f);
    tex[kNE] = solid(8, 77, 77, 77, 255);
    tex[kSW] = solid(8, 77, 77, 77, 255);
    tex[kT] = solid(8, 77, 77, 77, 255);
    tex[kCardDay] = solid(8, 100, 0, 0, 128);
    sky.cards[0].day.density = 0.5f;
    sky.cards[0].day.brightness = 2.0f;
    BakeResult r = make_sky_sampler(sky, 0, tex);
    float c[3], u[3];
    card_centre(az, latStored * kPi / 2, c);
    to_unity(c, u);
    const float base = 77.0f / 255.0f, a = 128.0f / 255.0f * 0.5f, red = 100.0f / 255.0f * 2.0f;
    Rgb p = r.radiance(u);
    CHECK_NEAR(p.r, red * a + base * (1 - a), 1e-3f);
    CHECK_NEAR(p.g, base * (1 - a), 1e-3f);
}

CM_TEST(skybake, sky_card_day_and_night_attributes_follow_mode) {
    // §3: modes 0/2 use `day` (t = 1), modes 1/3 `night` (t = 0).
    TextureMap tex;
    MapSky sky = card_sky(tex, 0.0f, 0.2f, 1.0f);
    sky.cards[0].night.texture = kCardNight;
    sky.cards[0].night.azimuth = kPi;
    tex[kCardDay] = solid(8, 255, 0, 0, 255);
    tex[kCardNight] = solid(8, 0, 0, 255, 255);
    float dayDir[3], nightDir[3], g[3];
    card_centre(0.0f, 0.2f * kPi / 2, g);
    to_unity(g, dayDir);
    card_centre(kPi, 0.2f * kPi / 2, g);
    to_unity(g, nightDir);
    for (size_t mode = 0; mode < 4; ++mode) {
        BakeResult r = make_sky_sampler(sky, mode, tex);
        const bool day = mode % 2 == 0;
        Rgb atDay = r.radiance(dayDir), atNight = r.radiance(nightDir);
        CHECK_NEAR(atDay.r, day ? 1.0f : 0.0f, 1e-4f);
        CHECK_NEAR(atNight.b, day ? 0.0f : 1.0f, 1e-4f);
    }
}

CM_TEST(skybake, haze_dominated_cards_are_left_out_and_warned) {
    // §9.4: minHaze lifts the haze weight h toward FogColorFar (UNPROVEN); the
    // reduced form would draw such a card un-hazed, so it stays out.
    TextureMap tex;
    MapSky sky = card_sky(tex, 0.0f, 0.2f, 1.0f);
    tex[kCardDay] = solid(8, 255, 255, 255, 255);
    float g[3], u[3];
    card_centre(0.0f, 0.2f * kPi / 2, g);
    to_unity(g, u);
    sky.cards[0].day.minHaze = 0.73f;
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK(r.layers == std::vector<std::string>{"base"});
    CHECK(any_warning(r, "card 0", "minHaze > 0: haze-dominated, FogColorFar UNPROVEN (gw2-sky.md §9.4)"));
    CHECK_EQ(r.radiance(u).r, 0.0f);
    CHECK(r.cardTextures.empty());
    sky.cards[0].day.minHaze = 0.0f;
    r = make_sky_sampler(sky, 0, tex);
    CHECK(r.layers == (std::vector<std::string>{"base", "cards"}));
    CHECK_NEAR(r.radiance(u).r, 1.0f, 1e-4f);
    CHECK_FALSE(any_warning(r, "minHaze"));
}

CM_TEST(skybake, material_cards_are_left_out_and_warned) {
    TextureMap tex;
    MapSky sky = card_sky(tex, 0.0f, 0.2f, 1.0f);
    sky.cards[0].day.texture = 0;
    sky.cards[0].night.texture = 0;
    sky.cards[0].materialFile = 3135800;
    BakeResult r = make_sky_sampler(sky, 0, tex);
    CHECK(r.ok);
    CHECK(r.layers == std::vector<std::string>{"base"});
    CHECK(any_warning(r, "material", "UNPROVEN"));
    CHECK(any_warning(r, "3135800"));
}

CM_TEST(skybake, unproven_layers_are_warned_not_baked) {
    TextureMap tex;
    tex[kNE] = solid(8, 128, 128, 128, 255);
    tex[kSW] = solid(8, 128, 128, 128, 255);
    tex[kT] = solid(8, 128, 128, 128, 255);
    float d[3] = {0.3f, 0.5f, 0.81f};
    normalize3(d);

    MapSky plain = one_mode_sky();
    plain.modes.push_back(plain.modes[0]);
    Rgb base = make_sky_sampler(plain, 0, tex).radiance(d);

    // Clouds (§10.5), haze and sun glow (§5) stay out; a star file that was
    // not read and a card texture that did not decode are named.
    MapSky sky = plain;
    sky.starFile = 187544;
    sky.params.dayStarDensity = sky.params.nightStarDensity = 1;
    sky.clouds.resize(1);
    sky.clouds[0].texture = 77;
    sky.cards.resize(1);
    sky.cards[0].day.texture = 78;
    sky.cards[0].day.density = 1;
    sky.cards[0].night = sky.cards[0].day;
    sky.params.dayHazeDensity = 0.5f;
    sky.params.dayLightIntensity = 0.7f;
    sky.params.nightHazeDensity = 0.5f;
    sky.params.nightLightIntensity = 0.7f;
    for (size_t mode = 0; mode < 2; ++mode) {
        BakeResult r = make_sky_sampler(sky, mode, tex);
        CHECK(r.ok);
        CHECK(r.layers == std::vector<std::string>{"base"});
        CHECK(any_warning(r, "stars:", "187544"));
        CHECK(has_warning(r, "clouds"));
        CHECK(any_warning(r, "sky cards:", "78"));
        CHECK(has_warning(r, "haze"));
        CHECK(has_warning(r, "sun glow"));
        CHECK_EQ(other_warnings(r).size(), size_t(5));
        Rgb c = r.radiance(d);                  // left out: base colour alone
        CHECK_NEAR(c.r, base.r, 1e-6f);
        CHECK_NEAR(c.g, base.g, 1e-6f);
        CHECK_NEAR(c.b, base.b, 1e-6f);
    }
    // Haze is warned only for the mode whose parameter set (§3) has it.
    MapSky dayHaze = plain;
    dayHaze.params.dayHazeDensity = 0.5f;
    CHECK(has_warning(make_sky_sampler(dayHaze, 0, tex), "haze"));
    CHECK(other_warnings(make_sky_sampler(dayHaze, 1, tex)).empty());
    // A map without those layers gets no such warnings.
    CHECK(other_warnings(make_sky_sampler(plain, 0, tex)).empty());
}

// ---------------------------------------------------------------------------
// Skybox export, pure stage: hand-built SkyInputs, output under the temp dir.
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

/// A fresh, empty parent folder for one test.
fs::path export_parent(const char* test) {
    fs::path p = fs::temp_directory_path() / "cm_test_skyexport" / test;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

bool png_size(const fs::path& p, int& w, int& h) {
    int comp = 0;
    return stbi_info(p.string().c_str(), &w, &h, &comp) != 0;
}

nlohmann::json read_json(const fs::path& p) {
    std::ifstream f(p);
    return nlohmann::json::parse(f, nullptr, false);
}

bool any_contains(const nlohmann::json& arr, const std::string& needle) {
    if (!arr.is_array()) return false;
    for (const auto& w : arr)
        if (w.is_string() && w.get<std::string>().find(needle) != std::string::npos) return true;
    return false;
}

/// One panorama mode (NE/SW/T = 11/12/13), all three decoded (8x8).
SkyInputs panorama_inputs() {
    SkyInputs in;
    in.sky = one_mode_sky();
    in.sky.envVersion = 76;
    in.textures[kNE] = solid(8, 200, 100, 50, 255);
    in.textures[kSW] = solid(8, 50, 100, 200, 255);
    in.textures[kT] = solid(8, 255, 255, 255, 255);
    in.mapFileId = 999;
    return in;
}

constexpr uint32_t kCube = 100;   // cube faces 100..105, stored order E W N S B T

SkyInputs cube_inputs() {
    SkyInputs in;
    in.sky.present = true;
    MapSkyMode m;
    for (uint32_t i = 0; i < 6; ++i) {
        m.cube[i] = kCube + i;
        in.textures[kCube + i] = solid(8, static_cast<uint8_t>(40 * i), 0, 0, 255);
    }
    in.sky.modes.push_back(m);
    return in;
}

const char* const kFaceFiles[] = {"px", "nx", "py", "ny", "pz", "nz"};

} // namespace

CM_TEST(skyexport, mode_names) {
    CHECK_EQ(mode_name(0), std::string("day"));
    CHECK_EQ(mode_name(1), std::string("night"));
    CHECK_EQ(mode_name(2), std::string("mode2"));
    CHECK_EQ(mode_name(3), std::string("mode3"));
}

CM_TEST(skyexport, no_sky_writes_nothing) {
    fs::path parent = export_parent("no_sky");
    SkyInputs in;   // sky.present == false
    SkyExportReport r = write_skybox(in, parent.string(), "Sky");
    CHECK_FALSE(r.ok);
    CHECK_EQ(r.error, std::string("no sky"));
    CHECK_FALSE(fs::exists(parent / "Sky"));

    // present, but no mode has a whole panorama or cube
    in.sky.present = true;
    in.sky.modes.resize(2);
    in.sky.modes[0].ne = 5;
    r = write_skybox(in, parent.string(), "Sky");
    CHECK_FALSE(r.ok);
    CHECK_EQ(r.error, std::string("no sky"));
    CHECK_FALSE(fs::exists(parent / "Sky"));
    fs::remove_all(parent);
}

CM_TEST(skyexport, panorama_mode_writes_baked_files) {
    fs::path parent = export_parent("panorama");
    SkyExportOptions opt;
    opt.faceSize = 16;
    opt.equirectWidth = 32;
    SkyExportReport r = write_skybox(panorama_inputs(), parent.string(), "Sky", opt);
    CHECK(r.ok);
    CHECK_EQ(r.modesWritten, 1);
    CHECK_EQ(r.rawSkyboxes, 0);
    CHECK(fs::path(r.folder) == parent / "Sky");
    fs::path day = parent / "Sky" / "day";
    int w = 0, h = 0;
    CHECK(png_size(day / "baked" / "equirect.png", w, h));
    CHECK_EQ(w, 32);
    CHECK_EQ(h, 16);
    for (const char* f : kFaceFiles) {
        w = h = 0;
        CHECK(png_size(day / "baked" / (std::string(f) + ".png"), w, h));
        CHECK_EQ(w, 16);
        CHECK_EQ(h, 16);
    }
    CHECK_FALSE(fs::exists(day / "skybox"));

    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    CHECK(j.is_object());
    CHECK_EQ(j["map"].get<int>(), 999);
    CHECK_EQ(j["envVersion"].get<int>(), 76);
    CHECK_EQ(j["modes"].size(), size_t(1));
    const auto& m = j["modes"][0];
    CHECK_EQ(m["name"].get<std::string>(), std::string("day"));
    CHECK(m["aliasOf"].is_null());
    CHECK(m["baked"].get<bool>());
    CHECK_FALSE(m["skybox"].get<bool>());
    CHECK(m["layers"] == nlohmann::json::array({"base"}));
    CHECK_EQ(m["sources"]["ne"].get<int>(), int(kNE));
    CHECK_EQ(m["sources"]["sw"].get<int>(), int(kSW));
    CHECK_EQ(m["sources"]["top"].get<int>(), int(kT));
    CHECK(m["sources"]["cube"].is_null());
    CHECK_EQ(m["unitySlots"]["px"].get<std::string>(), std::string("_LeftTex"));
    CHECK(m["sun"].is_null());   // no env light in these inputs
    CHECK(j["warnings"].is_array());
    fs::remove_all(parent);
}

CM_TEST(skyexport, day_mode_carries_the_env_sun) {
    fs::path parent = export_parent("sun");
    SkyInputs in = panorama_inputs();
    in.sky.modes.push_back(in.sky.modes[0]);
    in.sky.modes[1].ne = kSW;   // night differs from day
    in.daySun.present = true;
    in.daySun.sunDir[0] = 0; in.daySun.sunDir[1] = 0; in.daySun.sunDir[2] = -1;   // GW2 up
    in.daySun.sunColor[0] = 1; in.daySun.sunColor[1] = 0.5f; in.daySun.sunColor[2] = 0.25f;
    in.daySun.sunIntensity = 2.0f;
    SkyExportReport r = write_skybox(in, parent.string(), "Sky", SkyExportOptions{8, 16});
    CHECK(r.ok);
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    const auto& sun = j["modes"][0]["sun"];
    CHECK(sun.is_object());
    CHECK_NEAR(sun["direction"][0].get<float>(), 0.0f, 1e-6f);
    CHECK_NEAR(sun["direction"][1].get<float>(), 1.0f, 1e-6f);   // Unity up
    CHECK_NEAR(sun["direction"][2].get<float>(), 0.0f, 1e-6f);
    CHECK_NEAR(sun["color"][1].get<float>(), 0.5f, 1e-6f);
    CHECK_NEAR(sun["intensity"].get<float>(), 2.0f, 1e-6f);
    CHECK(j["modes"][1]["sun"].is_null());
    fs::remove_all(parent);
}

CM_TEST(skyexport, cube_mode_writes_raw_faces) {
    fs::path parent = export_parent("cube");
    SkyExportReport r = write_skybox(cube_inputs(), parent.string(), "Sky", SkyExportOptions{16, 32});
    CHECK(r.ok);
    CHECK_EQ(r.rawSkyboxes, 1);
    CHECK_EQ(r.modesWritten, 1);
    fs::path day = parent / "Sky" / "day";
    for (const char* f : kFaceFiles) {
        int w = 0, h = 0;
        CHECK(png_size(day / "skybox" / (std::string(f) + ".png"), w, h));
        CHECK_EQ(w, 8);
        CHECK_EQ(h, 8);
    }
    // gw2-sky.md §6: E->px W->nx N->pz S->nz B->ny T->py, pixels untouched.
    const std::pair<const char*, int> expect[] = {
        {"px", 0}, {"nx", 1}, {"pz", 2}, {"nz", 3}, {"ny", 4}, {"py", 5}};
    for (const auto& [f, stored] : expect) {
        int w = 0, h = 0, comp = 0;
        std::string path = (day / "skybox" / (std::string(f) + ".png")).string();
        unsigned char* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
        CHECK(px != nullptr);
        if (!px) continue;
        CHECK_EQ(int(px[0]), 40 * stored);
        stbi_image_free(px);
    }
    CHECK_FALSE(fs::exists(day / "baked"));   // cube-only: nothing to bake
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    CHECK(j["modes"][0]["skybox"].get<bool>());
    CHECK_FALSE(j["modes"][0]["baked"].get<bool>());
    CHECK_EQ(j["modes"][0]["sources"]["cube"]["E"].get<int>(), int(kCube));
    CHECK_EQ(j["modes"][0]["sources"]["cube"]["T"].get<int>(), int(kCube + 5));
    CHECK_EQ(j["modes"][0]["warnings"].size(), size_t(1));   // one note, no bake flood
    fs::remove_all(parent);
}

CM_TEST(skyexport, duplicate_modes_alias) {
    fs::path parent = export_parent("alias");
    SkyInputs in = panorama_inputs();
    in.sky.modes.resize(4, in.sky.modes[0]);
    in.sky.modes[1].ne = kSW;             // night and mode2 differ from day
    in.sky.modes[2].sw = kNE;
    const SkyExportOptions opt{8, 16};
    SkyExportReport r = write_skybox(in, parent.string(), "Sky", opt);
    CHECK(r.ok);
    CHECK_EQ(r.modesWritten, 3);
    CHECK_FALSE(fs::exists(parent / "Sky" / "mode3"));
    CHECK(fs::exists(parent / "Sky" / "mode2" / "baked" / "equirect.png"));
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    CHECK_EQ(j["modes"].size(), size_t(4));
    CHECK_EQ(j["modes"][3]["aliasOf"].get<std::string>(), std::string("day"));
    CHECK(j["modes"][2]["aliasOf"].is_null());
    CHECK(any_contains(j["warnings"], "mode2/mode3"));

    // Same textures but a different Brightness (mode 3 uses night*): no alias.
    in.sky.params.nightBrightness = 0.5f;
    r = write_skybox(in, parent.string(), "Sky", opt);
    CHECK(r.ok);
    CHECK_EQ(r.modesWritten, 4);
    CHECK(fs::exists(parent / "Sky" / "mode3" / "baked" / "equirect.png"));
    j = read_json(parent / "Sky" / "sky.json");
    CHECK(j["modes"][3]["aliasOf"].is_null());
    fs::remove_all(parent);
}

CM_TEST(skyexport, sky_json_lists_baked_star_and_card_sources) {
    fs::path parent = export_parent("sources");
    SkyInputs in = panorama_inputs();
    in.sky.modes.push_back(in.sky.modes[0]);
    in.sky.skyDistance = 36864;
    in.sky.starFile = kStarFile;
    in.sky.params.nightStarDensity = 1;   // stars at night only
    in.stars = one_star(1.0f, 0.5f, 0.125f);
    in.textures[kAtlas] = solid(16, 255, 255, 255, 255);
    MapSkyCard c;
    c.day.texture = kCardDay;
    c.day.density = 1;
    c.day.brightness = 1;
    c.day.scale[0] = c.day.scale[1] = 1;
    c.night = c.day;
    c.night.texture = kCardNight;
    in.sky.cards.push_back(c);
    in.textures[kCardDay] = solid(8, 255, 0, 0, 255);
    in.textures[kCardNight] = solid(8, 0, 0, 255, 255);
    SkyExportReport r = write_skybox(in, parent.string(), "Sky", SkyExportOptions{8, 16});
    CHECK(r.ok);
    // Same hemicube and Brightness, but night has stars and its own card: no alias.
    CHECK_EQ(r.modesWritten, 2);
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    const auto& day = j["modes"][0];
    const auto& night = j["modes"][1];
    CHECK(night["aliasOf"].is_null());
    CHECK_EQ(day["skyDistance"].get<float>(), 36864.0f);
    CHECK(day["layers"] == nlohmann::json::array({"base", "cards"}));
    CHECK(night["layers"] == nlohmann::json::array({"base", "stars", "cards"}));
    CHECK(day["sources"]["stars"].is_null());
    CHECK_EQ(night["sources"]["stars"]["file"].get<int>(), int(kStarFile));
    CHECK_EQ(night["sources"]["stars"]["atlas"].get<int>(), int(kAtlas));
    CHECK(day["sources"]["cards"] == nlohmann::json::array({kCardDay}));
    CHECK(night["sources"]["cards"] == nlohmann::json::array({kCardNight}));
    CHECK(any_contains(day["warnings"], "seams cross-faded over 2 texels"));
    CHECK(any_contains(night["warnings"], "layer order"));
    fs::remove_all(parent);
}

CM_TEST(skyexport, missing_layer_is_warned) {
    fs::path parent = export_parent("missing");
    SkyInputs in = panorama_inputs();
    in.sky.modes.push_back(in.sky.modes[0]);
    in.sky.modes[0].ne = 4242;            // day's NE never decoded
    in.decodeWarnings.push_back("fileId 4242: not a decodable texture");
    SkyExportReport r = write_skybox(in, parent.string(), "Sky", SkyExportOptions{8, 16});
    CHECK(r.ok);
    CHECK_EQ(r.modesWritten, 1);
    CHECK_FALSE(fs::exists(parent / "Sky" / "day"));
    CHECK(fs::exists(parent / "Sky" / "night" / "baked" / "equirect.png"));
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    CHECK_FALSE(j["modes"][0]["baked"].get<bool>());
    CHECK(any_contains(j["modes"][0]["warnings"], "4242"));
    CHECK(j["modes"][1]["baked"].get<bool>());
    CHECK(any_contains(j["warnings"], "fileId 4242"));
    bool reported = false;
    for (const std::string& w : r.warnings)
        if (w.find("4242") != std::string::npos) reported = true;
    CHECK(reported);
    fs::remove_all(parent);
}

CM_TEST(skyexport, warnings_are_deduplicated) {
    fs::path parent = export_parent("dedup");
    SkyInputs in = panorama_inputs();
    in.sky.starFile = 187544;             // stars shown but not read: warned by every mode
    in.sky.params.dayStarDensity = in.sky.params.nightStarDensity = 1;
    in.sky.modes.push_back(in.sky.modes[0]);
    in.sky.modes[1].ne = kSW;
    in.decodeWarnings = {"fileId 187544: not a decodable texture",
                         "fileId 187544: not a decodable texture"};
    SkyExportReport r = write_skybox(in, parent.string(), "Sky", SkyExportOptions{8, 16});
    CHECK(r.ok);
    nlohmann::json j = read_json(parent / "Sky" / "sky.json");
    CHECK_EQ(j["warnings"].size(), size_t(1));
    CHECK(any_contains(j["modes"][0]["warnings"], "stars"));
    CHECK(any_contains(j["modes"][1]["warnings"], "stars"));
    std::vector<std::string> sorted = r.warnings;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    fs::remove_all(parent);
}

CM_TEST(skyexport, overwrites_existing_folder) {
    fs::path parent = export_parent("overwrite");
    fs::path stale = parent / "Sky" / "day" / "skybox" / "stale.png";
    fs::create_directories(stale.parent_path());
    { std::ofstream(stale) << "old"; }
    SkyExportReport r = write_skybox(panorama_inputs(), parent.string(), "Sky", SkyExportOptions{8, 16});
    CHECK(r.ok);
    CHECK_FALSE(fs::exists(stale));
    CHECK_FALSE(fs::exists(parent / "Sky" / "day" / "skybox"));
    CHECK(fs::exists(parent / "Sky" / "day" / "baked" / "equirect.png"));
    fs::remove_all(parent);
}

CM_TEST(skyexport, report_json_fields) {
    SkyExportReport r;
    r.ok = true;
    r.folder = "C:/x/Sky";
    r.modesWritten = 2;
    r.rawSkyboxes = 1;
    r.warnings = {"a"};
    nlohmann::json j = report_json(r);
    CHECK(j["ok"].get<bool>());
    CHECK_EQ(j["folder"].get<std::string>(), std::string("C:/x/Sky"));
    CHECK_EQ(j["modesWritten"].get<int>(), 2);
    CHECK_EQ(j["rawSkyboxes"].get<int>(), 1);
    CHECK_EQ(j["warnings"].size(), size_t(1));
    CHECK(j["error"].is_null());
    r.ok = false;
    r.error = "no sky";
    CHECK_EQ(report_json(r)["error"].get<std::string>(), std::string("no sky"));
}
