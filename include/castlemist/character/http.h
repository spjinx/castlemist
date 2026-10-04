#ifndef CASTLEMIST_CHARACTER_HTTP_H
#define CASTLEMIST_CHARACTER_HTTP_H

// The character layer's one network seam. Production uses WinHttpClient; tests
// substitute an in-memory client, so nothing automated ever goes online.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace castlemist::character {

using Headers = std::vector<std::pair<std::string, std::string>>;

/// `status == 0` means the request never completed (DNS, TLS, timeout...):
/// `error` then says why. `error` never contains request headers.
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(const std::string& url, const Headers& headers) = 0;
};

/// HTTPS GET over WinHTTP: 15 s resolve/connect/send/receive timeouts,
/// User-Agent "castlemist".
class WinHttpClient final : public HttpClient {
public:
    HttpResponse get(const std::string& url, const Headers& headers) override;
};

/// RFC 3986 percent-encoding of UTF-8 bytes: A-Z a-z 0-9 - . _ ~ pass through,
/// every other byte becomes %XX (uppercase hex).
std::string url_encode(std::string_view utf8);

} // namespace castlemist::character

#endif // CASTLEMIST_CHARACTER_HTTP_H
