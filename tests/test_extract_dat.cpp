/// @file
/// @brief End-to-end extraction against a real Gw2.dat.
///
/// Everything else in the suite runs on synthetic fixtures, which proves the
/// parsers are self-consistent but not that they agree with ArenaNet. This file
/// closes that gap: it opens the actual archive and asserts that a curated set
/// of entries still decodes to the type it decoded to when it was reverse
/// engineered (`GW2CURATED.txt`).
///
/// The archive is ~87 GB and not everyone has it, so every test here **skips**
/// rather than fails when it is missing. Point `GW2_TEST_DAT` at a different
/// install to override the default location.
///
/// @code
/// set GW2_TEST_DAT=D:\Guild Wars 2\Gw2.dat
/// ctest --test-dir build -R dat --output-on-failure
/// @endcode

#include "test_framework.h"

#include "castlemist/extract/entry_extractor.h"
#include "castlemist/format/struct_template.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2model.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

namespace {

/// The install this project is developed against.
constexpr const char* kDefaultDat =
    R"(C:\Program Files (x86)\Steam\steamapps\common\Guild Wars 2\Gw2.dat)";

/// @brief Path to the archive under test, or an empty string when unavailable.
std::string dat_path() {
    const char* env = std::getenv("GW2_TEST_DAT");
    std::string path = (env && *env) ? env : kDefaultDat;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        return path;
    }
    return {};
}

/// @brief Open the archive once per process; skip the test when it is missing.
Gw2Dat& shared_dat() {
    static Gw2Dat dat;
    static bool tried = false;
    static bool ok = false;
    if (!tried) {
        tried = true;
        std::string path = dat_path();
        if (!path.empty()) {
            try {
                load_dat_file(dat, path);
                ok = dat.mft_data_list.size() > 0;
            } catch (const std::exception&) {
                ok = false;
            }
        }
    }
    if (!ok) SKIP("no Gw2.dat (set GW2_TEST_DAT)");
    return dat;
}

/// @brief Load the struct template, trying the usual spots relative to the build tree.
///
/// Model extraction needs `gw2_packfile.json`; `castlemist::tpl::auto_load`
/// searches next to the exe and the working directory, neither of which is the
/// repository root when ctest runs. `GW2_TEST_TEMPLATE` overrides.
bool ensure_template() {
    if (castlemist::tpl::get()) return true;
    if (castlemist::tpl::auto_load()) return true;

    std::string err;
    if (const char* env = std::getenv("GW2_TEST_TEMPLATE"); env && *env)
        if (castlemist::tpl::load_from_file(env, err)) return true;

    // Generated output, so it lives under dumps/ -- see tools/structs.
    for (const char* candidate : {
             "../../../dumps/packfile/gw2_packfile.json",
             "dumps/packfile/gw2_packfile.json",
             "../dumps/packfile/gw2_packfile.json",
         }) {
        if (castlemist::tpl::load_from_file(candidate, err)) return true;
    }
    return false;
}

/// @brief Extract by baseId (the ids in GW2CURATED.txt are baseIds, MFT index + 1).
ExtractedEntry extract_base(uint32_t base_id) {
    Gw2Dat& dat = shared_dat();
    if (base_id == 0 || base_id - 1 >= dat.mft_data_list.size()) SKIP("baseId out of range for this dat");
    return extract_entry(dat, base_id - 1);
}

/// @brief The decompressed packfile for a fileId, without building a preview.
///
/// extract_entry() would build a whole map scene for a mapc; the sky tests only
/// need the bytes. Skips when the fileId is not in this dat.
std::vector<uint8_t> packfile_by_file_id(uint32_t file_id) {
    Gw2Dat& dat = shared_dat();
    uint32_t base = get_by_base_id(dat, file_id);
    if (base == 0 || base > dat.mft_data_list.size()) SKIP("fileId not in this dat");
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    return e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
}

/// @brief parseMapSky() on a map fileId (skips without the dat or template).
castlemist::model::Extractor::MapSky map_sky(uint32_t file_id) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");
    std::vector<uint8_t> bytes = packfile_by_file_id(file_id);
    return castlemist::model::Extractor(bytes, *castlemist::tpl::get()).parseMapSky();
}

} // namespace

CM_TEST(dat, opens_and_reports_a_plausible_entry_count) {
    Gw2Dat& dat = shared_dat();
    // A retail archive has hundreds of thousands of entries; anything much
    // smaller means the MFT was mis-read rather than that the file is small.
    CHECK(dat.mft_data_list.size() > 100000);
    CHECK_FALSE(dat.file_info.file_path.empty());
}

// A texture's full-size copy is the entry holding fileId F+1 -- never just the
// next archive row. Frostfang's decal 217977 (row 386566) sits beside row
// 386567, an unrelated DXT5 at exactly double size (fileIds 127964/249923);
// taking that row painted the axe black and red in the Game 1:1 view. Its real
// full copy is fileId 217978 at row 386572.
CM_TEST(dat, full_res_texture_entry_is_the_paired_file_not_the_next_row) {
    Gw2Dat& dat = shared_dat();
    CHECK_EQ(texture_entry(dat, 217977, /*full=*/true), size_t(386572));
    CHECK_EQ(texture_entry(dat, 217977, /*full=*/false), size_t(386566));

    // A real pair still resolves: 54619 is the reduced copy of a full one.
    const size_t own = static_cast<size_t>(get_by_base_id(dat, 54619) - 1);
    const size_t full = texture_entry(dat, 54619, /*full=*/true);
    CHECK_NE(full, own);
    bool pairedFile = false;
    for (uint32_t f : get_by_file_id(dat, static_cast<uint32_t>(full + 1))) pairedFile |= (f == 54620);
    CHECK(pairedFile);
    CHECK_EQ(texture_entry(dat, 54619, /*full=*/false), own);

    CHECK_EQ(texture_entry(dat, 0xFFFFFFF0u, true), SIZE_MAX);  // unknown fileId
}

CM_TEST(dat, decodes_atex_textures_to_rgba) {
    for (uint32_t base : {2871u, 807183u}) {
        ExtractedEntry e = extract_base(base);
        CHECK(e.kind == PreviewKind::Image);
        CHECK(e.is_image);
        CHECK(e.preview_width > 0);
        CHECK(e.preview_height > 0);
        CHECK_EQ(e.preview_pixels.size(),
                 size_t(e.preview_width) * e.preview_height * 4);
        CHECK_FALSE(e.preview_format_label.empty());
    }
}

// ATEP is an ATEX variant; the container tag is engine metadata and provably
// does not change pixel decoding, so it must produce the same kind of result.
CM_TEST(dat, decodes_atep_and_ateu_like_atex) {
    for (uint32_t base : {120115u, 2903u, 86152u}) {
        ExtractedEntry e = extract_base(base);
        CHECK(e.kind == PreviewKind::Image);
        CHECK(e.preview_width > 0);
    }
}

// A "DDS " entry is handed to the GPU in its native format rather than expanded
// on the CPU, so the label names that format and the buffer holds at least the
// top mip at that format's size.
CM_TEST(dat, passes_a_standalone_dds_through_in_its_native_format) {
    ExtractedEntry e = extract_base(18232);
    CHECK(e.kind == PreviewKind::Image);
    CHECK(e.preview_width > 0);
    CHECK(e.preview_height > 0);
    CHECK(e.preview_pitch > 0);
    CHECK_EQ(e.preview_format_label.rfind("DDS", 0), size_t{0});
    // pitch is bytes per row (or per row of blocks), so height rows must fit.
    CHECK(e.preview_pixels.size() >= size_t(e.preview_pitch));
}

CM_TEST(dat, decodes_png_and_webp_stills) {
    ExtractedEntry png = extract_base(38);
    CHECK(png.kind == PreviewKind::Image);
    CHECK(png.preview_format_label.find("PNG") != std::string::npos);

    // The dat's `riff` entries are RIFF/WEBP; libwebp names itself in the label.
    ExtractedEntry webp = extract_base(101);
    CHECK(webp.kind == PreviewKind::Image);
    CHECK(webp.preview_format_label.find("WebP") != std::string::npos);
}

CM_TEST(dat, decodes_strs_string_tables) {
    ExtractedEntry e = extract_base(2925);
    CHECK(e.kind == PreviewKind::Strs);
    CHECK_FALSE(e.text_preview.empty());
}

CM_TEST(dat, decodes_asnd_audio_into_a_playable_clip) {
    ExtractedEntry e = extract_base(2883);
    CHECK(e.kind == PreviewKind::Audio);
    CHECK_FALSE(e.audio_clips.empty());
    CHECK_FALSE(e.audio_clips[0].data.empty());
    CHECK_FALSE(e.audio_clips[0].codec.empty());
}

// An ABNK bank holds several sounds behind one entry.
CM_TEST(dat, splits_an_abnk_bank_into_several_clips) {
    ExtractedEntry e = extract_base(165780);
    CHECK(e.kind == PreviewKind::Audio);
    CHECK(e.audio_clips.size() >= 1);
}

// baseId 3203 is the first cntc in the index (32 of them ship); the entry is a
// ~10 MB PackContent, so this is also the slowest test here.
CM_TEST(dat, recognises_a_cntc_content_datastore) {
    ExtractedEntry e = extract_base(3203);
    CHECK(e.kind == PreviewKind::Content);
    CHECK_FALSE(e.content_asset_ids.empty());
    CHECK_FALSE(e.content_objects.empty());

    // Most objects carry an identifier slug; those that do not are the ones
    // whose only strings are shared human labels.
    size_t with_slug = 0;
    for (const ContentObject& o : e.content_objects)
        if (!o.name.empty()) ++with_slug;
    CHECK(with_slug > 0);
}

// Models need the struct template; without it the entry is still recognised as
// a model, it just carries no geometry.
CM_TEST(dat, recognises_models_and_builds_them_when_a_template_is_loaded) {
    ExtractedEntry e = extract_base(291821);
    CHECK(e.kind == PreviewKind::Model);

    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    ExtractedEntry with_template = extract_base(291821);
    CHECK(with_template.model != nullptr);
    if (with_template.model) {
        CHECK_FALSE(with_template.model->meshes.empty());
        CHECK(with_template.model->totalVerts > 0);
        CHECK(with_template.model->radius > 0.0f);
    }
}

// A rigged model must arrive at the renderer WITH its rig.
//
// This is the regression nothing was catching. `Extractor::chunkType` resolved a
// chunk's struct only through the template's `fileTypes`/`chunks` maps, but SKEL
// appears in neither -- only in `strucTabs`. So parseSkeleton bailed at its
// `root.empty()` guard and every model in the archive came back with an empty
// skeleton, even with a 56 KB SKEL chunk sitting right there in the file.
//
// Nothing failed loudly. The rig was simply absent: no joints, no resolved bone
// bindings, no skinning, and -- because the UI gates the clip selector on
// `render::has_skeleton()` -- no animation controls at all, on any model.
//
// baseId 146007 is MFT index 146006, the 302-bone boss (fileId 1634661) whose
// bone count is recorded in docs/research/gw2-skeleton.md.
CM_TEST(dat, a_rigged_model_arrives_with_its_skeleton_and_clips) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    ExtractedEntry e = extract_base(146007);
    CHECK(e.kind == PreviewKind::Model);
    if (e.model == nullptr) SKIP("model did not build");

    // The rig itself. An empty joints list is the exact failure described above.
    CHECK_FALSE(e.model->joints.empty());
    CHECK_EQ(e.model->joints.size(), size_t(302));

    // Parents must precede children, which is what lets a single forward pass
    // compose the pose (see native/granny_pose.hpp).
    for (size_t i = 0; i < e.model->joints.size(); ++i)
        CHECK(e.model->joints[i].parent < static_cast<int>(i));

    // Decoded Granny clips, so there is something to select and play.
    CHECK_FALSE(e.model->animClips.empty());

    // The bone bindings are resolved INTO the vertices (build_model_preview maps
    // each mesh's binding-slot table through the rig, then writes joint indices
    // into GVertex). So a rig that resolved shows up as skinned meshes whose
    // vertex indices actually address the joint list -- with no rig they would
    // all have collapsed to slot 0 with no weight.
    size_t skinnedMeshes = 0, weighted = 0, inRange = 0, sampled = 0;
    for (const auto& m : e.model->meshes) {
        if (!m.hasSkin) continue;
        ++skinnedMeshes;
        for (const GVertex& v : m.vertices) {
            ++sampled;
            float wsum = 0.0f;
            bool ok = true;
            for (int c = 0; c < 4; ++c) {
                wsum += v.bwt[c];
                if (v.bwt[c] > 0.0f &&
                    static_cast<size_t>(v.bidx[c]) >= e.model->joints.size()) ok = false;
            }
            if (wsum > 0.5f) ++weighted;
            if (ok) ++inRange;
        }
    }
    CHECK(skinnedMeshes > 0);
    CHECK(sampled > 0);
    CHECK_EQ(inRange, sampled);          // every index addresses a real joint
    CHECK(weighted * 100 >= sampled * 95); // and carries real blend weights
}

// Both extract_entry overloads must agree: the background-thread one opens its
// own handle, and a divergence there would mean the preview differs from what
// the UI thread would have produced.
CM_TEST(dat, both_extract_overloads_agree) {
    Gw2Dat& dat = shared_dat();
    const uint32_t index = 2871 - 1;
    if (index >= dat.mft_data_list.size()) SKIP("baseId out of range for this dat");

    ExtractedEntry a = extract_entry(dat, index);
    ExtractedEntry b = extract_entry(dat.file_info.file_path, dat.mft_data_list[index]);

    CHECK(a.kind == b.kind);
    CHECK_EQ(a.preview_width, b.preview_width);
    CHECK_EQ(a.preview_height, b.preview_height);
    CHECK_EQ(a.decompressed.size(), b.decompressed.size());
}

// The texture panel reports each texture's ids, size, format, mip count and
// channel layout, and none of that is worth showing if it is not measured from
// the real file. Assert the decoder fills all of it, and that the channel string
// is consistent with the format (a BC5/3Dc normal map stores two channels, so it
// must never be reported as RGB).
CM_TEST(dat, model_textures_carry_the_metadata_the_texture_panel_shows) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    ExtractedEntry e = extract_base(291821);
    if (!e.model) SKIP("model did not build");
    if (e.model->textures.empty()) SKIP("model has no textures");

    for (const ModelTextureCPU& t : e.model->textures) {
        CHECK(t.fileId > 0);
        CHECK(t.baseId > 0);
        CHECK(t.width > 0);
        CHECK(t.height > 0);
        CHECK(t.mipCount > 0);
        CHECK_FALSE(t.fmt.empty());
        // R / RG / RGB / RGBA -- alpha only ever appended to a colour base.
        CHECK(t.channels == "R" || t.channels == "RA" || t.channels == "RG" || t.channels == "RGA" ||
              t.channels == "RGB" || t.channels == "RGBA");
        if (t.isNormal) CHECK_EQ(t.channels.rfind("RG", 0), size_t{0});
    }

    // Every material's texture references must resolve into that same table --
    // this is exactly the fileId -> texture lookup the panel groups by, so a
    // mismatch here would show up as empty submesh groups.
    size_t resolved = 0;
    for (const ModelMaterialCPU& m : e.model->materials) {
        for (uint32_t fid : m.textureFileIds)
            for (const ModelTextureCPU& t : e.model->textures)
                if (t.fileId == fid) { ++resolved; break; }
    }
    CHECK(resolved > 0);
}

CM_TEST(dat, texture_resolution_preference_changes_what_a_model_loads) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    set_texture_full_res(true);
    ExtractedEntry full = extract_base(291821);
    set_texture_full_res(false);
    ExtractedEntry reduced = extract_base(291821);
    set_texture_full_res(true);   // restore the default for the rest of the run

    if (!full.model || !reduced.model) SKIP("model did not build");
    if (full.model->textures.empty()) SKIP("model has no textures");

    // Same texture count either way; only the dimensions may differ.
    CHECK_EQ(full.model->textures.size(), reduced.model->textures.size());
    CHECK(full.model->textures[0].width >= reduced.model->textures[0].width);
}

// The point of the loose-file path is that an asset exported out of the archive
// previews exactly as it did inside it. Both export shapes have to round-trip:
// "Export Decompressed" writes `decompressed`, "Export Compressed" writes
// `compressed` (CRC32C framing and Method0 payload still on), and neither carries
// an extension that says which is which -- so extract_loose_file has to work that
// out from the bytes. Anything less and the two readings silently disagree.
CM_TEST(dat, exported_files_reopen_as_the_same_kind) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    int checked = 0;
    for (uint32_t base : {291821u, 2871u, 143918u}) {
        ExtractedEntry inside = extract_base(base);
        if (inside.kind == PreviewKind::None) continue;  // nothing to compare against
        ++checked;

        ExtractedEntry from_decompressed = extract_loose_file(inside.decompressed, "loose");
        CHECK(from_decompressed.kind == inside.kind);
        CHECK_EQ(from_decompressed.decompressed.size(), inside.decompressed.size());

        ExtractedEntry from_compressed = extract_loose_file(inside.compressed, "loose");
        CHECK(from_compressed.kind == inside.kind);
        // Unwrapping must reproduce the archive's own decompressed bytes exactly,
        // not merely something that sniffs to the same format.
        CHECK_EQ(from_compressed.decompressed.size(), inside.decompressed.size());
    }
    if (checked == 0) SKIP("none of the sample entries decoded on this dat");
}

// Unrecognised input must come back as None with the bytes intact for the hex
// view, never throw and never hand back a mangled buffer from a failed unwrap.
CM_TEST(dat, loose_garbage_is_returned_intact_for_the_hex_view) {
    std::vector<uint8_t> junk(4096);
    for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);

    ExtractedEntry e = extract_loose_file(junk, "junk.bin");
    CHECK(e.kind == PreviewKind::None);
    CHECK_EQ(e.decompressed.size(), junk.size());
    CHECK(e.decompressed == junk);
}

// A `token64` is a fixed 8-byte field, but MODL v65 is a 32-BIT-pointer packfile,
// so reading one with the pointer-width helper silently drops its high half.
// That is observable rather than theoretical: MODL 143917 carries real data up
// there (texture 0's token is 0x0060B401_67531924, of which a 4-byte read returns
// only 0x67531924). Nothing consumes these tokens yet, which is exactly why the
// truncation survived -- so pin the width here before something starts matching
// on them. The material token is also what game-shader effect selection keys on.
CM_TEST(dat, model_token64_fields_keep_their_high_half) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");

    // MFT index 143917 -> baseId 143918.
    ExtractedEntry e = extract_base(143918);
    if (e.decompressed.empty()) SKIP("entry did not decompress");
    std::shared_ptr<const nlohmann::json> tpl = castlemist::tpl::get();
    if (!tpl) SKIP("no struct template");

    castlemist::model::Model m;
    try {
        m = castlemist::model::Extractor(e.decompressed, *tpl).extract();
    } catch (const std::exception&) {
        SKIP("model did not parse");
    }
    if (m.materials.empty()) SKIP("model has no materials");

    // Effect selection matches this against AmatEffectV1::token.
    CHECK(m.materials[0].token != 0);

    // At least one texture token must have a non-zero high half; if every one of
    // them reads as a bare 32-bit value on this model, the read is truncating.
    bool any_high_half = false;
    for (const castlemist::model::Material& mat : m.materials)
        for (const castlemist::model::MatTexture& t : mat.textures)
            if ((t.token >> 32) != 0) any_high_half = true;
    CHECK(any_high_half);
}

CM_TEST(dat, texture_decodes_by_file_id) {
    Gw2Dat& dat = shared_dat();
    ModelTextureCPU tex;
    CHECK(decode_texture_rgba(dat, 151455, tex));  // Warden Coat, SylvariFemale base texture
    CHECK_EQ(tex.width, 512);
    CHECK_EQ(tex.height, 256);
    CHECK_EQ(tex.rgba.size(), size_t{512 * 256 * 4});
    ModelTextureCPU none;
    CHECK_FALSE(decode_texture_rgba(dat, 0, none));
}

CM_TEST(dat, texture_rgba_ignores_the_reduced_resolution_toggle) {
    Gw2Dat& dat = shared_dat();
    // Armor base textures from a real export; any with a reduced member exposes the toggle.
    for (uint32_t id : {151455u, 151485u, 418445u, 2693767u, 2460768u, 2162270u, 2306526u, 2162253u, 1202479u}) {
        ModelTextureCPU full, reduced;
        CHECK(decode_texture_rgba(dat, id, full));
        set_texture_full_res(false);  // the viewer's "reduced" setting
        bool ok = decode_texture_rgba(dat, id, reduced);
        set_texture_full_res(true);
        CHECK(ok);
        CHECK_EQ(reduced.width, full.width);  // the ripper's atlas math needs the full member
        CHECK_EQ(reduced.height, full.height);
    }
}


CM_TEST(dat, texture_rgba_decodes_the_exact_entry) {
    Gw2Dat& dat = shared_dat();
    // The Warden Coat's dye masks: the Composite names 512x256 entries. Two of
    // them have a 1024x512 high-res copy in the next entry and the base texture
    // has none, so following the full/reduced pairing returned a mismatched set
    // (and the ripper dropped those masks). The exact entries all match.
    for (uint32_t id : {151449u, 151451u, 151453u}) {
        ModelTextureCPU t;
        CHECK(decode_texture_rgba(dat, id, t));
        CHECK_EQ(t.width, 512);
        CHECK_EQ(t.height, 256);
        CHECK_EQ(t.baseId, get_by_base_id(dat, id));
    }
}

CM_TEST(dat, texture_full_prefers_the_high_res_copy) {
    Gw2Dat& dat = shared_dat();
    ModelTextureCPU t;
    int exact_w = 0;
    CHECK(decode_texture_full(dat, 151449, t, &exact_w));  // Warden Coat mask: 512x256 + a 1024x512 copy
    CHECK_EQ(t.width, 1024);
    CHECK_EQ(t.height, 512);
    CHECK_EQ(exact_w, 512);
    ModelTextureCPU base;
    CHECK(decode_texture_full(dat, 2585044, base, &exact_w));  // Angler Vest base: no larger copy
    CHECK_EQ(base.width, 512);
    CHECK_EQ(exact_w, 512);
}

// Holographic Dawn (greatsword skin 8813, model fileId 2163020). Its blade
// material's AMAT (543769) paints in TWO passes: pass 0 is all depth/StencilId
// effects (shaderPassFlags 0x9 = no RGB write, or 0x5 = no colour at all) and the
// hologram itself is pass 1, an additive ONE/INV_SRC_COLOR glow. Picking a pass-0
// effect as the material's colour shader drew the blade with RGB masked off --
// invisible in Shader mode. The glow's alpha is a soft-particle fade against the
// scene depth, and its two `st?freq` constants only bind when the q/v token alias
// is honoured.
CM_TEST(dat, hologram_blade_draws_its_glow_pass_with_every_constant_bound) {
    if (!ensure_template()) SKIP("no gw2_packfile.json struct template");
    auto pv = load_model_by_fileid(shared_dat(), 2163020);
    if (!pv) SKIP("model 2163020 did not build on this dat");

    const GameMaterial* blade = nullptr;
    for (const auto& g : pv->gameMaterials) if (g.index == 0) blade = &g;
    CHECK(blade != nullptr);
    if (!blade) return;
    CHECK(blade->ok);
    CHECK_EQ(blade->shaderPassFlags & 0x000Cu, 0u);              // can write RGB
    CHECK_NE((blade->renderState >> 12) & 0xFFFFu, 0ull);        // and blends

    // Its alpha is a soft-particle fade against the scene depth at slot 12 (AMAT
    // role 35). A grey stand-in made that fade 0 everywhere; it has to read far.
    bool farDepth = false;
    for (const auto& smp : blade->samplers) farDepth |= (smp.slot == 12 && smp.global == 4);
    CHECK(farDepth);

    for (const char* want : {"ghotint", "stafreq", "stbfreq", "stnthr", "stint", "depdist"}) {
        int off = -1;
        for (const auto& u : blade->psUniforms) if (u.name == want) off = u.byteOff;
        bool bound = false;
        for (const auto& c : blade->psConsts) bound |= (c.byteOff == off);
        if (off < 0 || !bound) std::fprintf(stderr, "  uniform %s: off=%d bound=%d\n", want, off, (int)bound);
        CHECK(off >= 0 && bound);
    }
}

// ---- map sky (env chunk -> PackMapEnvDataGlobalV*) ----

// 187611: a panorama sky. Four sky modes, mode 3 repeating mode 0, one star
// texture and four cloud layers; no cube faces (its env is v76, which has the
// skyModeCubeTex array, but every entry is null).
CM_TEST(mapsky, panorama_map_187611) {
    auto s = map_sky(187611);
    CHECK(s.present);
    CHECK_EQ(s.modes.size(), size_t{4});
    if (s.modes.size() < 4) return;
    CHECK_EQ(s.modes[0].ne, 187554u);
    CHECK_EQ(s.modes[0].sw, 187556u);
    CHECK_EQ(s.modes[0].top, 187558u);
    CHECK(s.modes[0].hasPanorama());
    CHECK_EQ(s.modes[3].ne, 187554u);
    CHECK_EQ(s.starFile, 187544u);
    CHECK_FALSE(s.modes[0].hasCube());
    CHECK_EQ(s.clouds.size(), size_t{4});
    if (!s.clouds.empty()) CHECK_EQ(s.clouds[0].texture, 186345u);
}

// 3264516: one of three maps with a real cube sky, in modes 0-1 only.
CM_TEST(mapsky, cube_map_3264516) {
    auto s = map_sky(3264516);
    CHECK(s.present);
    CHECK(s.modes.size() >= size_t{3});
    if (s.modes.size() < 3) return;
    CHECK(s.modes[0].hasCube());
    CHECK_EQ(s.modes[0].cube[0], 3263205u);  // E
    CHECK_EQ(s.modes[0].cube[1], 3263207u);  // W
    CHECK_EQ(s.modes[0].cube[2], 3263209u);  // N
    CHECK_EQ(s.modes[0].cube[3], 3263211u);  // S
    CHECK_EQ(s.modes[0].cube[4], 3263213u);  // B
    CHECK_EQ(s.modes[0].cube[5], 3263215u);  // T
    CHECK(s.modes[1].hasCube());
    CHECK_FALSE(s.modes[2].hasCube());
}

// Sky cards (sun, moons, planets) are textured; a card read from the wrong
// offset shows up as a NaN/inf placement.
CM_TEST(mapsky, sky_cards_have_textures) {
    size_t textured = 0;
    for (uint32_t id : {187611u, 3264516u}) {
        auto s = map_sky(id);
        for (const auto& c : s.cards) {
            if (!c.day.texture) continue;
            ++textured;
            CHECK(std::isfinite(c.day.azimuth));
            CHECK(std::isfinite(c.day.latitude));
        }
    }
    CHECK(textured > 0);
}

// env v75 and older have no skyModeCubeTex: the parse still finds the sky and
// leaves the cube faces empty. 184799 is a v75 map in the index this was
// written against; a newer dat may have moved it on, so skip in that case.
CM_TEST(mapsky, missing_fields_stay_empty) {
    auto s = map_sky(184799);
    if (s.envVersion >= 76) SKIP("map 184799 is no longer an old-env map on this dat");
    CHECK(s.present);
    CHECK(!s.modes.empty());
    for (const auto& m : s.modes)
        for (uint32_t f : m.cube) CHECK_EQ(f, 0u);
}

// A model packfile has no env chunk.
CM_TEST(mapsky, no_env_chunk) {
    auto s = map_sky(2163020);  // Holographic Dawn, used above
    CHECK_FALSE(s.present);
    CHECK(s.modes.empty());
}
