/// @file
/// @brief Tests for the info panel's "Game content" section (detail/content_links.h):
///        what a file is used by, with names and chat links, from gathered data.

#include "test_framework.h"

#include "detail/content_links.h"

#include <map>
#include <string>

using namespace castlemist::ui;
namespace cmap = castlemist::cmap;

namespace {

bool has(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

} // namespace

CM_TEST(content_links, lists_each_user_with_its_name_and_chat_link) {
    std::map<uint64_t, std::string> names{{content_name_key(cmap::CONTENT_TYPE_SKIN, 6506), "Astralaria"},
                                          {content_name_key(cmap::CONTENT_TYPE_ITEM, 76158), "Astralaria"}};
    NameLookup lookup = [&](uint32_t type, uint32_t id) -> const std::string* {
        auto it = names.find(content_name_key(type, id));
        return it == names.end() ? nullptr : &it->second;
    };
    std::vector<LinkedObject> users{{{cmap::CONTENT_TYPE_SKIN, 6506}, {{cmap::CONTENT_TYPE_ITEM, 76158}}}};
    std::wstring t = format_content_links(users, users.size(), lookup);
    CHECK(has(t, L"Skin 6506"));
    CHECK(has(t, L"Astralaria"));
    CHECK(has(t, L"[&CmoZAAA=]"));
    CHECK(has(t, L"Item 76158"));
    CHECK(has(t, L"[&AgF+KQEA]"));
}

CM_TEST(content_links, a_name_not_yet_fetched_says_so_and_an_unknown_one_is_blank) {
    std::string none;
    NameLookup lookup = [&](uint32_t, uint32_t id) -> const std::string* { return id == 1 ? &none : nullptr; };
    std::vector<LinkedObject> users{{{cmap::CONTENT_TYPE_ITEM, 1}, {}}, {{cmap::CONTENT_TYPE_ITEM, 2}, {}}};
    std::wstring t = format_content_links(users, users.size(), lookup);
    CHECK(has(t, L"Item 2  (looking up name"));
    CHECK_FALSE(has(t, L"Item 1  (looking up name"));
}

CM_TEST(content_links, types_without_an_api_id_show_their_number_and_no_link) {
    NameLookup lookup = [](uint32_t, uint32_t) -> const std::string* { return nullptr; };
    std::vector<LinkedObject> users{{{147, 70}, {}}};
    std::wstring t = format_content_links(users, users.size(), lookup);
    CHECK(has(t, L"Type 147 #70"));
    CHECK_FALSE(has(t, L"[&"));
    CHECK_FALSE(has(t, L"looking up"));
}

CM_TEST(content_links, says_how_many_more_there_are_past_the_shown_ones) {
    NameLookup lookup = [](uint32_t, uint32_t) -> const std::string* { return nullptr; };
    std::vector<LinkedObject> users{{{147, 1}, {}}};
    std::wstring t = format_content_links(users, 500, lookup);
    CHECK(has(t, L"500"));
    CHECK(has(t, L"499 more"));
}

CM_TEST(content_links, nothing_uses_it) {
    NameLookup lookup = [](uint32_t, uint32_t) -> const std::string* { return nullptr; };
    std::wstring t = format_content_links({}, 0, lookup);
    CHECK(has(t, L"No content object"));
}

CM_TEST(content_links, text_with_any_non_digit_is_a_name_query) {
    CHECK(is_name_query(L"astralaria"));
    CHECK(is_name_query(L"10 Slot"));
    CHECK_FALSE(is_name_query(L"477426"));
    CHECK_FALSE(is_name_query(L"  4774 "));
    CHECK_FALSE(is_name_query(L""));
    CHECK_FALSE(is_name_query(L"   "));
}

CM_TEST(content_links, name_match_ignores_case_and_finds_substrings) {
    const std::wstring q = name_query_key(L"  ASTRAL ");
    CHECK(name_matches("Astralaria", q));
    CHECK(name_matches("The astral one", q));
    CHECK_FALSE(name_matches("Bolt", q));
    CHECK_FALSE(name_matches("", q));
    CHECK(name_matches("Piñata Smashing", name_query_key(L"PIÑATA")));
}
