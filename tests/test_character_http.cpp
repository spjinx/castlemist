/// @file
/// @brief Tests for the character layer's HTTP seam (http.h): URL encoding and
///        the fake client the API tests are built on. WinHttpClient itself is
///        only exercised live (no automated test touches the network).

#include "test_framework.h"

#include "fake_http.h"

#include <string>

using namespace castlemist::character;

CM_TEST(http, url_encode_utf8_and_space) {
    CHECK_EQ(url_encode("\xC3\x9E\xC3\xB3rr Sk\xC3\xBD" "fa\xC3\xB0ir"),
             std::string("%C3%9E%C3%B3rr%20Sk%C3%BD" "fa%C3%B0ir"));
    CHECK_EQ(url_encode("A-z_0.~"), std::string("A-z_0.~"));
    CHECK_EQ(url_encode("a/b?c&d"), std::string("a%2Fb%3Fc%26d"));
}

CM_TEST(http, fake_records_headers) {
    FakeHttpClient fake;
    fake.routes["https://x/y"] = {200, "[]", ""};
    HttpResponse r = fake.get("https://x/y", {{"Authorization", "Bearer K"}});
    CHECK_EQ(r.status, 200);
    CHECK_EQ(fake.calls.size(), size_t{1});
    CHECK_EQ(fake.calls[0].first, std::string("https://x/y"));
    CHECK_EQ(fake.calls[0].second[0].second, std::string("Bearer K"));
    CHECK_EQ(fake.get("https://nope", {}).status, 404);
}
