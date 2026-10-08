/// @file
/// @brief Sky projection writers: turn any radiance callback into a Unity
///        `Skybox/Panoramic` equirect or the six `Skybox/6 Sided` faces.
/// @ingroup exportgltf
///
/// All directions are in Unity space (left-handed, +Y up, +Z forward).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace castlemist::exportgltf::sky {

/// Linear-ish colour, 0..1 after tonemapping; clamped when written.
struct Rgb { float r = 0, g = 0, b = 0; };

/// Sky colour seen along @p dir (normalized).
using Radiance = std::function<Rgb(const float dir[3])>;

/// RGBA8 image, row 0 = top.
struct Image { int width = 0, height = 0; std::vector<uint8_t> rgba; };

enum class Face { PX, NX, PY, NY, PZ, NZ };

/// A face as seen from inside the cube: where it looks, and which way the
/// image's right and up edges point.
struct FaceBasis { float forward[3], right[3], up[3]; };

const FaceBasis& face_basis(Face f);
/// File stem, "px".."nz".
const char* face_file(Face f);
/// Unity `Skybox/6 Sided` material slot, e.g. "_LeftTex".
const char* face_unity_slot(Face f);

/// Latitude-longitude image as Unity's `Skybox/Panoramic` samples it.
Image render_equirect(const Radiance& r, int width, int height);
/// One size x size cube face.
Image render_face(const Radiance& r, Face f, int size);
/// Direction through the centre of texel (@p x, @p y) of face @p f.
void face_texel_dir(Face f, int x, int y, int size, float out[3]);

/// Write @p img as an 8-bit RGBA PNG, creating parent directories.
bool write_png(const Image& img, const std::string& path, std::string& error);

} // namespace castlemist::exportgltf::sky
