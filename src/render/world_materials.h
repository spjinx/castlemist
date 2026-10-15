/// @file
/// @brief WorldView's texture cache: one GPU texture per dat fileId, uploaded
///        from a worker-decoded CappedTexture.
///
/// @warning Not a public header -- internal to `src/render/`.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>

struct CappedTexture;   // castlemist/extract/model_types.h

namespace castlemist::render {

/// @brief fileId -> texture (spec "render/world_materials"). Each texture is
///        R8G8B8A8_TYPELESS with every mip of the CappedTexture and two views:
///        UNORM (normal maps, pages, masks) and UNORM_SRGB (colour textures).
///        A fileId is taken once: later adds of it, ready or failed, are
///        ignored. Lives on the device's thread.
class TextureCache {
public:
    enum class State { Unknown, Ready, Failed };

    /// @brief Uploads @p t on @p dev, once per fileId. Empty mips (the decode
    ///        failed), mip sizes that do not halve from width x height, or a
    ///        failed texture creation mark the fileId Failed. Nothing happens
    ///        with a null device or fileId 0.
    void add(ID3D11Device* dev, const CappedTexture& t);
    /// @brief The view of @p fileId (UNORM_SRGB when @p srgb), or null unless Ready.
    ID3D11ShaderResourceView* srv(uint32_t fileId, bool srgb) const;
    State state(uint32_t fileId) const;
    int ready() const { return ready_; }
    int failed() const { return failed_; }
    /// @brief GPU bytes of the Ready textures: the sum over their mips of w*h*4.
    size_t bytes() const { return bytes_; }
    /// @brief Releases every texture; all counts back to 0.
    void clear();

private:
    struct Entry {
        State state = State::Unknown;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> unorm, srgb;
    };
    std::unordered_map<uint32_t, Entry> map_;
    int ready_ = 0, failed_ = 0;
    size_t bytes_ = 0;
};

} // namespace castlemist::render
