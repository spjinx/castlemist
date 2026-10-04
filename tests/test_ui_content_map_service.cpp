/// @file
/// @brief Tests for the shared content-map service's guards (content_map_service.cpp):
///        when the map may be reported ready, and when a rebuild may discard it.

#include "test_framework.h"

#include "detail/app_state.h"

using namespace castlemist::ui;

CM_TEST(cmap_service, building_wins_over_built) {
    // During a build's finalize the map already reports built(); readers must still wait.
    CHECK(ensure_precheck(true, true) == CmapEnsure::Building);
    CHECK(ensure_precheck(false, true) == CmapEnsure::Ready);
    CHECK(!ensure_precheck(false, false).has_value());
}

CM_TEST(cmap_service, rebuild_refuses_without_dat_or_index) {
    CHECK(rebuild_precheck(false, 0, false, true) == CmapEnsure::NeedDat);
    CHECK(rebuild_precheck(false, 0, true, false) == CmapEnsure::NeedIndex);
}

CM_TEST(cmap_service, rebuild_refuses_while_read_or_building) {
    CHECK(rebuild_precheck(false, 1, true, true) == CmapEnsure::InUse);
    CHECK(rebuild_precheck(true, 0, true, true) == CmapEnsure::Building);
}

CM_TEST(cmap_service, rebuild_allowed_when_idle_with_sources) {
    CHECK(!rebuild_precheck(false, 0, true, true).has_value());
}

CM_TEST(cmap_service, reader_count_tracks_acquire_release) {
    CHECK_EQ(content_map_readers(), 0);
    acquire_content_map_reader();
    CHECK_EQ(content_map_readers(), 1);
    CHECK(rebuild_precheck(false, content_map_readers(), true, true) == CmapEnsure::InUse);
    release_content_map_reader();
    CHECK_EQ(content_map_readers(), 0);
}
