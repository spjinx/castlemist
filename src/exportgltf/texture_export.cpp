/// @file
/// @brief Decoded RGBA8888 textures -> PNG bytes in memory, embedded straight
///        into the .glb (no sibling texture folder -- a glTF binary container
///        is meant to be one self-contained file), unless the writer was
///        given an image folder (GltfExportOptions::imageDirUtf8, used by the
///        world export's model files): then they are PNG files beside it.

#include "internal.h"
#include "long_path.h"
#include "sha1.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

// Leave STBI_WRITE_NO_STDIO undefined: merely defining it (regardless of
// value) compiles out stbi_write_png_to_func too, which this needs.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace castlemist::exportgltf {

namespace {

void append_to_vector(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

// The image's name in the .glb: the texture's own fileId (as castlemist's
// texture panel lists it), or the material a bake made it for. Without one,
// Blender names embedded images Image_0, Image_1, ... and unpacks them so.
std::string texture_name(const ModelTextureCPU& tex, size_t i) {
    if (tex.fileId & 0x80000000u) return "baked_diffuse_mat" + std::to_string(tex.fileId & 0xFFFFu);
    if (tex.fileId & 0x40000000u) return "baked_normal_mat" + std::to_string(tex.fileId & 0xFFFFu);
    if (tex.fileId) return std::to_string(tex.fileId);
    return "texture_" + std::to_string(i);
}

float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

int bake_effect_emissive_texture(GltfWriter& w, const ModelTextureCPU& diffuse) {
    if (diffuse.width <= 0 || diffuse.height <= 0 ||
        diffuse.rgba.size() < static_cast<size_t>(diffuse.width) * diffuse.height * 4) {
        return -1;
    }

    // Same luminance-threshold glow split castlemist's own reconstruction
    // shader uses for "effect" materials (src/render/detail/shaders.h):
    // smoothstep(0.30, 0.80, luminance), colored by the diffuse pixel itself
    // rather than a flat white glow, so a blue magic effect glows blue.
    std::vector<uint8_t> emissive(diffuse.rgba.size());
    for (size_t i = 0; i + 3 < diffuse.rgba.size(); i += 4) {
        float r = diffuse.rgba[i] / 255.0f;
        float g = diffuse.rgba[i + 1] / 255.0f;
        float b = diffuse.rgba[i + 2] / 255.0f;
        float lum = 0.299f * r + 0.587f * g + 0.114f * b;
        float glow = smoothstep(0.30f, 0.80f, lum);
        emissive[i] = static_cast<uint8_t>(std::lround(r * glow * 255.0f));
        emissive[i + 1] = static_cast<uint8_t>(std::lround(g * glow * 255.0f));
        emissive[i + 2] = static_cast<uint8_t>(std::lround(b * glow * 255.0f));
        emissive[i + 3] = 255;
    }

    std::vector<uint8_t> png;
    int ok = stbi_write_png_to_func(append_to_vector, &png, diffuse.width, diffuse.height, 4, emissive.data(),
                                    diffuse.width * 4);
    if (!ok || png.empty()) return -1;

    // A distinct dedup key from the diffuse texture's own fileId, so the two
    // don't collide in GltfWriter's shared texture cache.
    uint32_t emissiveKey = diffuse.fileId ^ 0x454D4953u; // "EMIS"
    return w.add_or_reuse_texture(emissiveKey, png, std::to_string(diffuse.fileId) + "_emissive");
}

std::vector<uint8_t> encode_png(const ModelTextureCPU& tex) {
    std::vector<uint8_t> png;
    if (tex.width <= 0 || tex.height <= 0 ||
        tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) {
        return png;
    }
    if (!stbi_write_png_to_func(append_to_vector, &png, tex.width, tex.height, 4, tex.rgba.data(),
                                tex.width * 4))
        png.clear();
    return png;
}

std::vector<uint8_t> encode_png_pixels(int width, int height, int channels, const std::vector<uint8_t>& pixels) {
    std::vector<uint8_t> png;
    if (width <= 0 || height <= 0 || channels < 1 || channels > 4 ||
        pixels.size() < static_cast<size_t>(width) * height * channels)
        return png;
    if (!stbi_write_png_to_func(append_to_vector, &png, width, height, channels, pixels.data(), width * channels))
        png.clear();
    return png;
}

TextureSaveResult save_texture_png(const ModelTextureCPU& tex, const std::string& pngPath) {
    TextureSaveResult r;
    if (tex.width <= 0 || tex.height <= 0 ||
        tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) {
        r.error = "texture has no decoded pixels";
        return r;
    }
    if (!stbi_write_png(pngPath.c_str(), tex.width, tex.height, 4, tex.rgba.data(), tex.width * 4)) {
        r.error = "failed to write PNG (bad path, or disk/permission error)";
        return r;
    }
    r.ok = true;
    return r;
}

bool write_model_textures(GltfWriter& w, const std::vector<ModelTextureCPU>& textures,
                          std::vector<int>& texIndices, const std::string& imageDirUtf8,
                          const std::string& uriPrefix, SharedImagePool* shared, const std::string& sharedPrefix) {
    texIndices.assign(textures.size(), -1);
    if (!imageDirUtf8.empty()) w.set_image_output(imageDirUtf8, uriPrefix);

    for (size_t i = 0; i < textures.size(); ++i) {
        const ModelTextureCPU& tex = textures[i];
        if (tex.width <= 0 || tex.height <= 0 ||
            tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) {
            continue; // nothing decoded for this slot; material export leaves the slot unbound
        }

        if (shared && tex.fileId != 0) {   // the world export's shared pool (ruling W8)
            std::string err;
            if (!shared->ensure(tex.fileId, [&tex] { return encode_png(tex); }, &err)) {
                w.set_image_error(err);
                continue;
            }
            texIndices[i] = w.add_texture_uri(tex.fileId, sharedPrefix + SharedImagePool::file_name(tex.fileId),
                                              texture_name(tex, i));
            continue;
        }

        std::vector<uint8_t> png;
        int ok = stbi_write_png_to_func(append_to_vector, &png, tex.width, tex.height, 4, tex.rgba.data(),
                                        tex.width * 4);
        if (!ok || png.empty()) continue;

        texIndices[i] = w.add_or_reuse_texture(tex.fileId, png, texture_name(tex, i));
    }
    return w.image_error().empty();
}

// ------------------------------------------------------------ SharedImagePool

struct SharedImagePool::Impl {
    struct Slot { bool done = false, ok = false; std::string error; };
    std::string dir;
    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, Slot> slots;   // by lower-case file name (Windows names ignore case)
    std::map<std::string, std::string> spelling;   // lower-case -> the first caller's name
    size_t written = 0;
    uint64_t bytesRequested = 0, bytesWritten = 0;
};

SharedImagePool::SharedImagePool(std::string dirUtf8) : impl_(std::make_unique<Impl>()) {
    impl_->dir = std::move(dirUtf8);
}
SharedImagePool::~SharedImagePool() = default;

std::string SharedImagePool::file_name(uint32_t fileId) { return std::to_string(fileId) + ".png"; }
const std::string& SharedImagePool::dir() const { return impl_->dir; }
size_t SharedImagePool::written() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->written;
}
uint64_t SharedImagePool::bytes_requested() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->bytesRequested;
}
uint64_t SharedImagePool::bytes_written() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    return impl_->bytesWritten;
}

bool SharedImagePool::ensure(uint32_t fileId, const std::function<std::vector<uint8_t>()>& encodePng,
                             std::string* error) {
    return ensure_file(file_name(fileId), encodePng, error, nullptr);
}

std::string SharedImagePool::ensure_content(const std::vector<uint8_t>& png, const std::string& suffix,
                                            std::string* error, const std::string& ext) {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        impl_->bytesRequested += png.size();
    }
    if (png.empty()) {
        if (error) *error = "Could not encode a generated image.";
        return {};
    }
    std::string name = sha1_hex(png) + suffix + ext;
    {
        // One file per name ignoring case: a later spelling gets the first one's
        // (the export's own suffixes never differ only by case; this is a guard).
        std::lock_guard<std::mutex> lk(impl_->mu);
        std::string low = name;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        name = impl_->spelling.try_emplace(low, name).first->second;
    }
    uint64_t bytes = png.size();
    return ensure_file(name, [&png] { return png; }, error, &bytes) ? name : std::string();
}

bool SharedImagePool::ensure_file(const std::string& name, const std::function<std::vector<uint8_t>()>& encodePng,
                                  std::string* error, uint64_t* bytes) {
    namespace fs = std::filesystem;
    Impl& p = *impl_;
    {
        std::unique_lock<std::mutex> lk(p.mu);
        std::string low = name;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto [it, claimed] = p.slots.try_emplace(low);
        if (!claimed) {   // another caller owns it: wait until it is in place (or failed)
            p.cv.wait(lk, [&] { return it->second.done; });
            if (!it->second.ok && error) *error = it->second.error;
            return it->second.ok;
        }
    }
    // This caller claimed the name: encode and write outside the lock. The guard
    // publishes the slot however this scope is left -- an exception (say a
    // bad_alloc building a path) included -- so no waiter blocks forever.
    std::string err;
    bool published = false;
    struct Publish {
        Impl& p; const std::string& name; const std::string& err; bool& published; uint64_t bytes;
        ~Publish() {
            {
                std::lock_guard<std::mutex> lk(p.mu);
                std::string low = name;
                for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                Impl::Slot& s = p.slots[low];
                s.done = true;
                s.ok = published;
                s.error = published ? std::string()
                          : err.empty() ? "Could not write image " + name + ": interrupted."
                                        : err;
                if (published) { ++p.written; p.bytesWritten += bytes; }
            }
            p.cv.notify_all();
        }
    } guard{p, name, err, published, bytes ? *bytes : 0};
    const fs::path dir = fs::path(std::u8string(p.dir.begin(), p.dir.end()));
    const fs::path dst = dir / name, tmp = dir / (name + ".tmp");
    std::vector<uint8_t> png;
    try {
        png = encodePng();
    } catch (...) {
        png.clear();
    }
    if (png.empty()) {
        err = "Could not encode image " + name + ".";
    } else {
        std::error_code ec;
        make_dirs(dir, ec);
        {
            std::ofstream out(long_path(tmp), std::ios::binary);
            if (out) out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            out.close();
            if (!out) err = "Could not write image '" + p.dir + "/" + name + ".tmp'.";
        }
        if (err.empty()) {
            fs::rename(long_path(tmp), long_path(dst), ec);
            if (ec) err = "Could not rename image '" + p.dir + "/" + name + "': " + ec.message();
        }
        if (!err.empty()) fs::remove(long_path(tmp), ec);
    }
    published = err.empty();
    if (!published && error) *error = err;
    return published;   // the guard publishes the slot
}

} // namespace castlemist::exportgltf
