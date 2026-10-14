/// @file
/// @brief fileId/baseId lookups over the sorted MFT lists: correctness and cost.
///
/// Model and texture streaming do thousands of these against ~800k entries, so
/// they must be logarithmic. The tests build the lists by hand (no Gw2.dat).

#include "test_framework.h"

#include "castlemist/native/gw2dat.h"

#include <chrono>
#include <cstdint>
#include <vector>

CM_TEST(dat_lookup, file_to_base_hit_and_miss) {
    Gw2Dat d;
    d.mft_file_id_data_list = {{10, 3}, {20, 5}, {20, 6}, {30, 7}};
    CHECK(get_by_base_id(d, 20) == 5u); // first equal entry, as the linear scan returned
    CHECK(get_by_base_id(d, 10) == 3u);
    CHECK(get_by_base_id(d, 30) == 7u);
    CHECK(get_by_base_id(d, 25) == 0u);
    CHECK(get_by_base_id(d, 5) == 0u);
    CHECK(get_by_base_id(d, 31) == 0u);
}

CM_TEST(dat_lookup, base_to_files_sorted) {
    Gw2Dat d;
    d.mft_base_id_data_list = {MftBaseIdData{{9, 4}, 3}, MftBaseIdData{{1}, 5}};
    CHECK((get_by_file_id(d, 3) == std::vector<uint32_t>{4, 9}));
    CHECK((get_by_file_id(d, 5) == std::vector<uint32_t>{1}));
    CHECK(get_by_file_id(d, 4).empty());
}

CM_TEST(dat_lookup, lookups_are_logarithmic) {
    constexpr uint32_t kN = 800000;
    Gw2Dat d;
    d.mft_file_id_data_list.reserve(kN);
    d.mft_base_id_data_list.reserve(kN);
    for (uint32_t k = 0; k < kN; ++k) {
        d.mft_file_id_data_list.push_back(MftFileIdData{2 * k + 1, k + 1});
        d.mft_base_id_data_list.push_back(MftBaseIdData{{2 * k + 1}, k + 1});
    }
    constexpr uint32_t kLookups = 20000;
    bool all_correct = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < kLookups; ++i) {
        const uint32_t k = static_cast<uint32_t>((uint64_t(i) * (kN - 1)) / (kLookups - 1));
        if (get_by_base_id(d, 2 * k + 1) != k + 1) all_correct = false;
        if (get_by_base_id(d, 2 * k + 2) != 0) all_correct = false; // even ids are absent
        const auto files = get_by_file_id(d, k + 1);
        if (files.size() != 1 || files[0] != 2 * k + 1) all_correct = false;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    CHECK(all_correct);
    CHECK(ms < 200);
}
