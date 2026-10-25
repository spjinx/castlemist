/// @file
/// @brief Tests for the Texture Gallery's pure parts (src/ui/detail/texture_gallery.h).

#include "test_framework.h"

#include "detail/texture_gallery.h"

#include <cstring>
#include <filesystem>

using namespace castlemist::ui::gallery;

namespace {

Header header(int w, int h, bool alpha = false) {
    Header hd;
    hd.known = true;
    hd.image = true;
    hd.width = static_cast<uint16_t>(w);
    hd.height = static_cast<uint16_t>(h);
    hd.format = alpha ? 26 : 22;
    hd.alpha = alpha;
    return hd;
}

IdQuery query(const char* text) {
    IdQuery q;
    std::string err;
    CHECK(parse_id_query(text, q, err));
    return q;
}

} // namespace

CM_TEST(texture_gallery, kinds_from_magic_and_type) {
    CHECK(kind_of("ATEU", "texture") == Kind::Ui);
    CHECK(kind_of("CTEU", "texture") == Kind::Ui);
    CHECK(kind_of("ATEX", "texture") == Kind::World);
    CHECK(kind_of("ATEP", "texture") == Kind::World);
    CHECK(kind_of("CTEX", "texture") == Kind::World);
    CHECK(kind_of("DDS ", "dds") == Kind::Dds);
    CHECK(kind_of("RIFF", "riff") == Kind::Web);
    CHECK(kind_of(".PNG", "png") == Kind::Web);
}

CM_TEST(texture_gallery, id_query_single_range_and_list) {
    IdQuery q = query("21900-22100");
    CHECK(q.contains(21900));
    CHECK(q.contains(22000));
    CHECK(q.contains(22100));
    CHECK_FALSE(q.contains(21899));
    CHECK_FALSE(q.contains(22101));

    q = query("42001");
    CHECK(q.contains(42001));
    CHECK_FALSE(q.contains(42000));

    // A pasted list: commas, semicolons, newlines, spaced dashes, a reversed range.
    q = query("63375, 63376;\r\n19665\n21924 - 21933  20097-20095");
    for (uint32_t id : {63375u, 63376u, 19665u, 21924u, 21930u, 21933u, 20095u, 20096u, 20097u}) CHECK(q.contains(id));
    for (uint32_t id : {63374u, 63377u, 19666u, 21923u, 21934u, 20094u, 20098u}) CHECK_FALSE(q.contains(id));
}

CM_TEST(texture_gallery, id_query_merges_and_rejects) {
    IdQuery q = query("10-20 15-30 31 40");
    CHECK_EQ(q.ranges.size(), size_t{2});
    CHECK_EQ(q.ranges[0].first, 10u);
    CHECK_EQ(q.ranges[0].second, 31u);
    CHECK_EQ(q.ranges[1].first, 40u);

    CHECK(query("   ").empty());
    CHECK(query("").empty());

    IdQuery keep = query("5");
    std::string err;
    CHECK_FALSE(parse_id_query("12 abc", keep, err));
    CHECK(!err.empty());
    CHECK(keep.contains(5));  // a bad query leaves the old one alone
    CHECK_FALSE(parse_id_query("1-", keep, err));
    CHECK_FALSE(parse_id_query("99999999999", keep, err));
}

CM_TEST(texture_gallery, icon_sizes) {
    CHECK(is_icon_size(16, 16));
    CHECK(is_icon_size(64, 64));
    CHECK(is_icon_size(128, 128));
    CHECK_FALSE(is_icon_size(256, 256));
    CHECK_FALSE(is_icon_size(64, 32));
    CHECK_FALSE(is_icon_size(48, 48));
}

CM_TEST(texture_gallery, filter_kinds_ids_and_headers) {
    Filter f;  // UI only, nothing else
    CHECK(matches(f, Kind::Ui, 1, Header{}));  // no header needed: unknown passes
    CHECK_FALSE(matches(f, Kind::World, 1, Header{}));

    Header not_image;
    not_image.known = true;
    CHECK_FALSE(matches(f, Kind::Ui, 1, not_image));

    f.ids = query("100-200");
    CHECK(matches(f, Kind::Ui, 150, Header{}));
    CHECK_FALSE(matches(f, Kind::Ui, 99, Header{}));
    f.ids = IdQuery{};

    // The survey's UI candidate filter: icons out, longest side <= 1024, alpha formats.
    f.exclude_icons = true;
    f.max_side = 1024;
    f.alpha_only = true;
    CHECK(f.needs_header());
    CHECK_FALSE(matches(f, Kind::Ui, 1, Header{}));               // header not read yet: hidden
    CHECK(matches(f, Kind::Ui, 1, header(256, 64, true)));         // a title bar
    CHECK_FALSE(matches(f, Kind::Ui, 1, header(64, 64, true)));    // an icon
    CHECK_FALSE(matches(f, Kind::Ui, 1, header(2048, 512, true))); // too big
    CHECK_FALSE(matches(f, Kind::Ui, 1, header(256, 64, false)));  // DXT1
    f.min_side = 100;
    CHECK_FALSE(matches(f, Kind::Ui, 1, header(90, 40, true)));
    CHECK(matches(f, Kind::Ui, 1, header(100, 40, true)));
}

CM_TEST(texture_gallery, header_pack_round_trip) {
    CHECK_EQ(Header{}.pack(), uint64_t{0});
    Header h = header(1024, 512, true);
    Header r = Header::unpack(h.pack());
    CHECK(r.known);
    CHECK(r.image);
    CHECK(r.alpha);
    CHECK_EQ(r.width, 1024);
    CHECK_EQ(r.height, 512);
    CHECK_EQ(r.format, 26);
    CHECK_FALSE(Header::unpack(0).known);
}

CM_TEST(texture_gallery, read_header_formats) {
    uint8_t atex[12] = {'C', 'T', 'E', 'U', 'D', 'X', 'T', '5', 0x00, 0x01, 0x40, 0x00};
    Header h = read_header(atex, sizeof atex);
    CHECK(h.image);
    CHECK(h.alpha);
    CHECK_EQ(h.width, 256);
    CHECK_EQ(h.height, 64);
    CHECK_EQ(format_label(h.format), std::string("DXT5"));
    atex[7] = '1';
    CHECK_FALSE(read_header(atex, sizeof atex).alpha);

    uint8_t png[64] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                       0, 0, 1, 0, 0, 0, 0, 32, 8, 6};
    h = read_header(png, sizeof png);
    CHECK(h.image);
    CHECK(h.alpha);
    CHECK_EQ(h.width, 256);
    CHECK_EQ(h.height, 32);
    png[25] = 2;  // RGB, no tRNS
    CHECK_FALSE(read_header(png, sizeof png).alpha);

    uint8_t dds[148] = {};
    std::memcpy(dds, "DDS ", 4);
    dds[12] = 64;   // height
    dds[17] = 2;    // width 512
    dds[80] = 0x4;  // DDPF_FOURCC
    std::memcpy(dds + 84, "DX10", 4);
    dds[128] = 98;  // BC7
    h = read_header(dds, sizeof dds);
    CHECK(h.image);
    CHECK(h.alpha);
    CHECK_EQ(h.width, 512);
    CHECK_EQ(h.height, 64);
    CHECK_EQ(format_label(h.format), std::string("BC7"));

    uint8_t wav[32] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    h = read_header(wav, sizeof wav);
    CHECK(h.known);
    CHECK_FALSE(h.image);
}

CM_TEST(texture_gallery, thumb_fits_box_over_checkerboard) {
    Rgba img;
    img.width = 256;
    img.height = 64;
    img.px.assign(static_cast<size_t>(256) * 64 * 4, 0);  // fully transparent
    Thumb t = make_thumb(img, 128);
    CHECK_EQ(t.width, 128);
    CHECK_EQ(t.height, 32);
    CHECK_EQ(t.bgra.size(), size_t{128} * 32 * 4);
    CHECK_EQ(t.bgra[0], 0x64);       // checker square
    CHECK_EQ(t.bgra[8 * 4], 0x9C);   // the next square along
    CHECK_EQ(t.bgra[3], 255);

    Rgba small;
    small.width = small.height = 16;
    small.px.assign(16 * 16 * 4, 255);
    t = make_thumb(small, 128);  // never upscales
    CHECK_EQ(t.width, 16);
    CHECK_EQ(t.bgra[0], 255);
}

CM_TEST(texture_gallery, cache_evicts_least_recently_used) {
    auto thumb = [](int px) {
        Thumb t;
        t.width = px;
        t.height = 1;
        t.bgra.assign(static_cast<size_t>(px) * 4, 0);
        return t;
    };
    const size_t one = thumb(100).bytes();
    ThumbCache c(one * 3);
    c.put(1, thumb(100));
    c.put(2, thumb(100));
    c.put(3, thumb(100));
    CHECK_EQ(c.size(), size_t{3});
    CHECK(c.get(1) != nullptr);  // 1 is now the most recent; 2 is the oldest
    c.put(4, thumb(100));
    CHECK_EQ(c.size(), size_t{3});
    CHECK_FALSE(c.contains(2));
    CHECK(c.contains(1));
    CHECK(c.contains(3));
    CHECK(c.contains(4));
    CHECK(c.bytes() <= c.cap());

    c.put(3, thumb(100));  // replacing does not double count
    CHECK_EQ(c.bytes(), one * 3);

    c.put(5, thumb(1000));  // bigger than the cap: kept alone
    CHECK_EQ(c.size(), size_t{1});
    CHECK(c.contains(5));
    c.clear();
    CHECK_EQ(c.bytes(), size_t{0});
}

CM_TEST(texture_gallery, header_file_round_trip) {
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "cm_test_texture_headers.bin";
    std::vector<std::pair<uint32_t, uint64_t>> in = {{19234, header(512, 256, true).pack()}, {42001, header(64, 32).pack()}};
    CHECK(save_headers(p, "123:456:789", in));
    std::vector<std::pair<uint32_t, uint64_t>> out;
    CHECK(load_headers(p, "123:456:789", out));
    CHECK(out == in);
    out.clear();
    CHECK_FALSE(load_headers(p, "123:456:999", out));  // another dat build
    CHECK(out.empty());
    std::filesystem::remove(p);
}
