/// @file
/// @brief An in-memory HttpClient for the character tests: canned responses by
///        URL, and a record of every request made (URL + headers).

#pragma once

#include "castlemist/character/http.h"

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct FakeHttpClient : castlemist::character::HttpClient {
    std::map<std::string, castlemist::character::HttpResponse> routes;
    std::vector<std::pair<std::string, castlemist::character::Headers>> calls;

    castlemist::character::HttpResponse get(const std::string& url,
                                            const castlemist::character::Headers& headers) override {
        calls.emplace_back(url, headers);
        auto it = routes.find(url);
        if (it == routes.end()) return {404, "{\"text\":\"no such route\"}", ""};
        return it->second;
    }
};

/// The text of tests/data/character/<name>; empty if missing.
inline std::string read_fixture(const char* name) {
    std::ifstream f(std::string(CM_TEST_DATA_DIR "/character/") + name, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
