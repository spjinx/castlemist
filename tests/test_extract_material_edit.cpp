/// @file
/// @brief Tests for the material editor's edits to a game-shader material
///        (material_edit.cpp): uniforms, sampler rebinding and the JSON form.

#include "test_framework.h"

#include "castlemist/extract/material_edit.h"

#include <string>
#include <vector>

using namespace castlemist::extract;

namespace {

// A material whose VS reads `World` (engine) and `scroll`, and whose PS reads
// `gloover`, the two-register array `tints` and `AlphaRef` (engine), plus two
// samplers. `gloover` carries a MODL constant.
GameMaterial sample_material() {
    GameMaterial g;
    g.index = 3;
    g.ok = true;
    g.vsUniforms = {{"World", 4, 0, 0, 4}, {"scroll", 2, 0, 64, 1}};
    g.psUniforms = {{"s_diffuse", 0, 2, 0, 1}, {"AlphaRef", 2, 1, 0, 1}, {"gloover", 2, 1, 16, 1},
                    {"tints", 2, 1, 32, 2}, {"scroll", 2, 1, 64, 1}};
    g.psConsts = {{16, {0.5f, 0.25f, 0.0f, 1.0f}}};
    g.samplers = {{0, 4, 0, 1, "diffuse"}, {1, -1, 1, 0, ""}};
    return g;
}

const MaterialUniform* find(const std::vector<MaterialUniform>& us, const std::string& key) {
    for (const auto& u : us)
        if (u.key == key) return &u;
    return nullptr;
}

}  // namespace

CM_TEST(material_edit, lists_material_uniforms_per_register_without_engine_ones) {
    const auto us = material_uniforms(sample_material());
    CHECK(find(us, "World") == nullptr);
    CHECK(find(us, "AlphaRef") == nullptr);
    CHECK(find(us, "s_diffuse") == nullptr);  // a sampler, not a constant
    CHECK_EQ(us.size(), size_t{4});           // scroll, gloover, tints, tints[1]

    const MaterialUniform* glo = find(us, "gloover");
    CHECK(glo != nullptr && glo->set && glo->ps && !glo->vs);
    if (glo) CHECK_NEAR(glo->value[1], 0.25f, 1e-6f);

    const MaterialUniform* t1 = find(us, "tints[1]");
    CHECK(t1 != nullptr && t1->name == "tints" && t1->reg == 1 && !t1->set);

    const MaterialUniform* sc = find(us, "scroll");
    CHECK(sc != nullptr && sc->vs && sc->ps);
}

CM_TEST(material_edit, set_uniform_writes_every_stage_at_the_register_offset) {
    GameMaterial g = sample_material();
    CHECK(set_material_uniform(g, "scroll", {1, 2, 3, 4}));
    CHECK(set_material_uniform(g, "tints[1]", {9, 9, 9, 9}));
    CHECK(set_material_uniform(g, "gloover", {7, 0, 0, 0}));
    CHECK_FALSE(set_material_uniform(g, "nosuch", {1, 1, 1, 1}));
    CHECK_FALSE(set_material_uniform(g, "tints[2]", {1, 1, 1, 1}));  // past the array

    CHECK_EQ(g.vsConsts.size(), size_t{1});
    CHECK_EQ(g.vsConsts[0].byteOff, 64);
    CHECK_EQ(g.psConsts.size(), size_t{3});  // gloover (rewritten in place), scroll, tints[1]
    bool tints1 = false, glo = false;
    for (const auto& c : g.psConsts) {
        if (c.byteOff == 48) tints1 = c.value[0] == 9.0f;
        if (c.byteOff == 16) glo = c.value[0] == 7.0f;
    }
    CHECK(tints1);
    CHECK(glo);
}

CM_TEST(material_edit, sampler_rebind_clears_global_and_atlas) {
    GameMaterial g = sample_material();
    CHECK(set_sampler_texture(g, 1, 6));
    CHECK_EQ(g.samplers[1].gameTex, 6);
    CHECK_EQ(g.samplers[1].global, 0);
    CHECK(set_sampler_texture(g, 0, 2));
    CHECK_EQ(g.samplers[0].atlas, 0);
    CHECK_FALSE(set_sampler_texture(g, 9, 2));
}

CM_TEST(material_edit, edits_apply_and_count_misses) {
    GameMaterial g = sample_material();
    MaterialEdit e;
    e.index = 3;
    e.textures = {{1, 500}, {1, 500}, {7, 501}, {0, 999}};  // slot 7 missing, 999 will not "decode"
    e.uniforms = {{"gloover", {2, 2, 2, 2}}, {"nosuch", {0, 0, 0, 0}}};
    const TextureLookup lookup = [](uint32_t fid) { return fid == 500 ? 8 : fid == 501 ? 9 : -1; };
    CHECK_EQ(apply_texture_and_uniform_edits(g, e, lookup), 3);
    CHECK_EQ(g.samplers[1].gameTex, 8);
    CHECK_EQ(g.samplers[0].gameTex, 4);  // 999 failed: slot left alone
}

CM_TEST(material_edit, json_round_trips_and_drops_empty_edits) {
    MaterialEdits edits;
    MaterialEdit& a = edit_for(edits, 2);
    a.fromModel = 1234;
    a.fromMaterial = 5;
    a.amatFile = 777;
    a.blended = true;
    a.textures[3] = 4242;
    a.uniforms["gloover"] = {0.5f, 1, 0, 1};
    edit_for(edits, 4);  // empty: not written
    CHECK(&edit_for(edits, 2) == &edits[0]);

    const nlohmann::json j = material_edits_to_json(edits, 99);
    CHECK_EQ(j["model"].get<uint32_t>(), 99u);
    CHECK_EQ(j["materials"].size(), size_t{1});

    const MaterialEdits back = material_edits_from_json(nlohmann::json::parse(j.dump()));
    CHECK_EQ(back.size(), size_t{1});
    const MaterialEdit& b = back[0];
    CHECK_EQ(b.index, 2u);
    CHECK_EQ(b.fromModel, 1234u);
    CHECK_EQ(b.fromMaterial, 5u);
    CHECK_EQ(b.amatFile, 777u);
    CHECK(b.blended);
    CHECK_EQ(b.textures.at(3), 4242u);
    CHECK_NEAR(b.uniforms.at("gloover")[0], 0.5f, 1e-6f);
}

CM_TEST(material_edit, json_reader_skips_what_it_cannot_read) {
    const auto j = nlohmann::json::parse(R"({"materials":[
        {"index":-1},
        {"amat":5},
        {"index":1,"uniforms":{"a":[1,2,3],"b":[1,"x",3,4],"c":[1,2,3,4]},"textures":{"0":"x"}}
    ]})");
    const MaterialEdits e = material_edits_from_json(j);
    CHECK_EQ(e.size(), size_t{1});
    CHECK_EQ(e[0].uniforms.size(), size_t{1});
    CHECK(e[0].uniforms.count("c") == 1);
    CHECK(e[0].textures.empty());

    bool threw = false;
    try { material_edits_from_json(nlohmann::json::array()); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
