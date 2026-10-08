/// @file
/// @brief Tests for the sky export: projection writers (face table, equirect,
///        PNG output) and the sky sampler (docs/research/gw2-sky.md).

#include "test_framework.h"

#include "castlemist/exportgltf/sky_bake.h"
#include "castlemist/exportgltf/sky_project.h"

#include <algorithm>
#include <array>
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
/// sampler's inverse). Unnormalised; R = 1. Inset e = 1/W.
void forward_dir(Tex t, float u, float v, int width, float out[3]) {
    const float a = 1.0f / width, b = 1.0f - a;
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
        CHECK(r.warnings.empty());
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

CM_TEST(skybake, sky_card_sits_at_azimuth_latitude) {
    SKIP("sky cards UNPROVEN in gw2-sky.md §4");
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

    // A map with every layer gw2-sky.md §4 leaves out.
    MapSky sky = plain;
    sky.starFile = 187544;
    sky.clouds.resize(1);
    sky.clouds[0].texture = 77;
    sky.cards.resize(1);
    sky.cards[0].day.texture = 78;
    sky.params.dayHazeDensity = 0.5f;
    sky.params.dayLightIntensity = 0.7f;
    sky.params.nightHazeDensity = 0.5f;
    sky.params.nightLightIntensity = 0.7f;
    for (size_t mode = 0; mode < 2; ++mode) {
        BakeResult r = make_sky_sampler(sky, mode, tex);
        CHECK(r.ok);
        CHECK(r.layers == std::vector<std::string>{"base"});
        CHECK(has_warning(r, "stars"));
        CHECK(has_warning(r, "clouds"));
        CHECK(has_warning(r, "sky cards"));
        CHECK(has_warning(r, "haze"));
        CHECK(has_warning(r, "sun glow"));
        CHECK_EQ(r.warnings.size(), size_t(5));
        Rgb c = r.radiance(d);                  // left out: base colour alone
        CHECK_NEAR(c.r, base.r, 1e-6f);
        CHECK_NEAR(c.g, base.g, 1e-6f);
        CHECK_NEAR(c.b, base.b, 1e-6f);
    }
    // Haze is warned only for the mode whose parameter set (§3) has it.
    MapSky dayHaze = plain;
    dayHaze.params.dayHazeDensity = 0.5f;
    CHECK(has_warning(make_sky_sampler(dayHaze, 0, tex), "haze"));
    CHECK(make_sky_sampler(dayHaze, 1, tex).warnings.empty());
    // A map without those layers gets no such warnings.
    CHECK(make_sky_sampler(plain, 0, tex).warnings.empty());
}
