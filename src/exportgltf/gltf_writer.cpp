/// @file
/// @brief GltfWriter: JSON scene graph (via nlohmann::json) + one flat binary
///        buffer, assembled into a binary glTF (.glb) container.
///
/// .glb layout (glTF 2.0 spec, "Binary glTF Layout" -- unlike FBX's binary
/// format this one is an official, precisely documented Khronos standard):
///   12-byte header: magic 0x46546C67 ("glTF"), version u32=2, total length u32.
///   JSON chunk: length u32, type 0x4E4F534A ("JSON"), then the JSON text
///     itself, space-padded (0x20) to a multiple of 4 bytes.
///   BIN chunk (optional): length u32, type 0x004E4942 ("BIN\0"), then the
///     raw buffer bytes, zero-padded to a multiple of 4 bytes.

#include "internal.h"
#include "long_path.h"

#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "castlemist/exportgltf/vrchat_export.h"  // safe_file_name

namespace castlemist::exportgltf {

using nlohmann::json;

namespace {

void put_u32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(static_cast<uint8_t>(v & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

} // namespace

std::string sanitize_name(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out.empty() ? std::string("Unnamed") : out;
}

GltfWriter::GltfWriter() {
    doc_["asset"] = {{"version", "2.0"}, {"generator", "castlemist glTF exporter"}};
    doc_["scene"] = 0;
    doc_["scenes"] = json::array({json{{"nodes", json::array()}}});
    doc_["nodes"] = json::array();
    doc_["meshes"] = json::array();
    doc_["accessors"] = json::array();
    doc_["bufferViews"] = json::array();
}

int GltfWriter::add_buffer_view(const void* data, size_t byteLength, int target) {
    while (bin_.size() % 4 != 0) bin_.push_back(0); // keep every view 4-byte aligned
    size_t offset = bin_.size();
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    bin_.insert(bin_.end(), bytes, bytes + byteLength);

    json bv{{"buffer", 0}, {"byteOffset", offset}, {"byteLength", byteLength}};
    if (target != 0) bv["target"] = target;
    doc_["bufferViews"].push_back(std::move(bv));
    return static_cast<int>(doc_["bufferViews"].size()) - 1;
}

int GltfWriter::add_accessor(const void* data, size_t byteLength, int componentType, const std::string& type,
                             size_t count, int target, const json* minVal, const json* maxVal) {
    int bvIndex = add_buffer_view(data, byteLength, target);
    json acc{{"bufferView", bvIndex}, {"componentType", componentType}, {"count", count}, {"type", type}};
    if (minVal) acc["min"] = *minVal;
    if (maxVal) acc["max"] = *maxVal;
    doc_["accessors"].push_back(std::move(acc));
    return static_cast<int>(doc_["accessors"].size()) - 1;
}

int GltfWriter::add_or_reuse_texture(uint32_t fileId, const std::vector<uint8_t>& pngBytes,
                                     const std::string& name) {
    // fileId 0 = generated in memory (a baked atlas), not a dat file: never shared.
    auto found = fileId ? textureByFileId_.find(fileId) : textureByFileId_.end();
    if (found != textureByFileId_.end()) return found->second;

    if (generatedPool_) {   // world export (Task 15d): content-named, once per export
        std::string err;
        const std::string leaf = generatedPool_->ensure_content(pngBytes, "", &err);
        if (leaf.empty()) {
            if (imageError_.empty()) imageError_ = err.empty() ? "Could not write a generated image." : err;
            return -1;
        }
        const std::string uri = generatedPrefix_ + leaf;
        const auto same = textureByUri_.find(uri);
        if (same != textureByUri_.end()) {
            if (fileId) textureByFileId_[fileId] = same->second;
            return same->second;
        }
        const int t = add_texture_uri(fileId, uri, name);
        textureByUri_[uri] = t;
        return t;
    }

    if (!imageDir_.empty()) {
        namespace fs = std::filesystem;
        // The image name (a dat texture's is its fileId) as a file name that
        // is also a valid glTF uri with no escaping: every byte outside
        // [A-Za-z0-9._-] becomes '_' (spaces, '#', '%' and non-ASCII would
        // need percent-encoding, which importers unquote inconsistently).
        // safe_file_name then trims dots and guards device names. Unique
        // ignoring case: Windows file names are case-insensitive.
        std::string raw = !name.empty() ? name : fileId ? std::to_string(fileId) : std::string("image");
        for (char& c : raw) {
            const auto u = static_cast<unsigned char>(c);
            if (!(std::isalnum(u) && u < 0x80) && c != '.' && c != '_' && c != '-') c = '_';
        }
        const std::string base = safe_file_name(raw);
        auto lower = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        std::string stem = base;
        for (int n = 2; imageStems_.count(lower(stem)); ++n) stem = base + "_" + std::to_string(n);
        imageStems_.insert(lower(stem));

        const std::string leaf = stem + ".png";
        const fs::path dir = fs::path(std::u8string(imageDir_.begin(), imageDir_.end()));
        std::error_code ec;
        make_dirs(dir, ec);
        std::ofstream out(long_path(dir / fs::path(std::u8string(leaf.begin(), leaf.end()))), std::ios::binary);
        if (out) out.write(reinterpret_cast<const char*>(pngBytes.data()), static_cast<std::streamsize>(pngBytes.size()));
        out.close();  // flush now, so a failed write shows in the stream state
        if (!out) {
            if (imageError_.empty()) imageError_ = "Could not write image '" + imageDir_ + "/" + leaf + "'.";
            return -1;
        }
        return add_texture_uri(fileId, imageUriPrefix_ + leaf, name);
    }

    int bvIndex = add_buffer_view(pngBytes.data(), pngBytes.size(), 0);
    json image{{"bufferView", bvIndex}, {"mimeType", "image/png"}};
    if (!name.empty()) image["name"] = name;
    doc_["images"].push_back(std::move(image));
    int imageIndex = static_cast<int>(doc_["images"].size()) - 1;

    json texture{{"source", imageIndex}};
    doc_["textures"].push_back(std::move(texture));
    int textureIndex = static_cast<int>(doc_["textures"].size()) - 1;

    if (fileId) textureByFileId_[fileId] = textureIndex;
    return textureIndex;
}

int GltfWriter::add_texture_uri(uint32_t fileId, const std::string& uri, const std::string& name) {
    auto found = fileId ? textureByFileId_.find(fileId) : textureByFileId_.end();
    if (found != textureByFileId_.end()) return found->second;

    json image{{"uri", uri}, {"mimeType", "image/png"}};
    if (!name.empty()) image["name"] = name;
    doc_["images"].push_back(std::move(image));
    int imageIndex = static_cast<int>(doc_["images"].size()) - 1;

    doc_["textures"].push_back(json{{"source", imageIndex}});
    int textureIndex = static_cast<int>(doc_["textures"].size()) - 1;

    if (fileId) textureByFileId_[fileId] = textureIndex;
    return textureIndex;
}

void GltfWriter::set_image_output(const std::string& dirUtf8, const std::string& uriPrefix) {
    imageDir_ = dirUtf8;
    imageUriPrefix_ = uriPrefix;
}

void GltfWriter::set_generated_pool(SharedImagePool* pool, const std::string& uriPrefix) {
    generatedPool_ = pool;
    generatedPrefix_ = uriPrefix;
}

std::string GltfWriter::texture_uri(int textureIndex) const {
    if (textureIndex < 0 || !doc_.contains("textures") || static_cast<size_t>(textureIndex) >= doc_["textures"].size())
        return {};
    const json& t = doc_["textures"][static_cast<size_t>(textureIndex)];
    if (!t.contains("source") || !t["source"].is_number_integer()) return {};
    const size_t img = t["source"].get<size_t>();
    if (!doc_.contains("images") || img >= doc_["images"].size()) return {};
    const json& im = doc_["images"][img];
    return im.contains("uri") && im["uri"].is_string() ? im["uri"].get<std::string>() : std::string();
}

int GltfWriter::add_node(json node) {
    doc_["nodes"].push_back(std::move(node));
    return static_cast<int>(doc_["nodes"].size()) - 1;
}

void GltfWriter::add_child(int parentIndex, int childIndex) {
    doc_["nodes"][static_cast<size_t>(parentIndex)]["children"].push_back(childIndex);
}

int GltfWriter::add_mesh(json mesh) {
    doc_["meshes"].push_back(std::move(mesh));
    return static_cast<int>(doc_["meshes"].size()) - 1;
}

int GltfWriter::add_material(json material) {
    doc_["materials"].push_back(std::move(material));
    return static_cast<int>(doc_["materials"].size()) - 1;
}

int GltfWriter::add_skin(json skin) {
    doc_["skins"].push_back(std::move(skin));
    return static_cast<int>(doc_["skins"].size()) - 1;
}

void GltfWriter::add_animation(json animation) { doc_["animations"].push_back(std::move(animation)); }

void GltfWriter::add_scene_root(int nodeIndex) { doc_["scenes"][0]["nodes"].push_back(nodeIndex); }

void GltfWriter::use_extension(const std::string& name) {
    json& used = doc_["extensionsUsed"];
    for (const json& e : used)
        if (e == name) return;
    used.push_back(name);
}

std::vector<uint8_t> GltfWriter::finish() {
    doc_["buffers"] = json::array({json{{"byteLength", bin_.size()}}});

    std::string jsonText = doc_.dump();
    while (jsonText.size() % 4 != 0) jsonText.push_back(' ');

    std::vector<uint8_t> bin = bin_;
    while (bin.size() % 4 != 0) bin.push_back(0);

    uint32_t jsonChunkLen = static_cast<uint32_t>(jsonText.size());
    uint32_t binChunkLen = static_cast<uint32_t>(bin.size());
    uint32_t totalLen = 12 + 8 + jsonChunkLen + (binChunkLen > 0 ? 8 + binChunkLen : 0);

    std::vector<uint8_t> out;
    out.reserve(totalLen);
    put_u32(out, 0x46546C67); // "glTF"
    put_u32(out, 2);          // version
    put_u32(out, totalLen);

    put_u32(out, jsonChunkLen);
    put_u32(out, 0x4E4F534A); // "JSON"
    out.insert(out.end(), jsonText.begin(), jsonText.end());

    if (binChunkLen > 0) {
        put_u32(out, binChunkLen);
        put_u32(out, 0x004E4942); // "BIN\0"
        out.insert(out.end(), bin.begin(), bin.end());
    }

    return out;
}

} // namespace castlemist::exportgltf
