/// @file
/// @brief Decoded RGBA8888 textures -> PNG bytes in memory, embedded straight
///        into the .glb (no sibling texture folder -- a glTF binary container
///        is meant to be one self-contained file).

#include "internal.h"

#include <algorithm>
#include <cmath>

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
    return w.add_or_reuse_texture(emissiveKey, png);
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
                          std::vector<int>& texIndices) {
    texIndices.assign(textures.size(), -1);

    for (size_t i = 0; i < textures.size(); ++i) {
        const ModelTextureCPU& tex = textures[i];
        if (tex.width <= 0 || tex.height <= 0 ||
            tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) {
            continue; // nothing decoded for this slot; material export leaves the slot unbound
        }

        std::vector<uint8_t> png;
        int ok = stbi_write_png_to_func(append_to_vector, &png, tex.width, tex.height, 4, tex.rgba.data(),
                                        tex.width * 4);
        if (!ok || png.empty()) continue;

        texIndices[i] = w.add_or_reuse_texture(tex.fileId, png);
    }
    return true;
}

} // namespace castlemist::exportgltf
