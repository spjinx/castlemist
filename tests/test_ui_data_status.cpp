/// @file
/// @brief The status-bar badge's wording for each mix of Data status rows.

#include "test_framework.h"

#include "detail/app_state.h"

#include <string>
#include <vector>

using namespace castlemist::ui;
using castlemist::db::Freshness;

namespace {

DataRow r(Freshness f) {
    DataRow d;
    d.state = f;
    return d;
}

} // namespace

CM_TEST(data_badge, says_checking_before_the_first_result) {
    CHECK(data_badge({}).text == L"Data: checking...");
}

CM_TEST(data_badge, all_current_is_up_to_date) {
    DataBadge b = data_badge({r(Freshness::Ok), r(Freshness::Optional)});
    CHECK(b.text == L"Data: up to date");
    CHECK_EQ(b.stale + b.missing, 0);
}

CM_TEST(data_badge, unknowns_do_not_raise_the_alarm) {
    DataBadge b = data_badge({r(Freshness::Ok), r(Freshness::Unknown), r(Freshness::Unknown)});
    CHECK(b.text == L"Data: up to date (2 unchecked)");
}

CM_TEST(data_badge, counts_stale_files) {
    DataBadge b = data_badge({r(Freshness::Stale), r(Freshness::Ok), r(Freshness::Stale)});
    CHECK(b.text == L"⚠ Data: 2 stale");
}

CM_TEST(data_badge, missing_files_need_attention) {
    CHECK(data_badge({r(Freshness::Missing)}).text == L"⚠ Data: 1 needs attention");
    CHECK(data_badge({r(Freshness::Missing), r(Freshness::Stale)}).text == L"⚠ Data: 2 need attention");
}
