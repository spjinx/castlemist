/// @file
/// @brief Equirect and cube-face writers over a radiance callback.

#include "castlemist/exportgltf/sky_project.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <system_error>

// texture_export.cpp already defines STB_IMAGE_WRITE_IMPLEMENTATION for this layer.
#include "stb_image_write.h"

namespace castlemist::exportgltf::sky {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Viewer inside the cube, Unity axes. right = up x forward (left-handed), so
// every face's edges meet its neighbours' without a flip.
const FaceBasis kBasis[] = {
    /* PX */ {{+1, 0, 0}, {0, 0, -1}, {0, +1, 0}},
    /* NX */ {{-1, 0, 0}, {0, 0, +1}, {0, +1, 0}},
    /* PY */ {{0, +1, 0}, {+1, 0, 0}, {0, 0, -1}},
    /* NY */ {{0, -1, 0}, {+1, 0, 0}, {0, 0, +1}},
    /* PZ */ {{0, 0, +1}, {+1, 0, 0}, {0, +1, 0}},
    /* NZ */ {{0, 0, -1}, {-1, 0, 0}, {0, +1, 0}},
};

const char* const kFile[] = {"px", "nx", "py", "ny", "pz", "nz"};
const char* const kSlot[] = {"_LeftTex", "_RightTex", "_UpTex", "_DownTex", "_FrontTex", "_BackTex"};

uint8_t to_byte(float c) {
    return static_cast<uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
}

void put(Image& img, int x, int y, const Rgb& c) {
    uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
    p[0] = to_byte(c.r);
    p[1] = to_byte(c.g);
    p[2] = to_byte(c.b);
    p[3] = 255;
}

Image blank(int width, int height) {
    Image img;
    img.width = width;
    img.height = height;
    img.rgba.resize(static_cast<size_t>(width) * height * 4);
    return img;
}

} // namespace

const FaceBasis& face_basis(Face f) { return kBasis[static_cast<int>(f)]; }
const char* face_file(Face f) { return kFile[static_cast<int>(f)]; }
const char* face_unity_slot(Face f) { return kSlot[static_cast<int>(f)]; }

void face_texel_dir(Face f, int x, int y, int size, float out[3]) {
    const FaceBasis& b = face_basis(f);
    float a = 2.0f * (x + 0.5f) / size - 1.0f;
    float v = 1.0f - 2.0f * (y + 0.5f) / size;
    float len2 = 0;
    for (int i = 0; i < 3; ++i) {
        out[i] = b.forward[i] + a * b.right[i] + v * b.up[i];
        len2 += out[i] * out[i];
    }
    float inv = 1.0f / std::sqrt(len2);
    for (int i = 0; i < 3; ++i) out[i] *= inv;
}

Image render_face(const Radiance& r, Face f, int size) {
    Image img = blank(size, size);
    float d[3];
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            face_texel_dir(f, x, y, size, d);
            put(img, x, y, r(d));
        }
    return img;
}

Image render_equirect(const Radiance& r, int width, int height) {
    // Unity Skybox/Panoramic (latitude-longitude): row 0 looks at +Y, the
    // image centre at +X, and u = 0.75 at -Z.
    Image img = blank(width, height);
    for (int y = 0; y < height; ++y) {
        float latitude = (y + 0.5f) / height * kPi;
        float sl = std::sin(latitude), cl = std::cos(latitude);
        for (int x = 0; x < width; ++x) {
            float longitude = (0.5f - (x + 0.5f) / width) * 2.0f * kPi;
            float d[3] = {sl * std::cos(longitude), cl, sl * std::sin(longitude)};
            put(img, x, y, r(d));
        }
    }
    return img;
}

bool write_png(const Image& img, const std::string& path, std::string& error) {
    if (img.width <= 0 || img.height <= 0 ||
        img.rgba.size() < static_cast<size_t>(img.width) * img.height * 4) {
        error = "image has no pixels";
        return false;
    }
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            error = "cannot create " + parent.string() + ": " + ec.message();
            return false;
        }
    }
    if (!stbi_write_png(path.c_str(), img.width, img.height, 4, img.rgba.data(), img.width * 4)) {
        error = "failed to write PNG (bad path, or disk/permission error)";
        return false;
    }
    return true;
}

} // namespace castlemist::exportgltf::sky
