/// @file
/// @brief Extractor readers for world materials and motion: MODL material
///        `texTransforms`, prop `color`/`animSequence`, the env lighting
///        preset index, env `dataGlobal` water presets and terrain chunk
///        `uvData` (docs/research/gw2-world-materials.md §3-§6).
///
/// Every fixture is a hand-built 32-bit-pointer packfile (pfVersion 1) with a
/// minimal template, so these tests need no Gw2.dat and never skip. The real-map
/// checks of the same readers live with the world tests.

#include "test_framework.h"

#include "castlemist/native/gw2model.hpp"

#include <cstring>
#include <string>
#include <utility>
#include <vector>

using castlemist::model::Extractor;
using castlemist::model::json;

namespace {

// --- packfile building (the pattern of test_core_packfile.cpp) ---

void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put4cc(std::vector<uint8_t>& v, const char* s) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(s[i]));
}

/// @brief A 32-bit-pointer packfile holding one chunk of the given version.
std::vector<uint8_t> make_packfile(const char* container, const char* chunk, uint16_t version,
                                   const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f;
    f.push_back('P'); f.push_back('F');
    put16(f, 0x0001);                   // pfVersion 1: 4-byte self-relative pointers
    put16(f, 0);                        // reserved
    put16(f, 12);                       // headerSize -> first chunk
    put4cc(f, container);
    put4cc(f, chunk);
    put32(f, uint32_t(8 + payload.size()));
    put16(f, version);                  // chunk struct version
    put16(f, 16);                       // chunk headerSize
    put32(f, 0);                        // descriptorOffset
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}

/// @brief A chunk payload laid out by hand. Offsets are payload-relative;
///        pointers are self-relative, so they survive the move into the file.
struct Payload {
    std::vector<uint8_t> b;
    size_t alloc(size_t n) { size_t at = b.size(); b.resize(at + n, 0); return at; }
    void u8(size_t at, uint8_t v) { b[at] = v; }
    void u16(size_t at, uint16_t v) { b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); }
    void u32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i)); }
    void u64(size_t at, uint64_t v) { u32(at, uint32_t(v)); u32(at + 4, uint32_t(v >> 32)); }
    void f32(size_t at, float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(at, u); }
    void ptr(size_t at, size_t target) { u32(at, uint32_t(target - at)); }
    /// array_ptr / ptr_array_ptr header: count, then a pointer at +4.
    void arr(size_t at, uint32_t count, size_t target) { u32(at, count); ptr(at + 4, target); }
    /// A filename target for fileId 1 (0xFF00 * 0x100 + 0x100 - 0xFF00FF), 0-terminated.
    size_t fileOne() { size_t at = alloc(8); u16(at, 0x100); u16(at + 2, 0x100); return at; }
};

json field(const char* name, const char* kind) { return json{{"name", name}, {"kind", kind}}; }
json arrayOf(const char* name, const char* kind, const char* elemStruct) {
    return json{{"name", name}, {"kind", kind}, {"element", json{{"struct", elemStruct}}}};
}
json ptrTo(const char* name, const char* target) {
    return json{{"name", name}, {"kind", "ptr"}, {"target", json{{"struct", target}}}};
}

// The field list of ModelMaterialTexTransformV66, copied from gw2_packfile.json.
json texTransformV66() {
    return json::array({field("flags", "dword"), field("uvIndex", "byte"), field("columns", "byte"),
                        field("rows", "byte"), field("count", "word"), field("fps", "float"),
                        field("scroll", "float2"), field("scrollFreq", "float2"), field("scale", "float2"),
                        field("scaleFreq", "float2"), field("rotate", "float"), field("rotate", "float")});
}
// The field list of ModelMaterialTexTransformV45, copied from gw2_packfile.json.
json texTransformV45() {
    return json::array({field("random", "byte"), field("uvIndex", "byte"), field("columns", "byte"),
                        field("rows", "byte"), field("count", "word"), field("fps", "float"),
                        field("scroll", "float2")});
}

/// @brief MODL template: TRoot{permutations} -> TPerm{materials} -> TMat{texTransforms?}.
json modlTemplate(const json& ttFields, bool withTexTransforms) {
    json t;
    t["chunks"]["MODL"]["66"] = "TRoot";
    t["types"]["TRoot"]["fields"] = json::array({arrayOf("permutations", "array_ptr", "TPerm")});
    t["types"]["TPerm"]["fields"] = json::array({arrayOf("materials", "ptr_array_ptr", "TMat")});
    t["types"]["TMat"]["fields"] = withTexTransforms
        ? json::array({field("sortOrder", "dword"), arrayOf("texTransforms", "array_ptr", "TT")})
        : json::array({field("sortOrder", "dword")});
    t["types"]["TT"]["fields"] = ttFields;
    return t;
}

/// @brief One permutation with one material. `writeTT` fills the material's
///        tail (after sortOrder) and returns nothing; the material is at `mat`.
template <class F>
std::vector<uint8_t> modlFile(size_t matSize, F&& writeTT) {
    Payload p;
    const size_t root = p.alloc(8);          // permutations: array_ptr
    const size_t perm = p.alloc(8);          // materials: ptr_array_ptr
    const size_t matPtrs = p.alloc(4);       // one material pointer
    const size_t mat = p.alloc(matSize);
    p.arr(root, 1, perm);
    p.arr(perm, 1, matPtrs);
    p.ptr(matPtrs, mat);
    p.u32(mat, 7);                           // sortOrder
    writeTT(p, mat);
    return make_packfile("MODL", "MODL", 66, p.b);
}

} // namespace

// Model 1126949's material 0 (§4): two V66 entries, uvIndex 1 and 0, both
// scroll (0, 0.2). The first also sets the two `rotate` fields apart so the
// by-position read of the duplicate name is pinned.
CM_TEST(native_materials, tex_transforms_read_by_name) {
    constexpr size_t kTT = 53;  // V66: 4 + 1 + 1 + 1 + 2 + 4 + 4 * 8 + 4 + 4
    const auto bytes = modlFile(4 + 8, [&](Payload& p, size_t mat) {
        const size_t tt = p.alloc(2 * kTT);
        p.arr(mat + 4, 2, tt);
        // entry 0
        p.u8(tt + 4, 1); p.u8(tt + 5, 1); p.u8(tt + 6, 1); p.u16(tt + 7, 1);
        p.f32(tt + 13, 0.f); p.f32(tt + 17, 0.2f);
        p.f32(tt + 45, 0.25f); p.f32(tt + 49, 0.5f);
        // entry 1
        const size_t e1 = tt + kTT;
        p.u8(e1 + 4, 0); p.f32(e1 + 13, 0.f); p.f32(e1 + 17, 0.2f);
    });
    const auto m = Extractor(bytes, modlTemplate(texTransformV66(), true)).extract();
    CHECK_EQ(m.materials.size(), size_t{1});
    const auto& tt = m.materials[0].texTransforms;
    CHECK_EQ(tt.size(), size_t{2});
    CHECK_EQ(int(tt[0].uvIndex), 1);
    CHECK_EQ(int(tt[0].columns), 1);
    CHECK_EQ(int(tt[0].rows), 1);
    CHECK_EQ(int(tt[0].count), 1);
    CHECK_NEAR(tt[0].scroll[0], 0.0, 1e-6);
    CHECK_NEAR(tt[0].scroll[1], 0.2, 1e-6);
    CHECK_NEAR(tt[0].rotate, 0.25, 1e-6);
    CHECK_NEAR(tt[0].rotate2, 0.5, 1e-6);
    CHECK_EQ(int(tt[1].uvIndex), 0);
    CHECK_NEAR(tt[1].scroll[1], 0.2, 1e-6);
    CHECK_NEAR(tt[1].rotate2, 0.0, 1e-6);
}

// V45 stores `random` where V66 has `flags`, and has no scale/rotate fields:
// those stay 0 rather than being read from the neighbouring bytes.
CM_TEST(native_materials, tex_transforms_v45_without_flags) {
    constexpr size_t kTT = 18;  // V45: 1 + 1 + 1 + 1 + 2 + 4 + 8
    const auto bytes = modlFile(4 + 8, [&](Payload& p, size_t mat) {
        const size_t tt = p.alloc(kTT + 16);  // trailing bytes: what a mis-sized read would see
        p.arr(mat + 4, 1, tt);
        p.u8(tt + 0, 0xAB);                   // random
        p.u8(tt + 1, 2);                      // uvIndex
        p.u8(tt + 2, 1); p.u8(tt + 3, 1); p.u16(tt + 4, 1);
        p.f32(tt + 6, 3.f);                   // fps
        p.f32(tt + 10, 0.5f); p.f32(tt + 14, 0.f);
        p.f32(tt + 18, 9.f); p.f32(tt + 22, 9.f);
    });
    const auto m = Extractor(bytes, modlTemplate(texTransformV45(), true)).extract();
    CHECK_EQ(m.materials.size(), size_t{1});
    const auto& tt = m.materials[0].texTransforms;
    CHECK_EQ(tt.size(), size_t{1});
    CHECK_EQ(tt[0].flags, uint32_t{0});
    CHECK_EQ(int(tt[0].uvIndex), 2);
    CHECK_NEAR(tt[0].fps, 3.0, 1e-6);
    CHECK_NEAR(tt[0].scroll[0], 0.5, 1e-6);
    CHECK_NEAR(tt[0].scale[0], 0.0, 1e-6);
    CHECK_NEAR(tt[0].scale[1], 0.0, 1e-6);
    CHECK_NEAR(tt[0].rotate, 0.0, 1e-6);
}

CM_TEST(native_materials, no_tex_transforms_field_is_empty) {
    const auto bytes = modlFile(4, [](Payload&, size_t) {});
    const auto m = Extractor(bytes, modlTemplate(texTransformV66(), false)).extract();
    CHECK_EQ(m.materials.size(), size_t{1});
    CHECK_EQ(m.materials[0].sortOrder, uint32_t{7});
    CHECK(m.materials[0].texTransforms.empty());
}

// A count that would run past the file is garbage: no entries, no throw.
CM_TEST(native_materials, tex_transforms_past_the_file_are_dropped) {
    const auto bytes = modlFile(4 + 8, [&](Payload& p, size_t mat) {
        const size_t tt = p.alloc(53);
        p.arr(mat + 4, 1000000, tt);
    });
    const auto m = Extractor(bytes, modlTemplate(texTransformV66(), true)).extract();
    CHECK_EQ(m.materials.size(), size_t{1});
    CHECK(m.materials[0].texTransforms.empty());
}

// propAnimArray keeps `color` and `animSequence` (§5: 463 = "on");
// propArray has no animSequence and keeps 0.
CM_TEST(native_materials, prop_color_and_anim_sequence) {
    json t;
    t["chunks"]["prp2"]["21"] = "TProps";
    t["types"]["TProps"]["fields"] = json::array({arrayOf("propArray", "array_ptr", "TObj"),
                                                  arrayOf("propAnimArray", "array_ptr", "TAnim")});
    t["types"]["TObj"]["fields"] = json::array({field("filename", "filename"), field("position", "float3"),
                                                field("color", "byte4"), field("scale", "float")});
    t["types"]["TAnim"]["fields"] = json::array({field("filename", "filename"), field("position", "float3"),
                                                 field("color", "byte4"), field("scale", "float"),
                                                 field("animSequence", "qword")});
    Payload p;
    const size_t root = p.alloc(16);
    const size_t obj = p.alloc(4 + 12 + 4 + 4);
    const size_t anim = p.alloc(4 + 12 + 4 + 4 + 8);
    p.arr(root, 1, obj);
    p.arr(root + 8, 1, anim);
    p.ptr(obj, p.fileOne());
    p.u8(obj + 16, 0x4A); p.u8(obj + 17, 0x81); p.u8(obj + 18, 0x64); p.u8(obj + 19, 0x80);
    p.f32(obj + 20, 1.f);
    p.ptr(anim, p.fileOne());
    p.u8(anim + 16, 0x80); p.u8(anim + 17, 0x80); p.u8(anim + 18, 0x80); p.u8(anim + 19, 0x80);
    p.f32(anim + 20, 1.f);
    p.u64(anim + 24, 463);
    const auto bytes = make_packfile("mapc", "prp2", 21, p.b);

    const auto props = Extractor(bytes, t).parseMapProps();
    CHECK_EQ(props.size(), size_t{2});
    CHECK_EQ(props[0].group, std::string("propArray"));
    CHECK_EQ(int(props[0].color[0]), 0x4A);
    CHECK_EQ(int(props[0].color[3]), 0x80);
    CHECK_EQ(props[0].animSequence, uint64_t{0});
    CHECK_EQ(props[1].group, std::string("propAnimArray"));
    CHECK_EQ(int(props[1].color[1]), 0x80);
    CHECK_EQ(props[1].animSequence, uint64_t{463});
    CHECK_EQ(castlemist::model::detokenizeName64(props[1].animSequence), std::string("on"));
}

namespace {

/// @brief env template: TEnv{dataGlobal: ptr} -> PackMapEnvDataGlobalV76{lighting, water}.
///        parseMapEnv resolves the global struct by name, so it carries the real name.
json envTemplate(const char* waterKind) {
    json t;
    t["chunks"]["env"]["76"] = "TEnv";
    t["types"]["TEnv"]["fields"] = json::array({ptrTo("dataGlobal", "PackMapEnvDataGlobalV76")});
    t["types"]["PackMapEnvDataGlobalV76"]["fields"] =
        json::array({arrayOf("lighting", "array_ptr", "TLighting"), arrayOf("water", waterKind, "TWater")});
    t["types"]["TLighting"]["fields"] = json::array({arrayOf("lights", "ptr_array_ptr", "TLight")});
    t["types"]["TLight"]["fields"] = json::array({field("color", "byte3"), field("intensity", "float"),
                                                  field("direction", "float3")});
    // A subset of PackMapEnvDataWaterV76 plus V29's own bumpTexture.
    t["types"]["TWater"]["fields"] = json::array({
        field("waterFlags", "dword"), field("bumpTile0", "float"), field("bumpTile1", "float"),
        field("surfaceDeepColor", "byte4"), field("materialFilename", "filename"),
        json{{"name", "textureFilenames"}, {"kind", "array_ptr"}, {"element", "filename"}},
        json{{"name", "constantTokens"}, {"kind", "array_ptr"}, {"element", "dword"}},
        json{{"name", "constantValues"}, {"kind", "array_ptr"}, {"element", "float4"}},
        field("foamSpawn", "float"), field("bumpTexture", "filename")});
    return t;
}
constexpr size_t kWater = 4 + 4 + 4 + 4 + 4 + 8 + 8 + 8 + 4 + 4;

/// @brief Fill one TWater at `w`: preset k gets bumpTile0 = 5 + k.
void writeWater(Payload& p, size_t w, int k) {
    p.u32(w, 0x10u + k);
    p.f32(w + 4, 5.f + k); p.f32(w + 8, 13.66f);
    p.u8(w + 12, 0x36); p.u8(w + 13, 0x4F); p.u8(w + 14, 0x21);
    p.ptr(w + 16, p.fileOne());
    const size_t tex = p.alloc(8);             // (null), fileId 1
    p.ptr(tex + 4, p.fileOne());
    p.arr(w + 20, 2, tex);
    const size_t tok = p.alloc(4); p.u32(tok, 0xC0FFEEu);
    p.arr(w + 28, 1, tok);
    const size_t val = p.alloc(16); p.f32(val, 1.f); p.f32(val + 12, 4.f);
    p.arr(w + 36, 1, val);
    p.f32(w + 44, 0.75f);
    p.ptr(w + 48, p.fileOne());
}

/// @brief One env chunk: two lighting presets (preset 1 brighter) and two
///        water presets, inline (`array_ptr`) or by pointer (`ptr_array_ptr`).
std::vector<uint8_t> envFile(bool waterByPointer) {
    Payload p;
    const size_t root = p.alloc(4);
    const size_t g = p.alloc(16);
    p.ptr(root, g);
    // lighting[2], each with one light
    const size_t lighting = p.alloc(16);
    p.arr(g, 2, lighting);
    for (int k = 0; k < 2; ++k) {
        const size_t lp = p.alloc(4), l = p.alloc(3 + 4 + 12);
        p.arr(lighting + 8 * k, 1, lp);
        p.ptr(lp, l);
        p.u8(l, 255); p.u8(l + 1, 255); p.u8(l + 2, 255);
        p.f32(l + 3, k == 0 ? 0.25f : 2.0f);
        p.f32(l + 7, 0.f); p.f32(l + 11, 0.f); p.f32(l + 15, 1.f);
    }
    if (waterByPointer) {
        const size_t ptrs = p.alloc(8);
        p.arr(g + 8, 2, ptrs);
        for (int k = 0; k < 2; ++k) {
            const size_t w = p.alloc(kWater);
            p.ptr(ptrs + 4 * k, w);
            writeWater(p, w, k);
        }
    } else {
        const size_t ws = p.alloc(2 * kWater);
        p.arr(g + 8, 2, ws);
        for (int k = 0; k < 2; ++k) writeWater(p, ws + k * kWater, k);
    }
    return make_packfile("mapc", "env", 76, p.b);
}

void checkWater(const std::vector<Extractor::MapEnvWater>& water) {
    CHECK_EQ(water.size(), size_t{2});
    for (int k = 0; k < 2; ++k) {
        const auto& w = water[size_t(k)];
        CHECK_EQ(w.waterFlags, uint32_t(0x10 + k));
        CHECK_NEAR(w.bumpTile[0], 5.0 + k, 1e-6);
        CHECK_NEAR(w.bumpTile[1], 13.66, 1e-5);
        CHECK_EQ(int(w.surfaceDeepColor[0]), 0x36);
        CHECK_EQ(int(w.surfaceDeepColor[2]), 0x21);
        CHECK_EQ(w.materialFileId, uint32_t{1});
        CHECK_EQ(w.textureFileIds.size(), size_t{2});
        CHECK_EQ(w.textureFileIds[0], uint32_t{0});   // a null entry is kept
        CHECK_EQ(w.textureFileIds[1], uint32_t{1});
        CHECK_EQ(w.constantTokens.size(), size_t{1});
        CHECK_EQ(w.constantTokens[0], uint32_t{0xC0FFEE});
        CHECK_EQ(w.constantValues.size(), size_t{1});
        CHECK_NEAR(w.constantValues[0][0], 1.0, 1e-6);
        CHECK_NEAR(w.constantValues[0][3], 4.0, 1e-6);
        CHECK_NEAR(w.foamSpawn, 0.75, 1e-6);
        CHECK_EQ(w.bumpTextureFileId, uint32_t{1});
        CHECK_EQ(w.patternTextureFileId, uint32_t{0});  // absent from this struct
        CHECK_NEAR(w.patternTile, 0.0, 1e-6);
    }
}

} // namespace

CM_TEST(native_materials, env_water_presets_by_pointer) {
    const auto bytes = envFile(true);
    checkWater(Extractor(bytes, envTemplate("ptr_array_ptr")).parseMapEnvWater());
}

CM_TEST(native_materials, env_water_presets_inline) {
    const auto bytes = envFile(false);
    checkWater(Extractor(bytes, envTemplate("array_ptr")).parseMapEnvWater());
}

CM_TEST(native_materials, env_water_absent_is_empty) {
    const auto bytes = make_packfile("mapc", "parm", 1, std::vector<uint8_t>(16, 0));
    json t; t["types"] = json::object();
    CHECK(Extractor(bytes, t).parseMapEnvWater().empty());
}

// presetIndex is the lighting[] entry parseMapEnv used: the brightest on auto,
// the requested one when valid, and -1 when nothing was read.
CM_TEST(native_materials, env_light_preset_index) {
    const auto bytes = envFile(true);
    const json t = envTemplate("ptr_array_ptr");
    const auto autoPick = Extractor(bytes, t).parseMapEnv();
    CHECK(autoPick.present);
    CHECK_EQ(autoPick.presetIndex, 1);
    CHECK_NEAR(autoPick.sunIntensity, 2.0, 1e-6);
    CHECK_EQ(Extractor(bytes, t).parseMapEnv(0).presetIndex, 0);
    CHECK_EQ(Extractor(bytes, t).parseMapEnv(7).presetIndex, 1);   // out of range -> auto

    const auto none = make_packfile("mapc", "parm", 1, std::vector<uint8_t>(16, 0));
    json empty; empty["types"] = json::object();
    const auto absent = Extractor(none, empty).parseMapEnv();
    CHECK_FALSE(absent.present);
    CHECK_EQ(absent.presetIndex, -1);
}

// trn.materials.materials[]: chunk 0 has uvData, chunk 1 a null pointer.
CM_TEST(native_materials, terrain_chunk_uv_data) {
    json t;
    t["chunks"]["trn"]["14"] = "TTrn";
    t["types"]["TTrn"]["fields"] = json::array({ptrTo("materials", "TMats")});
    t["types"]["TMats"]["fields"] = json::array({arrayOf("materials", "array_ptr", "TChunk")});
    t["types"]["TChunk"]["fields"] = json::array({ptrTo("uvData", "TUV")});
    // The field list of PackMapTerrainChunkUVDataV14, copied from gw2_packfile.json.
    t["types"]["TUV"]["fields"] = json::array({field("translation", "float2"), field("xScaleRange", "float2"),
                                               field("yScaleRange", "float2"), field("scaleSpeed", "float2"),
                                               field("rotation", "float")});
    Payload p;
    const size_t root = p.alloc(4), mats = p.alloc(8), chunks = p.alloc(8), uv = p.alloc(36);
    p.ptr(root, mats);
    p.arr(mats, 2, chunks);
    p.ptr(chunks, uv);
    for (int k = 0; k < 8; ++k) p.f32(uv + 4 * k, 0.5f + k);
    p.f32(uv + 32, 1.25f);
    const auto bytes = make_packfile("mapc", "trn", 14, p.b);

    const auto tm = Extractor(bytes, t).parseTerrainMaterials();
    CHECK(tm.present);
    CHECK_EQ(tm.chunks.size(), size_t{2});
    const auto& c0 = tm.chunks[0];
    CHECK(c0.hasUvData);
    CHECK_NEAR(c0.uv.translation[0], 0.5, 1e-6);
    CHECK_NEAR(c0.uv.translation[1], 1.5, 1e-6);
    CHECK_NEAR(c0.uv.xScaleRange[0], 2.5, 1e-6);
    CHECK_NEAR(c0.uv.yScaleRange[1], 5.5, 1e-6);
    CHECK_NEAR(c0.uv.scaleSpeed[1], 7.5, 1e-6);
    CHECK_NEAR(c0.uv.rotation, 1.25, 1e-6);
    CHECK_FALSE(tm.chunks[1].hasUvData);
    CHECK_NEAR(tm.chunks[1].uv.rotation, 0.0, 1e-6);
}
