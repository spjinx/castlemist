/// @file
/// @brief Tests for the Character Ripper's state transitions (detail/character_state.h):
///        what the dialog keeps after a characters-list or character fetch succeeds or fails.

#include "test_framework.h"

#include "detail/character_state.h"

using namespace castlemist::ui;
namespace ch = castlemist::character;

namespace {

ch::FetchResult result_for(const char* name) {
    ch::FetchResult r;
    r.manifest.name = name;
    return r;
}

} // namespace

CM_TEST(character_state, names_success_adopts_key_and_clears_character) {
    RipperState s;
    s.current = result_for("Old");
    s.names_done("KEY-A", {"One", "Two"}, "");
    CHECK_EQ(s.current_key, std::string("KEY-A"));
    CHECK_EQ(s.names.size(), size_t{2});
    CHECK(!s.current.has_value());
}

CM_TEST(character_state, names_failure_forgets_previous_account) {
    RipperState s;
    s.names_done("KEY-A", {"One"}, "");
    s.current = result_for("One");
    s.names_done("KEY-B", {}, "API key rejected (HTTP 401)");
    CHECK(s.current_key.empty());   // no fetch may pair KEY-A's names with KEY-B
    CHECK(s.names.empty());
    CHECK(!s.current.has_value());
}

CM_TEST(character_state, fetch_failure_drops_previous_character) {
    RipperState s;
    s.names_done("KEY-A", {"A", "B"}, "");
    s.fetch_done(result_for("A"), "");
    CHECK(s.current.has_value());
    s.fetch_done(std::nullopt, "GW2 API error (HTTP 404)");
    CHECK(!s.current.has_value());  // Save must not write A while B is selected
}

CM_TEST(character_state, fetch_success_replaces_character) {
    RipperState s;
    s.fetch_done(result_for("A"), "");
    s.fetch_done(result_for("B"), "");
    CHECK_EQ(s.current->manifest.name, std::string("B"));
}
