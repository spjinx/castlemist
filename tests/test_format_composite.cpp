/// @file
/// @brief Tests for the character Composite (cmpc) decoder (composite.h).
///
/// CompositeBuilder hand-assembles the smallest file the decoder accepts: the
/// PF + "comp" chunk headers, the PackCompositeV20 root at file offset 28, and
/// whatever races / fileData / blit rects a test needs, each reached through
/// the 64-bit self-relative pointers the real file uses. The live test reads
/// the real Composite (fileId 154681) when GW2_TEST_DAT points at a Gw2.dat.

#include "test_framework.h"

#include "castlemist/format/composite.h"
#include "castlemist/native/cmp_decompress_method0.hpp"
#include "castlemist/native/gw2dat.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace castlemist::composite;

namespace {

struct CompositeBuilder {
    std::vector<uint8_t> b = std::vector<uint8_t>(8192, 0);
    size_t heap = 2048;  // where strings / arrays / fileref records are appended
    size_t fd = 0;       // the last FileData one_race() wrote

    CompositeBuilder(const char* container = "cmpc") {
        std::memcpy(b.data(), "PF", 2);
        u16(2, 5);
        u16(6, 12);
        std::memcpy(b.data() + 8, container, 4);
        std::memcpy(b.data() + 12, "comp", 4);
        u32(16, 8000);
        u16(20, 19);
        u16(22, 16);
    }
    void u16(size_t at, uint32_t v) { b[at] = v & 0xFF; b[at + 1] = (v >> 8) & 0xFF; }
    void u32(size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) b[at + i] = (v >> (8 * i)) & 0xFF; }
    void u64(size_t at, uint64_t v) { for (int i = 0; i < 8; ++i) b[at + i] = (v >> (8 * i)) & 0xFF; }
    void ptr(size_t at, size_t target) { u64(at, static_cast<uint64_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at))); }
    size_t alloc(size_t n) { size_t at = heap; heap += n; return at; }
    void arr(size_t at, uint32_t count, size_t target) { u32(at, count); ptr(at + 4, target); }
    void wstr(size_t at, const std::string& s) {
        size_t t = alloc(2 * (s.size() + 1));
        for (size_t i = 0; i < s.size(); ++i) u16(t + 2 * i, static_cast<uint8_t>(s[i]));
        ptr(at, t);
    }
    void fileref(size_t at, uint32_t fid) {
        if (!fid) return;
        size_t t = alloc(4);
        u16(t, (fid - 1) % 0xFF00 + 0x100);
        u16(t + 2, (fid - 1) / 0xFF00 + 0x100);
        ptr(at, t);
    }
};

// The Warden Coat entry SylvariFemale uses in the live file.
constexpr uint64_t kWardenToken = 0x00000348C28A32A3ull;

CompositeBuilder one_race() {
    CompositeBuilder c;
    size_t race = c.alloc(224);
    c.arr(28 + 36, 1, race);
    c.wstr(race + 0, "SylvariFemale");
    c.fileref(race + 140, 31210);  // skeletonFile
    size_t fd = c.fd = c.alloc(103);
    c.arr(race + 104, 1, fd);
    c.u64(fd + 0, kWardenToken);
    c.b[fd + 8] = 9;  // type
    c.fileref(fd + 18, 40405);
    c.fileref(fd + 34, 151449);
    c.fileref(fd + 42, 151451);
    c.fileref(fd + 50, 151453);
    c.fileref(fd + 74, 151455);
    c.fileref(fd + 82, 151457);
    c.u32(fd + 94, 2048);  // hideFlags
    c.b[fd + 102] = 2;     // blitRectIndex
    return c;
}

} // namespace

CM_TEST(composite, parses_race_and_file_data) {
    CompositeBuilder c = one_race();
    auto comp = parse_composite(c.b);
    CHECK(comp.has_value());
    CHECK_EQ(comp->races.size(), size_t{1});
    const CompositeRace* r = comp->race("SylvariFemale");
    CHECK(r != nullptr);
    CHECK(comp->race("Nope") == nullptr);
    CHECK_EQ(r->skeleton_file, 31210u);
    auto it = r->file_data.find(kWardenToken);
    CHECK(it != r->file_data.end());
    const CompositeFileData& f = it->second;
    CHECK_EQ(f.token, kWardenToken);
    CHECK_EQ(int(f.type), 9);
    CHECK_EQ(f.mesh_base, 40405u);
    CHECK_EQ(f.mesh_overlap, 0u);
    CHECK(f.mask_dye == (std::array<uint32_t, 4>{151449, 151451, 151453, 0}));
    CHECK_EQ(f.texture_base, 151455u);
    CHECK_EQ(f.texture_normal, 151457u);
    CHECK_EQ(f.hide_flags, 2048u);
    CHECK_EQ(int(f.blit_set), 2);
}

CM_TEST(composite, parses_blit_rect_sets) {
    CompositeBuilder c = one_race();
    size_t set = c.alloc(40);
    c.arr(28 + 12, 1, set);
    c.wstr(set + 0, "ArmorHeavy");
    c.u32(set + 8, 1024);
    c.u32(set + 12, 1024);
    size_t rects = c.alloc(32);
    uint32_t vals[8] = {0, 512, 384, 1024, 896, 256, 1024, 512};
    for (int i = 0; i < 8; ++i) c.u32(rects + 4 * i, vals[i]);
    c.arr(set + 28, 2, rects);
    auto comp = parse_composite(c.b);
    CHECK(comp.has_value());
    CHECK_EQ(comp->blit_sets.size(), size_t{1});
    const BlitRectSet& s = comp->blit_sets[0];
    CHECK_EQ(s.name, std::string("ArmorHeavy"));
    CHECK_EQ(s.width, 1024u);
    CHECK_EQ(s.rects.size(), size_t{2});
    CHECK_EQ(s.rects[1].x0, 896u);
    CHECK_EQ(s.rects[1].y1, 512u);
}

CM_TEST(composite, fileref_zero_and_unset) {
    CompositeBuilder c = one_race();
    // maskDye4 stays a null pointer; maskCut points at a record whose high half is < 0x100.
    size_t rec = c.alloc(4);
    c.u16(rec, 0x0123);
    c.u16(rec + 2, 0x00FF);
    c.ptr(c.fd + 66, rec);
    auto comp = parse_composite(c.b);
    CHECK(comp.has_value());
    const CompositeFileData& f = comp->race("SylvariFemale")->file_data.at(kWardenToken);
    CHECK_EQ(f.mask_dye[3], 0u);
    CHECK_EQ(f.mask_cut, 0u);
}

CM_TEST(composite, bad_pointer_is_nullopt) {
    CompositeBuilder c = one_race();
    c.arr(28 + 36, 1, 1u << 20);  // raceSexData far past the end
    CHECK(!parse_composite(c.b).has_value());
}

CM_TEST(composite, wrong_container_is_nullopt) {
    CompositeBuilder c("cntc");
    CHECK(!parse_composite(c.b).has_value());
}

CM_TEST(composite, live_sylvari_female_warden_coat) {
    const char* env = std::getenv("GW2_TEST_DAT");
    if (!env || !*env) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    Gw2Dat dat;
    load_dat_file(dat, env);
    uint32_t base = get_by_base_id(dat, 154681);
    if (base == 0) SKIP("Composite fileId 154681 not in this dat");
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    std::vector<uint8_t> bytes = e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw;
    auto comp = parse_composite(bytes);
    CHECK(comp.has_value());
    CHECK_EQ(comp->races.size(), size_t{29});
    const CompositeRace* sf = comp->race("SylvariFemale");
    CHECK(sf != nullptr);
    const CompositeFileData& coat = sf->file_data.at(kWardenToken);
    CHECK_EQ(coat.mesh_base, 40405u);
    CHECK_EQ(coat.texture_base, 151455u);
    CHECK(coat.mask_dye == (std::array<uint32_t, 4>{151449, 151451, 151453, 0}));
    CHECK_EQ(comp->race("HumanMale")->file_data.at(0x0125089844F38644ull).mesh_base, 2693820u);
    CHECK(comp->blit_sets.size() >= 4);
    CHECK_EQ(comp->blit_sets[2].name, std::string("ArmorHeavy"));
}

CM_TEST(composite, parses_skin_styles_faces_and_hair) {
    CompositeBuilder c = one_race();
    size_t race = 2048;  // one_race() allocates the race first
    size_t styles = c.alloc(32);
    for (int k = 0; k < 4; ++k) c.u64(styles + 8 * k, 0x6272573ull + k);
    c.arr(race + 176, 1, styles);  // skinStyles
    size_t faces = c.alloc(16);
    c.u64(faces, 0x136513ull);
    c.u64(faces + 8, 0x262cab0ull);
    c.arr(race + 92, 2, faces);
    size_t hair = c.alloc(8);
    c.u64(hair, 0x285c822ull);
    c.arr(race + 120, 1, hair);
    size_t ears = c.alloc(8);
    c.u64(ears, 0x4b3ab8b6ull);
    c.arr(race + 60, 1, ears);
    auto comp = parse_composite(c.b);
    CHECK(comp.has_value());
    const CompositeRace* r = comp->race("SylvariFemale");
    CHECK_EQ(r->skin_styles.size(), size_t{1});
    CHECK_EQ(r->skin_styles[0][0], 0x6272573ull);
    CHECK_EQ(r->skin_styles[0][3], 0x6272576ull);
    CHECK(r->faces == (std::vector<uint64_t>{0x136513ull, 0x262cab0ull}));
    CHECK(r->hair_styles == std::vector<uint64_t>{0x285c822ull});
    CHECK(r->ears == std::vector<uint64_t>{0x4b3ab8b6ull});
}

CM_TEST(composite, live_sylvari_female_body_and_head) {
    const char* env = std::getenv("GW2_TEST_DAT");
    if (!env || !*env) SKIP("set GW2_TEST_DAT to run against a real Gw2.dat");
    Gw2Dat dat;
    load_dat_file(dat, env);
    uint32_t base = get_by_base_id(dat, 154681);
    const MftData& e = dat.mft_data_list[base - 1];
    std::vector<uint8_t> raw = read_entry_bytes(dat.file_info.file_path, e);
    auto comp = parse_composite(e.compression_flag ? castlemist::cmp::decompress_entry(raw) : raw);
    const CompositeRace* sf = comp->race("SylvariFemale");
    CHECK_EQ(sf->skin_styles.size(), size_t{2});
    CHECK_EQ(sf->skin_styles[0][0], 0x6272573ull);  // chest
    CHECK_EQ(sf->file_data.at(sf->skin_styles[0][0]).mesh_base, 41502u);
    CHECK_EQ(sf->faces.size(), size_t{21});
    CHECK_EQ(sf->hair_styles.size(), size_t{38});
    CHECK_EQ(int(sf->file_data.at(sf->faces[0]).type), 5);
}
