/// @file
/// @brief Live test for whole-character assembly (ripper/assemble.h). Needs a
///        real Gw2.dat in GW2_TEST_DAT; skips without one.

#include "test_framework.h"

#include "castlemist/ripper/assemble.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

using namespace castlemist::ripper;
namespace ch = castlemist::character;
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

ch::ManifestPiece armor(const char* slot, const char* name, uint64_t token) {
    ch::ManifestPiece p;
    p.slot = slot;
    p.skin_id = 1;
    p.skin_name = name;
    p.skin_type = "Armor";
    p.weight_class = "Heavy";
    p.skin_token = token;
    p.status = ch::PieceStatus::Ok;
    return p;
}

const AssemblyPart* part(const AssemblyReport& r, const std::string& name) {
    for (const AssemblyPart& p : r.parts)
        if (p.name == name) return &p;
    return nullptr;
}

} // namespace

CM_TEST(assemble, sylvari_female_with_vest_leggings_and_greatsword) {
    const char* dat = std::getenv("GW2_TEST_DAT");
    if (!dat || !*dat) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    ch::CharacterManifest m;
    m.name = "Test Character";
    m.race = "Sylvari";
    m.gender = "Female";
    m.pieces.push_back(armor("Coat", "Angler Vest", 0x0000694226933823ull));
    m.pieces.push_back(armor("Leggings", "Chainmail Leggings", 0x0031220DA8E48503ull));
    ch::ManifestPiece sword;
    sword.slot = "WeaponA1";
    sword.skin_id = 8813;
    sword.skin_name = "Holographic Dawn";
    sword.skin_type = "Weapon";
    sword.file_ids = {2163020};
    sword.status = ch::PieceStatus::Ok;
    m.pieces.push_back(sword);

    fs::path dir = fs::temp_directory_path() / "cm_test_assemble";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path out = dir / "character.glb";
    AssemblyReport r = assemble_character(m, dat, out.string());
    CHECK(r.error.empty());
    CHECK(r.ok);
    CHECK(fs::exists(out));
    CHECK(r.joints >= 153);

    CHECK(part(r, "body chest") && part(r, "body chest")->status == "hidden");  // under the Coat
    CHECK(part(r, "body legs") && part(r, "body legs")->status == "hidden");    // under the Leggings
    CHECK(part(r, "body hands") && part(r, "body hands")->status == "used");    // no Gloves
    CHECK(part(r, "face") && part(r, "face")->status == "used");
    CHECK(part(r, "hair") && part(r, "hair")->status == "used");                // no Helm
    CHECK(part(r, "Coat") && part(r, "Coat")->status == "used");
    CHECK(part(r, "WeaponA1") && part(r, "WeaponA1")->status == "used");

    std::ifstream f(out, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint32_t jlen;
    std::memcpy(&jlen, b.data() + 12, 4);
    json j = json::parse(std::string(b.begin() + 20, b.begin() + 20 + jlen));
    CHECK_EQ(j["skins"].size(), size_t{1});
    bool atlas_material = false;
    for (const auto& mat : j["materials"]) atlas_material |= mat.value("name", std::string()) == "CharacterAtlas";
    CHECK(atlas_material);

    // Upright, and in the same GW2 units as castlemist's other exports.
    bool upright_unscaled = false;
    for (const auto& node : j["nodes"])
        if (node.value("name", std::string()) == "GW2_ZupToYup")
            upright_unscaled = !node.contains("scale") && std::abs(node["rotation"][2].get<double>() - 0.70710678) < 1e-6;
    CHECK(upright_unscaled);

    // The face must survive the hair/scalp layer drawn over it (it used to go black).
    std::vector<uint8_t> bin(b.begin() + 20 + jlen + 8, b.end());
    for (const auto& mat : j["materials"]) {
        if (mat.value("name", std::string()) != "CharacterAtlas") continue;
        size_t img = j["textures"][mat["pbrMetallicRoughness"]["baseColorTexture"]["index"].get<size_t>()]["source"];
        const auto& bv = j["bufferViews"][j["images"][img]["bufferView"].get<size_t>()];
        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load_from_memory(bin.data() + bv.value("byteOffset", size_t{0}),
                                                  static_cast<int>(bv["byteLength"].get<size_t>()), &w, &h, &n, 4);
        CHECK(px != nullptr);
        if (!px) break;
        double red = 0;
        size_t count = 0;
        for (int y = 512; y < 768; ++y)
            for (int x = 384; x < 768; ++x) {
                red += px[(static_cast<size_t>(y) * w + x) * 4];
                ++count;
            }
        stbi_image_free(px);
        CHECK(red / count > 30.0);  // the face's own colour, not black
    }
}

CM_TEST(assemble, helm_keeps_the_scalp_and_metres_option_scales) {
    const char* dat = std::getenv("GW2_TEST_DAT");
    if (!dat || !*dat) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    ch::CharacterManifest m;
    m.name = "Test Character";
    m.race = "Sylvari";
    m.gender = "Female";
    m.pieces.push_back(armor("Helm", "Holographic Dragon Helm", 0x0425089844F38644ull));
    fs::path dir = fs::temp_directory_path() / "cm_test_assemble_helm";
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::path out = dir / "character.glb";
    AssemblyOptions opt;
    opt.metres = true;
    AssemblyReport r = assemble_character(m, dat, out.string(), opt);
    CHECK(r.ok);
    const AssemblyPart* hair = part(r, "hair");
    CHECK(hair != nullptr);
    CHECK_EQ(hair->status, std::string("used"));
    CHECK(hair->reason.rfind("scalp only", 0) == 0);  // strands hidden under the Helm, scalp kept

    std::ifstream f(out, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint32_t jlen;
    std::memcpy(&jlen, b.data() + 12, 4);
    json j = json::parse(std::string(b.begin() + 20, b.begin() + 20 + jlen));
    bool metres = false;
    for (const auto& node : j["nodes"])
        if (node.value("name", std::string()) == "GW2_ZupToYup")
            metres = node.contains("scale") && std::abs(node["scale"][1].get<double>() - 0.0254) < 1e-9;
    CHECK(metres);
}
