/// @file
/// @brief Live tests for piece / character export (ripper/piece_export.h,
///        ripper/character_export.h). They need a real Gw2.dat: set
///        GW2_TEST_DAT; without it every test here skips.

#include "test_framework.h"

#include "castlemist/native/gw2dat.h"
#include "castlemist/ripper/character_export.h"
#include "castlemist/ripper/piece_export.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace castlemist::ripper;
namespace ch = castlemist::character;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

const char* dat_env() {
    const char* env = std::getenv("GW2_TEST_DAT");
    return env && *env ? env : nullptr;
}

struct Live {
    Gw2Dat dat;
    std::optional<castlemist::composite::Composite> comp;
};

Live& live() {
    static std::unique_ptr<Live> l;
    if (!l) {
        l = std::make_unique<Live>();
        load_dat_file(l->dat, dat_env());
        l->comp = load_composite(l->dat);
    }
    return *l;
}

fs::path scratch(const char* name) {
    fs::path d = fs::temp_directory_path() / (std::string("cm_test_ripper_") + name);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

ch::ManifestPiece warden_coat() {
    ch::ManifestPiece p;
    p.slot = "Coat";
    p.item_id = 78611;
    p.skin_id = 517;
    p.skin_name = "Warden Coat";
    p.skin_type = "Armor";
    p.skin_token = 0x00000348C28A32A3ull;
    p.file_ids = {41610, 61170};
    p.status = ch::PieceStatus::Ok;
    p.dyes.push_back(ch::ManifestDye{0, 6, "Celestial", "cloth", {211, 208, 207}, true,
                                     ch::DyeShift{57, 1.25f, 0, 0, 1.40625f}});
    p.dyes.push_back(ch::ManifestDye{1, 1682, "Abyssal Sun", "leather", {0, 0, 0}, true,
                                     ch::DyeShift{-128, 1, 0, 0, 0}});
    return p;
}

// The JSON chunk and the BIN chunk of a .glb.
std::pair<json, std::vector<uint8_t>> read_glb(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; };
    uint32_t jlen = u32(12);
    json j = json::parse(std::string(b.begin() + 20, b.begin() + 20 + jlen));
    size_t bin = 20 + jlen;
    uint32_t blen = u32(bin);
    return {j, std::vector<uint8_t>(b.begin() + bin + 8, b.begin() + bin + 8 + blen)};
}

} // namespace

CM_TEST(export, exports_sylvari_female_warden_coat) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    CHECK(live().comp.has_value());
    PieceContext ctx{&live().dat, &*live().comp, "SylvariFemale"};
    fs::path out = scratch("coat") / "coat.glb";
    PieceExportResult r = export_piece(ctx, warden_coat(), out.string());
    CHECK(r.ok);
    CHECK_EQ(r.status, std::string("armor"));
    CHECK_EQ(r.mesh, 40405u);
    CHECK_EQ(r.texture_base, 151455u);
    CHECK_EQ(r.dyed_channels, 2);  // both of the piece's dyes reached a mask
    CHECK(fs::exists(out));

    auto [j, bin] = read_glb(out);
    bool found_384x512 = false;  // the full-resolution copy (the reduced one is 192x256)
    CHECK(j.contains("images"));
    for (const auto& img : j.value("images", json::array())) {
        if (!img.contains("bufferView")) continue;
        const auto& bv = j["bufferViews"][img["bufferView"].get<size_t>()];
        size_t off = bv.value("byteOffset", size_t{0});
        auto be32 = [&](size_t at) {
            return (uint32_t(bin[at]) << 24) | (uint32_t(bin[at + 1]) << 16) | (uint32_t(bin[at + 2]) << 8) | bin[at + 3];
        };
        if (be32(off + 16) == 384 && be32(off + 20) == 512) found_384x512 = true;  // PNG IHDR
    }
    CHECK(found_384x512);

    // Every primitive whose material has a baseColorTexture samples UVs in [0,1].
    size_t textured = 0;
    for (const auto& mesh : j.value("meshes", json::array())) {
        for (const auto& prim : mesh.value("primitives", json::array())) {
            if (!prim.contains("material") || !prim["attributes"].contains("TEXCOORD_0")) continue;
            const auto& mat = j["materials"][prim["material"].get<size_t>()];
            if (!mat.value("pbrMetallicRoughness", json::object()).contains("baseColorTexture")) continue;
            const json acc = j["accessors"][prim["attributes"]["TEXCOORD_0"].get<size_t>()];
            const json bv = j["bufferViews"][acc["bufferView"].get<size_t>()];
            size_t at = bv.value("byteOffset", size_t{0}) + acc.value("byteOffset", size_t{0});
            size_t stride = bv.value("byteStride", size_t{8});
            float lo = 1e9f, hi = -1e9f;
            for (size_t k = 0; k < acc["count"].get<size_t>(); ++k) {
                float uv[2];
                std::memcpy(uv, bin.data() + at + k * stride, 8);
                lo = std::min({lo, uv[0], uv[1]});
                hi = std::max({hi, uv[0], uv[1]});
            }
            CHECK(lo >= -0.01f);
            CHECK(hi <= 1.01f);
            ++textured;
        }
    }
    CHECK(textured >= 1);
}

CM_TEST(export, armor_without_race_entry_is_reported) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    PieceContext ctx{&live().dat, &*live().comp, "CharrFemale"};
    ch::ManifestPiece p = warden_coat();
    p.slot = "Boots";
    p.skin_token = 0x0125089844F38644ull;  // Holographic greaves: no CharrFemale entry
    PieceExportResult r = export_piece(ctx, p, (scratch("norace") / "x.glb").string());
    CHECK_FALSE(r.ok);
    CHECK_EQ(r.status, std::string("skipped"));
    CHECK(r.reason.rfind("no appearance for CharrFemale", 0) == 0);
}

CM_TEST(export, weapon_exports_own_model) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    PieceContext ctx{&live().dat, &*live().comp, "SylvariFemale"};
    ch::ManifestPiece p;
    p.slot = "WeaponA1";
    p.skin_id = 8813;
    p.skin_name = "Holographic Dawn";
    p.skin_type = "Weapon";
    p.file_ids = {2163020, 2163007};
    p.status = ch::PieceStatus::Ok;
    fs::path out = scratch("weapon") / "w.glb";
    PieceExportResult r = export_piece(ctx, p, out.string());
    CHECK(r.ok);
    CHECK_EQ(r.status, std::string("model"));
    CHECK_EQ(r.mesh, 2163020u);
    CHECK(fs::exists(out));
}

CM_TEST(export, export_character_writes_report) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    ch::CharacterManifest m;
    m.name = "Test Character";
    m.race = "Sylvari";
    m.gender = "Female";
    m.pieces.push_back(warden_coat());
    ch::ManifestPiece ring;
    ring.slot = "Ring1";
    ring.status = ch::PieceStatus::NoSkin;
    m.pieces.push_back(ring);
    fs::path dir = scratch("character");
    CharacterExportReport rep = export_character(m, dat_env(), dir.string());
    CHECK(rep.error.empty());
    CHECK_EQ(rep.pieces.size(), size_t{2});
    CHECK_EQ(rep.exported(), size_t{1});
    CHECK(fs::exists(dir / "export_report.json"));
    CHECK(fs::exists(dir / "manifest.json"));
    CHECK(fs::exists(dir / "01_Coat_Warden_Coat.glb"));
}

CM_TEST(export, weapon_with_stray_token_exports_model) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    PieceContext ctx{&live().dat, &*live().comp, "SylvariFemale"};
    ch::ManifestPiece p;
    p.slot = "WeaponB2";
    p.skin_id = 11871;
    p.skin_name = "Soulfire Lantern Torch";
    p.skin_type = "Weapon";
    p.skin_token = 1;  // weapons carry unrelated data at +208 (seen live on skin 11871)
    p.file_ids = {3171942};
    p.status = ch::PieceStatus::Ok;
    PieceExportResult r = export_piece(ctx, p, (scratch("torch") / "t.glb").string());
    CHECK(r.ok);
    CHECK_EQ(r.status, std::string("model"));
}

CM_TEST(export, old_manifest_armor_is_not_exported_as_default_model) {
    // A sub-project-1 manifest: no skin_type, no skin_token, but armor has a weight class.
    ch::ManifestPiece p = warden_coat();
    p.skin_type.clear();
    p.skin_token = 0;
    p.weight_class = "Heavy";
    PieceContext ctx{nullptr, nullptr, "SylvariFemale"};  // never reaches the dat
    PieceExportResult r = export_piece(ctx, p, (scratch("oldmanifest") / "x.glb").string());
    CHECK_FALSE(r.ok);
    CHECK_EQ(r.status, std::string("skipped"));
    CHECK(r.reason.rfind("no appearance token", 0) == 0);
}

CM_TEST(export, export_character_into_a_non_ascii_folder) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    ch::CharacterManifest m;
    m.name = "Test Character";
    m.race = "Sylvari";
    m.gender = "Female";
    m.pieces.push_back(warden_coat());
    fs::path dir = fs::temp_directory_path() / fs::path(u8"cm_test_ripper_José");
    fs::remove_all(dir);
    std::u8string u8 = dir.u8string();
    CharacterExportReport rep = export_character(m, dat_env(), std::string(u8.begin(), u8.end()));
    CHECK(rep.error.empty());
    CHECK_EQ(rep.pieces.size(), size_t{1});
    if (!rep.pieces.empty()) CHECK_EQ(rep.pieces[0].second.reason, std::string());
    CHECK_EQ(rep.exported(), size_t{1});
    CHECK(fs::exists(dir / "01_Coat_Warden_Coat.glb"));
    fs::remove_all(dir);
}

CM_TEST(export, mirrored_uv_angler_vest_textures_its_big_meshes) {
    if (!dat_env()) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    PieceContext ctx{&live().dat, &*live().comp, "SylvariFemale"};
    ch::ManifestPiece p;
    p.slot = "Coat";
    p.skin_id = 10484;
    p.skin_name = "Angler Vest";
    p.skin_type = "Armor";
    p.weight_class = "Heavy";
    p.skin_token = 0x0000694226933823ull;  // its meshes' UVs run from -0.99 (mirrored half) to 0.37
    p.file_ids = {2585035};
    p.status = ch::PieceStatus::Ok;
    fs::path out = scratch("angler") / "vest.glb";
    PieceExportResult r = export_piece(ctx, p, out.string());
    CHECK(r.ok);
    CHECK_EQ(r.status, std::string("armor"));

    auto [j, bin] = read_glb(out);
    size_t textured_verts = 0, untextured_verts = 0;
    for (const auto& mesh : j["meshes"]) {
        for (const auto& prim : mesh["primitives"]) {
            size_t n = j["accessors"][prim["attributes"]["POSITION"].get<size_t>()]["count"].get<size_t>();
            bool tex = prim.contains("material") &&
                       j["materials"][prim["material"].get<size_t>()].value("pbrMetallicRoughness", json::object()).contains("baseColorTexture");
            (tex ? textured_verts : untextured_verts) += n;
        }
    }
    CHECK(textured_verts > 15000);   // both big vest meshes (5999 + 10130 vertices)
    CHECK(untextured_verts < 1000);  // only the small skin patch stays bare
}
